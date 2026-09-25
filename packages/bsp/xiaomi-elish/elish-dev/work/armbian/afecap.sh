#!/bin/sh
# afecap.sh - stop PipeWire (frees the PCM), then capture dmesg while opening a FRESH stream.
su - axis -c "systemctl --user stop pipewire.socket pipewire-pulse.socket wireplumber pipewire pipewire-pulse" 2>/dev/null
pkill -f "[a]play" 2>/dev/null
sleep 2
echo "pipewire procs now: $(pgrep -c -u axis -f pipewire)"
rm -f /tmp/d2.log
timeout 22 dmesg --follow > /tmp/d2.log 2>&1 &
sleep 1
amixer -c0 cset numid=452 1 >/dev/null 2>&1
aplay -D hw:0,0 /root/t.wav >/dev/null 2>&1 &
sleep 12
echo "pcm=$(sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status | head -1)"
echo "=== captured audio lines ==="
grep -iE "afe|tdm|cs35l41|apr|clock|lpass" /tmp/d2.log | grep -viE "sensor_process|qmi|fastrpc" | head -30
