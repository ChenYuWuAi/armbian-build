#!/system/bin/sh
# Play via tinyplay (ADSP streaming) AND manually power up all 8 amps with the
# values observed in the stock "playing" reference (0x2014=1, 0x2018=0x3721).
dmesg -c >/dev/null
nohup tinyplay /sdcard/Download/sine1k.wav -D 0 -d 0 >/data/local/tmp/tp.log 2>&1 &
sleep 2
for b in 1 2; do
  for a in 0x40 0x41 0x42 0x43; do
    /data/local/tmp/ampreg $b $a 0x2014 1
    /data/local/tmp/ampreg $b $a 0x2018 0x3721
  done
done
echo "-- after manual PUP --"
for b in 1 2; do
  for a in 0x40 0x41 0x42 0x43; do
    /data/local/tmp/ampreg $b $a dump 0x00002014 0x00002018 0x00002084
  done
done
echo "-- tail tglog --"
tail -3 /data/local/tmp/tp.log
