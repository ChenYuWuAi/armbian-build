---
name: elish-kernel-bringup
description: 小米平板 5 Pro（elish / SM8250-AC）Armbian 内核与设备树改动的完整流程：远程机编译、补丁入库 fork、只换 DTB 的落地与开机验收、回滚。
whenToUse: 任务涉及给 elish 平板改内核/DTS/驱动、调 CPU 频率、改充电/温控、刷启动镜像、或"必须用远程编译机 + 结果要能重启验收/可回滚"时。
---

# elish 内核 / DTS 改动开发流程（远程编译 → 补丁入库 → 落地验收）

## 0. 现场拓扑（每次先核对，不要凭记忆）
| 角色 | 位置 | 说明 |
|---|---|---|
| 平板 | 本机 `xiaomi-elish`，Armbian 26.8.1，内核 `6.12.58-current-sm8250` | **只在平板跑/验收，不在平板编译** |
| 编译机 | `ssh pc`（WSL2 `CHENYU-GEEKPRO`，16 核，`aarch64-linux-gnu-gcc-12`） | 所有内核/DTS 编译、补丁生成 |
| 仓库 | fork `git@github.com:ChenYuWuAi/armbian-build.git`，分支 `elish-charging-6.12` | 平板 clone `~/axis_rnd/from-github/armbian-fork`（sparse, origin=SSH）；PC clone `~/axis_rnd/work/kernel/build/armbian-build`（remote 名 `fork`） |

`~/.ssh/config` 已有 `Host pc`（frp `frp-dad.com:24802`，公钥 + BatchMode）。远程一律 `ssh pc '...'`。
内核树：`linux-6.12.58-gh.tar.gz` 解包 → 按文件名顺序打 `patch/kernel/archive/sm8250-6.12/*.patch` → `build/device.config` → `make -j16 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules dtbs`。

## 1. 定位：机制 → 硬件证据 → 对比原厂
1. **读机制**：先在内核源码里找到"谁把功能关掉了"。例：`drivers/cpufreq/qcom-cpufreq-hw.c` 的
   `qcom_cpufreq_hw_read_lut()` 会拿 DT `operating-points-v2` 逐条交叉校验硬件 EPSS LUT，
   DT 里没有的频点 → `dev_pm_opp_adjust_voltage()` 返回 -ENOENT → `CPUFREQ_ENTRY_INVALID` 丢弃。
2. **拿硬件/寄存器证据**（root，比日志硬）：`/dev/mem` 直读寄存器。
   例：EPSS LUT = `mmap(0x1859{1000,2000,3000} + 0x100)`，row 4B，`freq = 19.2MHz*lval/1000`，
   `core_count = bits[18:16]`，`LUT_TURBO_IND=1`。
3. **对比原厂**：vendor 源码 `~/axis_rnd/work/kernel/elish-kernel`、`/home/axis/src/xiaomi-elish-kernel`
   （`MiCode/Xiaomi_Kernel_OpenSource` 分支 `elish-r-oss`）；原厂 DTS 参考 `~/axis_rnd/src/elish-downstream-refs/`。
   差异点=改动点（elish 频率一例：下游不查 DT，直接 `dev_pm_opp_add()` 每条 LUT ⇒ 原厂能跑满 3.1872GHz）。
4. 结论落成表：**现象 / 现状 / 根因 / 证据**，再动手。

## 2. 出补丁（在编译机上生成，别手写 hunk）
```bash
# 1) 先确认补丁能在"纯净序列"上干净应用（否则构建会静默 SKIP！）
V=~/axis_rnd/work/kernel/build/verify; rm -rf $V; mkdir -p $V
tar xzf ~/axis_rnd/work/kernel/build/linux-6.12.58-gh.tar.gz -C $V
cd $V/linux-6.12.58
for p in $PDIR/[0-9]*.patch; do patch -p1 --forward --batch --dry-run <"$p" >/dev/null 2>&1 \
   && patch -p1 --forward --batch <"$p" >/dev/null 2>&1 && echo "OK  $(basename $p)" || echo "SKIP $(basename $p)"; done
# 2) 用 diff -u 机械生成补丁（带 a/ b/ 前缀），别手写
diff -u --label a/$D --label b/$D orig.dtsi new.dtsi > /tmp/new.patch
# 3) 放到 $PDIR/NNNN-<subject>.patch（编号=应用顺序，追加在最后）
```
- 板级改动放**板级 dtsi**（`sm8250-xiaomi-elish-common.dtsi`），不要动共享的 `sm8250.dtsi`（会影响真的 865 机型）。
- 同类机型已有做法要**对齐**：如 Lenovo Xiaoxin Pad Pro 12.6（同 SM8250-AC）就是
  `&cpu7_opp_table { opp-3187200000 { opp-hz=/bits/ 64 <3187200000>; opp-peak-kBps=<8368000 51609600>; }; };`
- 入库：**只在这一个分支上**开发，不建临时分支。`git add` → `git commit` → `git push fork HEAD:elish-charging-6.12`；
  平板/PC 两处 clone 都要同步（见坑 5.6）。

## 3. 只改 DTB 时：不必整核重编，但必须"逐节点 diff"
```bash
# 在编译机上编 DTB（干净序列）
make -j16 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- qcom/sm8250-xiaomi-elish-csot.dtb
# 注意目标要写 qcom/xxx.dtb，写成 arch/... 会被再加一次前缀而报 No rule
```
落地前**必须**证明"新旧 DTB 只差你改的那部分"：从**实际在跑的 boot 分区**里抽出 DTB 再反编译对比。
```bash
source /boot/armbianEnv.txt        # abl_boot_partition_label=boot_b
sudo dd if=/dev/disk/by-partlabel/$abl_boot_partition_label of=/tmp/bootlive.img bs=1M count=32
python3 - <<'EOF'
d=open('/tmp/bootlive.img','rb').read()          # Android boot: kernel_addr 是加载地址，
page=int.from_bytes(d[36:40],'little')           # 文件偏移 = page_size(0x1000)
ksize=int.from_bytes(d[8:12],'little'); blob=d[page:page+ksize]
fdt=blob.find(b'\xd0\x0d\xfe\xed'); open('/tmp/live.dtb','wb').write(blob[fdt:])
EOF
dtc -I dtb -O dts -o live.dts /tmp/live.dtb
dtc -I dtb -O dts -o new.dts  <新编 dtb>
diff live.dts new.dts            # 只应出现你新增的节点（phandle 重编号可忽略）
```

## 4. 刷入 + 开机自动验收 + 回滚
```bash
K=$(uname -r); TS=$(date +%Y%m%d-%H%M%S)
P=/usr/lib/linux-image-$K/qcom/sm8250-xiaomi-elish-csot.dtb
sudo cp -a $P $P.bak-3g2-$TS                       # 备份，必须
sudo install -m644 <新 dtb> $P
test -f /boot/vmlinuz-$K.stock-bak-3g2 || sudo cp -a /boot/vmlinuz-$K /boot/vmlinuz-$K.stock-bak-3g2
sudo install -m644 <自制 Image> /boot/vmlinuz-$K    # ← 见坑 5.1，绝不能省
sudo /etc/kernel/postinst.d/zz-update-abl-kernel $K # 重生成镜像 + dd 到 boot_b
```
- ABL 钩子用的是 **`/usr/lib/linux-image-$K/qcom/*.dtb`**（不是 `/boot/dtb-$K/`）。
- 装个一次性 systemd 单元在开机时把自检写到 `~/<task>-verify.txt`（频率/温度/模块/dmesg 关键行）；
  脚本末尾 `systemctl disable <unit>` 自禁，避免每次开机都跑。
- 回滚脚本必须和改动一起留：恢复 `.bak-*` DTB → 重跑钩子 → 重启；**不要**顺手恢复原厂 vmlinuz。
- 重启会打断 DSH 会话（跑在桌面会话里）：确认好再重启，重启后第一件事读验收文件。

## 5. 必踩的坑清单
1. **`/boot/vmlinuz-$K` 可能不是当前在跑的内核**。现场那次它是原厂 `build@armbian` #1，而在跑的是自制 #59。
   ABL 钩子取 `/boot/vmlinuz` ⇒ 不先替换，重启就悄悄退回原厂内核。
   校验：镜像里 kernel blob（`Image.gz`+DTB）解出来的 Image md5 要等于自制 `Image` 的 md5。
2. **Android boot 头**：`kernel_addr=0x8000` 是加载地址；文件里 kernel blob 从 `page_size`(0x1000) 开始，
   `kernel_size` 含 `Image.gz` + 紧随其后的 DTB（DTB 起点=找 `\xd0\x0d\xfe\xed`）。
3. **运行内核里模块全是 `[permanent]`**（`.exit.text` 被丢），`rmmod` 一律 EBUSY；驱动 `.ko` 改动只能重启生效。
4. **脏树/干净树**：手工改过的构建树再跑一遍补丁序列会出现**重复节点**（DTC `ERROR (duplicate_node_names)`，
   所有 qcom 板 DTB 全挂）。要重现构建就先从 tarball 重新解包再按序打补丁。
5. **宿主工具链**：新 gcc 会让 `tools/bpf/resolve_btfids` 的 libbpf 因 `-Werror=discarded-qualifiers` 挂，
   用 `EXTRA_CFLAGS=-Wno-error=discarded-qualifiers` 或旧版宿主 gcc。
6. **ssh 里跑 git**：`git fetch` 会吃掉 heredoc 的 stdin（用 `ssh pc "cmd"` 参数形式或 `</dev/null`）；
   PC 上 `git fetch fork` 有时不更新跟踪引用，用 `git fetch fork <branch>` 后 `git rebase FETCH_HEAD`；
   平板 clone 的 origin 之前是 https（拉取会卡），改 `git remote set-url origin git@github.com:...` 走 SSH。
7. 补丁生成自脏树会**带上无关改动或干脆打不上**（本次 0055 在纯净树上就 SKIP）——纯净树验证不可省。
8. 大文件/镜像不入库，在 README 里登记路径；原厂源码、未改动的上游基线也不入库。

## 6. 本次实例（3.1872GHz 解锁）
- 现象：`policy7` 只有 2841600，dmesg `failed to update OPP for freq=3187200` ×2。
- 证据：LUT domain2 `[20] 0x400300a6 lval=166 cc=3 → 3187200 kHz`（DT 缺这档）；
  原厂下游不做 DT 校验。
- 补丁：`0056-arm64-dts-qcom-sm8250-xiaomi-elish-add-3.2GHz-prime-OPP.patch`（只加一档 OPP）。
- 验收：重启后 `scaling_available_frequencies` 末尾 `3187200`，`cpuinfo_max_freq`/`scaling_max_freq`/`scaling_cur_freq` = 3187200，
  dmesg 无 `failed to update OPP`，内核仍是 #59。
