# ADM (q6 DSP) COPP master-gain control — port notes

Target: mainline-based Linux 6.12.58 (`sound/soc/qcom/qdsp6/`), Xiaomi Pad 5 Pro
(`sm8250`, "elish").

Patch: `0001-q6adm-add-copp-master-gain-volume.patch` (unified diff, applies with
`git apply` or `patch -p1` from the kernel tree root).

This adds a way for userspace (PipeWire/UCM/`amixer`) to program the ADSP
"Volume Control" post-processing module of an **ADM COPP** — i.e. the DSP-side
playback master gain — instead of only the (nonexistent in mainline) ASM stream
volume.

---

## 1. Files changed

| file | change |
|------|--------|
| `sound/soc/qcom/qdsp6/q6adm.c` | + PP-param packing/sending, `q6adm_set_volume()`, basic-rsp callback case |
| `sound/soc/qcom/qdsp6/q6adm.h` | + `Q6ADM_VOLUME_MAX_Q27`, + `q6adm_set_volume()` prototype |
| `sound/soc/qcom/qdsp6/q6routing.c` | + `MultiMedia1 Playback Volume` ALSA kcontrol, session gain cache |

Full modified files are also in `../patched/` for dropping straight into a tree.

**Companion deliverable:** `../elish_adsp_vol/` is an out-of-tree kernel module
that provides the same control *without recompiling the kernel*, by sending the
same APR packet via the exported `apr_send_pkt()`.  It is the stop-gap; this
patch is the proper solution.  See `../elish_adsp_vol/NOTES.md`.

---

## 2. Exact protocol used (all numbers verified against downstream source)

### 2.1 APR opcodes

| symbol | value | source |
|--------|-------|--------|
| `ADM_CMD_SET_PP_PARAMS_V5` | `0x00010328` | `downstream/q6adm.c`, header `apr_audio-v2.h:614` |
| `ADM_CMD_SET_PP_PARAMS_V6` | `0x0001035D` | `apr_audio-v2.h:615` |
| `APR_BASIC_RSP_RESULT` | `0x000110E8` | mainline `include/linux/soc/qcom/apr.h` |

`ADM_CMD_SET_PP_PARAMS_V6` is chosen when the ADM service reports API version
`>= 3` (or when the version is unknown/zero); otherwise the V5 form is used.
Rationale: downstream selects V6 whenever userspace
(`msm_routing_instance_id_support_put()` in `msm-pcm-routing-v2.c:31495`, driven
by the Android HAL) says instance IDs are supported, and the sm8250 ADSP
reports ADM API version 3 (`ADSP_ADM_API_VERSION_V3 == 3`,
`apr_audio-v2.h`/`q6adm-v2.h:56`).  `adm->ainfo.api_version` is already filled
in by `q6core_get_svc_api_info()` in `q6adm_probe()`, so no new plumbing is
needed.

### 2.2 Command payload layout

`struct adm_cmd_set_pp_params` (downstream `apr_audio-v2.h:623`):

```
struct adm_cmd_set_pp_params {
    struct apr_hdr apr_hdr;        /* mainline: 20 bytes, __packed */
    struct mem_mapping_hdr mem_hdr;/* 12 bytes: addr_lsw, addr_msw, mem_map_handle */
    u32 payload_size;              /* size of the packed parameter blob */
    u8  param_data[0];             /* param header + param payload (in-band) */
} __packed;
```

The APR header is its **mainline** layout (`include/linux/soc/qcom/apr.h`),
which is 20 bytes (`hdr_field, pkt_size, src_svc, src_domain, src_port,
dest_svc, dest_domain, dest_port, token, opcode`).  `apr_send_pkt()` fills
`src_svc/src_domain/dest_svc/dest_domain` from the ADM `apr_device`; the driver
only sets:

* `src_port`  = real AFE port id, `q6afe_get_port_id(port_id)` (e.g. `0x1000`)
* `dest_port` = **DSP copp id** (`copp->id`, from the `ADM_CMDRSP_DEVICE_OPEN_V5`
  response), *not* the copp index
* `token`     = `port_id << 16 | copp_idx`
* `opcode`    = `ADM_CMD_SET_PP_PARAMS_V5/V6`
* `pkt_size`  = total packet size
* `payload_size` = packed parameter blob size
* `mem_hdr` zeroed (in-band; downstream `adm_set_pp_params()`,
  `downstream/q6adm.c:1014`)

### 2.3 Parameter blob (what `q6common_pack_pp_params()` builds)

Downstream `q6common_pack_pp_params()` (`dsp/q6common.c:54`) emits
`param_hdr_v3 + payload` when instance IDs are supported, else
`param_hdr_v1 + payload`.  Both headers are `__packed` and little-endian.

V6 / instance-ID form (`param_hdr_v3`, `apr_audio-v2.h:92`):

```
offset size field
0x00   4    module_id   = AUDPROC_MODULE_ID_VOL_CTRL (0x00010BFE)
0x04   2    instance_id = INSTANCE_ID_0 (0x0000)
0x06   2    reserved    = 0
0x08   4    param_id    = AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN (0x00010BFF)
0x0C   4    param_size  = 4
0x10   2    master_gain (Q13;  0x2000 == 1.0 == 0 dB)
0x12   2    reserved    = 0
```

V5 form (`param_hdr_v1`, `apr_audio-v2.h:67`): `module_id, param_id,
param_size(u16), reserved(u16)` = 12 bytes, then the same 4-byte payload.

Module/param IDs (`apr_audio-v2.h:9822,9833`):

```
ASM_MODULE_ID_VOL_CTRL                       0x00010BFE
AUDPROC_MODULE_ID_VOL_CTRL = ASM_MODULE_ID_VOL_CTRL
ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN            0x00010BFF
AUDPROC_PARAM_ID_VOL_CTRL_MASTER_GAIN = ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN
INSTANCE_ID_0                                0x0000
```

Resulting V6 packet size: `20 (apr_hdr) + 12 (mem_hdr) + 4 (payload_size) +
16 (param_hdr_v3) + 4 (gain) = 56` bytes.  V5 is 52 bytes.  Both are in-band,
little-endian, no out-of-band memory mapping.

Sending/response: the existing mainline `q6adm_apr_send_copp_pkt()` is reused.
Its fourth argument is **`rsp_opcode`** (an *additional* response opcode to wait
for; it is used by `DEVICE_OPEN`, whose response opcode differs from the command
opcode) — it is **not** a packet size, contrary to an earlier description of the
task.  Passing `0` therefore does **not** mean fire-and-forget: the helper waits
for `copp->result.opcode == opcode` and returns `-ETIMEDOUT`/`-EINVAL` on
timeout or DSP error.  For that to work, a matching case had to be added to the
`APR_BASIC_RSP_RESULT` switch in `q6adm_callback()`
(`ADM_CMD_SET_PP_PARAMS_V5`/`_V6`), which stores `copp->result = *result` and
wakes the waiter.  So `q6adm_set_volume()` is fully synchronous and
error-checked.  (The external module has no such luxury: APR responses are
delivered to the bound `q6adm` driver, so it can only log sends.)

---

## 3. Q27 vs Q13 — read this before debugging "it is silent"

**The userspace control is Q27** (`0x08000000 == 1.0 == 0 dB`), as requested.
**The DSP wire format is Q13** (`0x2000 == 1.0 == 0 dB`); `q6adm_set_volume()`
does `master_gain = volume >> 14`.

The task statement described the downstream struct as `{ u32 master_gain; }` in
Q27.  That is **not** what the actual downstream source says.  Evidence:

* `techpack/audio/include/dsp/apr_audio-v2.h:891`
  ```c
  struct audproc_volume_ctrl_master_gain {
          uint16_t master_gain;   /* Linear gain in Q13 format. */
          uint16_t reserved;      /* Clients must set this field to zero. */
  } __packed;
  ```
  (identical struct `asm_volume_ctrl_master_gain` at line 9892; both branches
  `lineage-20/21/22.1` are identical)
* Downstream `adm_set_volume()` (`downstream/q6adm.c:4574-4592`) writes the
  value straight into that `u16` field.
* The downstream userspace-facing limit for the same gain is **0x2000**:
  `msm_pcm_volume_info()` (`downstream/msm-pcm-q6-v2.c:1668`,
  `value.integer.max = 0x2000`) and the routing `App Type Gain` kcontrol max
  (`downstream/msm-pcm-routing-v2.c:23223`, `0x2000`).  If the firmware wanted
  Q27, 0x2000 would be ≈ −84 dB and every sm8250 phone would be silent.
* Mainline itself agrees, in two independent places:
  * `sound/soc/qcom/qdsp6/audioreach.h:637-643` (v6.12):
    ```c
    #define PARAM_ID_VOL_CTRL_MASTER_GAIN 0x08001035
    #define VOL_CTRL_DEFAULT_GAIN         0x2000
    struct param_id_vol_ctrl_master_gain { uint16_t master_gain; uint16_t reserved; } __packed;
    ```
    Mainline also has `audioreach_gain_set_vol_ctrl()` (`EXPORT_SYMBOL_GPL`),
    but that is the AudioReach/GPR stack (`q6apm`/`audioreach`), not the legacy
    APR/ADM stack this device uses — hence the q6adm work here.  (Cross-checked
    independently by the requester.)
  * `msm_pcm_volume_info()` downstream puts the userspace max at `0x2000`, and
    the same value is what `q6asm_set_volume()` writes into its `u16` gain
    field — if the firmware wanted Q27 the phones would be silent.

So `0x08000000` from userspace becomes `0x2000` on the wire.  If you ever want
the raw (unconverted) value, that is a one-line change in `q6adm_set_volume()`
(drop the `>> Q6ADM_Q27_TO_Q13_SHIFT`), but expect near-silence on this
firmware.  The external module variant (`../elish_adsp_vol/`) exposes both a
Q27 and a raw-Q13 attribute so this can be confirmed on-device without
recompiling.

---

## 4. The ALSA control

`q6routing.c` gains a non-DAPM ASoC control registered via the component
driver:

```
MultiMedia1 Playback Volume   (0 .. 0x08000000, default 0x08000000)
```

* `.put` clamps to `0..Q6ADM_VOLUME_MAX_Q27`, then calls
  `q6adm_set_volume(routing_data->dev, session->port_id, copp_idx, volume)` for
  every COPP currently in `session->copp_map` (a session can have several).
* The value is cached in `struct session_data.volume`; if it is set **before**
  the PCM stream is opened (no COPP yet) it is applied in
  `q6routing_stream_open()` right after `q6adm_matrix_map()`.
* The default `0 dB` is *not* pushed explicitly at stream open, so it does not
  override ACDB calibration; only a user-set value is re-applied.
* Choice of file: `q6routing.c` is the natural home because it already owns the
  session→port→COPP mapping (`session->copp_map`, `session->port_id`) that the
  control needs.  Doing it in `q6adm.c` would have required exporting/duplicating
  that mapping.  `q6adm.c` only exports the reusable primitive.

Usage:

```sh
amixer -c 0 controls | grep 'MultiMedia1 Playback Volume'
amixer -c 0 cset name='MultiMedia1 Playback Volume' 134217728   # 0x08000000 = 0 dB
amixer -c 0 cset name='MultiMedia1 Playback Volume' 67108864    # 0x04000000 = -6 dB
amixer -c 0 cset name='MultiMedia1 Playback Volume' 0           # mute
amixer -c 0 cget name='MultiMedia1 Playback Volume'
```

The control must be set **while a MultiMedia1 playback stream is open and
routed** for the change to reach an already-running COPP; otherwise it is cached
and applied on the next stream open.

---

## 5. How to apply and build

```sh
# from a Linux 6.12 source tree root
git apply /path/to/0001-q6adm-add-copp-master-gain-volume.patch
# or
patch -p1 < /path/to/0001-q6adm-add-copp-master-gain-volume.patch
```

Then build the q6 audio drivers as usual (they are part of
`CONFIG_SND_SOC_QDSP6`, `sound/soc/qcom/qdsp6/`):

```sh
make olddefconfig
make -j"$(nproc)" sound/soc/qcom/qdsp6/
```

`git apply --check` and `patch -p1 --dry-run` both pass against pristine
v6.12.58 copies of the three files (verified, see below).

### Verification done here (no kernel build, per instructions)

```sh
# baseline from the unmodified src files, patch applied, result compared
git apply --check -v 0001-...patch          # PASS
patch -p1 --dry-run < 0001-...patch         # PASS
# after applying: patched files are byte-identical to ../patched/
```

No kernel was compiled and no device was touched.

---

## 6. Assumptions / open questions

1. **V6 vs V5.** The patch defaults to `ADM_CMD_SET_PP_PARAMS_V6`
   (`param_hdr_v3`) and falls back to V5 only if `adm->ainfo.api_version` is a
   known value `< 3`.  If the ADSP rejects the packet (`DSP returned error` in
   dmesg) or never answers (`ADM copp cmd timedout`), try forcing V5.  The
   external module exposes this as a module parameter.
2. **Q13 vs Q27** — see §3; the code deliberately converts Q27→Q13.
3. `q6adm_set_volume()` operates on an **already-open** COPP (it uses the
   session's `copp_map`).  It does not open/close COPPs, so it cannot disturb
   an active stream's routing.
4. The control is only added for **MultiMedia1**.  Extending to MultiMedia2..8
   is a matter of repeating the `SOC_SINGLE_EXT(...)` entry with `shift` = 1..7
   (session index) — the handler already takes the session index from
   `mc->shift`.
5. `port_id` is the **AFE port index** used throughout mainline q6adm/q6routing
   (e.g. `PRIMARY_MI2S_RX == 16`, `WSA_CODEC_DMA_RX_0 == 105`), not the raw AFE
   port id (`0x1000`, `0xB000`).  `q6adm_set_volume()` converts via
   `q6afe_get_port_id()` for `src_port`.
6. The ACDB calibration path (`/dev/msm_audio_cal`, libacdbloader, app_type
   69940 / acdb_id 15) is out of scope and untouched.
7. `copp->id` is a firmware handle only known after `ADM_CMDRSP_DEVICE_OPEN_V5`;
   it is only reachable from inside `q6adm.c`, which is why the primitive lives
   there.  This also matters for the external-module variant (see its NOTES).
