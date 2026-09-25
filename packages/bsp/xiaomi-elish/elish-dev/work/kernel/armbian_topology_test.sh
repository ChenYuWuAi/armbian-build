#!/bin/bash
# Armbian 侧：判定 ADSP 是否认识 Android 的扬声器 COPP topology 0x1000a100
# 用法（在 WSL/host 上跑）: bash armbian_topology_test.sh
#
# 背景：mainline q6routing.c:393 硬编码 NULL_COPP_TOPOLOGY(0x10312)，
# 而 Android 用 0x1000a100 打开扬声器 COPP。本脚本用已验证的导出符号
# q6adm_open() 直接尝试用 0x1000a100 开一个 COPP：
#   * 成功 -> ADSP 固件认识该 topology，内核打 0052 补丁并设为 0x1000a100 即可
#   * 失败 -> 必须先下发 ADM_CMD_ADD_TOPOLOGIES(0x00010335) 再开 COPP
set -u
A=root@172.16.42.1
MOD=/home/axis/axis_rnd/work/kernel/elish_adsp_vol
S(){ timeout 240 ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 $A "$1" 2>&1; }

echo "=== 0) 设备确认 ==="
S 'uname -r; cat /proc/device-tree/model 2>/dev/null; echo; echo "SLPI:"; cat /sys/class/remoteproc/remoteproc*/name 2>/dev/null | head -3'

echo "=== 1) 推送模块源码并在设备上编译 ==="
timeout 120 scp -q -o StrictHostKeyChecking=no "$MOD/elish_adsp_vol.c" "$MOD/Makefile" $A:/root/elish_adsp_vol/ && echo pushed
S 'cd /root/elish_adsp_vol && make -C /lib/modules/$(uname -r)/build M=/root/elish_adsp_vol modules 2>&1 | grep -E "error:|warning:|\.ko" | tail -8'
S 'ls -la /root/elish_adsp_vol/elish_adsp_vol.ko'

echo "=== 2) 确保 SLPI/ADSP 在线（历史坑：slpi-off.service）==="
S 'systemctl is-active slpi-off.service 2>/dev/null; for f in /sys/class/remoteproc/remoteproc*/state; do echo start > $f 2>/dev/null; done; sleep 2; for f in /sys/class/remoteproc/remoteproc*/state; do echo -n "$f="; cat $f; done'

echo "=== 3) ★ 判定实验：用 topology 0x1000a100 开 COPP ==="
S 'rmmod elish_adsp_vol 2>/dev/null; dmesg -c >/dev/null
   insmod /root/elish_adsp_vol/elish_adsp_vol.ko use_q6adm_open=1 copp_topology=0x1000a100 bit_width=24 channels=2 rate=48000
   echo "insmod_rc=$?"; sleep 1
   dmesg | grep -iE "elish_adsp_vol|q6adm|copp" | tail -12'

echo "=== 4) 对照：上游行为（NULL topology 0x10312）==="
S 'rmmod elish_adsp_vol 2>/dev/null; dmesg -c >/dev/null
   insmod /root/elish_adsp_vol/elish_adsp_vol.ko use_q6adm_open=1 copp_topology=0x00010312
   echo "insmod_rc=$?"; sleep 1
   dmesg | grep -iE "elish_adsp_vol|q6adm|copp" | tail -8'

echo "=== 5) 收尾（卸载，恢复干净状态）==="
S 'rmmod elish_adsp_vol 2>/dev/null; echo "--- sysfs ---"; ls /sys/module/elish_adsp_vol/parameters/ 2>/dev/null | head -20; echo "--- q6routing param (0052 补丁装好后才有) ---"; cat /sys/module/q6routing/parameters/copp_topology 2>/dev/null || echo "q6routing param 不存在（当前内核未打 0052）"'

echo
echo "=== 判读 ==="
echo "3) 成功 -> ADSP 认识 0x1000a100，装 0052 内核 + echo 0x1000a100 即可"
echo "3) 失败 -> 需先 ADM_CMD_ADD_TOPOLOGIES；4) 应与 3) 对照看 NULL 是否成功"
