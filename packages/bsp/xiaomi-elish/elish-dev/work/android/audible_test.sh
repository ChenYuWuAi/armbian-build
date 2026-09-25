#!/system/bin/sh
# Long audible test: stream the long wav through ADSP and power up all 8 amps.
dmesg -c >/dev/null
nohup tinyplay /sdcard/Download/nice.wav -D 0 -d 0 >/data/local/tmp/tp2.log 2>&1 &
sleep 2
for b in 1 2; do
  for a in 0x40 0x41 0x42 0x43; do
    /data/local/tmp/ampreg $b $a 0x2014 1
    /data/local/tmp/ampreg $b $a 0x2018 0x3721
  done
done
echo "== audible test running (nice.wav) =="
cat /data/local/tmp/tp2.log
