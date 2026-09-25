#!/bin/sh
# elish: align the CS35L41 amplifier mixer state with the vendor (Android)
# `mixer_paths_overlay_static.xml`, and set the ADM COPP topology.
#
# Reference values (all taken from the stock overlay XML):
#   <PREFIX> PCM Source            = DSP          (audio runs through the amp DSP:
#                                                 protection + DRE + Class-H)
#   <PREFIX> DSP RX1/RX2 Source    = ASPRX1/ASPRX2
#   <PREFIX> ASP TX1 Source        = DSPTX1
#   <PREFIX> DSP1 Preload Switch   = 1
#   <PREFIX> DRE DRE Switch        = 1
#   <PREFIX> PCM Soft Ramp         = 4ms
#   <PREFIX> AMP PCM Gain          = 18           -> "Analog PCM Volume" = 18
#   <PREFIX> Digital PCM Volume    = 817 (0 dB)   (vendor register 0x6000 field 0)
# and TERT_TDM_RX_0 Audio Mixer MultiMedia1 = on.
#
# The remaining vendor register values live in the driver now:
#   SP_FORMAT = 0x20200000 / SP_RX_WL = 24  (8x32-bit TDM frame, S24_LE)
#   AMP_GAIN_CTRL = 0x253 (PCM gain 18 + PDM gain 19), NG_CFG = 0x3F75
set -u

LOG=/var/log.hdd/elish-amp-align.log
log() { echo "$(date '+%F %T') $*" >> "$LOG" 2>/dev/null; }
mkdir -p /var/log.hdd 2>/dev/null || true

[ -e /proc/asound/card0 ] || { log "no card0"; exit 1; }

# elish: wait for the user's PipeWire/WirePlumber to finish enumerating the
# card.  Our control writes raced with ACP's UCM parse and left the device
# with only an "off" profile (no hardware sink -> auto_null fallback).
i=0
while [ $i -lt 45 ]; do
	if su axis -c "XDG_RUNTIME_DIR=/run/user/1000 wpctl status" 2>/dev/null | grep -q "Speaker playback"; then
		break
	fi
	i=$((i + 1)); sleep 1
done
log "pipewire settled after ${i}s"

# ADM COPP topology (built-in q6routing parameter, resets to 0 on boot)
if [ -w /sys/module/q6routing/parameters/copp_topology ]; then
	echo 268476672 > /sys/module/q6routing/parameters/copp_topology
	log "copp_topology=$(cat /sys/module/q6routing/parameters/copp_topology)"
fi

for nm in TLH TRH TLL TRL BLH BRH BLL BRL; do
	amixer -c0 cset "name=$nm PCM Source"        DSP     >/dev/null 2>&1
	amixer -c0 cset "name=$nm DSP1 Preload Switch" 1     >/dev/null 2>&1
	amixer -c0 cset "name=$nm DRE Switch"        1       >/dev/null 2>&1
	amixer -c0 cset "name=$nm PCM Soft Ramp"     4ms     >/dev/null 2>&1
	amixer -c0 cset "name=$nm Analog PCM Volume" 20      >/dev/null 2>&1
	amixer -c0 cset "name=$nm Digital PCM Volume" 913    >/dev/null 2>&1
	amixer -c0 cset "name=$nm ASP TX1 Source"    DSPTX1  >/dev/null 2>&1
	# elish: bus1 (BRH/BLH/BRL/BLL) = right-hand speakers -> RX1 reads slot 1 (matches the driver)
	case "$nm" in BRH|BLH|BRL|BLL) amixer -c0 cset "name=$nm DSP RX1 Source" ASPRX2 >/dev/null 2>&1;; *) amixer -c0 cset "name=$nm DSP RX1 Source" ASPRX1 >/dev/null 2>&1;; esac
	amixer -c0 cset "name=$nm DSP RX2 Source"    ASPRX2  >/dev/null 2>&1
done

# the vendor speaker route: TERT TDM RX0 fed by MultiMedia1
amixer -c0 cset "name=TERT_TDM_RX_0 Audio Mixer MultiMedia1" on >/dev/null 2>&1

log "applied: $(amixer -c0 cget name='TLH PCM Source' 2>/dev/null | grep -E '^  : values=' | tr -d ' ') $(amixer -c0 cget name='TLH Analog PCM Volume' 2>/dev/null | grep -E '^  : values=' | tr -d ' ') $(amixer -c0 cget name='TLH Digital PCM Volume' 2>/dev/null | grep -E '^  : values=' | tr -d ' ')"

# persist so alsa-restore brings the same state back next boot
if alsactl store >/dev/null 2>&1; then log "alsactl store ok"; else log "alsactl store skipped"; fi

exit 0
