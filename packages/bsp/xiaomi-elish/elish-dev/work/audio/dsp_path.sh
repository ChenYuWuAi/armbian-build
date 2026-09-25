#!/bin/sh
H4(){ printf '0x%02x 0x%02x 0x%02x 0x%02x' $((($1>>24)&255)) $((($1>>16)&255)) $((($1>>8)&255)) $(($1&255)); }
W(){ i2ctransfer -f -y $1 w8@0x$2 $(H4 $3) $(H4 $4) >/dev/null 2>&1; }
R(){ i2ctransfer -f -y $1 w4@0x$2 $(H4 $3) r4@0x$2 2>/dev/null | tr -d '\n'; }
show(){ for r in "4c00:DAC_PCM1_SRC" "4c20:ASP_TX1_SRC" "4c24:ASP_TX2_SRC" "4c28:ASP_TX3_SRC" "4c2c:ASP_TX4_SRC" "4c40:DSP1_RX1_SRC" "4c44:DSP1_RX2_SRC" "6000:AMP_DIG_VOL" "6c04:AMP_GAIN"; do
  a=${r%%:*}; n=${r##*:}; printf "    %-16s = %s\n" "$n" "$(R 3 41 0x$a)"; done; }
PS=$(amixer -c0 controls 2>/dev/null | grep "name='TLH PCM Source'" | grep -oE "numid=[0-9]+" | cut -d= -f2)
echo "  TLH PCM Source 控件: $(amixer -c0 cget numid=$PS 2>/dev/null | sed -n '3p' | tr -d ' ')"
( sleep 3; echo "== 修复前(播放中) =="; show ) &
timeout 6 sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 pw-play /var/tmp/quiet.wav 2>/dev/null; wait
echo "== 写入 Android 的通路值 =="
for b in 1 3; do for a in 40 41 42 43; do
  W $b $a 0x4c00 0x00000000; W $b $a 0x4c20 0x00000032; W $b $a 0x4c24 0x00000000
  W $b $a 0x4c28 0x00000020; W $b $a 0x4c2c 0x00000021
  W $b $a 0x4c40 0x00000008; W $b $a 0x4c44 0x00000009
done; done
echo "== 修复后(播放中) =="
( sleep 3; show ) &
timeout 20 sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 pw-play /home/axis/26913a7d7e1b8b814aaecfa950acef33.wav 2>/dev/null; wait
