#!/bin/sh
C=/sys/class/power_supply/bq27z561-0/current_now
U="XDG_RUNTIME_DIR=/run/user/1000"; F=/usr/share/sounds/noise.wav
st(){ sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status 2>/dev/null|head -1; }
avg(){ n=0; s=0; while [ $n -lt 6 ]; do v=$(cat $C); s=$((s+v)); n=$((n+1)); sleep 0.5; done; echo $((s/n)); }
PSAMP=0;PNO=0;ISAMP=0;INO=0
playsamp(){ timeout 10 su axis -c "$U pw-play $F" >/dev/null 2>&1 & P=$!; sleep 1.5
  if [ "$(st)" = "RUNNING" ]; then a=$(avg); PSAMP=$((PSAMP+a)); PNO=$((PNO+1)); fi
  kill $P 2>/dev/null; wait $P 2>/dev/null; }
idlesamp(){ sleep 0.5; a=$(avg); ISAMP=$((ISAMP+a)); INO=$((INO+1)); }
meas(){ tag=$1; PSAMP=0;PNO=0;ISAMP=0;INO=0; i=1
  while [ $i -le 4 ]; do if [ $((i%2)) -eq 1 ]; then playsamp; idlesamp; else idlesamp; playsamp; fi; i=$((i+1)); done
  pm=$((PSAMP/PNO)); im=$((ISAMP/INO)); d=$((im-pm))
  printf "  %-22s 播放=%s 静音=%s 增量=%s uA (%s mW) 有效=%s/4\n" "$tag" $pm $im $d $(awk "BEGIN{printf \"%.1f\", $d*3.8/1000}") $PNO; }
sh /root/amp-digvol.sh 0 >/dev/null 2>&1
for x in TLH TRH TLL TRL BLH BRH BLL BRL; do amixer -c0 cset "name=$x Analog PCM Volume" 18 >/dev/null 2>&1; amixer -c0 cset "name=$x PCM Source" DSP >/dev/null 2>&1; done
sh /usr/local/bin/amp-kick.sh >/dev/null 2>&1
echo "=== whitenoise -6dBFS, digital 0dB, analog 18 ==="
meas "A: PCM Source=DSP"
for x in TLH TRH TLL TRL BLH BRH BLL BRL; do amixer -c0 cset "name=$x PCM Source" ASP >/dev/null 2>&1; done
sh /usr/local/bin/amp-kick.sh >/dev/null 2>&1
meas "B: PCM Source=ASP(直通)"
for x in TLH TRH TLL TRL BLH BRH BLL BRL; do amixer -c0 cset "name=$x PCM Source" DSP >/dev/null 2>&1; done
