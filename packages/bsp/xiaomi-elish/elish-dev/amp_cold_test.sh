#!/bin/bash
# 冷启动受控实验：冷态手工 GLOBAL_EN vs 热态手工 GLOBAL_EN
h4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
rd(){ sudo i2ctransfer -f -y $1 w4@0x$2 $(h4 $3) r4@0x$2 2>&1 | tr -d '\n' | tr -s ' '; }
wr(){ sudo i2ctransfer -f -y 3 w8@0x41 $(h4 $1) $(h4 $2); }
pup(){ wr 0x40 0x55; wr 0x40 0xAA; wr 0x2084 0x2F1AA0; wr 0x40 0xCC; wr 0x40 0x33; }
show(){
  echo "    PWR_CTRL1=$(rd 3 41 0x2014)  PWR_CTRL2=$(rd 3 41 0x2018)  PWR_CTRL3=$(rd 3 41 0x201c)"
  echo "    RAW1=$(rd 3 41 0x10090)  ST1=$(rd 3 41 0x10010)"
  echo "    MBOX_2=$(rd 3 41 0x13004)  CCM_CORE=$(rd 3 41 0x2bc1000)  0x2084=$(rd 3 41 0x2084)"
}
echo "##### 系统时间 $(date +%H:%M:%S)  uptime: $(uptime | tr -s ' ' | cut -d, -f1) #####"
echo
echo "===== 1. 冷态 (未播放任何音频) ====="
show
echo
for k in 1 2 3; do
  echo "----- 冷态手工上电尝试 #$k -----"
  pup; wr 0x2014 0x1
  for i in 1 2 3; do printf "   t=%d00ms PWR1=%s RAW1=%s\n" $i "$(rd 3 41 0x2014)" "$(rd 3 41 0x10090)"; sleep 0.1; done
  echo "   -> 冷态#$k 结果 RAW1: $(rd 3 41 0x10090)   (bit24=PUP_DONE)"
  wr 0x2014 0x0
  sleep 0.2
done
echo
echo "===== 2. 触发一次真实播放（让 ASoC 走完整流程）====="
timeout 12 speaker-test -c 2 -t sine -f 440 -l 1 >/dev/null 2>&1
echo "   Enable 失败数: $(sudo dmesg | grep -c 'Enable(1) failed')"
echo "   Firmware status 相关: $(sudo dmesg | grep -c 'Firmware status is invalid')"
sleep 0.5
echo
echo "===== 3. 热态手工上电尝试 #2 ====="
show
echo "----- 热态手工上电 -----"
pup; wr 0x2014 0x1
for i in 1 2 3; do printf "   t=%d00ms PWR1=%s RAW1=%s\n" $i "$(rd 3 41 0x2014)" "$(rd 3 41 0x10090)"; sleep 0.1; done
wr 0x2014 0x0
echo "   -> 热态结果: $(rd 3 41 0x10090)"
echo
echo "===== 4. dmesg 关键行 ====="
sudo dmesg | grep -iE "cs35l41|DSP1|Firmware status|Enable\(1\)|ASoC:" | tail -30
