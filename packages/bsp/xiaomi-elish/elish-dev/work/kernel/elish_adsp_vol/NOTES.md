# elish_adsp_vol — external (no-kernel-rebuild) ADM COPP + ASM stream gain module

Out-of-tree module for the Xiaomi Pad 5 Pro (`sm8250`, "elish") on a
mainline-based kernel (Armbian `6.12.58-current-sm8250`).  It programs the ADSP
"Volume Control" post-processing module of an **ADM COPP** — the DSP-side
playback master gain — by sending the ADM `SET_PP_PARAMS` command directly over
APR, using only already-exported symbols.

It also programs the **ASM stream volume** (the per-stream gain stage that
mainline never sets) on the ASM service, which is where the real loudness
headroom is: the ADM/COPP master gain can only attenuate (0 dB == Q13 0x2000 is
its ceiling).  See §8.

This is the **stop-gap**; the proper solution is the in-kernel patches
`../patches/0001-q6adm-add-copp-master-gain-volume.patch` (ADM COPP gain +
`MultiMedia1 Playback Volume` kcontrol) and
`../patches/0002-q6asm-add-stream-volume.patch` (`q6asm_set_volume()`, and the
same kcontrol also drives the ASM stream gain).  Use the patches once you can
rebuild the kernel.

---

## 1. Files

| file | purpose |
|------|---------|
| `elish_adsp_vol.c` | module source |
| `Makefile` | standard Kbuild `obj-m` external-module makefile |
| `build_and_test.sh` | build + load + sweep test, to be run **on the device** |
| `NOTES.md` | this file |

---

## 2. How it works (and where the COPP handle comes from)

The ADM `SET_PP_PARAMS` command must be addressed to the **firmware COPP
handle** (`dest_port = copp->id`), which the DSP only reveals in the
`ADM_CMDRSP_DEVICE_OPEN_V5` response.  `q6adm_get_copp_id()` returns the COPP
*index*, not the handle, and `q6adm_find_copp()` is `static`, so neither gives
the handle to a module.

This module obtains it by reading the driver-private data:

1. Find the ADM `apr_device`:
   * default: `bus_find_device(&aprbus, NULL, "aprsvc:service:4:8", ...)`
     (`aprbus` is `EXPORT_SYMBOL_GPL`; name observed on the device:
     `aprsvc:service:4:8`, domain 4 = ADSP, service 8 = APR_SVC_ADM).
     Override with the `adm_dev_name` module parameter.
   * fallback: scan the apr bus for the device bound to the `qcom-q6adm`
     driver.
2. `adm = dev_get_drvdata(&adev->dev)` — the `struct q6adm *`.
3. Walk `adm->copps_list` under `adm->copps_list_lock`, copying
   `(afe_port, copp_idx, id)` out, then send one packet per COPP.
   `struct q6copp` / `struct q6adm` are copied **field-for-field** from mainline
   `sound/soc/qcom/qdsp6/q6adm.c` (lines 37-54 and 56-66).  Because the module
   is compiled on the device against `/lib/modules/$(uname -r)/build`, every
   kernel type involved (`spinlock_t`, `wait_head_queue_t`, `struct mutex`, …)
   has exactly the same size/layout as the running kernel, so the copies
   reproduce the driver's layout exactly.  No runtime offset probing is needed.

The send itself:

* `apr_send_pkt(adev, pkt)` (EXPORT_SYMBOL_GPL) fills `src_svc`/`src_domain`/
  `dest_svc`/`dest_domain` from the ADM `apr_device`.
* `src_port = q6afe_get_port_id(port_index)` (EXPORT_SYMBOL_GPL), `dest_port =
  copp->id`, `token = port_index << 16 | copp_idx`.
* packet layout / IDs are identical to the in-kernel patch — see
  `../patches/NOTES.md` §2 and the big comment at the top of `elish_adsp_vol.c`.

### AFE port index

`port_index` is the **AFE port enum index** (e.g. `TERTIARY_TDM_RX_0 == 56`),
not the raw port id (`AFE_PORT_ID_TERTIARY_TDM_RX == 0x9020`).  The DT index
cells are not reliable, so the module does **not** hardcode the index: at load
it walks `q6afe_get_port_id(i)` for `i = 0 .. AFE_PORT_MAX-1` and picks the `i`
whose return value is `0x9020` (`AFE_PORT_ID_TDM_PORT_RANGE_START 0x9000 +
0x20`, mainline `q6afe.c:128`/`207`), i.e. the "Tertiary TDM Playback" speaker
link.  If that fails it falls back to `56`.  Override with `port_index=<n>`
(use `port_index=-2` for all ports).

---

## 3. Build / load / set gain on the device

```sh
# on the device, from this directory
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo insmod elish_adsp_vol.ko

dmesg | tail            # confirms the ADM device, detected port index, etc.
ls /sys/kernel/elish_adsp_vol/
```

Keep a playback stream open (that is what creates the COPP) and set the gain:

```sh
# Q27 interface (0x08000000 == 1.0 == 0 dB)
echo 0x08000000 > /sys/kernel/elish_adsp_vol/gain   # 0 dB
echo 0x04000000 > /sys/kernel/elish_adsp_vol/gain   # -6 dB
echo 0          > /sys/kernel/elish_adsp_vol/gain   # mute
cat /sys/kernel/elish_adsp_vol/gain                 # last Q27 value
cat /sys/kernel/elish_adsp_vol/copp                 # last port/copp/dest_port/q13

# raw firmware Q13 interface (0x2000 == 1.0 == 0 dB) - bypasses the >>14
echo 8192  > /sys/kernel/elish_adsp_vol/gain_q13
echo 4096  > /sys/kernel/elish_adsp_vol/gain_q13

# ASM *stream* volume (same Q27/Q13 interfaces, applied to the live ASM
# session) - this is where the extra loudness comes from (see §8)
cat /sys/kernel/elish_adsp_vol/asm                 # ASM state / live sessions
echo 0x08000000 > /sys/kernel/elish_adsp_vol/gain_asm       # 0 dB (ASM)
echo 0x10000000 > /sys/kernel/elish_adsp_vol/gain_asm       # +6 dB (ASM)
echo 8192  > /sys/kernel/elish_adsp_vol/gain_asm_q13        # raw Q13 0 dB
```

Every send is logged:

```
elish_adsp_vol: ADM port_idx=56 copp_idx=0 dest_port=<handle> src_port=0x9020 opcode=0x1035d q13=0x2000 (raw=0)
elish_adsp_vol: ASM session=1 stream_id=1 src_port=0x0101 dest_port=0x0101 opcode=0x1320d q13=0x2000
```

`apr_send_pkt()` only guarantees delivery to the APR transport — the APR
response is delivered to the bound in-kernel `q6adm` driver, so this module
cannot read the DSP status.  Watch `dmesg` for the q6adm driver's
`DSP returned error[...]` immediately after a write if you suspect a rejection.

### Verify / roll back

```sh
cat /sys/kernel/elish_adsp_vol/copp     # dest_port + Q13 actually sent
dmesg | grep -E 'elish_adsp_vol|q6adm'  # sends + any DSP errors
echo 0x08000000 > /sys/kernel/elish_adsp_vol/gain   # restore 0 dB
sudo rmmod elish_adsp_vol               # full rollback (no kernel change)
```

Nothing persistent is modified: removing the module (or rebooting) returns the
system to its previous state.  The DSP gain itself is not persisted, but it is
re-programmed by the audio stack on the next stream start.

### One-shot automated run

```sh
./build_and_test.sh              # builds, loads, sweeps gain, unloads
PCM=hw:0,0 WAV=/path/to/test.wav ./build_and_test.sh
```

---

## 4. Module parameters

| param | default | meaning |
|-------|---------|---------|
| `adm_dev_name` | `aprsvc:service:4:8` | APR device name of the ADM service |
| `port_index` | `-1` (auto-detect 0x9020) | AFE port enum index; `-2` = all ports |
| `copp_idx` | `-1` | COPP index filter; `-1` = all |
| `copp_id` | `-1` | explicit firmware COPP handle for `dest_port` |
| `rate` / `channels` / `bit_width` | `48000` / `2` / `24` | only for `use_q6adm_open=1` |
| `use_q6adm_open` | `0` | use `q6adm_open()`+`->id` instead of walking the COPP list |
| `close_on_exit` | `0` | call `q6adm_close()` on that COPP at unload (can stop an active stream!) |
| `use_v5` | `0` | send `SET_PP_PARAMS_V5` + `param_hdr_v1` instead of V6 + `param_hdr_v3` |
| `asm_dev_name` | `aprsvc:service:4:7` | APR device name of the ASM service (service 7 = `APR_SVC_ASM`) |
| `asm_stream_id` | `1` | stream id in the ASM `src_port`/`dest_port` (`(session << 8) \| stream_id`) |
| `use_asm_v2` | `0` | send `ASM_STREAM_CMD_SET_PP_PARAMS_V2` + `param_hdr_v1` instead of V3 + `param_hdr_v3` |
| `asm_q13` | `-1` | raw Q13 ASM stream gain to apply at load (`-1` = leave ASM untouched) |

### New sysfs attributes

| attribute | rw | meaning |
|-----------|----|---------|
| `gain_asm` | rw | ASM stream gain, Q27 (`0x08000000` == 0 dB); converted with `>> 14` |
| `gain_asm_q13` | rw | ASM stream gain, raw Q13 (`0x2000` == 0 dB) |
| `asm` | ro | last ASM session / stream id / ports / opcode / Q13 + live session count |

The existing `gain`, `gain_q13` and `copp` attributes (ADM COPP) are unchanged;
`copp` additionally prints the resolved `asm_dev` name.

### Why `use_q6adm_open` defaults to off

`q6adm_open()` is exported and, thanks to its internal
`q6adm_find_matching_copp()`, returns the **existing** COPP when the playback
parameters (port, topology, perf_mode, rate, bit_width, app_type) match; with
mismatched parameters it **creates a new COPP** instead.  Worse,
`q6adm_close()` unconditionally sends `ADM_CMD_DEVICE_CLOSE_V5`, so closing a
COPP that is still shared with an active stream stops that stream.  Directly
walking `adm->copps_list` avoids both problems: it neither creates nor closes
anything, and it does not need to guess rate/bit_width.  `use_q6adm_open=1` is
kept as a fallback if the private-struct layout ever mismatches.

---

## 5. Q27 vs Q13 (again)

The sysfs `gain` attribute is Q27 (`0x08000000 == 0 dB`); the firmware wants
linear **Q13** (`0x2000 == 0 dB`), so the module sends `q27 >> 14`.  The
`gain_q13` attribute lets you bypass the conversion and confirm this on-device.
Full evidence is in `../patches/NOTES.md` §3:

* `apr_audio-v2.h:891` `struct audproc_volume_ctrl_master_gain { uint16_t
  master_gain; uint16_t reserved; }` — "Linear gain in Q13 format"
* downstream `msm_pcm_volume_info()`/`App Type Gain` limit = `0x2000`
* mainline `sound/soc/qcom/qdsp6/audioreach.h:637-643`
  `VOL_CTRL_DEFAULT_GAIN 0x2000` + `uint16_t master_gain; uint16_t reserved;`

---

## 6. Relationship to the in-kernel patch

The in-kernel `q6adm_set_volume()` sends the same packet and **does** wait for
the DSP reply: `q6adm_apr_send_copp_pkt(adm, copp, pkt, 0)` waits for
`copp->result.opcode == opcode`.  In mainline, `rsp_opcode` is an *additional*
response opcode to wait for (it is used by DEVICE_OPEN, whose response opcode
differs), so passing `0` does **not** mean fire-and-forget: the patch adds
`ADM_CMD_SET_PP_PARAMS_V5/V6` cases to the `APR_BASIC_RSP_RESULT` switch in
`q6adm_callback()`, which store `copp->result` and wake the waiter, so
`q6adm_set_volume()` returns `-EINVAL`/`-ETIMEDOUT` on DSP error/timeout.
The module has no such luxury (responses go to the bound driver), hence the
dmesg-only verification.

---

## 7. Open questions / uncertainties

1. **V6 vs V5.** Defaults to V6 (`param_hdr_v3`), correct for sm8250 (ADM API
   v3).  If nothing happens, try `use_v5=1` and watch dmesg for q6adm errors.
2. **Port detection.** Auto-detection looks for port id `0x9020`.  If the
   actual speaker COPP runs on a different port, pass `port_index=<n>` (or `-2`
   to hit every open COPP).
3. **Private struct copies.** They must match the running kernel.  Rebuild the
   module on the device after every kernel update; if `struct q6copp`/`struct
   q6adm` change in `q6adm.c`, update the copies (they are marked
   "keep in sync").
4. **No DSP status feedback.** `apr_send_pkt()` success only means "queued to
   rpmsg".  Use dmesg from the q6adm driver for errors.
5. **Q13 vs Q27** — see §5; on-device confirmation via `gain_q13`.
6. **Lockdep/layout.** Safe because the module is built against the live
   kernel's headers; do **not** build it against a different kernel's headers
   and load it elsewhere.

---

## 8. ASM stream volume — the missing gain stage

The audio chain is:

```
ASM (stream) --> ADM / COPP --> AFE (Tertiary TDM) --> CS35L41 amps
```

The ADM/COPP master gain programmed in §2 is a **post-processing** gain inside
the COPP *and it can only attenuate*: its maximum, `0x2000` (Q13), is exactly
0 dB.  The stage that can actually *add* loudness is the **ASM stream volume**
(the "soft volume" module inside the ASM session), which mainline never
programs at all.  This module now programs that too, over the ASM service.

### 8.1 Packet

The ASM `SET_PP_PARAMS` command has the **same shape** as the ADM one already
sent in §2 (20-byte `apr_hdr` + 12-byte zeroed `mem_mapping_hdr` (in-band) +
`u32 payload_size` + packed parameter blob), and the parameter blob is
byte-for-byte identical (same module/param IDs, same 4-byte Q13 payload).
Only the opcode, the APR service and the port addressing differ.

| item | value | evidence |
|------|-------|----------|
| V3 opcode | `ASM_STREAM_CMD_SET_PP_PARAMS_V3` = `0x0001320D` | `downstream/apr_audio-v2.h:8417` |
| V2 opcode | `ASM_STREAM_CMD_SET_PP_PARAMS_V2` = `0x00010DA1` | `downstream/apr_audio-v2.h:8416` |
| module id | `ASM_MODULE_ID_VOL_CTRL` = `0x00010BFE` | `downstream/apr_audio-v2.h:9820` |
| instance id | `INSTANCE_ID_0` = `0x0000` | `downstream/apr_audio-v2.h` |
| param id | `ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN` = `0x00010BFF` | `downstream/apr_audio-v2.h:9832` |
| param size | `4` (`u16 master_gain` + `u16 reserved`) | `struct asm_volume_ctrl_master_gain`, `downstream/apr_audio-v2.h:9892` |
| APR service | `APR_SVC_ASM` = `7` (device `aprsvc:service:4:7`) | `include/dt-bindings/soc/qcom,apr.h:18`; observed in `/sys/bus/aprbus/devices/` |
| domains | `src_domain` = `APR_DOMAIN_APPS` = `5`, `dest_domain` = `APR_DOMAIN_ADSP` = `4` | `apr_send_pkt()`, `drivers/soc/qcom/apr.c:65-68` |
| ports | `src_port == dest_port == ((session << 8) & 0xFF00) \| stream_id` | mainline `q6asm.c:285-286` (`q6asm_add_hdr`) |
| token | `session` | mainline `q6asm.c:288` |

`SOFT_VOLUME_INSTANCE_1` maps to exactly this module/instance:
`q6asm_set_soft_volume_module_instance_ids()` sets
`ASM_MODULE_ID_VOL_CTRL` + `INSTANCE_ID_0` for `SOFT_VOLUME_INSTANCE_1`
(`downstream/q6asm.c:3058`), and `q6asm_set_volume()` calls
`__q6asm_set_volume(ac, volume, SOFT_VOLUME_INSTANCE_1)`
(`downstream/q6asm.c:8879`).  The downstream send path is
`__q6asm_set_volume()` (`downstream/q6asm.c:8850`) ->
`q6asm_pack_and_set_pp_param_in_band()` (`downstream/q6asm.c:3009`) ->
`q6asm_set_pp_params()` (`downstream/q6asm.c:2908`), the last of which sends
`ASM_STREAM_CMD_SET_PP_PARAMS_V3` when instance IDs are supported
(`downstream/q6asm.c:2952`).

### 8.2 How the module addresses the running session

There is no exported way to iterate ASM sessions, so the module:

1. finds the ASM `apr_device` `aprsvc:service:4:7` (fallback: the device bound
   to the `qcom-q6asm` driver), exactly like it finds the ADM one;
2. reads `dev_get_drvdata()` — the driver-private `struct q6asm` (copied
   field-for-field from mainline `q6asm.c:252-260`) — and walks its
   `session[]` table under `q6asm->slock`;
3. calls the exported `q6asm_get_session_id()` (declared locally because it is
   **not** in the installed headers; confirmed `EXPORT_SYMBOL_GPL` in the
   running kernel's `Module.symvars`) to get each live session id;
4. sends one ASM volume packet per live session with `src_port` /
   `dest_port` = `(session << 8) | asm_stream_id` (default `1`, which is what
   mainline `q6asm-dai.c` uses for playback).  The response is delivered to the
   bound `q6asm` driver, so — as with the ADM path — only dmesg proves a send.

The session table walk holds `q6asm->slock` around the slot read *and* the
`q6asm_get_session_id()` call; `q6asm_audio_client_release()` clears the slot
under the same lock before `kfree()`, so the pointer cannot go away mid-read.

### 8.3 Testing from the shell

The ASM volume only exists while a playback stream is open (that is what
creates the session), so keep audio playing and then:

```sh
insmod elish_adsp_vol.ko                       # default asm_q13=-1: no ASM write
dmesg | tail                                   # note the ASM device + live sessions
cat  /sys/kernel/elish_adsp_vol/asm            # live_sessions should be >= 1

# Q27: 0x08000000 == 0 dB; this is the extra loudness stage
echo 0x08000000 > /sys/kernel/elish_adsp_vol/gain_asm
echo 0x10000000 > /sys/kernel/elish_adsp_vol/gain_asm   # +6 dB
echo 0x04000000 > /sys/kernel/elish_adsp_vol/gain_asm   # -6 dB
echo 0          > /sys/kernel/elish_adsp_vol/gain_asm   # mute

# raw firmware Q13 (bypasses the >>14)
echo 8192       > /sys/kernel/elish_adsp_vol/gain_asm_q13

cat /sys/kernel/elish_adsp_vol/asm            # last session/ports/opcode/q13
dmesg | grep elish_adsp_vol                   # one line per send
```

Expected dmesg line:

```
elish_adsp_vol: ASM session=1 stream_id=1 src_port=0x0101 dest_port=0x0101 opcode=0x1320d q13=0x2000
```

If nothing happens, try `use_asm_v2=1` (V2 opcode `0x10da1` + `param_hdr_v1`),
or `asm_stream_id=2`, and watch dmesg for errors from the `q6asm` driver.
`insmod elish_adsp_vol.ko asm_q13=8192` applies the ASM gain once at load
(only useful once a stream is already running).

To compare against the in-kernel path, `../patches/0002-q6asm-add-stream-volume.patch`
adds `q6asm_set_volume()` and wires the existing `MultiMedia1 Playback Volume`
kcontrol so that a single ALSA/PipeWire volume write updates the ADM COPP gain
*and* the ASM stream gain with the same value (`volume >> 14`).

Because the in-kernel kcontrol keeps a `struct audio_client *` in
`session_data`, the patch also closes the resulting lifetime race:
`q6asm_dai_close()` / `q6asm_dai_compr_free()` now call
`q6routing_stream_close()` (which clears the pointer under the routing lock)
*before* `q6asm_audio_client_free()`, and
`msm_routing_put_copp_volume()` holds that same lock while it calls
`q6asm_set_volume()`.  A concurrent volume write therefore either sees a live
client or no client at all.  (The out-of-tree module does not have this
problem: it re-derives the session under `q6asm->slock` on every write and
never stores the pointer.)

### 8.4 Open questions / uncertainties

1. **No DSP response visible.** Like the ADM path, `apr_send_pkt()` succeeding
   only means "queued to rpmsg"; the response goes to the bound `q6asm` driver.
   If the ASM gain has no audible effect, check dmesg for a `q6asm` error and
   try V2.
2. **Stream id.** Defaults to `1` (mainline playback).  A gapless second track
   uses stream id `2`; set `asm_stream_id=2` if needed.
3. **`session[]` walk.** The `struct q6asm` copy must match the running kernel
   (same "keep in sync" rule as `struct q6adm`).  The ASM device is optional:
   if it is not found the module still loads and the ADM path works, with
   `gain_asm*` returning `-ENODEV`.
4. **Multiple sessions.** Every live ASM session is programmed.  Capture
   sessions may not contain a volume-control module and can return an error
   that the module cannot see.
5. **Whether the elish topology actually includes the ASM soft-volume module.**
   If the DSP replies with an "unsupported module" error for
   `0x00010BFE`/`0x00010BFF`, the stream volume cannot be used on this firmware
   build and the ACDB/COPP gain remains the only (attenuating) option.

