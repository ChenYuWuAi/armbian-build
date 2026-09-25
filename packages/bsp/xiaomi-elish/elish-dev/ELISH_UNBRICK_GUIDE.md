# 小米平板 5 Pro (elish) 救砖专题：ABL 无法加载任何启动镜像

> 设备：`M2105K81AC` / 代号 `elish`，SM8250，UFS `SAMSUNG KLUEG8UHDC-B0E1`（逻辑扇区 **4096 B**），序列号 `32b28a4a`
> 现象关键词：**两个槽都只进 fastboot**、`Failed to load/authenticate boot image: Device Error`、`Failed to load image from partition: Device Error`、重试计数不减
> 本文记录 2025-09 两次同类故障的完整根因、正确修法、以及**一个会二次伤害设备的巨大陷阱**。

---

## 0. TL;DR（30 秒版）

```bash
# 1) 判定（关键证据）
fastboot continue
#   坏: FAILED (remote: 'Failed to load image from partition: Device Error')
#   好: Resuming boot   OKAY

# 2) 重建几何正确的 GPT（离线、可校验、无损）
python3 work/tools/rebuild-gpt.py \
    --tables backup/partbackup \
    --rom    rom/elish_images_OS1.0.2.0.TKYCNXM_13.0/images \
    --out    /tmp/gptnew

# 3) 刷入 6 个 LUN 的 GPT（fastboot 里）
for n in 0 1 2 3 4 5; do
  fastboot flash partition:$n /tmp/gptnew/gpt_both${n}_fix.bin
done

# 4) 复验
fastboot continue          # 必须变成 Resuming boot OKAY
```

**绝对不要**直接刷 ROM 里的 `gpt_main*.bin` / `gpt_both*.bin` —— 见 §2.2，那会把 `userdata`/`esp`/`linux` 一起毁掉。

---

## 1. 症状与判定

### 1.1 典型表现

| 观察点 | 故障态 | 正常态 |
|---|---|---|
| 开机 | 两个槽都掉回 fastboot，屏幕无进展 | 正常进系统 |
| `fastboot continue` | `FAILED (remote: 'Failed to load image from partition: Device Error')` | `Resuming boot OKAY` |
| `fastboot boot <任意镜像>` | `FAILED (remote: 'Failed to load/authenticate boot image: Device Error')` | `Booting OKAY` |
| 槽重试计数 | 停在 6/7 不再递减（引导器**根本没尝试**启动） | 每次启动递减 |
| `fastboot flash <分区>` | **照常 OKAY**（这是最大的迷惑点） | OKAY |
| `unlocked` / `devinfo` | `unlocked: yes` ✓ 正常 | 同 |

> **迷惑点**：分区能刷、`getvar partition-size` 能读，看起来存储没问题；但 ABL 读取启动镜像时返回 `EFI_DEVICE_ERROR`。原因是 fastboot 协议路径用的是**启动时缓存的分区表**，而引导路径按 GPT 槽元数据解析分区，两者不一致/元数据损坏时就出现这个组合。

### 1.2 判定顺序（不要跳步）

```bash
fastboot devices                     # 确认在位
fastboot getvar current-slot         # 当前槽
fastboot getvar slot-retry-count:a   # 故障时: 不递减
fastboot getvar slot-retry-count:b
fastboot continue                    # ← 一句话定性：这是本文故障的决定性证据
```

**不要**把时间花在刷 `boot_a/boot_b`、`vbmeta`、`dtbo`、`misc`、安全分区上 —— 这条路上全部是死路（见 §8）。

---

## 2. 根因：两层陷阱

### 2.1 第一层：GPT 槽元数据被破坏

Qualcomm ABL 从 **GPT 分区项属性（attributes）/ type GUID** 里读 A/B 槽状态。写它的是 `qbootctl`（通过 UFS BSG 直接改写 **6 个 LUN 的 GPT**）：

* **第一次故障**：`qbootctl -s b` 被中断 → 6 个 LUN 的槽元数据处于半写状态 → ABL 认不出任何槽的完整分区集 → 读启动镜像失败。
* **第二次故障**：平板内 AI 改设备树/"写坏"分区表 → 同一症状复发。

特征：**GPT 里分区名和尺寸可能看起来都正常**（所以按尺寸比对查不出来），坏的是槽属性/type GUID 的**一致性**。这也是为什么"恢复几个很小的分区"这个历史经验方向是对的（GPT 只有 44 KB），但**照着刷原厂文件是错的**。

### 2.2 第二层（真正的坑）：原厂 `gpt_main*.bin` 是**未解析盘容量的模板**

ROM 的 `rawprogram*.xml` 里明文写着占位符：

```xml
<program ... label="BackupGPT" start_sector="NUM_DISK_SECTORS-5." num_partition_sectors="5" .../>
```

`NUM_DISK_SECTORS` 是给刷机工具（QFIL/EDL）替换的。而随 fastboot 包一起的 `gpt_main*.bin` / `gpt_both*.bin` **没有解析它**，于是 `last_usable_lba` 只等于"已声明分区之和 −1"，而不是真实盘尾：

| LUN | 设备真实扇区 | 原厂模板扇区 | 被截断的尾部分区 | 后果 |
|---|---|---|---|---|
| **0** | **61880320** (253 GB) | 2686981 (11 GB) | `userdata`（→ 0 长度） | **致命**：`esp`+`linux`(Armbian rootfs) 槽位直接消失 |
| 1 | 4096 | 2187 | `xbl_config_a` | xbl 配置分区被缩到 512 KB |
| 2 | 4096 | 2187 | `xbl_config_b` | 同上 |
| 3 | 8192 | 837 | `mdmddr` | 无关紧要 |
| 4 | 557056 | 525475 | `vm-data` | 无害（但备份 GPT 被写到盘中间） |
| 5 | 16384 | 8197 | `mdm1m9kefsc` | 调制解调器 NV 区被缩 |

**历史教训**：上一次故障时（会话内可查）就是刷了 `gpt_both0..5.bin`（原厂）→ `fastboot continue` 立刻变 OKAY（引导修好了），但 **LUN0 布局被毁**：`userdata` 变 0 长度、`esp`/`linux` 分区项消失 → Armbian 掉进 initramfs（`root=UUID=21ce0d2d...` 找不到分区）、Android 卡 logo。随后不得不手工重建分区表（这才有了 `backup/partbackup/gpt_sfdisk.bak` 这份 36 分区的权威表）。

> 所以"恢复几个很小的分区"这个经验**只对了一半**：分区确实是 GPT（44 KB），但**不能用原厂文件，必须用设备自己的表重建**。

---

## 3. 设备权威地图

### 3.1 LUN 与 Linux 命名（一一对应，来自设备自身 `/proc/partitions`）

| LUN | Linux | 真实扇区数 | 容量 | 内容 |
|---|---|---|---|---|
| 0 | `sda` | 61880320 | 253.5 GB | `switch`…`rescue`、`userdata`、`esp`、`linux`、`super`、`cust`、`devinfo` |
| 1 | `sdb` | 4096 | 16 MB | `xbl_a`, `xbl_config_a` |
| 2 | `sdc` | 4096 | 16 MB | `xbl_b`, `xbl_config_b` |
| 3 | `sdd` | 8192 | 32 MB | `ALIGN_TO_128K_1`, `cdt`, `ddr`, `mdmddr` |
| **4** | **`sde`** | **557056** | 2.18 GB | **`boot_a/boot_b`、`dtbo`、`vbmeta`、`abl`、`modem`、`vendor_boot`…（65 分区）** |
| 5 | `sdf` | 16384 | 64 MB | `modemst*`, `fsg`, `fsc` |

* **启动镜像全在 LUN4**（`boot_a` = 第 12 项 = `/dev/sde12`），所以 LUN4 是"能否加载镜像"的主战场。
* LUN0 是唯一含**自定义分区**的 LUN（`esp`、`linux` 是从 `userdata` 尾部切出来的）→ **只有它必须按设备表重建**。

### 3.2 LUN0 关键分区（`gpt_sfdisk.bak` / `gpt_parted_map.txt`，扇区 4096 B）

| # | 名字 | start | size | 说明 |
|---|---|---|---|---|
| 11 | `misc` | 1024 | 1024 | BCB / A/B 槽信息 |
| 16 | `devinfo` | 12288 | 4096 | **解锁标志所在，永不刷写**（attrs bit60） |
| 29 | `super` | 417792 | 2228224 | Android system/vendor/product（9.1 GB） |
| 30/31 | `vbmeta_system_a/b` | 2646016 / 2646048 | 32 | 槽属性示例：`_a` = bits `50,54,60`，`_b` = bit `60` |
| 33 | `rescue` | 2654208 | 32768 | |
| 34 | `userdata` | 2686976 | **24412634** | 原厂是"grow 到盘尾"，双系统布局把它截断 |
| 35 | `esp` | 27099610 | 122112 | 477 MB，双系统引导分区 |
| 36 | `linux` | 27221722 | **34658593** | 132 GB，**Armbian rootfs**，ext4 `UUID=21ce0d2d-58df-4703-821f-ada8a6b8ae4d` |

> **重要澄清**（曾误判）：Armbian rootfs 在**独立的 `linux` 分区**，**不在** `userdata`。`userdata`（93.1 G）当前**未格式化/为空**。因此 Android 与 Armbian **互不冲突**：Android 用 `super`+`userdata`，Armbian 用 `linux`+`esp`。

---

## 4. 正确修复流程

### 4.1 准备：设备自身分区表 dump（**在设备还能进系统时就要做！**）

```bash
# 在运行中的 Armbian 上
lsblk -o NAME,SIZE,FSTYPE,PARTLABEL   > lsblk.txt
cat /proc/partitions                  > proc_partitions.txt
blkid                                 > blkid.txt
for d in a b c d e f; do
  parted -m /dev/sd$d unit s print    > gpt_parted_map_$d.txt
  sfdisk -d /dev/sd$d                 > gpt_sfdisk_$d.bak      # ★ 6 个 LUN 全部 dump
done
```

> **本次教训**：备份里只有 `gpt_sfdisk.bak`（**仅 LUN0**）。LUN1–5 只能靠"原厂条目 + 真实容量"重建。以后请像上面那样**把 6 个 LUN 全 dump**，救援会简单得多。

### 4.2 生成几何正确的 GPT

工具：`work/tools/gptpatch.py`（字节级打补丁 + 校验原语）、`work/tools/rebuild-gpt.py`（驱动）。

```bash
python3 work/tools/rebuild-gpt.py \
    --tables backup/partbackup \
    --rom    rom/elish_images_OS1.0.2.0.TKYCNXM_13.0/images \
    --out    /tmp/gptnew
```

它在做什么：

1. **LUN0**：把设备权威表（36 分区，含 `esp`/`linux` 与全部 type/unique GUID、属性）**施加**到原厂 `gpt_main0.bin` 上 —— 同名分区就地改写起止，新分区写入空槽 34/35。
2. **LUN1–5**：保留原厂条目，只把**末尾分区扩展到真实盘尾**（`xbl_config_*`/`mdmddr`/`vm-data`/`mdm1m9kefsc`），并清掉 0 长度分区（`last_parti`）。
3. **几何字段修正**：`last_usable_lba = 总扇区-6`，主表 `alternate_lba = 总扇区-1`，备表 `my_lba = 总扇区-1`、`alternate_lba = 1`、`partition_entry_lba = 总扇区-5`。
4. **重算 CRC**：条目数组 CRC（头偏移 88）+ 头部 CRC（头偏移 16，计算时该字段清零）。
5. **原厂文件格式**（ABL 的 `flash partition:N` 就吃这个）：
   * primary = 6 扇区（MBR(1) + header(1) + 条目数组 [+零填充]）= 24576 B
   * backup = 5 扇区（**条目数组在偏移 0** [+零填充] + header 在最后一扇区）= 20480 B
   * 文件 = primary + backup = **45056 B**（与原厂 `gpt_both*.bin` 同尺寸）

### 4.3 刷入

```bash
for n in 0 1 2 3 4 5; do
  fastboot flash partition:$n gpt_both${n}_fix.bin
done
# 期望: Sending/Writing ... OKAY（"skip copying avb footer" 警告是正常的，GPT 不是 AVB 目标）
```

> 若出现 `FAILED (remote: 'Error Updating partition Table')` —— **说明文件尺寸/格式不对**（例如用了 17408 B 的 `dd bs=512 count=34` 半截 dump）。ABL 要求的就是上面那个 45056 B 结构。

### 4.4 复验（必须做）

```bash
fastboot continue                      # ← 必须 OKAY
fastboot getvar slot-retry-count:a     # 应重置为 7
fastboot getvar slot-retry-count:b     # 应重置为 7
```

---

## 5. 校验标准（本文所有镜像都过了这 5 关）

| 关卡 | 内容 |
|---|---|
| 1. 序列化器等价 | 用同样的写入逻辑重建**原厂未改动**文件，必须**逐字节相同**（否则说明格式理解有误） |
| 2. 几何 | `last_usable == 总扇区-6`、主表 `alternate == 总扇区-1`、备表 `pel == 总扇区-5` 等全字段断言 |
| 3. CRC | 主/备两套：条目数组 CRC + 头部 CRC 重算后自校验通过 |
| 4. 与设备表一致 | 生成表的**每个分区名/尺寸**与设备自身 `proc_partitions.txt`+`blkid.txt` **完全一致**，且无遗漏/无多余 |
| 5. 改动最小化证明 | 逐字节 diff 原厂文件：差异**只允许**出现在头部几何/CRC 字段与被改写的条目的 `last_lba`（LUN0 另加新增的 `esp`/`linux` 条目），其余一律不许变 |

本次产物（可复现）：

```
gpt_both0_fix.bin  md5 08e671ff70e3d42d68446d0292fefd9b
gpt_both1_fix.bin  md5 47204fd520acdeb3a657d1b1d2e94af5
gpt_both2_fix.bin  md5 cd7681a543ebcd663ee4d76abda343b2
gpt_both3_fix.bin  md5 2f06dd70f2abdf0a66be5c3ac547c835
gpt_both4_fix.bin  md5 81ee3ca8c0f81e6b908cde3278d5b1cb
gpt_both5_fix.bin  md5 cc9c591f3d9ae57bc4196fe4af4cc369
```

---

## 6. 修 GPT 之后：双系统槽位恢复

修复后状态（本次实测）：

```
内核: 6.12.58-current-sm8250
根:   /dev/sda36 ext4 /            ← linux 分区，完整保留 ✓
参数: root=UUID=21ce0d2d-58df-4703-821f-ada8a6b8ae4d slot_suffix=_a
      androidboot.verifiedbootstate=orange ... androidboot.vbmeta.device_state=unlocked
分区: sda34 userdata 93.1G(空) | sda35 esp 477M | sda36 linux 132.2G | sde12 boot_a 192M
音频: card 0: Xiaomi Mi Pad 5 Pro ✓
```

### 6.1 布局约定

* **slot a = Android**：`boot_a` = Android 启动镜像（要 root 就用 Magisk 修补版）
* **slot b = Armbian**：`boot_b` = `boot_b_l10c.img` 一类自建镜像
* **`dtbo` 规则**：**Linux 所在槽的 `dtbo` 必须为空**（Android 的 dtbo 会让 mainline 内核起不来）：
  ```bash
  fastboot erase dtbo_b        # Armbian 在 b 槽时
  ```
* **镜像与 rootfs 必须同 ABI**：ramdisk 里的 `lib/modules/6.12.58-current-sm8250` 必须与 `linux` 分区上 `/lib/modules/<版本>` 一致，否则模块全不加载（没声音、没网络）。

### 6.2 本次恢复用的命令

```bash
fastboot flash boot_b 'C:\...\boot_b_l10c.img'   # 192 MB，键盘修复 + TDM set_fmt + capture + l10c always-on
fastboot erase dtbo_b                            # Armbian 槽 dtbo 清空
fastboot --set-active=b
fastboot reboot
# 约 40–70 s 后: ssh root@172.16.42.1 'uname -r'  →  6.12.58-current-sm8250
```

### 6.3 切换系统

```bash
fastboot --set-active=a   # 切 Android
fastboot --set-active=b   # 切 Armbian
```

> Android 侧若要真正可用，`super` 必须是完整镜像（本次救援中途中断过一次 8 GB 的 `super` 刷写，Android 因此只能进 recovery）。补刷即可：
> ```bash
> fastboot flash super <ROM>\images\super.img     # 8 GB，USB2 下约 20–40 min
> ```

---

## 7. 禁忌清单（每条都有代价）

1. **永不回锁**：`fastboot flashing lock` / `oem lock` —— 解锁极难，且回锁后 `devinfo` 状态不匹配会直接砖死。
2. **永不刷 `devinfo`**（LUN0 sda16，attrs bit60）：它保存解锁标志（偏移 144 处 `0x01`）。
3. **永不直接刷 ROM 的 `gpt_main*.bin` / `gpt_both*.bin`**（§2.2）。
4. **永不刷 `dd bs=512 count=34` 之类半截 GPT dump**（4096 B 扇区下这是错的尺寸）。
5. **不要用 `fastboot boot` 的成功与否判断"分区内容对不对"** —— 它是引导器层面的测试；`fastboot continue` 才是判定 GPT 的关键。
6. **不要在 UFS 忙时写 GPT**（先杀掉占用 fastboot 的长时间刷写任务，例如后台的 8 GB `super`）。
7. **不要把 rootfs 和 `userdata` 混为一谈**（§3.2）。
8. 备份时要 **dump 全部 6 个 LUN 的 `sfdisk -d`**，不要只 dump `sda`。

---

## 8. 复盘：本次走过的死路（避免重复浪费时间）

| 尝试 | 结果 | 为什么无效 |
|---|---|---|
| 重刷 `boot_a`/`boot_b`（备份里的已知good镜像，md5 校验一致） | 无效 | 分区内容本来就没坏 |
| 恢复 `dtbo_a/b`、`misc`、`metadata`、`logfs`、`oops` | 无效 | 同上 |
| 恢复 `vbmeta_a/b`、`vbmeta_system_a/b`（含 `--disable-verity`） | 无效 | 设备已解锁，AVB 非强制 |
| 批量恢复 26 个安全/引导链分区（xbl/abl/tz/hyp/aop/keymaster/cmnlib…） | 无效 | 内容不是问题 |
| 再批量恢复 95 个分区 | 无效（9 个在其他 LUN 上失败） | 内容不是问题 |
| 全量刷原厂固件（两槽）+ `cust.img` | 无效 | 内容不是问题 |
| **`fastboot flash partition:0..5 <原厂 gpt_both>`** | **引导恢复 ✓ 但布局被毁 ✗** | §2.2 模板陷阱 |
| **`fastboot flash partition:0..5 <按设备表重建的 gpt_both>`** | **✓✓ 引导恢复且布局无损** | 正确解 |

关键判据始终是这一行：

```
fastboot continue   →   Resuming boot   OKAY
```

它从 `Failed to load image from partition: Device Error` 变成 `OKAY`，就说明引导器已能按 GPT 正确定位并读取启动分区。

---

## 9. 附录

### 9.1 工具与备份位置

| 路径 | 用途 |
|---|---|
| `work/tools/gptpatch.py` | GPT 字节级补丁 + 解析/校验原语（`hdr_get`/`hdr_fix`/`build_lun`/`verify`） |
| `work/tools/rebuild-gpt.py` | 从设备表 + 原厂模板生成 6 个几何正确的 GPT 并自校验 |
| `work/tools/gptbuild.py` | 早期"重新序列化"实现（含与出厂文件的往返测试，保留作格式参考） |
| `work/tools/devstate.sh` | 一句话探测 fastboot/adb/ssh 状态 |
| `work/tools/flash-boot.sh` | 统一的刷写+验证循环（`<img>` / `--verify-only` / `--rollback`） |
| `backup/partbackup/` | 设备分区表 dump（`proc_partitions.txt`/`blkid.txt`/`gpt_sfdisk.bak`/`gpt_parted_map.txt`）+ 112 个分区镜像 |
| `backup/partbackup-0921-2212.tar.gz` | 上述 dump 的打包（794 MB） |
| `rom/elish_images_OS1.0.2.0.TKYCNXM_13.0/` | 完整原厂 fastboot ROM（含 `gpt_main*.bin`、`super.img`、`boot.img` 等） |

### 9.2 命令速查

```bash
# 状态
fastboot devices
fastboot getvar current-slot
fastboot getvar slot-retry-count:a
fastboot getvar unlocked            # 必须 yes
fastboot continue                   # 判定引导器能否加载镜像

# 刷 GPT（修复）
fastboot flash partition:N gpt_bothN_fix.bin         # N = 0..5

# 槽与启动
fastboot --set-active=b
fastboot erase dtbo_b               # Linux 槽
fastboot flash boot_b boot_b_l10c.img
fastboot reboot

# 设备侧取表（供下次救援重建 GPT）
cat /proc/partitions; blkid; sfdisk -d /dev/sdX
```

### 9.3 术语

* **GPT 槽元数据**：分区项 `attributes` 与 `type GUID` 中承载 A/B 槽状态的部分，`qbootctl` 通过 UFS BSG 直接改写。
* **`partition:N`**：fastboot 的 GPT 伪目标（N = UFS LUN 号），ABL 内置的官方分区表写入通道。
* **`NUM_DISK_SECTORS`**：Qualcomm rawprogram XML 的盘容量占位符，由刷机工具在写入时替换；**未替换的 GPT 文件不可用**。
* **主/备 GPT**：主表在 LBA0..5（4096 B 扇区），备表在盘尾 5 个扇区；备表的条目数组在原厂文件里位于偏移 0。

---

*本文基于 2025-09-22 的实际救援过程整理；修复后设备实测正常启动（`6.12.58-current-sm8250`，根为 `/dev/sda36`），分区布局 `esp`/`linux` 完整保留，未回锁。*
