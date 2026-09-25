#!/bin/sh
# 最终版: 交替顺序(奇偶轮颠倒)抵消充电电流漂移; 每窗口校验 RUNNING
C=/sys/class/power_supply/bq27z561-0/current_now
U="XDG_RUNTIME_DIR=/run/user/1000"; F=/usr/share/sounds/noise.wav
st(){ sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status 2>/dev/null|head -1; }
avg(){ n=0; s=0; while [ $n -lt 6 ]; do v=$(cat $C); s=$((s+v)); n=$((n+1)); sleep 0.5; done; echo $((s/n)); }
PSAMP=0; PNO=0; ISAMP=0; INO=0
playsamp(){ timeout 10 su axis -c "$U pw-play $F" >/dev/null 2>&1 & P=$!; sleep 1.5
  if [ "$(st)" = "RUNNING" ]; then a=$(avg); PSAMP=$((PSAMP+a)); PNO=$((PNO+1)); else echo -n "[skip]"; fi
  kill $P 2>/dev/null; wait $P 2>/dev/null; }
idlesamp(){ sleep 0.5; a=$(avg); ISAMP=$((ISAMP+a)); INO=$((INO+1)); }
cond(){ db=$1; an=$2; PSAMP=0;PNO=0;ISAMP=0;INO=0
  sh /root/amp-digvol.sh $db >/dev/null 2>&1
  for x in TLH TRH TLL TRL BLH BRH BLL BRL; do amixer -c0 cset "name=$x Analog PCM Volume" $an >/dev/null 2>&1; done
  i=1; while [ $i -le 6 ]; do
    if [ $((i%2)) -eq 1 ]; then playsamp; idlesamp; else idlesamp; playsamp; fi
    i=$((i+1))
  done
  pm=$((PSAMP/PNO)); im=$((ISAMP/INO)); d=$((im-pm))
  printf "  digital=+%s analog=%s: 播放=%s 静音=%s 增量=%s uA (%s mW@3.8V) 有效窗口=%s/6\n" \
    $db $an $pm $im $d $(awk "BEGIN{printf \"%.1f\", $d*3.8/1000}") $PNO
  eval "R_${db}_${an}=$d"; }
sh /usr/local/bin/amp-kick.sh >/dev/null 2>&1
echo "=== 白噪 -6dBFS: 交替顺序 6 轮 ==="
cond 0 18
cond 12 20
sh /root/amp-digvol.sh 0 >/dev/null 2>&1
for x in TLH TRH TLL TRL BLH BRH BLL BRL; do amixer -c0 cset "name=$x Analog PCM Volume" 18 >/dev/null 2>&1; done
echo "=== 汇总 ==="
echo "  Android档(0dB/18): ${R_0_18}uA   ${R_0_18:+$(awk "BEGIN{printf \"%.1f mW\", $R_0_18*3.8/1000}")}"
echo "  极限档(+12dB/20): ${R_12_20}uA   $(awk "BEGIN{printf \"%.1f mW\", $R_12_20*3.8/1000}")"
