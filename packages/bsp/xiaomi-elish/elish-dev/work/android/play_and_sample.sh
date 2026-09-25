#!/system/bin/sh
# Play a wav via MultiMedia1/TERT_TDM_RX_0 and sample amps + kernel log while playing
dmesg -c >/dev/null
nohup tinyplay /sdcard/Download/sine1k.wav -D 0 -d 0 >/data/local/tmp/tp.log 2>&1 &
sleep 3
sh /data/local/tmp/dump_amps.sh > /data/local/tmp/reg_android_play_now.txt
dmesg > /data/local/tmp/dmesg_play.txt
sleep 1
pkill -x tinyplay
echo "== play_and_sample done =="
wc -l /data/local/tmp/reg_android_play_now.txt /data/local/tmp/dmesg_play.txt
