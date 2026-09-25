#!/bin/sh
get(){ amixer -c0 controls 2>/dev/null | grep "name='TLH DSP1 Protection cd $1'" | grep -oE "numid=[0-9]+" | cut -d= -f2 | head -1; }
show(){
  echo "  -- TLH 保护算法 (播放中) --"
  for c in CSPL_STATE ATTENUATION REDUCE_POWER SPK_OUTPUT_POWER CAL_R CAL_R_SELECTED CAL_AMBIENT CAL_STATUS; do
    n=$(get $c); [ -z "$n" ] && continue
    printf "    %-18s = %s\n" "$c" "$(amixer -c0 cget numid=$n 2>/dev/null | sed -n '3p' | tr -d ' ')"
  done
}
( sleep 3; show ) &
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 pw-play /var/tmp/test5s.wav 2>&1 | tail -1
wait
