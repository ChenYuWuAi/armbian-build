#!/bin/sh
# elish M2 刷机后自动验证 + 监控（在设备上跑）
# 用法: verify-m2.sh install   # 从 /root/elish_chg_stage 安装并启动服务
#       verify-m2.sh monitor   # 1Hz 采样 CSV（stdout）Ctrl-C 停
#       verify-m2.sh pumps     # 只读泵 ADC/寄存器（安全，不开泵）
set -e
STAGE=/root/elish_chg_stage
MOD=/lib/modules/6.12.58-current-sm8250/kernel/drivers/power/supply
PS=/sys/class/power_supply

case "$1" in
install)
	echo "=== 安装模块 + 守护 ==="
	cp $STAGE/bq2597x_elish.ko $MOD/
	cp $STAGE/qcom_pm8150b_charger.ko $MOD/
	cp $STAGE/elish-charged /usr/local/sbin/
	chmod +x /usr/local/sbin/elish-charged
	cp $STAGE/elish-charged.service /etc/systemd/system/
	depmod
	systemctl daemon-reload
	echo "=== 泵绑定检查 ==="
	ls /sys/bus/i2c/devices/ | grep -E "0065|0066" || echo "!! 泵未出现在 i2c 总线"
	ls /sys/bus/i2c/drivers/ | grep -i bq || echo "!! bq2597x 驱动未注册（检查模块/modalias）"
	for p in /sys/bus/i2c/devices/*-0065 /sys/bus/i2c/devices/*-0066; do
		[ -d "$p" ] || continue
		echo "--- $p (绑定前手动 modprobe)"
		modprobe bq2597x_elish 2>/dev/null || insmod $MOD/bq2597x_elish.ko 2>/dev/null || true
		ls $p/ 2>/dev/null | head -8
	done
	dmesg | grep -iE "bq2597|charge pump" | tail -5
	echo "=== 启动守护（服务） ==="
	systemctl enable --now elish-charged
	sleep 2
	systemctl status elish-charged --no-pager | head -6
	echo "=== 完成。跑 verify-m2.sh monitor 观察 ==="
	;;
pumps)
	for p in /sys/bus/i2c/devices/*-0065 /sys/bus/i2c/devices/*-0066; do
		[ -d "$p" ] || continue
		echo "--- $p"
		echo "charge_enabled=$(cat $p/charge_enabled 2>/dev/null)"
		echo "vbus_mv=$(cat $p/vbus_mv 2>/dev/null) vbat_mv=$(cat $p/vbat_mv 2>/dev/null) ibus_ma=$(cat $p/ibus_ma 2>/dev/null) vac_mv=$(cat $p/vac_mv 2>/dev/null)"
	done
	echo "--- 寄存器（master） ---"
	cat /sys/bus/i2c/devices/*-0065/regs 2>/dev/null | head -40
	;;
monitor)
	echo "time,t_ddc,cell0_mv,cell1_mv,ibat_ma,cap,tcpm_online,p_vbus,p_vbat,p_ibus_m,p_ibus_s,sw_cc,sw_fv,sw_en"
	while true; do
		T=$(cat $PS/bq27z561-0/temp 2>/dev/null)
		V0=$(cat $PS/bq27z561-0/voltage_now 2>/dev/null)
		V1=$(cat $PS/bq27z561-1/voltage_now 2>/dev/null)
		I0=$(cat $PS/bq27z561-0/current_now 2>/dev/null)
		I1=$(cat $PS/bq27z561-1/current_now 2>/dev/null)
		C=$(cat $PS/bq27z561-0/capacity 2>/dev/null)
		ON=$(cat $PS/tcpm-source-psy-*/online 2>/dev/null)
		PB=$(cat /sys/bus/i2c/devices/*-0065/vbus_mv 2>/dev/null)
		PV=$(cat /sys/bus/i2c/devices/*-0065/vbat_mv 2>/dev/null)
		PI=$(cat /sys/bus/i2c/devices/*-0065/ibus_ma 2>/dev/null)
		SI=$(cat /sys/bus/i2c/devices/*-0066/ibus_ma 2>/dev/null)
		CC=$(cat $PS/pm8150b-charger/charge_current 2>/dev/null)
		FV=$(cat $PS/pm8150b-charger/float_voltage 2>/dev/null)
		SE=$(cat $PS/pm8150b-charger/charge_enabled 2>/dev/null)
		echo "$(date +%H:%M:%S),$T,$((V0/1000)),$((V1/1000)),$(( (I0+I1)/1000 )),$C,$ON,$PB,$PV,$PI,$SI,$CC,$FV,$SE"
		sleep 1
	done
	;;
*)
	echo "用法: verify-m2.sh install|monitor|pumps"
	;;
esac
