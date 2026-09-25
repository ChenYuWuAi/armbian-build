#!/bin/sh
rd(){ n=$(amixer -c0 controls 2>/dev/null|grep "name='TLH DSP1 Protection cd $1'"|grep -oE "numid=[0-9]+"|cut -d= -f2|head -1); [ -n "$n" ] && amixer -c0 cget numid=$n 2>/dev/null|sed -n '3p'|tr -d ' '; }
snap(){ echo "   STATS PWR=$(rd SPK_OUTPUT_POWER) F0ST=$(rd DIAG_F0_STATUS) ZDIFF=$(rd DIAG_Z_LOW_DIFF) TEMP=$(rd CSPL_TEMPERATURE) FMT=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x48 0x08 r4@0x41 2>&1|tr -d '\n') WL=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x48 0x40 r4@0x41 2>&1|tr -d '\n')"; }
echo "=== A) aplay 直连 (S16) ==="
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 systemctl --user stop pipewire.socket pipewire-pulse.socket pipewire pipewire-pulse wireplumber >/dev/null 2>&1
pkill -x pipewire; pkill -x wireplumber; sleep 2
( sleep 2; snap ) & timeout 5 aplay -q -D plughw:0,0 -f S16_LE -r 48000 -c 2 /var/tmp/quiet.wav 2>/dev/null; wait
echo "=== B) pipewire ==="
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 systemctl --user start pipewire pipewire-pulse wireplumber >/dev/null 2>&1; sleep 4
sh /root/sinks.sh >/dev/null 2>&1
U="XDG_RUNTIME_DIR=/run/user/1000"
( sleep 2; echo "   sink=$(sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>&1|head -1)"; echo "   fmt=$(sed -n 's/^format: *//p' /proc/asound/card0/pcm0p/sub0/hw_params 2>/dev/null|head -1)"; snap ) &
timeout 6 sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 pw-play /var/tmp/quiet.wav 2>/dev/null; wait
echo "=== C) pipewire 播放中的 stream 音量 ==="
( sleep 2; sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 wpctl status 2>/dev/null | sed -n '/Streams:/,/Video/p' | head -8 ) &
timeout 6 sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 pw-play /var/tmp/quiet.wav 2>/dev/null; wait
