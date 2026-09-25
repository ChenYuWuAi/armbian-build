#!/bin/bash
# elish (Xiaomi Pad 5 Pro) Armbian —— 启动 USB gadget 网络
# 目的:让 PC 通过 Type-C 线 SSH 到平板 (平板 172.16.42.1 / PC 172.16.42.2)
# 用法:sudo bash elish_start_usbgadget.sh
set -u
echo "===== 1) USB 角色必须为 device ====="
MODE=/sys/kernel/debug/usb/a600000.usb/mode
if [ -e "$MODE" ]; then
    echo device > "$MODE" && echo "  已切到 device 模式(host 模式下 gadget 无法工作)"
    sleep 1
else
    echo "  未找到 $MODE(可能 debugfs 未挂载,继续)"
fi

echo "===== 2) 加载 NCM 功能模块 ====="
# 注意:服务名叫 usbgadget-rndis,但脚本实际用的是 NCM,不是 RNDIS
for m in usb_f_ncm libcomposite; do
    modprobe "$m" 2>/dev/null && echo "  modprobe $m ok" || echo "  modprobe $m 失败/已内置"
done

echo "===== 3) 挂载 configfs ====="
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
echo "  configfs: $(mountpoint -q /sys/kernel/config && echo 已挂载 || echo 未挂载)"

echo "===== 4) 启动 gadget ====="
if [ -e /sys/kernel/config/usb_gadget/g1 ]; then
    echo "  g1 已存在,先停掉旧实例"
    /usr/local/bin/remove-usbgadget-network.sh 2>/dev/null
    sleep 1
fi
/usr/local/bin/setup-usbgadget-network.sh

echo
echo "===== 自检 ====="
echo "-- UDC(必须非空,gadget 才能激活):"
ls /sys/class/udc 2>/dev/null || echo "  无 /sys/class/udc"
echo "-- usb0 地址(应为 172.16.42.1/16):"
ip -4 addr show usb0 2>/dev/null || echo "  没有 usb0 接口"
echo "-- unudhcpd(DHCP 服务器,本镜像里缺失 → PC 需手动设 172.16.42.2):"
pgrep -a unudhcpd || echo "  未运行(镜像内无 /usr/bin/unudhcpd,属正常)"
echo "-- 已激活的 gadget:"
cat /sys/kernel/config/usb_gadget/g1/UDC 2>/dev/null || true
echo
echo "完成。PC 侧执行(管理员 PowerShell):"
echo '  Get-NetAdapter | Where-Object InterfaceDescription -like "*NCM*"'
echo '  New-NetIPAddress -IPAddress 172.16.42.2 -PrefixLength 16 -InterfaceAlias "<上面的网卡名>"'
echo "然后:ssh root@172.16.42.1   (默认密码 1234)"
