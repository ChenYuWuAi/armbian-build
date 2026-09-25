#!/bin/sh
U="sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000"
echo "== 把 8 路 PCM Source 切成 DSP =="
for n in $(amixer -c0 controls 2>/dev/null | grep "PCM Source" | grep -oE "numid=[0-9]+" | cut -d= -f2); do
  amixer -c0 cset numid=$n DSP >/dev/null 2>&1 || amixer -c0 cset numid=$n 1 >/dev/null 2>&1
done
for name in TLH BLH; do
  n=$(amixer -c0 controls 2>/dev/null | grep "name='$name PCM Source'" | grep -oE "numid=[0-9]+" | cut -d= -f2)
  echo "   $name PCM Source = $(amixer -c0 cget numid=$n 2>/dev/null | sed -n '3p' | tr -d ' ')"
done
echo "== 播放你的 wav (20s, 听响度/音质变化) =="
timeout 20 $U pw-play /home/axis/26913a7d7e1b8b814aaecfa950acef33.wav 2>/dev/null
( sleep 6; echo "   播放中 DSP 诊断: SPK_OUTPUT_POWER=$(n=$(amixer -c0 controls|grep "name='TLH DSP1 Protection cd SPK_OUTPUT_POWER'"|grep -oE "numid=[0-9]+"|cut -d= -f2); amixer -c0 cget numid=$n 2>/dev/null|sed -n '3p'|tr -d ' ') ATTEN=$(n=$(amixer -c0 controls|grep "name='TLH DSP1 Protection cd ATTENUATION'"|grep -oE "numid=[0-9]+"|cut -d= -f2); amixer -c0 cget numid=$n 2>/dev/null|sed -n '3p'|tr -d ' ')" ) &
wait
