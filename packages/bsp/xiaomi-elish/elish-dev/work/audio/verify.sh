#!/bin/sh
echo "== warmup service =="; systemctl is-active amp-warmup.service; journalctl -b -u amp-warmup --no-pager 2>/dev/null | tail -3
echo "== Enable(1) failed 累计(开机后) =="; dmesg | grep -c "Enable(1) failed"
echo "== 播放 5 秒扫频(单次) =="
B=$(dmesg | grep -c "Enable(1) failed")
sudo -u axis env XDG_RUNTIME_DIR=/run/user/1000 pw-play /tmp/test5s.wav 2>&1 | tail -1
A=$(dmesg | grep -c "Enable(1) failed")
echo "   播放前=$B 播放后=$A  (新增 $((A-B)) 次失败)"
echo "   若新增=0 说明首次播放问题已解决"
