#!/bin/bash
# 给 GNOME Shell 的 Vitals 扩展接上 elish 的 2S 电池包聚合节点：
#   1) BATTERY_PATHS 增加 slot 8 -> 'battery'（elish_batt_agg 模块提供的节点）
#   2) 电池区块新增 Temperature / Current 两行
#   3) 设 battery-slot=8 并重载扩展
set -u
EXT="${1:-$HOME/.local/share/gnome-shell/extensions/Vitals@CoreCoding.com}"
DIR="$(cd "$(dirname "$0")" && pwd)"

[ -d "$EXT" ] || { echo "找不到 Vitals 扩展目录: $EXT"; exit 1; }

if grep -q "8: 'battery'" "$EXT/sensors.js"; then
	echo "补丁已存在，跳过打补丁"
else
	cp -a "$EXT/sensors.js" "$EXT/sensors.js.bak-$(date +%m%d-%H%M%S)"
	cp -a "$EXT/prefs.ui"   "$EXT/prefs.ui.bak-$(date +%m%d-%H%M%S)"
	(cd "$EXT" && patch -p1 --forward < "$DIR/vitals-elish-battery.patch")
fi

export GSETTINGS_SCHEMA_DIR="$EXT/schemas"
gsettings set org.gnome.shell.extensions.vitals battery-slot 8 || true

if command -v gnome-extensions >/dev/null; then
	export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
	export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"
	gnome-extensions disable Vitals@CoreCoding.com >/dev/null 2>&1 || true
	sleep 1
	gnome-extensions enable Vitals@CoreCoding.com >/dev/null 2>&1 || true
fi
echo "完成：Vitals 电池区块现在读 /sys/class/power_supply/battery/uevent"
