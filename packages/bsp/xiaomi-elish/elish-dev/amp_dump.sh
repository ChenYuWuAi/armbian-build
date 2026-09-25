#!/bin/sh
# amp_dump.sh — 抓取 8 个 CS35L41 的完整寄存器 + DSP 校准系数快照
#
# 用法:
#   Android (slot b):  adb push amp_dump.sh /data/local/tmp/ && adb shell su -c 'sh /data/local/tmp/amp_dump.sh' > android_dump.txt
#   Armbian  (slot a): sudo sh amp_dump.sh > armbian_dump.txt
# 然后:  python3 amp_diff.py android_dump.txt armbian_dump.txt
#
# 目的: 找出「Android 响度正常 / mainline 声音小」之间的寄存器级差异。

AMPS="1:40 1:41 1:42 1:43 3:40 3:41 3:42 3:43"

# 关注的寄存器 (名称:地址) —— 覆盖功放配置/供电/增益/升压/ASP/DSP
REGS="DEVID:0x0 REVID:0x4 OTPID:0x10 SFT_RESET:0x20 \
PWR_CTRL1:0x2014 PWR_CTRL2:0x2018 PWR_CTRL3:0x201c CTRL_OVRRIDE:0x2020 \
AMP_OUT_MUTE:0x2024 PROTECT_REL_ERR_IGN:0x2034 GPIO_PAD_CONTROL:0x242c \
PLL_CLK_CTRL:0x2c04 DSP_CLK_CTRL:0x2c08 GLOBAL_CLK_CTRL:0x2c0c \
BSTCVRT_VCTRL1:0x3800 BSTCVRT_VCTRL2:0x3804 BSTCVRT_PEAK_CUR:0x3808 \
BSTCVRT_SFT_RAMP:0x380c BSTCVRT_COEFF:0x3810 BSTCVRT_SLOPE_LBST:0x3814 \
BSTCVRT_SW_FREQ:0x3818 BSTCVRT_DCM_CTRL:0x381c BSTCVRT_DCM_MODE_FORCE:0x3820 \
BSTCVRT_OVERVOLT_CTRL:0x3830 DTEMP_WARN_THLD:0x4220 DTEMP_CFG:0x4224 DTEMP_EN:0x4308 \
SP_ENABLES:0x4800 SP_RATE_CTRL:0x4804 SP_FORMAT:0x4808 SP_HIZ_CTRL:0x480c \
SP_FRAME_TX_SLOT:0x4810 SP_FRAME_RX_SLOT:0x4820 SP_TX_WL:0x4830 SP_RX_WL:0x4840 \
DAC_PCM1_SRC:0x4c00 ASP_TX1_SRC:0x4c20 ASP_TX2_SRC:0x4c24 ASP_TX3_SRC:0x4c28 ASP_TX4_SRC:0x4c2c \
DSP1_RX1_SRC:0x4c40 DSP1_RX2_SRC:0x4c44 DSP1_RX3_SRC:0x4c48 DSP1_RX4_SRC:0x4c4c \
DSP1_RX5_SRC:0x4c50 DSP1_RX6_SRC:0x4c54 DSP1_RX7_SRC:0x4c58 DSP1_RX8_SRC:0x4c5c \
AMP_DIG_VOL_CTRL:0x6000 AMP_GAIN_CTRL:0x6c04 AMP_ERR_VOL:0x6418 \
CLASSH_CFG:0x6800 WKFET_CFG:0x6804 NG_CFG:0x6808 \
IRQ1_STATUS1:0x10010 IRQ1_STATUS4:0x1001c IRQ1_RAW_STATUS1:0x10090 IRQ1_RAW_STATUS4:0x1009c \
IRQ1_MASK1:0x10110 IRQ1_MASK4:0x1011c DSP_MBOX_2:0x13004 \
GPIO1_CTRL1:0x11008 GPIO2_CTRL1:0x1100c GPIO1_CTRL2:0x11010 GPIO2_CTRL2:0x11014 \
DSP1_CCM_CORE_CTRL:0x2bc1000"

# DSP 系数 (cs_dsp control 名后缀)
COEFFS="CAL_R CAL_R_SELECTED CAL_AMBIENT CAL_CHECKSUM CAL_STATUS CAL_SET_STATUS \
CSPL_STATE CSPL_COMMAND CSPL_ENABLE CSPL_ERRORNO UPDATE_PARAMS_CONFIG \
ATTENUATION REDUCE_POWER SPK_OUTPUT_POWER BDLOG_MAX_TEMP BDLOG_MAX_EXC \
ALGO_FRAME_DELAY RATE_NUMBER_BANDS MAX_LRCLK_DELAY LE_FULL_US_BYPASS"

echo "# amp dump  host=$(hostname)  kernel=$(uname -r)  date=$(date -Is 2>/dev/null || date)"
echo

# ---------- 1) 寄存器 ----------
if command -v i2ctransfer >/dev/null 2>&1; then
	# Linux / Armbian: 直接用 i2c-dev（可绕过驱动缓存）
	for a in $AMPS; do
		bus=${a%%:*}; adr=${a##*:}
		echo "== REG bus=$bus addr=0x$adr =="
		for r in $REGS; do
			n=${r%%:*}; off=${r##*:}
			h=$(printf '%08x' $((off)))
			b0=$((0x${h%??????})); rest=${h#??}; b1=$((0x${rest%????})); rest=${rest#??}
			b2=$((0x${rest%??})); b3=$((0x${rest#??}))
			v=$(i2ctransfer -f -y $bus w4@0x$adr $(printf '0x%02x 0x%02x 0x%02x 0x%02x' $b0 $b1 $b2 $b3) r4@0x$adr 2>/dev/null)
			echo "$n $v"
		done
		echo
	done
else
	# Android: 走 debugfs regmap dump（32 位大端已由 regmap 解码为 8 位十六进制文本）
	for a in $AMPS; do
		bus=${a%%:*}; adr=${a##*:}
		f=/sys/kernel/debug/regmap/$bus-00$adr/registers
		echo "== REG bus=$bus addr=0x$adr =="
		if [ -r "$f" ]; then
			for r in $REGS; do
				n=${r%%:*}; off=${r##*:}
				# 文本行格式: "%07x: %08x"
				printf '%s ' "$n"
				grep -i "^$(printf '%07x' $((off))):" "$f" | awk '{print $2}'
			done
		else
			echo "  (no $f — 需要 root 且 regmap debugfs 可用)"
		fi
		echo
	done
fi

# ---------- 2) DSP 系数 ----------
echo "== DSP COEFFS =="
PREFIXES="TRH TLH TRL TLL BRH BLH BRL BLL"
if command -v amixer >/dev/null 2>&1 && amixer -c 0 scontrols >/dev/null 2>&1; then
	for p in $PREFIXES; do
		for c in $COEFFS; do
			v=$(amixer -c 0 cget name="$p DSP1 Protection cd $c" 2>/dev/null | tail -1 | sed 's/.*values=//')
			[ -n "$v" ] && echo "$p.$c $v"
		done
	done
elif command -v tinymix >/dev/null 2>&1; then
	# Android: tinymix 输出 "numid  name  value"
	tinymix 2>/dev/null | grep -E "DSP1 Protection cd" | sed 's/^[0-9]*[[:space:]]*//' | \
		awk -F'\t' '{n=$1; v=$2; gsub(/ /,"",n); gsub(/ /,"",v); print n" "v}'
fi
