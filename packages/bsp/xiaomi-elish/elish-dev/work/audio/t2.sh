#!/bin/sh
CNT() { dmesg | grep -c "Enable(1) failed"; }
echo "开机后失败数=$(CNT)"; echo "预热服务: $(systemctl is-active amp-warmup.service)"
echo "== 现在播放 5 秒扫频(测试第一次播放) =="
B=$(CNT)
aplay -q -D plughw:0,0 /var/tmp/test5s.wav 2>&1 | tail -2
A=$(CNT); echo "播放后失败数=$A (新增 $((A-B)))"
echo "重试成功日志数: $(dmesg | grep -c 're-enabled after DSP start')"
dmesg | grep -E "re-enabled|factory calibration|coeff control" | tail -5
echo "TLH: PWR1=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x20 0x14 r4@0x41 2>&1|tr -d '\n'|tr -s ' ') RAW1=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x01 0x00 0x90 r4@0x41 2>&1|tr -d '\n'|tr -s ' ')"
echo "BRL: PWR1=$(i2ctransfer -f -y 1 w4@0x42 0x00 0x00 0x20 0x14 r4@0x42 2>&1|tr -d '\n'|tr -s ' ') RAW1=$(i2ctransfer -f -y 1 w4@0x42 0x00 0x01 0x00 0x90 r4@0x42 2>&1|tr -d '\n'|tr -s ' ')"
