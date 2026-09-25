#!/bin/sh
# +6dB = 817 + 48 步(0.125dB/步) = 865
for n in $(amixer -c0 controls 2>/dev/null | grep "Digital PCM Volume" | grep -oE "numid=[0-9]+" | cut -d= -f2); do
  amixer -c0 cset numid=$n 865 >/dev/null 2>&1
done
( sleep 3; echo "  播放中 TLH 数字音量控制 = $(amixer -c0 cget numid=1 2>/dev/null | sed -n '3p')"; echo "  播放中 TLH AMP_DIG_VOL 寄存器 = $(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x60 0x00 r4@0x41 2>&1|tr -d '\n')" ) &
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 pw-play /var/tmp/test5s.wav 2>&1|tail -1
wait
echo "  播放后读回 = $(amixer -c0 cget numid=1 2>/dev/null | sed -n '3p')"
