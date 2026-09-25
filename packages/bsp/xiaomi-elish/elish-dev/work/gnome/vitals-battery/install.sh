#!/bin/bash
# 给 GNOME Shell 的 Vitals 扩展补上 elish 电池的 Temperature / Current 两行。
#
# 背景：
#  - elish 的整包电池节点由 elish_batt_agg 模块提供，名字是标准的 BAT0，
#    所以 Vitals 原本就能读到 State/Percentage/Voltage/Power Rate，无需改代码。
#  - 但 Vitals 85 的电池区块不读 uevent 里的 TEMP / CURRENT_NOW，
#    本补丁加上 Temperature / Current 两行。
#  - Wayland 会话下 GNOME Shell 不会热重载扩展 JS（disable/enable 无效），
#    打完补丁必须重新登录 / 重启会话才会生效。
set -u
EXT="${1:-$HOME/.local/share/gnome-shell/extensions/Vitals@CoreCoding.com}"
DIR="$(cd "$(dirname "$0")" && pwd)"

[ -d "$EXT" ] || { echo "找不到 Vitals 扩展目录: $EXT"; exit 1; }

if grep -q "elish: 电池温度" "$EXT/sensors.js"; then
	echo "补丁已存在，跳过"
else
	cp -a "$EXT/sensors.js" "$EXT/sensors.js.bak-$(date +%m%d-%H%M%S)"
	(cd "$EXT" && patch -p1 --forward < "$DIR/vitals-elish-battery.patch")
fi

export GSETTINGS_SCHEMA_DIR="$EXT/schemas"
# BAT0 = elish_batt_agg 聚合出的 2S 整包节点，对应 Vitals 下拉第一项
gsettings set org.gnome.shell.extensions.vitals battery-slot 0 || true

echo "完成。注意：需要重新登录（或重启会话）GNOME Shell 才会加载新的扩展代码。"
