# Xiaomi Pad 5 Pro (elish) Armbian 修复报告

> 设备:`elish` / M2105K81AC / 256GB / CSOT 面板 / 序列号 `32b28a4a`
> 系统:Armbian(Ubuntu resolute)/ 内核 `6.12.58-current-sm8250`
> 报告时间:本次会话

---

## 0. 最终状态(全部实测)

| 项目 | 状态 | 证据 |
|---|---|---|
| 内核 ↔ 模块 | ✅ 匹配 | `uname -r` = `6.12.58-current-sm8250` = `/lib/modules` 唯一目录 |
| 内核包 | ✅ 正常 | `dpkg -l` → `ii linux-image-current-sm8250 25.11.2`(原为 `iF` 半配置) |
| 已加载模块 | ✅ 68 个(原 22 个) | `lsmod \| wc -l` |
| WiFi | ✅ **已恢复** | `wlp1s0 UP 10.0.0.123/24`,`nmcli: 100 (connected)` |
| 蓝牙 | ✅ **已恢复** | `hci0 BD 88:52:EB:E0:87:4B  UP RUNNING PSCAN ISCAN INQUIRY`;实测扫描→配对→连接蓝牙音箱 `SoundSticks 5` 成功 |
| A/B 槽位 | ✅ 健康 | `qbootctl`: `_a Active 1 / Successful 1 / Bootable 1` |
| `dtbo_a` | 擦除(按官方指南) | 官方 Flashing Guide 要求擦除 Linux 槽的 dtbo |
| 遗留 | ⚠️ SLPI 崩溃循环 / 重启卡死 / 扬声器 / WiFi MAC 为通用默认值 | 见 §5 |

---

## 1. 三个独立根因

这次"没 WiFi、没蓝牙、进不去 recovery"其实是**三个不相关的问题叠加**,必须分开看。

### 1.1 根因一:内核与模块完全不匹配(导致 WiFi+蓝牙+键盘"驱动全掉")

**现场证据:**
```
运行内核    : 6.19.14-edge-sm8250
/lib/modules: 6.12.58-current-sm8250     ← 没有 edge 的模块目录!
```
`lsmod` 只有 22 个模块(正常 68 个)→ 所有以模块形式提供的驱动(WiFi 的 `ath11k`、蓝牙的
`hci_uart`/`btqca`、键盘等)**一个都加载不了**。这就是"WiFi 和蓝牙驱动掉了、键盘要手动起"的真相。

**原因链:**
1. 安装了 `linux-image-current-sm8250`(6.12.58),但该包的 `postinst` **失败了**——因为镜像里缺 `mkbootimg`,
   而 `/etc/kernel/postinst.d/zz-update-abl-kernel` 必须用它生成启动镜像。
2. 结果:内核包停在 **`iF`(half-configured)**,`/boot` 里的符号链接悬空,启动镜像没有更新。
3. 启动分区里仍是老 edge 内核,而 edge 的模块已被删除 → 全模块失配。

**修复:** `apt install mkbootimg` → `dpkg --configure -a` 收尾 → 重建启动镜像并写入 `boot_a`。

### 1.2 根因二:OFRP recovery 进不去(Android 系 recovery 与槽位布局冲突)

- OFRP 是 **Android 系 recovery**,它必须能拿到**该槽位合法的 `dtbo`**;而官方指南明确要求
  **擦除 Linux 槽的 dtbo**(避免 Android overlay 干扰主线内核)——两者天然冲突。
- 本机是**槽位反转**布局:`super` 里只有一套 `_b` 逻辑分区 → **Android 在 slot b、Armbian 在 slot a**。
  而官方指南假设 Android 在 a、Linux 在 b(所以它写 `erase dtbo_b`)。
- 实测验证:把原厂 `dtbo.img`(29 个 entry)补进 `dtbo_a` 后,**主线内核反而起不来了** ——
  反证了指南"必须擦除"的要求。
- 另外手上这个 OFRP 包是 **A16(Android 16)** 构建,而本机 Android 是 **13**(OS1.0.2.0.TKYCNXM),版本也不匹配。

**结论:** 在本机布局下要进 OFRP,应当**在 slot b(Android 槽)引导**,而不是往 slot a 搬 dtbo。

### 1.3 根因三:蓝牙不可用 = NVM 里没有合法 BD_ADDR(本次重点,已彻底解决)

**症状:** `hci0` 卡在 `DOWN RAW`,`ACL MTU: 0:0`,`hciconfig hci0 up` → `Operation not supported (95)`,
mgmt 层 **0 个控制器**。固件下载其实**是成功的**(dmesg 有 `QCA setup on UART is completed`)。

**源码级定位(内核 6.12.58 源码,`net/bluetooth/hci_sync.c`):**
```c
invalid_bdaddr = test_bit(HCI_QUIRK_INVALID_BDADDR, &hdev->quirks) ||
                 test_bit(HCI_QUIRK_USE_BDADDR_PROPERTY, &hdev->quirks);
...
if (test_bit(HCI_QUIRK_EXTERNAL_CONFIG, &hdev->quirks) || invalid_bdaddr)
        hci_dev_set_flag(hdev, HCI_UNCONFIGURED);
```
→ 地址非法 ⇒ `HCI_UNCONFIGURED` ⇒ HCIDEVUP 被拒 ⇒ mgmt 看不到控制器。

**为什么地址非法:** `qca/htnv20.bin` 偏移 `0x10` 处的 6 字节(小端 = BD_ADDR)是:
```
ad 5a 00 00 00 00   →   00:00:00:00:5A:AD      ← OUI 前 4 字节被清零!
```
而且**原厂 ROM 的 `BTFM.bin/image/htnv20.bin` 同样残缺**(两边都是通用占位值)。

**真实地址的来源与"是否随机"的确认:**

| 证据 | 结果 | 结论 |
|---|---|---|
| `/persist/bluetooth/.bt_nv.bin`(6 字节) | `88 52 eb e0 87 4b` | 出厂写入的 BD 地址 |
| OUI 归属 | `88:52:EB` = **Xiaomi Communications Co Ltd** | 厂商注册的全球唯一 OUI |
| locally-administered 位 | 首字节 `0x88`=1000 1000,**bit1=0** | **不是随机生成**(随机会置位此位) |
| 文件时间戳 | `2022-06-18 05:51:49 +0800`(目录 2022-05-20) | 早于 2024 的 ROM → 出厂编程 |
| 原厂 `persist.img` | **没有 `bluetooth/` 目录** | 该文件是设备出厂时写入 `persist` 的 |

→ **`88:52:EB:E0:87:4B` 是本机真实的出厂蓝牙地址,不是随机值。**

**修复(两层,均已落地):**
1. **DT 层(主要):** 在 `bluetooth` 节点加 `local-bd-address = [4b 87 e0 eb 52 88];`
   — 字节序按官方规定 **LSB-first**(见 `bluetooth-controller.yaml`:要指定 `00:11:22:33:44:55`
   须写 `[55 44 33 22 11 00]`)。内核会据此调用 `qca_set_bdaddr()` 把地址写进芯片。
2. **固件层(加固):** 把同一地址写回 `htnv20.bin` 偏移 `0x10`(`4b 87 e0 eb 52 88`)。

**验证:** 服务保持 `disabled` 状态下**冷重启**,`hci0` 自动 `UP RUNNING`;实测扫描并连上真实蓝牙设备。

---

## 2. 本次改动清单

### 设备侧

| 路径 | 改动 | 备份 |
|---|---|---|
| `/dev/disk/by-partlabel/boot_a` | 写入新启动镜像(6.12.58 内核 + 打好 BT 补丁的 DTB) | 旧镜像本地留档 |
| `/usr/lib/linux-image-6.12.58-current-sm8250/qcom/sm8250-xiaomi-elish-csot.dtb` | 加入 `local-bd-address` | `.dtb.orig` |
| `/usr/lib/firmware/qca/htnv20.bin` | 偏移 `0x10` 写入真实 BD_ADDR | `htnv20.bin.armbian-orig` |
| `linux-image-current-sm8250` | `mkbootimg` + `dpkg --configure` 修复为 `ii` | — |
| `qbootctl` | 新装并 enable(自动标记 slot 成功) | — |
| `dtbo_a` | 擦除(按官方指南) | 原厂 `dtbo.img` 在 ROM 中 |
| `/etc/systemd/system/qca-bt-addr.service` | 安装了备用服务,但**已 disable**(DT 方案已生效,不需要) | — |
| `/usr/local/bin/qca-bt-addr.sh` | 同上 | — |

### 本地产物(`/home/axis/axis_rnd/`)

| 文件 | 说明 |
|---|---|
| `elish_boot/boot-btfix.img` | 含 BT 修复的启动镜像,`sha256 782667c9…`,59,228,160 B |
| `elish_boot/elish-csot-patched.dtb` | 打好补丁的 DTB(设备上 `sha256 06f97d4d…`) |
| `elish_boot/boot-current.img` | 未打 BT 补丁的版本(仅内核对齐),`sha256 6d5dc5f0…` |
| `rom/elish_images_OS1.0.2.0.TKYCNXM_13.0/` | 小米原厂 fastboot ROM(5,382,244,459 B,gzip 校验通过) |
| `platform-tools/` | adb/fastboot 37.0.1(Windows 版,经 WSL 互操作调用) |
| `src/linux-6.12.58/` | 主line 6.12.58 源码(用于本次源码级分析) |
| `src/armbian-xiaomi-elish/` | amazingfate 的移植工程 |
| `dtbo_empty.img` / `elish_start_usbgadget.sh` | 早前的救援工具 |

---

## 3. 关键操作复现命令

### 3.1 进入设备(USB gadget,不依赖 WiFi)

平板 USB-C 接 PC 后,平板侧 `usb0` 是 `172.16.42.1`;PC 侧需要静态地址(镜像缺 `unudhcpd`,没有 DHCP):
```powershell
Get-NetAdapter | ? InterfaceDescription -like "*Ncm*"
New-NetIPAddress -IPAddress 172.16.42.2 -PrefixLength 16 -InterfaceAlias "以太网 2"
```
```bash
ssh -i ~/.ssh/id_ed25519 axis@172.16.42.1     # axis 有免密 sudo
```
> WSL 是 `networkingMode=mirrored`,所以 WSL 内可直接访问 `172.16.42.1`。

### 3.2 重建启动镜像(复刻 `zz-update-abl-kernel`)

```
kernel  = gzip(/boot/vmlinuz-<ver>) + /usr/lib/linux-image-<ver>/qcom/sm8250-xiaomi-elish-csot.dtb
ramdisk = /boot/initrd.img-<ver>
cmdline = "root=UUID=21ce0d2d-58df-4703-821f-ada8a6b8ae4d slot_suffix=_a"
header  = v0, base=0, kernel_offset=0x8000, ramdisk_offset=0x1000000,
          tags_offset=0x100, pagesize=4096
写入    = dd if=boot.img of=/dev/disk/by-partlabel/boot_a   # axis 在 disk 组,无需 root
```
> 注意:Android boot v0 头的 `id` 字段是 32 字节,而 SHA1 摘要只有 20 字节,**必须补 12 个 0**。

### 3.3 蓝牙排障常用命令

```bash
sudo hciconfig -a hci0                 # 看 DOWN RAW / ACL MTU
sudo btmon -w /tmp/bt.snoop            # 抓 HCI 总线(判断内核是否真的发出命令)
sudo btmgmt info                       # mgmt 层控制器列表
sudo btmgmt public-addr 88:52:EB:E0:87:4B   # 手动补地址(应急)
sudo qbootctl                          # A/B 槽位 Active/Successful/Bootable
```

---

## 4. 为什么"进 recovery 就起不来"的完整解释

1. `fastboot erase dtbo_a`(官方指南要求)→ 槽位 a 没有合法 dtbo。
2. Android 系 recovery(OFRP)在槽位 a 引导时拿不到 dtbo → 失败回退 fastboot。
3. 每次失败都消耗 A/B 重试计数;Armbian **没有 `qbootctl`**(镜像缺该包)→ 成功启动也不标记
   `Successful` → 计数只减不增。
4. 计数耗尽 → 引导器把 slot a 标记为 **`unbootable`** → 自动切到 **slot b**(Android,userdata 已损坏)
   → "完全起不来"。
5. **已修复:** 装上 `qbootctl` 并 enable,现在开机自动 `mark successful`;`qbootctl` 显示
   `_a Active/Successful/Bootable = 1/1/1`。

---

## 5. 遗留问题(尚未解决)

1. **SLPI 崩溃循环 → 重启/关机卡死**(`USER-PD DOG detects stalled initialization`,`crash detected in 5c00000.remoteproc` 已 2 次)。
   目前**必须用 `sudo reboot -f`**。同源于 `qcom_glink` endpoint 拆除卡在 D 状态。
   → 线索:SLPI 固件/DT 配置;或把 SLPI 节点 `status = "disabled"`。
2. **扬声器 CS35L41 音量极小**:固件系数版本不匹配(driver 要求 ≥29.78,小米 `.bin` 是 29.53)。
3. **WiFi MAC 是通用默认值** `00:03:7f:12:69:69`(Atheros OUI),不是小米出厂 MAC。
   → 蓝牙已用上真实地址,WiFi 可能也需要类似的 BDF/DT 处理。
4. **DT 补丁的持久性**:本次已写入当前内核的官方 DTB 路径;若将来 `apt` 升级到**新的内核版本**,
   会新建 `linux-image-<新版本>/` 目录,**需要重新打这个补丁**(建议做成 DT overlay 或 apt hook)。
5. **OFRP**:如需使用,应在 **slot b** 引导;并考虑找 A13 版本的 OFRP。

---

## 6. 文档与资料索引

- 官方刷机指南:<https://github.com/amazingfate/armbian-xiaomi-elish/wiki/Flashing-Guide>
- WiFi 内核回归讨论:<https://forum.armbian.com/topic/50191-kernel-61213-breaks-wifi-on-xiaomi-elish/>
- QCA6390 热重启 BT 修复补丁(本机内核已含):<https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/commit/?id=e6e200b264271f62a3fadb51ada9423015ece37b>
- elish 蓝牙 DT 节点上游提交:<https://lkml.org/lkml/2024/10/7/682>
- `local-bd-address` 字节序规定:`Documentation/devicetree/bindings/net/bluetooth/bluetooth-controller.yaml`

---

## 7. 蓝牙鼠标卡顿:量化分析与优化

### 7.1 实测数据

设备:鼠标 `AULA-SC580`(BLE / HOG,句柄 11),适配器 `88:52:EB:E0:87:4B`。
方法:抓 `/dev/input/event9` 原始 `input_event`(24 字节/条),统计 `SYN_REPORT` 间隔。

| 指标 | 有 A2DP 音箱连接 | 断开 A2DP 后 | 说明 |
|---|---|---|---|
| 回报率 | 33.5 Hz | **59.7 Hz** | 正常 BT 鼠标应 100–133 Hz |
| 平均间隔 | 29.9 ms | 16.8 ms | |
| 间隔中位数 | 0.89 ms | 0.85 ms | **说明是"攒批送达"** |
| p95 / p99 / 最大 | 127 / 345 / 1499 ms | 45 / 150 / 3374 ms | 尖峰即卡顿感 |
| 抖动 σ | 93 ms | 100 ms | |
| 间隔 >30 ms 占比 | 20.7% | 9.9% | |
| 突发结构 | — | 815 簇,每簇 2.3 份,簇间中位 14.9 ms | 内部 125 Hz → 每 15 ms 发一批 |
| HCI RX | 1.3 KB/s,33 ACL 包/s | — | 与报告率一致 |
| HCI TX | 46 KB/s,77 包/s | — | **是 A2DP 音频流** |

### 7.2 结论:瓶颈不是带宽上限,而是延迟/抖动

| 环节 | 实测 | 是否瓶颈 |
|---|---|---|
| BLE 连接间隔 | `Connection interval: 7.50 msec`(btmon 实测,已是法定最小值) | ❌ |
| UART 速率 | 驱动 `oper_speed=3000000`;A2DP 实测跑通 368 kbps(> 115200,反证速率充足) | ❌ |
| 主机侧调度 | **所有 hci0 worker 与 bluetoothd 均为普通 TS 调度** | ✅ 主因之一 |
| WiFi/音频共存 | 同一颗 QCA6390;断开 A2DP 后回报率 **+78%**、p95 **−65%** | ✅ 主因之一 |

**A2DP 影响的量化**:`< ACL Data TX: Handle 7 ... Channel: 221 len 608`,RTP v2 头 `80 60 1e 13`
→ 那是 A2DP/SBC 音频流(~370 kbps)在持续占用空口,即使播放器处于暂停(sink 显示 `SUSPENDED` 仍在推流)。

### 7.3 已应用的优化(运行时生效,重启后由服务自动恢复)

| 项目 | 改动 | 落地方式 |
|---|---|---|
| hci0 内核 worker | `SCHED_FIFO` 优先级 55(原 TS) | `bt-latency.service` + `/usr/local/bin/bt-latency-tune.sh` |
| bluetoothd | `nice = -10`(HOG 数据路径在它里面) | 同上 |
| CPU 调频器 | `conservative` → **`schedutil`**(policy0/4/7) | `cpu-governor.service` |

> 注:`IBS tx_idle_delay` 驱动默认已是 2000 ms,调整它属空操作,故未纳入。

### 7.4 还能做什么(按收益排序)

1. **不用蓝牙音频时断开它** —— 实测对鼠标接近翻倍(33.5 → 59.7 Hz),这是单点收益最大的一项。
2. 若要长期用音频 + 鼠标:考虑让 WiFi 走 2.4 GHz 或调整共存策略(需驱动层支持)。
3. `hci_uart` 的 workqueue 若要**内核级**提升,需在 `alloc_workqueue()` 加 `WQ_HIGHPRI`(需重新编译内核)。
4. 蓝牙可被发现扫描(`ISCAN`)已确认关闭,无需处理。

### 7.5 复现测量的命令

```bash
# 抓输入事件(24 字节/条:2×int64 时间戳 + u16 type + u16 code + s32 value)
timeout 30 cat /dev/input/event9 > /tmp/ev.bin
# 解析并统计 SYN_REPORT(type=0,code=0)间隔
python3 -c "
import struct,statistics
d=open('/tmp/ev.bin','rb').read(); N=24
syn=[struct.unpack_from('<qqHHi',d,i*N) for i in range(len(d)//N)]
syn=[s+u/1e6 for s,u,t,c,v in syn if t==0 and c==0]
iv=[(syn[i+1]-syn[i])*1000 for i in range(len(syn)-1)]; iv.sort()
print('回报率 %.1f Hz  中位 %.2f ms  p95 %.2f ms  最大 %.2f ms'%(
  len(syn)/(syn[-1]-syn[0]), statistics.median(iv), iv[int(len(iv)*.95)], iv[-1]))"
# 链路层参数(btmon 抓重连时的 LE Connection Complete)
sudo btmon -w /tmp/bt.snoop & sleep 2
bluetoothctl disconnect <鼠标MAC>; sleep 3; bluetoothctl connect <鼠标MAC>; sleep 6
sudo pkill -f "btmo[n]"; sudo btmon -r /tmp/bt.snoop | grep -iE "Connection interval|Latency|Timeout"
```

---

## 8. 功放(CS35L41)音量极小 —— 反编译对比与根因定位

### 8.1 真实故障现象(重启后首次播放实测)

```
[   42.72] cs35l41 1-0040: DSP1: Legacy support not available
[   42.84] cs35l41 1-0041: Enable(1) failed: -110
[   42.84] cs35l41 1-0041: ASoC: PRE_PMU: BLH Main AMP event failed: -110
[   42.95] cs35l41 1-0042: Enable(1) failed: -110   ← 8 个功放全部相同
...
```

`Enable(1)` 是功放的**主电上电序列**(写 `PWR_CTRL1.GLOBAL_EN` 后轮询 `IRQ1_STATUS1` 的 PUP_DONE 位,
超时 100 ms)→ **8 个功放全部上电超时** → 只有极低电平的声音漏出,这就是"音量极小"。

### 8.2 关键结论:与固件内容**无关**

换上 Android 的 `wmfw`(34,236 B)+ TLH 调音(4,532 B),**重启后**再次测试 →
`Enable(1) failed: -110` 现象完全一样。

⇒ 上一轮"固件系数版本不匹配导致音量小"的结论**不成立**;固件不是这个症状的原因。
(固件确实与 Android 不同,但那是另一个问题,见 8.4)

### 8.3 反编译 Android DTB 对比(Armbian vs 原厂)

Android 的功放节点在 **dtbo overlay** 里(基础 DTB 只有 `cs35l41_int_speaker` pinctrl 节点):

```
cs35l41@40 {
        compatible = "cirrus,cs35l41";
        reset-gpios = <&tlmm 0x32 0x00>;        ← 与 Armbian 完全一致
        interrupts = <0x1b 0x08>;               ← 一致
        cirrus,boost-peak-milliamp = <0xfa0>;   ← 一致
        cirrus,boost-ind-nanohenry = <0x3e8>;   ← 一致
        cirrus,boost-cap-microfarad = <0x0f>;   ← 一致
        cirrus,asp-sdout-hiz = <0x03>;          ← 一致
        cirrus,temp-warn_threshold = <0x03>;    ← Armbian 无(主线驱动不支持该属性)
        cirrus,tuning-has-prefix;               ← Armbian 无(厂商私有,主线不支持)
        cirrus,fast-switch = "TRH-music.txt", …;← 厂商私有
        sound-name-prefix = "TRH";
        cirrus,gpio-config2 { … };              ← 厂商用子节点;Armbian 用扁平属性(主线支持)
};
```

**结论:DT 的关键项(reset GPIO 编号、IRQ 编号、boost 三参数、ASP 配置)与 Android 完全一致**
→ 不是 DT 配错。B2 版本的 errata patch 主线也正确处理(`cs35l41_revb2_errata_patch`)。

### 8.4 Android 与主线的固件架构差异(次一级问题)

| 项目 | Android(原厂) | Armbian 主线 |
|---|---|---|
| 保护算法 | `cs35l41-dsp1-spk-prot.wmfw` **34,236 B** | 34,056 B(不同版本) |
| 调音 | `<前缀>-…-spk-prot.bin` **8 个互不相同**(4532/4544/4556 B) | `…-spk-prot.bin` **948 B 通用占位** |
| 校准算法 | `cs35l41-dsp1-spk-cali.wmfw` + `<前缀>-…-spk-cali.bin` ×8 | **完全没有** |
| 出厂校准 | `/persist/audio/cs35l41_cal_spk1..8.bin`(cal_r 9117–9826,阻抗 6.52–7.03 Ω) | 未使用 |

主线驱动在**设备树**上无法选到每喇叭固件:`wm_adsp_request_firmware_files()` 里带前缀的命名
要求 `system_name` 非空,而 `system_name` 只来自 ACPI `_SUB`,DT 下恒为 NULL。
(厂商命名是 `<prefix>-<part>-<dsp>-<fw>.bin`,前缀在前,与主线约定也不同。)

### 8.5 下一步(按优先级)

1. **读功放真实寄存器**(`PWR_CTRL1` / `IRQ1_STATUS1`,经 `i2cget`):判断是"真的没上电"还是
   "已上电但驱动在 B2 上轮询的位/寄存器不对" —— 这是唯一能终结猜测的证据。
2. **延长 PUP_DONE 轮询超时**(100 ms → 500 ms)并重编模块:设备已具备编译条件
   (gcc 15.2.0 / make 4.4.1 / `linux-headers` + `Module.symvers`)。
3. **补 `cirrus,temp-warn_threshold` 支持**(Android 有、主线无)。
4. **实现 `cirrus,tuning-has-prefix`**:让主线按喇叭加载原厂 8 套固件 + cali,彻底恢复音量。

### 8.6 安全网(本次已全部就位)

- `elish_boot/boot-cs35l41-off.img` —— 8 个功放 `status = "disabled"` 的启动镜像(硬件层拔掉功放,
  任何固件/驱动异常都不会再引发中断风暴)
- 设备上 `/root/amp_backup/` —— 原固件 + 4 个功放相关模块
- 本地 `backup/` —— **112 个出厂分区**(0 异常)+ rootfs + `super_raw.img`
- Android 原厂固件集:`amp_fw/`(wmfw + 8×prot + 8×cali + diag)

### 8.7 深度排查结果(逐条排除)

用 `regmap` debugfs 直读功放硬件寄存器(注意:CS35L41 是 **32 位大端寄存器地址 + 32 位大端数据**,
`i2cget` 读不了,且驱动绑定后 I2C 地址被占用;`/sys/kernel/debug/regmap/1-0040/registers` +
`cache_bypass=1` 才是正确入口)。

**实测数据(8 个功放完全一致):**

| 寄存器 | 值 | 结论 |
|---|---|---|
| `DEVID` (0x0000000) | `00035a40` | 芯片正常识别 |
| `REVID` (0x0000004) | `000000b2` | B2,errata patch 已正确应用 |
| `PWR_CTRL1` (0x0002014) | 尝试时 `00000001`,失败后 `00000000` | **GLOBAL_EN 已写入**,后被驱动下电 |
| `PWR_CTRL2` (0x0002018) | `00000020` | **BST_EN = 2,升压已被使能** |
| `IRQ1_STATUS1` (0x0010010) | `00000000` | **PUP_DONE 始终不置位** |
| `IRQ1_STATUS4` (0x001001c) | `00000003` | OTP_BOOT_DONE = 1(芯片自身正常) |
| `DTEMP_EN` (0x0004308) | `00000000` | **温度检测是关闭的 → 温度不可能阻塞上电** |
| `AMP_ERR_VOL` (0x0006418) | `00000000` | 无错误 |
| 原始状态 `RAW_STATUS1` | `40806000` | bit31(AMP_SHORT_ERR)**未置位** → 无短路 |
| 未屏蔽的活跃条件 | `STATUS1=0` | **功放没有报告任何故障** |
| 升压寄存器 | `COEFF=0x2424` `PEAK_CUR=0x40` `SLOPE_LBST=0x7500` | 已按 DT 参数正确配置 |

**逐条排除的假设:**

| 假设 | 实验 | 结果 |
|---|---|---|
| 固件内容不对 | 换 Android wmfw+调音,重启后测试 | ❌ 现象相同 |
| 上电比 100ms 慢 | 补丁把超时改成 **1 秒**,重编模块 | ❌ 等满 1 秒仍不置位(报错间隔实测变成 ~1s,证明补丁生效) |
| DT 配错(reset/IRQ/boost) | 反编译 Android dtbo 逐项对比 | ❌ 关键项完全一致 |
| 缺供电 enable | 读 regulator 汇总 | ❌ 双方都是 dummy(常开轨),Android 的 DT 也没有 supply |
| 芯片坏了 | 读 DEVID/OTP/RAW_STATUS | ❌ 芯片健康、无故障 |
| 温度保护阻塞 | 读 `DTEMP_EN` | ❌ 温度检测未使能 |

**剩余怀疑(按可能性):**

1. **功放功率级供电(VBAT/VSPK)实际没电** —— I2C 走 VDDIO 所以能通信、OTP 能引导,
   但功率级没电则 GLOBAL_EN 永远等不到 PUP_DONE。需用万用表实测 VBST/VBAT,
   或查找一个在 Android 侧被标记为常开、而在 Armbian 的 DT 里没有使用者因而被
   regulator 核心关掉的供压器。
2. **厂商私有的上电前置步骤** —— Android 有 `cirrus,temp-warn_threshold` 与
   `cirrus,gpio-config2` 子节点,主线均不支持;可能还有寄存器级的前置初始化。
3. **`CS35L41_PROTECT_REL_ERR_IGN`(0x00002034)** 等保护相关寄存器的默认值差异 ——
   B2 的 errata patch 是否覆盖到,值得与厂商驱动逐寄存器比对。

**下一步建议:** 优先做 (1) —— 这是唯一能用廉价手段(万用表测 VBAT/VBST,或核对
Armbian DT 里所有 `regulator-always-on` 标记)证伪/证实的方向;其次是从 Android 的
`boot.img` 里提取厂商内核做符号/字符串级逆向比对。
