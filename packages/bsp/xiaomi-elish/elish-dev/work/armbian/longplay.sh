#!/bin/sh
# longplay.sh - verified long playback for listening tests (10x t.wav ~ 3min)
pkill -f "[a]play" 2>/dev/null
sleep 1
amixer -c0 cset numid=452 1 >/dev/null 2>&1
setsid sh -c 'for i in 1 2 3 4 5 6 7 8 9 10; do aplay -D hw:0,0 /root/t.wav; done' </dev/null >/tmp/longplay.log 2>&1 &
sleep 6
echo "playing=$(pgrep -c -f '[a]play')  pcm=$(sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status | head -1)"
echo "2014=$(/root/ampreg 1 0x40 0x2014 | cut -d= -f2)  10010=$(/root/ampreg 1 0x40 0x10010 | cut -d= -f2)"
