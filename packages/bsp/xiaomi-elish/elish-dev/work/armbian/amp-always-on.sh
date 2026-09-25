#!/bin/sh
# elish CS35L41: keep the 8 amps powered + make the speaker TDM route persistent.
#
# 1) amps: 强制 runtime PM 常开，避免掉电后再上电时序不满足导致
#    "ASoC: PRE_PMU: ... Main AMP event failed: -110"
# 2) FE->BE 路由【第 74 轮实测新增】: 'TERT_TDM_RX_0 Audio Mixer MultiMedia1..8'
#    (numid 452..459) 开机默认是 **off**，且此前**没有任何脚本设置它** ⇒
#    桌面/PipeWire 播放时数据到不了 TDM 后端，表现为"完全没声"。
#    这里在开机时置 on（等 card0 出现，最多约 20 秒）。
for b in 1 3; do
    for a in 40 41 42 43; do
        d=/sys/bus/i2c/devices/$b-00$a
        [ -e "$d/power/control" ] && echo on > "$d/power/control" 2>/dev/null
    done
done

i=0
while [ $i -lt 40 ]; do
    [ -e /proc/asound/card0 ] && break
    i=$((i + 1))
    sleep 0.5
done

for n in 452 453 454 455 456 457 458 459; do
    amixer -c 0 cset numid=$n 1 >/dev/null 2>&1
done

# 3) 采集(TX)方向同理【第 77 轮实测新增】: 'MultiMedia1 Mixer TERT_TDM_TX_0..7'
#    = numid 752..759，开机默认也是 off。播放不受影响，但**录音/采集**会完全失败
#    （实测 arecord 直接 rc=1、连文件都不产生）。这里一并置 on，
#    为后续"用平板自身麦克风做客观测量"扫清路由侧的障碍。
for n in 752 753 754 755 756 757 758 759; do
    amixer -c 0 cset numid=$n 1 >/dev/null 2>&1
done

logger -t amp-always-on "power/control=on + TERT_TDM_RX_0(452-459) + TERT_TDM_TX_0(752-759) routes on (card0 waited ${i}x0.5s)"
exit 0
