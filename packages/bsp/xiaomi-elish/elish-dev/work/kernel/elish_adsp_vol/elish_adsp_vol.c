// SPDX-License-Identifier: GPL-2.0
/*
 * elish_adsp_vol - out-of-tree ADM (q6 DSP) COPP master-gain and ASM
 *                  stream-volume control
 *
 * Target: Xiaomi Pad 5 Pro ("elish", sm8250) running a mainline-based
 * kernel (tested against 6.12.58-current-sm8250 / Armbian).
 *
 * This module lets userspace program the ADSP "Volume Control"
 * post-processing module of an ADM COPP - i.e. the DSP-side playback master
 * gain - *without recompiling the kernel*, by talking APR directly through
 * the already-exported apr_send_pkt().
 *
 * It also programs the **ASM stream volume** (the gain stage mainline never
 * touches).  The ADM/COPP master gain can only attenuate (its ceiling is
 * 0 dB), so the extra loudness has to come from the ASM stream volume, which
 * lives on the ASM service (APR_SVC_ASM == 7) rather than the ADM service
 * (8).  See the "ASM (stream) volume control" constant block below and the
 * NOTES.md section on the ASM volume.
 *
 * It exists as a stop-gap for the proper in-kernel patch
 * (../patches/0001-q6adm-add-copp-master-gain-volume.patch), which adds
 * q6adm_set_volume() + a "MultiMedia1 Playback Volume" ALSA kcontrol.
 * Prefer that patch once the kernel is rebuilt.
 *
 * ---------------------------------------------------------------------------
 * EVIDENCE / PROTOCOL (all from the local downstream Qualcomm techpack
 * checkout in ../../downstream/ unless stated otherwise)
 * ---------------------------------------------------------------------------
 *
 * Opcodes:
 *   ADM_CMD_SET_PP_PARAMS_V5 = 0x00010328   apr_audio-v2.h:614
 *   ADM_CMD_SET_PP_PARAMS_V6 = 0x0001035D   apr_audio-v2.h:615
 *   APR_BASIC_RSP_RESULT     = 0x000110E8   mainline include/linux/soc/qcom/apr.h
 *
 * Command payload (downstream struct adm_cmd_set_pp_params, apr_audio-v2.h:623;
 * the mainline apr_hdr is 20 bytes, see include/linux/soc/qcom/apr.h):
 *   struct apr_hdr hdr;          // 20 bytes
 *   u32 payload_addr_lsw;        // mem_mapping_hdr (in-band => all zero)
 *   u32 payload_addr_msw;
 *   u32 mem_map_handle;
 *   u32 payload_size;            // size of the packed parameter blob
 *   u8  param_data[];            // param header + parameter payload
 *
 * Parameter blob (downstream q6common_pack_pp_params(), dsp/q6common.c:54):
 *   V6 (instance IDs, used on sm8250): param_hdr_v3 (16 bytes) + 4-byte payload
 *       module_id   = AUDPROC_MODULE_ID_VOL_CTRL (0x00010BFE)
 *       instance_id = INSTANCE_ID_0 (0x0000)
 *       param_id    = AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN (0x00010BFF)
 *       param_size  = 4
 *   V5: param_hdr_v1 (12 bytes) + 4-byte payload
 *
 * Payload of AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN (apr_audio-v2.h:891):
 *   struct audproc_volume_ctrl_master_gain {
 *           uint16_t master_gain;   // linear gain in Q13, 0x2000 == 0 dB
 *           uint16_t reserved;      // must be 0
 *   } __packed;
 * The mainline AudioReach driver uses the identical layout
 * (sound/soc/qcom/qdsp6/audioreach.h:637-643, VOL_CTRL_DEFAULT_GAIN 0x2000).
 *
 * Packet fields (downstream adm_set_pp_params(), q6adm.c:963):
 *   src_port  = real AFE port id (q6afe_get_port_id(port index))
 *   dest_port = DSP COPP handle (copp->id, *not* the copp index)
 *   token     = port_index << 16 | copp_idx
 *   mem_hdr   = zeroed (in-band)
 *
 * The DSP COPP handle is only known after the ADM DEVICE_OPEN response; it is
 * stored in the driver-private struct q6copp.id.  q6adm_get_copp_id() returns
 * the *index*, not the handle, and q6adm_find_copp() is static - so this
 * module reads the handle out of the q6adm private data.  Because the module
 * is built on the target against the running kernel's headers
 * (/lib/modules/$(uname -r)/build), kernel-config-dependent types such as
 * spinlock_t / wait_queue_head_t / struct mutex have exactly the same size as
 * in the running kernel, so the verbatim copies below of struct q6copp and
 * struct q6adm reproduce the driver's layout exactly.
 *
 * Userspace interface is Q27 (0x08000000 == 1.0 == 0 dB) and is converted to
 * the firmware's Q13 on the wire (>> 14).  See ../patches/NOTES.md section 3
 * for why the wire format is Q13 and not Q27.
 *
 * NOTE: apr_send_pkt() only guarantees that the message reached the APR
 * transport; APR responses are delivered to the bound q6adm driver, so this
 * module cannot see the DSP result.  Every send is logged to dmesg and the
 * last values are readable from sysfs.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/soc/qcom/apr.h>

/* ------------------------------------------------------------------ */
/* constants (mainline sound/soc/qcom/qdsp6/)                          */
/* ------------------------------------------------------------------ */

/* q6afe.h: AFE_MAX_PORTS is AFE_PORT_MAX == 129 */
#define ELISH_AFE_MAX_PORTS		129
/* q6adm.h */
#define ELISH_MAX_COPPS_PER_PORT	8
#define ELISH_ADM_PATH_PLAYBACK		0x1
#define ELISH_NULL_COPP_TOPOLOGY	0x00010312

/* q6asm.h enum: LEGACY_PCM_MODE == 0 */
#define ELISH_LEGACY_PCM_MODE		0

/* AFE port enum, include/dt-bindings/sound/qcom,q6dsp-lpass-ports.h:61 */
#define ELISH_TERTIARY_TDM_RX_0		56

/* Q27 -> Q13 conversion (27 - 13) */
#define ELISH_Q27_TO_Q13_SHIFT		14
#define ELISH_Q27_MAX			0x08000000
#define ELISH_Q13_0DB			0x2000

/* ADM/AUDPROC volume control module and parameter IDs */
#define ELISH_AUDPROC_MODULE_ID_VOL_CTRL		0x00010BFE
#define ELISH_AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN	0x00010BFF
#define ELISH_INSTANCE_ID_0				0x0000

#define ELISH_CMD_SET_PP_PARAMS_V5	0x00010328
#define ELISH_CMD_SET_PP_PARAMS_V6	0x0001035D

/*
 * ASM (stream) volume control.
 *
 * The ADM/COPP master gain above only attenuates (its 0 dB ceiling is Q13
 * 0x2000); the actual loudness headroom lives in the ASM *stream* volume
 * module, which mainline never programs.  It uses the very same
 * AUDPROC/ASM module + parameter IDs and the same 4-byte Q13 payload, but a
 * different command opcode and the ASM service (APR_SVC_ASM == 7) instead of
 * the ADM service (8):
 *
 *   ASM_STREAM_CMD_SET_PP_PARAMS_V3 = 0x0001320D   (instance IDs / API v3)
 *   ASM_STREAM_CMD_SET_PP_PARAMS_V2 = 0x00010DA1   (older firmware)
 *   module_id = ASM_MODULE_ID_VOL_CTRL         = 0x00010BFE
 *   instance_id = INSTANCE_ID_0                = 0x0000
 *   param_id  = ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN = 0x00010BFF
 *   param_size = 4  (u16 master_gain in Q13 + u16 reserved)
 *
 * ASM_MODULE_ID_VOL_CTRL / ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN are numerically
 * identical to the AUDPROC_* spellings used by the ADM path (see
 * apr_audio-v2.h: `#define AUDPROC_MODULE_ID_VOL_CTRL ASM_MODULE_ID_VOL_CTRL`),
 * and correspond to SOFT_VOLUME_INSTANCE_1.
 *
 * Downstream reference: q6asm.c __q6asm_set_volume() (q6asm.c:8850),
 * q6asm_set_pp_params() (q6asm.c:2908), q6asm_pack_and_set_pp_param_in_band()
 * (q6asm.c:3009) and q6asm_set_soft_volume_module_instance_ids() (q6asm.c:3058).
 */
#define ELISH_ASM_CMD_SET_PP_PARAMS_V2		0x00010DA1
#define ELISH_ASM_CMD_SET_PP_PARAMS_V3		0x0001320D

/* mainline q6asm.h: #define MAX_SESSIONS 8 */
#define ELISH_ASM_MAX_SESSIONS			8

#define ELISH_MAX_TARGETS		32

/* ------------------------------------------------------------------ */
/* verbatim copies of the driver-private structures                    */
/*                                                                     */
/* struct q6copp: q6adm.c:37-54                                        */
/* struct q6adm : q6adm.c:56-66                                        */
/* struct q6core_svc_api_info: q6core.h:6-10                           */
/* ------------------------------------------------------------------ */

struct q6core_svc_api_info {
	u32 service_id;
	u32 api_version;
	u32 api_branch_version;
};

struct q6adm;

/* copied field-for-field from q6adm.c:37-54 - keep in sync! */
struct q6copp {
	int afe_port;
	int copp_idx;
	int id;
	int topology;
	int mode;
	int rate;
	int bit_width;
	int channels;
	int app_type;
	int acdb_id;

	struct aprv2_ibasic_rsp_result_t result;
	struct kref refcount;
	wait_queue_head_t wait;
	struct list_head node;
	struct q6adm *adm;
};

/* copied field-for-field from q6adm.c:56-66 - keep in sync! */
struct q6adm {
	struct apr_device *apr;
	struct device *dev;
	struct q6core_svc_api_info ainfo;
	unsigned long copp_bitmap[ELISH_AFE_MAX_PORTS];
	struct list_head copps_list;
	spinlock_t copps_list_lock;
	struct aprv2_ibasic_rsp_result_t result;
	struct mutex lock;
	wait_queue_head_t matrix_map_wait;
};

/* opaque to us - only ever passed back to q6asm_get_session_id() */
struct audio_client;

/*
 * copied field-for-field from mainline sound/soc/qcom/qdsp6/q6asm.c
 * (struct q6asm, q6asm.c:252-260) - keep in sync!
 *
 * dev_get_drvdata(&asm_adev->dev) is this struct (q6asm_probe() does
 * dev_set_drvdata(dev, q6asm)).  We only walk the session table to find the
 * live audio_client pointers; the session id itself is then obtained with the
 * exported q6asm_get_session_id().
 */
struct q6asm {
	struct apr_device *adev;
	struct device *dev;
	struct q6core_svc_api_info ainfo;
	wait_queue_head_t mem_wait;
	spinlock_t slock;
	struct audio_client *session[ELISH_ASM_MAX_SESSIONS + 1];
};

/* ------------------------------------------------------------------ */
/* exported symbols provided by the running kernel                     */
/* ------------------------------------------------------------------ */

/* sound/soc/qcom/qdsp6/q6afe.c - EXPORT_SYMBOL_GPL */
int q6afe_get_port_id(int index);

/* sound/soc/qcom/qdsp6/q6core.c - EXPORT_SYMBOL_GPL */
bool q6core_is_adsp_ready(void);

/* sound/soc/qcom/qdsp6/q6adm.c - EXPORT_SYMBOL_GPL.
 * Only used by the optional use_q6adm_open path. */
struct q6copp *q6adm_open(struct device *dev, int port_id, int path, int rate,
			  int channel_mode, int topology, int perf_mode,
			  u16 bit_width, int app_type, int acdb_id);
int q6adm_close(struct device *dev, struct q6copp *copp);

/* sound/soc/qcom/qdsp6/q6asm.c - EXPORT_SYMBOL_GPL (confirmed present in the
 * running kernel's Module.symvers).  Not declared in the installed headers,
 * so we declare it ourselves. */
int q6asm_get_session_id(struct audio_client *c);

/* ------------------------------------------------------------------ */
/* module parameters                                                   */
/* ------------------------------------------------------------------ */

static char *adm_dev_name = "aprsvc:service:4:8";
module_param(adm_dev_name, charp, 0444);
MODULE_PARM_DESC(adm_dev_name,
	"APR device name of the ADM service (default \"aprsvc:service:4:8\"; "
	"domain 4 = ADSP, service 8 = APR_SVC_ADM). If not found the module "
	"falls back to scanning the apr bus for the qcom-q6adm driver.");

static int port_index = -1;
module_param(port_index, int, 0644);
MODULE_PARM_DESC(port_index,
	"AFE port *index* (enum value) to program; -1 = auto-detect the index "
	"whose q6afe_get_port_id() is 0x9020 (AFE_PORT_ID_TERTIARY_TDM_RX, the "
	"elish 'Tertiary TDM Playback' speaker link). Use -2 to target every "
	"port. Override on the command line if detection picks the wrong link.");

static int copp_idx = -1;
module_param(copp_idx, int, 0644);
MODULE_PARM_DESC(copp_idx, "COPP index within the port (default -1 = all).");

static int copp_id = -1;
module_param(copp_id, int, 0644);
MODULE_PARM_DESC(copp_id,
	"Explicit DSP COPP handle for dest_port (default -1 = discover it "
	"from the q6adm COPP list; only use if auto-discovery misbehaves).");

static int rate = 48000;
module_param(rate, int, 0644);
MODULE_PARM_DESC(rate, "Only used by use_q6adm_open=1 (default 48000).");

static int channels = 2;
module_param(channels, int, 0644);
MODULE_PARM_DESC(channels, "Only used by use_q6adm_open=1 (default 2).");

static int bit_width = 24;
module_param(bit_width, int, 0644);
MODULE_PARM_DESC(bit_width, "Only used by use_q6adm_open=1 (default 24).");

/*
 * COPP topology id used by the use_q6adm_open=1 path.
 *
 * mainline hardcodes NULL_COPP_TOPOLOGY (0x00010312, a pass-through COPP) in
 * q6routing.c:393, so the ADSP never instantiates the real speaker audproc
 * chain.  Android opens the speaker COPP with 0x1000a100 instead (measured:
 * "adm_open:port 0x9020 ... topology 0x1000a100 ... app_type 69940
 * acdb_id 10011").  Set this to 0x1000a100 to find out whether the DSP
 * firmware already knows that topology -- if q6adm_open() succeeds, the
 * kernel-side q6routing patch (0003) is enough to get the real chain.
 * Default keeps the upstream NULL topology so nothing changes unless asked.
 */
static int copp_topology = ELISH_NULL_COPP_TOPOLOGY;
module_param(copp_topology, int, 0644);
MODULE_PARM_DESC(copp_topology,
	"COPP topology id for use_q6adm_open=1 (default 0x00010312 = "
	"NULL_COPP_TOPOLOGY; try 0x1000a100 = Android speaker topology).");

static bool use_q6adm_open;
module_param(use_q6adm_open, bool, 0644);
MODULE_PARM_DESC(use_q6adm_open,
	"Use q6adm_open()+->id instead of walking the q6adm COPP list "
	"(default false). q6adm_open() returns the existing COPP when the "
	"playback parameters match, otherwise it creates a new one.");

static bool close_on_exit;
module_param(close_on_exit, bool, 0644);
MODULE_PARM_DESC(close_on_exit,
	"Call q6adm_close() on a COPP obtained via use_q6adm_open (default "
	"false). DANGEROUS: q6adm_close() always sends DEVICE_CLOSE, so if the "
	"COPP is shared with an active playback stream this stops the audio.");

static bool use_v5;
module_param(use_v5, bool, 0644);
MODULE_PARM_DESC(use_v5,
	"Send ADM_CMD_SET_PP_PARAMS_V5 + param_hdr_v1 instead of V6 + "
	"param_hdr_v3 (default false; V6 is correct for sm8250/ADM API v3).");

static char *asm_dev_name = "aprsvc:service:4:7";
module_param(asm_dev_name, charp, 0444);
MODULE_PARM_DESC(asm_dev_name,
	"APR device name of the ASM service (default \"aprsvc:service:4:7\"; "
	"domain 4 = ADSP, service 7 = APR_SVC_ASM). If not found the module "
	"falls back to scanning the apr bus for the qcom-q6asm driver.");

static int asm_stream_id = 1;
module_param(asm_stream_id, int, 0644);
MODULE_PARM_DESC(asm_stream_id,
	"ASM stream id packed into src_port/dest_port "
	"((session << 8) | stream_id). Mainline q6asm-dai uses 1 for playback "
	"(toggling to 2 for gapless), which is also the default here.");

static bool use_asm_v2;
module_param(use_asm_v2, bool, 0644);
MODULE_PARM_DESC(use_asm_v2,
	"Send ASM_STREAM_CMD_SET_PP_PARAMS_V2 (0x00010DA1) + param_hdr_v1 "
	"instead of V3 (0x0001320D) + param_hdr_v3 (default false; V3 with "
	"instance IDs is correct for sm8250/ASM API v3).");

static int asm_q13 = -1;
module_param(asm_q13, int, 0644);
MODULE_PARM_DESC(asm_q13,
	"Optionally apply this raw Q13 ASM stream gain (0x2000 == 0 dB) at "
	"load time (default -1 = do not touch the ASM volume).");

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static struct device *elish_adm_dev;	/* held reference from bus_find_device */
static struct device *elish_asm_dev;	/* held reference from bus_find_device */
static struct kobject *elish_kobj;

static int last_gain_q27 = ELISH_Q27_MAX;
static int last_q13 = ELISH_Q13_0DB;
static int last_dest_port = -1;
static int last_port_idx = -1;
static int last_copp_idx = -1;

/* last ASM (stream) volume send */
static int last_asm_q27 = ELISH_Q27_MAX;
static int last_asm_q13 = ELISH_Q13_0DB;
static int last_asm_session = -1;
static int last_asm_stream_id = -1;
static int last_asm_src_port = -1;
static u32 last_asm_opcode;

/* AFE port index detected for AFE_PORT_ID_TERTIARY_TDM_RX (0x9020) */
static int detected_port_index = -1;

/*
 * Resolve the configured port selector to a concrete AFE port index, or -1
 * to mean "every port".  port_index == -1 auto-detects 0x9020, -2 means all
 * ports, >= 0 is used verbatim.
 */
static int elish_target_port(void)
{
	if (port_index >= 0)
		return port_index;
	if (port_index == -2)
		return -1;
	return detected_port_index;
}

/*
 * Walk the AFE port table through the exported q6afe_get_port_id() and find
 * the index whose real port id is 0x9020 (AFE_PORT_ID_TERTIARY_TDM_RX =
 * AFE_PORT_ID_TDM_PORT_RANGE_START 0x9000 + 0x20, mainline q6afe.c:128/207).
 * The DT port-index cells are not reliable, so detect instead of hardcoding.
 */
static int elish_detect_port_index(void)
{
	int i;

	for (i = 0; i < ELISH_AFE_MAX_PORTS; i++) {
		if (q6afe_get_port_id(i) == 0x9020)
			return i;
	}

	return -1;
}

/* device_find_child() callback: match the first child of the ADM device */
static int elish_match_any_child(struct device *dev, void *data)
{
	return 1;
}

/* ------------------------------------------------------------------ */
/* packet building / sending                                           */
/* ------------------------------------------------------------------ */

struct elish_param_hdr_v1 {
	u32 module_id;
	u32 param_id;
	u16 param_size;
	u16 reserved;
} __packed;

struct elish_param_hdr_v3 {
	u32 module_id;
	u16 instance_id;
	u16 reserved;
	u32 param_id;
	u32 param_size;
} __packed;

struct elish_master_gain {
	u16 master_gain;
	u16 reserved;
} __packed;

/* 36 bytes of fixed header + room for the largest param blob (20) */
struct elish_pp_pkt {
	struct apr_hdr hdr;		/* 20 */
	u32 payload_addr_lsw;		/* 12-byte mem_mapping_hdr */
	u32 payload_addr_msw;
	u32 mem_map_handle;
	u32 payload_size;		/* 4 */
	u8 param_data[24];
} __packed;

/*
 * Pack the volume-control PP-param blob: parameter header + 4-byte Q13 gain
 * payload.  v1 selects param_hdr_v1 (V2/V5 commands, no instance ID); else
 * param_hdr_v3 (V3/V6 commands, with instance ID).  Returns the blob size.
 *
 * Both the ADM/COPP gain and the ASM/stream gain use the same blob; only the
 * opcode, the APR service and the port addressing differ.
 */
static int elish_pack_vol_param(u8 *buf, bool v1, u16 q13)
{
	struct elish_master_gain gain = {
		.master_gain = q13,
		.reserved = 0,
	};

	if (v1) {
		struct elish_param_hdr_v1 *h = (void *)buf;

		h->module_id = ELISH_AUDPROC_MODULE_ID_VOL_CTRL;
		h->param_id = ELISH_AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN;
		h->param_size = sizeof(gain);
		memcpy(buf + sizeof(*h), &gain, sizeof(gain));
		return sizeof(*h) + sizeof(gain);
	} else {
		struct elish_param_hdr_v3 *h = (void *)buf;

		h->module_id = ELISH_AUDPROC_MODULE_ID_VOL_CTRL;
		h->instance_id = ELISH_INSTANCE_ID_0;
		h->param_id = ELISH_AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN;
		h->param_size = sizeof(gain);
		memcpy(buf + sizeof(*h), &gain, sizeof(gain));
		return sizeof(*h) + sizeof(gain);
	}
}

/*
 * Send one COPP master-gain PP-param. q27 is the userspace value; it is
 * converted to the firmware's Q13 unless raw is true.
 */
static int elish_send_volume(struct apr_device *adev, int port_idx,
			     int cidx, int dsp_copp_id, int q27, bool raw)
{
	struct elish_pp_pkt pkt;
	int afe_port_id, param_size, ret;
	u16 q13;
	u32 opcode;

	if (port_idx < 0 || port_idx >= ELISH_AFE_MAX_PORTS)
		return -EINVAL;
	if (cidx < 0 || cidx >= ELISH_MAX_COPPS_PER_PORT)
		return -EINVAL;
	if (dsp_copp_id < 0)
		return -EINVAL;

	afe_port_id = q6afe_get_port_id(port_idx);
	if (afe_port_id < 0)
		return -EINVAL;

	q13 = raw ? (u16)q27 : (u16)(q27 >> ELISH_Q27_TO_Q13_SHIFT);

	memset(&pkt, 0, sizeof(pkt));
	pkt.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					  APR_HDR_LEN(APR_HDR_SIZE),
					  APR_PKT_VER);
	pkt.hdr.src_port = afe_port_id;
	pkt.hdr.dest_port = dsp_copp_id;
	pkt.hdr.token = port_idx << 16 | cidx;

	opcode = use_v5 ? ELISH_CMD_SET_PP_PARAMS_V5 :
			  ELISH_CMD_SET_PP_PARAMS_V6;
	param_size = elish_pack_vol_param(pkt.param_data, use_v5, q13);

	pkt.hdr.opcode = opcode;
	pkt.payload_size = param_size;
	pkt.hdr.pkt_size = offsetof(struct elish_pp_pkt, param_data) +
			   param_size;

	ret = apr_send_pkt(adev, (struct apr_pkt *)&pkt);
	if (ret < 0) {
		pr_err("elish_adsp_vol: apr_send_pkt failed: %d\n", ret);
		return ret;
	}

	pr_info("elish_adsp_vol: ADM port_idx=%d copp_idx=%d dest_port=%d src_port=0x%x opcode=0x%05x q13=0x%04x (raw=%d)\n",
		port_idx, cidx, dsp_copp_id, afe_port_id, opcode, q13, raw);

	return 0;
}

/*
 * Send one ASM *stream* volume PP-param to a live playback session.
 *
 * The packet is delivered through the ASM apr_device, so apr_send_pkt() fills
 * in src_svc = dest_svc = APR_SVC_ASM (7), src_domain = APR_DOMAIN_APPS (5)
 * and dest_domain = APR_DOMAIN_ADSP (4).  Only the port addressing is ours:
 * mainline q6asm_add_hdr() uses
 *     src_port = dest_port = ((session << 8) & 0xFF00) | stream_id
 * and token = session.  We mirror that exactly.
 */
static int elish_send_asm_volume(struct apr_device *adev, int session,
				 int stream_id, int q13)
{
	struct elish_pp_pkt pkt;
	u16 port;
	int param_size, ret;
	u32 opcode;

	if (session < 0 || session > 0xff)
		return -EINVAL;
	if (stream_id < 0 || stream_id > 0xff)
		return -EINVAL;

	port = ((session << 8) & 0xff00) | (stream_id & 0xff);

	memset(&pkt, 0, sizeof(pkt));
	pkt.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					  APR_HDR_LEN(APR_HDR_SIZE),
					  APR_PKT_VER);
	pkt.hdr.src_port = port;
	pkt.hdr.dest_port = port;
	pkt.hdr.token = session;

	opcode = use_asm_v2 ? ELISH_ASM_CMD_SET_PP_PARAMS_V2 :
			      ELISH_ASM_CMD_SET_PP_PARAMS_V3;
	param_size = elish_pack_vol_param(pkt.param_data, use_asm_v2,
					  (u16)q13);

	pkt.hdr.opcode = opcode;
	pkt.payload_size = param_size;
	pkt.hdr.pkt_size = offsetof(struct elish_pp_pkt, param_data) +
			   param_size;

	ret = apr_send_pkt(adev, (struct apr_pkt *)&pkt);
	if (ret < 0) {
		pr_err("elish_adsp_vol: ASM apr_send_pkt failed: %d\n", ret);
		return ret;
	}

	pr_info("elish_adsp_vol: ASM session=%d stream_id=%d src_port=0x%04x dest_port=0x%04x opcode=0x%05x q13=0x%04x\n",
		session, stream_id, port, port, opcode, (u16)q13);

	last_asm_session = session;
	last_asm_stream_id = stream_id;
	last_asm_src_port = port;
	last_asm_opcode = opcode;
	last_asm_q13 = (u16)q13;

	return 0;
}

/*
 * Collect the DSP session ids of the currently live ASM audio clients by
 * walking the q6asm session table.  a->slock is held while reading the slots
 * so a concurrent q6asm_audio_client_release() cannot free the object between
 * the NULL check and q6asm_get_session_id() (that path clears the slot under
 * the same lock before kfree()).
 */
static int elish_collect_asm_sessions(int *sessions, int max)
{
	struct q6asm *a;
	struct audio_client *ac;
	unsigned long flags;
	int i, n = 0;

	if (!elish_asm_dev)
		return -ENODEV;

	a = dev_get_drvdata(elish_asm_dev);
	if (!a) {
		pr_err("elish_adsp_vol: ASM drvdata is NULL\n");
		return -ENODEV;
	}

	spin_lock_irqsave(&a->slock, flags);
	for (i = 1; i <= ELISH_ASM_MAX_SESSIONS && n < max; i++) {
		ac = a->session[i];
		if (!ac)
			continue;
		sessions[n++] = q6asm_get_session_id(ac);
	}
	spin_unlock_irqrestore(&a->slock, flags);

	return n;
}

/*
 * Apply a stream gain to every live ASM session.  raw selects Q13 (true) or
 * Q27 (false) for value.
 */
static int elish_apply_asm_gain(int value, bool raw)
{
	struct apr_device *adev;
	int sessions[ELISH_ASM_MAX_SESSIONS];
	int q13, n, i, ret = 0;

	if (!elish_asm_dev)
		return -ENODEV;

	if (!q6core_is_adsp_ready()) {
		pr_warn("elish_adsp_vol: ADSP not ready\n");
		return -EAGAIN;
	}

	q13 = raw ? (value & 0xffff) : (value >> ELISH_Q27_TO_Q13_SHIFT);

	n = elish_collect_asm_sessions(sessions, ARRAY_SIZE(sessions));
	if (n < 0)
		return n;
	if (n == 0) {
		pr_warn("elish_adsp_vol: no live ASM session (is a playback stream open?)\n");
		return -ENOENT;
	}

	if (asm_stream_id < 0 || asm_stream_id > 0xff) {
		pr_err("elish_adsp_vol: invalid asm_stream_id %d\n",
		       asm_stream_id);
		return -EINVAL;
	}

	adev = to_apr_device(elish_asm_dev);
	for (i = 0; i < n; i++) {
		ret = elish_send_asm_volume(adev, sessions[i], asm_stream_id,
					    q13);
		if (ret)
			break;
	}

	if (!ret) {
		last_asm_q27 = raw ? (q13 << ELISH_Q27_TO_Q13_SHIFT) : value;
		last_asm_q13 = q13;
	}

	return ret;
}

struct elish_target {
	int port_idx;
	int copp_idx;
	int dsp_copp_id;
};

/*
 * Copy the currently open COPPs out of the q6adm list.  We must not hold
 * copps_list_lock while sending APR commands, so only the scalar fields are
 * copied here.
 */
static int elish_collect_targets(struct q6adm *adm, struct elish_target *t,
				 int max)
{
	struct q6copp *c;
	unsigned long flags;
	int tport = elish_target_port();
	int n = 0;

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	list_for_each_entry(c, &adm->copps_list, node) {
		if (tport >= 0 && c->afe_port != tport)
			continue;
		if (copp_idx >= 0 && c->copp_idx != copp_idx)
			continue;
		if (n >= max)
			break;

		t[n].port_idx = c->afe_port;
		t[n].copp_idx = c->copp_idx;
		t[n].dsp_copp_id = c->id;
		n++;
	}
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	return n;
}

/*
 * Optional path: let q6adm_open() find/create the COPP for the configured
 * playback parameters and read its firmware handle from the returned pointer
 * (struct q6copp.id lives at offset 8).
 *
 * The COPP/child pair is opened once and cached, so repeated gain writes do
 * not leak references.  q6adm_close() always sends ADM_CMD_DEVICE_CLOSE_V5,
 * so it is only called on unload when close_on_exit=1 - closing a COPP that
 * is shared with an active playback stream would stop that stream.
 */
static struct q6copp *elish_open_copp;

static int elish_targets_via_q6adm_open(struct elish_target *t, int max)
{
	struct q6copp *copp;
	struct device *child;
	int tport = elish_target_port();

	if (elish_open_copp) {
		t[0].port_idx = tport;
		/* struct q6copp: copp_idx is the second int */
		t[0].copp_idx = ((int *)elish_open_copp)[1];
		/* struct q6copp: id (the firmware handle) is the third int */
		t[0].dsp_copp_id = ((int *)elish_open_copp)[2];
		return 1;
	}

	if (tport < 0)
		tport = detected_port_index;
	if (tport < 0) {
		pr_err("elish_adsp_vol: no usable AFE port index for q6adm_open\n");
		return -EINVAL;
	}

	child = device_find_child(elish_adm_dev, NULL, elish_match_any_child);
	if (!child) {
		pr_err("elish_adsp_vol: no child device of the ADM service found\n");
		return -ENODEV;
	}

	copp = q6adm_open(child, tport, ELISH_ADM_PATH_PLAYBACK, rate, channels,
			  copp_topology, ELISH_LEGACY_PCM_MODE,
			  bit_width, 0, 0);
	if (IS_ERR_OR_NULL(copp)) {
		pr_err("elish_adsp_vol: q6adm_open failed: %d\n",
		       PTR_ERR_OR_ZERO(copp));
		put_device(child);
		return -EINVAL;
	}

	elish_open_copp = copp;
	put_device(child);

	t[0].port_idx = tport;
	t[0].copp_idx = ((int *)copp)[1];
	t[0].dsp_copp_id = ((int *)copp)[2];

	pr_info("elish_adsp_vol: q6adm_open(port_idx=%d) -> copp_idx=%d dsp_copp_id=%d\n",
		tport, t[0].copp_idx, t[0].dsp_copp_id);

	return 1;
}

static int elish_apply_gain(int q27, bool raw)
{
	struct elish_target targets[ELISH_MAX_TARGETS];
	struct q6adm *adm;
	int n, i, ret = 0;
	int tport = elish_target_port();

	if (!elish_adm_dev)
		return -ENODEV;

	if (!q6core_is_adsp_ready()) {
		pr_warn("elish_adsp_vol: ADSP not ready\n");
		return -EAGAIN;
	}

	adm = dev_get_drvdata(elish_adm_dev);
	if (!adm) {
		pr_err("elish_adsp_vol: ADM drvdata is NULL\n");
		return -ENODEV;
	}

	if (copp_id >= 0) {
		if (tport < 0 || copp_idx < 0) {
			pr_err("elish_adsp_vol: copp_id needs an explicit port_index and copp_idx\n");
			return -EINVAL;
		}
		targets[0].port_idx = tport;
		targets[0].copp_idx = copp_idx;
		targets[0].dsp_copp_id = copp_id;
		n = 1;
	} else if (use_q6adm_open) {
		n = elish_targets_via_q6adm_open(targets, ELISH_MAX_TARGETS);
		if (n <= 0)
			return n;
	} else {
		n = elish_collect_targets(adm, targets, ELISH_MAX_TARGETS);
		if (n == 0) {
			pr_warn("elish_adsp_vol: no COPP matches port_index=%d copp_idx=%d (is a stream open?)\n",
				tport, copp_idx);
			return -ENOENT;
		}
	}

	for (i = 0; i < n; i++) {
		ret = elish_send_volume(to_apr_device(elish_adm_dev),
					targets[i].port_idx,
					targets[i].copp_idx,
					targets[i].dsp_copp_id, q27, raw);
		if (ret)
			break;

		last_dest_port = targets[i].dsp_copp_id;
		last_port_idx = targets[i].port_idx;
		last_copp_idx = targets[i].copp_idx;
		last_gain_q27 = q27;
		last_q13 = raw ? q27 & 0xffff : q27 >> ELISH_Q27_TO_Q13_SHIFT;
	}

	return ret;
}

/* ------------------------------------------------------------------ */
/* sysfs interface: /sys/kernel/elish_adsp_vol/                        */
/* ------------------------------------------------------------------ */

static ssize_t gain_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "%d\n", last_gain_q27);
}

static ssize_t gain_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	val = clamp_t(int, val, 0, ELISH_Q27_MAX);
	ret = elish_apply_gain(val, false);

	return ret ? ret : count;
}

static ssize_t gain_q13_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "%d\n", last_q13);
}

static ssize_t gain_q13_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	val = clamp_t(int, val, 0, 0xffff);
	ret = elish_apply_gain(val, true);

	return ret ? ret : count;
}

static ssize_t copp_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf,
			  "port_index=%d copp_idx=%d last_dest_port=%d last_q13=0x%04x last_gain_q27=%d use_v5=%d use_q6adm_open=%d asm_dev=%s\n",
			  last_port_idx, last_copp_idx, last_dest_port,
			  last_q13, last_gain_q27, use_v5, use_q6adm_open,
			  elish_asm_dev ? dev_name(elish_asm_dev) : "(none)");
}

/* ---- ASM stream volume ---- */

static ssize_t gain_asm_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "%d\n", last_asm_q27);
}

static ssize_t gain_asm_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	val = clamp_t(int, val, 0, ELISH_Q27_MAX);
	ret = elish_apply_asm_gain(val, false);

	return ret ? ret : count;
}

static ssize_t gain_asm_q13_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", last_asm_q13);
}

static ssize_t gain_asm_q13_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	val = clamp_t(int, val, 0, 0xffff);
	ret = elish_apply_asm_gain(val, true);

	return ret ? ret : count;
}

static ssize_t asm_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	int sessions[ELISH_ASM_MAX_SESSIONS];
	int n;

	n = elish_collect_asm_sessions(sessions, ARRAY_SIZE(sessions));

	return sysfs_emit(buf,
			  "dev=%s last_session=%d last_stream_id=%d last_src_port=0x%04x last_dest_port=0x%04x last_opcode=0x%05x last_q13=0x%04x last_gain_asm_q27=%d use_asm_v2=%d asm_stream_id=%d live_sessions=%d\n",
			  elish_asm_dev ? dev_name(elish_asm_dev) : "(none)",
			  last_asm_session, last_asm_stream_id,
			  last_asm_src_port, last_asm_src_port, last_asm_opcode,
			  last_asm_q13, last_asm_q27, use_asm_v2,
			  asm_stream_id, n);
}

static struct kobj_attribute gain_attr = __ATTR_RW(gain);
static struct kobj_attribute gain_q13_attr = __ATTR_RW(gain_q13);
static struct kobj_attribute gain_asm_attr = __ATTR_RW(gain_asm);
static struct kobj_attribute gain_asm_q13_attr = __ATTR_RW(gain_asm_q13);
static struct kobj_attribute copp_attr = __ATTR_RO(copp);
static struct kobj_attribute asm_attr = __ATTR_RO(asm);

static struct attribute *elish_attrs[] = {
	&gain_attr.attr,
	&gain_q13_attr.attr,
	&gain_asm_attr.attr,
	&gain_asm_q13_attr.attr,
	&copp_attr.attr,
	&asm_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(elish);

/* ------------------------------------------------------------------ */
/* ADM / ASM device lookup                                             */
/* ------------------------------------------------------------------ */

static int elish_match_driver(struct device *dev, const void *data)
{
	return dev->driver && !strcmp(dev->driver->name, "qcom-q6adm");
}

static int elish_match_asm_driver(struct device *dev, const void *data)
{
	return dev->driver && !strcmp(dev->driver->name, "qcom-q6asm");
}

static int elish_match_name(struct device *dev, const void *data)
{
	return sysfs_streq(dev_name(dev), data);
}

static struct device *elish_find_asm_device(void)
{
	struct device *dev;

	dev = bus_find_device(&aprbus, NULL, asm_dev_name, elish_match_name);
	if (dev) {
		pr_info("elish_adsp_vol: found ASM APR device '%s'\n",
			dev_name(dev));
		return dev;
	}

	pr_warn("elish_adsp_vol: '%s' not found, scanning apr bus for qcom-q6asm\n",
		asm_dev_name);
	dev = bus_find_device(&aprbus, NULL, NULL, elish_match_asm_driver);
	if (dev)
		pr_info("elish_adsp_vol: found ASM APR device '%s' by driver\n",
			dev_name(dev));
	else
		pr_warn("elish_adsp_vol: no ASM APR device found (ASM volume unavailable)\n");

	return dev;
}

static struct device *elish_find_adm_device(void)
{
	struct device *dev;

	dev = bus_find_device(&aprbus, NULL, adm_dev_name, elish_match_name);
	if (dev) {
		pr_info("elish_adsp_vol: found ADM APR device '%s'\n",
			dev_name(dev));
		return dev;
	}

	pr_warn("elish_adsp_vol: '%s' not found, scanning apr bus for qcom-q6adm\n",
		adm_dev_name);
	dev = bus_find_device(&aprbus, NULL, NULL, elish_match_driver);
	if (dev)
		pr_info("elish_adsp_vol: found ADM APR device '%s' by driver\n",
			dev_name(dev));
	else
		pr_err("elish_adsp_vol: no ADM APR device found\n");

	return dev;
}

/* ------------------------------------------------------------------ */
/* module init/exit                                                    */
/* ------------------------------------------------------------------ */

static int __init elish_adsp_vol_init(void)
{
	int ret;

	elish_adm_dev = elish_find_adm_device();
	if (!elish_adm_dev)
		return -ENODEV;

	elish_kobj = kobject_create_and_add("elish_adsp_vol", kernel_kobj);
	if (!elish_kobj) {
		ret = -ENOMEM;
		goto err_put;
	}

	ret = sysfs_create_groups(elish_kobj, elish_groups);
	if (ret)
		goto err_kobj;

	/*
	 * The ASM service is optional: without it the ADM/COPP path still
	 * works, the gain_asm* attributes just return -ENODEV.
	 */
	elish_asm_dev = elish_find_asm_device();

	detected_port_index = elish_detect_port_index();
	if (detected_port_index < 0) {
		pr_warn("elish_adsp_vol: could not detect AFE port 0x9020, falling back to index %d\n",
			ELISH_TERTIARY_TDM_RX_0);
		detected_port_index = ELISH_TERTIARY_TDM_RX_0;
	}

	pr_info("elish_adsp_vol: loaded (ADM=%s, ASM=%s)\n",
		dev_name(elish_adm_dev),
		elish_asm_dev ? dev_name(elish_asm_dev) : "(none)");
	pr_info("elish_adsp_vol: port_index: param=%d detected=%d (port 0x9020 = TERTIARY_TDM_RX), copp_idx=%d, copp_id=%d, use_q6adm_open=%d, use_v5=%d\n",
		port_index, detected_port_index, copp_idx, copp_id,
		use_q6adm_open, use_v5);
	pr_info("elish_adsp_vol: Q27 0x%x = 0 dB -> Q13 0x%x; write 'echo <q27> > /sys/kernel/elish_adsp_vol/gain'\n",
		ELISH_Q27_MAX, ELISH_Q13_0DB);
	pr_info("elish_adsp_vol: or raw firmware value 'echo <q13> > /sys/kernel/elish_adsp_vol/gain_q13' (e.g. %d = 0 dB)\n",
		ELISH_Q13_0DB);
	pr_info("elish_adsp_vol: ASM stream volume: use_asm_v2=%d asm_stream_id=%d asm_q13=%d; write 'echo <q27> > /sys/kernel/elish_adsp_vol/gain_asm' or raw 'echo <q13> > .../gain_asm_q13'\n",
		use_asm_v2, asm_stream_id, asm_q13);

	if (elish_asm_dev && asm_q13 >= 0) {
		ret = elish_apply_asm_gain(asm_q13, true);
		if (ret)
			pr_warn("elish_adsp_vol: initial asm_q13=0x%x not applied: %d\n",
				asm_q13, ret);
	}

	return 0;

err_kobj:
	kobject_put(elish_kobj);
err_put:
	put_device(elish_adm_dev);
	elish_adm_dev = NULL;
	return ret;
}

static void __exit elish_adsp_vol_exit(void)
{
	if (elish_open_copp) {
		if (close_on_exit) {
			struct device *child = device_find_child(elish_adm_dev,
								 NULL,
								 elish_match_any_child);

			pr_warn("elish_adsp_vol: closing cached COPP (port_idx=%d, copp_idx=%d)\n",
				elish_target_port(),
				((int *)elish_open_copp)[1]);
			if (child) {
				q6adm_close(child, elish_open_copp);
				put_device(child);
			}
		} else {
			pr_warn("elish_adsp_vol: leaving COPP open (close_on_exit=0); its reference is intentionally not dropped\n");
		}
		elish_open_copp = NULL;
	}
	if (elish_kobj) {
		sysfs_remove_groups(elish_kobj, elish_groups);
		kobject_put(elish_kobj);
	}
	if (elish_adm_dev) {
		put_device(elish_adm_dev);
		elish_adm_dev = NULL;
	}
	if (elish_asm_dev) {
		put_device(elish_asm_dev);
		elish_asm_dev = NULL;
	}
	pr_info("elish_adsp_vol: unloaded\n");
}

module_init(elish_adsp_vol_init);
module_exit(elish_adsp_vol_exit);

MODULE_DESCRIPTION("ADM (q6 DSP) COPP master-gain + ASM stream-volume control for Xiaomi Pad 5 Pro (sm8250/elish)");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("axis_rnd");
MODULE_VERSION("0.2");
