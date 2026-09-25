# ACDB_PORT_PLAN — push the speaker "level cal" into the ADSP (in-band ADM PP-param)

Scope: get the elish speaker loudness back on mainline/Armbian by pushing the
ADCB calibration that Android pushes.  Companion to
`elish_acdb_cal/{elish_acdb_cal.c,README.md}`.

All `file:line` are relative to `/home/axis/axis_rnd/` unless stated otherwise.

---

## 1. Strategy in one line

Use the **already-proven in-band `ADM_CMD_SET_PP_PARAMS_V5/V6` channel** on
`aprsvc:service:4:8`, targeting the open **ADM COPP** for the TDM speaker port,
with the **ADM_AUDVOL (cal_type 12)** block first (parent's priority), **after**
the COPP is active and **with retries** — no shared memory, no DMA, no ION, no
memory-map, no PSPD.

---

## 2. Feasibility verdict: is in-band AUDVOL possible?

**Mechanism: yes, and it is the *native* downstream path — not a workaround.**

* Downstream exposes a dedicated in-band calibration entry point:

  ```
  adm_send_calibration(port_id, copp_idx, path, perf_mode, cal_type, params, size)
      -> adm_set_pp_params(port_id, copp_idx, NULL, params, size)
  ```
  `downstream/q6adm.c:4891-4912`, the call at `:4907`, `EXPORT_SYMBOL` at `:4912`.

* `adm_set_pp_params()` with `mem_hdr == NULL` embeds the blob in the packet
  (`downstream/q6adm.c:1014-1026`) with a zeroed 12-byte `mem_mapping_hdr`
  (`downstream/apr_audio-v2.h:32-60`) and opcode
  `ADM_CMD_SET_PP_PARAMS_V6`/`V5` (`downstream/q6adm.c:1004-1007`).

* The packet docs explicitly allow a multi-parameter blob:
  `downstream/apr_audio-v2.h:638-645`.  ACDB cal data *is* such a blob (a
  sequence of `param_hdr + payload`), which is why `adm_send_calibration()` can
  hand it over untouched.

* The vendor ACDB loader uses exactly this: the
  `acdb_loader_adsp_set_audio_cal` / `acdb_loader_store_set_audio_cal` strings
  live in `work/android/acdb/libacdbloader.so`, and their runtime failures are
  the `-100` lines in `work/android/acdb/acdb_log.txt:135-205`.

**Why the evidence points at AUDVOL specifically**

* `libacdbloader.so` contains `acdb_loader_send_gain_dep_cal`,
  `ACDB_CMD_GET_AUDPROC_INSTANCE_GAIN_DEP_STEP_TABLE(_SIZE)` and
  `destroy_vol_index_list` — the volume/gain-dependent table is a real,
  separately-looked-up calibration.
* `struct audio_cal_info_audvol { acdb_id; path; app_type; vol_index; }`
  (UAPI `msm_audio_calibration.h:240-246`) confirms AUDVOL is keyed by **volume
  index**, and `AUDIO_SET_VOL_CAL cal type = <n>` in the log
  (`acdb_log.txt:596-621`) prints that index (12, 40, …), **not** the cal_type.
* The captured Android failures are for the same speaker key the module defaults
  to: topology `0x1000a100`, apptype `0x11134` = 69940, acdb_id 10011.

**Caveats (do not skip)**

* In-band is bounded by the APR/rpmsg transport (`apr_send_pkt()` →
  `rpmsg_trysend`, mainline `drivers/soc/qcom/apr.c:70`).  The per-COPP ADM
  tables are small, but the whole `Forte_Speaker_cal.acdb` is 766 696 B and can
  **never** go in-band.  See §4 for what a real block looks like.
* In the captured trace the gain-dep lookups returned `-19` (not found) and
  Android still played — so **AUDVOL may not be the missing piece**.  Trial order
  should be 12 → 11 → 9/10 (§5 step 4).
* `-100 "active device/stream not found"` is a mismatch of
  `(topology_id, app_type)` **or** a timing race.  The module can only fix the
  timing half; the topology is fixed when the COPP is opened
  (`adm_open: … topology 0x1000a100 …`, `acdb_log.txt:733`).

---

## 3. Timing model (why the stock HAL fails, and what the module does)

```
Android (stock)                              elish_acdb_cal.ko
────────────────────────────────────────     ──────────────────────────────────────
stream start
  adm_open(topology=0x1000a100, app_type)     (stream already open)
  ACDB loader -> adm_send_calibration()
     ├─ COPP not yet active?                  send:
     │    result=-100, "active device/         1. poll q6adm COPP list up to wait_ms
     │    stream not found"                   2. wait until a COPP matches
     │    -> audio_hw_primary:                 3. push blob once
     │       "retry previous failed            4. re-push retries times, retry_ms apart
     │        cal level set"                     (delayed workqueue; `stop` cancels)
```

Race windows to keep in mind:

1. **COPP not open yet** — fixed by `wait_ms` (default 3000 ms).
2. **COPP opened but DSP module not instantiated yet** — the retry loop covers
   this (`retries` = 5 × `retry_ms` = 200 ms).
3. **Wrong topology/app_type** — *not* fixable by this module; verify with
   `status` (`topology_id`, `app_type`) and the kernel log's `adm_open` line.

---

## 4. ACDB extraction plan (`work/acdb/parse_acdb.py`)

### 4.1 Format, as established from `Forte_Speaker_cal.acdb`

```
0x000000  "QCMSNDDB"              8-byte magic
0x000008  8 bytes reserved
0x000010  "AVDB" <u32 len>        sub-database header (len 0x000bb2c8 = 766 664)
0x000020  chunks, repeated:
              <8-byte tag> <u32 payload_len> <payload bytes>
          e.g. MODIFIED(0x12), SWPNAME, SWPVERS, OEMINFO, DEVCATIN,
               DPROPLUTT(0x…), AVOLLUT0(0x0002db44 = 187 204), AVOLCDFT,
               AVOLCDOT, "AFE LUT0", AFE CDFT, … , DATAPOOL (0x00020b6e = 133 998)
0x0305b4  "DATAPOOL" + 0x20b6e    pool of device/cal records; payload starts with a
                                  name table: u32 counts, u32 offsets (e.g.
                                  0x112a7, 0x12a58), then UTF-16 device names
                                  ("SPKR_PHONE_MIC_BROADSIDE", …)
file tail                         lookup/index tables (see 4.2 for the verified
                                  row formats and offsets)
```

The 8-byte-tag/4-byte-length framing matches the existing helper in
`work/acdb/parse_acdb.py` (`chunk()` reads `size` at tag+8, payload at tag+12),
which is why `tags` / `avol` already work.

File size is 0x0bb2e8 = 766 696 B.

### 4.2 Lookup tables (verified by direct inspection)

Three separate index tables exist, with three different strides:

1. **Cal-data offset table**, 32-byte rows, byte-packed (**NOT 4-byte aligned**):

   ```
   { u32 sample_rate; u32 index; u32 off1; u32 off2; u32 off3; u32 off4; u32 off5; u32 0 }
   ```

   * the tool `work/acdb/acdb_tables.py tail <file> 48000` finds **10 such runs**
     in `Forte_Speaker_cal.acdb` (one per category), each with `index` 0..10
     (11 rows per sample rate);
   * the last run is at `0x0babc2` (index 0) … `0x0bb282` (index 10), ending
     just before the `VS2ILUT0` chunk; the row before it is
     `sample_rate = 0x7d00` (32000);
   * the five offsets point into the data region
     (tail rows: `0x013085`, `0x013086`, `0x010fc0`…`0x010fd8`, `0x01308a`,
     `0x011363`);
   * **alignment gotcha:** the runs start at offsets ≡ 2 (mod 4), so any parser
     must use unaligned reads (Python `struct.unpack_from` is fine; C must not
     cast to `u32 *`).

   Example row (`0x0bb242`):
   `bb80 0000 0a00 0000 8530 0100 8630 0100 d80f 0100 8a30 0100 6313 0100 0000 0000`

2. **`acdb_id` key table**, ~12-byte stride, first hit of `0x271b` (10011) at
   `0x1734` (0x1734, 0x1740, 0x174c, … — stride 12).

3. **`app_type` key table**, ~20-byte stride, first hit of `0x11134` (69940) at
   `0x1f30` (0x1f30, 0x1f44, 0x1f58, … — stride 20).

**Working hypothesis for the lookup:** table 2/3 map
`(acdb_id, app_type, …)` → the `index` used by table 1, and table 1 maps
`(sample_rate, index)` → the five cal-data offsets (five = the five ADM/ASM
cal types, e.g. topology / audproc / audvol / audstrm / afe).  Confirm by
walking one known key (acdb_id 10011 + app_type 0x11134) end to end and checking
that the resulting offsets land on small, self-describing param blobs.

Verified decode of `AVOLLUT0` (`python3 parse_acdb.py avol …`): 9360 entries ×
5 × u32, keyed by `app_type ∈ {69936…69949}` × 16 volume steps, value ramp 12 or
36 per step, maxima 180…1116.  69940 = 0x11134 is the speaker app_type.  This is
the *source data* for the gain-dependent step table.

### 4.3 What is still missing

`parse_acdb.py` has no `(cal_type, acdb_id, app_type, sample_rate)` → cal-data
lookup.  That lookup is what `libacdbloader.so` implements in userspace.

### 4.4 Concrete next steps

1. **Join the three tables of §4.2.**  Use the read-only dumper
   `work/acdb/acdb_tables.py` (`chunks` / `tail` / `keytables` subcommands) to
   see them.  For key `(cal_type=12, acdb_id=10011, app_type=69940,
   sample_rate=48000)`: locate the `acdb_id` row (stride 12, first hit `0x1734`)
   and the `app_type` row (stride 20, first hit `0x1f30`), read the `index`
   field, then read the 32-byte row `(sample_rate=48000, index)` from the
   relevant run (`0x0babc2` + 32·index for the last one) and follow its five
   offsets.  Correlate each offset with the chunk it points into.
2. **Cross-check against `libacdbloader.so`** (`work/android/acdb/libacdbloader.so`,
   32-bit variant too).  It is pure userspace; disassemble only to read the
   struct offsets of the index entries (they will appear as
   `ldr rX, [rY, #imm]` with `imm` = 0, 4, 8, …).  Map those imm values to the
   stride found in step 1.  Strings of interest:
   `ACDB_CMD_GET_AUDPROC_INSTANCE_GAIN_DEP_STEP_TABLE(_SIZE)`,
   `acdb_loader_send_gain_dep_cal`, `destroy_vol_index_list`.
3. **Implement `parse_acdb.py cal <file> <cal_type> <acdb_id> <app_type>
   <sample_rate|vol_index> <out.bin>`**:
   walk the index, apply the key match, honour the
   `adm_find_cal_by_app_type()` fallback (`downstream/q6adm.c:2266-2349`),
   and write the raw cal-data bytes.
4. **Sanity-check the output before it ever reaches the DSP**: it must start with
   plausible PP-param header(s) — the module logs the first 32 bytes
   (`hexdump_bytes=32`) and the block sizes should be small (the audstrm table
   Android pushes is 20 B, `acdb_log.txt:625`).  A hundreds-of-KB extraction
   means the wrong node.
5. **Fallback if the lookup proves too costly:** extract `AVOLLUT0` directly and
   build the volume-table PP param by hand (module/param IDs from the ACDB
   `<->` AudioReach mapping), which is the same data the gain-dependent table
   carries.

---

## 5. Minimal experiment (each step independently falsifiable)

Target: 5 minutes on Armbian with a speaker stream playing.

```bash
# 0. prerequisites
#    - Armbian booted, SLPI/ADSP up (slpi-off.service disabled)
#    - a speaker playback stream OPEN (aplay / PipeWire), so a COPP exists
#    - build the module (§8a of README) or cross-build and scp the .ko

# 1. load (AUDVOL first, per priority)
insmod /root/elish_acdb_cal/elish_acdb_cal.ko \
       cal_type=12 cal_file=/lib/firmware/elish_cal.bin
cat /sys/kernel/elish_acdb_cal/status | grep -E "ADM dev|copp|cal_type|opcode|timing"

# 2. PROVE THE CHANNEL with the known-good master gain first (no ACDB needed).
#    This is the control: it must behave exactly like elish_adsp_vol.
echo 256 > /sys/kernel/elish_acdb_cal/gain_q13      # 0x0100  -> -24 dB, expect silence
echo 8192 > /sys/kernel/elish_acdb_cal/gain_q13     # 0x2000  -> 0 dB (ceiling)
dmesg | grep ADM_GAIN | tail -2
#    => if this does not change loudness, the COPP addressing is wrong; stop.

# 3. push the extracted level cal (small block! <= a few KB)
dmesg -C
echo 1 > /sys/kernel/elish_acdb_cal/send
dmesg | grep -E "CAL|retry"
dmesg | grep -E "Unknown Cmd:0x1035d|Unknown Cmd:0x10328|cmd = 0x103"
#    Accept/reject decision:
#      * no "return error" line          -> DSP accepted the packet
#      * "cmd = 0x1035d return error=0x2"-> ADSP_EBADPARAM: blob/param header wrong
#      * "cmd = 0x1035d return error=0x…"-> other DSP error: record the code
#      * "active device/stream not found"-> timing/topology: increase wait_ms,
#                                           check topology_id/app_type vs adm_open

# 4. trial order if step 3 shows no audible/measurable change
#    12 AUDVOL  ->  11 AUDPROC  ->  9 ADM_TOPOLOGY  ->  10 ADM_CUST_TOPOLOGY
#    (11 is the one Android actually sent successfully via send_audtable.)

# 5. V5 fallback (instance-ID / API mismatch)
rmmod elish_acdb_cal
insmod … use_v6=0
echo 1 > /sys/kernel/elish_acdb_cal/send

# 6. measure, don't listen: white noise -6 dBFS, SPL or the amp telemetry path
#    from ELISH_AMP_TDM_FIX.md §24; compare against the Android reference.

# 7. clean up
echo 1 > /sys/kernel/elish_acdb_cal/stop
rmmod elish_acdb_cal
```

Expected dmesg line (shape):

```
elish_acdb_cal: CAL opcode=0x1035d src_port=0x9020 dest_port=<copp id> token=0x… \
  cal_type=12(ADM_AUDVOL) acdb_id=10011 app_type=69940(0x11134) sample_rate=48000 \
  topology_id=0x1000a100 perf_mode=0 be_id=87 vol_index=-1 in_band=1 handle=0 size=<n>
elish_acdb_cal: CAL first 32 byte(s): <hex>
```

### What each outcome means

| observation | conclusion | next action |
|---|---|---|
| `gain_q13` changes loudness | APR in-band path + COPP addressing are correct | proceed to cal |
| `gain_q13` has no effect | wrong COPP / wrong port | fix `port_index`/`copp_id`; check `status` |
| cal accepted, no loudness change | block is wrong cal_type or already-default data | try cal_type 11, then AVOLLUT0-derived table |
| `return error = 0x2` (EBADPARAM) | param header/blob malformed | check `hexdump_bytes` output; try `wrap_hdr=1` |
| `active device/stream not found` | timing or topology mismatch | raise `wait_ms`; verify topology/app_type |

---

## 6. Fallback: out-of-band (only if in-band is proven impossible)

Kept for completeness; **do not switch silently**.

* Downstream's other cal path is
  `remap_cal_data()` (`downstream/q6adm.c:2054`) +
  `send_adm_cal_block()` (`:2172`) → `adm_set_pp_params(..., &mem_hdr, NULL, size)`
  with `mem_hdr = { paddr, q6map_handle }` (`:2215-2219`).
* It needs `ADM_CMD_SHARED_MEM_MAP_REGIONS` (`0x00010322`) and the DSP's
  `ADM_CMDRSP_SHARED_MEM_MAP_REGIONS` (`0x00010323`) reply.
* **Blocker:** mainline `apr_do_rx_callback()` delivers every response to the one
  driver bound to the destination apr_device
  (mainline `drivers/soc/qcom/apr.c:232-257`); that is `qcom-q6adm`, whose
  callback ignores `0x10323` (mainline `q6adm.c:203-206`).  A second
  `apr_driver` cannot bind the same device.  The handle must therefore come from
  outside (module param, or a q6adm patch).
* Use this only for blocks too large for the APR transport.

---

## 7. Open questions (ranked)

1. **Is the ADM_AUDVOL / gain-dependent table actually present for acdb_id
   10011?**  The trace's `-19` suggests maybe not.  Resolve by finishing §4.
2. **What is the exact cal-data framing** (one param vs several; module/param
   IDs; `param_hdr_v1` vs `v3`)?  Resolve from `hexdump_bytes` + the ACDB index.
3. **Which topology does the mainline COPP actually get?**  If the mainline
   `sm8250` machine driver opens the COPP with a different topology than
   `0x1000a100`, the DSP may reject the cal regardless of timing.  Check the
   kernel log for the `adm_open`/`q6adm` open line and compare with `status`.
4. **Does the DSP honour `vol_index` at all when the cal is pushed post-open?**
   Android pushes at open time.
5. **How large can an in-band packet be on this transport?**  Establish
   empirically with a padding experiment (send 512 / 1024 / 2048 / 4096 bytes of
   a harmless param and watch for `apr_send_pkt` errors).
6. **AFE/ASM halves.**  `AFE_COMMON_RX` (16) and the ASM topologies go to other
   services; if the missing loudness is AFE-side, this ADM module cannot fix it.
   The Android evidence that AFE cal fails yet audio works
   (`afe_get_cal_topology_id: cal_type 8 not initialized`,
   `AFE set topology id 0x0 … ret -22`, `send_afe_cal_type cal_block not found!!`)
   argues it is not required.

---

## 8. Evidence index

| topic | location |
|---|---|
| in-band cal entry point | `work/kernel/downstream/q6adm.c:4891-4912` |
| in-band packet build | `work/kernel/downstream/q6adm.c:963-1058` (`:1004-1007`, `:1014-1026`) |
| packer | `work/kernel/downstream/q6common.c:54` (`q6common_pack_pp_params`) |
| packet/struct docs | `work/kernel/downstream/apr_audio-v2.h:32-60`, `:614-647`, `:891-894` |
| out-of-band variant (fallback) | `work/kernel/downstream/q6adm.c:2054`, `:2172-2226` |
| PSPD (matrix mixer only) | `work/kernel/downstream/q6adm.c:501`, `:690`; `apr_audio-v2.h:673-715` |
| AUDVOL key struct | UAPI `msm_audio_calibration.h:240-246`, `:561-570` |
| Android speaker params | `work/acdb/../android/acdb/acdb_log.txt:619`, `:621`, `:733` |
| Android `-100` failures | `work/android/acdb/acdb_log.txt:135-205`, `:645-652` |
| `App Type Cfg` field order | `work/android/tm_android_working.txt:4496` |
| ACDB file + parser | `work/acdb/Forte_Speaker_cal.acdb`, `work/acdb/parse_acdb.py` |
| ACDB index-table dumper (new) | `work/acdb/acdb_tables.py` (`chunks` / `tail` / `keytables`) |
| working APR template | `work/kernel/elish_adsp_vol/elish_adsp_vol.c` |
| module | `work/kernel/elish_acdb_cal/` (this drop) |
