#!/bin/sh
# 用满幅音测量保护余量: 读 DSP 遥测 (不受电池充电状态影响)
U="XDG_RUNTIME_DIR=/run/user/1000"
get(){ timeout 6 amixer -c0 cget "name=$1 DSP1 Protection $2" 2>/dev/null | sed -n 's/  : values=//p' | tr -d ' '; }
att(){ for n in TLH TRH BLH BRL; do printf "%s=%s " $n "$(get $n "cd ATTENUATION")"; done; }
tmp(){ printf "TEMP(TLH)=%s " "$(get TLH "cd CSPL_TEMPERATURE")"; }
run(){ db=$1
  sh /root/amp-digvol.sh $db >/dev/null 2>&1
  timeout 16 su axis -c "$U pw-play /usr/share/sounds/sine_fs.wav" >/dev/null 2>&1 &
  P=$!; sleep 5
  s=$(sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status 2>/dev/null|head -1)
  echo "  digital=+${db}dB (val=$(amixer -c0 cget 'name=BRH Digital PCM Volume' 2>/dev/null|sed -n 's/  : values=//p')) state=$s"
  echo "     $(att)  $(tmp)"
  wait $P 2>/dev/null
}
echo "=== -3dBFS 1kHz, analog=18, 保护遥测 ==="
sh /usr/local/bin/amp-kick.sh >/dev/null 2>&1
run 0; run 6; run 12
echo "=== 再叠加 analog=20 + digital=+12dB (最激进) ==="
for n in TLH TRH TLL TRL BLH BRH BLL BRL; do amixer -c0 cset "name=$n Analog PCM Volume" 20 >/dev/null 2>&1; done
run 12
for n in TLH TRH TLL TRL BLH BRH BLL BRL; do amixer -c0 cset "name=$n Analog PCM Volume" 18 >/dev/null 2>&1; done
sh /root/amp-digvol.sh 0 >/dev/null 2>&1
