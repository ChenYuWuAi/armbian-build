#!/bin/bash
# Android (elish, slot A) ADSP/音频管线采样脚本
# 用法: bash adsp_sample.sh [输出目录]
# 依赖: /mnt/c/Users/cheny/Downloads/platform-tools/adb.exe (Windows 侧 adb)
set -u
ADB=/mnt/c/Users/cheny/Downloads/platform-tools/adb.exe
OUT=${1:-/home/axis/axis_rnd/work/android/capture}
mkdir -p "$OUT"
A(){ timeout 120 "$ADB" shell "$1" 2>&1; }
P(){ timeout 300 "$ADB" pull "$1" "$2" 2>&1 | tail -1; }

echo "=== 0) 设备状态 ==="
timeout 60 "$ADB" devices -l | tail -n +2
A 'getprop ro.build.version.incremental; getprop ro.boot.slot_suffix; getprop ro.boot.hardware'

echo "=== 1) 声卡与驱动 ==="
A 'cat /proc/asound/cards' > "$OUT/asound_cards.txt" 2>&1
A 'ls /proc/asound/' > "$OUT/asound_list.txt" 2>&1
A 'ls -l /dev/snd/ 2>/dev/null | head -30' > "$OUT/dev_snd.txt" 2>&1

echo "=== 2) Android 音频框架 ==="
A 'dumpsys media.audio_flinger' > "$OUT/audio_flinger.txt" 2>&1
A 'dumpsys media.audio_policy'  > "$OUT/audio_policy.txt"  2>&1
A 'dumpsys audio'               > "$OUT/audio_service.txt" 2>&1
A 'dumpsys media.audio_flinger' | grep -aiE "standby|Format|sample rate|Channel|device|volume|session" | head -40

echo "=== 3) mixer 控件（原厂实际写入值 = 逆向 ADSP 链路的关键）==="
A 'tinymix -D 0'    > "$OUT/tinymix_d0.txt" 2>&1 || A 'tinymix' > "$OUT/tinymix_d0.txt" 2>&1
A 'cat /vendor/etc/mixer_paths.xml 2>/dev/null' > "$OUT/mixer_paths.xml" 2>&1
A 'ls -l /vendor/etc/ | grep -iE "mixer|audio|acdb"' > "$OUT/vendor_etc_audio.txt" 2>&1
wc -l "$OUT/tinymix_d0.txt" 2>/dev/null

echo "=== 4) ADSP / q6 相关内核信息 ==="
A 'dmesg 2>/dev/null | grep -aiE "cs35|tdm|q6|adsp|wcd|lpass|slim|soundwire" | tail -80' > "$OUT/dmesg_audio.txt" 2>&1
A 'ls /sys/kernel/debug/ 2>&1 | head -20' > "$OUT/debugfs.txt" 2>&1
A 'cat /sys/module/snd_soc_cs35l41/parameters/* 2>/dev/null' > "$OUT/cs35l41_params.txt" 2>&1

echo "=== 5) 拉取原厂配置文件（管线逆向素材）==="
for f in /vendor/etc/audio_policy_configuration.xml \
         /vendor/etc/audio_policy_volumes.xml \
         /vendor/etc/default_volume_tables.xml \
         /vendor/etc/audio_effects.xml ; do
  P "$f" "$OUT/$(basename $f)"
done
A 'ls /vendor/etc/acdbdata/ /vendor/etc/acdbdata/Forte/ 2>&1' | tee "$OUT/acdb_listing.txt"
for f in /vendor/etc/acdbdata/Forte/*.acdb ; do
  P "$f" "$OUT/"
done

echo "=== 6) HAL / Cirrus 库（后续静态逆向）==="
A 'ls -l /vendor/lib64/hw/ | grep -iE "audio"' | tee "$OUT/hal_listing.txt"
A 'find /vendor/lib64 -maxdepth 1 -iname "*cirrus*" -o -maxdepth 1 -iname "*acdb*" 2>/dev/null' | tee "$OUT/cirrus_listing.txt"
for f in $(A 'ls /vendor/lib64/hw/audio.primary.*.so /vendor/lib64/libcirrusspkrprot.so /vendor/lib64/libacdbloader.so 2>/dev/null'); do
  P "$f" "$OUT/"
done

echo "=== 7) 播放中抓取（需先播放音频）==="
cat <<'TIP'
手动步骤（在平板上播放一段音乐/白噪声，然后重跑）:
  bash adsp_sample.sh capture_playing
对比 capture_playing/tinymix_d0.txt 与 capture/tinymix_d0.txt 的差异
 ⇒ 差异项就是从 Android 一路传到 CS35L41/TDM 的实际配置。
有 root 时（Magisk）可加:
  adb shell su -c 'cat /sys/kernel/debug/regmap/*/registers'
  /root/ampreg (!)  — CS35L41 寄存器需要 i2c，Android 下用 /vendor/bin/ 或 busybox i2cget
TIP
echo "=== 完成，输出在 $OUT ==="
