#!/bin/sh
T="timeout 8"; U="sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000"
W=/home/axis/26913a7d7e1b8b814aaecfa950acef33.wav
rd(){ n=$(amixer -c0 controls 2>/dev/null|grep "name='TLH DSP1 Protection cd $1'"|grep -oE "numid=[0-9]+"|cut -d= -f2|head -1); [ -n "$n" ] && amixer -c0 cget numid=$n 2>/dev/null|sed -n '3p'|tr -d ' '; }
( sleep 5
  echo "== sink 音量 =="; $T $U wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>&1 | head -1
  ID=$($T $U wpctl status 2>/dev/null | sed -n '/Streams:/,$p' | grep -oE "^ *[0-9]+\. pw-(play|cat)" | grep -oE "[0-9]+" | head -1)
  echo "== stream id=$ID 详情 =="
  [ -n "$ID" ] && $T $U wpctl inspect "$ID" 2>/dev/null | grep -iE "vol|channel|name" | head -12
  echo "== 功放(TLH) =="
  echo "   FMT=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x48 0x08 r4@0x41 2>&1|tr -d '\n') WL=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x48 0x40 r4@0x41 2>&1|tr -d '\n') PWR1=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x20 0x14 r4@0x41 2>&1|tr -d '\n') PUP=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x01 0x00 0x90 r4@0x41 2>&1|tr -d '\n')"
  echo "== DSP 诊断 =="; echo "   SPK_OUTPUT_POWER=$(rd SPK_OUTPUT_POWER) DIAG_F0_STATUS=$(rd DIAG_F0_STATUS) ZDIFF=$(rd DIAG_Z_LOW_DIFF) TEMP=$(rd CSPL_TEMPERATURE)"
) &
timeout 25 $U pw-play "$W" 2>&1 | tail -2
wait
