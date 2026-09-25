#!/bin/sh
# pwt : play the 1 kHz tone through PipeWire (desktop path) in a loop, report state.
pkill -f "[a]play" 2>/dev/null
pkill -f "[p]aplay" 2>/dev/null
sleep 1
amixer -c0 cset numid=452 1 >/dev/null 2>&1
amixer -c0 cset numid=453 1 >/dev/null 2>&1
setsid sh -c 'for i in 1 2 3 4 5 6 7 8; do paplay /root/t.wav; done' </dev/null >/tmp/pw.log 2>&1 &
sleep 5
echo "paplay=$(pgrep -c -f '[p]aplay')"
echo "pcm0p=$(sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status 2>/dev/null | head -1)"
echo "2014=$(/root/ampreg 1 0x40 0x2014 | cut -d= -f2)  4810=$(/root/ampreg 1 0x40 0x4810 | cut -d= -f2)"
echo "--- paplay log ---"; tail -3 /tmp/pw.log 2>/dev/null
