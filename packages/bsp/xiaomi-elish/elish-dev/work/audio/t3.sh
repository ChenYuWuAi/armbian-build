#!/bin/sh
W=/home/axis/26913a7d7e1b8b814aaecfa950acef33.wav
h4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
RD(){ i2ctransfer -f -y $1 w4@0x$2 $(h4 $3) r4@0x$2 2>&1 | tr -d '\n' | tr -s ' '; }
PWR(){ RD $1 $2 0x2014 | cut -d' ' -f4; }
RAW24(){ RD $1 $2 0x10090 | cut -d' ' -f3; }
SNAP(){ for b in 3 1; do for a in 40 41 42 43; do printf "    bus%s/%s PWR1=%s RAW1_hi=%s\n" $b $a "$(PWR $b $a)" "$(RAW24 $b $a)"; done; done; }
echo "开机失败数=$(dmesg|grep -c 'Enable(1) failed')"
echo "== 播放#1 (8 秒) =="
timeout 8 aplay -q -D plughw:0,0 "$W" 2>&1|tail -1
echo "   #1 后失败数=$(dmesg|grep -c 'Enable(1) failed')"
echo "== 播放#2 (8 秒, 播放中抓寄存器) =="
( sleep 3; echo "  --- 播放中快照 ---"; SNAP ) &
timeout 8 aplay -q -D plughw:0,0 "$W" 2>&1|tail -1
wait
echo "   #2 后失败数=$(dmesg|grep -c 'Enable(1) failed')"
