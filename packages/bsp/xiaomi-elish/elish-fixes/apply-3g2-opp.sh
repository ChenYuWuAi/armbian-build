#!/usr/bin/env bash
#
# 给 Xiaomi Mi Pad 5 Pro (elish / SM8250-AC / 骁龙 870) 补上超大核 3.1872GHz OPP。
#
# 背景：
#   drivers/cpufreq/qcom-cpufreq-hw.c 的 qcom_cpufreq_hw_read_lut() 会读硬件频率 LUT，
#   然后用 DT 的 operating-points-v2 表做交叉校验：DT 里不存在的频点会被标记为
#   CPUFREQ_ENTRY_INVALID 丢弃，并打印
#       cpu cpu7: failed to update OPP for freq=3187200
#   上游 sm8250.dtsi 的 cpu7_opp_table 只写到 2841600（SM8250 / 骁龙865 的档位），
#   所以 870 的第 21 档 3.1872GHz 在启动时被丢掉，policy7 最高只能到 2.84GHz。
#
# 本脚本做的事：
#   1. 备份并用本目录下打过补丁的 DTB 覆盖系统 DTB（两个位置）；
#   2. 调用内核安装钩子重新生成 ABL boot 镜像并写入 boot_b 分区；
#   3. 需要重启生效。
#
set -euo pipefail

KVER="$(uname -r)"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)"

if [[ $EUID -ne 0 ]]; then
    exec sudo -- "$(readlink -f "$0")" "$@"
fi

model="$(tr -d '\0' < /sys/firmware/devicetree/base/model)"
case "$model" in
    *BOE*)  p=boe  ;;
    *CSOT*) p=csot ;;
    *) echo "未知面板型号: '$model'" >&2; exit 1 ;;
esac
echo "面板型号: $model  ->  $p"

DTB_SRC="$SRC_DIR/sm8250-xiaomi-elish-$p.dtb"
[[ -f "$DTB_SRC" ]] || { echo "缺少 $DTB_SRC" >&2; exit 1; }

installed=0
for dst in "/usr/lib/linux-image-$KVER/qcom/sm8250-xiaomi-elish-$p.dtb" \
           "/boot/dtb-$KVER/qcom/sm8250-xiaomi-elish-$p.dtb"; do
    [[ -f "$dst" ]] || continue
    cp -a "$dst" "$dst.bak-$STAMP"
    install -m 0644 "$DTB_SRC" "$dst"
    echo "已更新 $dst"
    echo "     备份 $dst.bak-$STAMP"
    installed=$((installed + 1))
done
[[ $installed -gt 0 ]] || { echo "没找到目标 DTB，路径假设可能已变" >&2; exit 1; }

# 重新生成 Image.gz+dtb 的 ABL boot 镜像，并 dd 到 boot_b
/etc/kernel/postinst.d/zz-update-abl-kernel "$KVER"

cat <<EOF

完成。重启后验证：
  cat /sys/devices/system/cpu/cpufreq/policy7/scaling_available_frequencies | tr ' ' '\\n' | tail -3
  sudo dmesg | grep 'failed to update OPP'      # 应当没有任何输出

回滚：
  sudo cp -a /usr/lib/linux-image-$KVER/qcom/sm8250-xiaomi-elish-$p.dtb.bak-$STAMP \\
             /usr/lib/linux-image-$KVER/qcom/sm8250-xiaomi-elish-$p.dtb
  sudo /etc/kernel/postinst.d/zz-update-abl-kernel $KVER
  sudo reboot
EOF
