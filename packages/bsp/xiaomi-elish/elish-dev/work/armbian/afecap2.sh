#!/bin/sh
# afecap2.sh - force-release the PCM, then capture dmesg during a FRESH stream open.
fuser -k /dev/snd/pcmC0D0p 2>/dev/null
pkill -f "[a]play" 2>/dev/null
pkill -u axis -f "pipewire" 2>/dev/null
pkill -u axis -f "wireplumber" 2>/dev/null
sleep 3
echo "pipewire=$(pgrep -c -u axis -f pipewire)  pcm-before=$(sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status | head -1)"
rm -f /tmp/d3.log
timeout 20 dmesg --follow > /tmp/d3.log 2>&1 &
sleep 1
amixer -c0 cset numid=452 1 >/dev/null 2>&1
aplay -D hw:0,0 /root/t.wav >/dev/null 2>&1 &
sleep 12
echo "pcm-after=$(sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status | head -1)  captured-lines=$(wc -l < /tmp/d3.log)"
echo "=== audio lines ==="
grep -iE "afe|tdm|cs35l41|apr|lpass" /tmp/d3.log | grep -viE "sensor_process|qmi|fastrpc" | head -25
