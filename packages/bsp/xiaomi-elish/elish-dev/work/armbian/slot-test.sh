#!/bin/sh
# slot-test: start long playback, dump slot regs, write Android's 0x4810 value, keep playing.
pkill -f "[a]play" 2>/dev/null
sleep 1
amixer -c0 cset numid=452 1 >/dev/null 2>&1
setsid sh -c 'for i in 1 2 3 4 5 6 7 8; do aplay -D hw:0,0 /root/t.wav; done' </dev/null >/tmp/slot.log 2>&1 &
sleep 4
echo "playing=$(pgrep -c -f '[a]play')  2014=$(/root/ampreg 1 0x40 0x2014 | cut -d= -f2)"
echo "--- before ---"
for r in 0x4808 0x4810 0x4814 0x6c04; do echo "$r=$(/root/ampreg 1 0x40 $r | cut -d= -f2)"; done
/root/ampreg 1 0x40 0x4810 0x04040404 >/dev/null
echo "after 0x4810=$(/root/ampreg 1 0x40 0x4810 | cut -d= -f2)"
echo "STILL PLAYING - listen now ($(pgrep -c -f '[a]play') aplay procs)"
