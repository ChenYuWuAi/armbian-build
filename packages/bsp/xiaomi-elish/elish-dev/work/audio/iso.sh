#!/bin/sh
rd(){ n=$(amixer -c0 controls 2>/dev/null|grep "name='TLH DSP1 Protection cd $1'"|grep -oE "numid=[0-9]+"|cut -d= -f2|head -1); [ -n "$n" ] && amixer -c0 cget numid=$n 2>/dev/null|sed -n '3p'|tr -d ' '; }
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 systemctl --user stop pipewire.socket pipewire-pulse.socket pipewire pipewire-pulse wireplumber >/dev/null 2>&1
pkill -x pipewire; pkill -x wireplumber; sleep 2
echo "### 1) aplay S16_LE ###"
( sleep 2; echo "   播放中: WL=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x48 0x40 r4@0x41 2>&1|tr -d '\n') FMT=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x48 0x08 r4@0x41 2>&1|tr -d '\n') PWR=$(rd SPK_OUTPUT_POWER) ZDIFF=$(rd DIAG_Z_LOW_DIFF) F0ST=$(rd DIAG_F0_STATUS)" ) &
timeout 5 aplay -q -D plughw:0,0 -f S16_LE -r 48000 -c 2 /var/tmp/quiet.wav 2>/dev/null; wait
echo "### 2) aplay S24_LE ###"
( sleep 2; echo "   播放中: WL=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x48 0x40 r4@0x41 2>&1|tr -d '\n') FMT=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x48 0x08 r4@0x41 2>&1|tr -d '\n') PWR=$(rd SPK_OUTPUT_POWER) ZDIFF=$(rd DIAG_Z_LOW_DIFF) F0ST=$(rd DIAG_F0_STATUS)" ) &
timeout 5 aplay -q -D plughw:0,0 -f S24_LE -r 48000 -c 2 /var/tmp/quiet.wav 2>&1|tail -1; wait
echo "### 3) aplay S32_LE ###"
( sleep 2; echo "   播放中: WL=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x48 0x40 r4@0x41 2>&1|tr -d '\n') PWR=$(rd SPK_OUTPUT_POWER)" ) &
timeout 5 aplay -q -D plughw:0,0 -f S32_LE -r 48000 -c 2 /var/tmp/quiet.wav 2>&1|tail -1; wait
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 systemctl --user start pipewire pipewire-pulse wireplumber >/dev/null 2>&1; sleep 4; sh /root/sinks.sh >/dev/null 2>&1; echo "(pipewire 已恢复)"
