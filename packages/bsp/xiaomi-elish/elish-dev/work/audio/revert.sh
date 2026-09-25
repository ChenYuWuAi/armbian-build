#!/bin/sh
# 1) 增益回到 Android 同值: 模拟=18(+18.5dB), pipewire=100%
for n in $(amixer -c0 controls 2>/dev/null | grep "Analog PCM Volume" | grep -oE "numid=[0-9]+" | cut -d= -f2); do
  amixer -c0 cset numid=$n 18 >/dev/null 2>&1
done
echo "模拟增益(应18): $(amixer -c0 cget numid=2 2>/dev/null | sed -n '3p')"
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 wpctl set-volume @DEFAULT_AUDIO_SINK@ 1.0 >/dev/null 2>&1
echo "sink 音量: $(sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>&1|head -1)"
# 2) 撤掉 amp-sink.service 里我加的 2.0
sed -i 's#wpctl set-volume @DEFAULT_AUDIO_SINK@ 2.0#wpctl set-volume @DEFAULT_AUDIO_SINK@ 1.0#' /etc/systemd/system/amp-sink.service 2>/dev/null
systemctl daemon-reload
# 3) 找 DSP 保护算法的系数控件
echo "== DSP 系数控件 =="; amixer -c0 controls 2>/dev/null | grep -iE "attenuation|reduce|cal_r|output_power|cspl|algo" | head -14
