#!/bin/sh
# elish 音频晨间修正（chg10 刷入后跑）
# 依据：vendor audio_cs35l41.ko 逆向（详见 AGENT_STATE 159 轮）
#   - AMP PCM Gain=18 → Digital PCM Volume (0x6000, 刻度 1231+val, 与主线 SOC_SINGLE_SX_TLV 完全同构)
#   - Channel Swap(B组On) → DSP 算法控件 CH_BAL 写 4 字节 BE 0x00400000 (bit22)
set -e
echo "=== 1. 每颗功放 Digital PCM Volume = 18 (Android AMP PCM Gain 值) ==="
for i in $(seq 0 7); do
	amixer -c 0 cset "numid=$(amixer -c 0 controls | grep "Digital PCM Volume" | sed -n "$((i+1))p" | sed "s/numid=\([0-9]*\).*/\1/")" 18 >/dev/null 2>&1 || true
done
amixer -c 0 controls | grep -c "Digital PCM Volume"
echo "=== 2. CH_BAL 探测（主线 cs_dsp 是否暴露 DSP 算法控件） ==="
amixer -c 0 controls | grep -i "ch_bal" | head -8 || echo "ALSA 中无 CH_BAL（需要内核补丁或 debugfs 路径）"
ls /sys/kernel/debug/cs_dsp 2>/dev/null | head -4
echo "=== 3. 拓扑注册（chg10 内核带 AVS 状态门 + DEREG） ==="
echo 1 > /sys/module/q6core/parameters/elish_topologies
sleep 4
dmesg | grep -E "elish: (AVS state|deregistered|AVCS topologies|register topologies failed|refusing)" | tail -5
echo "=== 4. 若注册成功：启用拓扑 COPP 并播放 ==="
echo 0x1000a100 > /sys/module/q6routing/parameters/copp_topology 2>/dev/null || true
echo "现在: amixer -c 0 cset numid=452 1; setsid aplay -D hw:0,0 /root/t.wav &  然后实听"
