#!/bin/bash
# A/B: 只把「上排 4 个功放」(i2c bus3) 的 ASP 改成 32-bit slot, 下排(bus1)保持原样
H4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
WR(){ i2ctransfer -f -y $1 w8@0x$2 $(H4 $3) $(H4 $4); }
RD(){ i2ctransfer -f -y $1 w4@0x$2 $(H4 $3) r4@0x$2 2>&1 | tr -d '\n' | tr -s ' '; }

echo "== 1) 预热(吸收首次上电失败) =="
aplay -q -t raw -f S16_LE -r 48000 -c 2 -d 1 /dev/zero 2>&1 | tail -1
echo "   Enable 失败数: $(dmesg | grep -c 'Enable(1) failed')"

echo "== 2) 起测试音(无限循环) =="
pkill -f speaker-test 2>/dev/null
nohup speaker-test -c 2 -t sine -f 440 -l 0 >/dev/null 2>&1 &
sleep 1

echo "== 3) 上排(bus3)改成 32-bit slot / 16-bit word =="
for a in 40 41 42 43; do
  WR 3 $a 0x4808 0x20200000     # SP_FORMAT: RX slot 32, TX slot 32
  WR 3 $a 0x4840 0x00000010     # SP_RX_WL = 16 bit
done
sleep 0.3
echo "   bus3 TLH(41) SP_FORMAT=$(RD 3 41 0x4808)  RX_WL=$(RD 3 41 0x4840)"
echo "   bus1 BLH(41) SP_FORMAT=$(RD 1 41 0x4808)  RX_WL=$(RD 1 41 0x4840)"
echo "== 测试音在响: 441Hz 正弦, 左/右声道交替 =="
