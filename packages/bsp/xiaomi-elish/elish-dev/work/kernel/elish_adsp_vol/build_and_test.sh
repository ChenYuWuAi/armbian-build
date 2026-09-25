#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# build_and_test.sh - build, load and exercise the elish_adsp_vol out-of-tree
#                     module for the Xiaomi Pad 5 Pro (sm8250 / "elish").
#
# THIS SCRIPT IS MEANT TO BE RUN **ON THE DEVICE** (Armbian
# 6.12.58-current-sm8250), not on the build host.  It never touches a remote
# machine by itself.
#
# Copy the whole ../elish_adsp_vol directory to the device, e.g.:
#   scp -r elish_adsp_vol user@elish:~/
#   ssh user@elish 'cd elish_adsp_vol && ./build_and_test.sh'
#
# What it does:
#   0. sanity-checks the kernel build dir / Module.symvers
#   1. builds elish_adsp_vol.ko
#   2. loads it, prints dmesg
#   3. verifies the sysfs interface exists
#   4. starts a playback stream (so a COPP exists) and sweeps the ADSP COPP
#      gain via sysfs, printing the resulting dest_port / Q13 values
#   5. shows how to test the raw Q13 (firmware) path and the V5 fallback
#   6. unloads
#
# Notes:
#   * must run as root (or with sudo) for insmod/rmmod and for aplay when the
#     user is not in the audio group
#   * a playback stream MUST be open while you sweep the gain, otherwise the
#     module reports "no COPP matches ... (is a stream open?)" and nothing is
#     sent
#   * "0x08000000" (Q27 1.0 / 0 dB) is the maximum; the module converts it to
#     firmware Q13 0x2000.  Values 0..0x08000000 map linearly to 0..0x2000.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MOD=elish_adsp_vol
KREL="$(uname -r)"
KDIR="/lib/modules/${KREL}/build"
SYSFS="/sys/kernel/elish_adsp_vol"
LOG=/tmp/${MOD}_test.log

say()  { printf '\n=== %s ===\n' "$*"; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

exec > >(tee -a "$LOG") 2>&1
echo "logging to $LOG"

# --------------------------------------------------------------------------
say "0. environment"
# --------------------------------------------------------------------------
uname -a
[ -d "$KDIR" ] || die "$KDIR missing - install the matching kernel headers"
[ -f "$KDIR/Module.symvers" ] || echo "WARNING: $KDIR/Module.symvers missing; \
the build may warn about undefined symbols (apr_send_pkt, q6afe_get_port_id, ...)"
echo "kernel release : $KREL"
echo "kernel build   : $KDIR"
grep -E 'CONFIG_MODULE_SIG|CONFIG_KALLSYMS|CONFIG_DEBUG_LOCK_ALLOC|CONFIG_LOCKDEP' \
	"$KDIR/.config" 2>/dev/null || true

# --------------------------------------------------------------------------
say "1. build"
# --------------------------------------------------------------------------
make -C "$KDIR" M="$HERE" clean >/dev/null 2>&1 || true
make -C "$KDIR" M="$HERE" modules || die "module build failed"
ls -l "$HERE/${MOD}.ko" || die "no .ko produced"

# --------------------------------------------------------------------------
say "2. load"
# --------------------------------------------------------------------------
if lsmod | grep -q "^${MOD}"; then
	$SUDO rmmod "$MOD" || die "could not unload an old copy"
fi
$SUDO insmod "$HERE/${MOD}.ko" || die "insmod failed"
dmesg | tail -n 20

# --------------------------------------------------------------------------
say "3. interface"
# --------------------------------------------------------------------------
[ -d "$SYSFS" ] || die "$SYSFS missing - did the module load?"
ls -l "$SYSFS"
echo "--- initial state ---"
cat "$SYSFS/copp" || true

# --------------------------------------------------------------------------
say "4. gain sweep"
# --------------------------------------------------------------------------
# Pick a PCM to play.  On elish the speaker path is TERTIARY_TDM_RX_0; the
# frontend is usually MultiMedia1.  The module only needs *some* stream that
# opens a COPP on the configured port.
PCM="${PCM:-hw:0,0}"
WAV="${WAV:-/usr/share/sounds/alsa/Front_Center.wav}"

if [ ! -f "$WAV" ]; then
	echo "no test wav at $WAV; generating a 10 s 48 kHz stereo tone with speaker-test"
	TONE="speaker-test -D $PCM -t sine -f 440 -c 2 -l 1"
else
	TONE="aplay -D $PCM -f S16_LE -r 48000 -c 2 '$WAV'"
fi

echo "Starting playback in the background: $TONE"
echo "(playback is what creates the COPP the module programs)"
eval "$TONE" >/dev/null 2>&1 &
PLAY_PID=$!
sleep 1

# Q27 sweep: 0 (mute) .. 0x08000000 (0 dB).  You should hear the level change.
for v in 0 16777216 33554432 67108864 134217728; do
	printf 'gain=%d (0x%x) -> ' "$v" "$v"
	echo "$v" > "$SYSFS/gain" || echo "(write failed)"
	cat "$SYSFS/copp"
	sleep 2
done

echo
echo "--- dmesg (last sends) ---"
dmesg | grep elish_adsp_vol | tail -n 15

kill "$PLAY_PID" 2>/dev/null || true
wait "$PLAY_PID" 2>/dev/null || true

# --------------------------------------------------------------------------
say "5. optional: raw Q13 and V5 paths"
# --------------------------------------------------------------------------
cat <<'EOF'
If the Q27 sweep produced no audible change:

  a) Confirm the module actually found the firmware COPP handle:
       dmesg | grep elish_adsp_vol
     Each send logs "dest_port=<handle>".  If you instead see
     "no COPP matches port_index=... (is a stream open?)", keep a stream open
     while writing the control.

  b) The control interface may need an explicit port index (DT index cells are
     unreliable; the module auto-detects the index whose q6afe_get_port_id()
     is 0x9020).  Override at load time:
       sudo rmmod elish_adsp_vol
       sudo insmod ./elish_adsp_vol.ko port_index=<n>
     or use -2 for "all ports":
       sudo insmod ./elish_adsp_vol.ko port_index=-2

  c) Send the raw firmware Q13 value (bypasses the >>14 conversion):
       echo 8192 > /sys/kernel/elish_adsp_vol/gain_q13    # 0x2000 = 0 dB
       echo 4096 > /sys/kernel/elish_adsp_vol/gain_q13    # -6 dB
     If raw Q13 works but the Q27 'gain' path does not, the Q27->Q13 mapping
     assumption is wrong (see ../patches/NOTES.md section 3).

  d) Try the V5 command form (older ADSP API) if the V6 packet is rejected:
       sudo rmmod elish_adsp_vol
       sudo insmod ./elish_adsp_vol.ko use_v5=1
     (responses are delivered to the in-kernel q6adm driver, so the module
      cannot see DSP errors - check dmesg for "DSP returned error" from the
      q6adm driver right after each write.)

  e) Alternative COPP lookup via q6adm_open() (creates a COPP when the
     playback parameters do not match an existing one):
       sudo insmod ./elish_adsp_vol.ko use_q6adm_open=1 rate=48000 channels=2 bit_width=24
EOF

# --------------------------------------------------------------------------
say "6. unload"
# --------------------------------------------------------------------------
$SUDO rmmod "$MOD" || echo "rmmod failed (module still loaded)"
lsmod | grep "$MOD" || echo "module unloaded"

say "done"
echo "full log: $LOG"
