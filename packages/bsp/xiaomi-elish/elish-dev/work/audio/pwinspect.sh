#!/bin/sh
T="timeout 8"; U="sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000"
( sleep 3
  ID=$($T $U wpctl status 2>/dev/null | sed -n '/Streams:/,$p' | grep -oE "^ *[0-9]+\. pw-play" | grep -oE "[0-9]+" | head -1)
  echo "  stream node id = $ID"
  [ -n "$ID" ] && $T $U wpctl inspect "$ID" 2>/dev/null | grep -iE "^\s*\*\s*(volume|node.volume|audio.channel|node.name|media.name|node.software|channelVolumes)" | head -12
) &
$T $U pw-play /var/tmp/quiet.wav 2>&1|tail -1; wait
