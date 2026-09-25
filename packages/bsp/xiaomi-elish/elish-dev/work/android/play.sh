#!/system/bin/sh
# play.sh [file] - play a wav on the elish speakers without needing the screen
# unlocked.  Two paths:
#   * default (framework-free): tinyplay on MultiMedia1 + TERT_TDM_RX_0, then
#     power up all 8 CS35L41 amps.  Works headless and is deterministic.
#   * Mi Music (stock HAL/CSPL path): open the file in the player and tap play.
#
# Usage:  play.sh                 -> plays /sdcard/Music/nice.wav
#         play.sh /sdcard/Music/sine1k.wav
F="${1:-/sdcard/Music/nice.wav}"

# 1) FE -> BE route (Android naming: <BACKEND> Audio Mixer <FRONTEND>)
tinymix "TERT_TDM_RX_0 Audio Mixer MultiMedia1" 1 1

# 2) stream
tinyplay "$F" -D 0 -d 0 &

# 3) the HAL normally raises AMP Enable via CSPL; do it by hand.
#    (cs35l41 driver resets the amps on every new PCM stream, so wait a moment)
sleep 2
for b in 1 2; do
  for a in 0x40 0x41 0x42 0x43; do
    /data/local/tmp/ampreg $b $a 0x2014 1
    /data/local/tmp/ampreg $b $a 0x2018 0x3721
  done
done
echo "playing $F"
