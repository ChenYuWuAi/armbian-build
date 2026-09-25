#!/bin/sh
h4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
RD(){ i2ctransfer -f -y $1 w4@0x$2 $(h4 $3) r4@0x$2 2>&1 | tr -d '\n' | tr -s ' '; }
SNAP(){ for b in 3 1; do for a in 40 41 42 43; do v=$(RD $b $a 0x10090); printf "    bus%s/%s PWR1=%s RAW1=%s PUP=%s\n" $b $a "$(RD $b $a 0x2014|cut -d' ' -f4)" "$v" "$(echo $v|cut -d' ' -f1)"; done; done; }
echo "失败数=$(dmesg|grep -c 'Enable(1) failed')"
( sleep 3; echo "  --- 播放中 ---"; SNAP ) &
timeout 10 speaker-test -c 2 -t sine -f 440 -l 3 >/dev/null 2>&1
wait
echo "之后失败数=$(dmesg|grep -c 'Enable(1) failed')"
echo "== 该 wav 格式 =="; soxi /home/axis/26913a7d7e1b8b814aaecfa950acef33.wav 2>/dev/null | head -8
