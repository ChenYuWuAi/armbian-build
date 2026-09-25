#!/bin/sh
# 把 8 路功放模拟增益设到硬件上限(20 => +20.5dB), 并把 pipewire sink 音量抬到 1.5
for n in $(amixer -c0 controls 2>/dev/null | grep "Analog PCM Volume" | grep -oE "numid=[0-9]+" | cut -d= -f2); do
  amixer -c0 cset numid=$n 20 >/dev/null 2>&1
done
echo "模拟增益: $(amixer -c0 cget numid=2 2>/dev/null | sed -n '3p')"
U="XDG_RUNTIME_DIR=/run/user/1000"
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 wpctl set-volume @DEFAULT_AUDIO_SINK@ 1.5 2>&1 | tail -1
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>&1 | head -1
echo "-- 试更高 (2.0) --"
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 wpctl set-volume @DEFAULT_AUDIO_SINK@ 2.0 2>&1 | tail -1
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>&1 | head -1
