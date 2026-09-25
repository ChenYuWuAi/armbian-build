#!/bin/sh
# playtest.sh [reg] [val] - start VERIFIED playback, then read (and optionally write) registers.
# Verification is mandatory: pcm must reach RUNNING and amp 0x2014 must be 1.
WAV=/root/t.wav
pkill -f "[a]play" 2>/dev/null
sleep 1
amixer -c 0 cset numid=452 1 >/dev/null 2>&1
setsid aplay -D hw:0,0 "$WAV" </dev/null >/tmp/pt_ap.log 2>&1 &
i=0; s=""
while [ $i -lt 24 ]; do
  s=$(sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status 2>/dev/null | head -1)
  [ "$s" = "RUNNING" ] && break
  sleep 0.5; i=$((i + 1))
done
echo "VERIFY pcm=$s waited=${i}x0.5s"
echo "VERIFY ampfix_last=$(journalctl -t amp-fix -n1 --no-pager 2>/dev/null | tail -1 | sed 's/.*amp-fix\[[0-9]*\]: //')"
echo "VERIFY 2014=$(/root/ampreg 1 0x40 0x2014 | cut -d= -f2)"
echo "--- current ---"
echo "2D10 =$(/root/ampreg 1 0x40 0x2D10  | cut -d= -f2)"
echo "10014=$(/root/ampreg 1 0x40 0x10014 | cut -d= -f2)"
echo "10010=$(/root/ampreg 1 0x40 0x10010 | cut -d= -f2)"
if [ -n "$1" ]; then
  echo "--- write $1 = $2 ---"
  /root/ampreg 1 0x40 "$1" "$2" >/dev/null
  sleep 1
  echo "2D10 =$(/root/ampreg 1 0x40 0x2D10  | cut -d= -f2)"
  echo "10014=$(/root/ampreg 1 0x40 0x10014 | cut -d= -f2)"
  echo "10010=$(/root/ampreg 1 0x40 0x10010 | cut -d= -f2)"
fi
