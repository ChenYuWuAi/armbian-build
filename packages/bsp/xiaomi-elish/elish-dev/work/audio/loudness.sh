#!/bin/sh
U="sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000"
F=/var/tmp/sine1k.wav
cur(){ for p in /sys/class/power_supply/*/current_now; do [ -f "$p" ] && { echo -n "$(basename $(dirname $p))=$(cat $p) "; }; done; }
pw(){ n=$(timeout 8 amixer -c0 controls 2>/dev/null|grep "name=.TLH DSP1 Protection cd SPK_OUTPUT_POWER."|grep -oE "numid=[0-9]+"|cut -d= -f2); [ -n "$n" ] && timeout 8 amixer -c0 cget numid=$n 2>/dev/null|sed -n '3p'|tr -d ' '; }
run(){ ( sleep 5; echo "  [$1] 电流: $(cur)  SPK_OUTPUT_POWER=$(pw)" ) & timeout 9 $U pw-play $F >/dev/null 2>&1; wait; }
echo "== 空闲 =="; echo "  电流: $(cur)"
/usr/local/bin/amp-digvol.sh 0  >/dev/null 2>&1; run "数字 0dB"
/usr/local/bin/amp-digvol.sh 12 >/dev/null 2>&1; run "数字 +12dB"
/usr/local/bin/amp-digvol.sh 6  >/dev/null 2>&1; echo "(已设为 +6dB)"
