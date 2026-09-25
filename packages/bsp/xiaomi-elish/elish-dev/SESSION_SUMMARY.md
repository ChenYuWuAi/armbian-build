# elish（小米平板 5 Pro）刷机与排障 · 全程总结与重刷方案

> 设备：Xiaomi Pad 5 Pro `elish` / M2105K81AC / 256GB / **CSOT 面板** / 序列号 `32b28a4a`
> 目标系统：Armbian（Ubuntu 26.04 "resolute"），双系统保留 Android（slot b）

---

## 0. 当前状态

| 项目 | 状态 |
|---|---|
| **整盘重刷** | ✅ **已完成**（`boot_a` = `images\boot-final.img`；`linux` = `images\rootfs.allraw.sparse`；`dtbo_a` 已擦除；`slot a` 已激活并重启） |
| **slot label** | ✅ **已在镜像里预先改好**：`/boot/armbianEnv.txt` → `abl_boot_partition_label=boot_a`（开机即是正确的，无需再手动改） |
| 启动镜像 cmdline | `root=UUID=21ce0d2d-… slot_suffix=_a` ✅ |
| boot_b（Android 内核） | **完好未动** ✅ |
| Android | 分区完好，但 `userdata` 已被缩到 100G，**首次进 Android 需清数据** |
| 仍待处理 | 蓝牙（`hci0` 卡 `DOWN RAW`，属原版即存在的问题）、扬声器音量（CS35L41 固件系数不匹配） |

### 重刷后待办（用户配置完成后由我执行）
1. `grep abl_boot /boot/armbianEnv.txt` 复核（应为 `boot_a`）
2. 换 USTC 源（`/tmp/set_cn_mirrors.sh`）
3. 换 `current` 6.18 **LTS** 内核（消花屏）：`apt install linux-image-current-sm8250 linux-dtb-current-sm8250` + `apt-mark hold linux-image-edge-sm8250 linux-dtb-edge-sm8250`
4. 复核 `/proc/cmdline`、`qbootctl`
5. 重启一律 `sudo reboot -f`


---

## 1. 这次会话做了什么（时间线）

1. **找到 parted**：wiki 链接 `renegade-project.tech/tools/parted.7z` 已 403，改从 Wayback 快照取得（静态 aarch64 parted 3.5）。
2. **确定 Linux 必须刷 slot a**：`super` 里只有一套 `_b` 逻辑分区（虚拟 A/B），所以 Android 只活在 slot b；把内核刷进 `boot_b` 会直接丢 Android。
3. **分区（已执行）**：`userdata` 缩到 100G、新建 `esp` 0.5G、`linux` 142G。
4. **刷 Armbian 26.8.1**（CSOT 内核 + rootfs），并用 **Android sparse 格式绕过 fastboot 的 `std::bad_alloc`**（9.5GB 原始镜像直接刷会爆内存）。
5. **内核切到 `current` 6.18.44**：修掉 `edge` 6.19.14 的 **A650 GMEM_BASE 回归**（就是"窗口内偶发花屏"的根因）。
6. **换 USTC 中国镜像源**（实测清华/BFSU 对本机 403），1.5 亿字节级更新从十几分钟降到 11 秒。
7. **关闭 WiFi MAC 随机化**（IP 乱跳 .223→.172→.173→.224 的原因）。
8. **排查蓝牙与扬声器**：替换了 BT (`htnv20.bin`/`htbtfw20.tlv`) 与功放 (`cs35l41`) 固件 → **导致启动崩溃**。
9. **用 raw 块写入救回 cs35l41 固件**（不挂载、直接改数据块）→ 系统成功启动 ✓（这一步验证了方法可靠）。
10. **修 BT 固件时再次失败** → 决定整盘重刷。

---

## 2. 有长期价值的技术结论

### 2.1 花屏（已定位，源码级证据）
`6.19.14-edge` 内核里 `adreno_get_param()`：
```c
case MSM_PARAM_GMEM_BASE:
    if (adreno_gpu->info->family >= ADRENO_6XX_GEN4) *value = 0;
    else                                            *value = 0x100000;   // ← A650 落这里，错
```
A650 = `ADRENO_6XX_GEN3`（`compatible = qcom,adreno-650.2`），错报 1MB 偏移 → 任何从 GMEM 采样的渲染（advanced blend / custom resolve / MSAA resolve）随机错乱 → **窗口内部偶发花屏**。
- 上游修复：`drm/msm: Fix GMEM_BASE for A650`（改成 `>= ADRENO_6XX_GEN3`，[dri-devel 2026-03](https://lists.freedesktop.org/archives/dri-devel/2026-March/555897.html)）
- **6.18.44-current 里是正确的旧逻辑**（`adreno_is_a650_family()`）→ 换 current 即修好 ✓
- edge 26.8.3 仍是 6.19.14 → 升级 edge 无用

### 2.2 蓝牙
- DT 与驱动**都不需要自己改**：上游 2024-09 已合入 [`qca6390-pmu` 节点](https://lkml.org/lkml/2024/9/29/257) 与 [`bluetooth` 节点](https://lkml.org/lkml/2024/9/29/259)；本机 DT 里这些节点齐全，`pwrseq-qcom_wcn` 正常绑定。
- 症状根因线索：`hci0` 停在 **`DOWN RAW`**（内核 `HCI_UNCONFIGURED`）→ mgmt 层 0 个控制器 → bluez/GNOME 完全看不到；`hciconfig hci0 up` 报 `Operation not supported (95)`；TX 902 条命令 / RX 仅 35 个事件。
- **WiFi 与 BT 是同一颗 QCA6390、共用供电时序**：把 BT 的 NVM 换成小米 Android 版后，**WiFi 也一起消失** —— 这是本次把系统搞崩的关键教训（**BT 固件不要乱换**）。
- 原版固件（务必用这两份）：`qca/htbtfw20.tlv` 210,704B md5 `109CE428…`、`qca/htnv20.bin` 5,857B md5 `74AC04B7…`。

### 2.3 扬声器（CS35L41）
- 8 个功放（`speaker-amp@40..43` on i2c1/i2c3，`sound-name-prefix` = TLH/TRH/BLH/BRH/TLL/TRL/BLL/BRL）。
- 现象：全部 `Enable(1) failed: -110` + `Legacy support not available`，音量极小。
- 根因：**固件与驱动版本不匹配** —— Armbian 把 `cs35l41-dsp1-spk-prot-<id>.wmfw` 软链到 Cirrus **v6.83.0（系数 29.85）**，而 `.bin` 是小米 2021 年固件（**系数 29.53**）；驱动期望 ≥29.78 → DSP 保护固件起不来（[上游补丁](https://patchew.org/linux/20230922142818.2021103-1-sbinding@opensource.cirrus.com/)）。
- Android 侧 `/vendor/firmware/` 里有配套的 `cs35l41-dsp1-spk-prot.wmfw` + 每喇叭 `.bin` + `-spk-cali.bin`；`/persist/audio/cs35l41_cal_spk1..8.bin` 是出厂校准（4 字节 cal_r）。
- **待办**：需要为这些喇叭找系数 ≥29.78 的固件，或把驱动侧的版本检查绕开（下一阶段再研究）。

### 2.4 重启卡死（你遇到的"重启卡 udev"）
内核栈实锤：
```
(udev-worker) State: D   wchan: device_del
 device_del → device_unregister → rpmsg_unregister_device
 → qcom_glink_destroy_ept → rpmsg_destroy_ept → rpmsg_dev_probe ← driver_register
```
**SLPI（传感器 DSP）先崩**（`USER-PD DOG detects stalled initialization` + `crash detected in slpi`），随后 glink endpoint 拆除永远等不到响应 → worker 永久 D 状态 → `systemctl stop systemd-udevd` 与关机/重启全部卡死。
→ **结论：这台机器只能用 `sudo reboot -f`（或 `echo b > /proc/sysrq-trigger`）**。

### 2.5 为什么 OFRP 救不了文件系统
设备 rootfs 用了新 ext4 特性（`FEATURE_C12`=orphan_file、`FEATURE_R16`=metadata_csum_seed），而 OFRP 里是 **e2fsprogs 1.45.4（2019）**，无 `debugfs`，且其内核不认这些 ro_compat 特性 → **只能 `ro,noload` 挂载，无法 rw 写入**。

### 2.6 A/B 回退会"越弄越起不来"
slot a 连续启动失败 N 次后，引导器会**自动切到 slot b**（Android，且其 userdata 已损坏）→ 表现就是"完全起不来"。`fastboot set_active a` 会重置重试计数，**每次改动后都要确认 `current-slot: a`**。

### 2.7 其它坑
- Windows 版 fastboot 37.0.0 刷 >4GB 原始镜像必 `std::bad_alloc`；`-S 256M` 无效 → 必须转 **Android sparse**。
- `fastboot erase dtbo_a` 是 elish wiki 要求的（避免 Android 的 dtbo 覆盖层干扰主线内核）；**但擦掉后 OFRP 无法临时启动**（Android 系 recovery 需要 dtbo），需要时用「合法空 dtbo」（`dt_entry_count=0`，2KB）临时补回。
- `/proc/cmdline` 里 `slot_suffix` 是**烧死在启动镜像里**的；`/boot/armbianEnv.txt` 的 `abl_boot_partition_label` 决定内核装到哪个槽 —— **必须先改成 `boot_a`**，否则 `apt upgrade` 装内核会写进 Android 的 `boot_b`。
- 该镜像**缺 `mkbootimg`**（`apt install mkbootimg` 才能让 Armbian 的内核钩子正常工作）。
- MAC 随机化：`nmcli con mod "<名>" 802-11-wireless.cloned-mac-address permanent` + `/etc/NetworkManager/conf.d/00-no-mac-randomization.conf`。

---

## 3. 重刷方案（整盘重刷 Linux 侧，Android 不动）

**前提**：平板进 fastboot（电源+音量减）；PC 上 `fastboot devices` 能看到 `32b28a4a`。

```powershell
# 0) 确认在 fastboot、且 slot 状态
fastboot devices
fastboot getvar current-slot          # 需要时再 set_active a

# 1) 擦除 Linux 槽的 dtbo（elish wiki 要求）
fastboot erase dtbo_a

# 2) 刷启动镜像（原版：edge 6.19.14 内核 + CSOT dtb，已在本会话验证可启动）
fastboot flash boot_a C:\axis_rnd\images\boot-csot.img

# 3) 刷 rootfs（9.5GB，全 RAW sparse，已做字节级校验；约 4.5 分钟）
fastboot flash linux C:\axis_rnd\images\rootfs.allraw.sparse

# 4) 切 slot a 并重启
fastboot set_active a
fastboot reboot
```

用到的镜像与校验：

| 文件 | 大小 | sha256（前 16） | 说明 |
|---|---|---|---|
| `images\boot-csot.img` | 60,813,312 | `C0477DC0D6FD500B` | 原版启动镜像（edge 6.19.14 + CSOT dtb） |
| `images\rootfs.allraw.sparse` | 9.5 GB | 展开后 = `92BC621B0A7CC8FA` | 原版 rootfs（转 sparse 后与原图**逐字节一致**已验证） |
| `images\rootfs.img`（原始） | 10,200,547,328 | `92BC621B0A7CC8FA…` | ext4 UUID `21ce0d2d-58df-4703-821f-ada8a6b8ae4d` |

---

## 4. 重刷后收尾清单

1. **首次登录**：USB-C 接电脑 → PC（管理员）给 gadget 网卡配 IP：
   `Get-NetAdapter | ? InterfaceDescription -like '*UsbNcm*' | New-NetIPAddress -IPAddress 172.16.42.2 -PrefixLength 24`
   然后 `ssh root@172.16.42.1`（密码 `1234`，首登会要求改密码/建用户）。
2. **换中国源**：`/tmp/set_cn_mirrors.sh`（本会话用过，USTC）
3. **改槽位标签**（务必先做）：
   `sed -i 's/^abl_boot_partition_label=.*/abl_boot_partition_label=boot_a/' /boot/armbianEnv.txt`
   然后 `dpkg-reconfigure linux-image-<当前内核>`，并用 `cat /proc/cmdline`、`qbootctl` 复核。
4. **换 `current` 内核**（消花屏）：`apt install linux-image-current-sm8250 linux-dtb-current-sm8250`，之后 `apt-mark hold linux-image-edge-sm8250 linux-dtb-edge-sm8250`。
5. **重启一律用 `sudo reboot -f`**（见 2.4）。
6. **WiFi/蓝牙**：用原版固件即可（不要替换 `/usr/lib/firmware/qca/ht*`）；如 BT 仍不行，再单独研究（见 2.2）。
7. **扬声器**：音量小是固件版本问题（见 2.3），需要单独处理。
8. **Android**：`fastboot set_active b` 后进 recovery 清数据即可回到 Android。

---

## 5. 本地产物清单（`C:\axis_rnd`）

**镜像**
- `images\rootfs.allraw.sparse`（重刷用，已验证）、`images\rootfs.img`（原始 9.5G）
- `images\boot-csot.img`（原版启动镜像）、`images\boot-cs35l41-off.img`（禁用功放驱动版）
- `images\boot-fwfix2.img`（注入 initramfs 修复钩子的版本，79MB）
- `images\dtbo_empty.img`（合法空 dtbo，救 OFRP 用）
- `images\orig_fw\cs35l41-dsp1-spk-prot.wmfw`（原版功放固件 34,056B）
- `images\orig_qca\{htnv20.bin,htbtfw20.tlv}`（原版 BT 固件）

**脚本**
- `raw2sparse.py` / `sparse2raw.py`（raw↔sparse + 哈希校验）
- `ext4_dev_find.py`（经 adb 按设备真实元数据走目录树定位文件数据块）
- `ext4_local_find.py` / `ext4_find_blocks.py`（本地/远程 ext4 解析）
- `patch_bootcmdline.py`（改 boot 镜像 cmdline）
- `build_rescue2.py`（构建注入 initramfs 钩子的启动镜像，含硬链接展开与自检）
- `lp_list.py` / `lp_extract.py`（解析 liblp 超分区、抽取逻辑分区）
- `set_cn_mirrors.sh` / `fix_label.sh` / `diag_*.sh`（换源/修标签/各类诊断）
- `mk_dtbo_empty.py`、`unxz.py`、`imgstat.py`、`inspect_boot.py`、`get_busybox.py`

**报告**
- `elish_flash_result.md`（刷机执行记录）、`elish_recovery_readonly_report.md`（只读勘察）
- `CHANGES_SUMMARY.md`（改动总览）、本文件（会话总结 + 重刷方案）
