#!/bin/sh
run(){ sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 "$@"; }
DEV=$(run wpctl status 2>/dev/null | grep -oE "[0-9]+\. Built-in Audio" | grep -oE "^[0-9]+" | head -1)
echo "device id = $DEV"
run wpctl inspect "$DEV" 2>&1 | grep -iE "device.profile|api.alsa.card.name" | head -6
for i in 1 2 3 4 5; do
  run wpctl set-profile "$DEV" $i >/dev/null 2>&1
  sleep 1
  if run wpctl status 2>/dev/null | grep -q "Built-in Audio Speaker"; then echo "profile $i 生效"; break; fi
done
echo "--- sinks ---"; run wpctl status 2>/dev/null | sed -n '/Sinks:/,/Sources:/p' | head -6
SINK=$(run wpctl status 2>/dev/null | grep -oE "[0-9]+\. Built-in Audio Speaker playback" | grep -oE "^[0-9]+" | head -1)
echo "sink=$SINK"; [ -n "$SINK" ] && run wpctl set-default "$SINK" && echo "已设为默认"
echo "--- 播放 ---"; run pw-play /var/tmp/test5s.wav 2>&1 | tail -2
echo "TLH: PWR1=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x20 0x14 r4@0x41 2>&1|tr -d '\n') RAW1=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x01 0x00 0x90 r4@0x41 2>&1|tr -d '\n') FMT=$(i2ctransfer -f -y 3 w4@0x41 0x00 0x00 0x48 0x08 r4@0x41 2>&1|tr -d '\n')"
