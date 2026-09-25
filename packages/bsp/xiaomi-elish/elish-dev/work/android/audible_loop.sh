#!/system/bin/sh
# Repeated audible test: 3 rounds of nice.wav, re-asserting AMP Enable each round
# (the cs35l41 driver resets the amps on every new PCM stream).
for i in 1 2 3; do
  tinyplay /sdcard/Download/nice.wav -D 0 -d 0 >/data/local/tmp/tp_loop.log 2>&1 &
  sleep 2
  for b in 1 2; do
    for a in 0x40 0x41 0x42 0x43; do
      /data/local/tmp/ampreg $b $a 0x2014 1
      /data/local/tmp/ampreg $b $a 0x2018 0x3721
    done
  done
  wait
done
echo "loop finished"
