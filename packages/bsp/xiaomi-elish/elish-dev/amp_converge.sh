#!/bin/bash
# 从失败态出发，反复播放 + 每次手工探测，抓出"能上电"的翻转点
h4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
rd(){ sudo i2ctransfer -f -y $1 w4@0x$2 $(h4 $3) r4@0x$2 2>&1 | tr -d '\n' | tr -s ' '; }
wr(){ sudo i2ctransfer -f -y 3 w8@0x41 $(h4 $1) $(h4 $2); }
pup(){ wr 0x40 0x55; wr 0x40 0xAA; wr 0x2084 0x2F1AA0; wr 0x40 0xCC; wr 0x40 0x33; }

probe(){
  local tag="$1"
  pup
  wr 0x2014 0x1
  sleep 0.15
  echo "  [$tag] PWR1=$(rd 3 41 0x2014) RAW1=$(rd 3 41 0x10090) PWR2=$(rd 3 41 0x2018)"
  echo "        CCM_CORE=$(rd 3 41 0x2bc1000) MBOX2=$(rd 3 41 0x13004) PWR3=$(rd 3 41 0x201c)"
  wr 0x2014 0x0
  sleep 0.1
}

sudo dmesg -C
echo "##### 起点: $(uptime | tr -s ' ' | cut -d, -f1) #####"
probe "初始状态"
echo
for k in 1 2 3 4 5 6; do
  timeout 12 speaker-test -c 2 -t sine -f 440 -l 1 >/dev/null 2>&1
  N=$(sudo dmesg | grep -c 'Enable(1) failed')
  echo "===== 播放 #$k 完成 (累计 Enable 失败数=$N) ====="
  probe "播放#$k 后"
  echo
done
echo "===== 最终 dmesg 尾部 ====="
sudo dmesg | tail -6
