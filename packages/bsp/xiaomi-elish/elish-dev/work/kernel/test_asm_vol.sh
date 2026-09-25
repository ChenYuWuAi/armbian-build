#!/bin/bash
# ADSP ASM 流音量验证（模块编译 → 加载 → 0dB 试听 → 双向扫描）
# 用法: bash test_asm_vol.sh
set -u
A=root@172.16.42.1
M=/home/axis/axis_rnd/work/kernel/elish_adsp_vol
WAV=/home/axis/26913a7d7e1b8b814aaecfa950acef33.wav
S(){ timeout 200 ssh -o StrictHostKeyChecking=no $A "$1" 2>&1; }

echo "=== 1) 推送并编译 ==="
timeout 120 scp -q -o StrictHostKeyChecking=no $M/elish_adsp_vol.c $M/Makefile $A:/root/elish_adsp_vol/ && echo pushed
S 'cd /root/elish_adsp_vol && make -C /lib/modules/$(uname -r)/build M=/root/elish_adsp_vol modules 2>&1 | grep -E "error:|\.ko" | tail -4'

echo "=== 2) 起播放 + 加载模块 ==="
S "pkill -x pw-play; sleep 1; (su - axis -c \"XDG_RUNTIME_DIR=/run/user/1000 pw-play $WAV\" >/dev/null 2>&1 &); sleep 3; head -1 /proc/asound/card0/pcm0p/sub0/status"
S 'rmmod elish_adsp_vol 2>/dev/null; dmesg -c >/dev/null; insmod /root/elish_adsp_vol/elish_adsp_vol.ko use_q6adm_open=1 bit_width=24 channels=2 rate=48000; sleep 1; dmesg | tail -6'

echo "=== 3) ASM 音量 0 dB（0x2000）保持 8s —— 听响度是否补回 ==="
S 'ls /sys/kernel/elish_adsp_vol/; echo 0x2000 > /sys/kernel/elish_adsp_vol/gain_asm_q13; sleep 8; (cat /sys/kernel/elish_adsp_vol/asm 2>/dev/null || cat /sys/kernel/elish_adsp_vol/copp)'

echo "=== 4) -6 dB（0x1000）8s ==="
S 'echo 0x1000 > /sys/kernel/elish_adsp_vol/gain_asm_q13; sleep 8'

echo "=== 5) -12 dB（0x0800）8s ==="
S 'echo 0x0800 > /sys/kernel/elish_adsp_vol/gain_asm_q13; sleep 8'

echo "=== 6) 恢复 0 dB + 发送记录 ==="
S 'echo 0x2000 > /sys/kernel/elish_adsp_vol/gain_asm_q13; sleep 1; dmesg | grep -iE "elish_adsp_vol" | tail -10'

echo "=== 7) 收尾：卸载模块、重启播放（保证恢复声音）==="
S "rmmod elish_adsp_vol 2>/dev/null; pkill -x pw-play; sleep 1; (su - axis -c \"XDG_RUNTIME_DIR=/run/user/1000 pw-play $WAV\" >/dev/null 2>&1 &); sleep 2; head -1 /proc/asound/card0/pcm0p/sub0/status"
