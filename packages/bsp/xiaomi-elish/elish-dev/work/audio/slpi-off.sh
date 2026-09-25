#!/bin/sh
# elish: Armbian 没有传感器栈, SLPI(5c00000) 的 sensor_process PD 初始化超时,
# 看门狗每 ~30s 重启一次 ADSP/SLPI 固件(实测已 75 次)。音频走 AP 侧 LPASS, 不依赖 SLPI
# (实测 stop 后播放仍 RUNNING、负载正常), 故直接停掉它, 消除崩溃循环。
TARGET=5c00000.remoteproc
for r in /sys/class/remoteproc/remoteproc*; do
  [ -e "$r/name" ] || continue
  if [ "$(cat $r/name)" = "$TARGET" ]; then
    echo stop > $r/state 2>/dev/null
    logger -t slpi-off "stopped $(cat $r/name) -> $(cat $r/state)"
    exit 0
  fi
done
exit 0
