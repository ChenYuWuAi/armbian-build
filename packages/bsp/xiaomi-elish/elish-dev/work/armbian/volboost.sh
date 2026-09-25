#!/bin/sh
# volboost.sh - SAFE volume alignment to Android (NO test-key / NO PLL / NO GLOBAL_EN writes).
# Only: ALSA controls (via kernel) + config registers 0x4808/0x4810/0x6c04/0x6808.
W(){ i2ctransfer -f -y $1 w8@0x$2 $(printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($3>>24)&255)) $((($3>>16)&255)) $((($3>>8)&255)) $(($3&255))) >/dev/null 2>&1; }

pkill -f "[a]play" 2>/dev/null; sleep 1
amixer -c0 cset numid=452 1 >/dev/null 2>&1
setsid sh -c 'for i in 1 2 3 4 5 6 7 8 9 10; do aplay -D hw:0,0 /root/t.wav; done' </dev/null >/tmp/vb.log 2>&1 &
sleep 5
echo "pcm=$(sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status | head -1)  2014=$(/root/ampreg 1 0x40 0x2014 | cut -d= -f2)"

echo "--- step 1: ALSA controls (safe, via kernel) ---"
for nm in BRH BLH BRL BLL TRH TLH TRL TLL; do
  amixer -c0 cset "name=$nm Analog PCM Volume" 18  >/dev/null 2>&1
  amixer -c0 cset "name=$nm Digital PCM Volume" 865 >/dev/null 2>&1
  amixer -c0 cset "name=$nm DRE Switch" 1 >/dev/null 2>&1
done
echo "done (Analog=18, Digital=865/+6dB, DRE=on)"

echo "--- step 2: config regs (0x4808/0x4810/0x6c04/0x6808) ---"
for b in 1 3; do for a in 40 41 42 43; do
  W $b $a 0x00004808 0x20200000
  W $b $a 0x00004810 0x04040404
  W $b $a 0x00006c04 0x00000253
  W $b $a 0x00006808 0x00003F75
done; done
echo "applied. readback (bus1 0x40):"
for r in 0x4808 0x4810 0x6c04 0x10010; do echo "  $r=$(/root/ampreg 1 0x40 $r | cut -d= -f2)"; done
echo "PLAYING NOW - listen"
