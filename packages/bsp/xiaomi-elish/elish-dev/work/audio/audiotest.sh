#!/bin/sh
PLAY="sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 pw-play"
CNT() { dmesg | grep -c "Enable(1) failed"; }
RD() { i2ctransfer -f -y "$1" w4@0x"$2" 0x00 0x00 0x"$3" 0x"$4" r4@0x"$2" 2>&1 | tr -d '\n' | tr -s ' '; }
echo "起点失败数=$(CNT)"
echo "== 预热1 =="; $PLAY /tmp/silence1s.wav 2>&1 | tail -1; echo "   失败数=$(CNT)"
echo "== 预热2 =="; $PLAY /tmp/silence1s.wav 2>&1 | tail -1; echo "   失败数=$(CNT)"
echo "== 播放 5s 扫频 =="; $PLAY /tmp/test5s.wav 2>&1 | tail -1
echo "   失败数=$(CNT)"
echo "   3-41 SP_FORMAT=$(RD 3 41 48 08)  RX_WL=$(RD 3 41 48 40)  PWR1=$(RD 3 41 20 14)"
