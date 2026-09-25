#!/bin/sh
# elish 完整充电管线部署（刷完 boot_b_chg6.img 后执行）
set -e
DEV=root@10.0.0.192
MOD=/lib/modules/6.12.58-current-sm8250/kernel/drivers/power/supply
CH=/home/axis/axis_rnd/work/kernel/elish_chg
KT=/home/axis/axis_rnd/work/kernel/build/linux-6.12.58

echo "=== 1. 部署守护进程 + 模块到 rootfs ==="
scp -o StrictHostKeyChecking=no $CH/elish-charged $DEV:/usr/local/sbin/elish-charged
scp -o StrictHostKeyChecking=no $CH/elish-fc2.service $DEV:/etc/systemd/system/elish-charged.service
scp -o StrictHostKeyChecking=no $CH/bq2597x_elish.ko $DEV:$MOD/bq2597x_elish.ko
scp -o StrictHostKeyChecking=no $KT/drivers/power/supply/qcom_pm8150b_charger.ko $DEV:$MOD/qcom_pm8150b_charger.ko
ssh $DEV 'chmod +x /usr/local/sbin/elish-charged; depmod; systemctl daemon-reload'

echo "=== 2. 泵绑定检查 ==="
ssh $DEV 'ls /sys/bus/i2c/devices/ | grep -E "0065|0066"; ls /sys/bus/i2c/drivers/ 2>/dev/null | grep bq'
ssh $DEV 'for p in /sys/bus/i2c/devices/*-0065 /sys/bus/i2c/devices/*-0066; do [ -d "$p" ] || continue; echo "--- $p"; cat $p/vbus_mv $p/vbat_mv $p/ibus_ma 2>/dev/null; done'
echo "=== 3. 手动测试（前台跑观察日志） ==="
echo "ssh $DEV /usr/local/sbin/elish-charged"
