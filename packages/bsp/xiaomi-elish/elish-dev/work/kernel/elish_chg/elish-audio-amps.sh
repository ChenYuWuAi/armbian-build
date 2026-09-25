#!/bin/sh
# elish 音频出厂参数对齐（逆向自 vendor audio_cs35l41.ko，见 AGENT_STATE 159 轮）
# - Digital PCM Volume=18：Android "AMP PCM Gain=18" 同刻度（默认 865 落在 ≥817 负增益区=声音小的根因）
# - B 组 CH_BAL=0x00,0x40,0x00,0x00：Channel Swap On（右声道）；T 组全零（左声道）
# numid 可能随内核配置漂移，按名字查找
amixer -c 0 controls | while IFS=, read -r id rest; do
	name=$(echo "$rest" | sed "s/iface=MIXER,name='\(.*\)'/\1/")
	case "$name" in
	*Digital\ PCM\ Volume)
		amixer -c 0 cset "$id" 18 >/dev/null 2>&1 ;;
	*B[LR][HL]\ DSP1\ Protection\ cd\ CH_BAL)
		amixer -c 0 cset "$id" 0x00,0x40,0x00,0x00 >/dev/null 2>&1 ;;
	*T[LR][HL]\ DSP1\ Protection\ cd\ CH_BAL)
		amixer -c 0 cset "$id" 0x00,0x00,0x00,0x00 >/dev/null 2>&1 ;;
	esac
done
logger -t elish-audio "amp params applied (vol=18, B-group swap on)"
