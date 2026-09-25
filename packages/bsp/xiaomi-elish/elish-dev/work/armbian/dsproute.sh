#!/bin/sh
# dsproute.sh - route audio through the amp's DSP (so Digital PCM Volume +6dB actually applies)
for nm in BRH BLH BRL BLL TRH TLH TRL TLL; do
  amixer -c0 cset "name=$nm PCM Source" DSP >/dev/null 2>&1
done
echo "PCM Source set to DSP on all 8:"
for nm in BRH TLH; do
  printf "  %s=" $nm; amixer -c0 cget "name=$nm PCM Source" 2>/dev/null | grep ": values" | head -1 | tr -d " \n"; echo
done
pkill -f "[a]play" 2>/dev/null; sleep 1
amixer -c0 cset numid=452 1 >/dev/null 2>&1
setsid sh -c 'for i in 1 2 3 4 5 6 7 8 9 10 11 12; do aplay -D hw:0,0 /root/t.wav; done' </dev/null >/tmp/dr.log 2>&1 &
sleep 5
echo "pcm=$(sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status | head -1)"
echo "10010=$(/root/ampreg 1 0x40 0x10010 | cut -d= -f2)  2014=$(/root/ampreg 1 0x40 0x2014 | cut -d= -f2)"
echo "PLAYING via DSP path - listen"
