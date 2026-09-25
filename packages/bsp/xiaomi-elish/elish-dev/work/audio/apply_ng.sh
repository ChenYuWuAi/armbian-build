#!/bin/sh
H4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
W(){ i2ctransfer -f -y $1 w8@0x$2 $(H4 $3) $(H4 $4) >/dev/null 2>&1; }
R(){ i2ctransfer -f -y $1 w4@0x$2 $(H4 $3) r4@0x$2 2>/dev/null|tr -d '\n'; }
for b in 1 3; do for a in 40 41 42 43; do
  W $b $a 0x6808 0x00003F75
  W $b $a 0x4c00 0x00000000; W $b $a 0x4c20 0x00000032; W $b $a 0x4c24 0x00000000
  W $b $a 0x4c28 0x00000020; W $b $a 0x4c2c 0x00000021
  W $b $a 0x4c40 0x00000008; W $b $a 0x4c44 0x00000009
  W $b $a 0x4808 0x20200000; W $b $a 0x4840 0x00000010
done; done
echo "  TLH NG_CFG=$(R 3 41 0x6808)  DSP1_RX2=$(R 3 41 0x4c44)  FMT=$(R 3 41 0x4808)"
