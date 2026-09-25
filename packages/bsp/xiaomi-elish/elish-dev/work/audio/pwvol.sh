#!/bin/sh
T="timeout 10"
U="sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000"
( sleep 3
  echo "  -- sink 音量 --"; $T $U wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>&1 | head -2
  echo "  -- 播放中的 stream 列表 --"; $T $U wpctl status 2>/dev/null | sed -n '/Streams:/,/Video/p' | head -8
  echo "  -- wpctl status 全量(前 25 行) --"; $T $U wpctl status 2>/dev/null | head -25
) &
$T $U pw-play /var/tmp/quiet.wav 2>&1 | tail -1
wait
