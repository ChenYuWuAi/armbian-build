#!/bin/sh
U="sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000"; F=/var/tmp/sine1k.wav
H4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
R(){ i2ctransfer -f -y 3 w4@0x41 $(H4 $1) r4@0x41 2>/dev/null; }
cur(){ cat /sys/class/power_supply/bq27z561-0/current_now; }
mux(){ n=$(timeout 8 amixer -c0 controls|grep "name=.TLH PCM Source."|grep -oE "numid=[0-9]+"|cut -d= -f2); timeout 8 amixer -c0 cget numid=$n 2>/dev/null|sed -n '5p'|tr -d ' '; }
st(){ echo "mux=$(mux) NG=$(R 0x00006808|tr -d '\n') FMT=$(R 0x00004808|tr -d '\n') 模拟=$(timeout 8 amixer -c0 cget numid=2 2>/dev/null|sed -n '3p'|tr -d ' ') 数字=$(timeout 8 amixer -c0 cget numid=1 2>/dev/null|sed -n '3p'|tr -d ' ')"; }
run(){ ( sleep 5; echo "  >> [$1]"; echo "     配置: $(st)"; echo "     电流=$(cur)" ) & timeout 9 $U pw-play $F >/dev/null 2>&1; wait; }
echo "空闲电流=$(cur)"
timeout 10 systemctl stop amp-fix.service >/dev/null 2>&1; pkill -x amp-fix.sh 2>/dev/null
run "A 旧 baseline（服务停→驱动默认）"
/usr/local/bin/amp-kick.sh >/dev/null 2>&1
for nm in TLH TRH TLL TRL BLH BRH BLL BRL; do timeout 8 amixer -c0 cset "name=$nm PCM Source" DSP >/dev/null 2>&1; done
/usr/local/bin/amp-digvol.sh 12 >/dev/null 2>&1
for n in $(timeout 8 amixer -c0 controls|grep "Analog PCM Volume"|grep -oE "numid=[0-9]+"|cut -d= -f2); do timeout 8 amixer -c0 cset numid=$n 20 >/dev/null 2>&1; done
timeout 10 systemctl start amp-fix.service >/dev/null 2>&1
run "B 现在（全部对齐+模拟20+数字+12+调音下发）"
