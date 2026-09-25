#!/bin/sh
# 手工上电 kick + 只修 RX slot 宽度(保留 word length)
H4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
W(){ i2ctransfer -f -y $1 w8@0x$2 $(H4 $3) $(H4 $4) >/dev/null 2>&1; }
R(){ i2ctransfer -f -y $1 w4@0x$2 $(H4 $3) r4@0x$2 2>/dev/null; }
FIXASP(){ for b in 1 3; do for a in 40 41 42 43; do
    v=$(R $b $a 0x00004808); set -- $v; [ -z "$1" ] && continue
    new=$(( (0x20 << 24) | ($2 << 16) | ($3 << 8) | $4 )); W $b $a 0x00004808 "$new"
  done; done; }
for b in 1 3; do for a in 40 41 42 43; do
  W $b $a 0x00000040 0x00000055; W $b $a 0x00000040 0x000000AA
  W $b $a 0x00002084 0x002F1AA0; W $b $a 0x00000040 0x000000CC; W $b $a 0x00000040 0x00000033
  W $b $a 0x00002018 0x00003721; W $b $a 0x00002014 0x00000001
done; done
FIXASP
echo "kick done"
