#!/bin/sh
# elish CS35L41: 强制 8 颗放大器 runtime PM 常开(power/control=on)
#
# 背景(2026-09-22 实测): 驱动 runtime PM 会把放大器掉电, 之后再上电时时序不满足,
# 播放开始驱动报 "ASoC: PRE_PMU: xxx Main AMP event failed: -110" (ETIMEDOUT),
# 放大器实际没工作 ⇒ i2c 读寄存器全 ERR、声音很小。
# 置 power/control=on 后实测: 新增 PRE_PMU 失败 = 0, 8 颗全部 active, 寄存器可读且与
# 原厂 Android 播放态一致。
for b in 1 3; do
    for a in 40 41 42 43; do
        d=/sys/bus/i2c/devices/$b-00$a
        [ -e "$d/power/control" ] && echo on > "$d/power/control" 2>/dev/null
    done
done
exit 0
