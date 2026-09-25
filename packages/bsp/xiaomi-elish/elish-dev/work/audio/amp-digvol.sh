#!/bin/sh
# elish: 设置 8 路功放的数字增益(dB).  0 = Android 默认(817), 范围 -102 .. +12
DB=${1:-0}
V=$(awk -v d="$DB" 'BEGIN{v=817+d*8; if(v<1)v=1; if(v>913)v=913; printf "%d", v}')
for n in $(amixer -c0 controls 2>/dev/null | grep "Digital PCM Volume" | grep -oE "numid=[0-9]+" | cut -d= -f2); do
  amixer -c0 cset numid=$n "$V" >/dev/null 2>&1
done
echo "8 路数字增益 = ${DB} dB (value=$V)   当前: $(amixer -c0 cget numid=1 2>/dev/null | sed -n '3p')"
