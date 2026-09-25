#!/bin/sh
# volcheck.sh - restart playback, show actual volume control values
pkill -f "[a]play" 2>/dev/null; sleep 1
amixer -c0 cset numid=452 1 >/dev/null 2>&1
setsid sh -c 'for i in 1 2 3 4 5 6 7 8 9 10 11 12; do aplay -D hw:0,0 /root/t.wav; done' </dev/null >/tmp/vc.log 2>&1 &
sleep 5
echo "pcm=$(sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status | head -1)"
for nm in BRH TLH; do
  a=$(amixer -c0 cget "name=$nm Analog PCM Volume" 2>/dev/null | grep ": values" | head -1)
  d=$(amixer -c0 cget "name=$nm Digital PCM Volume" 2>/dev/null | grep ": values" | head -1)
  echo "$nm $a | $d"
done
echo "10010=$(/root/ampreg 1 0x40 0x10010 | cut -d= -f2)  6c04=$(/root/ampreg 1 0x40 0x6c04 | cut -d= -f2)"
echo "PLAYING - listen now"
