#!/bin/bash
# 冷启动诊断：抓"首次播放失败"时的 DAPM 事件顺序 + 硬件时间线
h4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
rd(){ sudo i2ctransfer -f -y 3 w4@0x41 $(h4 $1) r4@0x41 2>&1 | tr -d '\n' | tr -s ' '; }

sudo bash -c '
T=/sys/kernel/tracing
echo 1 > $T/events/asoc/snd_soc_dapm_widget_event_start/enable
echo 1 > $T/events/asoc/snd_soc_dapm_widget_event_done/enable
echo > $T/trace
echo 1 > $T/tracing_on
'

sudo dmesg -C
echo "##### 冷态寄存器 (未播放) #####"
echo "  PWR1=$(rd 0x2014) PWR2=$(rd 0x2018) PWR3=$(rd 0x201c) CCM=$(rd 0x2bc1000) MBOX2=$(rd 0x13004)"
echo

# 硬件采样线程
( for i in $(seq 1 120); do
    printf "%s PWR1=%s RAW1=%s PWR2=%s CCM=%s MBOX2=%s\n" "$(date +%S.%3N)" \
      "$(rd 0x2014)" "$(rd 0x10090)" "$(rd 0x2018)" "$(rd 0x2bc1000)" "$(rd 0x13004)"
    sleep 0.02
  done ) > /tmp/fine2.log 2>&1 &
LP=$!

sleep 0.4
echo "##### 触发第 1 次播放 #####"
timeout 12 speaker-test -c 2 -t sine -f 440 -l 1 >/dev/null 2>&1
wait $LP 2>/dev/null

sudo bash -c 'echo 0 > /sys/kernel/tracing/tracing_on'
echo "  Enable 失败数: $(sudo dmesg | grep -c 'Enable(1) failed')"
echo "  'Firmware status is invalid' 次数: $(sudo dmesg | grep -c 'Firmware status is invalid')"
echo
echo "===== DAPM 事件顺序 (TLH) ====="
sudo grep -a "TLH" /sys/kernel/tracing/trace | head -20
echo
echo "===== 硬件时间线 (变化点) ====="
awk 'NR==1{print;prev=$0;next} $0!=prev{print}{prev=$0}' /tmp/fine2.log | head -20
