# elish (Xiaomi Pad 5 Pro) Armbian 抢救记录 — 2026-09-21

## 结论:已恢复 ✅

```
Armbian 26.x / kernel 6.12.58-current-sm8250
root  = /dev/sda36  (GPT 里名为 linux 的分区, 130 GB, 已 resize2fs 填满分区)
WiFi  = wlp1s0 已连接 3SE-305-5G
USB   = usb0 172.16.42.1/16 (gadget), SSH 可用
systemctl --failed = 0
Active slot: _a
```

PC 侧访问(USB-C 接电脑):

```powershell
# 管理员 PowerShell(只需一次)
Get-NetAdapter | ? InterfaceDescription -like '*Ncm*'      # 记下网卡名, 例如 "以太网 2"
New-NetIPAddress -InterfaceIndex <idx> -IPAddress 172.16.42.2 -PrefixLength 16
```

```bash
ssh root@172.16.42.1        # 已装本机公钥; 密码仍是 1234
```

---

## 1. 真正的根因(两个, 都在我这边)

### 1.1 我重建的 rootfs 缺 `/dev` 等空目录 → kernel panic

用 `backup/rootfs.tar.xz` 重建 ext4 时, tar 里**根本没有** `dev/ proc/ sys/ run/ tmp/ mnt/`
(打包时被排除了, 顶层只有 bin boot etc home lib media opt root sbin selinux srv usr var)。

initramfs(`/init`)最后一步是:

```sh
exec run-init ${drop_caps} "${rootmnt}" "${init}" "$@" \
        <"${rootmnt}/dev/console" >"${rootmnt}/dev/console" 2>&1
```

`${rootmnt}/dev/console` 不存在 → `/root/dev/console: No such file or directory`
→ init 退出 → **Kernel panic**。这与屏幕上看到的报错完全一致。

**修复**: 重建镜像时补齐 `dev proc sys run tmp mnt`(权限 755, `tmp` 1777),
并建好 `/dev/{console,null,zero,full,random,urandom,tty,ptmx}` 与 `fd/stdin/stdout/stderr` 链接。

### 1.2 诊断脚本自己造成的 "e2fsck cannot continue, aborting (exit 8)"

第一版诊断镜像里我做了 `mount -o ro` 挂载测试却**没有卸载**, 之后 initramfs 的
fsck 步骤就对着"已挂载"的文件系统跑 e2fsck → 直接 abort(exit 8)。
**这反而证明 rootfs 本身是能挂载的**。后续诊断脚本已改成挂载后必须 umount。

---

## 2. 分区表(GPT)恢复

设备原厂 GPT 里 `userdata` 是 **0 尺寸占位符**, 靠 ABL 首次启动时自动扩展到盘尾;
Armbian 时期的分区表(来自本机 `getvar_all.txt` 快照)是:

| 分区 | 起始 LBA | 结束 LBA | 大小 |
|---|---|---|---|
| userdata | 2686976 | 27099609 | 99.99 GB (0x17481DA000) |
| esp | 27099610 | 27221721 | 500.17 MB (0x1DD00000) |
| linux | 27221722 | 61880025 | 141.96 GB (0x210D800000) |

`build_gpt_elish.py` 就是按这个表把 `gpt_both0.bin` 改成可用的 LUN0 GPT
(主/备表项 CRC + 两个 header CRC 全部重算), 然后

```bash
fastboot flash partition:0 gpt_elish0.bin
```

**注意**: ABL 会把**最后一个分区自动扩展到盘尾**(实际盘 = 61,880,320 扇区),
所以刷完后 `linux` 变成 0x210D921000(比原始大 288 扇区), 这是正常行为、无害。

---

## 3. 这次实际刷写的内容

| 项 | 内容 | 来源 |
|---|---|---|
| LUN0 GPT | `gpt_elish0.bin` | `build_gpt_elish.py` 重建 |
| LUN4 GPT | `gpt_both4.bin`(原厂) | ROM images |
| boot_a | Armbian 启动镜像 md5 `ded90d33d934bafa1df32ece0b571fdf` | `backup/boot_a.img.gz` |
| vbmeta_a | Armbian 时期 vbmeta(AVB 关闭) | `backup/vbmeta_a.img.gz` |
| dtbo_a | 擦除(全 0) | elish wiki 要求 |
| linux | **重建的 rootfs**(补齐 /dev 等 + 内置 usb0 服务) | `work/rootfs_new.simg` |

`linux` 上跑过一次 `resize2fs`, 现已填满分区(34,658,593 块 / 127 GiB 可用)。

---

## 4. 为防止"SSH 连不上"做的持久化修复

原 initramfs 里 `setup-usbgadget-network.sh` 在 UDC 绑定后**立刻** `ip a add ... dev usb0`,
而 netdev 是异步创建的 → 经常 race 失败 → usb0 永远没 IP(之前连不上就是这个原因)。

新 rootfs 里写入了:

* `/etc/systemd/system/usb0-static.service`(已 enable)
  → `ip link set usb0 up; ip addr replace 172.16.42.1/16 dev usb0`
* `/etc/NetworkManager/conf.d/99-usb0-unmanaged.conf`
  → 让 NM 不要插手 usb0(否则 NM 会把 IP 冲掉)

所以**以后每次启动 USB 网络都可用**, 不再依赖 initramfs 里那步 race。

---

## 5. 现场保留的可复用产物

| 文件 | 说明 |
|---|---|
| `build_gpt_elish.py` / `gpt_elish0.bin` | LUN0 GPT(含 linux/esp/userdata, 可直接 `fastboot flash partition:0`) |
| `work/rootfs_new.simg` | 8.6 GB sparse, 可直接 `fastboot flash linux`(130 GB ext4, UUID 21ce0d2d) |
| `work/rootfs_new.img` | 上面的 raw 版(130 GB 逻辑) |
| `work/build_rootfs.sh` | 从 `backup/rootfs.tar.xz` 重建 rootfs 的完整脚本(含 /dev 补齐 + 服务注入) |
| `work/boot_diag.img` + `work/build_diag.sh` + `work/diag.sh` | 内存诊断/救援镜像: 修 usb0、补目录、把内核看到的分区/blkid/e2fsck/挂载测试结果通过 UDP 发回 PC |
| `work/boot_a_flash.img` | md5 `ded90d33…` 的 Armbian 启动镜像 |
| `backup/` | 116 个分区镜像 + `rootfs.tar.xz` |

救援镜像用法(fastboot 下, 内存启动不写盘):

```bash
fastboot boot work/boot_diag.img
# 设备里 initramfs 会: 补齐 rootfs 缺失目录 + 装 usb0 服务 + 发诊断报告
```

---

## 6. 遗留事项 / 注意

* **rootfs 内容 = 03:35 的备份快照**。04:00 之后那批功放(CS35L41)实验文件不在里面
  (`/root/amp_backup`、自编译模块等); 音频课题按你的要求已停, 需要时从 `backup/` 和
  `/mnt/c/axis_rnd` 里取。
* **Android(slot b)未动**: super/boot_b 都是官方的。但要真进 Android 需要
  `fastboot set_active b` 并在 recovery 里清一次 userdata(现在 userdata 是 93 GB 的旧数据)。
  Linux 与 Android **不共享**分区: Linux 在 `linux`, Android 在 `userdata`。
* `esp`(477 MB)目前是空分区、无文件系统, Armbian 不用它(不写 fstab), 属正常。
* 关不掉/重启卡住是**原版就有的 SLPI 传感器 DSP 崩溃**(见 `SESSION_SUMMARY.md` §2.4),
  所以重启请用 `sudo reboot -f`。
* 一切 GPT 写入都可逆: 原厂 `rom/.../images/gpt_both0-5.bin` 原封未动。
* **不要** `fastboot oem lock` / `flash_all_lock.*` / MiFlash "clean all and lock"。
