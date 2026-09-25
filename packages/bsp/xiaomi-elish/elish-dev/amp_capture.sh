#!/bin/bash
# 冷启动后捕获：播放前状态 -> 第1次播放(失败) -> 第2次播放(成功)
h4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
rd(){ sudo i2ctransfer -f -y $1 w4@0x$2 $(h4 $3) r4@0x$2 2>&1 | tr -d '\n' | tr -s ' '; }
dump(){
  echo "    PWR_CTRL1=$(rd $1 $2 0x2014)  PWR_CTRL2=$(rd $1 $2 0x2018)  PWR_CTRL3=$(rd $1 $2 0x201c)"
  echo "    0x2084=$(rd $1 $2 0x2084)  MASK1=$(rd $1 $2 0x10110)"
  echo "    RAW1=$(rd $1 $2 0x10090)  ST1=$(rd $1 $2 0x10010)"
  echo "    PAD=$(rd $1 $2 0x242c)  GPIO1=$(rd $1 $2 0x11008)  GPIO2=$(rd $1 $2 0x1100c)"
  echo "    DIGVOL=$(rd $1 $2 0x6000)  AMPGAIN=$(rd $1 $2 0x6c04)  VCTRL1=$(rd $1 $2 0x3800)"
}
echo "##### 系统时间: $(date +%H:%M:%S)  uptime: $(uptime | tr -s ' ' | cut -d, -f1) #####"
echo
echo "===== A. 播放前 (冷态, 没有任何音频播放过) ====="
dump 3 41
echo
echo "===== B. 第 1 次播放 (预期失败) ====="
( sleep 1.0; echo "  [播放中]"; dump 3 41 ) &
S=$!
timeout 12 speaker-test -c 2 -t sine -f 440 -l 1 >/dev/null 2>&1
wait $S 2>/dev/null
sleep 0.3
echo "  [播放结束后]"; dump 3 41
echo "  -> 第1次 Enable 失败数: $(sudo dmesg | grep -c 'Enable(1) failed')"
echo
echo "===== C. 第 2 次播放 (预期成功) ====="
( sleep 1.0; echo "  [播放中]"; dump 3 41 ) &
S=$!
timeout 12 speaker-test -c 2 -t sine -f 440 -l 1 >/dev/null 2>&1
wait $S 2>/dev/null
sleep 0.3
echo "  [播放结束后]"; dump 3 41
echo "  -> 累计 Enable 失败数: $(sudo dmesg | grep -c 'Enable(1) failed')"
echo
echo "===== D. 从 boot 起的音频关键时间线 (本次未清空 dmesg) ====="
sudo dmesg | grep -iE "cs35l41|wm_adsp|cs_dsp|DSP1|ASoC|PLL" | head -70
