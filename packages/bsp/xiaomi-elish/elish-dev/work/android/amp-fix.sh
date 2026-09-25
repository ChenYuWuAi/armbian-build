#!/bin/sh
# elish CS35L41:
#  - 播放开始时手工上电 kick(驱动自动序列在本机经常 -110)
#  - ASP: RX slot 固定 32bit(与 8x32bit TDM 一致), RX word length 按「当前真实 PCM 格式」设
H4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
W(){ i2ctransfer -f -y $1 w8@0x$2 $(H4 $3) $(H4 $4) >/dev/null 2>&1; }
R(){ i2ctransfer -f -y $1 w4@0x$2 $(H4 $3) r4@0x$2 2>/dev/null; }
ASP(){
  # 让音频走 DSP 内部通路(Android: PCM Source=DSP), 否则是 ASP 直通、丢掉小米调音的增益/EQ 和保护
  # 其余控件按两份权威来源逐条对齐:
  #   Android static mixer (mixer_paths_overlay_static.xml) 与 postmarketOS UCM
  #   (alsa-ucm-conf-xiaomi-elish 1.0-r1, ucm2/Xiaomi/elish/HiFi.conf)
  # 注意: pmOS UCM 写的 'DSP RX2 Source' ASPTX2 在本内核枚举里不存在(0=Zero,1=ASPRX1,2=ASPRX2,
  #       3..6=mon,7=DSPTX1,8=DSPTX2), cset 会静默失败; 按 Android 取 ASPRX2。
  for nm in TLH TRH TLL TRL BLH BRH BLL BRL; do
    amixer -c0 cset "name=$nm PCM Source" DSP >/dev/null 2>&1
    amixer -c0 cset "name=$nm DSP1 Preload Switch" 1 >/dev/null 2>&1
    amixer -c0 cset "name=$nm DRE Switch" 1 >/dev/null 2>&1
    amixer -c0 cset "name=$nm PCM Soft Ramp" 4ms >/dev/null 2>&1
    amixer -c0 cset "name=$nm Analog PCM Volume" 18 >/dev/null 2>&1
    # 数字音量: 控件范围 0..913, 817=0dB(驱动默认), 865=+6dB, 913=+12dB(上限)
    # 【2026-09-22 实测结论】原厂 Android 寄存器 0x8004 = 0, 但原厂同时由 HAL 通过 ADSP 专有接口
    # (App Type Cfg / ADM ChMixer) 下发校准增益; mainline 没有这些接口(q6asm/q6adm 无此 mixer 控件,
    # 控件数 1688 vs Android 6019), 所以 Armbian 侧整条链路的电平偏低(听感"很小")。
    # 软件域加增益会在源已满刻度时削顶(实测 +6dB 软件增益 = 变响但失真), 因此把补偿加在
    # 放大器内部 DSP(有内部余量 + 保护算法限幅)。865 = +6dB 为起点, 可用 MGAIN 调。
    amixer -c0 cset "name=$nm Digital PCM Volume" "${MGAIN:-865}" >/dev/null 2>&1
    amixer -c0 cset "name=$nm ASP TX1 Source" DSPTX1 >/dev/null 2>&1
    amixer -c0 cset "name=$nm DSP RX1 Source" ASPRX1 >/dev/null 2>&1
    amixer -c0 cset "name=$nm DSP RX2 Source" ASPRX2 >/dev/null 2>&1
  done
  f=$(sed -n 's/^format: *//p' /proc/asound/card0/pcm0p/sub0/hw_params 2>/dev/null | head -1)
  case "$f" in S24*) wl=24;; S32*) wl=32;; S16*) wl=16;; *) wl=16;; esac
  for b in 1 3; do for a in 40 41 42 43; do
    # 【2026-09-22 用 Magisk root 实测原厂 Android 播放态】0x00004808 = 0x20200000
    #   ⇒ RX/TX slot 宽度均为 32bit。注意 slot 字段在 bit[23:16], 不是最高字节!
    #   旧代码写 (0x20<<24)|保留byte1 属空操作(最高字节原本就是 0x20), 所以一直没修好,
    #   放大器仍按 24bit 采样 8x32bit 的 TDM 槽 ⇒ 声音很小/失真。直接写原厂值。
    W $b $a 0x00004808 0x20200000
    W $b $a 0x00004840 "$wl"           # RX word length = 实际位宽(原厂 S24 时为 0x18=24 ✓)
    # 模拟音量对齐原厂: Android 播放态实测 0x6c04 = 0x253(595), 而控件 18 只给到 0x240(576),
    # 按寄存器 1/8 dB 计原厂比我们高 ~2.4 dB, 直接写寄存器补齐(控件上限 20 也够不到)
    W $b $a 0x00006c04 0x00000253
  done; done
  logger -t amp-fix "asp: pcm=$f wl=$wl"
}
KICK(){ for b in 1 3; do for a in 40 41 42 43; do
    # 【2026-09-22】强制放大器常供电: 否则驱动 runtime PM 掉电后再上电时序不满足,
    # 播放时驱动报 "ASoC: PRE_PMU: ... Main AMP event failed: -110"(ETIMEDOUT, 实测累计 40 次),
    # 放大器实际没起来 ⇒ i2c 读全 ERR、声音很小。置 on 后实测新增失败 = 0 ✓
    echo on > /sys/bus/i2c/devices/$b-00$a/power/control 2>/dev/null
    W $b $a 0x00000040 0x00000055; W $b $a 0x00000040 0x000000AA
    W $b $a 0x00002084 0x002F1AA0; W $b $a 0x00000040 0x000000CC; W $b $a 0x00000040 0x00000033
    W $b $a 0x00002018 0x00003721; W $b $a 0x00002014 0x00000001
    W $b $a 0x00006808 0x00003F75   # NG_CFG = Android(16245)
  done; done; [ -f /root/xm.on ] && XM; ASP; }
# Android vendor 驱动播放态写的 17 个运行期寄存器
# 来源: work/hal/audio_cs35l41.ko .rodata 表(file 0x15100 起, 17 组 (reg,val)), 顺序照抄
# 必须在上电态(PWR_CTRL1=1)写, 断电态写不进去(实测 0x4000 会回到 0x08000800)
XM(){ for b in 1 3; do for a in 40 41 42 43; do
    W $b $a 0x00002030 0x1;     W $b $a 0x0000208c 0x2
    W $b $a 0x0000300c 0x1;     W $b $a 0x0000394c 0x5
    W $b $a 0x0000416c 0x1;     W $b $a 0x00004160 0x1
    W $b $a 0x00004170 0x1;     W $b $a 0x00004360 0x1
    W $b $a 0x00004448 0x2;     W $b $a 0x00006e30 0xe
    W $b $a 0x00007418 0x2;     W $b $a 0x00007434 0x1
    W $b $a 0x00007068 0x1;     W $b $a 0x0000410c 0x1
    W $b $a 0x0000400c 0x1;     W $b $a 0x00004000 0x1
    W $b $a 0x00017040 0x2
  done; done; }
ST=/proc/asound/card0/pcm0p/sub0/status
last=closed; re=0
while :; do
  s=$(sed -n 's/^state: *//p' "$ST" 2>/dev/null | head -1); [ -z "$s" ] && s=closed
  if [ "$s" = "RUNNING" ]; then
    if [ "$last" != "RUNNING" ]; then KICK; re=1
    elif [ "$re" = "1" ]; then sleep 0.4; ASP; re=0; fi
  fi
  last=$s; sleep 0.3
done
