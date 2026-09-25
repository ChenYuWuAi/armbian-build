// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/soc/qcom/apr.h>
#include <linux/wait.h>
#include <sound/asound.h>
#include "q6adm.h"
#include "q6afe.h"
#include "q6core.h"
#include "q6dsp-common.h"
#include "q6dsp-errno.h"

#define ADM_CMD_DEVICE_OPEN_V5		0x00010326
#define ADM_CMDRSP_DEVICE_OPEN_V5	0x00010329
#define ADM_CMD_DEVICE_CLOSE_V5		0x00010327
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5	0x00010325
#define ADM_CMD_SET_PP_PARAMS_V5	0x00010328
#define ADM_CMD_SET_PP_PARAMS_V6	0x0001035D

/* ADSP ADM service API version that introduced instance IDs (param_hdr_v3) */
#define ADM_API_VERSION_INSTANCE_ID	3

/*
 * Post-processing module/parameter IDs of the ADSP "Volume Control" module.
 * Taken verbatim from the downstream Qualcomm techpack:
 *   techpack/audio/include/dsp/apr_audio-v2.h
 *     #define ASM_MODULE_ID_VOL_CTRL            0x00010BFE
 *     #define AUDPROC_MODULE_ID_VOL_CTRL ASM_MODULE_ID_VOL_CTRL
 *     #define ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN 0x00010BFF
 *     #define AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN \
 *                                     ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN
 *     #define INSTANCE_ID_0                     0x0000
 */
#define AUDPROC_MODULE_ID_VOL_CTRL		0x00010BFE
#define AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN	0x00010BFF
#define ADM_INSTANCE_ID_0			0x0000

/* 1.0 (0 dB) in Q13, the format the ADSP volume module expects on the wire */
#define Q6ADM_VOLUME_0DB_Q13			0x2000
/* right shift that converts Q27 -> Q13 (27 - 13) */
#define Q6ADM_Q27_TO_Q13_SHIFT			14

#define TIMEOUT_MS 1000
#define RESET_COPP_ID 99
#define INVALID_COPP_ID 0xFF
/* Definition for a legacy device session. */
#define ADM_LEGACY_DEVICE_SESSION	0
#define ADM_MATRIX_ID_AUDIO_RX		0
#define ADM_MATRIX_ID_AUDIO_TX		1

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

struct q6adm {
	struct apr_device *apr;
	struct device *dev;
	struct q6core_svc_api_info ainfo;
	unsigned long copp_bitmap[AFE_MAX_PORTS];
	struct list_head copps_list;
	spinlock_t copps_list_lock;
	struct aprv2_ibasic_rsp_result_t result;
	struct mutex lock;
	wait_queue_head_t matrix_map_wait;
};

struct q6adm_cmd_device_open_v5 {
	u16 flags;
	u16 mode_of_operation;
	u16 endpoint_id_1;
	u16 endpoint_id_2;
	u32 topology_id;
	u16 dev_num_channel;
	u16 bit_width;
	u32 sample_rate;
	u8 dev_channel_mapping[8];
} __packed;

struct q6adm_cmd_matrix_map_routings_v5 {
	u32 matrix_id;
	u32 num_sessions;
} __packed;

struct q6adm_session_map_node_v5 {
	u16 session_id;
	u16 num_copps;
} __packed;

/*
 * In-band "set post-processing parameters" command, as defined by the
 * downstream techpack (techpack/audio/include/dsp/apr_audio-v2.h):
 *
 *   struct adm_cmd_set_pp_params {
 *           struct apr_hdr apr_hdr;
 *           struct mem_mapping_hdr mem_hdr;
 *           u32 payload_size;
 *           u8 param_data[0];
 *   } __packed;
 *
 * The in-band case leaves mem_hdr zeroed and puts the packed parameter
 * header (param_hdr_v1 or param_hdr_v3) followed by the parameter payload
 * right after payload_size.
 */
struct q6adm_mem_mapping_hdr {
	u32 data_payload_addr_lsw;
	u32 data_payload_addr_msw;
	u32 mem_map_handle;
} __packed;

struct q6adm_cmd_set_pp_params {
	struct apr_hdr hdr;
	struct q6adm_mem_mapping_hdr mem_hdr;
	u32 payload_size;
	u8 param_data[];
} __packed;

/* param_hdr_v1: no instance ID (used with ADM_CMD_SET_PP_PARAMS_V5) */
struct q6adm_param_hdr_v1 {
	u32 module_id;
	u32 param_id;
	u16 param_size;
	u16 reserved;
} __packed;

/* param_hdr_v3: with instance ID (used with ADM_CMD_SET_PP_PARAMS_V6) */
struct q6adm_param_hdr_v3 {
	u32 module_id;
	u16 instance_id;
	u16 reserved;
	u32 param_id;
	u32 param_size;
} __packed;

/*
 * Payload of AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN.  Despite the "Q27"
 * naming of the userspace control, the ADSP's volume-control module takes a
 * *linear Q13* gain (0x2000 == 1.0 == 0 dB); the mainline AudioReach driver
 * uses the very same layout and default (VOL_CTRL_DEFAULT_GAIN 0x2000).
 * See NOTES.md for the evidence and for the Q27 -> Q13 conversion.
 */
struct q6adm_audproc_volume_ctrl_master_gain {
	u16 master_gain;
	u16 reserved;
} __packed;

static struct q6copp *q6adm_find_copp(struct q6adm *adm, int port_idx,
				  int copp_idx)
{
	struct q6copp *c;
	struct q6copp *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	list_for_each_entry(c, &adm->copps_list, node) {
		if ((port_idx == c->afe_port) && (copp_idx == c->copp_idx)) {
			ret = c;
			kref_get(&c->refcount);
			break;
		}
	}

	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	return ret;

}

static void q6adm_free_copp(struct kref *ref)
{
	struct q6copp *c = container_of(ref, struct q6copp, refcount);
	struct q6adm *adm = c->adm;
	unsigned long flags;

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	clear_bit(c->copp_idx, &adm->copp_bitmap[c->afe_port]);
	list_del(&c->node);
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);
	kfree(c);
}

static int q6adm_callback(struct apr_device *adev, struct apr_resp_pkt *data)
{
	struct aprv2_ibasic_rsp_result_t *result = data->payload;
	int port_idx, copp_idx;
	struct apr_hdr *hdr = &data->hdr;
	struct q6copp *copp;
	struct q6adm *adm = dev_get_drvdata(&adev->dev);

	if (!data->payload_size)
		return 0;

	copp_idx = (hdr->token) & 0XFF;
	port_idx = ((hdr->token) >> 16) & 0xFF;
	if (port_idx < 0 || port_idx >= AFE_MAX_PORTS) {
		dev_err(&adev->dev, "Invalid port idx %d token %d\n",
		       port_idx, hdr->token);
		return 0;
	}
	if (copp_idx < 0 || copp_idx >= MAX_COPPS_PER_PORT) {
		dev_err(&adev->dev, "Invalid copp idx %d token %d\n",
			copp_idx, hdr->token);
		return 0;
	}

	switch (hdr->opcode) {
	case APR_BASIC_RSP_RESULT: {
		if (result->status != 0) {
			dev_err(&adev->dev, "cmd = 0x%x return error = 0x%x\n",
				result->opcode, result->status);
		}
		switch (result->opcode) {
		case ADM_CMD_DEVICE_OPEN_V5:
		case ADM_CMD_DEVICE_CLOSE_V5:
		case ADM_CMD_SET_PP_PARAMS_V5:
		case ADM_CMD_SET_PP_PARAMS_V6:
			copp = q6adm_find_copp(adm, port_idx, copp_idx);
			if (!copp)
				return 0;

			copp->result = *result;
			wake_up(&copp->wait);
			kref_put(&copp->refcount, q6adm_free_copp);
			break;
		case ADM_CMD_MATRIX_MAP_ROUTINGS_V5:
			adm->result = *result;
			wake_up(&adm->matrix_map_wait);
			break;

		default:
			dev_err(&adev->dev, "Unknown Cmd: 0x%x\n",
				result->opcode);
			break;
		}
		return 0;
	}
	case ADM_CMDRSP_DEVICE_OPEN_V5: {
		struct adm_cmd_rsp_device_open_v5 {
			u32 status;
			u16 copp_id;
			u16 reserved;
		} __packed *open = data->payload;

		copp = q6adm_find_copp(adm, port_idx, copp_idx);
		if (!copp)
			return 0;

		if (open->copp_id == INVALID_COPP_ID) {
			dev_err(&adev->dev, "Invalid coppid rxed %d\n",
				open->copp_id);
			copp->result.status = ADSP_EBADPARAM;
			wake_up(&copp->wait);
			kref_put(&copp->refcount, q6adm_free_copp);
			break;
		}
		copp->result.opcode = hdr->opcode;
		copp->id = open->copp_id;
		wake_up(&copp->wait);
		kref_put(&copp->refcount, q6adm_free_copp);
	}
	break;
	default:
		dev_err(&adev->dev, "Unknown cmd:0x%x\n",
		       hdr->opcode);
		break;
	}

	return 0;
}

static struct q6copp *q6adm_alloc_copp(struct q6adm *adm, int port_idx)
{
	struct q6copp *c;
	int idx;

	idx = find_first_zero_bit(&adm->copp_bitmap[port_idx],
				  MAX_COPPS_PER_PORT);

	if (idx >= MAX_COPPS_PER_PORT)
		return ERR_PTR(-EBUSY);

	c = kzalloc(sizeof(*c), GFP_ATOMIC);
	if (!c)
		return ERR_PTR(-ENOMEM);

	set_bit(idx, &adm->copp_bitmap[port_idx]);
	c->copp_idx = idx;
	c->afe_port = port_idx;
	c->adm = adm;

	init_waitqueue_head(&c->wait);

	return c;
}

static int q6adm_apr_send_copp_pkt(struct q6adm *adm, struct q6copp *copp,
				   struct apr_pkt *pkt, uint32_t rsp_opcode)
{
	struct device *dev = adm->dev;
	uint32_t opcode = pkt->hdr.opcode;
	int ret;

	mutex_lock(&adm->lock);
	copp->result.opcode = 0;
	copp->result.status = 0;
	ret = apr_send_pkt(adm->apr, pkt);
	if (ret < 0) {
		dev_err(dev, "Failed to send APR packet\n");
		ret = -EINVAL;
		goto err;
	}

	/* Wait for the callback with copp id */
	if (rsp_opcode)
		ret = wait_event_timeout(copp->wait,
					 (copp->result.opcode == opcode) ||
					 (copp->result.opcode == rsp_opcode),
					 msecs_to_jiffies(TIMEOUT_MS));
	else
		ret = wait_event_timeout(copp->wait,
					 (copp->result.opcode == opcode),
					 msecs_to_jiffies(TIMEOUT_MS));

	if (!ret) {
		dev_err(dev, "ADM copp cmd timedout\n");
		ret = -ETIMEDOUT;
	} else if (copp->result.status > 0) {
		dev_err(dev, "DSP returned error[%d]\n",
			copp->result.status);
		ret = -EINVAL;
	}

err:
	mutex_unlock(&adm->lock);
	return ret;
}

static int q6adm_device_close(struct q6adm *adm, struct q6copp *copp,
			      int port_id, int copp_idx)
{
	struct apr_pkt close;

	close.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					APR_HDR_LEN(APR_HDR_SIZE),
					APR_PKT_VER);
	close.hdr.pkt_size = sizeof(close);
	close.hdr.src_port = port_id;
	close.hdr.dest_port = copp->id;
	close.hdr.token = port_id << 16 | copp_idx;
	close.hdr.opcode = ADM_CMD_DEVICE_CLOSE_V5;

	return q6adm_apr_send_copp_pkt(adm, copp, &close, 0);
}

static struct q6copp *q6adm_find_matching_copp(struct q6adm *adm,
					       int port_id, int topology,
					       int mode, int rate,
					       int channel_mode, int bit_width,
					       int app_type)
{
	struct q6copp *c;
	struct q6copp *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&adm->copps_list_lock, flags);

	list_for_each_entry(c, &adm->copps_list, node) {
		if ((port_id == c->afe_port) && (topology == c->topology) &&
		    (mode == c->mode) && (rate == c->rate) &&
		    (bit_width == c->bit_width) && (app_type == c->app_type)) {
			ret = c;
			kref_get(&c->refcount);
		}
	}
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	return ret;
}

static int q6adm_device_open(struct q6adm *adm, struct q6copp *copp,
			     int port_id, int path, int topology,
			     int channel_mode, int bit_width, int rate)
{
	struct q6adm_cmd_device_open_v5 *open;
	int afe_port = q6afe_get_port_id(port_id);
	struct apr_pkt *pkt;
	void *p;
	int ret, pkt_size;

	pkt_size = APR_HDR_SIZE + sizeof(*open);
	p = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	open = p + APR_HDR_SIZE;
	pkt->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					   APR_HDR_LEN(APR_HDR_SIZE),
					   APR_PKT_VER);
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.src_port = afe_port;
	pkt->hdr.dest_port = afe_port;
	pkt->hdr.token = port_id << 16 | copp->copp_idx;
	pkt->hdr.opcode = ADM_CMD_DEVICE_OPEN_V5;
	open->flags = ADM_LEGACY_DEVICE_SESSION;
	open->mode_of_operation = path;
	open->endpoint_id_1 = afe_port;
	open->topology_id = topology;
	open->dev_num_channel = channel_mode & 0x00FF;
	open->bit_width = bit_width;
	open->sample_rate = rate;

	ret = q6dsp_map_channels(&open->dev_channel_mapping[0],
				 channel_mode);
	if (ret)
		goto err;

	ret = q6adm_apr_send_copp_pkt(adm, copp, pkt,
				      ADM_CMDRSP_DEVICE_OPEN_V5);

err:
	kfree(pkt);
	return ret;
}

/**
 * q6adm_open() - open adm and grab a free copp
 *
 * @dev: Pointer to adm child device.
 * @port_id: port id
 * @path: playback or capture path.
 * @rate: rate at which copp is required.
 * @channel_mode: channel mode
 * @topology: adm topology id
 * @perf_mode: performace mode.
 * @bit_width: audio sample bit width
 * @app_type: Application type.
 * @acdb_id: ACDB id
 *
 * Return: Will be an negative on error or a valid copp pointer on success.
 */
struct q6copp *q6adm_open(struct device *dev, int port_id, int path, int rate,
	       int channel_mode, int topology, int perf_mode,
	       uint16_t bit_width, int app_type, int acdb_id)
{
	struct q6adm *adm = dev_get_drvdata(dev->parent);
	struct q6copp *copp;
	unsigned long flags;
	int ret = 0;

	if (port_id < 0) {
		dev_err(dev, "Invalid port_id %d\n", port_id);
		return ERR_PTR(-EINVAL);
	}

	copp = q6adm_find_matching_copp(adm, port_id, topology, perf_mode,
				      rate, channel_mode, bit_width, app_type);
	if (copp) {
		dev_err(dev, "Found Matching Copp 0x%x\n", copp->copp_idx);
		return copp;
	}

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	copp = q6adm_alloc_copp(adm, port_id);
	if (IS_ERR(copp)) {
		spin_unlock_irqrestore(&adm->copps_list_lock, flags);
		return ERR_CAST(copp);
	}

	list_add_tail(&copp->node, &adm->copps_list);
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	kref_init(&copp->refcount);
	copp->topology = topology;
	copp->mode = perf_mode;
	copp->rate = rate;
	copp->channels = channel_mode;
	copp->bit_width = bit_width;
	copp->app_type = app_type;

	ret = q6adm_device_open(adm, copp, port_id, path, topology,
				channel_mode, bit_width, rate);
	if (ret < 0) {
		kref_put(&copp->refcount, q6adm_free_copp);
		return ERR_PTR(ret);
	}

	return copp;
}
EXPORT_SYMBOL_GPL(q6adm_open);

/**
 * q6adm_get_copp_id() - get copp index
 *
 * @copp: Pointer to valid copp
 *
 * Return: Will be an negative on error or a valid copp index on success.
 **/
int q6adm_get_copp_id(struct q6copp *copp)
{
	if (!copp)
		return -EINVAL;

	return copp->copp_idx;
}
EXPORT_SYMBOL_GPL(q6adm_get_copp_id);

/**
 * q6adm_set_volume() - set the master gain of an ADM COPP
 *
 * @dev: Pointer to adm child device.
 * @port_id: AFE port index (the same value used for q6adm_open()).
 * @copp_idx: copp index of the ADM copp.
 * @volume: linear gain in Q27 (0x08000000 == 1.0 == 0 dB).
 *
 * Programs the "Volume Control" post-processing module of the given COPP
 * with AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN.  The ADSP expects a linear
 * Q13 gain, so the Q27 value is converted before it is put on the wire.
 *
 * Return: Will be an negative on error or a zero on success.
 */
int q6adm_set_volume(struct device *dev, int port_id, int copp_idx, int volume)
{
	struct q6adm *adm = dev_get_drvdata(dev->parent);
	struct q6adm_cmd_set_pp_params *set;
	struct q6adm_audproc_volume_ctrl_master_gain vol;
	struct q6copp *copp;
	int afe_port, pkt_size, ret;
	u32 param_size, opcode;

	if (port_id < 0 || port_id >= AFE_MAX_PORTS) {
		dev_err(dev, "Invalid port_id %d\n", port_id);
		return -EINVAL;
	}

	if (copp_idx < 0 || copp_idx >= MAX_COPPS_PER_PORT) {
		dev_err(dev, "Invalid copp_idx %d\n", copp_idx);
		return -EINVAL;
	}

	volume = clamp_t(int, volume, 0, Q6ADM_VOLUME_MAX_Q27);

	copp = q6adm_find_copp(adm, port_id, copp_idx);
	if (!copp) {
		dev_err(dev, "No copp for port %d idx %d\n", port_id, copp_idx);
		return -EINVAL;
	}

	afe_port = q6afe_get_port_id(port_id);
	if (afe_port < 0) {
		ret = -EINVAL;
		goto err_put_copp;
	}

	/*
	 * sm8250 (ADM API v3) understands the V6 command with a param_hdr_v3
	 * carrying an instance ID.  Only fall back to V5/param_hdr_v1 for
	 * ADSP firmware that reports a known older API version.
	 */
	if (adm->ainfo.api_version && adm->ainfo.api_version <
	    ADM_API_VERSION_INSTANCE_ID) {
		opcode = ADM_CMD_SET_PP_PARAMS_V5;
		param_size = sizeof(struct q6adm_param_hdr_v1) +
			     sizeof(vol);
	} else {
		opcode = ADM_CMD_SET_PP_PARAMS_V6;
		param_size = sizeof(struct q6adm_param_hdr_v3) +
			     sizeof(vol);
	}

	pkt_size = sizeof(*set) + param_size;
	set = kzalloc(pkt_size, GFP_KERNEL);
	if (!set) {
		ret = -ENOMEM;
		goto err_put_copp;
	}

	set->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					   APR_HDR_LEN(APR_HDR_SIZE),
					   APR_PKT_VER);
	set->hdr.pkt_size = pkt_size;
	set->hdr.src_port = afe_port;
	set->hdr.dest_port = copp->id;
	set->hdr.token = port_id << 16 | copp_idx;
	set->hdr.opcode = opcode;
	set->payload_size = param_size;
	/* mem_hdr stays zeroed: parameter data is in-band */

	vol.master_gain = volume >> Q6ADM_Q27_TO_Q13_SHIFT;
	vol.reserved = 0;

	if (opcode == ADM_CMD_SET_PP_PARAMS_V6) {
		struct q6adm_param_hdr_v3 *hdr = (void *)set->param_data;

		hdr->module_id = AUDPROC_MODULE_ID_VOL_CTRL;
		hdr->instance_id = ADM_INSTANCE_ID_0;
		hdr->param_id = AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN;
		hdr->param_size = sizeof(vol);
		memcpy(set->param_data + sizeof(*hdr), &vol, sizeof(vol));
	} else {
		struct q6adm_param_hdr_v1 *hdr = (void *)set->param_data;

		hdr->module_id = AUDPROC_MODULE_ID_VOL_CTRL;
		hdr->param_id = AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN;
		hdr->param_size = sizeof(vol);
		memcpy(set->param_data + sizeof(*hdr), &vol, sizeof(vol));
	}

	ret = q6adm_apr_send_copp_pkt(adm, copp, (struct apr_pkt *)set, 0);
	if (ret)
		dev_err(dev, "Failed to set copp volume, err %d\n", ret);

	kfree(set);
err_put_copp:
	kref_put(&copp->refcount, q6adm_free_copp);

	return ret;
}
EXPORT_SYMBOL_GPL(q6adm_set_volume);

/**
 * q6adm_matrix_map() - Map asm streams and afe ports using payload
 *
 * @dev: Pointer to adm child device.
 * @path: playback or capture path.
 * @payload_map: map between session id and afe ports.
 * @perf_mode: Performace mode.
 *
 * Return: Will be an negative on error or a zero on success.
 */
int q6adm_matrix_map(struct device *dev, int path,
		     struct route_payload payload_map, int perf_mode)
{
	struct q6adm *adm = dev_get_drvdata(dev->parent);
	struct q6adm_cmd_matrix_map_routings_v5 *route;
	struct q6adm_session_map_node_v5 *node;
	struct apr_pkt *pkt;
	uint16_t *copps_list;
	int pkt_size, ret, i, copp_idx;
	void *matrix_map;
	struct q6copp *copp;

	/* Assumes port_ids have already been validated during adm_open */
	pkt_size = (APR_HDR_SIZE + sizeof(*route) +  sizeof(*node) +
		    (sizeof(uint32_t) * payload_map.num_copps));

	matrix_map = kzalloc(pkt_size, GFP_KERNEL);
	if (!matrix_map)
		return -ENOMEM;

	pkt = matrix_map;
	route = matrix_map + APR_HDR_SIZE;
	node = matrix_map + APR_HDR_SIZE + sizeof(*route);
	copps_list = matrix_map + APR_HDR_SIZE + sizeof(*route) + sizeof(*node);

	pkt->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					   APR_HDR_LEN(APR_HDR_SIZE),
					   APR_PKT_VER);
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.token = 0;
	pkt->hdr.opcode = ADM_CMD_MATRIX_MAP_ROUTINGS_V5;
	route->num_sessions = 1;

	switch (path) {
	case ADM_PATH_PLAYBACK:
		route->matrix_id = ADM_MATRIX_ID_AUDIO_RX;
		break;
	case ADM_PATH_LIVE_REC:
		route->matrix_id = ADM_MATRIX_ID_AUDIO_TX;
		break;
	default:
		dev_err(dev, "Wrong path set[%d]\n", path);
		break;
	}

	node->session_id = payload_map.session_id;
	node->num_copps = payload_map.num_copps;

	for (i = 0; i < payload_map.num_copps; i++) {
		int port_idx = payload_map.port_id[i];

		if (port_idx < 0) {
			dev_err(dev, "Invalid port_id %d\n",
				payload_map.port_id[i]);
			kfree(pkt);
			return -EINVAL;
		}
		copp_idx = payload_map.copp_idx[i];

		copp = q6adm_find_copp(adm, port_idx, copp_idx);
		if (!copp) {
			kfree(pkt);
			return -EINVAL;
		}

		copps_list[i] = copp->id;
		kref_put(&copp->refcount, q6adm_free_copp);
	}

	mutex_lock(&adm->lock);
	adm->result.status = 0;
	adm->result.opcode = 0;

	ret = apr_send_pkt(adm->apr, pkt);
	if (ret < 0) {
		dev_err(dev, "routing for stream %d failed ret %d\n",
		       payload_map.session_id, ret);
		goto fail_cmd;
	}
	ret = wait_event_timeout(adm->matrix_map_wait,
				 adm->result.opcode == pkt->hdr.opcode,
				 msecs_to_jiffies(TIMEOUT_MS));
	if (!ret) {
		dev_err(dev, "routing for stream %d failed\n",
		       payload_map.session_id);
		ret = -ETIMEDOUT;
		goto fail_cmd;
	} else if (adm->result.status > 0) {
		dev_err(dev, "DSP returned error[%d]\n",
			adm->result.status);
		ret = -EINVAL;
		goto fail_cmd;
	}

fail_cmd:
	mutex_unlock(&adm->lock);
	kfree(pkt);
	return ret;
}
EXPORT_SYMBOL_GPL(q6adm_matrix_map);

/**
 * q6adm_close() - Close adm copp
 *
 * @dev: Pointer to adm child device.
 * @copp: pointer to previously opened copp
 *
 * Return: Will be an negative on error or a zero on success.
 */
int q6adm_close(struct device *dev, struct q6copp *copp)
{
	struct q6adm *adm = dev_get_drvdata(dev->parent);
	int ret = 0;

	ret = q6adm_device_close(adm, copp, copp->afe_port, copp->copp_idx);
	if (ret < 0) {
		dev_err(adm->dev, "Failed to close copp %d\n", ret);
		return ret;
	}

	kref_put(&copp->refcount, q6adm_free_copp);

	return 0;
}
EXPORT_SYMBOL_GPL(q6adm_close);

static int q6adm_probe(struct apr_device *adev)
{
	struct device *dev = &adev->dev;
	struct q6adm *adm;

	adm = devm_kzalloc(dev, sizeof(*adm), GFP_KERNEL);
	if (!adm)
		return -ENOMEM;

	adm->apr = adev;
	dev_set_drvdata(dev, adm);
	adm->dev = dev;
	q6core_get_svc_api_info(adev->svc_id, &adm->ainfo);
	mutex_init(&adm->lock);
	init_waitqueue_head(&adm->matrix_map_wait);

	INIT_LIST_HEAD(&adm->copps_list);
	spin_lock_init(&adm->copps_list_lock);

	return devm_of_platform_populate(dev);
}

#ifdef CONFIG_OF
static const struct of_device_id q6adm_device_id[]  = {
	{ .compatible = "qcom,q6adm" },
	{},
};
MODULE_DEVICE_TABLE(of, q6adm_device_id);
#endif

static struct apr_driver qcom_q6adm_driver = {
	.probe = q6adm_probe,
	.callback = q6adm_callback,
	.driver = {
		.name = "qcom-q6adm",
		.of_match_table = of_match_ptr(q6adm_device_id),
	},
};

module_apr_driver(qcom_q6adm_driver);
MODULE_DESCRIPTION("Q6 Audio Device Manager");
MODULE_LICENSE("GPL v2");
