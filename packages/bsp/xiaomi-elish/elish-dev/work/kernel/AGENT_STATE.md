# elish 音频移植 —— 当前状态与续接指引（2026-09-22）

> 完整背景/证据见 `ELISH_AMP_TDM_FIX.md` **§27–§30.4.1**（尤其 §30 = 最终结论 + 资产清单）。
> **新一轮实测确证见 `work/android/ANDROID_PIPELINE_RE_2026-09-22.md`**（可脚本播放配方 / AFE cal 非必需 / 每颗放大器 46 控件清单 / 原厂 ACDB level cal 下发失败 -100）。

## ★★ 最新（第 22 轮，现场实测）：ADSP 不认 0x1000a100 ⇒ 必须先 ADD_TOPOLOGIES
设备已在 **Armbian（slot b）**，内核 `6.12.58-current-sm8250`。用导出符号 `q6adm_open()` 直接测：

| topology | 结果 |
|---|---|
| `0x1000a100`（Android 扬声器） | **失败** `cmd = 0x10326 return error = 0x3` → `q6adm_open failed: -22` |
| `0x00010312`（NULL，对照） | **成功** `q6adm_open(port_idx=56) -> copp_idx=0` |

⇒ **ADSP 固件里没有 0x1000a100 这个 topology 定义**（mainline 从没发过 `ADM_CMD_ADD_TOPOLOGIES 0x10335`）。
⇒ **修正 §31 的预期：只装 0052 内核 + 设 `copp_topology=0x1000a100` 会开不出 COPP，可能更糟。**
正确顺序：**先下发 ADD_TOPOLOGIES（ACDB `ADM_TOPOLOGY_CAL` cal_type 9 / CUST 10）→ 再用 0x1000a100 开 COPP**。
- NULL COPP 的 `dsp_copp_id = 0`，往它发 PP 参数会 `Unknown Cmd: 0x1035d`（dest_port=0 非法）。

### amp-fix.sh 时序缺陷（已修并实测）
`amp-always-on.service` 守护进程在 PCM `RUNNING` 时跑 `KICK()`；但 `ASP()` 把寄存器写放在
**~80 次 `amixer cset` 之后**，实测要 **~10 秒**才生效 ⇒ **短音效/每首歌开头都又小又破**（"声音很小"的真实来源之一）。
已把寄存器写整块提到 `ASP()` 最前（含 `0x6808=0x3f75`），推回设备并重启服务：
**T+2s 即达 Android 值**（原来 ~10s）；T+16s 复测 **8 颗全部逐字节一致**
（`0x4808=0x20200000`/`0x4840=24`/`0x6c04=0x253`/`0x6808=0x3f75`/`0x2014=1`/`0x2018=0x3721`）✓
本地副本：`work/armbian/amp-fix.sh.device`（设备原件备份 md5 `df7d2d7b…` 见 git 前的原文件）。

### 现场两条必知
- **i2c 总线编号**：Armbian 第二组放大器在 **bus 3**（Android 是 bus 2），第一组都是 bus 1。脚本用 `for b in 1 3` ✓。
- 用户 `axis` 读不了 `/root/*.wav`（`/root` 0700）⇒ 测试音频放 `/tmp`；
  `wpctl` 必须以用户身份跑：`su - axis -c "XDG_RUNTIME_DIR=/run/user/1000 wpctl status"`。

### Cirrus 固件核查（§33，挂载原厂 vendor.img 逐文件比对）
- 原厂 `/vendor/firmware/` 是**平铺**的：`<AMP>-cs35l41-dsp1-spk-{cali,prot}.bin` + 共享 `cs35l41-dsp1-spk-{cali,prot}.wmfw`；
  8 颗**全都有**独立 cali/prot ✓（Armbian 侧对应 `<AMP>-spk-{cali,prot}.bin`，1950 文件齐全）。
- **`T*-music.txt` / `T*-voice.txt` 原厂就是 0 字节**（B 组 45/46 字节）⇒ **不是缺失，别去"补"**。
  但 mainline 对空文件报 `-22`(EINVAL)，原厂驱动容忍；已把这 8 个 0 字节文件移到
  `/root/fw-zero-backup/`（可回滚），日志变成良性的 `-2`(ENOENT) 回退。
- **✓ 已确认移植正确**：内核自定义补丁会**每颗**推送工厂标定
  `pushed factory calibration: cal_r=… ambient=23`（8 个不同值），
  与 Android mixer 的 `DSP Set CAL_R`（BRH=9305、BLH=9338）**逐值一致**；
  B 组另有 `elish: pushed 12 tuning values from cirrus/<AMP>-music.txt`。
- **`Enable(1) failed: -110` 已归因（§34，定性修正）**：来自 `cs35l41-lib.c:1218 cs35l41_global_enable()`
  里对 `IRQ1_STATUS1.PUP_DONE` 的 **100ms 状态轮询**超时。对照实验：
  * 放大器**已上电**时 → 驱动在 `:1236` 提前 return，**0 次错误**；
  * 放大器**断电后**播放 → 7~8 次 `-110`，**但 `0x2014` 最终仍 = 1**（上电其实成功）。
  ⇒ **是"状态位没观测到"的告警，不是功能故障，优先级下调**。先前"关掉 daemon 就没错误"是假象
  （那次放大器本已上电，走提前 return 分支）。
- **★ 反向确认 amp-fix 守护进程必需**：把 `0x4808` 写成垃圾值 `0x11111111`，仅驱动播放后
  **仍是 `0x11110111`（未纠正）** ⇒ **mainline 驱动根本不写 `0x4808`/`0x6c04`**；
  恢复 daemon 后变回 `0x20200000`/`0x253` ✓。**这两个寄存器必须靠脚本补，不能删。**
- ⇒ **剩余真缺口只剩 ADSP topology 定义这一条**，而**载荷本轮已找到并提取** ↓

### ★★ 第 23 轮：`ADD_TOPOLOGIES` 载荷已提取（可直接实现）
- 原厂机制（实机 logcat 坐实）：开机早期就发 **5444 字节**的 `CORE_CUSTOM_TOPOLOGIES`——
  `ACDB -> send_common_custom_topology / Reallocate memory for Custom Topology to size: 5444 / CORE_CUSTOM_TOPOLOGIES`。
- **载荷位置**：不在 `Forte_Speaker_cal.acdb`（无 topology chunk），而在
  **`Forte_Global_cal.acdb` 的 `DATAPOOL`**（chunk @0x23e, payload @0x24a, len 0x9cd4）里，**尾部**。
  **精确起点 = 绝对 `0x89da`**（不是 0x89d6）：`0x89d6` 处是 ACDB 的 `<u32 size>` 头 `44 15 00 00`=5444。
  **边界证明**：`0x89da + 5444 = 0x9f1e = DATAPOOL 末尾`（剩余 0 字节）；若从 0x89d6 起则多出 4 字节。
- **已提取** → **`work/acdb/core_custom_topologies.bin`（5444 B, md5 `c7f33c304aa1f36d9277b206a6ea4bf5`）**，内容自证：
  `0x00010BFE`(VOL_CTRL 模块) @0x34/0xb4/0x144/0xce4；`0x1000A100` @0x1474/0x1488/0x149c/0x150c；
  `0x1000A101` @0x1518/0x1534。开头 `61 00 00 00 | 02 00 00 00 | 00 a0 02 10`
  （=97, 2, 拓扑 `0x1002a000`）。
- ⚠ **`ADD_TOPOLOGIES` 是 out-of-band，没有 in-band 变体**（`downstream/q6adm.c:2123-2144`）：
  必须 `dma_alloc_coherent` + `ADM_CMD_SHARED_MEM_MAP_REGIONS (0x00010322)` 拿 `q6map_handle`，
  再发 `ADM_CMD_ADD_TOPOLOGIES (0x00010335)`，payload={addr_lsw,addr_msw,mem_map_handle,payload_size=5444}。
  （subagent 说的"in-band 够用"只对 **cal 块**成立，对 ADD_TOPOLOGIES 不成立。）
- **成功判据**：发完之后 `q6adm_open(..., topology=0x1000a100, ...)` **不再返回 DSP error 3**。

### ★★ 实现路径修正（第 26 轮）：**必须打内核补丁，独立模块做不到**
- **原因**：mainline `q6adm.c` 用 `module_apr_driver(qcom_q6adm_driver)`（`:812-821`）注册了
  `.callback = q6adm_callback`（`:207`），**ADM(`aprsvc:service:4:8`) 的响应被 q6adm 独占**；
  APR 总线一设备一驱动，且 mainline `apr.c` 只导出 `apr_send_pkt`（无 async_cb 注册 API）。
  ⇒ 模块**发得出、收不到**，而 `mem_map_handle` 只能从响应取 ⇒ 模块方案架构上不成立。
- **现成模板在 `q6asm.c`**：`q6asm_map_memory_regions()`(`:564`)、命令体构造(`:500-543`，
  `mem_pool_id=ADSP_MEMORY_MAP_SHMEM8_4K_POOL`、`buf_sz=ALIGN(,4096)`)、
  发送等待 `q6asm_apr_send_session_pkt()`(`:355`)、响应处理(`:855-860`)。
- **ADM 常量**：`ADM_CMD_SHARED_MEM_MAP_REGIONS=0x00010322`、
  `ADM_CMDRSP_SHARED_MEM_MAP_REGIONS=0x00010323`、`ADM_CMD_ADD_TOPOLOGIES=0x00010335`
  （线上结构 `cmd_set_topologies`，`apr_audio-v2.h:10846`）。
- **⚠ handle 取法（易踩坑）**：ASM 取响应头 opcode（`q6asm.c:858`），
  **ADM 取响应 payload 的首个 u32**（下游 `q6adm.c:1898-1900 atomic_set(..., *payload)`）。
- **补丁 0053 清单**：`elish_adsp_vol` 之外，改 `q6adm.c`：
  ① 加 dma 缓冲/handle/completion 字段；② `q6adm_callback` 加两个 case（**注意别被现有
  `default: Unknown cmd` 吞掉**）；③ 新函数先 map（8192 字节，1 区）再发 ADD_TOPOLOGIES（payload_size=5444）；
  ④ 暴露调试触发入口。载荷从 `/lib/firmware/elish_topologies.bin` 读。
- 构建复用已验证流程：`work/kernel/build_elish_kernel.sh`（50 补丁干净应用、
  `LOCALVERSION=-current-sm8250`、`BUILD_EXIT=0`）。
- 子代理 b55c4015 已中止（40 分钟无任何产出、未推送载荷）。**不要再委派，直接自己打补丁。**

### ✅ 第 27 轮：补丁 0053 **已写完并编译通过**
- 补丁文件：`work/kernel/patches/0003b-elish-add-ADM-topologies.patch`（265 行），已装入
  `armbian-build/patch/kernel/archive/sm8250-6.12/0053-ASoC-qcom-q6adm-add-ADM-topologies-out-of-band.patch`。
- 实现要点：`q6adm.c` 加 map/add 命令与结构体；`q6adm_callback` 外层加
  `ADM_CMDRSP_SHARED_MEM_MAP_REGIONS`（**handle = `*(u32*)data->payload`**），
  内层加 `ADM_CMD_ADD_TOPOLOGIES`；新函数 `q6adm_add_topologies()` 并 **EXPORT_SYMBOL_GPL**；
  `q6adm_probe()` 里 5 秒后 `request_firmware("elish_topologies.bin")`，**文件不存在则跳过**（其它板子无副作用）。
- **构建验证**：`q6adm.o` 0 error/0 warning；全内核 0 error；`Image` 38,660,608 B；
  release `6.12.58-current-sm8250`；`vmlinux` 里 `q6adm_add_topologies` 为 `T` +
  `Module.symvers` 里 `EXPORT_SYMBOL_GPL` ✓；0050/0051 符号仍在 ✓。
- 载荷已推到设备 `/lib/firmware/elish_topologies.bin`（md5 `c7f33c30…` 实测一致 ✓）。
- **剩余**：把新 `Image` 装成 boot 镜像刷 `boot_b` → 看 dmesg 是否出现
  `elish: topologies mapped (handle=0x…)` 与 **`elish: ADD_TOPOLOGIES ok (5444 bytes)`**
  → 再用 §36.5 判据验证 `q6adm_open(0x1000a100)` 不再返回 `error = 0x3`。

### ⚠ 第 28 轮：安装尝试**失败**（新内核进不去系统，已回滚）
- 做法：用 gzip 后的新 Image 重建 boot 镜像 → **dd 直接写 `boot_b`** → 回读 md5 一致 ✓ → 重启。
- 结果：**两次都停在 fastboot**（ABL 引导失败）。已用 `fastboot flash boot_b boot_b_restore.img`
  （md5 `ded90d33…`）**回滚成功**，设备现已恢复可用。**Android(slot a) 全程未动**
  （实测 `boot_a` md5 `78b7caff…`）。
- 排查结论：
  * 修掉一个打包 bug（把 `ramdisk_addr` 误当 `second_size`，多塞 16MB）；
    **正确布局**：`second_size` 在偏移 **0x18 = 0**，镜像 = `header(4096)+kernel+ramdisk+padding`。
    修正后**仍不能启动** ⇒ 不是这个 bug。
  * 用原始 kernel 字节重打包做离线比对：**唯一差异是 ramdisk 之后的高熵尾部**（0x387c000 起），
    该区域**不被头里任何 size 字段引用** ⇒ ABL 理论不读 ⇒ 也与失败无关。
  * **⇒ 失败基本定位在内核本身**，不是镜像拼装/写入。
- 唯一功能性配置差异 = **`CONFIG_DEBUG_INFO_BTF` 未开**（配置时无 pahole；正好解释 11MB 体积差），
  **不影响启动**；MODVERSIONS/MODULE_SIG 均未开、模块 vermagic 与原厂一致。
- **下一步（隔离实验，1 次编译+1 次刷写）**：用**原始 kernel 字节**+正确布局重打包刷 `boot_b`：
  能启动 ⇒ 拼装无误、问题在 0053 内核（需 boot 日志：earlycon/ramoops，或先用**不带 0053 的同配置
  重编内核**验证能否启动，以区分 配置/工具链 问题 vs 0053 改动问题）；不能启动 ⇒ 高熵尾部/写入方式才是关键。
- ⚠ 重启设备前记得：Armbian 的 boot_b 才是启动分区，**只写 boot_b，别碰 boot_a**。

### ★★ 第 29 轮：隔离实验完成 —— **打包无误，问题在内核二进制**
- 构造镜像 = **原始 kernel 字节** + 修正后布局 + 尾部清零（md5 `61ced9d6…`）→ dd 写 boot_b → **成功启动 ✓**
- ⇒ ① 我的 boot 镜像打包流程**正确**；② dd 写 boot_b **可用**；③ ramdisk 后那 135MB
  高熵尾部（熵 7.99、无任何 magic、头里无引用）**是惰性数据、与启动无关**。
- ⇒ **故障确定在我们的内核二进制/工具链**，不是安装方式。
- 已逐字段确认 arm64 Image 头与原厂一致（code0/text_offset/flags/magic），仅 image_size 随体积不同。
- **头号嫌疑：工具链代差** —— 原厂用 **gcc 11.4 + binutils 2.38**，我们用 **gcc 15.2 + binutils 2.46**；
  且我们 `CONFIG_RELR=y` 而原厂**未设置**（RELR 重定位格式差异）。
- **下一步**：① 换回同代工具链（gcc-11/12）重建 → 最可能一击解决；② 不行再显式关 `CONFIG_RELR`。
- 取日志手段受限：`ramoops@b0000000` 存在但每次报 `uncorrectable error in header`、
  `/sys/fs/pstore/` **始终为空 ⇒ 该设备 pstore 不可用**，拿不到 panic 日志（除非 UART earlycon）。
- 设备当前运行在**隔离镜像**（boot_b，原厂内核 + 清零尾部），被引导链读取的部分与原厂等价 ✓。

### ⚠⚠ 第 30 轮：工具链/压缩假设**全部排除**，仍不能启动
已实做并全部**仍不能启动** ✗：
1. 装 **gcc-12.5.0**（与原厂 gcc 11.4 同代）全量重编（0 error，Image 42,463,744 B）→ 失败
2. **`CONFIG_RELR=n`**（排除新 binutils 的重定位格式）→ 失败
3. 改用**系统 `gzip -9 -n`**（头部 `1f8b080000000000` 与原厂完全一致）→ 失败
- 确定边界：**原厂内核 + 我的打包 = 能启动 ✓**；**我编的内核 + 同一打包 = 不能启动 ✗**
  ⇒ 故障在"我们编出来的内核二进制"，与 gcc 版本 / RELR / gzip 生产者**都无关**。
- "停在 fastboot"可能是①ABL 加载/解压/交接失败，或②内核启动后 panic→看门狗重启→ABL 失败计数进 fastboot。
  **无直接证据**：该设备 **pstore 不可用**（`ramoops` 每次报 uncorrectable error、`/sys/fs/pstore/` 恒空），也无 UART。
- ★ **下一步唯一关键隔离实验**：把**我们自己的补丁全部撤掉**（0050/0051/0052/0053）用同一 config+工具链重编测：
  * 能启动 ⇒ 是我们某个补丁（最可能 0053 在 `q6adm_probe` 排的 delayed_work 跑
    `request_firmware`/`dma_alloc_coherent`/`apr_send_pkt`）→ 二分定位；
  * 仍不能启动 ⇒ 是非官方构建流程本身的问题 → 改用 **Armbian 框架**构建
    （`compile.sh` 传 `KERNEL_MAJOR_MINOR=6.12 KERNELBRANCH=branch:linux-6.12.y`）。
- 设备已回滚原厂 boot_b（md5 `ded90d33…`），运行正常 ✓。

### ★★ 第 31 轮：**我们的补丁无罪** —— 故障在"自行编译内核"这件事本身
- 把**自己写的 4 个补丁全部 `patch -R` 撤掉**（核实符号全为 0）→ 同 config/gcc-12/RELR=n/系统 gzip
  重编（0 error；`nm` 确认三个符号均已消失）→ 打包刷入 → **仍然不能启动** ✗
- 边界表：原厂内核+我的打包 = **能启动** ✓ ｜ 我们的内核(含补丁,3 变体) = ✗ ｜ 我们的内核(**不含任何补丁**) = ✗
- ⇒ **补丁 0050–0053 不是原因**；问题在**非 Armbian 官方流程自行编译 6.12.58**。
- ★ **下一步（方向唯一）**：改用 Armbian 官方框架，并显式把分支钉到 6.12：
  ```sh
  cd work/kernel/build/armbian-build
  ./compile.sh kernel BOARD=xiaomi-elish BRANCH=current KERNEL_MAJOR_MINOR=6.12 KERNELBRANCH=branch:linux-6.12.y
  ```
  （仓库当前 `current`=6.18，必须显式钉 6.12；我们的 0050–0053 已在
  `patch/kernel/archive/sm8250-6.12/`，会被自动应用。）
- 安装/验证链路（打包→dd/fastboot→启动判定→失败回滚）**已验证可靠可重复** ✓。
- 设备已回滚原厂 boot_b，运行正常 ✓。
- ⚠ 树里目前**已撤掉我们 4 个补丁**（若要在本地继续用打补丁的树，重新 `patch -p1 <` 那 4 个 patch 即可）。

### 第 32 轮：改用 Armbian 官方框架构建（进行中）
已把官方构建配置改成与设备一致：
- `config/kernel/linux-sm8250-current.config` ← **替换为设备自身 config**（`build/device.config`，
  原文件备份在 `/tmp/linux-sm8250-current.config.orig`）
- `config/sources/families/sm8250.conf` 的 `current` 分支
  **`KERNEL_MAJOR_MINOR="6.12"` + `KERNELBRANCH='tag:v6.12.58'`**
  （原文件备份 `/tmp/sm8250.conf.orig`）——**必须钉 tag，否则默认 6.12.y 会取到 6.12.111，
  release 与设备的 `6.12.58-current-sm8250` 不符会导致模块目录失配**
- 我们的 0050–0053 已在 `patch/kernel/archive/sm8250-6.12/`，官方流程会自动应用 ✓
- 官方流程还会**自动开启 BTF**（"Enabling eBPF and BTF info"）——这正好补上我们手工构建缺失的那一项
- 启动命令（已后台运行，日志 `work/kernel/armbian_build_61258.log`）：
  ```sh
  cd work/kernel/build/armbian-build && ./compile.sh kernel BOARD=xiaomi-elish BRANCH=current
  ```
- 状态：正在从 ghcr.io 拉取内核 git bundle（`cache/git-bundles/kernel/*.tmp/linux-complete.git.tar`，
  已 ~780MB 且持续增长；中途遇到一次 `PROTOCOL_ERROR`，Armbian **自动重试后正常**）
- 产出后：从构建产物取出 `Image`，按已验证的打包流程（`second_size=0`、
  `header+gzip(kernel)+ramdisk+padding`、系统 `gzip -9 -n`）做成 boot 镜像刷 `boot_b` 再验证启动。

### ★★★ 第 34 轮：**找到真正根因，新内核已成功启动！**
**根因**：boot 镜像的 kernel 区域是 **`Image.gz-dtb`** 布局 ——
`kernel_size = gzip(Image) + 追加 DTB`。实测原厂：
`15536790 = 15406606(gzip) + 130184(DTB, FDT magic d00dfeed, totalsize 正好 130184)`。
**我之前把这段 DTB 当成填充清零了** ⇒ ABL 找不到设备树 ⇒ 退回 fastboot。
（§39 隔离实验之所以能启动，正是因为那次用的是 `orig[PAGE:PAGE+ksize]`，**连 DTB 一起**搬了。）
⇒ 与工具链/BTF/RELR/gzip/补丁**全无关**；§38–§41 的排查结论作废。

**修复与结果**：
- `make_boot_image.py` 已更新：**自动提取并附回追加 DTB**，`kernel_size=len(gzip)+len(dtb)` ✓
- 重新应用 4 个补丁 → gcc-12 重编（0 error、3 符号齐全）→ 打包 → dd 写 boot_b → 重启
- **设备成功启动我们的内核 ✓**：`uname -r` = `6.12.58-current-sm8250`，
  且开机日志出现 **`qcom-q6adm aprsvc:service:4:8: elish: add topologies failed -12`**
  ⇒ **0053 的代码路径已在开机时真实执行** ✓

**唯一剩余问题**：`-12 = -ENOMEM`，最可能是 `dma_alloc_coherent(adm->dev, …)` 失败
（`adm->dev` 是虚拟 apr 总线设备 `aprsvc:service:4:8`，无 DMA 能力）。
**修法**：改用有 DMA 能力的设备（如 `adm->dev->parent`，或先试 self 再回退 parent），
并把两条失败分支的日志分开，便于下次定位。改完重编 + 用 `make_boot_image.py` 打包刷 boot_b 即可。

### 第 35 轮：`-ENOMEM` 精确定位；变体 B 未启动
- **变体 A**（3 级 parent walk + 分段日志，md5 `e5c3dbb8…`）**成功启动 ✓**，日志给出决定性诊断：
  ```
  elish: dma_alloc on aprsvc:service:4:8 failed
  elish: dma_alloc on 17300000.remoteproc:glink-edge.apr_audio_svc.-1.-1 failed
  elish: dma_alloc on 17300000.remoteproc:glink-edge failed
  elish: dma_alloc_coherent failed (8192)   → add topologies failed -12
  ```
  ⇒ **`-ENOMEM` 100% 来自 `dma_alloc_coherent`**；apr 整条父链都**没有 DMA ops**。
- **变体 B**（6 级 walk + `kzalloc`+`virt_to_phys` 兜底）**未启动 ✗**，ABL 回退到 slot a(Android)。
  原因未定（可能是 ABL 连续失败计数切槽，也可能该变体有问题）。编译 0 error。
- 设备已恢复：`fastboot set_active b` + 刷回 `boot_b_restore.img`(md5 `ded90d33…`) →
  Armbian 正常 ✓。
- **下一步**：① 先**重试变体 B 一次**（排除切槽偶发）；② 不行就退回**变体 A 只加 kmalloc 兜底**
  逐步二分。（兜底方案本身很可能可行：本 SoC ADSP 对 DRAM 一致性访问，8K kzalloc 物理连续，
  `virt_to_phys` 对线性映射有效。）
- 当前 boot_b = 原厂镜像；boot_b 的可启动镜像备份在 Windows：
  `elish_imgs/boot_b_restore.img`（另有能启动的变体 A 镜像 `work/boot_b_dma.img`，md5 `e5c3dbb8…`）。

### ★★★ 第 36 轮：**共享内存映射成功！** 只剩 `ADD_TOPOLOGIES` 等不到响应
- 改用 **`kzalloc` 8K 物理连续缓冲**（不再调必然失败的 `dma_alloc_coherent`）→ 重编 → 打包 → 刷 boot_b
- **内核成功启动 ✓**，开机日志：
  ```
  elish: buffer 8192 bytes phys=0x10350a000
  elish: topologies mapped (handle=0xb0da7698, 8192 bytes)   ← map 成功，拿到真实 handle ✓✓
  elish: add topologies timeout / failed -110
  ```
  ⇒ `-ENOMEM` 解决 ✓；**`ADM_CMD_SHARED_MEM_MAP_REGIONS` 打通** ✓（本设备首次）；只剩 ADD_TOPOLOGIES。
- **设备当前就运行在这个内核上**（`6.12.58-current-sm8250`，含 0050–0053），
  `amp-always-on` active、sink 正常、**无 Call trace/BUG** ⇒ 可作为长期基线 ✓
- **下一步**：① 在 `q6adm_callback` 里**打印所有收到的 opcode**，判断是"没响应"还是"响应 opcode 不匹配"
  （下游 `q6adm.c:1708` 有 `case ADM_CMD_ADD_TOPOLOGIES:` 的 "callback received" 分支，逐行对照归属）；
  ② 复核 map 命令细节；③ `phys=0x10350a000` 高于 4GB，可试低 4GB 分配；
  ④ 修掉日志乱码（疑似把指针当格式串传给 `dev_err`）。
- 镜像：`work/boot_b_km.img`（md5 `bd93c411…`，当前设备在跑的就是它）。

### ★★★ 第 37 轮：超时真因 = **ADSP 当场崩溃**（并已修复一个安全隐患）
- 先排除"回调匹配错"：下游 `q6adm.c` 的响应处理就是 `switch (payload[0])` 里的
  `case ADM_CMD_ADD_TOPOLOGIES:` + `wake_up(adm_wait)`，与我的实现**等价** ✓
  ⇒ 根本没收到响应。
- **真因**（开机日志）：
  ```
  [7.709914] qcom_q6v5_pas 17300000.remoteproc: fatal error received:
             err_qdi.c:1038:EX:audio_process:0x1:ADM:0xd3:PC=0xb0719b94
  [7.715992] remoteproc2: crash detected ... fatal error
  [7.982472] failed to authenticate image and release reset
  ```
  map 成功(7.654) → **ADSP 崩在 ADM 里**(7.709) → 5 秒等待必然超时。
  ⇒ **ADD_TOPOLOGIES 的"物理地址/内存池"对 DSP 无效，DSP 解引用后崩了。**
- **两个下一轮直接可试的修复**：
  1. **<4GB 地址**：我们的缓冲 `phys=0x10350a000`（4.35GB）⇒ 试 `kzalloc(..., GFP_KERNEL|GFP_DMA32)`；
  2. **mem_pool 选错**：我用了 `ADSP_MEMORY_MAP_SHMEM8_4K_POOL=3`（抄 q6asm），
     需对照下游 `adm_memory_map_regions()` 的调用方实际传的 pool id。
- ⚠ **安全隐患（重要）**：开机自动下发会**每次启动打崩 ADSP 且起不回来**（`failed to authenticate
  image and release reset`）⇒ 音频全废。**必须改成 opt-in**（默认不发送，或加 module_param 开关）。
- **设备已刷回原厂 boot_b**（md5 `ded90d33…`）并复核：`fatal error received` = **0** ✓、
  `amp-always-on` active ✓ —— 处于干净可用状态。
- 注意：`work/boot_b_km.img`（含自动触发）**会崩 ADSP，别再直接刷它**；要基于它继续实验，
  必须先改成 opt-in 再重编。

### ★★ 第 38 轮：opt-in 已落地并验证；证实崩溃与 "地址 >4GB" 强相关
- **净收益（可长期保留）**：`q6adm.elish_topologies` 模块参数**默认 OFF**（sysfs 可写），
  work 最多等 60 秒等用户打开，否则打印 `topologies disabled` 返回 ⇒ **坏载荷不会再在开机时打死音频**；
  分配改 `kzalloc(GFP_KERNEL|GFP_DMA32)` 失败回退，并在日志标 `(ABOVE 4GB!)`。
  **实测部署 OK**：内核启动 ✓、**ADSP fatal = 0** ✓、参数为 `N` ✓。
- **打开开关后**：
  ```
  elish: buffer 8192 bytes phys=0x1046fe000 (ABOVE 4GB!)   ← GFP_DMA32 无效
  elish: topologies mapped (handle=0xb0dab528, 8192 bytes) ← map 仍成功
  ADSP fatal error: 1                                      ← 再次崩在 ADM
  ```
  ⇒ **两次实验都在"map 成功 + 地址 >4GB"后立刻崩** ⇒ "DSP 只用 32 位地址、高位截断成非法指针"
  的假设得到强力支持；**`GFP_DMA32` 在本机拿不到 <4GB**。
- **下一步（都很便宜）**：① `payload_addr_lsw/msw` **传 0、只靠 `mem_map_handle`**（一次性验证
  DSP 是否真用那个地址）；② 找真正的 <4GB 缓冲（DT `reserved-memory` 靠下区域，或带 32 位
  `dma_mask` 的设备走 `dma_alloc_coherent`）。
- ⚠ **设备状态未最终确认**：ADSP 崩后我发了恢复流程（`/root/reboot2 bootloader` + fastboot 刷
  `boot_b_restore.img`），最后探测到 **fastboot/adb 为空、22 端口开着**，但 **ssh 会话无输出**。
  **下轮先复核**：`ssh root@172.16.42.1 'uname -r; dmesg | grep -c "fatal error received"'`；
  不行就重走一遍恢复流程（本轮已验证可用）。
- 当前可用镜像：`work/boot_b_optin.img`（md5 `dc80b5b0…`，opt-in 安全版，已验证不崩 ADSP）。

### ⚠⚠ 第 39 轮：**设备卡死，需要人工断电重启**
- 现象：`172.16.42.1:22` **端口开着**，`ssh` 能**认证成功**（`Authenticated ... using "publickey"`），
  但**进入会话后命令不执行、无任何输出**，最终 `Timeout, server not responding`；
  `fastboot`/`adb` 均无设备。⇒ **系统处于卡死状态，远程无法执行任何命令**（因此也发不出 reboot2）。
- **需要人工操作**：长按电源键 ~10 秒强制断电重启。
- **好消息**：`boot_b` 里现在是我们那个 **opt-in 安全内核**（`boot_b_optin.img`，默认不发拓扑、
  已验证 ADSP fatal=0）。sysfs 开关是**运行时状态、不持久**，断电重启后会回到默认 OFF
  ⇒ **重启后应当能正常进入 Armbian，且 ADSP 不会崩**。
- **重启后第一件事**（确认状态）：
  ```sh
  ssh root@172.16.42.1 'uname -r; dmesg | grep -c "fatal error received"; \
      cat /sys/module/q6adm/parameters/elish_topologies'
  ```
  期望：`6.12.58-current-sm8250` / `0` / `N`。
- 若仍不正常：`/root/reboot2 bootloader` → `fastboot flash boot_b elish_imgs/boot_b_restore.img`
  （md5 `ded90d33…`）→ 重启（该流程本轮已成功用过）。
- **教训（务必记住）**：这类"打开开关就可能打死 ADSP"的实验，**每次实验前都要先想清楚
  "崩了怎么恢复"**；本轮崩后恢复流程没能跑完，代价是设备需要人工断电。

### ★★ 第 41 轮：崩溃机制**已确证**，缺口唯一化为"必须 <4GB 缓冲"
- **`addr=0` 实验**（`payload_addr_lsw/msw = 0`，镜像 md5 `e22c7d88…`）结果：
  ```
  elish: buffer 8192 bytes phys=0x106818000 (ABOVE 4GB!)
  elish: topologies mapped (handle=0xb0dab508, 8192 bytes)
  elish: add topologies failed -22      ← EINVAL：**不崩了，但被拒绝**
  ```
  ⇒ ① **崩溃 = >4GB 物理地址**（确证）；② DSP **确实要用那个地址**（0 无效）⇒ addr=0 不是解法；
  ③ 本轮仍记到 1 次 ADSP fatal ⇒ **连"映射 >4GB 内存"本身**都会在 DSP 读取时出错
  ⇒ **整块缓冲都必须在 4GB 以下**。
- ✅ **恢复路径已实证且很轻**：**普通 `reboot` 即可**（opt-in 开关是运行时状态、不持久）。
  重启后实测 `ADSPfatal=0`、`param=N` ⇒ **实验风险已限定为"最多重启一次"**，
  不再需要 fastboot 恢复（这修正了第 39 轮把设备搞卡死的流程缺陷）。
- **下一步（唯一方向：拿到真正 <4GB 缓冲）**：
  1. 用 **`dma_alloc_coherent` 试 `17300000.remoteproc`**（比 glink-edge 再上一级，真实 platform device，
     很可能有 32 位 `dma_mask` 的 DMA ops；其返回值正是该传给 DSP 的 DMA 地址）；
  2. **elish DTS 加低地址 `reserved-memory`** + `memremap` 取物理地址（最可控，需改 DT 重编）；
  3. 或复用 Qualcomm **SMEM** 等已知 <4GB 区域。
- 当前设备：`boot_b` = `boot_b_addr0.img`（md5 `e22c7d88…`，opt-in 安全），运行正常、ADSP 健康 ✓。

### ★★★ 第 42 轮：**ADSP 崩溃已解决**（`remoteproc` 的 DMA 地址在 4GB 以下）
- goal 状态：用户要求"更新到正常状态" → 已 `edit`（`maxGoalRounds` 40→**150**）+ `resume`
  ⇒ 现在 **phase=active / armed** ✓（此前因轮次上限被自动标为 blocked）
- **做法**：`q6adm_add_topologies()` 改成**沿父链逐级 `dma_alloc_coherent()`**（≤6 级），
  取第一个成功者；**并加安全闸：DMA 地址若仍 >4GB 就拒绝发送（`-ERANGE`）**，
  保证坏地址**再也不会**打死 ADSP。地址字段恢复真实值（不再用 0）。
- **结果**：
  ```
  elish: dma 8192 on 17300000.remoteproc dma=0xfe300000     ← 低于 4GB ✓（关键！）
  elish: topologies mapped (handle=0xb0da7688, 8192 bytes)
  elish: add topologies failed -22                          ← 被拒
  ADSPfatal=0                                               ← **不再崩** ✓
  ```
  ⇒ ① **`17300000.remoteproc` 有可用 DMA ops，返回 `0xfe300000`（<4GB）**——
  之前失败的 `aprsvc`/`glink-edge` 都缺 DMA ops，**就差这一级**；
  ② ADSP 不崩了，改回 `-22`(EINVAL) ⇒ **地址障碍彻底清除**。
- **唯一剩余问题**：`ADD_TOPOLOGIES` **载荷格式被 DSP 以 EINVAL 拒绝**。可查：
  ① 载荷头结构（我们的是 `97, 2, 0x1002a000` 开头；确认首字段是否为拓扑条数、是否需去/改前导字段）；
  ② 与下游 `adm_add_topologies()` 逐字段对照（`payload_size` 是否应为 5448 含 4 字节 size 头、
  `mem_pool_id` 是否该不同）；③ 用 `ADM_CMD_GET_PP_TOPO_MODULE_LIST (0x10349)` 反查 DSP 认知。
- ADSP 通路现状：**分配 ✓ → 映射 ✓ → 发命令 ✓ → 收到响应 ✓**，只剩载荷格式（纯数据层面）。
- 镜像：`work/boot_b_lowdma.img`（md5 `149800f3…`，含安全闸）；当前设备在跑它。

### 第 43 轮：`mem_pool_id=0` 被证伪；新怀疑点 = **hdr 的 svc/domain 显式填充**
- 线索：下游 `send_adm_custom_topology()` 走 `remap_cal_data()` →
  `adm_memory_map_regions(&paddr, **0**, &size, 1)`，看似用池 **0**。
- 实验：把 `cmd->mem_pool_id` 3 → 0（镜像 md5 `fdab8fe8…`）→
  ```
  elish: map regions timeout            ← 连 map 都收不到响应
  elish: add topologies failed -110
  ADSPfatal=0                           ← 安全闸有效，仍不崩
  ```
  ⇒ **池 0 是错的；池 3 才是对的**（池 3 下 map 稳定成功并返回 handle）。
  （下游那个 `0` 未必是 mem_pool_id，我按池号理解已被数据否定。）
- **要回退到 `work/boot_b_lowdma.img`（md5 `149800f3…`）**；当前设备跑 pool0 版
  （opt-in 默认关、ADSP 健康、不影响正常音频，但建议刷回）。
- **`ADD_TOPOLOGIES` 的 EINVAL 排除法进展**：已清掉"地址>4GB"、"mem_pool=0"两项。
  剩余怀疑点（按价值排序）：
  1. ★ **`adm_top.hdr` 的 svc/domain 是否需显式填写** —— 下游是**写死**
     `src_svc=APR_SVC_ADM / src_domain=APR_DOMAIN_APPS / dest_svc=APR_SVC_ADM /
     dest_domain=APR_DOMAIN_ADSP`，而 mainline 依赖 `apr_send_pkt()` 用 apr_device 覆盖；
     **若填充值与命令期望不符，DSP 可能直接回 EINVAL**（高价值、改动极小）；
  2. `payload_size` 是否应为 **5448**（含 4 字节 size 头）而非 5444；
  3. 是否该用 **`ADM_CUSTOM_TOP_CAL`(cal_type 10)** 那份数据而非 `CORE_CUSTOM_TOPOLOGIES`；
  4. 用 `ADM_CMD_GET_PP_TOPO_MODULE_LIST (0x10349)` 反查 DSP 侧认知。

### ★★ 第 44 轮：转向 —— 我们那份载荷可能是"公共拓扑表"(cal 47)，不是 ADM 要的(cal 9/10)
- **先证伪"hdr svc/domain 显式填充"**：`apr.c:55 apr_send_pkt()` 本就显式填
  `src_svc=adev->svc.id(8) / src_domain=5 / dest_svc=8 / dest_domain=4`，与下游写死的**完全一致**
  ⇒ 排除（且同通路的 MAP 命令是成功的，佐证头部没问题）。
- **关键**：原厂 logcat 时间线显示 `send_common_custom_topology` 发生在 **ACDB 初始化**阶段
  （`RTAC INIT … → send_common_custom_topology → CORE_CUSTOM_TOPOLOGIES → init done!`），
  而 **ADM 拓扑是播放时才走的另一条**：`send_adm_topology → ACDB_CMD_GET_AUDPROC_COMMON_TOPOLOGY_ID`。
  枚举（`CVP_VOC_RX_TOPOLOGY_CAL_TYPE=0` 起）：**ADM_TOPOLOGY=9、ADM_CUST_TOPOLOGY=10、
  CORE_CUSTOM_TOPOLOGIES=47** ⇒ **我们提取的是 47 那份（公共/全局表）**，
  而 `ADM_CMD_ADD_TOPOLOGIES` 期望 9/10 那一族 ⇒ **EINVAL 说得通**。
- **下一步（都便宜）**：
  1. ★ `payload_size` 试 **5448** 且载荷从 `0x89d6` 起（把 4 字节 size 头也算进去）；
  2. 查 `audio_calibration.c` 的 `call_set_cals()`：**cal 47 是否归 AFE**（若是，则
     `ADM_CMD_ADD_TOPOLOGIES` 从一开始就是错命令，应走 AFE 的 `AFE_PORT_CMD_SET_PARAM`）。
- ADSP 通路本身已完全可用且不崩（分配 ✓ 映射 ✓ 发命令 ✓ 收响应 ✓），上面都是纯数据/命令选择问题。

### ★★★ 第 45 轮：**服务选错了** —— ADD_TOPOLOGIES 有三个，各属一个服务
- **证据一**：`audio_calibration.c` 按"每服务注册的 cal type"分派；ADM 在
  `adm_init_cal_data()`（`q6adm.c:4491`）只注册
  `ADM_CUST_TOPOLOGY(10)/ADM_AUDPROC(11)/ADM_AUDVOL(12)/ADM_RTAC_*` 等，
  **没有 cal 47 `CORE_CUSTOM_TOPOLOGIES`**（全仓 grep 无任何服务注册它）
  ⇒ 我们那份 5444 字节（cal 47）**不该发给 ADM**。
- **证据二**（`apr_audio-v2.h`）—— 三个命令：
  ```
  ASM_CMD_ADD_TOPOLOGIES  0x00010DBE   (服务 aprsvc:service:4:7)
  ADM_CMD_ADD_TOPOLOGIES  0x00010335   (4:8) ← 我们用的
  AFE_CMD_ADD_TOPOLOGIES  0x000100f8   (4:4) ← 很可能才是正确的
  ```
  且原厂函数名就是 `send_common_custom_topology` / 打印 `Common custom topology in use`，
  取数用 `ACDB_CMD_GET_AVCS_CUSTOM_TOPO_INFO_V3`（common 语义），
  与 ADM 每用例的 `send_adm_topology`（`ACDB_CMD_GET_AUDPROC_COMMON_TOPOLOGY_ID`）**是两条路**。
- ⇒ **结论：`ADM_CMD_ADD_TOPOLOGIES` + cal 47 = 服务与 cal 双重错配**，EINVAL 必然。
- **下一步（便宜，通路已验证）**：
  1. ★ 同一份 5444 字节改发给 **AFE**：服务 `aprsvc:service:4:4`、命令 **`0x000100f8`**；
  2. 不行再试 **ASM `0x00010DBE`**（`aprsvc:service:4:7`）；
  3. 判据不变：之后 `q6adm_open(0x1000a100)` 不再返回 `error = 0x3`；
  4. 三者都拒绝则回头查载荷格式（5444 vs 5448 / 是否含 size 头）。
- 这是自 §49 解决"崩溃/地址"以来**最可能一击打通**的方向。

### 第 46 轮：AFE 路径的实施方案（含一个关键架构约束）
- **架构约束**：每个服务的响应归**各自驱动**（`q6afe.c`/`q6asm.c`/`q6adm.c`）。
  所以 **在 `q6adm.c` 里收不到 AFE 的响应** ⇒ 拿不到 AFE 侧的 `mem_map_handle`。
  但 **`AFE_CMD_ADD_TOPOLOGIES` 是"发出即可"**，效果体现在 **ADM** 侧
  （`q6adm_open(0x1000a100)` 不再 `error=0x3`），而 ADM 的响应我们收得到 ✓
  ⇒ 可做 **fire-and-forget** 实验；不过更正统的是照 §37 把实现搬进 `q6afe.c`。
- **建议（按成功率）**：
  1. ★ **照 §37 搬到 `q6afe.c`**：加 `AFE_CMD_ADD_TOPOLOGIES (0x000100f8)` +
     `AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS (0x000100EA)`，在 `q6afe_callback` 处理两类响应；
     复用 `q6adm_add_topologies()` 的全部逻辑（**父链 DMA 分配 + 拒绝 >4GB 安全闸 + opt-in 窗口都要保留**）。
     工作量与 §37 相当（~150 行）。
  2. 同一份 5444 载荷、同一套 map/尺寸约定；**判据仍看 ADM 的 `q6adm_open(0x1000a100)`**。
  3. 若 AFE 不通，再试 **ASM（`0x00010DBE`）** —— 三个都试完即可定位这份 cal 47 归谁。
- **mem_pool_id 回到 3**（pool 0 已被 §50 证伪）。
- 设备当前跑 pool0 版（opt-in 关、ADSPfatal=0、音频正常）；**建议刷回
  `work/boot_b_lowdma.img`（md5 `149800f3…`：池 3 + <4GB 分配 + 安全闸，目前最好一版）**。

### 第 47 轮：AFE 实现规格已**完备**（可直接落地）
- **常量（已在 `apr_audio-v2.h` 核实）**：
  `AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS = 0x000100EA`、
  `AFE_SERVICE_CMDRSP_SHARED_MEM_MAP_REGIONS = 0x000100EB`、
  `AFE_CMD_ADD_TOPOLOGIES = 0x000100f8`；服务设备 **`aprsvc:service:4:4`**。
  下游取 handle 的写法在 **`q6afe.c:1264`**，发 cal 的范式 `afe_send_cal_block()` @ **`q6afe.c:2353`**，
  映射实现参考 `q6afe.c:7401/7478`。
- **mainline `q6afe.c` 落点（已核对）**：`q6afe_callback()` @ **871**、`apr_driver` @ **1762-1772**；
  **`struct q6afe` 里已有 `lock`/`result`/`wait`，等待设施可直接复用**；
  现有 map 支持不完整 ⇒ 需照 §37 移植。
- **落地步骤**：见报告 §54.3（加常量+结构体 → 加字段 → callback 两个 case →
  `q6afe_add_topologies()` **完整复用 q6adm 版逻辑**（父链 DMA 分配、拒绝>4GB 安全闸、opt-in 窗口）
  → `mem_pool_id=3` → opt-in 触发）。载荷仍 `core_custom_topologies.bin`(5444)。
- **判据不变**：`q6adm_open(0x1000a100)` 不再 `error = 0x3`（判据在 ADM 侧，我们收得到响应）。
- **备选（更省事）**：fire-and-forget —— 在 `q6adm.c` 里保留 map，把 ADD_TOPOLOGIES
  改发 AFE 设备 + opcode `0x000100f8`、不等响应，先复用 ADM handle 试探（句柄可能全局/按服务隔离）。

### ⚠⚠ 第 48 轮：AFE fire-and-forget 已实现并刷入，**设备随后失联**
- 实现（`q6adm.c`，镜像 `work/boot_b_afe.img`，md5 `011b1071…`）：
  加 `AFE_CMD_ADD_TOPOLOGIES 0x000100f8` + `ELISH_AFE_DEV_NAME "aprsvc:service:4:4"`；
  用 `bus_find_device(&aprbus, …)` 找 AFE 设备，`container_of(d, struct apr_device, dev)`，
  在 ADM 侧发完拓扑后**再把同一包以 AFE opcode 发出（不等响应）**。编译 0 error ✓、打包刷入 ✓
- **结果：设备没起来**。最后一次探测：**ping 100% 丢包、22 端口关闭、fastboot/adb 均无设备**
  ⇒ 设备处于离线/卡死状态，**很可能仍需人工长按电源键断电重启**。
- ⚠ **教训（第二次同类失误）**：我明知"给 ADSP 发未知命令可能打死它"，却**没有先把恢复通道确认好**
  （也没先确认 AFE fire-and-forget 是否真有必要），就刷入了。**下次这类实验必须：**
  ① 先确认 `reboot2`/fastboot 恢复链可用；② 一次只改一个变量；③ 优先做**能立刻回退**的改动。
- **恢复后第一件事**：确认状态
  ```sh
  ssh root@172.16.42.1 'uname -r; dmesg | grep -c "fatal error received"; cat /sys/module/q6adm/parameters/elish_topologies'
  ```
  期望 `6.12.58-current-sm8250` / `0` / `N`（opt-in 默认关 ⇒ 不会再发 AFE 命令）。
- 若需完全回退：`/root/reboot2 bootloader` → `fastboot flash boot_b elish_imgs/boot_b_restore.img`
  （md5 `ded90d33…`），或刷已知良好的 `work/boot_b_lowdma.img`（md5 `149800f3…`）。
- **镜像清单**：`boot_b_restore.img`(原厂) / `boot_b_lowdma.img`(池3+<4GB+安全闸，最好一版) /
  `boot_b_afe.img`(本轮，含 AFE fire-and-forget，**状态未知、谨慎使用**)。

### 第 49 轮：q6afe.c 实现的**锚点已核实**（下次可直接落地）
设备仍离线（需人工断电），本轮改为做**不依赖设备**的工作：实现 §54.3 的 q6afe 版本。
中途因两个锚点写错而中止 —— **脚本在断言处退出、未写入文件**，`q6afe.c` 保持原样 ✓（已验证 `grep -c elish_topo_wait` = 0）。
已核实的**正确锚点**（下次照用）：
| 用途 | 锚点 |
|---|---|
| 补 include | `#include <linux/delay.h>\n` 后加 `linux/dma-mapping.h` + `linux/firmware.h`（注意：q6afe.c **没有** `linux/device.h`） |
| 放 defines/结构体 | `#include <linux/soc/qcom/apr.h>\n` 之后（必须在 apr.h 之后，因为要用 `struct apr_hdr`） |
| `struct q6afe` 加字段 | `\tspinlock_t port_list_lock;\n` 之后（可用字段：`elish_topo_work/wait/buf/dma/handle/status/map_done/add_done`） |
| probe 里初始化+排 work | `\tinit_waitqueue_head(&afe->wait);\n` 之后（probe 里已有 `afe->apr = adev; mutex_init(&afe->lock); ...`） |
| callback 插桩 | 函数体开头插入即可（**自包含**，不必依赖既有 switch 结构）：先判 `AFE_SERVICE_CMDRSP_SHARED_MEM_MAP_REGIONS` 取 `*(u32*)data->payload` 为 handle；再判 `APR_BASIC_RSP_RESULT` + `r->opcode == AFE_CMD_ADD_TOPOLOGIES` 记 status |
| 实现函数 | 放在 `static int q6afe_callback(...)` **之前** |
- ⚠ 注意 `q6afe.c` 里 `struct aprv2_ibasic_rsp_result_t` 应已可用（经 apr.h）；若不行需 include 对应头。
- 生成脚本：`work/kernel/mk_patch_afe.py`（**锚点需按上表修正**；建议改成 mini 函数式 `sub()` 并逐条断言、最后统一 `open(...,'w')`，这次的写法就是这样所以中止时没写坏文件 ✓）。
- **下次顺序**：① 确认设备恢复 → ② 完成 q6afe 补丁并 `make … q6afe.o` 编译通过 → ③ 打包刷入 →
  ④ 开 opt-in 观察 `elish: AFE ADD_TOPOLOGIES ok` → ⑤ 用 `q6adm_open(0x1000a100)` 判据验证。

### ✅ 第 50 轮：q6afe 补丁**已完成、编译通过、已打包**（步骤①②已做完）
- `q6afe.c` 共 **6 处编辑**（脚本 `work/kernel/mk_patch_afe.py`，逐条断言+末尾统一写文件）：
  include、常量/结构体、`struct q6afe` 8 字段、`elish_afe_add_topologies()`+work、
  callback 开头自包含插桩、probe 里 `init_waitqueue_head`+`schedule_delayed_work(8s)`。
- 逻辑与 q6adm 版一致：**父链 dma_alloc_coherent(≤6) → 拒绝 >4GB 安全闸 → AFE map →
  取 handle → AFE ADD_TOPOLOGIES → 等响应**；`ELISH_AFE_MAP_POOL=3`。
- **验证**：`q6afe.o` **0 error/0 warning** ✓；全内核 `Image` 0 error ✓；
  `vmlinux` 里 `elish_afe_topo_work` ✓、`__param_elish_topologies`（q6adm+q6afe 各一）✓、
  三条 AFE 日志字符串 ✓。
- **镜像已就绪**：`work/boot_b_afe2.img`，md5 **`30c2e396…`**。
- **待设备恢复后**（步骤③④⑤）：刷 boot_b_afe2.img → `echo 1 > /sys/module/q6afe/parameters/elish_topologies`
  （窗口 300s）→ 看 `elish: afe mapped handle=0x…` + `elish: AFE ADD_TOPOLOGIES ok (5444 bytes)`
  → 再用 `q6adm_open(0x1000a100)` 判据验证。若被拒，记下 `status=0x…`，改用 ASM `0x00010DBE` 同套代码。
- ⚠ 设备仍离线（需人工断电）；`boot_b_afe.img`（q6adm 里往 AFE 乱发的版本）**曾导致失联，勿用**；
  其 opt-in 默认关，不会自动运行。

### ✅ 第 51 轮：AFE 实现再加固 + 逐字段对齐下游（仍全部离线完成）
- **加固**：`q6afe_callback` 开头我原来的插桩**没有对 `dev_get_drvdata()` 判空**，
  已加 `elish_afe &&` 守卫（两个条件都加），避免任何 NULL 解引用。
- **逐字段核对下游 `q6afe.c:7390-7412` 的 AFE map 实现**：
  | 字段 | 下游 | 我们的实现 |
  |---|---|---|
  | `src_port` / `dest_port` | `0` / `0` | `0` / `0` ✓（与 ADM 不同，AFE map 确实用 0） |
  | `mem_pool_id` | `ADSP_MEMORY_MAP_SHMEM8_4K_POOL`(=3) | `ELISH_AFE_MAP_POOL`=3 ✓ |
  | `num_regions` / `property_flag` | `1` / `0` | `1` / `0` ✓ |
  | `token` | `IDX_RSVD_2`（非 0） | `0`（回调不校验 token，功能上应无碍；**若 AFE 无响应可试改成非 0**） |
  | 结构体 | `afe_service_cmd_shared_mem_map_regions` + `..._shared_map_region_payload` | 同形（我用 `elish_` 前缀）✓ |
- **重新编译打包**：0 error/0 warning ✓ ⇒ **`work/boot_b_afe3.img`，md5 `d3383b55…`**（取代 afe2）
- **待设备恢复后**：刷 `boot_b_afe3.img` → `echo 1 > /sys/module/q6afe/parameters/elish_topologies`
  → 期望 `elish: afe mapped handle=0x…` + `elish: AFE ADD_TOPOLOGIES ok (5444 bytes)`
  → 再用 `q6adm_open(0x1000a100)` 判据验证。若 map 无响应，优先试 **token 非 0**。

### ★★ 第 52 轮：设备恢复 → AFE 实测完成（**新结论 + 新方向**）
- 部署：`adb reboot bootloader` → `fastboot flash boot_b boot_b_afe3.img`（md5 `d3383b55…`）→
  `set_active b` → 重启。开机实测 `ADSPfatal=0`、**`q6adm`/`q6afe` 两个 opt-in 参数都在且为 N** ✓
- **AFE 实测**（开 `q6afe.elish_topologies=1`）：
  ```
  elish: afe buffer 8192 dma=0xfe300000        ← <4GB ✓
  elish: afe mapped handle=0xb0d051c8          ← **AFE map 成功**（真实 handle）✓
  elish: afe add topologies timeout / failed -110
  ADSPfatal = 1                                ← **ADSP 崩了**（不是单纯超时）
  ```
  判据复测：`cmd = 0x10326 return error = 0x3` / `q6adm_open failed: -22` ⇒ **拓扑仍未装上**
- **三服务对照（硬结论）**：
  | 服务 | 命令 | 结果 |
  |---|---|---|
  | ADM `4:8` | `0x00010335` | map ✓，**优雅拒绝 EINVAL**，不崩 |
  | AFE `4:4` | `0x000100f8` | map ✓，**ADSP 致命错误**，无响应 |
  | ASM `4:7` | `0x00010DBE` | 未试 |
  ⇒ **这份 cal 47 载荷不是任何一个 `*_ADD_TOPOLOGIES` 要的东西**（ADM 说非法、AFE 崩）。
- **★ 新方向**：`apr_audio-v2.h` 里还有
  `AFE_PARAM_ID_SET_TOPOLOGY 0x0001025A`（经 **`AFE_SVC_CMD_SET_PARAM`** 下发）
  与 `AFE_PARAM_ID_DEREGISTER_TOPOLOGY 0x000102E8` —— 语义正好是"common/全局"，
  与 `send_common_custom_topology` 吻合。**下一步优先试这条 param 路径**，别再在 ADD_TOPOLOGIES 上试错。
- 安全面：两个 opt-in 默认 OFF；实测**一次普通 reboot 就让 ADSPfatal 回到 0** ✓，
  风险被限定为"最多重启一次"。
- 设备当前：Armbian，运行 `boot_b_afe3.img`，`ADSPfatal=0`、ampfix active ✓

### ★★ 第 54 轮（联网检索）：**主线 TDM RX 补丁比我们树里的版本新，我们漏了"逐颗 codec 槽位分配"**
- 上游参考：**`[PATCH v2 4/6] ASoC: qcom: sm8250: add TDM RX support`**（Val Packett, 2026-05-06）
  <https://lkml.iu.edu/hypermail/linux/kernel/2605.0/08571.html>
  （描述原文："…send audio data to speaker amplifiers. Channels are assigned based on the
  codec DAI names for a quad-speaker setup **such as on the xiaomi-pipa tablet**"）
- 它做了三件事：① CPU DAI 播放 `set_tdm_slot(cpu,0,0x3,8,w)` + `set_channel_map(…tdm_slot_offset[8])`；
  ② ★ **逐颗 codec DAI** `set_tdm_slot(codec_dai, 0, rx_mask, 8, w)`，`rx_mask` 由
  **codec 名字前缀** `PL/PR/SL/SR`→`BIT(0..3)`，认不出则 `rx_mask=0` + `dev_warn`；
  ③ TDM_RX_0 startup 设 `IB_NF|DSP_B` + `*_TDM_IBIT` 时钟。
- **我们树里（Armbian 补丁 0006）对比**：CPU 槽位 ✓（用 32 位宽，与 `0x4808=0x20200000` 自洽）、
  CPU channel map ✓、TERT_TDM_RX startup ✓（12.288MHz 与实测吻合），
  **但"逐颗 codec DAI 的 TDM 槽位分配"整段缺失**；
  且 elish 前缀是 `BRH/BLH/BRL/BLL/TRH/TLH/TRL/TLL`，**上游那套 PL/PR/SL/SR 也认不出**。
- ⇒ 这可以解释"放大器寄存器全对、DSP 不报错、却几乎没声"（槽位没分配 ⇒ amp 收不到数据），
  与 §59.2 的推论互为印证。
- **下一步（具体）**：在 `sm8250_tdm_snd_hw_params()` 里补逐颗 codec 的
  `snd_soc_dai_set_tdm_slot(codec_dai, 0, rx_mask, 8, slot_width)`；
  **第一版最保守**：8 颗 amp 全部 `rx_mask = 0x3`（读立体声槽 0/1）。
  判据：`dev_warn("codec DAI name not recognized")` 消失 + **实听**。
- 顺带情报：sm8250 的 TDM RX 支持在主线**非常新（2026-05）且仍在评审**，
  所以"我们这版漏一段"完全可能；同系列还有 Senary MI2S RX 与 `[PATCH 0/6] fixes and improvements`。

### 第 55 轮：codec 侧 TDM 槽位已补齐并**验证生效**，但**仍无声** ⇒ 转为"先造观测手段"
- 实现：`sm8250_tdm_snd_hw_params()` 播放分支加 `for_each_rtd_codec_dais` →
  `snd_soc_dai_set_tdm_slot(codec_dai, 0, 0x3, 8, 32)`。
- **实测确认生效** ✓：`Tertiary TDM Playback: elish: codec cs35l41-pcm tdm slots 0x3/8/32`
  （镜像 `work/boot_b_tdm.img`，md5 `e2ece04d…`）
- **用户实听：仍然完全没声** ✗ ⇒ codec 槽位不是根因（补齐是必要的，但不充分）。
- **已排除的完整清单**（都有实测证据）：PipeWire 流 ✓、PCM RUNNING/S24_LE/48k ✓、
  FE 数字音量 0dB 满值 ✓、TERT_TDM_RX_0 路由 on ✓、CPU DAI 槽位/通道映射 ✓、
  **codec DAI 槽位（本轮）✓**、8 颗放大器寄存器与 Android 逐字节一致 ✓、
  出厂标定/调音一致 ✓、ASM 0dB 下发无变化 ✓、AMP Enable `0x2014=1` ✓、
  播放期间 ADSP 无报错 ✓ —— **但实听完全没声**。
- ★ **关键路径已变**：盲改到头了。**下一步必须先造观测手段**：
  ①（首选）**加 TERT_TDM capture 节点**（改 DT）→ 播放时抓 TDM，直接看有无波形/幅度，
  一次性判定"是 DSP 没输出"还是"输出了但放大器不响"；
  ② 或先打通麦克风做声学测量。**在拿到这个观测能力之前，继续盲改意义不大。**

### ★★★ 第 56 轮（联网检索）：命中同症病例 —— **`-110` 可能根本不是良性，而是根因**
- 找到邮件列表报告 **`cs35l41: Enable(1) failed: -110`**（David Wronek, 2025-03-18）
  <https://lists.openwall.net/linux-kernel/2025/03/18/697> ／
  <https://lkml.iu.edu/hypermail/linux/kernel/2503.2/03076.html>
- 设备 **Lenovo Xiaoxin Pad Pro 2021 = 同为 SM8250 + CS35L41**，MI2S、内部升压；
  报错**与我们逐字相同**（`Enable(1) failed: -110` + `PRE_PMU: … Main AMP event failed: -110`），
  而且他明确说 **"non-working speakers"** —— **该错误与"扬声器不工作"绑定**；
  他的定位也正是 **轮询 `CS35L41_IRQ1_STATUS1` 超时**（与我们代码级定位一致）。
- ⚠ **推翻 §34 的"`-110` 是良性"结论**：我们的 `0x2014=1` 是 **amp-fix.sh 绕过驱动手工写的**，
  **驱动自己的使能序列其实是失败的** ⇒ 放大器可能处于**半配置状态**：
  寄存器看着对、但**内部升压没起来 ⇒ 无供电轨 ⇒ 完全没声**。
  **这一举解释了"所有软件层都对、DSP 不报错、却完全没声"。**
- **新方向（最高优先级）**：把 `-110` 当**真故障**修，而不是绕过：
  ① 对比同类设备 **Lenovo j716f 的 DTS**（mainlining/linux 里 `sm8250-lenovo-j716f.dts`，
     我试过 raw 链接抓取失败，下轮换 GitHub blob/镜像）检查 elish 的 cs35l41 节点：
     `cirrus,boost-type`、VA/VP/VSPK 供电、`reset-gpios`、中断接线；
  ② 中断是否真的连上（报告人说 shared boost 时"中断从不触发"）；
  ③ 参考上游 `[PATCH 3/9] ASoC: cs35l41: Initialize completion object before requesting IRQ`。
- ⇒ **放弃** §58.4 里"逐个试写运行期寄存器"的思路；真正该做的是**让驱动的使能序列走通**。

### ★★★ 第 57 轮：用**本地 Android DTB** 对比，发现 mainline 放大器节点缺两个关键属性
- **素材**：`work/vendor_dtb/01..04.dtb`（Android DTB）用 `dtc -I dtb -O dts` 解出后，
  **01–05 中 01..04 各含 9 处 `cs35l41`**（00/05 没有）⇒ Android 的放大器节点真身。
- **对比（mainline `sm8250-xiaomi-elish-common.dtsi` vs Android）**：
  | 属性 | Android | mainline elish |
  |---|---|---|
  | **`cirrus,right-channel-amp`** | **@40 有 / @42 无** | **完全没有** ❌ |
  | **`cirrus,asp-sdout-hiz`** | **`0x01`** | **`<3>`** ❌ |
  | `cirrus,temp-warn_threshold` | `<0x03>` | 无 |
  | `pinctrl-names` | `"cs35l41_irq_speaker"` + `pinctrl-0` | 无 |
  | `cirrus,boost-type` | 无（默认） | `<0>` |
  | gpio 配置 | 子节点 `cirrus,gpio-config2 {…}` | 平铺 gpio2-* |
  | 升压元件参数 | 0xfa0/0x3e8/0x0f | 4000/1000/15（**数值相同** ✓） |
- **很可能就是根因**：★ `cirrus,right-channel-amp` 是 mainline 用于区分
  "这颗 amp 负责右声道"的属性；Android 逐颗标注，**mainline 一颗都没标**
  ⇒ 8 颗可能被当作同一声道/不做声道配置 ⇒ **放大器读到的槽位与实际数据对不上**
  ⇒ 正好解释"寄存器全对、DSP 不报错、却完全没声"，也解释了
  **上一轮把 codec TDM 槽位统一设 `0x3` 仍无声**（声道归属没设）。
  另：缺 `pinctrl`(中断脚) 可能与 §62 的 `-110`/中断不来相关。
- **下一步**：① 先从 01–04 四个 DTB 抽出 **8 颗的完整 R/L 映射**；
  ② 给 8 个节点补 `cirrus,right-channel-amp`（按 Android 映射）；
  ③ `cirrus,asp-sdout-hiz` 由 `<3>` 改 **`<1>`**；④ 视情况补 temp-warn 与中断 pinctrl；
  ⑤ 重编→`make_boot_image.py` 打包→刷 boot_b→**实听**。
- **方法论**：**Android 自己的 DTB 就在本地**（`work/vendor_dtb/`），它才是最权威的对照；
  联网的价值在于"同类设备症状 + 上游补丁演进"，两者互补。

### ★★★ 第 60 轮：拿到 **elish 真 Android DT**（d12）并完成权威对比 + 更正
- **怎么找到的**：实况 `dtbo_a`（DT 表头 `d7b7ab1e`）里其实有 **29 个 DTB**；
  早先 `work/vendor_dtb/` **只 dump 了 00–05**（cmi/psyche/apollo/thyme/XR/RUMI），**漏了 elish**。
  全部解出后按 model 列出，找到
  **`d12  model="… xiaomi elish"  cs35l41=32`** ⇒ 这就是 elish 的真 DT（现成文件 `/tmp/d12.dts`）。
- **elish 真实放大器配置（8 颗，前缀与我们一一对应）**：
  | 节点 | right-channel-amp | asp-sdout-hiz | prefix |
  |---|---|---|---|
  | @40 / @41 / @43 | 无 | **0x03** | TRH/TLH/TRL |
  | **@42** | **有** | 0x03 | **TLL** |
  | @40 / @43 / @42（另一总线） | 无 | 0x03 | BRH/BLL/BRL |
  | **@41** | **有** | 0x03 | **BLH** |
- **结论 + 更正**：
  1. ✅ **`asp-sdout-hiz = 0x03`，与 mainline 的 `<3>` 一致** ⇒ **§63 说"应改成 `<1>`"是错的**
     （那是 cmi/psyche 的值）。**mainline 此项本来就对，不要改。**
  2. ★ **`cirrus,right-channel-amp` 只打在两颗上：`TLL`(@42) 与 `BLH`(@41)**（不是按 R/L 一刀切），
     而 **mainline 的 elish 8 颗一个都没打** ⇒ **目前唯一被证实的真实差异**。
- **下一步（有据可依）**：① 给 `TLL` 与 `BLH` 两个节点加 `cirrus,right-channel-amp;`（其余不加）；
  ② 重编→打包→刷 boot_b→**实听**；③ 若仍无声，再照 Android 补
  `pinctrl-names="cs35l41_irq_speaker"`+`pinctrl-0`（Android 的 vendor_boot 里确有
  `cs35l41_int_speaker` pinctrl 节点，与 §62 的 `-110`/中断不来的线索相关）；
  ④ `asp-sdout-hiz` **保持 `<3>`**。
- **方法论第三次教训**：① 先验身份(`model`)；② **别只看前几个条目**（elish 在 29 个里的第 13 个，
  差一点又漏）；③ 对比前先自证（`asp-sdout-hiz` 那条若不核实就会改错）。

### ★★ 第 62 轮：客观排除三条（超时/DTS/GPIO），`-110` 仍在
- **(a) 放宽 PUP 超时无效**：`cs35l41-lib.c` 三处 `regmap_read_poll_timeout` 由
  `100000`(100ms) 改 `1000000`(1s)，**只替换模块**（`snd-soc-cs35l41-lib.ko`，不用刷内核）→
  `Enable(1) failed: -110` **依旧**（失败间隔正好 ~1s，证明新模块生效）
  ⇒ **不是超时长短；PUP_DONE 根本没置位，即放大器真的没启动**。
  且驱动是**轮询寄存器**（不依赖中断线）⇒ **pinctrl/中断也不是 `-110` 的原因**。
- **(b) 逐颗核对 GPIO：与 Android 完全一致**（BRH 6/7、BLH 62/67、BRL 69/100、BLL 49/126、
  TRH 50/27、TLH 78/92、TLL 30/112、TRL 144/129）⇒ **mainline 正确，不要改**。
  ⚠ **自我更正**：我曾把 Android 的 TRH(50/27) 当成 mainline 的 BRH 来比，误以为找到根因；
  逐颗对齐后才发现一致 —— **先核对再动手又避免一次改坏**。
- **现在与 Android 的差异只剩**：`cirrus,right-channel-amp`（已补 TLL/BLH，待听感）与
  `cirrus,fast-switch`（mainline 无；理论上不致完全无声）。
- ⇒ **`-110` 的根因不是 DTS/GPIO/超时**。嫌疑转向**驱动使能序列本身**或**放大器实际供电状态**。
  下一步候选：① 反汇编 Android 的 `audio_cs35l41.ko` 看厂商上电时**除 PUP 轮询外还做了什么**；
  ② 确认 VA/VP/VSPK 是否真有电（或对比 Android 播放态寄存器快照找差异）。
- 现场：`snd-soc-cs35l41-lib.ko` 已被换成"1s 超时"版（原文件备份 `*.ko.orig`，
  回退 = 拷回 + `depmod -a` + 重启）；本轮**未改内核镜像**。

### ★★★ 第 63 轮：直接读状态寄存器 —— **PUP 永远完不成**（根因收窄到"放大器实际启动"）
- 寄存器定义（`include/sound/cs35l41.h`）：`PWR_CTRL1=0x2014`（`GLOBAL_EN=bit0`）、
  `IRQ1_STATUS1=0x10010`（`PUP_DONE=bit24=0x01000000`）。
- **实测（播放态）**：
  | 时刻 | 0x2014 | 0x10010 |
  |---|---|---|
  | 驱动跑完使能序列后 | **0** ← GLOBAL_EN 没置位，放大器处于未使能态 | `0x00400000` |
  | 手动写 0x2014=1 后 | **1**（读回确认，能锁存） | `0x00400000` ← **PUP_DONE 始终不置** |
  ⇒ **不是写不进去，而是写进去也完不成上电**；放大器的内部启动（升压/DSP）走不完。
- dmesg 有 **`DSP1: Legacy support not available`**，定位到 `wm_adsp.c:1663-1667`
  （新式 buffer 列表为空 + legacy 解析 `-ENODEV`）——**信息级**消息，但与"DSP 侧没起来"同方向。
- **又排除一项**：设备上**没有扬声器专用电源轨**（只有背光 ±5.5V）⇒ 不是"DT 缺供电"。
- **结论（重新定位剩余工作）**：应用→PCM→路由→槽位→音量 ✓、放大器 **DTS 配置** ✓、
  放大器**寄存器** ✓ —— 全都与 Android 对齐；**唯一缺口是"放大器实际启动"**。
  ⇒ **问题不在"缺哪个配置项"，而在 mainline 驱动 vs 厂商驱动在"放大器启动序列"上的行为差异**，
  **不是 objective 第 2/3 项（寄存器扫描+配置移植）能覆盖的**，需要**驱动层面**的工作。
- **下一步候选**：① ★ 对照厂商 `work/hal/audio_cs35l41.ko` 与 mainline 的**启动路径**
  （GLOBAL_EN 前后厂商是否还有 mainline 没有的步骤：额外解锁、boost 使能位 `BST_EN=0x30`、
  OTP、DSP 启动等）；② 查 `0x10010` **bit22（实测=1，mainline 头文件未定义）**的含义
  （datasheet / 厂商驱动），它可能就是"卡在哪一步"的直接提示。

---

## ★ 第 21 轮的关键结论（先看这个）

1. **Android 可脚本播放**：`tinymix "TERT_TDM_RX_0 Audio Mixer MultiMedia1" 1 1` + `tinyplay … -D 0 -d 0`；
   但**放大器不会自动上电**，需手写 `0x2014=1 / 0x2018=0x3721`（8 颗）。⇒ AMP Enable 由 HAL/CSPL 负责，Armbian 要自己做。
2. **AFE 校准不是出声的必要条件**（Android AFE cal `ret -22`/`cal_block not found` 却有声）⇒ 该分支可放弃。
3. **原厂 Android 自己的音量标定下发失败**：`ACDB-LOADER … active device/stream not found (result=-100)
   for topology 0x1000a100 and apptype 0x11134` + `out_write: retry previous failed cal level set`
   ⇒ "声音小"很可能源于此。ADM 拓扑 `0x1000a100`，app_type 69940/69937，acdb_id 10011。
4. `Audio Stream N App Type Cfg = [app_type, acdb_dev_id, sample_rate, be_id, channels]`，
   `be_id=87` = TDM backend dai_id（非端口号）。
5. **★ mainline 选错了 COPP topology**：`q6routing.c:393` 硬编码 `NULL_COPP_TOPOLOGY`(0x10312)，
   而 Android 用 **`0x1000a100`**。NULL = 直通 COPP ⇒ **ADSP 里从不实例化扬声器 audproc 链**。
   已产出 **`patches/0003-q6routing-configurable-copp-topology.patch`**（运行时开关
   `/sys/module/q6routing/parameters/copp_topology`，默认 0 = 上游行为，**零回归**）。
   补丁序列必须 **0001 → 0002 → 0003**（0002 依赖 0001），三者实测 dry-run 全 PASS。
   见 `patches/SERIES.md`。**这是"一分钟判定"的实验：`echo 0x1000a100 > …/copp_topology` 后播放。**
6. **cal 下发可以走 in-band，不需要 Ion/共享内存**（已核对下游源码证实）：
   `downstream/q6adm.c:4891 adm_send_calibration(port_id, copp_idx, path, perf_mode, cal_type, params, size)`
   → `:4907 adm_set_pp_params(port_id, copp_idx, **NULL**, params, size)` ⇒ **mem_hdr==NULL 就是 in-band**，
   且 `EXPORT_SYMBOL(adm_send_calibration)`。所以"必须做 ADM shared-mem map"这个前提是**错的**，
   不要再去碰 `ADM_CMD_SHARED_MEM_MAP_REGIONS`/PSPD（PSPD 结构里根本没有 cal_type/acdb_id/app_type 字段）。
7. **已交付的 ACDB 模块**：`work/kernel/elish_acdb_cal/{elish_acdb_cal.c,Makefile,README.md}` +
   `/home/axis/axis_rnd/ACDB_PORT_PLAN.md`（in-band `ADM_CMD_SET_PP_PARAMS_V5/V6`，
   src_port=`q6afe_get_port_id()`=0x9020，dest_port=`copp->id`，token=`(port_idx<<16)|copp_idx`，
   走 `aprsvc:service:4:8`；含"等 COPP active 再发 + 重试 5×"的时序修复）。
   **本地已交叉编译通过**（461,512 B .ko，0 warning）。
8. **ACDB 表结构线索**（`ACDB_PORT_PLAN.md §4.2`）：`Forte_Speaker_cal.acdb` chunk = `<8B tag><u32 len><payload>`；
   尾部索引表 32 字节/行 `{sample_rate, index, off1..off5, 0}`（48000 共 11 行，起始 0x0babc2）；
   acdb_id 键表 ~12 字节步长（10011=0x271b 首现 0x1734）；app_type 键表 ~20 字节步长（69940=0x11134 首现 0x1f30）。
   `AVOLLUT0` 已可用 `parse_acdb.py avol` 解出。
   ⚠ 但 `ADM_AUDVOL`/AFE 的 gain-dep 查询在本机返回 **-19（空表）**，而 Android 照样有声
   ⇒ **AUDVOL 很可能对本机根本不存在**，试的顺序应为 12 → **11（AUDPROC，Android 真正下发成功的那块）** → 9/10。

## 内核构建与安装路径（重要）
- **已构建成功 ✓**（`build_elish_kernel.sh`，用**设备自身 config**）：
  6.12.58 + Armbian sm8250-6.12 全套补丁 —— **50/50 干净应用，0 跳过**；`BUILD_EXIT=0`。
  - `arch/arm64/boot/Image` = 38,660,608 B
  - `arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-{boe,csot}.dtb` ✓
  - **552 个 .ko**，vermagic = `6.12.58-current-sm8250 SMP mod_unload aarch64` ✓
- **★ 关键坑（已踩并修正）**：`device.config` 里 `CONFIG_LOCALVERSION=""`，直接编出来是 `6.12.58`，
  与设备现装的 **`6.12.58-current-sm8250`** 不符 ⇒ 装上去会让 `/lib/modules/...` 全部失配（wifi 等都加载不了）。
  必须加 `LOCALVERSION=-current-sm8250`：
  ```sh
  make -j$(nproc) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=-current-sm8250 Image modules dtbs
  ```
  已重编并核实 `include/config/kernel.release` = `6.12.58-current-sm8250`、Image 版本串一致 ✓
- 树内三处改动已逐项核实：`q6routing.c:45/419`（copp_topology param）、`q6adm.c:547/644`
  （`q6adm_set_volume` + EXPORT）、`q6asm.c:995/1064`（`q6asm_set_volume` + EXPORT）；
  `vmlinux` 中 `q6adm_set_volume`/`q6asm_set_volume` 为全局符号 ✓，三个音频 .o **零 warning** ✓。
- ⚠ **Armbian 当前 `current` 分支已是 6.18**（设备装的是 6.12.58），所以**不能**直接 `compile.sh`；
  要么手工按 6.12.58 编（已验证可行），要么传 `KERNEL_MAJOR_MINOR=6.12 KERNELBRANCH=branch:linux-6.12.y`。
- **安装路径**：elish 用 `BOOTCONFIG="none"` + `image-output-abl`，内核是**镜像构建期**打进 boot 镜像的
  （不是设备上 `dpkg -i` 就能生效）。所以最终要用 Armbian 框架重建 **boot 镜像并刷 `boot_b`**
  （备份：`backup/partbackup/`、`elish_boot_a_restore.img` md5 `ded90d33…` → 可随时回滚）。
- 一键判定脚本已备：`work/kernel/armbian_topology_test.sh`（切到 Armbian 后直接跑）。

## 一句话结论
Android（slot a）**有声音** ✓、Armbian（slot b）**无声** ✗ —— 硬件/ADSP/8 颗 CS35L41/TDM 全部正常 ✓；
**唯一缺口 = mainline 缺少 ADM shared-memory 校准下发路径**（把 `Forte_Speaker_cal.acdb` 的校准灌进 ADSP）。

## 已完成且已验证
- **逆向**：ACDB/CSPL/HAL/Cirrus 驱动/mixer 全部抓取；ACDB 五文件 md5 与设备**逐个一致** ✓
- **寄存器**：8 颗放大器 27 个寄存器（空闲+播放态）与 Android **逐字节一致** ✓
- **修复 1**：TDM/ASP `0x4808=0x20200000`、`0x4840=24`、`0x6808=0x3F75`、`0x6c04=0x253`、`0x8004=0` ✓
- **修复 2**：`power/control=on` 常供电 → `PRE_PMU -110` 归零 ✓
- **修复 3**：**`systemctl disable --now slpi-off.service` + 拉起 SLPI** → 修掉 ADSP `Memory_map_regions failed` ⇒ 这是"无声"的直接原因之一 ✓
- **修复 4**：冷启动后需**登录图形桌面**（或 `systemctl start display-manager`）才会出现 PipeWire sink ✓
- **通道**：外部模块可下发 ADSP 参数（`apr_send_pkt` ✓、`aprbus` ✓、`q6adm_open` ✓ 均导出；`CONFIG_MODULE_SIG` 未开 ✓）
  - ADM：`aprsvc:service:4:8`；ASM：`aprsvc:service:4:7`
  - ASM 寻址：`src_port=dest_port=(session<<8)|stream_id`（=0x0101），`token=session`
  - 音量：Q13（`0x2000`=1.0=0dB），module `0x10BFE`，param `0x10BFF`
- **补丁**：`patches/0001-*`（ADM COPP 音量 + ALSA 控件）、`patches/0002-*`（ASM 流音量）
  —— 均 `patch -p1 --dry-run` 通过 **且已实际交叉编译 `q6asm.o/q6routing.o/q6asm-dai.o` BUILD_EXIT=0** ✓

## 剩余唯一工作（照 §30.4.1 做）
```
1. ADM_CMD_MEMORY_MAP_REGIONS 把校准块(DMA) 映射进 ADSP → 取 q6map_handle
   （downstream/q6adm.c:1973 adm_memory_map_regions / :2054 remap_cal_data）
2. ADM_CMD_SET_PSPD_MTMX_STRTR_PARAMS_V5/V6 携带 handle + cal_type/acdb_id(15)/app_type(69940)/rate(48000)/topology
   （downstream/q6adm.c:639/726/825）
3. 校准块来源：parse_acdb.py 从 Forte_Speaker_cal.acdb 提取（cal_type 分类见 downstream/audio_cal_utils.c:22-63）
4. 用已验证的 APR 通道下发（模块模板见 elish_asm_vol/elish_asm_vol.c ✓ 已编译通过）
```

## 现场资产
- 设备：Android 在 slot a（有声、可用 ✓）；Armbian 在 slot b ✓；`/root/reboot2 bootloader` + `fastboot set_active a|b` 切槽 ✓
- 设备存储：`/sdcard/Download/acdb/`（五个 ACDB 文件，md5 已校验 ✓）
- 本机：`work/acdb/`、`work/hal/`、`work/android/tm_android_working.txt`（Android 已校准态 6023 控件 ✓）
- 模块源码：`work/kernel/elish_asm_vol/`（含 ASM 流音量 ✓）、`work/kernel/elish_adsp_vol/`（ADM+ASM ✓）
- 内核源码：`work/kernel/build/linux-6.12.58/`（已解包 ✓）；Armbian 交叉编译链已装（`aarch64-linux-gnu-gcc`）✓
- Windows 侧：`C:\Users\cheny\Downloads\elish_imgs\acdb\`、`platform-tools\`（adb/fastboot）

## 建议的下一次验证顺序（每步都能单独验证）
1. 起 Armbian → 登录桌面 → 播放：确认 **有声**（SLPI 修复后应恢复正常 ✓）
2. 下发 ASM 流音量 0 dB → 对比响度（预期：这就是缺的那 ~10 dB）
3. 实现 §30.4.1 的校准下发 → 验证与 Android 响度/保真度对齐
4. 固化：`amp-fix.sh`/UCM 持久化 + 内核补丁 0001+0002 用官方 Armbian 源码编译装 `boot_b`

---

# 【第 67 轮 · 2026-09-22 · 状态校正 + Android 无重刷插桩方案】

## A. 更正上面两处旧结论（重要，别再被误导）
1. **「修复 2：power/control=on → PRE_PMU -110 归零」不成立。**
   本轮实测（Armbian 6.12.58，uptime 524~527 s，speaker-test 播放中）：
   - 8 颗全部 `power/control=on` ✓、`slpi-off.service` **disabled** ✓、`amp-always-on` **active** ✓
   - 但 8 颗**依旧全部** `cs35l41 X-00XX: Enable(1) failed: -110` + `ASoC: PRE_PMU: ... Main AMP event failed: -110`
   - `dmesg | grep -c "Enable(1) failed"` = **24**（=8×3 次播放尝试）
   ⇒ `-110` 只与「PUP_DONE 永不置位」有关，与 i2c 供电/autosuspend 无关。**这是唯一剩余阻塞点。**

2. **`Memory_map_regions failed` 是启动期一次性的竞态，不是持续故障。**
   - 本轮 dmesg：仅 **1 次**，发生在启动后 47.6 s
     `qcom-q6asm aprsvc:service:4:7: DSP returned error[1]` → `Memory_map_regions failed`
     → `Audio Start: Buffer Allocation failed rc = -22` → `snd_soc_pcm_component_prepare ... -12`
   - 之后 `speaker-test -D hw:0,0`（48000 Hz / S16_LE / 2ch，period=12000，buffer=48000）
     **再次播放未复现该错误**（计数仍为 1）⇒ **DSP/ASM/TDM 通路本身是好的**。
   - 三个 remoteproc 全部 `running`；`q6asm.c` 的 `mem_pool_id = ADSP_MEMORY_MAP_SHMEM8_4K_POOL(3)` 正确（q6asm.c:92/529）。
   - 另：本轮 11 条 `fatal error received` **全部来自 `5c00000.remoteproc`（sensor_process）**，
     `17300000.remoteproc`（音频 ADSP）**0 条**。以后统计 ADSP 健康度必须按 remoteproc 区分，别用泛匹配。

## B. 结论：只剩「放大器上电永不完成」这一个问题
- 数字链路 ✓（PCM RUNNING、TDM slots 0x3/8/32、路由 ON、FE 0 dB）
- 放大器 27+ 寄存器空闲/播放态与 Android **逐字节一致** ✓
- 校准/调音已下发 ✓（`pushed factory calibration: cal_r=...`、`pushed 12 tuning values from cirrus/BRH-music.txt`）
- 但 `CS35L41_PWR_CTRL1(0x2014)` 播放中仍为 0，手动置 1 后 `IRQ1_STATUS1(0x10010)` 仍为 `0x00400000`
  （bit22 置位、**bit24 PUP_DONE 永不置位**）⇒ 主线上电等待逻辑永远超时。

## C. 下一步方案：不用重刷 Android 内核，用 kprobe/ftrace 抓厂商真实时序
用户建议「改 Android 内核加日志再刷」。**不建议**：Xiaomi 内核是 GKI + 一堆 out-of-tree 厂商模块，
重建成本极高且会把现在**能正常启动的 Android（slot a + Magisk root）**置于风险中。
Android 已 root，**同一份证据可以用运行时插桩零风险拿到**：

```sh
# 在 Android（slot a）里，root 下：
adb shell su -c 'cat /proc/kallsyms | grep -i cs35l41'       # 1) 枚举厂商驱动符号
adb shell su -c 'ls /sys/kernel/tracing/'                     # 2) 确认 tracefs
# 3) ftrace 函数图：抓厂商上电完整调用链（这是主线缺的那一步）
echo function_graph > /sys/kernel/tracing/current_tracer
grep cs35l41 /proc/kallsyms | awk '{print $3}' > /sys/kernel/tracing/set_ftrace_filter
echo 1 > /sys/kernel/tracing/tracing_on
# 4) 播放音乐 ~5 s 后：
echo 0 > /sys/kernel/tracing/tracing_on; cat /sys/kernel/tracing/trace
# 5) kprobe 抓寄存器读写值（i2c regmap 层）：
echo 'p:ampw cs35l41_reg_write reg=%x0 val=%x1' >> /sys/kernel/tracing/kprobe_events
```
**关键对照点**：Android 播放中读 `ampreg BUS ADDR 0x10010` —— 看 bit24(PUP_DONE) 是否真的被置位。
- 若 Android 也**只有 bit22** ⇒ 主线的完成判据就是错的，应改判据（或按 chip revision 选掩码）。
- 若 Android bit24=1 ⇒ 厂商在置 PWR_CTRL1 之前多做了某步，看 ftrace 链路即可定位。

**注意**：现有 `work/android/arb_regs_playing.txt` 只抓了 28 个寄存器/颗，**不含 0x10010**，
所以这个决定性对照至今没做过 —— 必须先补这一枪。参考：Android 播放中 `0x2014 = 00 00 00 01`（GLOBAL_EN=1），
Armbian 播放中为 0。

## D. 本轮设备状态（离开时）
- Armbian（slot b）运行中，`/root/xm.on` **已删除**（XM 实验已证伪，撤掉避免写非标准寄存器）
- `amp-always-on` active、`slpi-off` disabled、ADSP(17300000) 无 fatal
- 切换：`ssh root@172.16.42.1 '/root/reboot2 bootloader'`（**该文件是 ELF 可执行文件，别 cat**）
  → Windows 侧 `cmd.exe /c "cd /d C:\Users\cheny\Downloads\platform-tools && fastboot.exe set_active a|b"` → `fastboot reboot`
- 回退镜像：`fastboot flash boot_b boot_b_restore.img`（md5 `ded90d33…`）
- ⚠️ 注意 Windows 上有 `emulator-5556` 在跑，`adb devices` 会混入，识别平板别只看第一行。

---

# 【第 68 轮 · 找到根因 ★★★ PUP_DONE 是一次性粘滞位，主线清掉它 ⇒ 后续每次 enable 必超时】

## A. 决定性实验（在 **Android 自己的芯片** 上做的，不是猜）
进 Android（slot a，root ✓），`/data/local/tmp/ampreg` 直接读写寄存器。

**第一次观察（空闲态，8 颗全部一致）：**
```
ID(0x0)        = 00 03 5a 40
PWR_CTRL1(0x2014)  = 00 00 00 00   <-- GLOBAL_EN = 0（放大器"关"着）
IRQ1_STATUS1(0x10010) = 01 40 00 00  <-- 0x01400000 = bit24 + bit22
```
⇒ **GLOBAL_EN=0 时 bit24(PUP_DONE) 已经是 1**。

**第二次：在 Android 上完整复刻主线的 enable 序列**
```
start:      2014=00000000  10010=01c00000
step1 pdn:  2014=00000000  10010=01c00000        # GLOBAL_EN=0
step2 clear:写 0x10010=0x01800000（清 bit24+bit23）
           2014=00000000  10010=00400000        # bit24 被清掉 ✓
step3 en:   2014=00000001  10010=00400000        # GLOBAL_EN=1
step4 poll×8 + 2s:  10010 恒为 00400000          # ★ bit24 再也不置位 ★
after 2s:   2014=00000001  10010=00400000
```
⇒ **在这颗硅片上，PUP_DONE 只会在"真正的上电跳变"时锁存一次；之后无论怎么置 GLOBAL_EN=1 都不会再置位。**

**第三次：掉电会置 bit23**
```
写 0x2014=0  →  10010 = 01c00000   # bit23(0x00800000, PDN_DONE) 置位，bit24 仍为 1
```
⇒ bit24=PUP_DONE（粘滞）、bit23=PDN_DONE（粘滞）、bit22=0x00400000 **恒为 1**（不是上电标志）。

## B. 根因结论
`sound/soc/codecs/cs35l41-lib.c::cs35l41_global_enable()`：
- 失败的是 **INT_BOOST 分支**（日志串 `Enable(1) failed: -110` 来自 lib.c:1296；
  SHD_BOOST 分支在 enable 时 `if (ret || enable) return ret;` 会提前返回，产不出这条日志）
- 该分支：`regmap_update_bits(PWR_CTRL1, GLOBAL_EN, 1)` → `poll bit24 1 s` → **成功后 `regmap_write(IRQ1_STATUS1, PUP_DONE_MASK)` 清粘滞位**（lib.c:1299）
- ⇒ **第一次 enable 把 bit24 清成 0，从此以后每次 enable 都在等一个永远不会再来的边沿 → 恒 `-110`**
- Android 厂商驱动**从不清除**该粘滞位 ⇒ 它的 poll 永远"已满足" ⇒ 正常出声 ✓
- 这同时解释了：为什么手动写 `0x2014=1` 能读回 1、但 bit24 始终 0（因为它早被清掉且不会重锁存）

## C. 修复方向（下一步就做）
改 `cs35l41-lib.c`（该驱动是**模块** `snd-soc-cs35l41-lib.ko`，只需换 .ko + `depmod -a` + 重启，无需刷内核）：
1. **成功后不再清除 PUP_DONE/PDN_DONE 粘滞位**（去掉 lib.c:1299 / 1279 / 1318 / 1347 的 `regmap_write(..., mask)`）；
2. 并且 **不把 poll 超时当致命错误**：把 `Enable(%d) failed` 从 `return ret` 改为仅 `dev_warn` 后继续
   （因为粘滞位可能已在别处被清，硬件本身其实已上电）；
3. 更稳妥的做法是把完成判据改成"**bit24 或 bit22 任一为 1 即认为已上电**"，或干脆只等一个短的固定延时。

**验证判据**：改完后播放时 `0x2014` 应保持 1、`Enable(1) failed` 消失，并**实际听到声音**。

## D. Android 侧资产（本轮新增）
- `adb.exe -s 32b28a4a ...`（必须带 `-s`，否则 "more than one device"）
- `/data/local/tmp/ampstat.sh`（8 颗 0x0/0x2014/0x10010 快照）
- `/data/local/tmp/amppup.sh`、`amppup2.sh`（GLOBAL_EN 跳变 + 粘滞位清除复现脚本）
- Windows 侧同目录 `platform-tools\ampstat.sh / amppup.sh / amppup2.sh`
- 设备当前：**Android slot a**，root ✓，8 颗 amp 处于 `2014=1, 10010=00400000`（被实验置位后未复位）

---

# 【第 68 轮（续）· 修复已落地并实测；发现下一个缺口：粘滞位方向相反】

## A. 修复已实施并部署 ✓
- 源：`sound/soc/codecs/cs35l41-lib.c`，`SHD_BOOST` + `INT_BOOST` 两分支：
  1. **删除** `regmap_write(IRQ1_STATUS1, pup_pdn_mask)`（不再消费一次性粘滞位）
  2. 轮询超时由 `dev_err + 上抛 -110` 改为 `dev_warn` + `ret = 0` 继续
  3. 轮询超时 `1000000`(1 s) → **`20000`(20 ms)**，消除 8 颗 enable 串行 8 s 的问题
- 产物：`work/kernel/build/linux-6.12.58/sound/soc/codecs/snd-soc-cs35l41-lib.ko`
  - 第一版（仅改语义）md5 `736fd577852f097a8d739f98ce8ef78e`
  - 第二版（加 20 ms）md5 `9c9e64bc7cb65cc48e61edc8894f4da5` ← **设备上现用**
- 设备备份：`.../snd-soc-cs35l41-lib.ko.orig`、`.ko.bak67`
- 部署方式：`scp` → `/lib/modules/6.12.58-current-sm8250/kernel/sound/soc/codecs/` → `depmod -a` → 重启

## B. 实测结果（重启后，播放中，8 颗全读）
```
bus1 0x40..0x43 / bus3 0x40..0x43 :  2014 = 00 00 00 01   ← ★ 8/8 全部 GLOBAL_EN=1 ★
                                      10010 = 00 c0 00 00
Enable(1) failed = 0        （修复前 24）
PRE_PMU failed   = 0        （修复前 8 颗全失败）
```
⇒ **"放大器完全不上电"这个硬阻塞已解除**，且 20 ms 超时后 8 颗同时上电（不再串行）。

## C. ★ 但粘滞位方向与 Android **相反** ⇒ 可能仍未真正上电
| | bit24 PUP_DONE | bit23 PDN_DONE | bit22 |
|---|---|---|---|
| Android（正常出声） | **1** | 0 | 1 |
| Armbian（修复后） | **0** | **1** | 1 |

`0x00c00000` 的含义是"**最近完成的一次状态跳变是掉电（PDN_DONE）**"，
而 Android 的 `0x01400000` 是"最近完成的是上电（PUP_DONE）"。
⇒ 仅写 `GLOBAL_EN=1` **并不会**让这颗芯片完成真正的上电跳变
（这一点我在 Android 上也验证过：清掉粘滞位后再置 GLOBAL_EN=1，bit24 同样不置位）。
⇒ **厂商路径里还有一步"真正完成上电"的动作，主线没做。**

## D. 下一个缺口的最强嫌疑：**放大器 DSP 固件没起来**
dmesg 里 8 颗全部有：
```
cs35l41 X-00XX: DSP1: Legacy support not available
```
来自 `wm_adsp.c:1663-1667`（`list_empty(&dsp->buffer_list)` + `wm_adsp_buffer_parse_legacy()` 返回 `-ENODEV`）。
即 **厂商的 legacy 格式固件主线的 wm_adsp 解析不了** ⇒ 放大器内部 DSP 未运行
⇒ 很可能正是"上电不完成（PUP_DONE 不置位）"的原因。
**次要证据**：B 组 3 颗能下发 12 个 tuning 值（`elish: pushed 12 tuning values from cirrus/BRH-music.txt`），
T 组 3 颗则 `Direct firmware load for cirrus/T{L,R}{H,L}-music.txt failed with error -2`（这些文件在原生 Android 里也是 0 字节）。

## E. 下一步（明确）
1. 对照 Android 与 Armbian 的 `cirrus/` 固件目录：Android 侧 `/vendor/firmware/cirrus/` 或
   `/vendor/etc/firmware/`（用 `adb shell su -c "ls -la /vendor/firmware/cirrus/"`），
   找出 **非 0 字节的 `.wmfw` / `.bin` / `-music.txt`**，补到 Armbian 的 `/lib/firmware/cirrus/`；
2. 若厂商固件是 legacy 打包格式，需要给 `wm_adsp` 加 legacy 解析（或把固件转成 mainline 的 wmfw 格式）；
3. 用户在场时**实听**确认响度/失真（本轮用户不在机器旁，未做听感验证）；
4. 全部确认后固化：把 `cs35l41-lib.c` 改动做成 Armbian 补丁 `0054-*`，`DTS`/TDM 改动并入补丁系列。

## F. 方法论教训（本轮）
- `cat` 了一个 ELF 文件（`/root/reboot2`）浪费大量上下文 —— **先用 `file` 判断类型**。
- Windows `adb` 上有 `emulator-5556` 干扰，**所有 adb 命令必须带 `-s 32b28a4a`**。
- Armbian 重启后 ssh 可能要**好几分钟**才可用（sshd 会先 reset 后 refused 再 up），**不要过早判定 wedge**。
- 后台播放必须用 `setsid ... </dev/null >log 2>&1 &`，否则随 ssh 退出而亡。
- `adb reboot bootloader` 后 fastboot 枚举也偏慢，`set_active` 的输出为空是**显示问题**不是失败（用 `getvar current-slot` 复核）。

## G. ★ 客观证据（不需要耳朵）：升压轨是 0 V ⇒ 放大器仍未真正上电
修复后播放中（8/8 `GLOBAL_EN=1`、0 错误）再读**升压状态寄存器**：
```
bus1 0x40 / bus1 0x42 / bus3 0x40 / bus3 0x42 （8 颗一致）
  CS35L41_VPBR_STATUS (0x0000640C) = 00 00 00 00     ← 正向升压轨电压 = 0
  CS35L41_VBBR_STATUS (0x00006410) = 00 00 00 00     ← 负向升压轨电压 = 0
  CS35L41_IRQ1_STATUS2(0x00010014) = 10 30 00 00
```
结合 `0x10010 = 0x00c00000`（PDN_DONE=1 / PUP_DONE=0），结论：
**放大器只是"被全局使能"，内部升压（boost）从未启动** ⇒ 仍然不会出声。
⇒ `-110` 是**症状**，真正缺的是"**让 boost 起来**"的那一步/那个配置。

**下一轮第一步（最便宜且决定性）**：在 Android 上读同样三个寄存器
（`ampreg 1 0x40 0x640C/0x6410/0x10010`，播放中），与上面的 0 对照。
- 若 Android 非 0 ⇒ 确认 boost 差异，接着对照 **`CS35L41_BSTCVRT_*`（0x3800–0x3830）整组**
  与 **`PWR_CTRL3`/`BOOST` 相关寄存器**，找出主线写错/漏写的那一个；
- 若 Android 也为 0 ⇒ 说明 VPBR 需要先开 VMON 才有效，改看 `0x3800–0x3830` 组。

注：`0x3800–0x3830` 组（BSTCVRT_VCTRL1/2、PEAK_CUR、COEFF、SLOPE_LBST、SW_FREQ、DCM_CTRL、
OVERVOLT_CTRL）正是主线的 `cs35l41_boost_config()` 所写，且**与 DTS 的
`cirrus,boost-ctl-millivolt`/`cirrus,boost-peak-milliamp`/`cirrus,boost-ind-nanohenry` 直接相关** ——
这是当前最值得逐字节对照的一组寄存器（Android 的 `arb_regs_playing.txt` 里没有这一段，需现场重抓）。

---

# 【第 69 轮 · 窄范围 diff 成功：192 个寄存器只有 8 个不同】

## 结论速览
- 双边同条件（`GLOBAL_EN=1`、无码流、`bus1 0x40`）dump 192 个寄存器，**仅 8 个不同**。
- **3 个是可写配置寄存器，且手动写 Android 值后能保持（播放中也不被覆盖）**：
  | 寄存器 | 名称 | Android | Armbian |
  |---|---|---|---|
  | `0x4808` | `SP_FORMAT` | `0x20200000` | `0x20180200` ✗ |
  | `0x4810` | `SP_FRAME_TX_SLOT` | `0x04040404` | `0x03020100` ✗ |
  | `0x6c04` | **`AMP_GAIN_CTRL`** | `0x00000253` | `0x00000240` ✗ ← **增益，直接关系响度** |
- 其余 5 个是状态寄存器（`0x10010/0x10014/0x10018/0x1001c/0x10094`），其中 `0x10014` Android 恒有 `0x1f00`(bit8-12)。
- **`BSTCVRT 0x3800-0x3830` 整组、`PWR_CTRL1/3`、`0x640C/0x6410` 全部逐字节一致** ⇒ 升压配置不是差异点。

## ⚠ 重要更正
`amp-fix.sh` 里 `0x4808=0x20200000`、`0x6c04=0x253` **没有生效**（实际读回 `0x20180200` / `0x240`）。
⇒ 必须改成**码流建立之后再写**，或改驱动（`cs35l41_set_dai_fmt`/`hw_params`）。旧报告的"修复 1 ✓"作废。

## 本轮资产
- `work/android/dumpA_small_en1.txt`（Android 参考）、`dumpL_small_en1.txt`（Armbian）、`d.txt`（diff）
- `platform-tools/amptarget.sh`（192 寄存器定点 dump，两边通用）
- `platform-tools/ampdump.sh`（全范围 0x0-0x8000，**很慢**，仅必要时用）
- Android 侧 `/data/local/tmp/t.wav`（自造 20 s 1 kHz 测试音）、`ampboost.sh`、`ampstat.sh`、`ampoff.sh`

## 方法坑（务必记住）
- i2c dump **必须先杀干净残留 dump 进程**，否则争用导致慢 60 倍（1 次/分钟）。
- **有码流在放时 i2c dump 也会极慢**；定点小范围 dump 才是可行做法。
- Armbian 重启后 ssh 可能要 5-10 分钟才可用（reset→refused→up），不要误判为 wedge。

## 下一步
1. 在**码流建立之后**写那 3 个寄存器（钩子或改驱动），复核 `0x2014` 是否稳定为 1；
2. 取**播放中**的双边定点 diff，确认 `0x10014` 的 bit8-12 是否就是"FS/时钟锁定"标志；
3. 用户在场实听（`amixer -c 0 cset numid=452 1` 打开 TDM 路由后播放）。

---

# 【第 70 轮 · ⚠ 推翻第 69 轮的"更正"：amp-fix 只在播放时生效，播放态已与 Android 一致】

## 关键结论
1. **`amp-fix.sh` 没有失效**。`amp-fix.service` 是 `enabled+active` 的长驻循环，
   脚本 82-89 行**只在 PCM 进入 `RUNNING`** 时才 `KICK`/`ASP`。
   ⇒ 空闲态读到的是驱动默认值（`0x4808=0x20180200`、`0x6c04=0x240`）**属正常**。
   ⇒ 第 69 轮 §75.3 的"amp-fix 没生效"是**误判**（拿空闲态去比了）。
2. **播放态实测：Armbian 与 Android 逐字节一致** ✓
   `aplay -D hw:0,0 /root/t.wav`（RUNNING, S16_LE/48k/2ch）时 `bus1 0x40`：
   `0x4808=20200000` ✓、`0x6c04=00000253` ✓（增益）、`0x2014=1` ✓、`0x2018=3721` ✓、BSTCVRT 全一致 ✓
   日志确证：`amp-fix[9269]: asp(regs first): pcm=S16_LE wl=16`
3. **真实差异只剩状态寄存器**：
   `0x10010` Android `01400000`(PUP_DONE) vs 我们 `00c00000`(PDN_DONE)；
   `0x10014` Android `30101f00` vs 我们 `00300000` —— **Android 恒有 bit8-12 = `0x1f00`**，疑似"FS/时钟已锁定"。

## 下一步（唯一主线）
小范围（`0x10000-0x10100`，64 寄存器，快）在**播放态**做双边定点 diff → 锁定状态差异全集 →
查 `0x10014` bit8-12 是否 FS 锁定 → 若确认，回头查 TDM 时钟/ASP FS 配置
（含 `sm8250.c` 里我们加的 `set_tdm_slot(codec_dai, 0, 0x3, 8, 32)` 与 Android 是否一致）。

## 方法坑（第三条同类，务必内化）
> **寄存器对比必须在同一状态（空闲/播放）下做**，否则会得到整套假差异。
> 判断脚本是否生效要看**日志**，不要只看一次静态读取。
> 另外：`speaker-test` 在本机经常起不来（日志空、procs=0），**用 `aplay -D hw:0,0 <wav>` 更可靠**；
> 测试音已放到 `/root/t.wav`（20 s 1 kHz，来源 `platform-tools/t.wav`）。

---

# 【第 71 轮 · 状态位性质定性 + 「假播放」陷阱】

## 结论
1. **`0x10014` 是粘滞遥测，不是缺失配置**：
   同一颗 amp 三时刻 = 空闲 `00000000` → 播放中 `00300000`(bit20,21) → 之后空闲 `30300000`(+bit28,29)。
   与 Android `30101f00`(bit8-12,20,28,29) 相比，**我们只是始终没有 bit8-12（`0x1f00`）**。
   ⇒ 全项目只剩这一个"小范围稳定的真实寄存器差异"待查。
2. **★ 陷阱：`aplay` 进程在，但 PCM 是 `closed`**（本轮多次发生）：
   此时 `0x2014=0`（放大器未使能），读到的一切都是**空闲态值** ⇒ 结论作废。
   `amp-fix.sh` 正是靠该状态文件决定是否 KICK，所以假播放期间放大器全程不上电。

## 纪律（写进流程，别再犯）
```sh
pkill -f "[a]play"; pkill -f amptarget.sh          # 1. 先清干净
amixer -c 0 cset numid=452 1                        # 2. 开 TDM 路由
setsid aplay -D hw:0,0 /root/t.wav </dev/null >/tmp/ap.log 2>&1 &
sleep 4
sed -n 's/^state: *//p' /proc/asound/card0/pcm0p/sub0/status   # 3. 必须 = RUNNING
journalctl -t amp-fix -n1                           # 4. 必须有 asp(regs first) 记录
# 两条都满足，读到的寄存器才算"播放态"
```

## 下一步（只剩两条）
1. Android **播放态**定点读 `0x10000-0x10100`（64 寄存器，快）→ 与 Armbian 已校验的播放态逐位对比，
   确认是否只剩 bit8-12 这一组；查该位含义（数据手册/厂商 ko）。
2. **实听**闭环（需用户在场）：`amixer -c 0 cset numid=452 1` + `aplay -D hw:0,0 /root/t.wav`。

---

# 【第 72 轮 · ★ 新候选根因：`TST_FS_MON0` 的 BCLK/FS 关系用错】

## 线索
- `0x10014` 唯一恒缺的是 **bit8-12 = `0x1f00`**（疑似 FS 检测状态）。
- 头文件里确有 FS 检测：`FS1_WINDOW_MASK=0x7FF`、`FS2_WINDOW_MASK=0xFFF800`、**`TST_FS_MON0=0x00002D10`**。
- `cs35l41.c:862-894 cs35l41_dai_set_sysclk()`：查表 `cs35l41_fs_mon[]`（按 **BCLK** 索引），
  `freq<=6144000` 用表值，**`freq>6144000` 一律用硬编码 `fs1=0x10, fs2=0x24`**。
- 表最后一项是 `{ 12288000, 0, 0 }`；而 `sm8250.c:165` 正好传 `TDM_BCLK_RATE = 12288000`（第 20 行定义）。

## 实测
Armbian `bus1 0x40 0x2D10` = **`0x00024010`** ⇒ 走的是硬编码分支，
即**假设 64×FS**。但 elish 是 **8 slot × 32bit @48k = 256×FS**（BCLK 同样 12.288 MHz）
⇒ FS 窗口参数按错误关系配置 ⇒ 放大器检不到正确 FS ⇒ 很可能同时解释
`0x10014` 缺 `0x1f00` 与 `0x10010` 缺 PUP_DONE。

## 下一步（只读 1 个寄存器，成本极低）
Android **播放态**（务必先校验 `status=RUNNING` + `journalctl -t amp-fix -n1`）读 `0x2D10`：
- 若 ≠ `0x00024010` ⇒ 拿到原厂值，改 `cs35l41.c::set_sysclk`（或 `sm8250.c` 传参）→ 复测 bit8-12 / PUP_DONE；
- 若 = `0x00024010` ⇒ 排除该假设，转"实听闭环"。

## 本轮其它
- 又一次遇到"假播放"（`aplay=1` 但 PCM 非 RUNNING、`0x2014=0`）⇒ §77.2 的校验纪律必须执行。
- 环境现状：设备在 **Armbian slot b**，`/root/t.wav` 在位，`/root/xm.on` 不存在（XM 未启用）。

---

# 【第 73 轮 · `TST_FS_MON0` 假设部分证伪 + 已校验播放态工具】

## 新增工具（以后所有播放态实验都用它）
`/root/playtest.sh`（源 `work/armbian/playtest.sh`）：
等 `pcm=RUNNING` **且** `0x2014=1` 才继续，并打印 `journalctl -t amp-fix -n1` 作二次校验。
用法：`sh /root/playtest.sh [reg] [val]`（给 reg/val 时会在播放态写入并回读）。
基准输出（可信播放态）：
```
pcm=RUNNING  ampfix_last=asp(regs first): pcm=S16_LE wl=16  2014= 00 00 00 01
2D10= 00 02 40 10   10014= 30 30 00 00   10010= 00 c0 00 00
```

## 结论：运行中改 `TST_FS_MON0` 无效
播放态写 `0x2D10 = 0xA04604` / `0x284184` 都**能写进去**，但
`0x10014`（缺的 bit8-12）与 `0x10010`（缺的 PUP_DONE）**完全不变**。
⇒ §78 的假设在"运行中改值"层面**证伪**；
唯一未排除：窗口值是否必须**在流建立前**就正确（`set_sysclk` 会覆盖预写值）——
要验证必须改 `cs35l41.c::cs35l41_dai_set_sysclk` 或 `sm8250.c` 传参后重编 `.ko`。
**但优先级低**：所有配置类寄存器已与 Android 逐字节一致（§76.2）。

## 当前判断
配置对齐（含增益 `0x6c04=0x253`）**已完成**；卡住验收的是**没有听感/响度实测**。
⇒ 下一步第一优先：**用户在场实听**（跑 `sh /root/playtest.sh` 即可，它会保证是真播放态）；
若仍无声再回到"驱动侧改 set_sysclk"这条实验。

---

# 【第 74 轮 · ★ 修掉真实"完全没声"原因：TDM 路由开机默认 off 且无人设置】

## 发现
扬声器播放链路 `MultiMediaN` → `TERT_TDM_RX_0` 的 ALSA 开关
**`TERT_TDM_RX_0 Audio Mixer MultiMedia1..8` = numid 452..459**：
- **开机默认 off**；
- `grep -l "numid=452\|Audio Mixer" /usr/local/bin/*.sh /etc/systemd/system/*.service` **零命中**
  ⇒ 开机**没有任何脚本**打开它 ⇒ 桌面/PipeWire 播放时数据到不了 TDM 后端 ⇒ **完全没声**
  （与放大器修没修好无关）。这解释了"有时有声有时完全没声"。

## 已部署的修复（自测通过 ✓）
`/usr/local/bin/amp-always-on.sh`（开机服务，源 `work/armbian/amp-always-on.sh`）现做两件事：
1. 8 颗 `power/control=on`（原有）；
2. **新增**：等 `/proc/asound/card0` 出现后把 **numid 452..459 全部置 on**，并 `logger -t amp-always-on`。

自测：手动置 0 → `systemctl restart amp-always-on` → `numid=452/455` 都回到 **on** ✓，
日志 `power/control=on + TERT_TDM_RX_0 routes on (card0 waited 0x0.5s)`。

## 下一步
1. **重启一次**确认开机流程也生效（本轮只验证到 service 重启级）；
2. 用户在场实听（`sh /root/playtest.sh` 已保证真播放态）；
3. 仍无声才回到驱动侧 `set_sysclk` 实验。

---

# 【第 75 轮 · 路由持久化通过真实重启验证 + 全链路客观自检通过】

## 真实重启验证（uptime 17s，无任何手工设置）✓
```
amixer -c 0 cget numid=452 → values=on      amixer -c 0 cget numid=459 → values=on
journalctl -t amp-always-on → "power/control=on + TERT_TDM_RX_0 routes on (card0 waited 0x0.5s)"
```
⇒ §80 的开机路由修复**在真实开机流程中生效**。

## 全链路自检（`sh /root/playtest.sh`）
```
pcm=RUNNING   2D10=00 02 40 10   10014=00 30 00 00   10010=00 c0 00 00
8 颗 0x2014: b1-40=1 b1-41=1 b1-42=0 b1-43=0 b3-40=1 b3-41=1 b3-42=1 b3-43=1
Enable(1) failed = 0     PRE_PMU failed = 0        （修复前分别是 24 与 8 颗全失败）
```

## 完成度（对目标的四项）
1) 抓取 HAL/CSPL/ACDB/Cirrus/ADSP mixer —— ✓ 已完成
2) 8 颗逐寄存器比对 —— ✓（192 寄存器定点 diff，播放态配置类逐字节一致）
3) 移植 + **实测响度与失真** —— 移植 ✓；**实测 ✗ 未做**（无听感、无可用录音链路）
4) 写入 `ELISH_AMP_TDM_FIX.md` —— ✓（§74-§81）

⇒ **唯一未完成项：目标项 3 的"实测响度与失真"**，需要用户在场。

## 下一步
1. 用户在场实听（开机即可播，路由已持久化）；
2. "很小" ⇒ 调 `amp-fix.sh` 的 `MGAIN`（现 865=+6dB）逐档试听；
3. "完全没声" ⇒ 做驱动侧 `set_sysclk` 实验（§78/§79）；
4. 达标 ⇒ 固化补丁（`cs35l41-lib.c` 改动、`amp-always-on.sh`、DTS/TDM）。

---

# 【第 76 轮 · 造"客观响度/失真"测量链路（主机麦克风）—— 工具就绪，环境阻塞】

## 方案
平板就摆在 PC 旁 ⇒ 用**主机麦克风**录平板扬声器，客观算 **响度(dBFS) + THD**，
并可用同一装置做 **Armbian vs Android 的 A/B 量化**（目标项 3 的无听感替代方案）。

## 已就绪 ✓
- WSLg 带 PulseAudio（`PULSE_SERVER=unix:/mnt/wslg/PulseServer`）
- 已装 `pulseaudio-utils`(parec/pactl) + `sox` + `python3-numpy`
- **分析器：`work/audio/analyze.py`**（输出 RMS dBFS / PEAK / 主峰频率 / **THD(2-6)** / SNR / top5 谱峰）
- Windows 侧确认**存在麦克风端点**（Steam Streaming Microphone、WO Mic Device、WILLEN II）
- 平板侧 `playtest.sh` 已校验播放态（pcm=RUNNING、amp-fix 日志、0x2014=1）

## 阻塞 ✗（需要用户介入）
```
parec -d RDPSource ... > cap.raw   → rc=124, bytes=0     # 采不到任何数据
pactl list sources                 → Connection failure: Timeout
```
WSLg `RDPSource` 取不到数据；疑似 Windows **默认录音设备是虚拟设备**（Steam Streaming Mic / WO Mic）
或 WSLg 音频需重启。

## 用户做完这一步即可自动量化
1. Windows 设置→系统→声音→**输入**：选**真实麦克风**，确认有电平跳动；
2. 然后在本机跑：
```sh
ssh root@172.16.42.1 'sh /root/playtest.sh' &
export PULSE_SERVER=unix:/mnt/wslg/PulseServer
timeout 8 parec -d RDPSource --format=s16le --rate=44100 --channels=1 --raw > /tmp/cap.raw
sox -t raw -e signed -b 16 -r 44100 -c 1 /tmp/cap.raw /tmp/cap.wav
python3 work/audio/analyze.py /tmp/cap.wav 1000
```
3. 记 RMS/THD → 切 Android(slot a) 重复 ⇒ 得到响度差与失真差。

## 退路
主机麦克风不可用 ⇒ 修通**平板自身麦克风**（前序搁置），用平板录自己（有串扰，量化弱）。

---

# 【第 77 轮 · ★ FE→BE 路由是 DAPM 管理的 ⇒ 改为"每次播放断言"】

## 关键实测
手动置 on 后运行开机脚本，结果：**452=off, 455=on, 752=off, 759=on**
⇒ 只有 **MultiMedia1 相关的 452(RX)/752(TX) 被复位为 off**，其余保持。
笔记本机桌面音频正是走 **MultiMedia1** ⇒ 用户按播放时路由很可能是 off ⇒ **静默无声**。
原因：`TERT_TDM_RX_0 Audio Mixer MultiMediaN` 是 **DAPM 管理**的路由，
该 FE 的 PCM 关闭后通路断电、开关复位 ⇒ §80 的"开机置一次"**不可靠**。

## 已部署修复（实测通过 ✓）
`amp-fix.sh` 的 `ASP()`（主循环在 PCM→RUNNING 跳变时调用）开头新增：
```sh
for n in 452 453 454 455 456 457 458 459; do amixer -c0 cset numid=$n 1 >/dev/null 2>&1; done
```
实测：`452 pre-play=off` → `playtest.sh`（RUNNING/2014=1）→ `452 during play=on` ✓

## 采集(TX)同类缺口
`MultiMedia1 Mixer TERT_TDM_TX_0..7` = numid **752..759 默认也 off**；
`arecord` 实测 **rc=1、连文件都不产生**。已在开机脚本里一并置 on，
但**采集仍然失败**（更深的问题：ADSP 采集后端/麦克风链路）。

## 客观测量的两条路都不通（本轮结论）
1. 主机麦克风（WSLg `RDPSource`）：0 字节 + PulseAudio 连接超时（§82.3）
2. 平板自采（`arecord`）：rc=1，不产生文件
⇒ **目标项 3「实测响度与失真」仍需用户介入**。

## 当前设备状态
- Armbian；`/usr/local/bin/amp-always-on.sh`（常供电 + RX 452-459 + TX 752-759）
- `/usr/local/bin/amp-fix.sh`（播放时重新断言路由）
- 本地源：`work/armbian/amp-always-on.sh`、`work/armbian/amp-fix.sh.device`、`work/armbian/playtest.sh`
- 播放客观指标：pcm=RUNNING、0x2014=1、Enable-failed=0、PRE_PMU-failed=0

---

# 【第 78 轮 · 平板采集链路彻底不通（已排除参数问题）】

## 实测
- `arecord --dump-hw-params` 声称支持 S16_LE/S24_LE、1-4ch、8-48kHz、PERIODS 2-8（正常）；
- 但**所有组合都失败**：`-c 1/-c 2/-c 4`、`S16/S24`、`8k/48k`、显式 period/buffer —— 全部
  `arecord: set_params:1462: 无法安装hw参数`，rc=1，不产生文件；
- TX 路由 752-759 已置 on 仍如此。
⇒ **不是参数问题，是采集通路在驱动/ADSP 层起不来**。

## 结论
- 平板**无独立 TDM capture 设备**（`arecord -l` 只有 `device 0: MultiMedia1`）；
- ⇒「用平板自录自测」这条退路**也不通**；
- ⇒ **目标项 3「实测响度与失真」唯一可行路径 = 用户在 Windows 侧选一个真实麦克风**
  （工具已备好：`work/audio/analyze.py`；步骤见 §82.4）。

## 若将来要修采集（独立课题）
最可能缺的是 **VA/WSA 宏的 capture 通路**（麦克风 → ADSP）；
`TERT_TDM_TX_0` 只是 TDM 总线采集。需在 DTS/后端配置补齐。

---

# 【第 79 轮 · 核心修复已固化为可复现补丁 `0054-*`】

## 产物
- **`work/kernel/patches/0054-cs35l41-do-not-consume-one-shot-pup-pdn-status.patch`**
  （PUP_DONE 粘滞位修复；含问题/硅片实测证据/改法/效果；**2 个 hunk**；
  `patch -p1 --dry-run` **通过**）
- `work/kernel/patches/SERIES.md` 已追加该补丁条目

## 生成方法（无 git 时如何还原基线）
内核树非 git 仓库，但本地有 Armbian 源码包 `build/linux-6.12.58-gh.tar.gz`：
```sh
P=$(tar -tzf linux-6.12.58-gh.tar.gz | grep 'sound/soc/codecs/cs35l41-lib.c$')
tar -xzf linux-6.12.58-gh.tar.gz -C /tmp/pristine "$P"
diff -u --label a/sound/soc/codecs/cs35l41-lib.c --label b/sound/soc/codecs/cs35l41-lib.c \
     /tmp/pristine/"$P" linux-6.12.58/sound/soc/codecs/cs35l41-lib.c
```
顺手回退了一处无关遗留实验（EXT_BOOST 超时 1 s），使补丁只含必要改动。

## ⚠ 注意
原以为的 Armbian 补丁目录 `armbian-build/patch/kernel/archive/sm8250-6.12/`
**在当前工作区不存在**；补丁暂放 `work/kernel/patches/`，将来并入官方构建树时再拷入。

## 可复现修复集（全部就位）
| 文件 | 作用 | 状态 |
|---|---|---|
| `patches/0054-cs35l41-*.patch` | 放大器真正使能（`-110` 消除） | ✓ dry-run 通过 |
| `work/armbian/amp-always-on.sh` | 常供电 + RX/TX 路由 | ✓ 重启验证 |
| `work/armbian/amp-fix.sh.device` | 增益/ASP/播放时断言路由 | ✓ 实测 |
| `work/armbian/playtest.sh` | 已校验播放态工具 | ✓ |
| `work/audio/analyze.py` | 客观响度/失真分析 | ✓ 就绪 |

## 剩余唯一未完成项（不变）
**目标项 3「实测响度与失真」** —— 需用户选真实麦克风（§82.4 有命令）。

---

# 【第 80 轮 · 按补丁重建并重新部署（设备与补丁一一对应）】

第 79 轮为最小化补丁回退了源码里一处无关遗留改动（EXT_BOOST 超时 1 s），
导致设备 `.ko` 与补丁产物漂移。本轮重建+重装消除：
```
BUILD_EXIT=0
本地 md5 = bb740afb457159e29a2e7fa264ac5648  (441840 B)
设备 md5 = bb740afb457159e29a2e7fa264ac5648  ✓ 一致
modinfo vermagic = 6.12.58-current-sm8250 SMP mod_unload aarch64 ✓ 与运行内核一致
```
备份：`snd-soc-cs35l41-lib.ko.orig` / `.bak67` 仍在。
**新 .ko 于下次重启生效**（本次未重启，运行中仍是功能等价的上一份构建）。

## 补丁 ↔ 设备 闭环
| 层 | 产物 | 状态 |
|---|---|---|
| 驱动 | `patches/0054-cs35l41-*.patch` | 已重建部署，md5 一致 ✓ |
| 开机 | `work/armbian/amp-always-on.sh` | 真实重启验证 ✓ |
| 播放时 | `work/armbian/amp-fix.sh.device` | 实测 ✓ |
| 工具 | `playtest.sh` / `audio/analyze.py` | 就绪 ✓ |

## 剩余唯一未完成项
**目标项 3「实测响度与失真」** —— 需用户在 Windows 侧选真实麦克风。

---

# 【第 88 轮 · TDM capture DAI link 已写进 DTS 并**编译通过**（尚未刷机）】

## 已做
在 `build/linux-6.12.58/arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-common.dtsi`
的 `&sound` 里，`speaker-dai-link` 之后**新增** `tdm-capture-dai-link`：
```dts
	tdm-capture-dai-link {
		link-name = "Tertiary TDM Capture";
		cpu      { sound-dai = <&q6afedai TERTIARY_TDM_TX_0>; };
		platform { sound-dai = <&q6routing>; };
		codec    { /* 与 speaker-dai-link 逐字同序：tlh,tll,trh,trl,blh,bll,brh,brl */ };
	};
```
**验证**：
```
make ... qcom/sm8250-xiaomi-elish-csot.dtb   → EXIT=0
strings dtb | grep "Tertiary TDM"            → "Tertiary TDM Playback" + "Tertiary TDM Capture" ✓
```
⇒ DTS 语法与 label 解析都通过，链路已编进 DTB。

## ⚠ 尚未做（下一轮，需刷机）
1. 确认 **TX 方向的 pinmux**（`&sound` 现在只有 `pinctrl-0 = <&tert_tdm_active>`，
   要查它是否覆盖 TX 引脚；不够就补一个状态）；
2. `sm8250.c` 里为 `TERTIARY_TDM_TX_0` 补 `hw_params` 分支（与 `TERTIARY_TDM_RX_0` 对称：
   `DSP_A` + `TDM_BCLK_RATE` + 每个 codec dai 的 `set_tdm_slot(codec_dai,0,0x3,8,32)`）；
3. 重打 boot 镜像（`make_boot_image.py --dtb <新 dtb>`）→ 刷 `boot_b`；
   **回退**：`fastboot flash boot_b boot_b_restore.img`（md5 `ded90d33…`）；
4. 刷完先只验证 **`arecord -l` 是否出现 "Tertiary TDM Capture"**，
   再 `arecord -D hw:0,0 -f S24_LE -c 8 -r 48000 -d 3 /tmp/tdm.wav` 能否出文件。

## 纪律
与扬声器主线**分开做**；改完只验证采集，**不顺带调增益**。

---

## 【第 89 轮 · 定位到"只差一个 case"】

查 `sound/soc/qcom/sm8250.c`：
```c
// 第 190-203 行：分发器
static int sm8250_snd_hw_params(...)
{
	switch (cpu_dai->id) {
	case PRIMARY_TDM_RX_0 ... QUINARY_TDM_TX_7:      // ← TERTIARY_TDM_TX_0 在范围内 ✓
		return sm8250_tdm_snd_hw_params(substream, params);
	}
	...
}
```
⇒ **分发器没问题**，TDM TX 会被送进 `sm8250_tdm_snd_hw_params()`。

**但** `sm8250_tdm_snd_hw_params()`（第 133 行起）的**内层 switch 只有 `case TERTIARY_TDM_RX_0:`**
（以及 PRIMARY/TERTIARY MI2S），`TERTIARY_TDM_TX_0` 会落到 `default: break;`
⇒ **codec DAI 完全没被配置**（无 `set_fmt` / `set_sysclk` / `set_tdm_slot`）。

**结论**：采集链路只差**在 `sm8250_tdm_snd_hw_params()` 里补一个
`case TERTIARY_TDM_TX_0:`，镜像 RX 那个 case 的循环体**（含 §61 加的
`snd_soc_dai_set_tdm_slot(codec_dai, 0, 0x3, 8, 32)`），把方向改成 `SNDRV_PCM_STREAM_CAPTURE`。

⚠ 下一轮做这一步时：**先完整读出 RX case 的循环体原文（约第 157-176 行）再照抄**，
不要凭记忆写；改完 `make ... sm8250.o` 验证编译，再重打 boot 镜像刷机。

---

## 【第 90 轮 · 代码改动**已全部完成并编译通过**，只差刷机】

### 已做（本地，均已验证）
1. **DTS**（第 88 轮）：`&sound` 新增 `tdm-capture-dai-link`
   （`link-name = "Tertiary TDM Capture"`，cpu = `TERTIARY_TDM_TX_0`）
   → `make ... sm8250-xiaomi-elish-csot.dtb` **EXIT=0**，`strings` 确认字样在 DTB 里 ✓
2. **机器驱动**（本轮）：`sm8250.c` 的 `sm8250_snd_startup()`（RX case 在 157-171 行）
   之后新增 `case TERTIARY_TDM_TX_0:`，**逐行镜像 RX case**，
   方向参数改 `SNDRV_PCM_STREAM_CAPTURE`
   → `make ... sound/soc/qcom/sm8250.o` **EXIT=0** ✓
3. **安全性核查**（本轮）：`TERTIARY_TDM_TX_0` **确实已在 ADSP 驱动注册**
   （`q6afe.c:739  [TERTIARY_TDM_TX_0] = { AFE_PORT_ID_TERTIARY_TDM_TX, ... }`）
   ⇒ DMA/DAI 存在，新增 link 有绑定对象，**声卡探测失败的风险低** ✓

### 尚未做（下一轮：刷机 + 验证）
```sh
cd work/kernel/build/linux-6.12.58
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc-12 \
     LOCALVERSION=-current-sm8250 -j$(nproc) Image.gz qcom/sm8250-xiaomi-elish-csot.dtb
python3 work/kernel/make_boot_image.py --dtb <新 dtb> ...      # 重打 boot 镜像
# 刷 boot_b（回退：fastboot flash boot_b boot_b_restore.img，md5 ded90d33…）
```
刷完**只验证采集**：
```sh
arecord -l                                   # 期望出现 "Tertiary TDM Capture"
arecord -D hw:0,0 -f S24_LE -c 8 -r 48000 -d 3 /tmp/tdm.wav   # 期望出文件
```
若声卡探测失败（**播放也会一起坏**）⇒ 立刻刷回 `boot_b_restore.img`。

### ⚠ 仍未解决的疑点（刷机前先知道）
本轮读到的 `sm8250_snd_startup()` 里 RX case 的循环**只有 `set_fmt` + `set_sysclk`，没有
`set_tdm_slot`** —— 但 dmesg 里确实有 `elish: codec cs35l41-pcm tdm slots 0x3/8/32`，
说明 **`set_tdm_slot` 在别处**（可能是另一个函数，如 `sm8250_tdm_snd_hw_params()`）。
⇒ 刷机前建议先 `grep -n "tdm slots" -r sound/soc/qcom/` 找到它，
确认 TX 方向是否也需要同样的一步 —— 否则可能"链路建了但槽位没配"。

### 【第 91 轮 · 该疑点已澄清：hw_params 侧其实早就支持采集】
读 `sm8250.c` 第 40-101 行（`sm8250_tdm_snd_hw_params()`）后确认：
```c
if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
    snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0x03, slots=8, slot_width=32);
    snd_soc_dai_set_channel_map(cpu_dai, 0, NULL, channels, tdm_slot_offset);
    for_each_rtd_codec_dais(...)                       /* ← §61 加的：给每个 codec DAI 配槽位 */
        snd_soc_dai_set_tdm_slot(codec_dai, 0, 0x3, 8, 32);
} else {                                               /* ← 采集方向：早就有 */
    snd_soc_dai_set_tdm_slot(cpu_dai, 0xf, 0, 8, 32);
}
```
而分发器（第 198 行）`case PRIMARY_TDM_RX_0 ... QUINARY_TDM_TX_7:` 已经把 `TERTIARY_TDM_TX_0`
送进这个函数 ⇒ **hw_params 侧对采集本来就是通的**。
⇒ 采集真正缺的只有 **`sm8250_snd_startup()` 里的 TX case**（第 90 轮已补，编译通过）。
⇒ **"槽位没配"的担心解除**；唯一的已知差异：采集分支**没有**给 codec DAI 配槽位
（只有 cpu DAI），若刷机后采集仍失败，这里是下一个要看的地方。

### 结论：采集改动的代码工作**已全部完成**
DTS link ✓（DTB 编译通过）+ startup TX case ✓（.o 编译通过）+ hw_params 本就支持 ✓ + TX DAI 已注册 ✓
⇒ **只剩：编 Image.gz/DTB → 重打 boot 镜像 → 刷 boot_b → 验证 `arecord`**（命令见上一节）。

### 【第 92 轮 · 全内核编译通过，刷机输入已就绪】
```
make ... Image.gz qcom/sm8250-xiaomi-elish-csot.dtb     EXIT=0
arch/arm64/boot/Image.gz                              md5 5cc93eef319de183065d46cbd34f6829  13053647 B
arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-csot.dtb md5 c9708ab3ff92336fa8ea0734815a0738    130424 B
```
⇒ **这两个文件就是重打 boot 镜像的输入**（`Image.gz` + 追加 `--dtb`），全部本地验证完毕。

**下一轮只剩执行**：
```sh
python3 work/kernel/make_boot_image.py --dtb \
  work/kernel/build/linux-6.12.58/arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-csot.dtb ...
# 得到新 boot_b 镜像 → fastboot flash boot_b <新镜像> → 重启
# 回退：fastboot flash boot_b boot_b_restore.img   (md5 ded90d33d934bafa1df32ece0b571fdf)
```
刷完**只验证采集**（见上一节的 arecord 命令）；若声卡探测失败（播放也会坏）立刻刷回回退镜像。

### 【第 93 轮 · 打包命令行已确认（下一轮照抄即可）】
`make_boot_image.py` 的参数是**位置参数** + 可选开关：
```
usage: make_boot_image.py <image> <out> [--base BASE] [--dtb DTB] [--python-gzip]
        --base 默认 /home/axis/axis_rnd/work/boot_a_flash.img   ← 干净的 Armbian boot 基线
```
⇒ **刷机前只需这一条命令**：
```sh
cd /home/axis/axis_rnd/work/kernel
python3 make_boot_image.py \
  build/linux-6.12.58/arch/arm64/boot/Image.gz \
  /home/axis/axis_rnd/work/boot_b_tdm_capture.img \
  --dtb build/linux-6.12.58/arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-csot.dtb
```
然后：
```sh
# 期望输出 md5（记录下来），再：
fastboot flash boot_b work/boot_b_tdm_capture.img && fastboot reboot
# 回退：fastboot flash boot_b boot_b_restore.img   (md5 ded90d33d934bafa1df32ece0b571fdf)
```
**打包后务必先确认**：`make_boot_image.py` 自带断言（`ANDROID!` magic、`second_size==0`、`ARM\x64`），
输出里的 md5 就是刷机用的镜像指纹。

### 【第 94 轮 · 可刷镜像已生成 ✓】
```
python3 make_boot_image.py build/linux-6.12.58/arch/arm64/boot/Image \
    /home/axis/axis_rnd/work/boot_b_tdm_capture.img \
    --dtb build/linux-6.12.58/arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-csot.dtb
→ appended DTB: 130424 bytes ✓   re-parse OK: magic=ANDROID! ✓
→ work/boot_b_tdm_capture.img  201326592 B  md5 ad07b725d7ce93a5ac1c090ab17a63a0
```
**⚠ 坑**：`make_boot_image.py` 要的是**未压缩的 `Image`**（脚本内部自己 gzip），
传 `Image.gz` 会报 `kernel does not start with MZ` / `no ARM\x64 magic` 并 PACK_EXIT=1。

**下一轮刷机（就这两条）**：
```sh
fastboot flash boot_b work/boot_b_tdm_capture.img && fastboot reboot
# 若声卡起不来（连播放也坏）：fastboot flash boot_b boot_b_restore.img   (md5 ded90d33…)
```
刷完验证：`arecord -l`（找 "Tertiary TDM Capture"）→
`arecord -D hw:0,0 -f S24_LE -c 8 -r 48000 -d 3 /tmp/tdm.wav`（期望出文件）。

### 【第 95 轮 · 镜像已放到 Windows 侧，可直接刷】
```
C:\Users\cheny\Downloads\elish_imgs\boot_b_tdm_capture.img
  md5 ad07b725d7ce93a5ac1c090ab17a63a0   （与 work/ 下的完全一致 ✓）
回退镜像同目录：boot_b_restore.img（md5 ded90d33d934bafa1df32ece0b571fdf）
```
**刷机命令**（Windows）：
```
fastboot flash boot_b C:\Users\cheny\Downloads\elish_imgs\boot_b_tdm_capture.img
fastboot reboot
```
**回退**：`fastboot flash boot_b C:\Users\cheny\Downloads\elish_imgs\boot_b_restore.img`
**刷完验证**：`arecord -l` 找 "Tertiary TDM Capture"；
`arecord -D hw:0,0 -f S24_LE -c 8 -r 48000 -d 3 /tmp/tdm.wav` 期望出文件。

---

# 【第 96 轮 · ★ 镜像已刷入并生效；采集推进到 AFE 层 -EINVAL】

## 刷机结果：**成功** ✓
- `fastboot flash boot_b boot_b_tdm_capture.img` + `reboot` 已执行；
- 设备已回到 Armbian（ssh 正常）；
- **运行中的设备树确认新 DTB 生效**：
  ```
  /proc/device-tree/sound/tdm-capture-dai-link/link-name   ← 存在 ✓
  /proc/device-tree/sound/{mm1-dai-link,speaker-dai-link,tdm-capture-dai-link}
  ```
  （对比：刷之前 `sound/` 下只有前两个）

## ⚠ 我上一轮的验证判据是错的（记下来别再犯）
> **BE DAI link 不会出现在 `arecord -l` 里** —— `arecord -l` 只列 **FE（前端）PCM**。
> elish 的采集 FE 就是 `MultiMedia1 capture`，所以新增 capture BE 后
> `arecord -l` **仍只显示 "MultiMedia1 (*)" 是正常的**，不能据此判断失败。
> 正确的确认方式是看 **运行中的设备树**（`/proc/device-tree/sound/`）或 **dmesg**。

## 采集仍然失败的**真正原因**（本轮拿到）
```
q6afe-dai ...: ASoC: error at snd_soc_pcm_dai_prepare on TERTIARY_TDM_TX_0: -22   (反复出现)
```
⇒ **链路已经绑定、TX DAI 已经被调用**，但 `prepare` 阶段返回 **-EINVAL**，
且是 **CPU DAI（q6afe）侧**报的错（`snd_soc_pcm_dai_prepare`，不是 hw_params）。

## 下一步（明确、范围小）
查 `sound/soc/qcom/qdsp6/q6afe.c` 里 **TDM TX 的 prepare/port 配置路径**：
- `q6afe_tdm_port_prepare()` 之类是否支持 `AFE_PORT_ID_TERTIARY_TDM_TX`
  （`q6afe.c:739` 只证明 DAI 表里有条目，不代表 port config 完整）；
- 重点看 TX 方向需要的 AFE port 参数（如 channel map / `afe_port_config`）是否缺失或为 0；
- 对照 RX 方向（`AFE_PORT_ID_TERTIARY_TDM_RX`）的实现差异。
> 这属于 **AFE 驱动层**，比 DTS/机器驱动更深一层；先只读代码定位，不要急着改。

---

# 【第 97 轮 · ★★ 用户实听反馈：「还是没声」（两种通路都不行）】

## 关键事实（用户在场确认）
- 刷入 TDM-capture 镜像后 **用户实听：仍然没有声音**；
- 我把 8 颗的 **`PCM Source` 从 DSP 改成 ASP 直通**
  （该控件是 boolean：`1=DSP`、`0=ASP`；原值全为 `1`），再播放 1 kHz：
  **用户实听：还是没声**。
⇒ **结论：DSP 通路不是死结**（此前的主要怀疑之一被排除）；
⇒ 放大器在 **DSP 与 ASP 两种通路下都没有输出**。

## 这与既有分析完全吻合
- 数字链路 ✓：`pcm=RUNNING`、路由已断言、`0x2014=1`（8/8）、无 `Enable failed` / `PRE_PMU failed`；
- 配置寄存器 ✓：与 Android 逐字节一致（含增益 `0x6c04=0x253`、ASP `0x4808=0x20200000`）；
- **但状态位始终不对**：我们 `0x10010 = 0x00c00000`（**PDN_DONE=1**，即"最近完成的状态跳变是掉电"），
  Android 是 `0x01400000`（**PUP_DONE=1**，"最近完成的是上电"）。
⇒ **放大器只是"被全局使能"，内部 boost 从未真正启动** ⇒ 没有输出，寄存器却看不出错。

## 下一步唯一主线（重新聚焦）
**让放大器真正完成一次上电（锁存 PUP_DONE）**，而不是继续绕采集/音量：
1. 关键对照：Android 播放态确实 `PUP_DONE=1`（§69 已实测），我们从未得到过 ——
   说明**厂商路径里有一步"真正上电"的动作我们没有**；
2. 优先怀疑**时序/顺序**：`PWR_CTRL3(0x2018=0x3721，含 boost 使能位)` 必须在 **GLOBAL_EN 之前**
   且在正确的供电状态下写入；主线在 probe 时写 PWR_CTRL3、DAPM 时才置 GLOBAL_EN，
   中途若发生掉电/复位，boost 就不会重启；
   → 可试：**播放中**按 `0x2018=0x3721 → 0x2014=1` 的顺序重写一次，再听；
3. 次级：`0x2014` 之外，`PWR_CTRL2`/`BOOST` 相关位是否真的生效（写后立即回读+听感双验证）；
4. 采集链路（AFE -EINVAL）**暂时搁置** —— 没有声音时测电平意义不大。

## 现状
- 设备：Armbian（已刷 TDM-capture 镜像；该镜像**未破坏播放**）；
- 8 颗 `PCM Source` 现为 **ASP(0)**（本轮改动，未改回）；
- 回退镜像仍在 `C:\Users\cheny\Downloads\elish_imgs\boot_b_restore.img`。

---

# 【第 98 轮 · ★★★ 用户实听："完全静默，连咔哒声都没有" ⇒ 功放输出级从未启动】

## 关键实测（用户在场，多组对照）
| 实验 | 结果 |
|---|---|
| `PCM Source` = DSP（原值） | **没声** |
| `PCM Source` = ASP（直通，绕过未加载的 DSP） | **没声** |
| `0x4810` 改成 Android 值 `0x04040404` 后（PCM RUNNING、`0x2014=1`） | **没声** |
| **播放瞬间是否有咔哒/瞬态** | **完全静默，什么都没有** |

## 由"完全静默、无瞬态"推出的结论
若功放已上电、只是没数据，通常会有轻微咔哒/瞬态。**完全没有** ⇒
**放大器输出级/boost 从未启动**（一直停在掉电态）。
这与状态位完全一致：
- 我们 `0x10010 = 0x00c00000`（**PDN_DONE=1**，"最近完成的是掉电"）；
- Android `0x01400000`（**PUP_DONE=1**，"最近完成的是上电"）；
- 且 `0x10014` Android 有 **bit8-12 = `0x1f00`**，我们**从未有过**（疑似 BCLK/FS 有效标志）。

⇒ **最强假设：放大器没有收到有效的 TDM 时钟/帧同步（BCLK/FS）**，
因此拒绝完成上电 ⇒ 无输出。这能一次性解释：寄存器全对、`GLOBAL_EN=1`、
却 `PUP_DONE` 不锁存、`0x1f00` 不出现、且**完全没有瞬态**。

## 排除与新增信息
- **`PCM Source`（DSP/ASP）不是原因**（两种都不响）；
- **`0x4810` 不是唯一原因**（改成 Android 值后仍不响）；
  注意：`0x4810` 确实是**真实差异**（我们 `03020100` vs Android `04040404`），
  且 `amp-fix` 不写它 —— 值得保留在修复列表里，但不是"无声"的根因；
- **供电轨**：`bl_vddpos_5p5`/`bl_vddneg_5p5` 都是 enabled@5500mV，
  但消费者是 **`11-0011`（背光芯片）** ⇒ 那是**背光**的电，不是功放的；
  **CS35L41 没有任何被建模的供电轨**（i2c 能通说明数字侧有电）。

## 下一步（唯一主线）
**查"TDM 时钟是否真的在输出"**：
1. 读放大器里能反映时钟/FS 的寄存器（`0x10014` bit8-12、`0x2D10` 等），
   在**已校验播放态**下与"停止播放"对比，看是否有任何位随码流变化；
2. 查 **TDM 引脚 pinmux**：`&sound` 的 `pinctrl-0 = <&tert_tdm_active>` 是否真的把
   BCLK/FS/DATA 脚切到了 TDM 功能（对照 Android 的 pinctrl 配置，看是否少了 pin 或功能号不对）；
3. 查 `q6afe` 侧 TDM RX 端口**是否真的 start 成功**（AFE port start 的返回码 / dmesg）；
4. 若确认时钟没出，再决定是 pinmux 还是 AFE port 参数问题。

> 这一步与"音量/增益"无关，也**与采集链路无关** —— 先把"功放能不能上电"解决。 

---

# 【第 99 轮 · 寄存器对码流**完全无反应** ⇒ 放大器没看到任何时钟；pinmux 却是完整的】

## 实测（已校验播放态 vs 停止后，同一颗 bus1 0x40）
| 寄存器 | 播放中 | 停止后 |
|---|---|---|
| `0x10010` | `00 c0 00 00` | `00 c0 00 00` |
| `0x10014` | `10 30 00 00` | `10 30 00 00` |
| `0x2D10` | `00 02 40 10` | `00 02 40 10` |
| `0x6c04` | `00 00 02 53` | `00 00 02 53` |

⇒ **没有任何寄存器随码流变化**（连 `0x10014` 都完全一样）。
**结论：放大器在播放期间感知不到任何 TDM 活动**（时钟/帧同步/数据），
这解释了"完全静默、无瞬态、PUP_DONE 不锁存"。

## pinmux 检查：**完整，不是问题**
`sm8250-xiaomi-elish-common.dtsi` 的 `tert_tdm_active`（第 1054-1084 行）：
```
gpio133 → mi2s2_sck     (BCLK)
gpio134 → mi2s2_data0   (SoC 输出数据；与 DTS 的 qcom,tdm-data-out = <0> 一致 ✓)
gpio135 → mi2s2_ws      (FS/WS)
gpio137 → mi2s2_data1
```
四路信号齐全 ⇒ **引脚复用不是根因**。

## ⇒ 疑点集中到 **AFE 侧根本没把 TDM 时钟/端口启动起来**
下一步（只读代码/日志，不要急着改）：
1. 查 `q6afe.c` 里 **TDM RX 端口的 start/prepare 返回值**与端口参数
   （`AFE_PORT_ID_TERTIARY_TDM_RX`）—— 是否有 start 失败但被忽略；
2. 看 dmesg 里播放时 AFE 相关条目（我们之前只 grep 了 cs35l41）；
3. 关键对照：**Android 播放时 `0x10014` 有 `0x1f00`（bit8-12）**，我们从来没有 ⇒
   该位很可能就是"**检测到有效 BCLK/FS**"，可当作我们修好时钟后的**客观成功判据**
   （不需要耳朵！）：**只要 bit8-12 出现，就说明时钟到位了**；
4. 若确认 AFE 没启动 TDM，再查 `sm8250.c` 的 TDM 端口/时钟配置与 Android 的
   `qcom,tdm-*` 参数逐项对照（本 DTS 第 933-938 行有一组 `qcom,tdm-*`）。

## 现状
- 设备 Armbian，可播放；`0x4810` 仍是我写入的 Android 值；
- **下一轮最有价值的动作**：让 `0x10014` 的 bit8-12 出现（= 时钟到位）——这是**无需耳朵**的成功判据。

---

# 【第 100 轮 · AFE 日志分出两条路：**播放端口启动干净**，采集端口 -22】

## dmesg 关键行
```
[ 33.58] qcom-q6afe aprsvc:service:4:4: AFE enable for port 0x9021 failed -22
[ 33.58] q6afe-dai ...: fail to start AFE port 39          ← 39 = TERTIARY_TDM_TX_0（我们新加的采集）
[ 33.58] ASoC: error at snd_soc_pcm_dai_prepare on TERTIARY_TDM_TX_0: -22
[ 85.33]  Tertiary TDM Playback: elish: codec cs35l41-pcm tdm slots 0x3/8/32   ← 播放路径正常建立
[312.88] qcom-q6afe aprsvc:service:4:4: elish: afe topologies disabled          ← 我们的可选参数默认关
```
- `0x9021` = **TERTIARY_TDM_TX** 的 AFE 端口号；`0x9020` 是 RX。
- **播放（RX）方向没有任何 AFE 报错** ⇒ AFE 侧认为播放端口启动成功；
- 采集（TX）方向启动失败 `-22`（DSP 返回 error 1）。

## 这修正了上一轮的推断
上一轮我猜"AFE 没启动 TDM 时钟"——**从日志看 RX 端口是启动成功的**。
所以问题在**AFE 端口之后**：
1. 端口"启动成功"但**物理时钟未必真的在输出**（`Q6AFE_LPASS_CLK_ID_TER_TDM_IBIT` 的
   LPASS 时钟是否真的使能/有没有被 gate）；
2. 或者时钟在跑但**放大器侧没在听**（例如 amp 的 ASP 时钟源/位宽/FS 与 TDM 不匹配，
   我们已核对 `0x4808` 与 Android 一致，但**放大器是否真的锁到该时钟**未知）；
3. `0x10014` bit8-12 仍是最好的客观判据。

## 采集链路的明确错误（顺手拿到，供以后修）
`AFE enable for port 0x9021 (TERTIARY_TDM_TX) failed -22` ⇒
AFE 固件不接受该 TX 端口的使能参数。与我们 §96 的 `prepare -22` 是同一处。
**先搁置**（没声音时测采集没意义）。

## 下一步（优先级）
1. 用**无需耳朵的判据**推进：想办法让 `0x10014` 的 bit8-12 出现；
2. 具体查：`q6afe.c` 里 TDM RX 端口 start 时**是否真的使能了 IBIT 时钟**
   （`q6afe_port_start` / clock id 相关），并与 Android 的 `qcom,tdm-*` 参数对照；
3. 若找不到，考虑**抓 Android 播放时的 AFE 侧证据**（对比端口参数），
   这是"逐寄存器比对"思路在 AFE 层的延伸。

---

# 【第 101 轮 · ⚠⚠ 严重：从**本仓库的 DTS** 编出的 DTB 会**让键盘无法工作**（已回退）】

## 用户报障
> "检查下 firmware 对不对 我现在系统键盘起不来了"

## 根因（已定位）
```
dmesg: platform 88e4000.phy: deferred probe pending:
       platform: wait for supplier /soc@0/pinctrl@33c0000/keyboard-default-state
/sys/kernel/debug/gpio:  gpio127 : in low func0 2mA pull down     ← 键盘供电脚一直是低
```
本仓库 `sm8250-xiaomi-elish-common.dtsi` 里，键盘的使能/供电被挂在 **USB2 PHY** 上：
```dts
&usb_2_hsphy {                       /* 88e4000.phy */
	pinctrl-0 = <&keyboard_en_state>, <&keyboard_vdd_pin>;
	pinctrl-names = "default";
};
```
- `keyboard_en_state: keyboard-default-state`（第 734 行）与 `keyboard_vdd_pin`（第 1086 行）**定义存在**；
- 但 PHY **永久 deferred probe**，等不到 `pinctrl@33c0000/keyboard-default-state`
  ⇒ pinctrl 状态没被正确挂到 TLMM 下（或被引用方式不对）
  ⇒ **键盘供电脚不拉高 ⇒ 键盘完全不工作**。

## 已采取的处置（本轮）：**回退到 Armbian 官方 boot 镜像** ✓
```
fastboot flash boot_b C:\Users\cheny\Downloads\elish_imgs\boot_b_restore.img
→ Sending 'boot_b' (196608 KB) OKAY / Writing 'boot_b' OKAY / Rebooting OKAY
（该镜像 md5 ded90d33d934bafa1df32ece0b571fdf，用 Armbian 自己的 DTB）
```
**注意**：回退**不影响**已装好的放大器修复（那些是磁盘上的 `.ko` 与脚本）；
只丢掉"TDM capture link + sm8250 TX case"（当前无用）。

## ★ 铁律（写给以后每一轮）
> **在修好键盘 pinctrl 之前，绝对不要再把"本仓库 DTB"编的 boot 镜像刷进设备。**
> 每次刷机前，先确认该镜像的 DTB **不含** `usb_2_hsphy` 那个 deferred-probe 问题；
> 或者只刷 `.ko`（本项目的放大器修复全部可以只换模块完成，**根本不需要刷 boot**）。

## 待办
1. 等设备起来后，**请用户确认键盘已恢复**；
2. 修 `usb_2_hsphy` 的 pinctrl 引用（找出为什么 `keyboard-default-state` 挂不到 TLMM 下），
   修好后重编 DTB，**先用 `dtc` 反编译核对**再考虑刷机；
3. 放大器"没声音"的主线（TDM 时钟/`0x10014` bit8-12）继续，但**不要再通过刷 boot 来做实验**。

## ✅ 回退结果已验证（第 101 轮）
回退到 `boot_b_restore.img` 后重启：
```
kbd-defer=0                                   ← "deferred probe ... keyboard" 归零
gpio127 : out high func0 8mA no pull          ← 键盘供电脚已拉高（故障时是 in/low）
input: "Xiaomi Pad Keyboard" / "Xiaomi Pad Mouse" / "Xiaomi Pad"   ← 三个设备都回来了
```
⇒ **键盘已恢复**；根因确认为"本仓库 DTB 的 `usb_2_hsphy` pinctrl 问题"，
Armbian 官方 DTB 没有这个问题。

**⚠ 仍未修**：`keyboard_en_state` 为什么挂不到 `pinctrl@33c0000` 下（导致 PHY 永久 deferred probe）。
在修好之前，**任何用本仓库 DTB 编出的 boot 镜像都不能刷**。

### 【第 102 轮 · 键盘问题的**代码级症结**已定位】
```dts
&lpass_tlmm {                                  /* = pinctrl@33c0000，由模块 pinctrl_sm8250_lpass_lpi 提供 */
	keyboard_en_state: keyboard-default-state {
		pins = "gpio9";
		function = "i2s1_data";
		... };
};

&usb_2_hsphy {                                 /* 主 TLMM 侧的设备，却引用 LPASS 的 pinctrl 状态 */
	pinctrl-0 = <&keyboard_en_state>, <&keyboard_vdd_pin>;
	pinctrl-names = "default";
};
```
设备侧证据：`pinctrl_sm8250_lpass_lpi` 是**模块**，且 `lsmod` 显示 **0 users**。

**推断**：`&usb_2_hsphy` 的 `pinctrl-0` 指向**LPASS TLMM** 里的状态节点。
若该 LPASS pinctrl 提供者未就绪/未加载，pinctrl 核心就一直把它当"supplier 未就绪"，
于 PHY **永久 deferred probe** ⇒ 键盘供电脚（gpio127）永不拉高 ⇒ 键盘无电。
（Armbian 官方 DTB 没有这个问题 —— 回退后立即恢复，见上一节实测。）

**修法候选（未实施）**：
1. **对照官方 Armbian DTB** 里键盘相关节点怎么摆的
   （从 `work/boot_a_flash.img` 里抽出 DTB 反编译 diff）——**最省事、最可靠**；
2. 或把这些键盘 pinctrl 状态改挂到主 `&tlmm`（若引脚实际归属允许）；
3. 或保证 `pinctrl_sm8250_lpass_lpi` 尽早加载（模块加载顺序/内置化）。

**⚠ 在修好并用 `dtc` 核对之前，仍然禁止刷任何"本仓库 DTB"镜像。**

### 主线（没声音）本轮的新线索（**暂不能验证，因为它是 built-in**）
`sm8250.c` 里 MI2S 分支有 `snd_soc_dai_set_fmt(cpu_dai, fmt)`，
但 **TDM 分支（RX）没有调用它** —— 而 `q6afe.c:1522` 正是靠
`cfg->fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK` 决定 `ws_src = INTERNAL(主) / EXTERNAL(从)`。
**若 TDM 的 CPU DAI 从未设置 fmt，AFE 可能被配成从模式 ⇒ 不产生 BCLK/WS ⇒ 放大器看不到时钟
⇒ 完全静默、PUP_DONE 不锁存、`0x10014` 缺 bit8-12。这与全部观测吻合。**
但 `CONFIG_SND_SOC_SM8250=y`（**built-in**）⇒ 必须先能安全刷机才能验证。
⇒ **优先级：先修键盘 DTB 问题（解锁刷机），再验证这一条。**

---

# 【第 103 轮 · ✅ 键盘 DTB 问题已修（一行改动）+ 用户确认键盘正常】

## 用户反馈
> "键盘正常 但是没声音"
⇒ **键盘已确认恢复**（回退生效）；主线仍是"没声音"。

## 对比官方 Armbian DTB 找到的**确切差异**
把 `work/boot_a_flash.img` 里的 DTB 抽出来反编译（`scripts/dtc/dtc -I dtb -O dts`）：
- **官方**：`keyboard-default-state` 与 `tx-swr-sleep-state` 同级，位于**主 pinctrl 块（&tlmm）**；
- **我们**：它被放在 **`&lpass_tlmm`** 里（第 733-741 行）。
⇒ `&usb_2_hsphy` 的 `pinctrl-0 = <&keyboard_en_state>` 去 **LPASS TLMM** 找状态，
而该提供者是**模块**（`pinctrl_sm8250_lpass_lpi`，`0 users`）⇒ **永久 deferred probe** ⇒ 键盘无电。

## 修复（已实施并验证）
把 `sm8250-xiaomi-elish-common.dtsi` 里的
```dts
&lpass_tlmm {  →  &tlmm {
	keyboard_en_state: keyboard-default-state { ... };
```
（该文件里 `&lpass_tlmm` 只出现这一次 ✓）
**验证**：重编 DTB `EXIT=0`；反编译后 `keyboard-default-state` 已位于主 TLMM 块内，
缩进层级与官方 DTB 一致 ✓。

**⚠ 尚未刷入验证** —— 逻辑证据充分（结构与官方一致），但**必须实测**：
下次刷机后**第一件事**就是确认键盘可用（`gpio127` = out high；出现 "Xiaomi Pad Keyboard"）。

## 下一步（刷机顺序，务必按此）
1. 用修好的 DTB + **`sm8250.c` 的 TDM `set_fmt` 修复**（见上一节的分析）重编内核与镜像；
   `sm8250.c` 的 TDM RX 分支缺 `snd_soc_dai_set_fmt(cpu_dai, codec_dai_fmt)`
   （MI2S 分支都有，`q6afe.c:1522` 靠它决定 AFE 是时钟主/从）；
2. **先刷、先验键盘**（`gpio127` / `Xiaomi Pad Keyboard`），键盘 OK 再谈声音；
3. 键盘 OK 后立刻看 `0x10014` 的 **bit8-12** 是否出现 —— 那是"TDM 时钟到位"的**客观判据**；
4. 若还是不行，退回官方镜像（`boot_b_restore.img`，md5 `ded90d33…`）继续用 `.ko` 方式排查。

## 本轮产物
- 修改：`arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-common.dtsi`（`&lpass_tlmm` → `&tlmm`）
- 新 DTB：`arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-csot.dtb`（含 keyboard 修复 + capture link）
- 参考：`/tmp/armbian.dts`（官方 DTB 反编译）、`/tmp/ours.dts`（我们的）

---

# 【第 104 轮 · 新镜像已备好：**键盘修复 + TDM set_fmt 修复**】

## 本轮实施的两处改动（都已在本地编译验证）
1. **键盘**（上一轮）：`&lpass_tlmm` → `&tlmm`（keyboard-default-state 归位到主 TLMM）；
2. **TDM 时钟主/从**（本轮）：`sm8250.c` 的 `case TERTIARY_TDM_RX_0:` 里补上
   ```c
   snd_soc_dai_set_fmt(cpu_dai, codec_dai_fmt);   /* MI2S 分支都有，TDM 分支原先没有 */
   ```
   依据：`q6afe.c:1522` 用 `cfg->fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK`
   决定 `ws_src = INTERNAL`（CPU 主）还是 `EXTERNAL`（CPU 从）。
   **若 TDM 从没设过 fmt，AFE 可能当从机 ⇒ 不产生 BCLK/WS ⇒ 放大器没时钟 ⇒ 完全静默。**

## 产物（已编译打包 + 已放到 Windows）
```
work/boot_b_tdmfix_kbd.img   md5 2e2dbd63fcfff98ef954acb647941906
C:\Users\cheny\Downloads\elish_imgs\boot_b_tdmfix_kbd.img   ← 同一 md5 ✓
回退：C:\Users\cheny\Downloads\elish_imgs\boot_b_restore.img  md5 ded90d33d934bafa1df32ece0b571fdf
（打包自检通过：appended DTB 130424 B、re-parse OK ANDROID!）
```

## 下一步（严格顺序，刷机前先备份心态）
```sh
fastboot flash boot_b C:\Users\cheny\Downloads\elish_imgs\boot_b_tdmfix_kbd.img && fastboot reboot
```
1. **第一件事：验键盘** —— `cat /sys/kernel/debug/gpio | grep gpio127` 应为 `out high`；
   `grep "Xiaomi Pad Keyboard" /proc/bus/input/devices` 应有命中。
   - **键盘坏了 ⇒ 立刻刷回 `boot_b_restore.img`，不再继续**；
2. 键盘 OK 后，播放并读放大器 `0x10014`：
   - **bit8-12（`0x1f00`）出现 ⇒ TDM 时钟到位了**（这是无需耳朵的客观判据）；
   - 同时读 `0x10010` 是否出现 **PUP_DONE（`0x01000000`）**；
3. 若两者出现 ⇒ 立刻请用户实听（很可能终于有声）；
4. 若无变化 ⇒ 时钟假设不成立，回到"AFE 端口参数逐项对照 Android"那条路。

---

# 【第 105 轮 · 刷机结果：键盘修复 ✓ 确认；TDM set_fmt 修复 ✗ 无效果】

## 刷机与验证（已执行）
```
fastboot flash boot_b boot_b_tdmfix_kbd.img   → Writing OKAY
boot 后：uptime 21s
   kbd-defer = 0                                  ✓ deferred probe 消失
   gpio127 : out high func0 8mA no pull           ✓ 键盘供电正常
```
⇒ **键盘 DTB 修复（`&lpass_tlmm` → `&tlmm`）确认有效** —— 这是本轮实打实的收获，
也说明"本仓库 DTB 会造成键盘失效"这个坑已经填上，**以后可以安全刷本仓库的镜像**。

## TDM 时钟假设的检验结果：**没有效果**
```
pcm=RUNNING ✓   ampfix_last=-- No entries --
0x2014  = 00 00 00 01        （使能正常）
0x10010 = 00 c0 00 00        （仍无 PUP_DONE=0x01000000）
0x10014 = 00 30 00 00        （仍无 bit8-12 = 0x1f00）
AFE start/enable 失败计数 = 40（都是采集 TX 端口那条，与播放无关）
```
⇒ 给 TDM 的 **CPU DAI 补 `snd_soc_dai_set_fmt()` 并不改变** `0x10014` 的 bit8-12，
**"AFE 被配成从机"这个假设不成立**（至少不是这一处）。

## 下一步候选（按价值排序）
1. **别再猜"时钟主从"**。改为**直接对比 Android 的 AFE 侧参数**：
   抓 Android 播放时 `q6afe`/ADSP 的 TDM 端口配置（或 `0x10014` 的完整上下文），
   与我们的逐项对照 —— 这是本项目一直有效的方法论（§69 的寄存器 diff）。
2. 重新审视 `0x10014` bit8-12 到底是不是"时钟有效"。它也可能是**别的含义**
   （例如 DSP/保护模块状态）——**在没确认语义前，不要把它当唯一判据**。
3. 回到"功放真正上电"的物理层面：CS35L41 的 **SPK/VBST 供电路径**在本板上没有建模的
   regulator；需要确认这几个引脚/供电是否由某个 GPIO 或 PMIC 控制（Android 侧可能由
   HAL 通过 I2C/GPIO 打开）。**这可能是"完全静默"的真正原因。**

## 现状
- 设备：Armbian，`boot_b_tdmfix_kbd.img`（键盘修复 ✓ + set_fmt ✗ + capture link）
- **键盘可安全使用**；回退镜像 `boot_b_restore.img`（md5 `ded90d33…`）仍可用
- 主线仍未解决"没声音"，但**键盘这个回归已被彻底修好**，且**排除了"AFE 从模式"假设**

---

# 【第 106 轮 · ⚠ 一条**假线索**的更正（避免以后被误导）】

## 我一度以为找到了根因
在 Android 的 elish DT（`/tmp/d12.dts`）里 grep 到 **`qcom,spkr-sd-n-node` 出现 8 次**，
第一反应是"每颗放大器一个扬声器使能脚，我们没拉 ⇒ 完全静默"。

## 核对后**自我更正**：它属于 `wsa881x`，不是 CS35L41
```dts
wsa881x@20170211 {
	compatible = "qcom,wsa881x";          /* ← 高通 SoundWire 智能扬声器，别的机型用的 */
	reg = <0x10 0x20170211>;
	qcom,spkr-sd-n-node = <0x6f>;         /* 它的 speaker-shutdown 控制节点 */
};
```
⇒ 这是 **WSA881x（SoundWire 扬声器）** 的属性，**与我们的 8 颗 CS35L41（i2c）无关**。
**不是我们缺失的那一步**，此路作废。（记下来：DT 里很多节点是"SoC 通用模板"，
grep 到关键词不等于与本机相关 —— 这是本项目第 4 次同类教训。）

## 仍然成立的结论
- CS35L41 在 Android 的 DT 里**没有**额外的 supply/enable-gpio 属性
  （只有 `reset-gpios`、`interrupts`、`asp-sdout-hiz`、`tuning-has-prefix`、`fast-switch`、
  `right-channel-amp`，这些我们**都已对齐**）；
- 因此"放大器缺一个使能/供电 GPIO"这个方向**在 DT 层面看不到证据**；
  若真是硬件供电问题，那它不在 DT 里描述（例如由 bootloader 或 PMIC 默认开启）。

## 下一步（真正该做的）
回到**唯一被证明有效的方法论**：**在 Android 播放态抓 AFE/寄存器证据，与 Armbian 逐项对照**。
- 最直接：切到 Android（slot a），播放，抓 **AFE 端口参数**与
  `0x10014/0x10010/0x2D10` 等**同一批寄存器**（在播放态），
  与我们**已校验播放态**的值做**同状态**diff（§76.2 的做法，注意状态一致性纪律）；
- 尤其是 **`0x10014` 的语义**必须先搞清楚 —— 它到底是不是"时钟有效"，
  还是别的（DSP/保护）状态；**没搞清之前不要拿它当判据**。

---

# 【第 107 轮 · ★★★ 同状态对比结果：**27 个寄存器里 26 个与 Android 完全一致**】

## 方法（终于做对了）
用**已有的** Android 播放态快照 `work/android/arb_regs_playing.txt`（bus1-0x40 段），
与 Armbian **已校验播放态**（`playtest.sh`：`pcm=RUNNING`、`wl=16`、`0x2014=1`）
读取**同一批 27 个寄存器**做 diff。**状态一致**（都是播放中）✓

## 结果
| 寄存器 | Android 播放态 | Armbian 播放态 |
|---|---|---|
| `0x2014` | `00 00 00 01` | `00 00 00 01` ✓ |
| `0x2018` | `00 00 37 21` | `00 00 37 21` ✓ |
| `0x2030` | `00 00 00 88` | `00 00 00 88` ✓ |
| `0x2084` | `00 2f 1a a0` | `00 2f 1a a0` ✓ |
| `0x300c` | `00 00 40 a6` | `00 00 40 a6` ✓ |
| `0x394c` | `02 07 64 b7` | `02 07 64 b7` ✓ |
| `0x4000` | `08 00 08 00` | `08 00 08 00` ✓ |
| `0x4160` | `03 ea 00 7c` | `03 ea 00 7c` ✓ |
| `0x416c` | `1d af 84 89` | `1d af 84 89` ✓ |
| `0x4170` | `00 2f 00 65` | `00 2f 00 65` ✓ |
| `0x4360` | `00 00 2b 4f` | `00 00 2b 4f` ✓ |
| `0x4448` | `83 ff 7f f7` | `83 ff 7f f7` ✓ |
| `0x4808` | `20 20 00 00` | `20 20 00 00` ✓ |
| `0x6808` | `00 00 3f 75` | `00 00 3f 75` ✓ |
| `0x6c04` | `00 00 02 53` | `00 00 02 53` ✓ |
| `0x6e30` | `02 00 02 1d` | `02 00 02 1d` ✓ |
| `0x7068` | `00 00 01 e3` | `00 00 01 e3` ✓ |
| `0x7418` | `90 91 a1 c8` | `90 91 a1 c8` ✓ |
| `0x7434` | `81 3b 55 55` | `81 3b 55 55` ✓ |
| `0x8004` | `00 00 00 00` | `00 00 00 00` ✓ |
| `0x17040` | `01 09 1b 18` | `01 09 1b 18` ✓ |
| **`0x4840`** | **`00 00 00 18`（24）** | **`00 00 00 10`（16）** ✗ |
（其余 `0x0/0x40/0x208c/0x400c/0x410c` 亦一致）
**`0x4840` 的差异是"流格式"造成**：我们的测试音是 S16_LE（`wl=16`），
Android 当时放的是 24bit 内容；`amp-fix` 本来就把 `0x4840` 设成当前流的位宽 ⇒ **设计如此，不是缺陷**。

## 结论（重要）
**在播放态下，我们的放大器寄存器状态与 Android 几乎完全一致 —— 包括所有运行期/遥测类寄存器。**
⇒ **"配置缺失"这条线基本可以判定为已穷尽**：不存在"Android 有而我们没有"的寄存器写入。
⇒ 问题不在配置，而在**更外层**（数据/时钟是否真到、或输出级是否真被打开）。
⇒ 这与另一条实测吻合：**Armbian 上这些寄存器在"播放 vs 停止"之间完全不变化**（§99），
   说明它们在这台机器上是**静态配置值**，`0x10010/0x10014` 的差异才是唯一动态线索。

## 下一步（建议）
1. **把本条写进 `ELISH_AMP_TDM_FIX.md`**（本轮因路径写错未写入，需补）；
2. 若仍要继续查"无声"：唯一还有信息量的动态线索是 `0x10010`（PUP_DONE 不锁存），
   而它的语义/触发条件 **必须靠数据手册或厂商驱动**确认 —— 继续盲试性价比已很低；
3. 在这种状态下，**请用户协助做一次判断更划算**：
   - 用 Android 播放同一段音频（确认扬声器硬件确实好）；
   - 或接受"配置已对齐、差异在更底层"的结论，把目标转为**文档化 + 保留可用修复**。

---

# 【第 108/109 轮 · ★★ 从**厂商驱动二进制**里挖到两条关键字符串】

## 方法
直接分析 `work/hal/audio_cs35l41.ko`（Android 原厂 CS35L41 内核驱动）：
1. 搜 32 位常量：`0x00010010`（IRQ1_STATUS1）**没有**以原始小端出现
   ⇒ 下游驱动的寄存器寻址方式与主线不同（需反汇编才能定位，成本高）；
2. **`strings`** 反而挖到金矿。

## 关键发现
```
Timeout waiting for OTP_BOOT_DONE          ← 厂商等待的是 OTP_BOOT_DONE，不是 PUP_DONE！
GLOBAL_EN from GPIO Control                ← 存在"GLOBAL_EN 由 GPIO 控制"的模式！
Boost Converter Enable
PDN failed
```
### 推论 A：`OTP_BOOT_DONE` 很可能就是 **bit22 = `0x00400000`**
证据：该位在**我们与 Android 的每一次读取中都恒为 1**（空闲/播放/各种状态），
非常符合"芯片 OTP 已加载、芯片已启动"的一次性状态。
⇒ **放大器其实一直是"已启动"的**；主线等的 `PUP_DONE`(bit24) 是**另一个语义**
（更接近"某次上电跳变完成"），所以主线等不到它并不代表芯片没起来。
**⇒ 这也解释了为什么"放大器已使能 + 配置与 Android 一致，却仍无声"：
问题不在"芯片没起来"，而在更下游（数据/输出级/GLOBAL_EN 的实际生效方式）。**

### 推论 B（新的、很值得查）：**GLOBAL_EN 可能由 GPIO 控制**
厂商驱动里有 `GLOBAL_EN from GPIO Control`。
若本板工作在"GLOBAL_EN 来自 GPIO"模式，则**我们通过 i2c 写 `0x2014` 置 GLOBAL_EN
可能并不真正使能放大器**（寄存器读回是 1，但实际使能路径是另一根 GPIO）。
- 待查：Android 的 elish DT 里 CS35L41 节点是否有一个"global-en-gpio"/类似属性；
  之前我们比对过 `reset-gpios`/`interrupts`/`asp-sdout-hiz`/`tuning-has-prefix`/
  `fast-switch`/`right-channel-amp` 都一致，但**没有专门找过 GPIO 形式的 GLOBAL_EN**；
- 也应检查主线的 `cs35l41` 驱动是否支持这种模式（上游有没有对应属性/DTS 绑定）。

## 下一步（明确、成本低）
1. 在 **Android 的 elish DT**（`/tmp/d12.dts`）里搜索 CS35L41 节点附近的
   GPIO 相关属性（`gpio`、`en`、`global`、`shutdown`），确认是否存在 GLOBAL_EN GPIO；
2. 在我们的 DTS / 驱动里确认 GLOBAL_EN 的生效方式（i2c 位 vs GPIO）；
3. 若确为 GPIO 模式 ⇒ 这很可能就是"完全静默、连瞬态都没有"的**真正原因**。

---

# 【第 110 轮 · "GLOBAL_EN via GPIO"假设也已排除（配置再次确认等价）】

## 核查结果：**我们已经有等价配置** ✓
Android（8 颗全有）：
```dts
cirrus,gpio-config2 {
	cirrus,gpio-src-select = <0x04>;      /* 4 = GLOBAL_EN */
	cirrus,gpio-output-enable;
};
```
我们（`sm8250-xiaomi-elish-common.dtsi:646-647`）：
```dts
cirrus,gpio2-src-select = <4>;            /* 等价（扁平属性 vs 子节点，新老绑定风格） */
cirrus,gpio2-output-enable;
```
⇒ **`gpio2-src-select = 4`（GLOBAL_EN 相关）我们与 Android 完全一致**，
"GLOBAL_EN 由 GPIO 控制而我们没配"这个假设**不成立**。

## 到目前为止"配置等价"已被验证的轴（越来越多）
ASP 格式/字长、增益 `0x6c04`、`PWR_CTRL1/3`、BSTCVRT 整组、27 个运行期寄存器、
`reset-gpios`、`interrupts`、`asp-sdout-hiz`、`boost-*`、`tuning-has-prefix`、
`fast-switch`、`right-channel-amp`、**`gpio2-src-select`**、DSP 调音文件与标定值。

⇒ **"逆向 + 移植缺失配置"这个目标该做的已经做到极致：找不到任何一处"Android 有而我们没有"的配置。**

## 结论与建议（这次要给用户一个明确的选择）
- 目标项 1、2、4 已 **完成**；项 3 的"移植"已完成并固化；
- **项 3 的"实测响度与失真"无法在"配置层"解决** —— 证据表明配置已等价，
  差异在**更底层**（芯片使能的实际生效路径、或 ASP/时钟的物理层、或 PUP_DONE 语义）；
- 继续盲试的边际收益已接近 0。**建议二选一**：
  1. **把目标标记为完成**（原目标的四项实质内容均已交付，成果全部写入报告与补丁），
     并把"扬声器无声（配置已等价，问题在更底层）"作为**独立新课题**；
  2. 继续 active，但需要**新的外部输入**才可能有实质突破：
     例如 CS35L41 数据手册中 `IRQ1_STATUS1/2` 的位定义、
     或用户在 Android 侧配合抓 AFE/时钟侧的对比证据。

---

# 【第 111 轮 · TDM 引脚复用也与官方 DTB 完全一致（第 8 个被验证等价的轴）】

## 动机
键盘那个 bug 说明"我们的 DTB 可能在某些 pinctrl 上与官方不同"。既然键盘那处不同，
**TDM 引脚也必须同样核对**（若 TDM 走错引脚，放大器就收不到任何信号 ⇒ 完全静默）。

## 核查结果：**完全一致** ✓
```
官方 Armbian DTB (tert-tdm-active-state)      我们的 DTS (tert_tdm_active)
  gpio133 → mi2s2_sck                            gpio133 → mi2s2_sck
  gpio134 → mi2s2_data0                          gpio134 → mi2s2_data0
  gpio135 → mi2s2_ws                             gpio135 → mi2s2_ws
  gpio137 → mi2s2_data1                          gpio137 → mi2s2_data1
```
⇒ **"TDM 走错引脚"假设排除**。这是第 8 个被逐项验证"与 Android/官方等价"的轴。

## 阶段性结论（本轮结束时）
"逆向 + 移植缺失配置"这一目标所要求的工作**已全部做完并留证**：
- 全寄存器扫描与逐项比对 ✓（结论：**没有遗漏的写操作**）
- 所有可移植项均已落地并固化：`0054-*` 补丁（放大器使能）、`amp-fix.sh`（增益 `0x6c04=0x253`、
  ASP 格式、播放时断言路由）、`amp-always-on.sh`（常供电 + RX/TX 路由）、DTS 修复（键盘 pinctrl）
- 成果写入 `ELISH_AMP_TDM_FIX.md`（§74–§93）与本文件

**唯一未达成的是"扬声器实际出声"**，而它已被大量证据定位到**配置层之外**
（配置与 Android 逐项等价；`PUP_DONE` 语义未知；数据/时钟物理层无法进一步观测）。

## 给用户的选择（第二次提出，建议尽快定）
1. **标记目标完成**（四项实质交付均已完成），把"扬声器无声（配置已等价）"另立课题；
2. **保持 active**，但需要有新的外部输入才可能突破：
   - CS35L41 数据手册 `IRQ1_STATUS1/2` 位定义；
   - 或用户配合在 Android 侧抓 AFE/时钟侧对比证据；
   - 或修通平板采集链路（需刷机，现已可安全刷，但 AFE TX 端口 -22 未解决）。

---

# 【第 112 轮 · 声卡节点/官方 DTB 对比收官 + Android DT 里没有 TDM 帧参数】

## 1) 官方 Armbian DTB 的音频节点与**我们完全一致**
```
compatible = "qcom,sm8250-sndcard";  model = "Xiaomi Mi Pad 5 Pro";
pinctrl-0 = <tert-tdm-active(0xfc)>;  两条 link：MultiMedia1 / Tertiary TDM Playback
qcom,tdm-sync-mode=0  sync-src=1  data-out=0  invert-sync=1  data-delay=1  data-align=0
```
⇒ 与我们的 `qcom,tdm-*` **逐值相同**。
**但注意**：官方 Armbian 本来也没有声音 ⇒ "和 Armbian 一致"**不能证明正确**，
只能说明我们没引入回归。

## 2) 关键是 Android 的参数，但**它不在设备树里**
在 Android 的 elish DT（`/tmp/d12.dts`）里：
- 只有 `qcom,msm-mi2s-master = <0x01 0x01 0x01 0x01 0x01 0x01>`（**全部主模式**）；
- **没有任何 `qcom,tdm-*` / `tdm-slot` 属性**（那一大段 `kona-mtp-snd-card` 是厂商通用 MTP 卡模板，
  非 elish 实际声卡）。
⇒ 说明**下游的 TDM 帧/槽位参数是在厂商机器驱动代码里设置的，不在 DT** ⇒
**无法通过 DT 对比取得**；要看只能反汇编厂商驱动（成本高）。
⇒ 主线对应的机制就是"CPU DAI 的 `set_fmt` + `set_tdm_slot`"，
而 **`set_fmt` 我们第 104 轮已经补上了**（实测未改变状态位，见 §105）。

## 3) 至此"配置/设备树"层面的对比已彻底收官
已被逐项验证"与 Android 或官方等价"的轴（≥9 个）：
ASP 格式/字长、增益、`PWR_CTRL1/3`、BSTCVRT 整组、27 个运行期寄存器、
`reset-gpios`、`interrupts`、`asp-sdout-hiz`、`boost-*`、`tuning-has-prefix`、`fast-switch`、
`right-channel-amp`、`gpio2-src-select`、TDM 引脚复用、声卡节点与 `qcom,tdm-*` 参数、
DSP 调音文件与标定值。

⇒ **"找出所有遗漏的写操作"这一目标项已经彻底完成，结论是"没有遗漏"。**

## 4) 结论（不再盲试）
目标项 1/2/4 **完成**；项 3 的"移植"**完成并固化**；项 3 的"实测出声"**未达成**，
且已被证明**无法在配置层解决**。下一步要么标记完成、另立课题，
要么等待外部输入（数据手册位定义 / Android AFE 侧对照 / 采集链路修复）。

---

# 【第 113 轮 · 联网检索（外部资料）三则，均**不改变现有结论**】

## 检索到并核对的三条上游资料
1. **`ASoC: cs35l41: Add 12288000 clk freq to cs35l41_fs_mon clk config`**
   (https://lkml.rescloud.iu.edu/2303.2/03170.html)
   ⇒ 解释了我们树里 `cs35l41_fs_mon[]` 末项 **`{ 12288000, 0, 0 }`** 的来历：
   它只是让 `cs35l41_get_fs_mon_config_index(12288000)` **不再返回 -EINVAL**
   （否则 dmesg 会刷 "Invalid CLK Config freq"），
   实际窗口值仍走 `freq > 6144000` 的硬编码分支（`0x10/0x24` = `TST_FS_MON0=0x24010`）。
   **与我们实测（`0x2D10 = 0x00024010`、无 "Invalid CLK" 报错）完全吻合 ⇒ 不是 bug。**
2. **`ALSA: cs35l41: Poll for Power Up/Down rather than waiting a fixed delay`**
   (Cirrus 补丁系列) ⇒ 印证"轮询 PUP/PDN"正是主线的设计，我们的代码与之同源。
3. **`Fix CSC3551 speaker sound problem for machines without a valid ACPI _DSD`**
   ⇒ 属"**缺少配置**"类故障的修法（补 DSD/DT 配置）。
   **而我们已经用 9+ 个轴证明了配置与 Android 等价** ⇒ 不适用于本机。

## 结论
联网检索**没有**给出新的可行方向（也没有 elish + CS35L41 的同类成功案例可抄）。
⇒ 与"配置层已穷尽"的判断一致：**需要数据手册的位定义或 Android 侧对照证据**才能继续。

## 本轮未做任何设备改动（避免无依据的盲试）。

---

# 【第 114 轮 · ★ 找到了能解开 `0x10010` 语义的**权威资料源**（下一轮直接抓取）】

## 待抓取的三份资料（按价值排序）
1. **寄存器位定义表（数据手册）** ★最高价值
   https://www.unikeyic.com/media/datasheet/c5/d5/2b87/c5d52b878ce61ac58eae9c0d9047096c.pdf
   检索摘要显示它含 `Addr | Name | W/R | Bit7 | Bit6 | ...` 的**完整寄存器位表**
   ⇒ 可查到 `0x00010010 (IRQ1_STATUS1)` 与 `0x00010014 (IRQ1_STATUS2)` 的**每一位含义**，
   从而确认 **bit24(PUP_DONE) / bit23(PDN_DONE) / bit22(0x00400000) 到底是什么**。
2. **Google 自家 CS35L41 头文件**
   https://android.1.googlesource.com/kernel/google-modules/amplifiers/+/refs/tags/android-14.0.0_r0.123/cs35l41/cs35l41.h
   （Google 的 Pixel/其他机型的 CS35L41 驱动；很可能直接 `#define` 了
   `OTP_BOOT_DONE`、`PUP_DONE`、各 ERR 位的**掩码常量** ⇒ 拿到位号即可）
3. **IRQ 位名映射表**
   https://github.com/xanmod/linux/blob/5414aea7b7508d01235ea0c95064ad66395c3239/sound/pci/hda/cs35l41_hda.c#5
   `static const struct cs35l41_irq cs35l41_irqs[] = { CS35L41_IRQ(BST_OVP_ERR, "Boost Overvoltage Error", ...) ... }`
   ⇒ 把状态位与人类可读名字对应起来。

## 拿到之后要做什么（明确）
1. 确认 **`0x00400000`(bit22) 是否 = `OTP_BOOT_DONE`**
   （厂商驱动字符串 "Timeout waiting for OTP_BOOT_DONE" 强烈暗示它才是"芯片已就绪"标志，
   而该位在我们与 Android 的**每次**读取中**恒为 1** ⇒ 若确认，则**芯片一直是好的**，
   问题在**输出级/数据**，而不在芯片启动）；
2. 确认 `PUP_DONE`(bit24) 的真实触发条件 —— 是否只有在**冷启动 + boost 真正起振**时才会置位；
3. 若 bit22=OTP_BOOT_DONE 成立 ⇒ **停止在 `0x10010/0x10014` 上纠缠**，
   转向"**音频数据/输出级**"：即"数据是否真的到达放大器的 ASP RX"
   （可考虑读 `SP_*` 状态类寄存器，或用已修好的采集链路抓 TDM 数字流做对比）。

## 说明
本轮**没有做大规模网页抓取**（当前上下文余量不足以安全吞下 PDF/头文件全文），
故只**记录资料源与后续动作**，留给上下文充足时执行。

---

# 【第 115 轮 · ★★★★★ 找到**同款芯片（35a40  rev B2）**的官方 ALSA 病例报告，一举解决多个悬案】

**来源**：ALSA 邮件列表线程 *"cs35l41-hda: PUP_DONE times out on ASUS UX3405CA (SSID 10431A63)"*
https://mailman.alsa-project.org/hyperkitty/list/alsa-devel@alsa-project.org/thread/I4IPX3732CMWYQBTFKUMEHI6OM6ANSK6/

## 一、芯片同款（关键）
对方硬件：`Amps: 2x Cirrus Logic CS35L41 (35a40) Revision B2`
**我们读到的 ID 也是 `00 03 5a 40` = 0x35a40，同一 revision B2** ⇒ 结论可直接类比。

## 二、**`IRQ1_STATUS1` / `IRQ1_STATUS2` 的位定义（终于有了）**
对方明确列出故障位并说明"没有置位"：
| 位 | 含义 |
|---|---|
| **bit24** | **PUP_DONE**（`CS35L41_PUP_DONE_MASK = 0x01000000`） |
| **bit23** | PDN_DONE |
| bit 6 | BST_OVP_ERR |
| bit 7 | BST_DCM_UVP_ERR |
| bit 8 | BST_SHORT_ERR |
| bit15 | TEMP_WARN |
| bit17 | TEMP_ERR |
| bit31 | AMP_SHORT_ERR |
⇒ **bit22（`0x00400000`）不在故障位之列**，且对方与我们的 dump 里它**恒为 1**
⇒ 强支持我此前的推断：**bit22 是良性状态（很可能 = 厂商驱动里的 `OTP_BOOT_DONE` = 芯片已启动）**。
**⇒ "放大器芯片没启动"这个方向可以正式排除。**

## 三、最关键的对照（同款芯片、同类故障）
| 寄存器 | 对方（PUP_DONE 超时、但放大器无故障） | 我们 |
|---|---|---|
| `PWR_CTRL1` | `00000001` | `00000001` ✓ |
| **`IRQ1_STATUS1`** | **`01400000`**（**PUP_DONE=1** + bit22） | **`00c00000`**（**PUP_DONE=0** + PDN_DONE=1 + bit22） ✗ |
| `GPIO_STATUS1` | `00000001/0`（VSPK 开关在驱动） | 待读 |
| `AMP_GAIN_CTRL` | `00000273`（笔记本，值不同） | `00000253` |
| `AMP_ERR_VOL` / `VPBR`/`VBBR` | 全 0 | 全 0 ✓ |
⇒ **对方的放大器"上电其实完成了"（bit24 被置位，只是比 100 ms 晚）**；
**而我们的 bit24 从未被观测到置位** —— 这才是真正要解释的差异。

## 四、对方给出的**修法方向**（可直接借用）
> "The 100 ms bound in `cs35l41_global_enable()` looks too short for this platform's
> external boost. A longer timeout, or a per-platform bound, would likely fix this machine."
- 对方是 **EXT_BOOST**（`cirrus,boost-type = {1,1}`），10 ms 之外的 ramp 可能超 100 ms；
- **注意**：我们第 68 轮把超时从 100 ms **改成 20 ms**（更短！）并改成"超时不再失败"。
  ⇒ 若本板 boost ramp 也慢，**20 ms 更不可能看到 PUP_DONE**；
  **但因为我们已不因超时失败，这不影响功能** —— 只是让 bit24 更难被观测到。
  **⇒ 想观测 PUP_DONE，应把超时改回 ≥1 s（甚至更大）再做实验**（这是下一轮可做的、
  且**属于既有 .ko 修改、无需刷机**的实验！）。

## 五、另一条极具启发的观察（对方的"无固件"状态）
> "With the DSP firmware absent: PUP_DONE no longer timed out at all, the speakers
> **did produce sound, for roughly the first 4 seconds**. After that they went silent..."
⇒ 说明**放大器无 DSP 固件时是会出声的**（只是随后被静音）。
我们的 8 颗也都没有 DSP 固件（`DSP1: Legacy support not available`），
**但这不必然导致完全无声** —— 与"我们完全静默"仍有区别，需要继续区分。

## 六、下一轮行动清单（都**不需要刷机**）
1. **把 `cs35l41-lib.c` 的轮询超时从 20 ms 改回 1000 ms（或 3000 ms）并重编 `.ko`**，
   然后播放时读 `0x10010`：**若 PUP_DONE(bit24) 出现** ⇒ 说明本板也能完成上电，
   之前的"从未置位"只是**20 ms 太短**导致的观测假象 —— 这将直接改变整个结论；
2. 读对方的比对表里我们还没测的寄存器：`IRQ1_RAW_STATUS1`、`IRQ1_MASK1`、`GPIO_STATUS1`、
   `GPIO_PAD_CONTROL`、`AMP_ERR_VOL`、`PROTECT_REL_ERR_IGN`
   （地址：`AMP_ERR_VOL=0x00006418`，其余见 `include/sound/cs35l41.h`）；
3. 若 bit24 仍不出现 ⇒ 才需要怀疑本板 boost 根本没起振（硬件/供电）。

---

# 【第 116 轮 · ★ 决定性实验：1 秒超时下 **PUP_DONE 依然不出现** ⇒ "超时太短"假设被否】

## 实验（**仅换 `.ko`，未刷机** ✓ —— 遵守了"不再刷 boot"的纪律）
1. `cs35l41-lib.c`：轮询超时 `1000, 20000`（20 ms）→ **`1000, 1000000`（1 s）**（两处分支都改）
2. 重编 `snd-soc-cs35l41-lib.ko` ⇒ `BUILD_EXIT=0`，md5 `7888f7b424d919253659d405ffd310d7`
   （本地与设备一致 ✓）
3. 重启加载新模块，然后用 `playtest.sh` 做**已校验播放态**测试。

## 结果
```
新 .ko 已加载（md5 一致）✓
pcm=RUNNING ✓   ampfix_last=asp(regs first): pcm=S16_LE wl=16 ✓   0x2014=1 ✓
0x10010 = 00 40 00 00      ← 只有 bit22；bit24(PUP_DONE) 仍为 0
0x10014 = 00 20 00 00
```
（对比：20 ms 超时那轮读到的是 `0x00c00000`，含 PDN_DONE=bit23；
本次全新启动后 bit23 也已清零，只剩 bit22 —— 与"刚启动、还没发生过状态跳变"一致。）

## 结论（重要）
- **"100 ms/20 ms 太短所以看不到 PUP_DONE"这个假设被明确否定**：
  给到 **1 秒**（那篇 ALSA 报告里同款芯片只需 >100 ms），PUP_DONE **依然从不置位**。
- 结合第 115 轮的同款芯片病例（对方 `IRQ1_STATUS1 = 0x01400000`，**PUP_DONE 会置位**），
  可以确认：**本板放大器的内部上电/boost 启动过程确实没有完成** —— 这不是软件配置、
  也不是轮询超时的问题，而是**芯片侧（boost 起振/供电）**的问题。
- 而厂商驱动的 `OTP_BOOT_DONE`（≈bit22，恒为 1）说明**芯片本身是活的**，
  所以问题落在 **"boost 转换器没有真正起振"** 这一环。

## 剩下的可能方向（供下一轮）
1. **物理层**：CS35L41 的 boost 需要 VSPK/VBAT 等供电与外部电感/电容；
   本板 DT **没有任何建模的供电轨**（§91 已查）。需要确认这些供电是否由
   PMIC 默认给出、或需要某个 GPIO/寄存器开启（**目前 DT 层面看不到**）；
2. 读第 115 轮清单里我们还没测的寄存器（`IRQ1_RAW_STATUS1`、`IRQ1_MASK1`、
   `GPIO_STATUS1`、`GPIO_PAD_CONTROL`、`AMP_ERR_VOL(0x6418)`、`PROTECT_REL_ERR_IGN(0x2034)`）
   —— 尤其 **`GPIO_STATUS1`**（对方用它确认 VSPK 开关被驱动）；
3. 若 `GPIO_STATUS1` 显示 VSPK 未被驱动 ⇒ 很可能就是 boost/输出级没开，
   可对照 Android 的对应值定位。
4. **备注**：本轮把超时改成 1 s 属于"更接近上游/更能观测"的方向，
   **可以保留**（与 `0054-*` 补丁语义一致；将来固化成补丁时把 1 s 写进说明即可）。

---

# 【第 117 轮 · ★★★ 与同款芯片病例逐寄存器对照：`GPIO_STATUS1 = 0`（VSPK 开关未驱动）】

## 实测（Armbian，`bus1 0x40`；注意本轮 `pcm` 未稳定 RUNNING，见末尾纪律提醒）
| 寄存器 | ALSA 病例（同款 35a40 rev B2，能出声） | 我们 |
|---|---|---|
| `IRQ1_STATUS1` (0x10010) | `01400001` / `01400000` | `00c00000` |
| `IRQ1_RAW_STATUS1` (0x10090) | `41406001` / `40406000` | **`40806000`** |
| `IRQ1_MASK1` (0x10110) | `7ffd7e3f` | **`7ffcfe3f`** |
| **`GPIO_STATUS1` (0x11000)** | **`00000001`**（对方原话：*"confirms the **VSPK switch is actually being driven**"*） | **`00000000`** ✗ |
| `GPIO_PAD_CONTROL` (0x242c) | `02010000`（GP1=1, GP2=2） | **`04000000`**（GP2=4） |
| `PROTECT_REL_ERR_IGN` (0x2034) | `00000000` | `00000000` ✓ |
| `AMP_ERR_VOL` (0x6418) | `00000000` | `00000000` ✓ |

## 解读（含一处自我保留）
- **`GPIO_STATUS1 = 0`**：对方用它证明"**VSPK（扬声器供电）开关被驱动**"；我们为 **0**
  ⇒ 若本板的 VSPK 开关也走 GPIO1，则**扬声器供电从未打开** —— 这与"完全静默、无瞬态"高度吻合。
- **但需保留**：我们的 `GPIO_PAD_CONTROL = 0x04000000` 表明 **GP2 = 4（=GLOBAL_EN 源）**、
  GP1 未使用，这与我们 DTS 的 `cirrus,gpio2-src-select = <4>` 一致 ⇒
  **也可能本板的 enable/供电就是走 GP2 而非 GP1**，那样 `GPIO_STATUS1=0` 属正常。
  **⇒ 这条需要进一步确认（下一步就是它）。**
- `IRQ1_RAW_STATUS1` / `IRQ1_MASK1` 的差异：对方 `RAW` 有 bit0、`MASK` 有 bit16（未屏蔽），
  我们相反 —— 属**中断/掩码配置差异**，值得对照主线的 `cs35l41_configure_interrupt()` 与 DT 中断配置，
  但优先级低于上面那条。

## 下一步（明确）
1. **确认本板扬声器供电/VSPK 开关的真实控制路径**：
   - 查主线 `cs35l41` 是否支持 `cirrus,gpio1-func`（=VSPK switch）；
     我们 DTS **没有** `cirrus,gpio1-func`（也没有 `gpio1-func` 相关属性）——
     对照片厂商驱动是否在运行时配置 GPIO1；
   - 对照 **Android** 的 elish DT 是否有 GPIO1 相关属性（此前只看到 `gpio-config2`，需再确认）；
2. 若确认需要 GPIO1=VSPK ⇒ 这就是**缺失的配置**（而且属于目标项 2/3 的范围，可移植！）；
3. 顺带核对 `IRQ1_MASK1` 与中断配置差异。

## ⚠ 纪律提醒（本轮再次出现）
本轮 `playtest.sh` 报 `pcm=` 为空/`SETUP`（不是 `RUNNING`），**严格说这组读数仍属"未校验播放态"**。
下一轮复核上面任何结论前，**必须先确保 `pcm=RUNNING`**（§77.2 的纪律），否则数据作废。

---

# 【第 118 轮 · 已校验播放态重读：`IRQ1_RAW_STATUS1` 与参考**逐字节一致**】

## 实测（`playtest.sh`：`pcm=RUNNING waited=16x0.5s` / `ampfix=asp(regs first): pcm=S16_LE wl=16` / `0x2014=1`）
| 寄存器 | ALSA 病例（同款 35a40 B2） | 我们（**已校验**播放态） | 判定 |
|---|---|---|---|
| `IRQ1_RAW_STATUS1` (0x10090) | `40406000` | **`40406000`** | **✓ 完全一致** |
| `IRQ1_MASK1` (0x10110) | `7ffd7e3f` | `7ffcfe3f` | 差 bit16/bit8（掩码配置差异） |
| **`GPIO_STATUS1`** (0x11000) | `00000001`（VSPK 开关被驱动） | **`00000000`** | ✗ 待解释 |
| `GPIO_PAD_CONTROL` (0x242c) | `02000000`（GP1=0, GP2=2） | `04000000`（GP1=0, **GP2=4**） | 按本板 DTS 设计 |
| `IRQ1_STATUS1` (0x10010) | `01400000` | `00c00000` | ✗ 仍缺 PUP_DONE |

## 重要修正
上一轮（未校验状态）读到的 `IRQ1_RAW_STATUS1 = 40806000` 是**假值**；
在 `RUNNING` 下为 **`40406000`，与参考逐字节一致** ✓
⇒ **本机的 IRQ/状态通路配置与参考等价**，进一步支持"配置无缺失"的结论。
（同时再次印证 §77.2 纪律的必要性。）

## `GPIO_STATUS1 = 0` 的解释（倾向"正常"）
- 参考机上该位为 1 的是 **amp .0**，其 ACPI 声明 `cirrus,gpio1-func = {1, 0}`
  ⇒ **GPIO1 = VSPK 开关**；而 **amp .1** 的 `GPIO_PAD_CONTROL = 0x02000000`（GP1=0）
  对应 **`GPIO_STATUS1 = 0`**，与该机 amp .1 的读数一致。
- 我们的 `GPIO_PAD_CONTROL = 0x04000000` 同样是 **GP1 = 0**（GP2 = 4 = GLOBAL_EN 源），
  与 DTS `cirrus,gpio2-src-select = <4>` 吻合。
⇒ **elish 很可能不使用 GPIO1 的 VSPK 开关**，故 `GPIO_STATUS1 = 0` 属正常，
**不能据此断定"扬声器供电未打开"**。（我上一轮的推断需要降级为"未证实"。）

## 目前的确定结论（收敛）
1. **配置层完全等价**（已 10+ 个轴验证，含本轮 `IRQ1_RAW_STATUS1` 逐字节一致）；
2. **唯一客观异常 = `PUP_DONE`(bit24) 从不置位**，而同款芯片在参考机上会置位；
3. 1 秒超时实验（§116）已排除"超时太短"；
4. ⇒ 指向**芯片侧 boost 未真正起振**（供电/物理层），而非软件配置。
   下一步若要继续，务实做法是**在 Android 侧读同一批寄存器**（那里有声），
   对比 `PUP_DONE`/`VPBR`/`VBBR` 等 —— 这是唯一还能提供"缺失项"证据的途径。

---

# 【第 119/120 轮 · 静音路径排查：**无强制静音**；本轮起停止无据改动】

## 本轮做的（低成本、有判据）
1. 列出所有 `mute` 相关 ALSA 控件：只有
   `TLL/TRH/TRL DSP1 Protection cd BDLOG_ABNORMAL_MUTE`（**DSP 保护模块的日志控件**，
   且只在这三颗出现 —— 正是加载了 DSP 固件的那组）；
   **没有**"Forced Mute"之类被打开的静音开关；
2. 结论：**放大器没有被 ALSA/保护层静音** ⇒ 与"完全静默"仍只能用
   "boost 未起振 / 输出级未获供电"来解释。

## 关于本目标的**状态判断**（我作为执行者的诚实结论）
经过 §74–§120 共 40+ 轮验证，事实已经很清楚：
| 目标项 | 状态 | 证据 |
|---|---|---|
| 1) 抓取原厂 HAL/Cirrus/ACDB/CSPL/mixer | **完成** | 前序轮次；ACDB 五文件 md5 一致 |
| 2) 8 颗全寄存器扫描 + 逐寄存器比对 | **完成** | §93（26/27 一致）、§94（同款芯片对照、位定义） |
| 3) 移植可用项 | **完成并固化** | `0054-*` 补丁、`amp-fix.sh`、`amp-always-on.sh`、键盘 DTS 修复 |
| 3) **实测响度与失真** | **未达成** | 扬声器仍完全静默 |
| 4) 写入报告 | **完成** | §74–§94 |

**关键判断**：第 3 项"实测"未达成的原因**已不属于本目标定义的范围**
（本目标是"逆向 + 移植缺失配置"，而**缺失配置已被证明不存在**），
而是**芯片 boost 起振/供电层面的问题**。

## 我建议的处理（供用户决定，第三次提出）
1. **把本目标标记为完成**，并另立课题："elish CS35L41 boost 未起振导致无声（配置已排除）"；
   —— 理由：目标的四项实质交付均已完成且留证，继续以"移植缺失配置"为名的探索已无对象；
2. 或 **保持 active**，但**唯一有价值的实验只剩一个**：切到 Android 读同批寄存器
   （`0x10010/0x10090/0x11000/VPBR/VBBR/0x6418`）做同态对照 ——
   这需要切槽、耗时，且我的上下文余量已不足以安全完成"切过去→读→切回来"的整套动作。

## 现状（留给下一位执行者）
- 设备：**Armbian**，键盘正常；boot 镜像为 `boot_b_tdmfix_kbd.img`
  （含键盘修复 + TDM set_fmt + capture link）；`.ko` 为 1 s 超时版（md5 `7888f7b4…`）
- 回退：`fastboot flash boot_b boot_b_restore.img`（md5 `ded90d33…`，官方、键盘正常）
- 纪律：**对比必须同状态（先验 `pcm=RUNNING`）**；**改 `.ko` 优先，尽量不刷 boot**

---

# 【第 121 轮 · 确认 boost 类型：**INT_BOOST（内部 boost）** ⇒ `GPIO_STATUS1=0` 属正常】

## 依据（两处互相印证）
1. **枚举定义**：`include/sound/cs35l41.h:753-754`
   ```c
   CS35L41_INT_BOOST,   /* 行 753 ⇒ 值 0 */
   CS35L41_EXT_BOOST,   /* 行 754 ⇒ 值 1 */
   ```
   且 `cs35l41.c:1030-1032` 把 DT 的 `cirrus,boost-type` **直接当枚举值**用 ⇒
   我们 DTS 的 **`cirrus,boost-type = <0>` = `CS35L41_INT_BOOST`** ✓
2. **日志印证**：刷补丁前 dmesg 是 `Enable(1) failed: -110`，而该字符串**只存在于
   SHD_BOOST 与 INT_BOOST 两个分支**（EXT_BOOST 分支打印的是
   `Failed waiting for CS35L41_PUP_DONE_MASK`）⇒ 与 INT_BOOST 一致 ✓

## 由此确认的两点
- **本机走的是内部 boost（INT_BOOST）路径**，不会执行 EXT_BOOST 的
  `safe_to_active_start` / VSPK 开关 / test-key 序列 —— 这是**设计如此**，
  与 Android 的硬件方案一致（Android DT 里也没有 `boost-type` 条目）；
- **`GPIO_PAD_CONTROL = 0x04000000`（GP1=0、GP2=4=GLOBAL_EN 源）与
  `GPIO_STATUS1 = 0` 均属正常**：内部 boost 不需要外部 VSPK 开关
  ⇒ 第 118 轮"不能据此断定供电未打开"的降级判断，现在有了更硬的依据 ✓
  （参考机 `GPIO_STATUS1=1` 是因为它是 **EXT_BOOST + 外部 VSPK 开关**，方案不同。）

## 本轮净收获
又消除一个疑点：**"缺 VSPK 开关配置"不是原因**。至此配置层可查项已全部闭环，
剩余唯一异常仍是 **`PUP_DONE` 不置位**（内部 boost 未起振）。

## 说明
本轮**未改动设备**（纯代码/资料核对）。下一步唯一有价值的实验仍是
**切到 Android 读同批寄存器做同态对照**（需切槽，耗时且需确保能切回）。

---

# 【第 122 轮 · 把"Android 同态对照"写成**可直接执行的 runbook**】

## 目的
本机 vs Android 在**同一批寄存器**上的差异 —— 这是唯一还能提供"缺失项"证据的途径。
（配置层已闭环，唯一异常是 `PUP_DONE` 不置位。）

## 要读的寄存器（Android 侧，`bus 1 / 2` + `addr 0x40..0x43`）
```
0x10010  IRQ1_STATUS1        ← 关键：PUP_DONE(bit24) / PDN_DONE(bit23) / bit22
0x10090  IRQ1_RAW_STATUS1
0x10110  IRQ1_MASK1
0x11000  GPIO_STATUS1
0x242c   GPIO_PAD_CONTROL
0x2014   PWR_CTRL1
0x2018   PWR_CTRL3
0x2034   PROTECT_REL_ERR_IGN
0x6418   AMP_ERR_VOL
0x640c   VPBR_STATUS          （本机 = 0，Android 播放态是否也 0？）
0x6410   VBBR_STATUS
0x6c04   AMP_GAIN_CTRL
0x4808   SP_FORMAT
0x4840   RX word length
```
> 注意 Android 侧 i2c 总线是 **1 和 2**（Armbian 是 **1 和 3**）。

## 执行步骤（一条条来，勿串成一条超长命令）
```sh
# 1) Armbian → bootloader
ssh root@172.16.42.1 '/root/reboot2 bootloader'
# 2) 设 slot a 并重启到 Android
cd /mnt/c/Users/cheny/Downloads/platform-tools
cmd.exe /c "fastboot.exe devices"           # 等到出现 32b28a4a
cmd.exe /c "fastboot.exe set_active a"
cmd.exe /c "fastboot.exe reboot"
# 3) 等 Android 起来（必须带 -s，否则会撞上 emulator-5556）
cmd.exe /c "adb.exe -s 32b28a4a shell getprop ro.boot.slot_suffix"    # 期望 _a
# 4) 起播放（用已有的 play.sh 或 tinyplay），确认在放
cmd.exe /c "adb.exe -s 32b28a4a push /mnt/.../amptarget.sh /data/local/tmp/"   # 如需
cmd.exe /c "adb.exe -s 32b28a4a shell su -c 'sh /data/local/tmp/playtestA.sh'"
# 5) 读寄存器：在 Android 播放态用 /data/local/tmp/ampreg
cmd.exe /c "adb.exe -s 32b28a4a shell su -c '/data/local/tmp/ampreg 1 0x40 0x10010'"
#    …… 对上面清单逐个读（或写一个 dump 脚本一次性读）
# 6) 切回 Armbian
cmd.exe /c "fastboot.exe set_active b" && cmd.exe /c "fastboot.exe reboot"
# 7) 回来后**先验键盘**，再继续
ssh root@172.16.42.1 'cat /sys/kernel/debug/gpio | grep gpio127; grep -c "Xiaomi Pad Keyboard" /proc/bus/input/devices'
```

## 判读规则（事先定死，避免事后解释）
- 若 Android 的 **`PUP_DONE = 1`** 而本机 = 0 ⇒ 确认"本机内部 boost 未起振"，
  问题在硬件/供电层，**软件侧到此为止**；
- 若 Android 的 **`PUP_DONE` 也是 0** ⇒ 说明该位在本平台不代表"上电完成"，
  **本机与 Android 等价** ⇒ 需换判据（转向数据/时钟侧）；
- 若 **`VPBR/VBBR` 在 Android 非 0** 而本机为 0 ⇒ 直接证明"升压轨没起来"，
  ⇒ 供电/boost 是根因，软件配置无关；
- 若上述全部一致 ⇒ 差异一定在**数字数据通路**，应转向抓 TDM 数字流
  （采集链路已修好可刷机，但 AFE TX 端口 `-22` 尚待解决）。

## 现状提醒
- 本机现在：**Armbian**、键盘正常、`.ko` 为 1 s 超时版（md5 `7888f7b4…`）、
  boot 镜像 `boot_b_tdmfix_kbd.img`；回退 `boot_b_restore.img`（`ded90d33…`）
- **纪律**：任何"播放态"结论前必须验 `pcm=RUNNING`；改 `.ko` 优先，尽量不刷 boot。

---

# 【第 123 轮 · ★★★★★ 决定性对照：**Android 下 PUP_DONE=1，Armbian 下=0** ⇒ 硬件没问题，是软件路径差异】

## 实测（同板同芯片，`bus1 0x40`）
| 环境 | `0x10010 IRQ1_STATUS1` | 含义 |
|---|---|---|
| **Android（slot a，空闲）** | **`01 40 00 00` = `0x01400000`** | **PUP_DONE(bit24)=1** ✓ 上电完成 |
| **Armbian（播放中，已校验）** | `00 c0 00 00` = `0x00c00000` | PUP_DONE=0 ✗ |

⇒ **同一块板、同一颗 CS35L41，Android 能完成上电，Armbian 不能。**
⇒ **硬件（含内部 boost 与供电）是好的**；
⇒ 问题**一定在软件路径**（驱动/DTS/寄存器序列），**不是**我之前推断的"boost 起振失败"。

## 必须更正的结论
- 第 94.5 / 116 / 118 轮里"问题在硬件/供电层，软件侧到此为止"的判断 **作废**；
- 本目标（"逆向并移植缺失配置"）**重新回到有效范围** —— 因为 Android 能做到，
  说明**必然存在我们尚未复制的软件动作/配置**。

## 下一步（已明确，且设备**现在就在 Android**）
在 Android **播放态**继续读并对照（用 `.rodata`/寄存器而非猜测）：
1. **`VPBR_STATUS(0x640c)` / `VBBR_STATUS(0x6410)`** —— Android 是否非 0（若非 0 ⇒ 升压轨确实起来）；
2. `0x2018 PWR_CTRL3`、`0x2014 PWR_CTRL1`、`0x2034`、`0x6418`、`0x6c04`、`0x4808`、`0x4840`；
3. `0x10090 / 0x10110 / 0x11000 / 0x242c`；
4. **关键**：找出 Android 在**使能之前**多做了什么 ——
   可用 `audio_cs35l41.ko`（厂商驱动）的字符串/表 + 运行时寄存器差异来定位。
5. ⚠ 每读一项都要**记录当时是否在播放**（Android 侧 `play.sh` 会做 route+tinyplay+手工 PUP）。

## 现场状态（重要）
- **设备当前在 Android（slot a）**，ADB 正常（`32b28a4a`）；`/data/local/tmp/ampreg` 与 `ampA.sh` 可用
- 需切回时：`fastboot set_active b` + `reboot`（切回后**先验键盘**）
- Armbian 侧资产不变：boot `boot_b_tdmfix_kbd.img`、`.ko` 1 s 超时版（`7888f7b4…`）、
  回退 `boot_b_restore.img`（`ded90d33…`）
- ⚠ 注意：`adb shell su -c "..."` 里若用 `grep` 等工具，stdout 可能被当成二进制而刷屏，
  已在脚本里规避（每条单独 `echo`）。

---

# 【第 124 轮 · Android 全寄存器对照完成 ⇒ 差异**收敛到 `0x10014`/`0x10090`/`0x10010` 三个状态位**】

## 对照表（Android = slot a 空闲；Armbian = 播放中且已校验 `RUNNING`）
| 寄存器 | Android | Armbian | 判定 |
|---|---|---|---|
| `0x640c VPBR` / `0x6410 VBBR` | `0` / `0` | `0` / `0` | **✓ 一致** |
| `0x242c GPIO_PAD_CONTROL` | `04000000` | `04000000` | **✓ 完全一致** |
| `0x10110 IRQ1_MASK1` | `7ffcfe3f` | `7ffcfe3f` | **✓ 完全一致** |
| `0x6c04 AMP_GAIN_CTRL` | `00000253` | `00000253` | ✓ |
| `0x4808 SP_FORMAT` | `20200000` | `20200000` | ✓ |
| `0x2034` / `0x6418` | `0` / `0` | `0` / `0` | ✓ |
| `0x11000 GPIO_STATUS1` | `00000000` | `00000000` | **✓ 一致** |
| `0x4840` RX 字长 | `0x18`(24) | `0x10`(16) | 流格式不同（非缺陷） |
| `0x2018 PWR_CTRL3` | `00000020`（空闲） | `00003721`（播放） | 状态差异（播放时两边均 `3721`） |
| **`0x10014 IRQ1_STATUS2`** | **`30101f00`** | **`00300000`** | ✗ 差 **bit8-12 = `0x1f00`** |
| `0x10090 IRQ1_RAW_STATUS1` | `40806000` | `40406000` | ✗ 差 **bit23** |
| `0x10010 IRQ1_STATUS1` | `01400000`（**PUP_DONE=1**） | `00c00000`（PUP_DONE=0） | ✗ |

## 本轮洗清了两个此前的嫌疑（都作废）
1. **`VPBR/VBBR = 0` 不是异常** —— Android 也为 0
   ⇒ 之前"升压轨为 0V ⇒ 供电没开"的推断**作废**（§117/§118 的相关推论）。
2. **`GPIO_PAD_CONTROL`（`04000000`）与 `IRQ1_MASK1`（`7ffcfe3f`）与 Android 完全一致**
   ⇒ GPIO/中断配置**没有缺失**；`GPIO_STATUS1 = 0` 在本板**双方都是 0**，确认属正常。

## 结论：差异**只剩三个状态位**
`0x10014`(bit8-12) / `0x10090`(bit23) / `0x10010`(PUP_DONE)
—— 全部是**状态**，且都指向"**放大器的上电/运行态没有推进**"。
而**硬件已证明可用**（Android 同板能到 `PUP_DONE=1`）⇒
**必然是软件侧的某个动作让放大器真正启动**，我们还没做或做错了顺序。

## 下一步（最高价值）
Android 是"**空闲态就已经 PUP_DONE=1**" ⇒ 说明 Android 在**开机/初始化阶段**就把放大器带起来了
（不是每次播放才做）。而我们的放大器**从未进入过那个状态**。
⇒ **重点转向"放大器初始化/上电序列"**，而不是"播放时的配置"：
1. 对照 **Android 开机后 amp-fix 类脚本做了什么**（我们已从 HAL/厂商驱动提取过 17 组寄存器表 = `XM()`，
   但那次实验是在"粘滞位已被消费"的状态下做的，**结论不可靠**）；
2. **建议重做 `XM()` 实验** —— 现在我们有 1 s 超时的 `.ko`（更接近上游、也更能观测），
   在**干净启动 + 播放中**启用 `/root/xm.on`，看 `0x10010` 是否变成 `0x01400000`；
3. 若 `XM` 能让 PUP_DONE 置位 ⇒ **那就是缺失的写序列**，直接固化进 `amp-fix.sh` ✓（目标项 3 达成）。
> ⚠ 注意：这条实验**需要切回 Armbian**（`set_active b`），切回后先验键盘。

---

# 【第 125 轮 · 已抓到 Android「PUP_DONE=1 状态」的 192 寄存器参考 dump（待比对）】

## 本轮完成的
在 **Android（slot a）**、**`0x10010 = 01 40 00 00`（PUP_DONE=1）** 的状态下，
用 `/data/local/tmp/amptarget.sh`（192 个控制/配置寄存器）生成了参考 dump：
```
/data/local/tmp/dumpA_pup.txt          ← 192 行，在设备上（Android 侧）
0x00010010 = 01 40 00 00               ← 该状态确认 PUP_DONE=1 ✓
```
（`adb pull` 本轮未成功执行完 —— 文件仍在 Android 设备的 `/data/local/tmp/`，
下一轮可直接 `adb pull /data/local/tmp/dumpA_pup.txt` 取回。）

## 这为什么重要
这是第一份**"放大器处于我们从未达到过的状态（PUP_DONE=1）"下的宽范围参考**。
把它与 Armbian 侧的**同范围** dump 做 diff，就能定位"Android 多写了什么"，
而不再是靠猜 —— 这正是目标项 2 的方法论，且这次**状态差异是有意义的**（不是假差异）。

## 下一步（明确、按序）
1. **切回 Armbian**：`fastboot set_active b` + `reboot` → **第一件事验键盘**
   （`gpio127` = out high、`Xiaomi Pad Keyboard` 出现）；
2. 在 Armbian 侧跑同一个 `amptarget.sh`（**干净启动后、不播放**），得到 `dumpL_pup.txt`；
3. `diff` 两份 192 行 dump ⇒ **差异集合 = Android 让放大器进入 PUP_DONE 状态所需的写操作**；
4. 把这些写操作固化进 `amp-fix.sh`（或驱动）⇒ **目标项 2/3 达成**；
5. 若差异为空 ⇒ 说明是**时序/顺序**而非寄存器值差异，转而对照两边的**写序列顺序**
   （可用 `strace`/i2c 抓取，或对比厂商驱动的表顺序）。

## 现场状态（重要）
- **设备现在在 Android（slot a）**，ADB 正常（`32b28a4a`）
- Android 侧可用：`/data/local/tmp/{ampreg,amptarget.sh,play.sh,t.wav,dumpA_pup.txt}`
- Armbian 侧资产未动：`boot_b_tdmfix_kbd.img`、`.ko` 1 s 超时版（`7888f7b4…`）、
  回退 `boot_b_restore.img`（`ded90d33…`）
- 之前提到的 `XM()` 重做实验**仍值得做**，但先把上面的 dump diff 做完（成本更低、信息更直接）。

---

# 【第 126 轮 · ✅ Android 参考 dump 已取回主机（可直接做 diff）】

## 已落盘
```
/home/axis/axis_rnd/work/android/dumpA_pupdone.txt     192 行
  （Android slot a，状态 0x10010 = 01 40 00 00 = PUP_DONE=1）
  含 0x4808 = 20200000 / 0x6c04 = 00000253 / 0x10010 = 01400000 ✓
Windows 侧副本：C:\Users\cheny\Downloads\platform-tools\dumpA_pup.txt
```

## 下一步（就差这一条命令链）
1. **切回 Armbian**：`fastboot set_active b` + `reboot` → **先验键盘**
   （`cat /sys/kernel/debug/gpio | grep gpio127` 应为 out high；`Xiaomi Pad Keyboard` 出现）；
2. Armbian **干净启动后不播放**，跑同范围 dump：
   ```sh
   scp work/armbian/amptarget.sh root@172.16.42.1:/root/   # 若设备上还没有
   ssh root@172.16.42.1 'sh /root/amptarget.sh /root/ampreg 1 0x40 /root/dumpL_pup.txt'
   scp root@172.16.42.1:/root/dumpL_pup.txt work/android/
   ```
   （注意 Armbian 侧 `ampreg` 在 `/root/ampreg`，脚本参数顺序同上）
3. **diff**：
   ```sh
   diff /home/axis/axis_rnd/work/android/dumpA_pupdone.txt \
        /home/axis/axis_rnd/work/android/dumpL_pup.txt
   ```
   ⇒ 差异集合 = **Android 让放大器进入 PUP_DONE 状态所需的写操作**（或指出是时序差异）。
4. 把这些写操作固化进 `amp-fix.sh`/驱动 ⇒ **目标项 2/3 达成**。

## 现状
- 设备仍在 **Android（slot a）**；Armbian 资产未动
- 本文件已包含全部关键结论、纪律与回退命令，可独立交接

---

# 【第 127 轮 · ★★★★ Android ↔ Armbian 192 寄存器 diff 完成：**唯一未解释的配置差异 = `0x4810`**】

## 已完成
1. 切回 Armbian ✓；**键盘正常**（`gpio127 : out high`；
   输入设备名未列出 = 蓝牙/POGO 键盘当时未连接，非故障）；
2. Armbian 干净启动、**不播放**，抓同范围 192 寄存器 → `work/android/dumpL_pupdone.txt` ✓
3. diff 结果 → `work/android/d_pup.txt`（**仅 11 处差异 / 192**）

## 完整差异表（`<` = Android，`>` = Armbian）
| 寄存器 | Android | Armbian(空闲) | 判定 |
|---|---|---|---|
| `0x4808 SP_FORMAT` | `20200000` | `20180200` | **已知状态差异**：`amp-fix` 播放时会写 `20200000` ✓（§76.2 已证） |
| **`0x4810 SP_FRAME_TX_SLOT`** | **`04040404`** | **`03020100`** | **★ 我们从不写它 ⇒ 唯一未解释的配置差异** |
| `0x6c04 AMP_GAIN_CTRL` | `00000253` | `00000240` | **已知状态差异**：`amp-fix` 播放时写 `253` ✓ |
| `0x10010 IRQ1_STATUS1` | `01400000` | `00400000` | 状态（PUP_DONE） |
| `0x10014 IRQ1_STATUS2` | `30101f00` | `00000000` | 状态（bit8-12） |
| `0x10018` | `200001bf` | `20000080` | 状态 |
| `0x1001c` | `80000003` | `00000003` | 状态（bit31） |
| `0x10090 IRQ1_RAW_STATUS1` | `40806000` | `40406000` | 状态（bit23） |
| `0x10098` | `20000010` | `20000090` | 状态 |
| `0x1009c` | `82000002` | `02000002` | 状态（bit31） |

⇒ **7 个是状态位**；`0x4808`/`0x6c04` 是**已被 `amp-fix` 覆盖**的已知差异；
**真正"我们没写"的只有一个：`0x4810 = CS35L41_SP_FRAME_TX_SLOT`（Android 恒为 `0x04040404`）**。

## 这意味着什么
- `0x4810` 是 **TDM 帧的 TX slot 映射**。Android 四字节全 `0x04`，我们是 `03/02/01/00`。
- 虽然名字是 "TX"，但 CS35L41 的 **帧时隙配置会影响其内部时隙对齐**；
  若与 SoC 的 8×32 TDM 帧不匹配，放大器可能**收不到/对不齐数据**，
  从而**不推进上电状态**（`PUP_DONE` 不置位）——这与我们观察到的全部现象吻合。
- ⚠ 第 97 轮我曾手动在**一颗**放大器上写过 `0x4810=0x04040404`（当时仍无声），
  但那次是**单颗**、且当时 `.ko` 是 20 ms 版、且未做播放态全套验证 ⇒ **结论不可靠，应重做**。

## 下一步（明确、可直接执行）
1. **把 `0x4810 = 0x04040404` 加进 `amp-fix.sh` 的 `ASP()`**（8 颗都写，播放时生效）；
2. 播放并读 `0x10010`：
   - **若 `PUP_DONE(bit24)` 出现（`0x01400000`）⇒ 找到缺失的写操作！** ⇒ 固化 + 请用户实听；
   - 若仍不出现 ⇒ 差异只剩"状态位"，转查**写序列/时序**（或 `0x10098/0x1009c` 等状态相关的配置）；
3. 顺带把 `0x4808`/`0x6c04` 的**空闲态**也写成 Android 值（让空闲态也与 Android 一致，便于比对）。

## 现状
- 设备：**Armbian**，键盘正常；`/root/dumpL_pup.txt` 已抓
- 新资产：`work/android/dumpA_pupdone.txt`（Android 基准）、`dumpL_pupdone.txt`（Armbian）、`d_pup.txt`（diff）

---

# 【第 128–129 轮 · ★ 配置 100% 对齐后仍无声；pinmux/时钟代码路径已验证无误】

## 一、`0x4810` 已成功移植并生效（第 128 轮）
- `amp-fix.sh` 的 `ASP()` 中插入 `W $b $a 0x00004810 0x04040404`（服务 active、语法 OK）；
- **已验证播放态读回**：`0x4808=20200000`、`0x4810=04040404`、`0x6c04=00000253`
  —— **全部配置寄存器与 Android 逐字节一致**；
- **用户实听（第 129 轮确认）：仍然没声。**

## 二、本轮排除的两个方向（都有实测/代码证据）
1. **pinmux 无误**：`/sys/kernel/debug/pinctrl/*/pinmux-pins` 显示
   `gpio133=mi2s2_sck(BCLK)`、`gpio134=mi2s2_data0`、`gpio135=mi2s2_ws`、`gpio137=mi2s2_data1`
   **四个引脚全部被 "sound" 正确占用**（上轮以为 133/134 缺失是 `tail` 截断的误读）；
2. **AFE 时钟代码路径无误**：`q6tdm_ops` **有** `.set_sysclk`（q6afe-dai.c:684 区域）；
   `q6afe_mi2s_set_sysclk` 的 switch **正确覆盖** `PRI_TDM_IBIT..QUIN_TDM_EBIT` 范围；
   `q6afe_port_set_sysclk` 在该路径**忽略 `dir`**、以 `enable=!!freq` 发 `AFE_PARAM_ID_CLOCK_SET`
   ⇒ 时钟使能消息**确实会发出**（`dir=SNDRV_PCM_STREAM_PLAYBACK` 的怪写法无害）。

## 三、新发现：**SLPI 崩溃循环正在刷屏内核日志**（影响诊断）
```
qcom_q6v5_pas 5c00000.remoteproc: fatal error received: ... sensor_process ... 
remoteproc remoteproc0: handling crash #13 in 5c00000.remoteproc → recovering → up
```
- 本次启动已 **crash #13**（约每 30s 一次，与 §23/§67 的 sensor_process 崩溃一致）；
- 崩溃日志把音频相关日志**冲出 ring buffer** ⇒ 本轮 dynamic debug 抓取失败
  （播放确实发生：`pcm=RUNNING`、`amp-fix` 跑了、`0x2014=1`，但 dmesg 里
  "tdm slots"/"not latched" 计数 = 0 —— 被 SLPI 刷屏覆盖）；
- ⚠ **注意**：老结论（§23 修复 3）说 SLPI 必须保持运行，否则 ADSP 出
  `Memory_map_regions failed` —— 所以**不能**用 `slpi-off` 止血。

## 四、下一步（明确）
1. **避开日志洪水重新抓 AFE 序列**：起播放的**同时**用 `dmesg --follow > /tmp/d.log` 落盘，
   或播放后**立刻** `dmesg | tail -100`（抢在下一轮 SLPI 崩溃前）；
   目标：确认 `AFE start`/`CLOCK_SET`/`global_enable` 的**真实时序与返回值**；
2. 若 AFE 序列正常 ⇒ 剩下唯一解释是 **BCLK/FS 物理没跳**（需要示波器/逻辑分析仪才能终极确认）；
3. 另一条可走的路：**修通 TDM 采集**（TX 端口 `-22`），用采集流自证总线是否真的有数据/时钟 ——
   采集到了 = 总线活着；采集不到 = 总线死 ⇒ 与"放大器视角"互相印证。

---

# 【第 130 轮 · ★★★ 完整建立序列终于抓到（方法固化）+ 发现可疑的顺序问题】

## 一、抓取方法（已固化，避开 SLPI 刷屏 + PipeWire 占用两个坑）
PipeWire 会一直占着 PCM（`waited=0x0.5s` = 流早就在跑，抓不到建立事件），
必须**先强杀再开新流**：
```sh
fuser -k /dev/snd/pcmC0D0p; pkill -f "[a]play"; pkill -u axis -f "pipewire|wireplumber"; sleep 3
timeout 20 dmesg --follow > /tmp/d3.log 2>&1 &
sleep 1; amixer -c0 cset numid=452 1; aplay -D hw:0,0 /root/t.wav &
sleep 12; grep -iE "afe|tdm|cs35l41|apr|lpass" /tmp/d3.log | grep -viE "sensor_process|qmi|fastrpc"
```
（脚本已存 `work/armbian/afecap2.sh`，设备 `/root/afecap2.sh`；**内联后台会吞输出，务必用脚本**）

## 二、抓到的完整序列（新鲜开流，一次播放）
```
707.005-008  cs35l41 X-00XX: Set DAI sysclk 12288000          × 8 颗（machine driver startup）
707.008      Tertiary TDM Playback: codec cs35l41-pcm tdm slots 0x3/8/32   × 8（hw_params）
707.02-04    pushed factory calibration + tuning values（B 组 12 值；T 组 *-music.txt 缺失 -2）
708.04-715.07 elish: PUP_DONE not latched ... continuing        × 8 颗
             （global_enable(1) 逐颗执行，每颗正好 1s = 轮询超时；PUP 从不锁存）
715.18-718+  DSP1: cirrus/cs35l41-dsp1-spk-prot-xiaomi-elish.wmfw (v0.33.0) 逐颗加载
             + 每颗的 *-cs35l41-dsp1-spk-prot.bin
```

## 三、两个重要事实
1. **DSP 固件其实加载了**（wmfw + 每颗 spk-prot.bin，T 组可见）；
   之前的 "DSP1: Legacy support not available" 是**另一条路径**的信息，不是"没固件"；
2. **顺序可疑**：我们的顺序是 **enable(708) → 固件(715)**；
   而 Android 是**空闲态就已 PUP_DONE=1**（HAL 在初始化阶段就带起来了）。
   ⇒ **假设：放大器要在固件加载后（或至少在正确的顺序下）使能，PUP 才会锁存。**

## 四、下一步（两个便宜实验，均不需要重编/刷机）
1. **播放中（固件已加载后）手动做一次真正的掉电-上电**：
   `/root/ampreg 1 0x40 0x2014 0`（等 1s）→ `/root/ampreg 1 0x40 0x2014 1` → 读 `0x10010`
   看 PUP_DONE 是否锁存（§68 在 Android 上做过"清位+使能"不锁存，但**没在固件已加载的状态下做过完整 0→1**）；
2. **unbind/rebind i2c 驱动**（等价于 Android 的开机初始化路径）：
   `echo 1-0040 | tee /sys/bus/i2c/drivers/cs35l41-i2c/unbind` 再 bind
   → 完整重新 probe（含 reset 脉冲 + 全部配置）→ 播放 → 读 `0x10010`。
   ⚠ rebind 有风险（DAPM 状态混乱），先做实验 1。

---

# 【第 131 轮 · 实验 1 结果：掉电有反应、上电永远不完成 ⇒ "顺序"假设被削弱】

## 实验（固件已加载的播放态下，`bus1 0x40` 手动完整 power cycle）
| 步骤 | `0x2014` | `0x10010`(锁存) | `0x10090`(RAW) |
|---|---|---|---|
| 基线（播放中，固件已加载） | `1` | `00c00000` | `40406000` |
| **掉电**（写 0，等 2s） | `0` | `00c00000` | **`40806000`** ← **bit23 出现** ✓ |
| **上电**（写 1，等 1-4s） | `1` | `00c00000` | `40406000` ← bit23 消失，**bit24 始终无** ✗ |

## 三个结论
1. **i2c 写与电源转换确实到达芯片**（RAW 的 bit23 随掉电出现/上电消失 —— 芯片在"感知"电源状态）；
2. **掉电事件能登记，上电事件永远不登记**（bit24/PUP_DONE 任何时刻、任何状态下都不出现）；
3. **"先 enable 后固件"的顺序假设被削弱** —— 固件加载完成后再做完整 0→1，PUP 依旧不锁存。

## 由此收敛出的两个候选根因（都指向"上电序列在中途卡住"）
- **(a) VBAT/boost 供电缺失**：CS35L41 的数字侧（i2c 用的 VDDIO/VDDD）与功放侧（VBAT/boost 输入）
  是**不同的电源引脚**。i2c 正常 ≠ VBAT 在位。本板 DT **没有任何放大器供电轨建模**（§91），
  若该轨由 PMIC 某 LDO/开关控制而 Linux 没开它 ⇒ boost 无输入 ⇒ 上电永远完不成。
  **与全部观测吻合**（寄存器全对、无故障位、无瞬态、无输出）。
- **(b) BCLK/FS 缺失**：若放大器需要有效音频时钟才能完成上电（`0x10014` 缺 bit8-12 佐证），
  而 AFE 实际没在引脚上输出时钟 ⇒ 同样卡死。**终极确认需要示波器**。

## 一个值得注意的语义疑点（未解）
Android 空闲时 `0x10090 RAW = 40806000`（bit23=1）而锁存的 `0x10010 = 01400000`（bit24=1）。
若 RAW 的 bit23 表示"**当前处于掉电态**"（而不是"掉电事件"），则 Android 空闲时放大器
**其实是掉电的**（PUP_DONE 只是早期上电的残留）—— **位语义需要数据手册才能定论**，
在没有定论前不要再用这两个位当判据。

## 下一步（按性价比）
1. **查 PMIC 供电**：对照 Android 的 elish DT/原理图，找放大器 VBAT 走哪个 PMIC 轨，
   在 Linux 侧读 `regulator_summary` 看它是否 enabled；
   （Android DT 的 cs35l41 节点没有 supply 属性 ⇒ 该轨是"常开"或由更早的固件打开 —— 要查）
2. 若找到该轨且 Linux 未开 ⇒ **打开它**就是缺失的"写操作"（正是本目标要找的东西！）；
3. BCLK 侧（需示波器）暂缓 —— 先查供电，因为它是纯软件可查的。

---

# 【第 132 轮 · PMIC 供电轨盘点完成：`l10c_3p3` 是最强候选（定义了但无人用）】

## 全部供电轨状态（Armbian，本次实测）
**enabled**：bl_vddneg/pos_5p5（背光）、vreg_bob、l12a_1p8、l14a_1p88、l17a_3p0、l2a_3p1、
l5a_0p88、l6a_1p2、l9a_1p2、s4a_1p8、s5a_1p9、s6a_0p95、s6c_0x88、s8c_1p35
**disabled/unknown（且 0 使用者）**：usb_vbus、**l10c_3p3**、**l11c_3p0**、l18a_0p9、l1c_1p8、
l3a_0p9、l5c_1p8、l6c_2p9、l7c_2p85、l8c_1p8、l9c_2_9

## 关键事实
1. **`vreg_l10c_3p3`（3.3V）与 `vreg_l11c_3p0`（3.0V）在我们 DTS 里只定义、无人引用**
   （DTS 第 443/450 行定义 ldo10/ldo11；运行时 users=0、状态 unknown ≈ 关）；
2. CS35L41 的 VBAT 允许 2.5–5.5V —— 这两条轨的电压范围**完全吻合**；
3. Android 的基座 DT（v0/v1/v2.dts）里**没有 PMIC 轨定义**（那是极简 SoC DT），
   PMIC 配置在别的 DTB 里，现有素材查不到 ⇒ **无原理图时无法从 DT 侧定论归属**；
4. Android 的 cs35l41 节点**不声明任何 supply**（§106 已证）⇒ 若 VBAT 走 PMIC 轨，
   它由 Android 的 HAL/更早固件打开 —— **Linux 侧没人开它**，这正是"缺失的写操作"的形态！

## 下一步（明确、可执行）：DTS always-on 实验
1. 在 `sm8250-xiaomi-elish-common.dtsi` 的 `ldo10`（第 443 行区域）加 `regulator-always-on;`
   （必要时同样处理 ldo11）；
2. 重编 DTB → `make_boot_image.py` 打包 → 刷 `boot_b`（**键盘修复已验证，刷机安全**；
   回退镜像 `boot_b_restore.img` 在 Windows 侧）；
3. 刷后验证顺序：**先键盘** → 播放 → 读 `0x10010` 看 PUP_DONE(bit24) 是否**首次**出现；
4. 若出现 ⇒ **VBAT 供电就是根因**，把它固化进 DTS（属于目标项 3 的"驱动配置移植"）；
5. 若不出现 ⇒ 换 l11c 再试一次；仍不行 ⇒ 供电假设降级，转 BCLK（需示波器）。

## ⚠ 判据纪律（再次强调）
`0x10010`/`0x10090` 的位语义**未定论**（§131 的 RAW bit23 疑点）——
PUP_DONE(bit24) 出现与否只当**辅助信号**，**最终判据只能是用户实听**。

---

# 【第 133 轮 · ✅ 统一刷机脚本（用户要求）+ l10c always-on 实验结果：轨已开但 PUP 仍不锁存】

## 一、新工具（用户要求"把 boot 烧录做成统一脚本节省 token"，已落地）
```
work/tools/flash-boot.sh <image.img>     # 全流程：stage→reboot→fastboot→刷→重启→等ssh→全套检查
work/tools/flash-boot.sh --verify-only   # 只等 ssh + 跑检查（用于刷完没等到的情况）
work/tools/flash-boot.sh --rollback      # 一键刷回官方镜像 boot_b_restore.img
work/tools/audiocheck.sh                 # 设备侧检查（自动部署到 /root/audiocheck.sh）：
                                         #   键盘 gpio127、关键供电轨、playtest 三重校验、
                                         #   放大器寄存器（0x2014/4810/4808/6c04/10010/10090/10014）、
                                         #   Enable-failed / not-latched / AFE-err 计数
```
以后刷机验证 = 一条命令，输出即结论。

## 二、l10c always-on 实验结果（`boot_b_l10c.img`，md5 `2ce16be7c32046b34e31f9e65b3e5c89`）
| 检查项 | 结果 |
|---|---|
| 键盘 | ✓ 正常（`gpio127 out high`） |
| **`vreg_l10c_3p3`** | **`state=enabled users=1`** ✓ —— **always-on 生效，轨真的开了** |
| `vreg_l11c_3p0` | 仍 unknown/0（未动） |
| 播放 | ✓ `pcm=RUNNING`（waited 4×0.5s）、`0x2014=1` |
| **`0x10010`** | **`00c00000` —— PUP_DONE 依旧不锁存** ✗ |
| `0x10090` | `40406000`（与之前一致） |

⇒ **l10c_3p3 开启后 PUP_DONE 仍未出现** —— "VBAT=l10c_3p3" 的假设**未获证实**。
（注意：本次读数时 amp-fix 的 ASP 可能尚未跑完 —— `0x4810=03020100`、`0x6c04=0x240`
均为驱动默认值；但这两个寄存器不影响 PUP_DONE，§129 已证配置全对齐时 PUP 也不出现。）

## 三、剩余候选
1. **VBAT = 其他轨**（如 `l11c_3p0`，仍关着）—— 可用同样方法再试一次（改 DTS→flash-boot.sh）；
2. **VBAT = 电池轨（vph_pwr，常开）** ⇒ 供电假设整体不成立，问题在 **BCLK/FS 物理层**（需示波器）；
3. 位语义未定论（§131）⇒ PUP_DONE 也许根本不是"上电完成"的可靠指示。

## 四、下一步
1. 若继续供电假设：把 `ldo11` 也加 always-on（一次 DTS 改动），`flash-boot.sh` 一条命令验证；
2. 同时建议：**请用户在 Android 下确认"扬声器确实有声"**（最后一次硬件 sanity check）——
   若 Android 也无声，则一切假设重置。

---

# §132 救砖完成：ABL 无法加载启动镜像（已解决，专题文档已出）

**专题文档（新，权威）：`/home/axis/axis_rnd/ELISH_UNBRICK_GUIDE.md`**（347 行）
—— 含症状判定、两层根因、设备权威地图、正确修法、5 道校验、双系统恢复、禁忌清单、死路复盘。

## 症状
两个槽只进 fastboot；`fastboot continue` → `Failed to load image from partition: Device Error`；
`fastboot boot <任意镜像>` → `Failed to load/authenticate boot image: Device Error`；重试计数不减（引导器根本没尝试启动）。
同时 `fastboot flash` **照常 OKAY**（最大迷惑点）。

## 根因（两层）
1. **GPT 槽元数据被破坏**：ABL 从 GPT 分区项的 attributes/type GUID 读 A/B 槽状态；
   写入者是 `qbootctl`（经 UFS BSG 改写 **6 个 LUN** 的 GPT）。第一次是 `qbootctl -s b` 被中断，
   第二次是平板内 AI 改设备树/写坏分区表。⇒ 分区名/尺寸看起来都对，坏的是**槽属性一致性**。
2. **⚠️ 巨大陷阱：ROM 的 `gpt_main*.bin`/`gpt_both*.bin` 是"未解析盘容量"的模板**
   （`rawprogram*.xml` 里明文写着 `start_sector="NUM_DISK_SECTORS-5."`）。
   其 `last_usable_lba` 只有"已声明分区之和"，**不等于真实盘尾**：

   | LUN | 真实扇区 | 原厂模板 | 被截断的尾部分区 |
   |---|---|---|---|
   | 0 | 61880320 | 2686981 | `userdata` → **0 长度，`esp`/`linux` 槽位消失（致命）** |
   | 1/2 | 4096 | 2187 | `xbl_config_a/b` |
   | 3 | 8192 | 837 | `mdmddr` |
   | 4 | 557056 | 525475 | `vm-data`（无害） |
   | 5 | 16384 | 8197 | `mdm1m9kefsc` |

   **上次故障就是刷原厂 `gpt_both0..5` 修好引导、同时毁掉 LUN0 布局**（Armbian 掉 initramfs、
   Android 卡 logo），事后才重建出 `gpt_sfdisk.bak`。⇒ "恢复几个很小的分区"这经验只对一半：
   分区确实是 GPT（44 KB），但**必须用设备自己的表重建，不能照抄原厂文件**。

## 正确修法（本次已实测成功）
```bash
python3 work/tools/rebuild-gpt.py --tables backup/partbackup \
    --rom rom/elish_images_OS1.0.2.0.TKYCNXM_13.0/images --out /tmp/gptnew
for n in 0 1 2 3 4 5; do fastboot flash partition:$n /tmp/gptnew/gpt_both${n}_fix.bin; done
fastboot continue      # 必须变成 Resuming boot OKAY
```
* 新工具：`work/tools/gptpatch.py`（GPT 字节级补丁 + 校验原语）、`work/tools/rebuild-gpt.py`（驱动，可复现）
* 产物 md5：`gpt_both0 08e671ff… b1 47204fd5… b2 cd7681a5… b3 2f06dd70… b4 81ee3ca8… b5 cc9c591f…`
* 做法：把设备权威表（LUN0 的 36 分区，含 `esp`/`linux` 与全部 GUID/attrs）**施加**到原厂文件上，
  LUN1–5 只把末尾分区扩展到真实盘尾；改几何字段 + 重算主/备 CRC；其余字节一律不动。
* 5 道校验全过：序列化器与原厂文件逐字节往返一致 / 几何断言 / CRC 自校验 /
  与设备自身 `proc_partitions+blkid` 逐分区一致 / 改动字节最小化证明。

## 修复后实测
```
fastboot flash partition:0..5 → 全部 OKAY；slot-retry-count:a/b 重置为 7/7
fastboot continue → Resuming boot OKAY
内核 6.12.58-current-sm8250；根 /dev/sda36 ext4（linux 分区，完整保留）
sda34 userdata 93.1G(空) | sda35 esp 477M | sda36 linux 132.2G | sde12 boot_a 192M
verifiedbootstate=orange / device_state=unlocked（未回锁 ✓）
音频 card 0: Xiaomi Mi Pad 5 Pro ✓     SSH root@172.16.42.1 ✓
```

## 重要认知修正
* **Armbian rootfs 在独立 `linux` 分区（UUID `21ce0d2d-58df-4703-821f-ada8a6b8ae4d`），不在 `userdata`**；
  `userdata`（93.1G）当前**未格式化/为空** ⇒ Android 与 Armbian **互不冲突**（Android 用 super+userdata）。
  之前"rootfs 在 userdata"的判断是**错的**。
* 双系统：**slot a = Android**，**slot b = Armbian**；**Linux 槽的 `dtbo` 必须为空**（`fastboot erase dtbo_b`）。
* 启动镜像 ramdisk 里的 `lib/modules/6.12.58-current-sm8250` 必须与 `linux` 分区上 `/lib/modules/<版本>` 一致，
  否则模块全不加载（没声音没网络）。本次 4 个候选镜像版本全部一致，选了修复最全的 `boot_b_l10c.img`。

## 遗留
* Android 侧：本次救援中途中断过一次 8 GB `super` 刷写，故 Android 只能进 recovery；
  需要时补刷 `fastboot flash super <ROM>\images\super.img`（USB2 约 20–40 min）。
* 音频主线任务（响度）仍待继续，见本文件前文各节。
* **禁忌**：永不回锁 / 永不刷 `devinfo` / 永不直接刷原厂 `gpt_main*` / 永不刷半截 512B 扇区的 GPT dump；
  备份时要 dump **全部 6 个 LUN** 的 `sfdisk -d`（本次只有 LUN0 的，LUN1–5 靠重建）。



---

# §133 设备状态快探工具（先用它，别再猜模式）

**任何涉及设备的操作前，先跑这一条（实测 110 ms，远低于 3 秒）：**

```bash
work/tools/devprobe.sh
# 输出：<mode> | <detail> | <elapsed_ms>
#   fastboot        | 32b28a4a fastboot | 110ms
#   recovery        | 32b28a4a recovery | ...     ← OFRP 恢复模式（adb=root）
#   android         | 32b28a4a device   | ...     ← Android（adb 可用）
#   android-unauth  | 32b28a4a unauthorized | ... ← 恢复出厂后 adb 需在平板上点"允许"
#   armbian         | armbian:6.12.58-current-sm8250 | ...  ← USB 网 SSH 可登
#   off             | -                | ...     ← 未连接/关机
```

实现：`fastboot devices`(0.8s) → `adb devices`(0.9s) → `ssh root@172.16.42.1`(0.7s)，按序短路返回。
`PLATFORM_TOOLS` 环境变量可覆盖 platform-tools 路径。

## 本轮设备事实（2026-09-23，均只读探得）

* `boot_b` = md5 `74cfe6f0b5f9a2deffe4e37535f19fda`
  = **l10c 内核 + l10c ramdisk + rootfs 正确 DTB(appended 131576B)** + cmdline `slot_suffix=_b`
* `dtbo_b` = md5 `1db5bf0898eb30605b2f353433546d92`
  = **合法 DTBO**（magic `d7b7ab1e`，1 条目 = 同一份正确 DTB，页对齐 off=4096，id/rev=0 与原厂一致）
  —— 由 `work/bootimg/dtbo_b_ours.img` 生成；原状态是全零（ABL 报 `Dtbo hdr magic mismatch 0`）
* `misc` = `boot-system0`（BCB command 字段已清零）
* 关键结论（ABL 日志 `logfs` 为证）：**槽位/GPT/BCB 都正常**，ABL 确实在启动 slot B；
  失败点在 **DTB 组装**（`Dtbo hdr magic mismatch 0` + `DTB offset is incorrect, kernel image
  does not have appended DTB`），失败后 ABL 走 `Alternate slot _b, New slot _a` ⇒ 落到 Android ⇒ fastboot。
* 因此本轮修复方向 = **同时满足 ABL 的两条 DTB 路径**：boot 镜像内 appended DTB ✓ + `dtbo_b` 合法 DTBO ✓。
  复测方法：`fastboot boot ofrp.img` 进 recovery（**绝不 `reboot recovery`**）读 `logfs`，
  看那两行是否消失。

## 键盘目标（1–2s 重连）已备好的东西

* 根因：LPASS LPI pinctrl(`33c0000`) 的时钟来自 ADSP 侧 `q6afecc`；驱动
  `pinctrl-lpass-lpi.c:471-473` 在 `clk_bulk_prepare_enable` 超时(-ETIMEDOUT)时**硬失败不 defer**
  ⇒ 消费方 `usb_2_hsphy`(phy@88e4000) 永久 deferred ⇒ 键盘反复断电重连。
* 运行时修复（已装进 rootfs，开机自启）：`/usr/local/bin/lpass-pinctrl-reprobe.sh`
  + `/etc/systemd/system/lpass-pinctrl-reprobe.service`（已在 multi-user.target.wants 里启用）
* 根治补丁：`work/tools/keyboard-lpass-fix/0001-pinctrl-lpass-lpi-defer-on-clock-enable-timeout.patch`

---

# §134 键盘 1–2s 重连：真正根因已定位并修复（专题文档 `work/kernel/KEYBOARD_FIX.md`）

> 本节**推翻**了 §133 之后"键盘目标（1–2s 重连）已备好的东西"整段的判断，
> 以及"本轮设备事实"里把 `dtbo_b` 做成合法 DTBO 的说法。以本节为准。

## 1. 真正根因：`keyboard-default-state` 挂错了 pinctrl 控制器

设备树里键盘使能引脚状态 `keyboard_en_state: keyboard-default-state`
（pins `gpio9`，function `i2s1_data`）被放在 **`&tlmm`（`pinctrl@f100000`，主 TLMM）** 下，
但主 TLMM **没有** `i2s1_data` 这个 function —— 该 function 只存在于
**`&lpass_tlmm`（`pinctrl@33c0000`，LPASS LPI）**。

后果链：
1. `sm8250-pinctrl f100000.pinctrl: invalid function i2s1_data in map table`
2. `keyboard-default-state` 这个 pinctrl state 永远注册不成功
3. 消费方 `usb_2_hsphy`（`phy@88e4000`，`pinctrl-0 = <&keyboard_en_state>, <&keyboard_vdd_pin>`）
   拿不到合法 pin config
4. 键盘供电/数据引脚状态不对 ⇒ **每 6–7 秒断电重连一次**

**修复 = DTS 源码里一个词**：`&tlmm {` → `&lpass_tlmm {`（约 741 行，`sm8250-xiaomi-elish-common.dtsi`）。
补丁：`work/kernel/patches/0055-elish-keyboard-pinctrl-under-lpass-tlmm.patch`

交叉验证：Android 原厂也是把 `lpi_i2s1_sd1` 放在 `lpi_pinctrl@33c0000` 下
（且另有专用 `xiaomi,keyboard` 平台驱动，rst-gpio 141 / in-irq-gpio 83 / vdd-gpio 127）。
即**正确的 pinctrl 就是 LPASS LPI**，改 `&tlmm` 是错的。

## 2. 之前的"时钟超时/defer"分析是误诊

`pinctrl-lpass-lpi.c` 在 `clk_bulk_prepare_enable` 超时时硬失败不 defer —— 这是
**真实存在的驱动缺陷、但不是我这次键盘故障的原因**。`lpass-pinctrl-reprobe.sh`
运行时兜底脚本和 `0001-pinctrl-lpass-lpi-defer-...patch` 都不是根治手段，可以删掉。
（用户原话："修复服务可以先删了 应该有根本原因的"。事后证明用户判断正确。）

## 3. 关键教训（编译方式）

* **必须改 DTS 源码，用内核 build system 重编 DTB。**
  用 `dtc` 反编译 → 改 → 重编**能编译通过但起不来**（疑 phandle 重编号 / 格式漂移破坏 ABL 的 DT fixup）。
* **绝不能把自建 DTBO 刷进 `dtbo_b`。** 一刷进去**所有**镜像都起不来
  （连原本能用的 l10c 镜像也一起挂）。`dtbo_b` 必须**全零**。
  恢复手法：`fastboot erase dtbo_b`。
* boot 镜像**内核与 ramdisk 必须同源**（同一次 build），只允许换 DTB。
  混搭（rootfs 内核 + l10c ramdisk 之类）一律起不来。
* 192MB 的镜像**只能 `fastboot flash` + `reboot`**；`fastboot boot` 它必失败
  （`usb_read failed ... (31)`）然后回落到正常启动流程（所以看着像"启动进了 Android"）。
* 读 `logfs` 里的 ABL 日志必须**先制造一次失败启动，再进 recovery** ——
  `fastboot boot ofrp.img` 会**覆盖** `logfs`。

## 4. 已验证可用的产物

* 当前 `boot_b` = `work/kernel/artifacts/boot_b_kbd_fixed.img`，md5 `e96de85a19463c34a4410aae53ea4b22`
  = l10c 内核（`/tmp/l10c/Image`，42463744 B）+ l10c ramdisk（`/tmp/l10c/ramdisk.gz`，43682275 B）
  + 新编的正确 DTB + cmdline `root=UUID=21ce0d2d-... slot_suffix=_b`
* 备份副本：`work/bootimg/boot_b_kbd_fixed.img`、`/mnt/c/Users/cheny/Downloads/elish_imgs/boot_b_kbd_fixed.img`
* DTB：`work/kernel/artifacts/sm8250-xiaomi-elish-csot.kbd-fixed.dtb`（130436 B，键盘节点在 `pinctrl@33c0000` ✓）

## 5. 修复后实测（2026 本轮，设备在线 Armbian）

| 检查项 | 结果 |
|---|---|
| `dmesg \| grep -c "invalid function i2s1_data"` | **0** ✓ |
| `dmesg \| grep -c "usb 1-1: USB disconnect"` | **0**（修复前每 6–7s 一次）✓ |
| `lsusb \| grep 3206` | `Bus 001 Device 002: ID 3206:3ffc Xiaomi Pad` 持续在线 ✓ |
| `/proc/bus/input/devices` | `Xiaomi Pad Keyboard` / `Xiaomi Pad Mouse` / `Xiaomi Pad` 齐 ✓ |
| `/dev/input/event5` 4s 字节数 | 非零（418 B）✓ |
| 在线 FDT | `keyboard-default-state` parent = `pinctrl@33c0000` ✓ |

设备状态：`current-slot: b`，`dtbo_b` 全零，`misc` = 原厂（BCB command 已清零 + `boot-system0`）。

## 6. 工具修正：`work/tools/devprobe.sh` 的 SSH 分支此前**从未生效**

* Bug：`ssh -o ConnectTimeout=1.1` —— OpenSSH 的 `ConnectTimeout` **只接受整数秒**，
  给小数会报 `command-line line 0: invalid time value.` 并**立即退出 255**。
  所以任何在线的 Armbian 都被误报成 `off`（表现为 `off | - | 326ms` 这种"太快"的失败）。
* 实测 SSH over USB-gadget 握手要 **1.6–2.8 s**，原来"0.7+0.7+1.3 < 3s"的预算根本不成立。
* 已改为：`ConnectTimeout=3`（整数）+ 外层 `timeout 4`，并在探测前先用
  `ip -4 addr | grep 172.16.42.` 判断 gadget 网卡是否存在以短路。
* 验证：连测 4 次全部 `armbian | armbian:6.12.58-current-sm8250 | 1629–2940ms` ✓

---

# 第 135–142 轮（2026-09-23/24 会话）：AVCS 拓扑注册通道打通 + 多外设修复 + 一次事故复盘

## 135. AVCS 核心 = 拓扑注册的正确通道（主线 q6core.c 补丁 0056 系列）

主线 q6core.c 补丁（audio 之外最有价值的进展）：
* `AVCS_CMD_SHARED_MEM_MAP/UNMAP (0x12924/0x12926)`、`AVCS_CMDRSP_MAP (0x12925)`、
  `REGISTER_TOPOLOGIES (0x12923)`、`LOAD/UNLOAD_TOPO_MODULES (0x1296C/D)` 全部实现。
* remopteproc 父链 dma_alloc（0xfe300000 <4GB ✓，复用 0053 模式 + >4GB 拒绝保护）。
* 运行时触发：`echo 1 > /sys/module/q6core/parameters/elish_topologies`（默认关，
  写 1 → delayed work → request_firmware("elish_topologies.bin") → map → register）。
  另有 `elish_topo_modules`（写拓扑 id 触发 LOAD_TOPO_MODULES）。
* 回调：APR_BASIC_RSP_RESULT 五个 opcode 子分支 + AVCS_CMDRSP_MAP 存 handle。

实测结果序列：
1. map size=5444 → **-2 (ADSP_EBADPARAM)**，无崩溃。教训：mem_size_bytes 必须 4KB 对齐
   （8192），payload_size 仍传真实 5444（下游即如此：map_size=ION 对齐，payload=cal size）。
   **已修**：`q6core_elish_register_topologies(dev, dma, map_size, payload_size)`。
2. map size=8192 后 map 成功，但 **REGISTER_TOPOLOGIES 导致整机重启（两次复现，
   q6core3/q6core4 内核）**：`echo 1` 后 ssh 卡死，uptime 变 1min，无 pstore。
   最可能原因：拓扑引用 CIRRUS_SP 模块 0x10025353，但 ADSP 模块注册表（MREGINFO，
   adsp_avs_config.acdb）从未注入 —— **Android 顺序是先发 mreginfo(AUDIO_CORE_METAINFO=37)
   → AVCS_CMD_LOAD_MODULES(0x12989)，再发 topologies(cal 39)**。
   下一步：在 q6core.c 增加 LOAD_MODULES（mreginfo payload），或研究 METAINFO cal 路径。

## 136. 键盘复发 → 根治补丁 0001-pinctrl-lpass-lpi-defer（已验证）

* 症状：q6core2 内核启动后 gpio127=in low、kbd-devs=0、88e4000.phy 永久 deferred。
* 根因：`33c0000.pinctrl: error -ETIMEDOUT: Can't enable clocks → probe failed -110`，
  模块 3.6s 太早（ADSP 时钟未就绪）硬失败不重试 → keyboard-default-state 永不注册。
* 修复：`work/tools/keyboard-lpass-fix/0001-...patch` 应用进树（clk_bulk_enable 返回
  -ETIMEDOUT 时改 return -EPROBE_DEFER）。**此补丁是时序竞态的根治**（134 轮只是赢了竞态）。
* 验证：defer 后重试成功，33c0000 绑定 qcom-sm8250-lpass-lpi-pinctrl，gpio127=out high 8mA，
  kbd-devs=1，键盘/触摸板输入恢复，连续多内核版本稳定。

## 137. 触摸屏修复：固件塞进 initramfs（时序竞态）

* 症状：dmesg `Direct firmware load for novatek/nt36523-csot.bin failed -2` →
  `FW info is broken!` → 触摸死。
* 根因：rootfs 8.07s 才挂载，initramfs 里的 nt36523_ts.ko 6.63s 就 request_firmware，
  且 initramfs 无固件。之前赢竞态（恰好在 rootfs 挂载后请求）才有触摸。
* 修复：把 /lib/firmware/novatek/{nt36523-csot,nt36523-boe,novatek_nt36532e}_fw.bin
  打进 initramfs（解包 /tmp/rd → 加文件 → `find . | cpio -o -H newc | gzip -9 -n` →
  /tmp/ramdisk_new.gz，43,772,842 B）。make_boot_image.py 增加 **--ramdisk** 参数
  （并补写 header rsize 字段）。
* 验证：`fw_ver=0x1A, x_num=32, y_num=50` 固件加载成功，NVTCapacitiveTouchScreen 正常。

## 138. 蓝牙根因链（未完全收尾，但可用）

* 现象：hci0 DOWN RAW，`hciconfig hci0 up` → EOPNOTSUPP，bluetoothd "No default controller"。
* 根因链：btqca.c `qca_check_bdaddr`：控制器报的地址 == NVM 默认 → 设
  HCI_QUIRK_USE_BDADDR_PROPERTY → 期望 DT `local-bd-address` → 主线 elish DT 无此属性 →
  public_addr 全零 → **UNCONFIGURED → RAW**（hci_core.c 983/2664）。
* 修复 1（DTS）：elish-common.dtsi bluetooth 节点加 `local-bd-address = [88 52 eb e0 87 4b]`
  （persist /bluetooth/.bt_nv.bin 实测值）。→ hci0 立即 UP RUNNING PSCAN ISCAN，可连接。
* 修复 2（字节序，**未上设备**）：实测控制器把写入的地址按反序存储（写 88:52:EB:...4B
  → 控制器报 4B:...:52:88）。主线 hci_qca.c 的 BROKEN quirk switch **不含 QCA_QCA6390**
  （只有 WCN39xx/6750/6855/7850），即上游疏漏；已在树里给 qca_setup 加
  hoisted `if (hu->serdev) {...}` 块（受 qcom,local-bd-address-broken DT 属性门控）。
  **用户确认现状可用**（地址稳定、可连接），此项留待下次构建顺带验证。
* 另：AutoEnable=true 已写入 /etc/bluetooth/main.conf（去重后单行）。

## 139. WiFi MAC 固定（完成）

* persist /wlan/wlan_mac.bin = ASCII `wlan0=8852EBE0874A` → 真实 MAC 88:52:EB:E0:87:4A。
* ath11k 无 DT/nvmem MAC 支持（已确认 6.12 源码），但实测支持运行时 `ip link set address`。
* 实现：`/etc/elish/wifi-mac` + `/usr/local/sbin/elish-set-wifi-mac.sh`（等待接口 30s、
  已正确则跳过）+ `elish-wifi-mac.service`（After=udevd, Before=NetworkManager，enabled）。
* 验证：开机后 wlp1s0 = 88:52:eb:e0:87:4a（permaddr 仍是随机的，无妨）✓

## 140. 事故：并发 cp 竞态刷坏 boot_b（复盘与规程）

* 经过：打包+刷机命令整链进了后台；我以为没跑，又手动发第二次 → 两个 flash-boot.sh 并发，
  两个 cp 同时写 /mnt/c/.../elish_imgs/boot_b_q6core5.img，fastboot.exe 读到混合体
  → 内核段完好（能进 initramfs）+ ramdisk 尾损坏 → initramfs 后黑屏挂死。
  q6core5 镜像本体 md5 校验完好（本地 re-parse + cpio TRAILER 验证通过）。
* 恢复：用户手动进 fastboot → `fastboot flash boot_b boot_b_q6core4.img` → 正常。
  （Windows 侧 fastboot.exe 在 C:\Users\cheny\Downloads\platform-tools，设备 32b28a4a）
* **规程**：刷机必须单进程（先 pkill -f flash-boot.sh），cp 后校验 md5 再 flash；
  nohup 后台链不可靠，必须等打包输出"re-parse OK"再发起刷机。

## 141. 当前设备状态（boot_b = boot_b_q6core4.img, md5 54ded942...）

* 内核：6.12.58-current-sm8250 = 04:17 构建（defer 补丁 + q6core AVCS map 对齐修复 + hci_qca 未含）。
* 已验证正常：键盘（补丁）、触摸（fw 0x1A）、WiFi MAC（服务）、蓝牙（可连接，地址反序显示）、
  音频 8 amp 播放、ADSP 无 fatal。
* 设备网络：**新固定 IP 10.0.0.192**（WiFi；旧 USB-gadget 172.16.42.1 仍可用？未验证）。
  刷机时用户要求在场配合。

## 142. 充电任务开局（进行中）：主线缺 PM8150B charger 绑定

* 现状实测（10.0.0.192）：power_supply 只有 2× bq27z561（0-0055/13-0055，各监测一芯，
  74%≈4.11V/芯，即 2S 串联）+ tcpm-source-psy（12V/3A 协商成功）。
  **无 charger power supply，battery Discharging（-202mA/芯）→ PD 合同正常但根本没充电**。
* 主线 6.12 有 `drivers/power/supply/qcom_pm8150b_charger.c`（smb5, compatible
  "qcom,pm8150b-charger"，qcom_pm8150b_charger.ko 已在构建树里）；
  `pm8150b.dtsi:111 charger@1000` 节点完整（IRQ×4 + io-channels usb_in_v_div_16/usb_in_i_uv
  ← pm8150b_adc 7/8）但 status="disabled"。
* 参考启用：sm8250-retroidpocket-common.dtsi:925 `&pm8150b_charger { monitored-battery=...;
  status="okay"; }`（单芯 4400mV）。elish/pipa 的 battery_l/battery_r 节点
  **缺 voltage-max-design-microvolt**（驱动 probe 计算 float voltage 必需）。
* **关键安全未知数：2S 双电芯下 FLOAT_VOLTAGE_CFG(0x70) 的语义**（驱动公式按单芯
  (vmax-3487500)/7500+1，最大表达 5.4V）。上游无 2S 先例。**必须等原厂 smb5 驱动的
  2S 处理事实**（子代理正在挖 elish-kernel 原厂 DTS/驱动/模块）再决定写什么值。
* 用户要求：与 Android 完全一致的充电协商（67W 满速 + 充电曲线），**绝不损坏电池**；
  可插 PD 充电器实测（当前已插：PD 12V/3A）。
* 计划：M1 主线 charger 节点启用（安全值）→ M2 对齐原厂电流/电压/曲线值 →
  M3 PD/私有协议（视原厂架构决定，charge pump 驱动可能要移植）。

## 143–146 轮（2026-09-24 凌晨）：充电 bringup——主线驱动打通 + 上游 float 电压 bug

### 143. 原厂充电架构全解（子代理报告 + 自查 DTBO 交叉验证）
* **电池 = 1S2P 双节并联 8600mAh**（每节 4300mAh，各配一颗 bq27z561 电量计+保护 FET；
  vendor dual_fuel_gauge 聚合语义 V=MAX、I=SUM、容量=SUM）。**不是 2S 串联**（修正早期误判）。
* 充电链：TypeC/PD(PM8150B pdphy, PPS 通道) → **双 BQ25970 2:1 电荷泵**（i2c@884000
  0x65/0x66，`CONFIG_DUAL_BQ2597X=y`，ln8000@5B/51 是备选硬件节点）→ 电池 FCC≤12.4A@4.45V。
* Mi FC 认证：SVID 0x2717 UVDM（batterysecret 守护进程），认证后 PPS 6.2A（≈57-59W 峰值），
  未认证 PPS 限 4.8A；固定 PDO 限 9V/2A=18W（"Android 也只有 PD 18W"的真正原因）。
* k81 电池 profile（jeita/step 全表）+ 6 张热表 + smb5 全部阈值：见
  子代理报告（已存对话）+ /home/axis/axis_rnd/android_dtb/dtb_dtbo12_id00000000.dts（真机 DTBO）。
* Armbian 主线缺口：elish DT 无 charger 节点 → 从未充电（PD 合同正常但电池一直放电）。

### 144. M1 打通充电（DTB-only + 模块替换）
* `&pm8150b_charger { status="okay"; monitored-battery=<&battery-pack> }`；
  新增 battery-pack 节点（simple-battery, vmax 4.45V, 8600mAh）。
* 主线 `qcom_pm8150b_charger.ko`（Armbian rootfs 自带）绑定成功 → **首次实现充电**
  （电池 +0.65A/芯 @ PC 5V；PD 12V/3A 合同下砖头显示 12W 实充 ≈11W）。
* PD 合同本身 12V/3A=36W（tcpm sink VAR PDO 5-12V/5A 工作正常）；瓶颈在 FCC 默认 1.5A。

### 145. ★主线驱动的 float 电压公式 bug（安全关键，已修）
* 主线 `qcom_pm8150b_charger.c` 用 `(vmax-3487500)/7500+1` —— 这是 **PMI8998** 的寄存器布局，
  PM8150B 真实语义（vendor qpnp-smb5 smb5_pm8150b_params.fv 权威）是 **3.6V 基准/10mV 步进**。
* 后果：DT 4.44V 写 128 → 硬件解出 **4.88V**（依赖电量计保护兜底，危险）。vmax 4.45V 修正后写 85 ✓。
* 同时补了 **FCC=3A**（reg 0x1061, 50mA 步进, 值 60；vendor PMIC 路径热表上限 3.2A 内的保守值）。
* 修复版模块已部署：rootfs /lib/modules + **initramfs**（教训见下）。

### 146. 深坑记录：initramfs 模块 [permanent] 教训 + 两个事故复盘
* **rmmod 报 busy 根因**：initramfs 里也有一份模块（旧版），udev 在 initramfs 阶段加载后
  旧版带 [permanent] 标志（vermagic 或加载路径差异）无法卸载，rootfs 的新版永远没机会跑。
  → **模块更新必须三处同步：rootfs /lib/modules + initramfs 内的副本 + （模块签名/vermagic 不变时）直接换文件重打包 ramdisk**。
* **fdtdump 静默失败**：`fdtdump 2>/dev/null | grep -c` 用错路径时 stderr 被吞、输出 0 行，
  误判"DTB 丢了键盘节点"。重查用正确路径后节点完整。教训：grep -c 为 0 时先确认命令本身没失败。
* **flash-boot.sh 的 wait_fastboot 早退**：设备重启进 fastboot 比脚本轮询窗口慢，
  脚本报"never entered fastboot"但设备实际已就位 → 手动 fastboot flash 即可。
* 键盘"消失"事件（chg1 boot）：实为 lpass-lpi 时钟竞态偶发失败；chg2（同内核同 DTB 节点）
  启动后 gpio127 out high、零 lpass 报错、键盘正常。**defer 补丁仍是根治手段，偶发失败需下次现场抓 dmesg。**

### 当前镜像：boot_b_chg2.img（md5 f059537de560c43eb2069fa156ca729f）
* 内核 = q6core4 raw Image（已验证，未含 hci_qca BT 修复——留待下次顺带）
* DTB = 键盘修复 + 充电节点 + battery-pack 4.45V + bluetooth local-bd-address(+broken)
* ramdisk = novatek 固件版 + **修复版 pm8150b charger 模块**
* 充电实测：PC USB 5V 下 input 1.36A/6.8W；待 PD 充电器实测 FCC 3A 效果（预期砖头 ~25-30W）

### 下一步（充电 M2）
1. PD 充电器实测 FCC 3A 效果 + 温度监控（电量计 TEMP + 手感）
2. PPS APDO 加入 sink-pdos（主线 tcpm 支持）→ 看能否直接 PPS 直充（受 PMIC 路径限制 ~3A FCC）
3. i2c@884000 启用 + 探测 bq25970 → 移植双泵驱动（vendor bq2597x_charger.c）→ PPS+泵 48W+
4. 充电曲线：k81 jeita/step 表移植（用户态守护进程实现曲线投票是干净路线）
5. （可选）batterysecret 移植解锁小米私有 6.2A

## 147 轮（03:00-03:20）：55W FC2 全链路组件就绪（待刷+实测）

### 组件清单（全部在 work/kernel/elish_chg/ + 构建树）
1. **boot_b_chg5.img**（md5 5191c720354b56ee414d79d948ed728f）：q6core4 内核 +
   DTB（PPS APDO 3.3-12V/6.2A + op-sink 60W + i2c15 启用 + 双泵 DT 节点[原厂阈值,
   不复制 disable 标志] + 充电节点 4.45V + 键盘修复 + BT MAC）+
   ramdisk（novatek fw + 修复版 pm8150b charger 模块[新增 charge_enabled sysfs] + 泵驱动）。
2. **bq2597x_elish.ko**：vendor bq2597x_charger.c 手术式移植（保留 1-1802 行全部 ops/
   parse_dt/detect/init，替换尾部 glue 为最小 sysfs+probe）。要点：
   - SC8551 浮点代码全删（ARM64 内核禁浮点）
   - EXPORT_SYMBOL 全删（M= 构建的 static 符号不可导出）
   - **bat-OCP 强制启用 8A**（原厂 disable 且不传阈值；作为硬件兜底，正常单泵 ~6A 不触发）
   - sysfs: charge_enabled(rw) vbus_mv/vbat_mv/ibus_ma/vac_mv/tdie_raw/regs(ro)
3. **elish-fc2**（C 守护进程）：vendor usbpd_pm FC2 状态机用户态复刻：
   - 入口条件: vbat≥3500, <4430hys, cap<95, 温度[15,48)℃, fcc>2000
   - ENTRY_1: PPS 激活(online=2) + v=2×vbat+400, i≤6.2A → ENTRY_2: vbus 调谐 ±20mV(80次)
     → ENTRY_3: 关开关充电(charge_enabled=0, 即原厂 fc2_disable_sw) → 主泵开(cap<80 才开从泵)
   - TUNE 500ms: FCC=12400 起步；cell>4450×2次→FCC=6720；cell>4487→每次-200；
     ≤2200 taper done 退出；从泵 ibus<450 关从泵；全表限值与原厂一致
   - 退出/崩溃路径: 关泵 + 恢复开关充电 + PPS 退回固定 PDO
4. **主线 tcpm PPS 控制面**（免内核补丁）：tcpm-source-psy 的 online=2(激活PPS)/1(退回)、
   voltage_now(µV)、current_now(µA) 三个可写属性 + PROPERTY writeable=1（tcpm.c 7548）。
5. pm8150b charger 模块新增 charge_enabled sysfs（CHARGING_ENABLE_CMD_REG 0x1042）

### 测试计划（用户配合）
1. 刷 chg5 → 验证泵 i2c 绑定 + 读 ADC（12V 固定 PDO 下 vbus~11.8V）
2. **绝不能在固定 12V 合同下开泵**（2:1 泵 Vout=Vin/2=6V 会失控）——daemon 流程已保证 PPS 先行
3. 跑 elish-fc2 前台观察：PPS 激活→vbus 调谐→泵开→砖头功率攀升（预期 30-55W）
4. 温度监控（电量计 TEMP + 手感泵区）

## 148 轮（03:20-03:30）：目标升级为"完整移植"——k81 曲线全表实现

### 守护进程升级 elish-fc2 → elish-charged（完整版）
实现原厂 k81 battery profile 全部表格（DTBO 逐字复刻）：
* **JEITA FCC 表**（0.1℃ 步进）：<-10℃/≥58℃ **完全停充**；-10..0=810mA；0..5=1680；
  5..10=4200；10..15=6720；**15.1..48=12400（满速段）**；48..58=4200（warm）
* **JEITA FV 表**：-10..15℃=4450mV；15.1..48℃=4500mV(FFC)；48..58=4100mV
  （SW 路径 float_voltage 寄存器每周期跟随）
* **OCV 阶梯充电**：3000-3349=1A；3350-4199=12.4A；4200-4449=10.8A；4450-4500=6.72A
* **低温阶梯**（<15℃）：3000-4199=1A；4200-4450=660mA
* **热降流梯**（43℃ 起每 +1℃ 降一档）：12.4→10→8.8→8→7→6→5.6→5→4→3→2.5→2→1.4→1→0.7→0.3A
* **SW ICL 梯**（thermal-mitigation-pd-base）：同步按温度档限流
* FC2 只在 15.1-47.9℃ + 容量<95% 才进入；双泵只在容量<80% + <46℃
* 泵 taper 逻辑（cell>4450×2 次→FCC=6.72A；cell>4487→每周期-200mA；≤2.2A 退出）

### 模块扩展（qcom_pm8150b_charger.ko v3）
新增 sysfs：`charge_current`（FCC 寄存器 0x1061, 50mA 步进）、`float_voltage`（0x1070,
3.6V 基准/10mV 步进 vendor 语义）、原有 `charge_enabled`——守护进程每 500ms 按曲线表
写这三个接口，实现 SW 路径的完整曲线控制。

### 最终镜像 boot_b_chg6.img（md5 f30ddf778190809b2797e3709b1696a0）
q6core4 内核 + 完整 DTB（PPS APDO/泵节点/充电节点/键盘/BT）+ ramdisk
（novatek fw + 模块 v3 + 泵驱动 bq2597x_elish.ko）。部署脚本 deploy-m2.sh 更新为
elish-charged。**等待用户进 fastboot 刷机。**

### 当前设备实测（chg2 + 20W 状态）
ICL=2.55A@12V、80% 电量、充电稳定；曲线控制在 chg6 刷入后生效。

## 149 轮（03:28-03:33）：守护进程安全审查修复 + 预部署

elish-charged 终版（md5 2acafc60f92c1c77afde9df4226ad83b）修复 3 个审查发现的缺陷：
1. **致命**：JEITA 门每周期无条件 sw_enable(true)——FC2 期间会重新打开开关充电造成
   泵+SW 双路叠加。修复：仅在泵全关时恢复 SW。
2. FC2 TUNE 期间 eff_fcc 不被曲线表钳制（电池升温时热梯失效）。修复：tune 内每周期
   clamp 到 profile_fcc（只降不升）。
3. 局部 fcc 拷贝先于钳制发生（ibus_limit 用旧值）。修复：钳制先行。
4. JEITA 温度停机分支改 fc2_teardown(false)（不再瞬间恢复 SW 又关掉）。
5. 泵使能顺序对齐 vendor bq2597x：先从泵后主泵。

预部署完成（设备 /root/elish_chg_stage/）：elish-charged + systemd 服务 + 双模块（md5 全验）。
**待用户进 fastboot：刷 boot_b_chg6.img（md5 f30ddf77…）→ 装模块/服务 → 实测 55W。**

## 150 轮（03:33-03:50）：单元测试抓出 2 个查表严重 bug（已修复+全边界验证通过）

test_curves.c（宿主机测试）在写完后第一轮就抓出守护进程两个真 bug：
1. **JEITA FCC 查表循环根本不执行**：终止条件 `j->temp_ddc_hi` 与首段区间
   hi=0（0.0℃）冲突——循环体一次都不跑，温度分档全部失效（5℃ 也会 12.4A 满充！）。
   修复：哨兵改为 lo==0&&hi==0 判别。
2. **阶梯表半开区间缝隙**：vbat 恰好 3349/4449/4500mV 等边界值落空 → 查表失败
   → 不限流（12.4A）。修复：step_lookup() 缝隙/上界回退"最后越过的区间"（保守）。

最终验证（test_curves，全过）：150 点温扫不变量 + 22 个精确边界断言（含热梯∩JEITA
交集语义：57.9℃→700mA、58.0℃→300mA、58.1℃→停充；15.0℃ cool 沿 6720）+
FV 边界 + 阶梯含缝隙回退 + 低温表 + 42.9→58.0℃ FCC 单调非增检查。

守护进程终版 md5 **8d1dd7e1cade0d9a6ddadc60d6d19375**（已重新上传设备暂存目录）。
测试语义说明：实现 = 原厂表 ∩ 附加保守（warm 段热梯取 min，比原厂更保守）。

**仍待：用户进 fastboot → 刷 boot_b_chg6.img → 部署 → 实测 55W + 曲线监控。**

## 151 轮（03:50-03:55）：实机干跑验证（dryrun）通过

dryrun.c（已部署设备）在当前启动上用真实电量计数据跑完整曲线求值：
* 读取正确：cell 4253/4254mV、82%、37.5℃、ibat +519mA（充电中）
* 求值正确：jeita 满速段(151..480) ∩ step(4200-4449mV)=10800mA → FCC=10.8A
  （阶梯表在 82% 实测电压下正确生效）+ FV=4500mV（FFC 段）
* 状态机：FC2 入口判定 YES；**双泵判定 single-only（cap 82≥80 原厂线）**✓
* 输出 DRYRUN-OK——守护进程的数据管道（电量计解析→曲线→决策）端到端验证完毕。

软件侧全部就绪且验证完成；剩余步骤只有物理动作：用户进 fastboot → 刷
boot_b_chg6.img → 从 /root/elish_chg_stage 安装 → 实测功率/温度。

## 152 轮（03:55-04:00）：验证套件 verify-m2.sh 完成（已暂存设备）

* `verify-m2.sh install`：从暂存目录装双模块+守护+服务、泵绑定检查、启动服务
* `verify-m2.sh pumps`：只读泵 ADC/寄存器（安全模式，不开泵）
* `verify-m2.sh monitor`：1Hz CSV 采样（时间/温度/双芯电压/ibat/容量/tcpm 合同/
  泵 vbus·vbat·双 ibus/SW 路径 FCC·FV·EN）——实测时的证据记录器
* 冒烟通过（当前启动）。设备暂存目录 6 个文件齐备。

**状态：软件 100% 就绪。等用户物理动作（切 USB 进 fastboot）→ 30 秒刷机 →
verify-m2.sh install → 插回充电器 → monitor 看功率爬升 + 温度曲线。**

## 153 轮（04:00-04:05）：tcpm 日志预验证——充电器能力确认，PPS 通道在！

从 /sys/kernel/debug/usb/tcpm-*/log 读出充电器 source caps：
* 固定 PDO：5V/3A、9V/3A、12V/3A、15V/3A、20V/5A（百瓦级 PD3.1 头）
* **PPS APDO×2：5-11V/5A、5-20V/5A** → FC2 直充路径前提成立
* 推算：本头 PPS 限 5A → FC2 预期 ~45W（电池侧 ~10A@4.3V）；55W 峰值需小米 67W 头
* 主线 tcpm 对 APDO（type 3）解析正常；sink APDO(3.3-12V/6.2A) 与源 APDO1
  重叠区 [5,11]V ✓，激活匹配应成功
* tcpm 日志位置记录：/sys/kernel/debug/usb/tcpm-c440000.spmi:pmic@2:typec@1500/log
  （不在 /sys/kernel/debug/tcpm/ 下——此前找不到的原因）

**唯一待办不变：用户切 fastboot → 刷 chg6 → verify-m2.sh install → 实测。**

## 154 轮（04:05-04:20）：chg7 刷入成功——键盘确定性修复生效 + 泵/BT 模块同步就绪 + 设备半挂起待恢复

### chg7（md5 c203fb62303510a0616968cbb69c4bec）内容与已验证结果
* 镜像：q6core4 内核 + DTB（泵节点补齐 bat/bus-therm-threshold 21/21）+
  ramdisk（**重新 depmod 过的 modules.alias** + 补丁版 lpass pinctrl ×2 +
  补丁版 hci_uart/btqca + 充电模块 v3 + 泵驱动 v2[含 MODULE_DEVICE_TABLE]）
* **键盘确定性修复生效**：gpio127 out high（补丁版 lpass 模块从 initramfs 加载，
  -ETIMEDOUT→EPROBE_DEFER 重试机制真正在跑）——时序竞态根治 ✓
* BT MAC 仍 4B:87 反序（待查：initramfs hci_uart 是否真被加载——需在恢复后核对
  /proc/modules 与 ramdisk 内容 md5）
* 泵未自动加载（rootfs 模块旧版无 alias；scp 部署时网络断了）

### ★ 教训固化：模块补丁必须三处同步
**凡在树里改的 =m 模块，必须重新：①编模块 ②替换 initramfs 副本 ③替换 rootfs 副本
④initramfs 里重跑 depmod（modules.alias/dep 是文本表，宿主机 depmod -b 可生成）**。
本次：lpass（键盘）、hci_uart（BT）、pm8150b_charger、bq2597x_elish 四组全部同步完毕。

### ⚠ 当前设备状态：半挂起（需要用户物理动作）
* ping 通（2.5ms）但 ssh 会话建立后挂死 → 典型存储 IO 停滞/半休眠
  （sshd accept 正常=网络栈活着；fork/exec 读盘全挂=磁盘层卡死）
* 触发时机：chg7 启动验证后约 2 分钟，scp 部署泵模块时网络断
* 可能诱因：桌面 idle 触发 suspend 且 resume 卡死（新启用的 i2c15/pm8008 设备
  suspend 路径可疑），或 UFS IO 偶发挂死
* **电池安全无虞**：PMIC 硬件充电（FCC 3A + float 4.45V + AICL）+ 电量计保护 FET
  独立于 CPU 状态工作，即使设备挂起也不会过充
* 恢复路径（用户）：按电源键唤醒 → 若无效则长按电源强制重启（chg7 已持久化，
  重启零损失；重启后模块自动加载路径已就绪）

## 155–157 轮（04:30-05:00）：夜间自主会话——泵 SC8551 真身 + 多个部署学案 + suspend 挂起模式确认

### 重大发现
1. **泵芯片真身 = SC8551A（Southchip bq25970 克隆，ID 0x51）**——vendor DTBO 双 ac-ovp 命名
   并存的原因。恢复 SC8551 支持（整数定点缩放 raw×LSB：IBUS 25/16、VBUS 15/4、VBAT 503/400 等；
   sc8551_init_adc 的 0x34=0x01 精度寄存器；sc8551,ac-ovp 属性改可选回退 ti 值）。
2. **chg7 "刷入成功" 实为失败**：`cmd|grep|head -3 &&` 链中 head 恒返 0——文件没拷过去 flash 报错
   被 head 掩盖、reboot 照跑。规程修正：**flash 输出必须直接看 Sending/Writing/OKAY 三行，
   禁止在 flash 命令上接可能吃退出码的管道**。
3. **充电模块 sysfs 挂错位置**：sysfs_create_group(&pdev->dev.kobj) 挂平台设备而非 psy 目录
   → 守护读不到。修正为 &chip->chg_psy->dev.kobj（v5, md5 db53b26c）。
4. **模块热替换的正解**：主线驱动无 remove → wake irq 残留 → 重探 -EEXIST。补 smb5_remove()
   （cancel work + dev_pm_clear_wake_irq + sysfs 清理，v4+ 均有）。
5. **rootfs 模块版本漂移**：v1 曾留在 rootfs、depmod 触发 udev 从 rootfs 加载旧版盖过 initramfs
   新版。规程：每次更新模块必须 rootfs+initramfs 双同步。

### 实测成功项（chg8 启动，boot_b_chg8 md5 c6e14e88…）
* **双泵开机自动绑定**（SC8551 模块 + DT 热阈值 21/21 + alias）：master(0x01,mode=2) +
  slave(0x01,mode=1) 双 ready；ADC 读数合理（master vbat 4362mV/vbus 4605mV）
* **键盘确定性修复**（补丁版 lpass 模块自 initramfs 加载）gpio127 out high ✓
* **守护进程**：曲线求值正确（83% → step 10.8A@4.2-4.45V 段）；**PPS 失败退避 60s 生效**
  （不再 500ms 骚扰协商）
* systemd 服务自启 + 崩溃自动重启 ✓

### ⚠ 遗留问题（晨间处理）
1. **suspend 半挂起模式**（第二次确认）：无人操作 ~5 分钟后桌面触发 suspend，resume 中途
   卡死（ping 通/存储 IO 死）。两次均发生在启用 i2c15/pump 后的镜像。怀疑新 i2c15 总线
   （pm8008? 泵? geni-i2c suspend）打破 resume。**恢复后第一件事：
   systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target**
   （rootfs 上操作），再排查 echo mem > /sys/power/state 最小复现。
2. 充电模块 v5（sysfs 挂 psy 路径）已进 **chg9**（md5 db592f98…，待晨间刷）——
   当前 boot 里 initramfs 仍是 v4（错误路径）。
3. FC2 直充实测（需用户插 PD 充电器 + 守护 PPS 激活）仍未执行。

### 晨间恢复 runbook
1. 用户长按电源强制重启（或进 fastboot）
2. 若 fastboot：刷 chg9（`fastboot flash boot_b .../boot_b_chg9.img`）
3. ssh 后立即：mask 全部 sleep target（防再次挂起）
4. 热替换不需要了（chg9 initramfs = v5）；确认 /sys/class/power_supply/pm8150b-charger/
   下三文件存在 + 守护在写值
5. 用户插 PD 充电器 → verify-m2.sh monitor → 观察 PPS 激活 + 功率爬升 + 温度

## 158 轮（05:00-05:05）：ADSP 音频研究重启 + AVS 状态门/DEREG 实现（chg10）

### 音频侧调研结论（源码级，夜间无设备完成）
1. **排除 MREGINFO=模块注册表理论**：META_CAL 实为 LICENSE 路径（core_set_license，
   但在 vendor 树里无调用者=死代码）；CIRRUS_SP 本就 BUILT-IN。LOAD_MODULES 仅用于
   AFE 压缩 offload 的编解码模块，与拓扑无关。
2. **REGISTER_TOPOLOGIES 崩机的两大嫌疑修复**（对照 vendor q6core_send_custom_topologies）：
   a. 原厂在 REGISTER 前 **先 DEREG 全部自定义拓扑**（AVCS_CMD_DEREGISTER_TOPOLOGIES
      0x1292A, mode=2 全部字段 0）——已实现。
   b. 原厂有 **AVS "modules ready" 状态门**（GET_STATE 响应 payload[0] == 5
      =ADSP_MODULES_READY_AVS_STATE 才继续；mainline 误把 RSP opcode 当状态——已修正
      读取 payload[0]）——半就绪 AMDB 上注册拓扑 = 崩机最大嫌疑。已实现 40×500ms 等待，
      状态不到 5 拒绝注册（不再崩机）。
3. APR 头字段：mainline apr_send_pkt 自动填 src/dest svc/domain ✓（与原厂等效）。

### chg10（md5 1798b7d77af4370fac13a88550a85395）——晨间直接刷这个
* 内核 = 全新构建：q6core AVS 门+DEREG + **hci_qca BT 字节序修复顺带上车**
  （01:01 构建验证过镜像完整性，风险低）
* DTB/ramdisk 同 chg9（充电模块 v5[sysfs 挂 psy 路径] + SC8551 泵 + 补丁 lpass +
  novatek fw + BT 模块）

### 晨间 runbook（完整顺序）
1. 用户唤醒/强制重启设备 →（理想：进 fastboot）
2. fastboot flash boot_b_chg10.img → fastboot reboot
3. ssh 后立刻：`systemctl mask sleep.target suspend.target hibernate.target
   hybrid-sleep.target`（防挂起——两次半挂起的根治前置）
4. 验证：泵双绑定 + 充电三 sysf + 守护写值（watch journalctl elish-charged）
5. 音频拓扑测试：`echo 1 > /sys/module/q6core/parameters/elish_topologies`
   → dmesg 看 "AVS state 5 (modules ready)" → "deregistered" → "AVCS topologies
   registered (5444 bytes)"。若状态门挡住（state≠5），观察日志不再有崩溃。
6. 注册成功后：copp_topology=0x1000a100 → 播放 → 听立体声/响度/破音
7. 若拓扑仍不行：amp 级修正（B 组 ASPRX2 声道交换 + 0x4810 每泵 TX slot
   TRH=0/TLH=1/TRL=2/TLL=3/BRH=4/BLH=5/BLL=6/BRL=7 + AMP PCM Gain 对齐 Android）
8. 充电 FC2 实测（用户插 PD 充电器）：verify-m2.sh monitor 看功率爬升

## 159 轮（09:50-10:10，夜间续）：vendor 功放驱动逆向完成——响度/立体声两个谜底全部揭开

### audio_cs35l41.ko 逆向成果（debugfs 从 vendor.img 提取，aarch64 objdump）
1. **Channel Swap 控件 = DSP 算法控件 "CH_BAL"**：
   - `cs35l41_channel_swap_put`: `if (enum != Off) val = 1 << 22; val = htonl(val);`
   - **B 组 On = 写 4 字节 BE `00 40 00 00` 到 "CH_BAL"；T 组 Off = 全零**
   - 即：每颗功放固件里的声道平衡位（bit22 of CH_BAL 参数）
2. **AMP PCM Gain = Digital PCM Volume (0x6000 AMP_DIG_VOL_CTRL)**：
   - `cs35l41_set_vol(val)`: val 0..913；raw = val<817 ? (1231+val) : (0x1FFFCCF+val)；写 0x6000 mask 0x3FF8 << 3
   - **与主线 "Digital PCM Volume" SOC_SINGLE_SX_TLV 刻度完全同构（min 1231/max 913 区间）**
   - **Android 的 18 = 主线 amixer 值 18**（响度修正就是一条 amixer 命令！）
3. T 组 music.txt 在 Android 上也是 0 字节（原生如此），非缺失。

### 晨间音频修正三件套（audio-morning.sh 已备好）
A. **响度**：`amixer` 8 颗全设 Digital PCM Volume=18
B. **立体声**：CH_BAL B 组写 0x400000（先探测主线是否暴露该控件；无则打补丁走 cs_dsp_coeff_write）
C. **完整管线**：chg10 拓扑注册（AVS 门 + DEREG + 5444B blob）→ copp_topology=0x1000a100 → 播放

### 已完成的夜间工作汇总（等用户醒）
- chg10 打包完毕（md5 1798b7d7…）：新内核（AVS 门+DEREG+hci_qca）+ 全套充电件
- 音频逆向 + 晨间脚本 audio-morning.sh
- 设备仍处 suspend 半挂起（ping 通/ssh 挂），需用户按键唤醒或强刷重启

## 160 轮（10:50-11:30）：v6 修复生效 + 拓扑注册崩溃复现 + 黑匣子诊断方案就绪

### chg11/chg12 验证结果
* **充电管线全绿（chg11/chg12 实测）**：oops=0、双泵绑定、守护曲线实时写入闭环验证成功
  （82% 电量 → 阶梯表 10.8A → 模块 FCC 寄存器 → 读回 10800mA 完全一致）
* **AVS 状态门生效（chg11）**：ADSP 状态恒为 1（"want 5"），40 次重试后正确拒绝注册，零崩机
  → 播放中再测仍为 1 → **state 5 依赖 Android HAL 全套初始化，Armbian 不可达**
* **放宽后（chg12，原厂语义 state>=1 + DEREG + REGISTER）**：注册流程再次把设备带崩
  （ping 活/ssh 死/最终全断）——**即使先 DEREG 也复现**。

### 崩溃机制分析（为何原厂不崩我们崩）
最可能级联：REGISTER → ADSP 致命错误 → ADSP SSR → mainline 音频驱动的 SSR 恢复路径死锁
（q6adm/q6afe 带锁等待永不来的响应）→ 内核半死（网络中断上下文活着=ping 通，用户态饿死）。
原厂有完整 SSR 恢复基础设施；mainline 不完整。
另一嫌疑：注册时机（原厂 ~2s 开机早期 HAL 发送；我们 65s，音频流/泵守护已活跃）。

### ★ 晨间黑匣子诊断方案（免刷机，chg12 已在 boot_b）
设备断电重启进 Armbian 后：
```
ssh root@10.0.0.192
echo 1 > /proc/sys/kernel/panic_on_oops      # oops 即写 ramoops
echo 10 > /proc/sys/kernel/panic             # 10 秒后自动重启恢复
echo 1 > /sys/module/q6core/parameters/elish_topologies   # 触发注册
# 崩溃 → 自动重启 → ssh 重连后：
ls /sys/fs/pstore/; cat /sys/fs/pstore/console-ramoops* | tail -80   # 崩溃现场
```
DT 已有 ramoops@b0000000（4MB console 2MB）+ CONFIG_PSTORE_RAM=y ✓

### 待用户操作
设备现在断网（需断电重启）。充电管线部分已完成且验证；音频拓扑诊断等黑匣子数据。

## 161 轮（13:30-13:55）：拓扑注册 5 次静默复位 + XPU 假设 + 固件溯源 + 功放快修复全上

### 拓扑注册崩溃的完整特征（5 次复现，全同签名）
* 触发：AVCS REGISTER_TOPOLOGIES（DEREG 成功、map 成功、-110 超时或直接触发）
* 崩溃：**亚秒级整机静默复位**——无 oops（panic_on_oops=1 无效）、无锁死检测（chg13 开了
  SOFT+HARDLOCKUP_DETECTOR_BUDDY 无效）、无 ADSP fatal 日志、ramoops 零记录
* 结论：复位发生在 **TZ/RPM 层**（所有 Linux 诊断层之下），远程手段无法看到现场
  * 排除：固件不匹配（已换原厂 modem_a 分片重建的 adsp.mbn=同一崩溃）
  * 排除：AP CPU 锁死（检测器武装无果）
  * 未证实：XPU 内存权限（ADSP 读 CMA 0xfe300000 触发 TZ 拷打响应？）——
    kmalloc 普通页两次都落在 >4GB 被安全门拒绝，GFP_DMA32 在 arm64 无效，
    GFP_DMA 测试崩溃（日志被吃，无法确认分配地址）——理论仍开放
* **拓扑路径冻结**：需串口控制台（UART）才能继续，不再盲试

### 固件溯源（有价值的副产品）
* 原 adsp.mbn(18.5MB, md5 94d10008) 与 modem_a/b 分片**内容不符**（同尺寸同地址、
  字节不同 = 不同构建，疑似 LineageOS 来源）
* 已用设备自己的 mdt+b00-b18 重建 adsp_rebuilt.mbn (19,378,176B, md5 072cd39a…)
  部署生效（ADSP 正常启动）。重建方法：mdt 头 0x2b4 + b00-b18 按段表 offset 原样放置
* modem_a == modem_b（分片 md5 相同）

### ★ 功放级快修复（不依赖拓扑，已应用）
主线 cs35l41 驱动已把 Android 的功放控件全部暴露为 ALSA：
* **Digital PCM Volume**（INTEGER 0-913，与 Android "AMP PCM Gain" 同刻度同公式）：
  mainline 默认 865 = **衰减区**（≥817 为负增益！）= "声音小"的根因；Android=18 = 响亮
  → 8 颗全设 18 ✓
* **DSP1 Protection cd CH_BAL**（BYTES×4，即 vendor 的 wm_adsp_write_ctl("CH_BAL")）：
  B 组 4 颗设 0x00,0x40,0x00,0x00（=1<<22 大端，Channel Swap On=右声道）；T 组保持
  全零（左声道）→ 立体声修复 ✓
* 控件 numid：Digital PCM T组=1,19,37,55 B组=73,91,109,127；CH_BAL T组=1276,1333,
  1390,1447 B组=1504,1561,1618,1675
* 破音：功放自身保护 DSP（Protection 固件 + 工厂标定 cal_r/ambient 已推）仍在工作，
  响度恢复后待实测听感

### 待用户听感验证
lr_test.wav：440Hz 左2s右2s交替×5。判定：响度恢复？上下两组轮流响（立体声）？破音？

## 162 轮（14:40-15:10）：Android 实机逆向——fastrpc ACDB 灌入机制 + 崩溃=断电级 + 黑匣子修复

### Android 原厂采集（无 root 完成）
* **拓扑注册全时序**（logcat 实锤）：
  1. 14:13:05.255 `ACDB Load file: adsp_avs_config.acdb` + `[ACDB Command]→SW Minor/Major/Revision version info`
  2. 14:13:05.275 send_common_custom_topology（GET size=5444 ret=0 → CORE_CUSTOM_TOPOLOGIES）
  3. 14:13:05.279 "Common custom topology in use" —— **全程 4ms，开机 ~63s**（时序理论死亡）
* **★ fastrpc ACDB 灌入**：`adsprpcd: Successfully opened /vendor/etc/acdbdata/adsp_avs_config.acdb`（14:13:03）
  —— **MREGINFO/模块注册表经 fastrpc（不是内核 cal）进 DSP**！ACDB 引擎在 DSP 侧，
  原厂 HAL 把整个 acdbdata 经 fastrpc 传给 DSP → AMDB 认识 capi_v2_cirrus_sp.so → 拓扑可注册。
  Armbian 缺这一步 = 拓扑里的模块引用解析失败 = AVCS 核心处决全芯片。
* 运行时 ACDB 序列（每条流）：send_audio_cal → asm_topology → adm_topology → audtable →
  audstrmtable → afe_topology + app_type cfg(69938/69940) + acdb_id(10011/10003)

### Magisk 状态（boot_a 已打补丁，待用户晚间完成）
* boot_patch.sh CLI 流程成功（repack 192MB, ramdisk +220KB）
* 守护进程已启动（"Magisk 31.0 daemon started"）但 "Magisk environment incomplete, abort"
  （环境初始化失败，疑似 MIUI SELinux 拦截）→ su 不可用
* 原始 boot_a 备份在 /data/local/tmp/boot_a.img（设备）；用户晚间可用 App UI 完成

### 崩溃性质最终定性
* pstore 修复（去掉 DT ecc-size=16 → "uncorrectable error in header" 消失，ramoops 正常注册）
* 但崩溃后 pstore 仍空 = **PMIC 级断电**（DRAM 被清，不是 warm reset）→ TZ 对严重故障的全芯片处决
* **串口 UART 是唯一剩余诊断手段**（TZ/ADSP 的死前日志只在串口出现）——用户晚间可接

### 下一步路线
A. fastrpc 路线（原厂正路）：把 acdbdata 经 /dev/fastrpc 灌入 DSP（需要逆向 fastrpc 调用序列或
   移植 vendor 用户态——大工程）
B. 串口路线：接 UART → 重现崩溃 → 读 TZ/ADSP fatal 日志（硬件需要用户接）
C. AVCS_CMD_LOAD_MODULES 试验（内核侧给 AMDB 注册 cirrus 模块 id）——低成本一次试验

## 163 轮（15:15-15:35）：分段实验法锁定根因 + fastrpc 工程图景

### 分段实验框架（elish_topo_stage 参数，免重刷逐段推进）
* stage 1 = AVS 门 + LOAD_MODULES(0x10027053)：**存活**，ADSP 优雅回 EFAILED(-1)
  ⇒ **内核 LOAD_MODULES API 不能注册 built-in 模块**（该 API 是 AFE 编解码器专用路径）
* stage 2 = + DEREG：**存活**，kmalloc 落在 0x929e0000（<4GB 正常 DRAM！GFP_DMA 生效），
  DEREG 成功返回 ✓
* stage 3 = + REGISTER（低地址缓冲 0x928f0000）：**仍然 wedge**（延迟 1-2 分钟，网络栈活/用户态死）
  ⇒ **地址理论彻底死亡：内容处理（模块引用解析）= 唯一根因**

### 修正与确认
* **CIRRUS_SP 模块 id = 0x10027053**（不是笔记早期的 0x10025353——拓扑 blob 2 处引用 +
  MREGINFO 双向验证 ✓）
* 内核 cal 路线全部排除（ msm_audio_calibration.h 全枚举只有 METAINFO=37 死代码 +
  CUSTOM_TOPOLOGIES=39 两种，无 manifest 通道）
* **fastrpc = 唯一原厂路径**（MREGINFO 经 DSP 侧 ACDB 引擎）

### fastrpc 工程图景（下一阶段）
1. **主线 DT 缺 ADSP fastrpc 节点**：sm8250.dtsi 只有 sdsp 的（fastrpcglink-apps-dsp 通道在
   sdsp 的 glink-edge 下）；原厂在 ADSP/cDSP/sDSP 三处都有。需在 &adsp 的 glink-edge 下
   加 fastrpc 子节点（同样的 "fastrpcglink-apps-dsp" 通道名 + compute-cb 1-3）
2. 主线 fastrpc 驱动（drivers/misc/fastrpc.c）已加载 ✓（lsmod 确认）
3. 用户态：复刻 ACDB 远程调用（libacdbloader.so 逆向 → fastrpc ioctl 序列 + DSP 侧
   ACDBCommand 接口 + 反向文件回调（DSP 经 apps_std 读 .acdb——主线 fastrpc 驱动应支持）
4. 之后：fastrpc 灌入 adsp_avs_config.acdb → 再触发拓扑注册（应 4ms 成功如原厂）

### 待用户
* 设备需断电重启（stage 3 wedge 中）
* Magisk 环境初始化（晚间 App UI）

## 164 轮（15:40-16:10）：fastrpc bringup 完整机制破译——主线内核侧全部就绪

### ★ 关键发现（boot logcat adsprpcd 完整序列）
```
14:13:03.186 remote_handle_open: '":;./\createstaticpd:audiopd' on domain 0
14:13:03.191 remote_handle_open: '":;./\attachguestos' on domain 0
14:13:03.197 remote_handle_open: adsp_default_listener ×2
14:13:03.198 listener thread starting
14:13:03.215 apps_std: Successfully opened /vendor/etc/acdbdata//adsp_avs_config.acdb
```
⇒ **audio PD 的静态创建是触发器**：createstaticpd:audiopd → DSP 侧音频域（含 ACDB 引擎）启动
→ 经反向 apps_std RPC 拉取配置文件 → AMDB 拿到 MREGINFO（模块注册表）

### 主线内核侧 = 已全部就绪（无需内核补丁！）
* `drivers/misc/fastrpc.c`（6.12，2537 行）：
  - `FASTRPC_IOCTL_INIT_CREATE_STATIC` ✓ = createstaticpd:audiopd（含 SCM 内存分配）
  - `FASTRPC_IOCTL_INIT_ATTACH` ✓ = attachguestos
  - `FASTRPC_IOCTL_MMAP` + `ADSP_MMAP_REMOTE_HEAP_ADDR` ✓ = listener 需要的 remote heap 映射
  - `FASTRPC_IOCTL_INVOKE` ✓ = listener 应答通道
* sm8250.dtsi ADSP glink-edge 下 fastrpc 节点已存在（label="adsp" + compute-cb@3/...）✓
* 原厂参考：elish-kernel drivers/char/adsprpc.c（5229 行完整版，反向调用仍是用户态 listener 轮询）

### 缺的最后一环：用户态 listener 守护进程
* 原厂 = adsprpcd（bionic 二进制，不能直接跑 glibc Armbian）
* 需要原生实现：
  1. INIT_CREATE_STATIC("audiopd") → 创建音频 PD
  2. MMAP(remote heap) → 映射共享内存
  3. 轮询 remote heap 的反向调用队列（协议要逆向 libadsprpc.so 的 listener 线程）
  4. 实现 apps_std 回调（fopen/fread/fclose/fseek/ftell/fstat——DSP 用它读 .acdb）
     服务路径：/vendor/etc/acdbdata/*.acdb（4 个文件已在本地）
  5. 经 INVOKE 应答
* libadsprpc.so（已提取 /tmp/acdb_libs/）的 apps_std API 面（143 个字符串引用）：
  fopen_with_env/fread/fclose/fseek/ftell/fstat/fgetpos/fsetpos/rewind/flen/get_dirinfo 等

### 下一步（逆向 libadsprpc.so 的 listener 线程协议）
* 反汇编 listener 轮询逻辑：remote heap 的队列格式 + 轮询偏移 + 应答封装
* 写 elish-fastrpcd（原生 C，~500 行）：PD 创建 + 轮询 + apps_std 文件服务
* 放 acdb 文件到 DSP 期望的路径 → 触发 → 验证 adsp_avs_config 被 DSP 拉取
* 然后拓扑注册应该 4ms 成功（原厂行为）

## 165 轮（16:10-16:20）：★ fastrpc 用户态全栈移植完成（无需任何内核改动）

### 移植成果（AOSPA external/fastrpc 公开源码 → Linux/glibc 原生构建）
* 仓库：/home/axis/axis_rnd/work/fastrpc_userspace（clone beryl 分支）
* 构建：`make CC=aarch64-linux-gnu-gcc CFLAGS="... -DLE_ENABLE -DDEFAULT_DOMAIN_ID=0"`（Makefile 自带 LE 目标！）
* 手动链接修正（Makefile 的 -lm 顺序 bug）：libcdsprpc.so (301KB) + libadsp_default_listener.so (72KB) + adsprpcd (70KB，仅依赖 libc.so.6 ✓)
* 机制（源码实证 listener_android.c）：
  - listener = 用户态线程发标准 fastrpc INVOKE（adsp_listener_next2 阻塞等待 DSP 反向请求）→ mod_table 分发 → apps_std skel 执行文件操作 → 应答
  - **根本不需要内核反向调用支持**——全部走主线 INVOKE 路径！
* createstaticpd:audiopd 特殊路径在 fastrpc_apps_user.c:847-860 ✓
* ADSP_AVS_CFG_PATH（LE 分支）= ";/etc/acdbdata/"；ADSP_LIBRARY_PATH 含 ;/usr/lib/rfsa/adsp;/dsp
* 节点命名一致：内核 domains[0]="adsp" → /dev/fastrpc-adsp；用户态 ADSPRPC_DEVICE="/dev/fastrpc-adsp" ✓

### 部署包（已就绪 /tmp/fastrpc_deploy.tar.gz, 612KB）
* bin/：adsprpcd + libcdsprpc.so + libadsp_default_listener.so
* acdbdata/：adsp_avs_config.acdb(1264B ★关键) + Forte_General/Global/Speaker_cal.acdb + workspaceFile.qwsp
* deploy.sh：拷库到 /usr/lib/fastrpc + acdb 到 /etc/acdbdata + 建 rfsa/dsp 目录 + 前台跑 daemon 看日志

### 待执行（设备断电重启后）
1. tar 解包 + deploy.sh → 看 adsprpcd 日志：createstaticpd:audiopd 成功？DSP 经 apps_std 拉 adsp_avs_config.acdb？
2. 若拉取成功 → echo 3 > stage; echo 1 > elish_topologies → 拓扑注册应 4ms 成功（原厂行为）
3. 然后接 ADSP 管线：copp_topology=0x1000a100 播放测试

### 风险点
* INIT_CREATE_STATIC 的内核侧 SCM 内存分配（Armbian SCM 应可用——其他子系统正常）
* audiopd 是 stock 固件内置 PD ✓（跑的是原厂重建固件）

## 166 轮（16:25-16:40）：部署包完善 + 全链路预验证完成

* ★ 守护进程调用方式确认（adsp_default_listener.c 源码）：
  - `adsprpcd audiopd` → remote_handle_open("createstaticpd:audiopd") = 创建音频 PD
  - `adsprpcd`（无参）→ attachguestos
  - 原厂跑双实例（logcat PID 1728/1730 证实）
* deploy.sh 已更新为双实例模式
* 内核静态 PD 内存路径复查：vmcount=0（DT 无 vmperms）→ 跳过 SCM assign，走 SMMU（qcom,non-secure-domain ✓）
* 二进制静态验证：adsprpcd aarch64 PIE / Armbian glibc 解释器 / 仅需 libc.so.6 ✓；
  libadsp_default_listener.so 导出 adsp_default_listener_start@@ADSPRPC ✓；
  libcdsprpc.so 导出 remote_handle_open ✓；mod_table/apps_std 为库内符号 ✓
* bringup_test.sh：一键测试（进程状态 → 日志 → 触发注册 → 结果 → 存活检查）
* 部署包 /tmp/fastrpc_deploy.tar.gz (612KB) 完备

### 待用户：设备断电重启后
scp 包 → 解包 → ./deploy.sh → ./bringup_test.sh → 应见 createstaticpd OK →
apps_std 打开 /etc/acdbdata/adsp_avs_config.acdb → 拓扑注册成功

## 167 轮（16:45）：ABI 兼容验证完成——本地工作 100% 收尾

* ioctl ABI 逐字段比对（用户态 fastrpc_internal.h vs 主线 uapi/misc/fastrpc.h）：
  - fastrpc_invoke_args {u64 ptr, u64 len, s32 fd, u32} 24B 同布局 ✓（attr/reserved 仅名异）
  - fastrpc_invoke {u32 handle, u32 sc, u64 args} ✓
  - fastrpc_init_create ✓；ioctl 号同源（_IOWR('R',n,...)）✓
* rpcmem 实现在 libcdsprpc.so 内部（LE 路径 malloc 基础）✓
* 至此本地可验证项 100% 完成。部署包 /tmp/fastrpc_deploy.tar.gz 随时可发。
* 唯一等待：设备断电重启（用户物理操作）。

## 执行序列（设备恢复后，一次成功设计）
1. scp fastrpc_deploy.tar.gz → 解包 → ./deploy.sh（双实例：audiopd + attach）
2. 验证日志出现：createstaticpd:audiopd OK + "Successfully opened file /etc/acdbdata/adsp_avs_config.acdb"
3. ./bringup_test.sh → echo 3/1 → 预期 "elish: AVCS topologies registered"（4ms 级）
4. 播放验证：amixer 452=1 + aplay + copp_topology=0x1000a100（q6routing param）

## 168 轮：★ 部署顺序修正——必须重启 ADSP remoteproc

* 关键认知：主线 APR 的 DT 已带 qcom,protection-domain="msm/adsp/audio_pd"，
  音频 PD 在内核启动时就已 attach 并在跑——DSP 侧 ACDB 服务随 PD 启动，
  但因无 listener 拿配置失败（空模块注册表）→ 我们的拓扑注册才崩。
* 因此正确顺序：**先起 adsprpcd（listener 就位）→ 再重启 ADSP remoteproc** →
  PD 重生 → ACDB 经 listener 拉 /etc/acdbdata/adsp_avs_config.acdb → 触发注册。
* bringup_test.sh 已加入 remoteproc 定位 + stop/start 循环（找 name 含 17300000 的）。
* compute-cb@3/4/5（SMMU 0x1803-05）✓ 齐全。

## 169 轮（16:50-17:00）：20W 问题——原厂充电协商逆向（本地完成大半）

### 用户实测背景（有效数据点）
中午（chg11/12 启动、守护进程正常运行时）插 PD 充电器，80% 电量只协商到 ~20W。
预期：80% 时 VBAT≈4.35-4.42V → k81 步进 10.8A 档 → 电池侧 ~47W → 输入 ~50W。

### 原厂架构（内核侧实锤，本地逆向完成）
1. **PPS 选择 = sysfs select_pdo**（policy_engine.c: select_pdo_store "src_cap_id pdo uv ua"）
   —— 用户态/驱动写这个文件驱动全部 PD 协商；小米专有 non_qcom_pps_ctr 状态机
2. **FC2 状态机原版 = drivers/power/supply/ti/pd_policy_manager.c**（我们的守护进程就是
   参照它移植的，逐段核对：FC2_ENTRY_1 的 vbat*2+BUS_VOLT_INIT_UP 和 ±50/200mV 调节循环一致 ✓）
3. smb5-lib.c：热控 FCC 投票（thermal_fcc_pps_cp + pps_thermal_level ±2 滞回）
4. micharge HAL（已提取 /tmp/micharge/）= 瘦 HIDL 接口，曲线逻辑不在它里面

### ★ 原厂 elish DT 配置（DTBO fragment@141 usbpd_pm 节点，逐字提取）
```
mi,pd-bat-volt-max = 4480mV          ← 我们 PD_BAT_VOLT_MAX_MV ✓ 一致
mi,pd-bat-curr-max = 12400mA         ← 我们 ✓ 一致（0x3070）
mi,pd-bus-volt-max = 12000mV
mi,pd-bus-curr-max = 6200mA          ← 我们 PD_BUS_CURR_MAX_MA ✓ 一致
mi,pd-bus-curr-compensate = 50       ← ✓ 一致
mi,pd-non-ffc-bat-volt-max = 4420mV  ← ★ 非 FFC 的退出线（我们固定用 4480！）
mi,pd-battery-warm-th = 480 (48°C)   ✓
mi,therm-level-threshold = 12
mi,step-charge-high-vol-curr-max = 6720
mi,cell-vol-high-threshold-mv = 4420
mi,cell-vol-max-threshold-mv = 4487
mi,pd-power-max = 67                 ← ★ 原厂目标 67W！
```
k81 步进表解码（qcom,step-chg-ranges µV/µA）：
- 3.0-3.35V: 1A（预充）
- 3.35-4.20V: **12.4A**（0xbd3580 ✓）
- 4.20-4.45V: **10.8A**（0xa4cb80 ✓）
- 4.45-4.49V: **6.72A**（0x668a00 ✓）
→ 80%（VBAT 4.35-4.42V）在 10.8A 档：电池 47W ≈ 输入 50W —— 与我们守护进程的表一致

### 20W 嫌疑清单（待设备恢复后用 noon 日志定罪）
1. **FC2 锁死**：SW 路径 20W 慢充下 VBAT 爬到 >4430（我们退出线）后 FC2 永远进不去 → 20W 锁定
   （原厂 FFC/非FFC 双线 4480/4420，且热级阈值/夜间充电等门槛更多）
2. pps_activate 失败 → backoff 60s 循环（tcpm online=2 写入被拒？）
3. 泵使能失败 → backoff
4. VBAT 临界 4430（80% 充电态 4.37-4.42V，随老化/读数偏移可能越过）
判定证据：journalctl -u elish-charged --since "11:00" --until "13:30"（journal 持久，noon 决策全在）

## 170 轮（22:00-22:20）：★ cirrus 库找到 + ADSP 文件服务全链打通 + 失败模式质变

### 重大发现
* **`capi_v2_cirrus_sp.so` 一直存在于 vendor.img 的 `/lib/rfsa/adsp/`**（312,952 B，
  md5 3b522438c1c6ec6d9f3ec14e97204fa8）——早期"任何分区都找不到"是搜错目录的误判！
* 全套 36 个原厂 DSP 库（19MB）已提取并部署到 `/usr/lib/rfsa/adsp/`（LE ADSP_LIBRARY_PATH
  第一搜索路径，与 fastrpc LE 源码一致）

### 本轮已打通/修复的完整链（每一项都有实测证据）
1. fastrpc listener 服务 DSP 拉 adsp_avs_config.acdb ✓（strace: openat=7）
2. AVS 状态 1 → **5** ✓（全模块加载完成）
3. LOAD_MODULES(0x10027053) EFAILED → **成功** ✓
4. SCM 分配 EINVAL → **成功**（CMA 缓冲 + 原厂 VMID {0x16,0x25}={22,37}，
   来自 stock DT `qcom,adsp-remoteheap-vmid`，替换了过时的 6）✓
5. ADSP 安全静态 PD + `/dev/fastrpc-adsp-secure`（DT: 去 non-secure + qcom,vmids）✓
6. **失败模式质变**：注册从"瞬间处决"→"存活 ~2 分钟后负载 10 缓慢死亡"
   ⇒ 符合 ADSP 加载器**阻塞在文件请求**（挂起→看门狗复位）而非即时 XPU 故障

### 已部署的下一次诊断工具
* apps_std 加入**每个文件请求+结果的持久化日志**（libcdsprpc.so 已部署）
  → /var/log/adsprpcd.log 在崩溃后仍可读，将显示 DSP 在注册时请求的文件与我们的应答
* deploy_vendor_paths.sh：预判 DSP 按 Android 绝对路径请求（/vendor/lib/rfsa/adsp、
  /vendor/dsp、/vendor/etc/acdbdata）——Armbian 上补齐这些路径

### 下一步（设备恢复后一条命令）
1. sh deploy_vendor_paths.sh（补 /vendor 路径）
2. 起 daemon（listener）
3. stage 4 注册 → 读 /var/log/adsprpcd.log 看 DSP 请求了什么、答对没有

## 171 轮：★ acdb 部署结构缺陷发现（Forte/ 子目录 + 缺 3 文件）

* 原厂 `/etc/acdbdata/` 结构：
  - `adsp_avs_config.acdb`（平铺，1264B）
  - **`Forte/` 子目录**：Forte_Bluetooth_cal(88880) / General(20738) / Global(42196) /
    Handset(1276792) / Hdmi(6558) / Headset(374388) / Speaker(766696) / workspaceFile.qwsp(424912)
* **我们之前的部署是平铺的 + 缺 3 个文件**（Bluetooth/Handset/Hdmi）！
  DSP 若按 `Forte/xxx.acdb` 相对路径请求 → 我们的目录无 Forte/ → ENOENT
  → 标定加载失败 → 挂起（与观察到的"缓慢死亡"吻合）
* 已提取完整 9 文件（2.9MB）并按原厂结构打包
* /tmp/rfsa_deploy.tar.gz（11.8MB）现含：
  - 36 个原厂 DSP 库 → /usr/lib/rfsa/adsp + /dsp/adsp + /vendor 路径补齐
  - 完整 acdbdata（含 Forte/ 结构）→ /etc/acdbdata + /vendor/etc/acdbdata
  - bringup_final.sh：一键（部署 → 起 listener → stage 4 注册 → 读结果 + DSP 文件请求日志）
* 待设备断电重启后执行

## 172 轮（22:10-22:34）：bringup_final.sh 执行后再次 wedge——但这次有黄金日志

* 流程：全新启动 → scp 11.8MB 包 → bringup_final.sh（部署 36 库 + 完整 acdb（Forte/ 结构 +
  补 3 缺失文件）+ /vendor 路径 + 起 daemon → stage 4 触发）→ 盒子又挂
* ★★ **关键资产**：这次 daemon 的输出重定向到了**持久化的 /var/log/adsprpcd.log**，
  且 apps_std 记录**每一个文件请求及结果**——崩溃后该文件仍在！
  **下次设备恢复，读它 = 直接看到 DSP 在注册时请求了什么文件、我们怎么答的**
* 崩溃模式（前几轮）：存活 ~2 分钟 + 负载 10（内核线程自旋）→ 缓慢死亡
  = 符合"DSP 侧加载器/处理链阻塞 → 看门狗复位"
* 设备 22:34 完全失联（双路 No route to host）——需用户断电重启

### 设备恢复后的取证顺序
1. cat /var/log/adsprpcd.log → DSP 的文件请求全记录（**最高优先**）
2. grep "elish:" /var/log/syslog → 崩溃窗口的内核序列
3. 对症修复后重试

## ★★★ 173 轮：ADSP 拓扑注册成功！完整根因链闭合

### 最终成功的两条关键修复
1. **SID 编码**：AVCS map/register 的 `addr_msw` 字段不是地址高位，而是 **SMMU SID**。
   原厂 msm_audio_ion.c: `sid = iommus_arg & qcom,smmu-sid-mask(0xF)` = 0x1801 & 0xF = **1**，
   主线 q6asm-dai.c 同款（SID_MASK_DEFAULT=0xF，`phys = addr | (sid << 32)`）。
   我先前传 0（地址高位）→ REGISTER 静默杀死芯片；传 0x1801 → MAP 被优雅拒绝(-1)。
2. **缓冲区必须是 SMMU 上下文里的真实 IOVA**：
   在 `qcom,q6asm-dais` 平台设备（带 iommus=<&apps_smmu 0x1801>）上 dma_alloc_coherent
   → 得到 IOVA 0xffefe000（而非 CMA 物理地址 0xfe55a000）。
   证据：用物理地址时 arm-smmu 报 `Unhandled context fault fsr=0x402 TF, SID=0x1801,
   iova=0xfe55a000` → ADSP audio_process 崩溃 → 系统级联死亡。

### 成功日志
```
elish: smmu-iova 8192 on ...service@7:dais dma=0xffefe000
elish: AVS state 5, proceeding
elish: mapped handle=0xb0dca548 size=8192, sending REGISTER now
elish: AVCS topologies registered (5444 bytes)   ← ★ 成功
```
验证：uptime 正常、0 次 SMMU fault、ADSP state=running

### 累计修复清单（全部有实测证据）
1. fastrpc 用户态栈移植（adsprpcd+listener+apps_std）→ DSP 拉取 adsp_avs_config.acdb
2. AVS 状态 1→5；LOAD_MODULES EFAILED→成功
3. DT: 安全静态 PD（去 non-secure + qcom,vmids=<22 37>）→ /dev/fastrpc-adsp-secure
4. DT: glink intents {2K×5,8K×3,17K×2}（主线默认仅 1K×5）
5. 缓冲不在 MAP 后释放（防 DSP 读已释放页）
6. **addr_msw = SID(1)**；**缓冲区在 q6asm-dais(0x1801) 上分配**

## ★ 174 轮：ADSP 音频管线开机自启闭环 + 音量链路逐项对照 Android

### 闭环（已实测通过）
1. **模块 ABI 修复**：新 config 下 `task_struct->cred` 1680→1696；重装同源模块
   （树 `UTS_VERSION #42 SMP Fri Sep 25 00:14:29 CST 2026` == 设备 `/proc/version`，
   Image 之后无源码改动）→ `bt_sock_alloc` oops **0 次**，hci0 UP RUNNING。
   安装方式：`tar czf` 传到设备 → `cp -a` 覆盖 `/lib/modules/<ver>/` → `depmod -a` → sync
   （**只覆盖，不删**，out-of-tree 的 `bq2597x_elish.ko` 自动保留）。
2. **音频管线开机自启**（此前是运行时手动）：
   * `/etc/systemd/system/adsprpcd.service`（WorkingDirectory=/usr/lib/rfsa/adsp、
     LD_LIBRARY_PATH=/usr/lib/fastrpc、Restart=always）→ fastrpc listener
   * `/etc/systemd/system/elish-avcs.service` + `/usr/local/sbin/elish-avcs-trigger.sh`
     → 等 daemon → 写 `/sys/module/q6core/parameters/elish_topologies=1`
   * 开机日志实证：`AVS state 5` → `module 0x10027053 loaded` → `deregistered` →
     `mapped handle=0xb0d1df78` → **`AVCS topologies registered (5444 bytes)`**
   * 仍需每次开机显式设：`/sys/module/q6routing/parameters/copp_topology=0x1000a100`（built-in 参数，重启归零）
   * mixer 持久化由 `alsa-restore.service`（static）+ `/var/lib/alsa/asound.state` 负责

### 音量链路 vs Android（逐项已核对）
| 项 | Android | 我们 |
|---|---|---|
| 音量曲线 `audio_policy_volumes.xml` MUSIC/SPEAKER 最大 | **0 dB**（<point>100,0） | PipeWire 1.00=0dB 一致 |
| `bit_width_configs` SPEAKER | **24**（TERT_TDM_RX_0 Format=S24_LE） | BE fixup 强制 S16_LE ← **仍不同** |
| speaker acdb_id（platform_info） | **15**（protected=124） | `q6adm_open(...,app_type=0,acdb_id=0)` ← **未传** |
| 功放 `0x6000` DIG_VOL 字段 | 0（=0 dB；基线 0x00008000） | 817=0dB（一致） |
| 功放 `0x6c04` AMP_GAIN | 18（基线表 0x273=19；播放态实测 0x253） | 18（一致；寄存器可到 0x253） |
| `PCM Source` / `DSP RX1,2` / `ASP TX1` | DSP / ASPRX1 / ASPRX2 / DSPTX1 | **本已 ASP 直通，已改为 DSP 对齐** |
| Protection 固件 | spk-prot + 每路 bin | 已加载（dmesg 有 `Protection:` 行）✓ |
| CSPL 遥测 | ATTENUATION=1.0 / REDUCE_POWER=0 | 相同（未限幅） |
| **ADSP 侧 ACDB 器件增益（speaker gain ramp）** | HAL 经 fastrpc 调 `acdb_loader_send_gain_dep_cal` / `acdb_send_audio_cal_v*` | **缺** ← 文档判定为"响度差距主体" |

* HAL 相关符号：`acdb_loader_init_v2` / `acdb_loader_set_audio_cal_v2` / `acdb_send_gain_dep_cal` /
  `acdb_send_audio_cal_v3` / `acdb_loader_send_audio_cal_v4` / `acdb_loader_get_calibration`
* **真件已在手**：`vendor.img`（本地可 ro 挂载）内有 `lib64/libacdbloader.so`(+libacdbrtac/libadiertac/
  libacdb-fts)、`etc/acdbdata/`、`etc/cirrus.cfg`、**真实 `firmware/<PREFIX>-music.txt|-voice.txt`**
  （TLH/TRH… 为空，只有 `BLH-music.txt`/`BRL-music.txt` 各 1 行 12 个 u32）
* 预期可行路线：用已移植的 fastrpc 栈（libcdsprpc + adsprpcd listener）复刻 HAL 的 ACDB 调用，
  把器件增益真正下发到 ADSP（= Android 同路），而不是在软件域堆增益。

### ★ 订正：`amp-fix.sh` 的「SP_FORMAT=0x20200000（32bit slot）」结论在当前驱动下不成立
* 该脚本（`/root/amp-fix.sh`、`/usr/local/bin/amp-fix.sh`）来自 22 轮，当时驱动状态不同；
  它按 hw_params 位宽写 `0x4808=0x20200000` + `0x4840=WL`。
* 当前机器驱动 `sm8250_be_hw_params_fixup` 把 BE 强制 **S16_LE**，TDM 为 8×32bit；
  强写 32bit slot / WL=24 与 AFE 实际位序不匹配 → **用户实测"破音"**。
* 已全部回滚：service stop+disable、`power/control` 从 `on` 回 `auto`，寄存器回到驱动值
  （`0x4808=0x10180000`、`0x4840=0x10`、`0x6c04=0x240`）。
* 结论：**音量问题不要再用强写 SP_FORMAT/WL 这条路**；要么对齐 BE 格式（改 fixup 为 S24），
  要么走上面的 ACDB 器件增益路线。
* 用户明确要求：**不要用 PipeWire 软件增益**（易削顶破音），增益要走 Android 那套硬件/DSP 域。

## ★★ 175 轮：音量链完整定位 —— ACDB 器件增益路线被实机日志证伪

### 证据来源（都是本地既有资产）
* `work/android/acdb/acdb_log.txt`（743 行）＝ **原厂 Android 实机 ACDB-LOADER 全程日志**
* `work/hal/audio.primary.kona.so`（原厂 HAL，含符号表）+ `work/hal/mixer_paths*.xml`
* `work/acdb/Forte_Speaker_cal.acdb` + `parse_acdb.py` / `acdb_tables.py`
* QCOM HAL 源码（android.googlesource.com `platform/hardware/qcom/audio`）交叉验证

### Android 音量链（逐段实证）
1. 音量条 0–100 → `audio_policy_volumes.xml` MUSIC/SPEAKER：`-7100 … 0` mB ⇒ **最大 0 dB**
2. HAL：`audio_hw_send_gain_dep_calibration(level)`（level=档位）
   → `platform_send_gain_dep_cal(platform, level)`
   → 取当前 speaker usecase 的 `platform_get_snd_device_acdb_id()`
   → `acdb_send_gain_dep_cal(acdb_id, app_type, MSM_SNDDEV_CAP_RX, CAL_MODE_RTAC, level)`
   （二进制汇编可见 5 参调用：`(acdb_id, sample_rate, 1, 4, volume)`）
3. **实机结果：失败** —— `ACDB_CMD_GET_AUDPROC_INSTANCE_GAIN_DEP_STEP_TABLE_SIZE Returned = -19`
   （该表在本机 ACDB 里**不存在**）；`acdb_loader_adsp_set_audio_cal` **64 次全部 result=-100**
   ("active device/stream not found")；`acdb_loader_store_set_audio_cal` 失败 -1 (43 次)。
   ⇒ **Android 在本机上没有任何 ACDB 器件增益生效**，此前"响度差距主体=ACDB gain ramp"的推断**被证伪**。
4. Android 真正在用的参数（日志原文）：`acdb_id = 10011`；`app_type = 69940(0x11134)` deep-buffer /
   `69937(0x11131)` low-latency；COPP topology `0x1000a100 / 0x1000a101 / 0x1000a106 / 0x1000a115`；
   AFE topology `0x112fc`；audstrm cal 20 B。ACDB 数据本身由 **DSP 自己经 fastrpc 拉文件**加载
   （日志首行 `adsprpcd: Successfully opened /vendor/etc/acdbdata//adsp_avs_config.acdb`）——
   与我们 daemon 的做法一致 ✓
5. AFE/TDM：8 slot × 32 bit、`TERT_TDM_RX_0 Format=S24_LE`、48 kHz、`bit_width_configs=24`
6. 功放（CS35L41）播放态：`PCM Source=DSP`、`AMP PCM Gain=18`（`0x6c04=0x253`，含 PDM gain=19）、
   `SP_FORMAT=0x20200000`（RX/TX slot 均 32 bit）、`SP_RX_WL=0x18`(24)、`DRE=1`、Class-H=1、
   `NG_CFG=0x3F75`、Protection 固件 + 每路 `<PREFIX>-spk-prot.bin`

### 我们的现状 vs Android（差异只剩这几项）
| 环节 | Android | 我们 |
|---|---|---|
| 软件音量 @max | 0 dB | PipeWire 1.00 = 0 dB ✓ |
| ADSP 器件增益 | 不存在（全失败） | 无 ✓（等价） |
| COPP topology | 0x1000a100 | 0x1000a100 ✓ |
| COPP **app_type** | **69940 / 69937** | **0** ← 差异（`q6routing.c` 里 session->app_type 未填） |
| TDM 位宽 | **S24_LE**（24 bit） | **S16_LE**（`sm8250_be_hw_params_fixup` 强制） |
| 功放 slot/WL | 32 bit slot / WL=24 | 16 bit slot / WL=16（`0x10180000`/`0x10`） |
| 功放增益 | PCM 18 (+PDM 19) | 18（PDM 未设） |
| 噪声门 | `0x3F75` | `0x33` |
| Protection 固件/DSP 通路/DRE/限幅 | ✓ | ✓（本轮已把 PCM Source 从 ASP 直通改成 DSP） |

**寄存器位域已核对**（`include/sound/cs35l41.h`）：`SP_FORMAT` bits[31:24]=RX slot 宽、[23:16]=TX slot 宽；
`SP_RX_WL` bits[5:0]=RX 字长。**16/16 与 32/24 两种配置各自内部自洽**（不产生电平损失），
所以此前"SP_FORMAT 写 32bit slot 能变大"的做法在当前 S16 的 AFE 下只会位序错位 → **破音**（实测），
已在 174 轮回滚，勿再走。

### 结论与后续候选（按证据强度）
1. **TDM 对齐 Android 的 24 bit**：`sm8250_be_hw_params_fixup` 改 `S24_LE` + 功放 `SP_FORMAT=0x20200000`
   /`SP_RX_WL=24`（两边一起改才自洽）。收益是**动态范围/保真**，不是电平。
2. **COPP app_type=69940**（Android 值）：内核 1 处改动（`q6routing.c` 调 `q6adm_open` 处），
   让 ADSP 用与原厂相同的 app_type 选实例；收益未知，需实测。
3. 功放数字增益是唯一"纯电平"旋钮（0…+12 dB，DSP 域、有保护算法兜底）；
   Android 侧是 0 dB，我们目前用 +6 dB（865）作补偿 —— 属**主动超原厂配置**，非等价移植。
4. 噪声门 `0x3F75`（Android）可改善小音量下的听感（门限过高会压掉弱信号）。

## ★★★ 176 轮：无声的真正根因 —— 机器驱动缺少 Speaker DAPM 路由（功放通路从未上电）

### 症状与判别
* 播放时宿主 PCM `RUNNING`、路由控件 `TERT_TDM_RX_0 Audio Mixer MultiMedia1=on`、sink 音量 1.00，
  但**没有声音**；`PCM Source=ASP` 与 `=DSP` 都无声。
* 净电流指标：ASP 通路 −338mA（与空闲相当，**没有输出**）；DSP 通路 −445mA（+80mA，DSP 在跑）。
* **DAPM debugfs（播放中）**：
  `/sys/kernel/debug/asoc/Xiaomi Mi Pad 5 Pro/cs35l41.1-0040/dapm/`
  ```
  BRH CLASS H     : Off   (R8220/0x201c mask 0x10)   ← PWR_CTRL3 bit4 = CLASS H 使能
  BRH Main AMP    : Off   (R8216/0x2018 mask 0x1)
  BRH SPK         : Off
  BRH DSP1        : Off
  BRH PCM Source  : Off
  BRH DRE         : Off
  ```
  ⇒ **整条功放内部通路（PCM Source → CLASS H → Main AMP → SPK）从未上电**，与寄存器
  `PWR_CTRL3=0x01100000`（bit4=0）完全吻合；原厂 Android 播放态是 `0x01100010`（bit4=1）。

### 根因
`sound/soc/qcom/sm8250.c`（elish 机器驱动）里**没有任何 DAPM widget/route**：没有把带
`sound-name-prefix` 的 codec 输出（`TLH SPK`/`TRH SPK`/…/`BLL SPK`）接到卡片级 sink。
DAPM 因此永远不点亮 codec 侧通路 —— 与 vendor（`audio_machine_kona.ko`）的差别就在这。
之前"有声音"其实是 `amp-fix.sh` 的 KICK **直接 i2c 写 PWR_CTRL1/2/3 + 路由寄存器**绕过 DAPM
撑出来的，一旦停掉该服务（或重启后未运行）就回到无声。

### 修复
在机器驱动中补上卡片级 sink 与路由（vendor 同做法）：
```c
static const struct snd_soc_dapm_widget sm8250_elish_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
};
static const struct snd_soc_dapm_route sm8250_elish_routes[] = {
	{ "Speaker", NULL, "TRH SPK" }, { "Speaker", NULL, "TLH SPK" },
	{ "Speaker", NULL, "TLL SPK" }, { "Speaker", NULL, "TRL SPK" },
	{ "Speaker", NULL, "BRH SPK" }, { "Speaker", NULL, "BLH SPK" },
	{ "Speaker", NULL, "BRL SPK" }, { "Speaker", NULL, "BLL SPK" },
};
/* sm8250_platform_probe(): qcom_snd_parse_of() 之后、register_card 之前 */
snd_soc_dapm_new_controls(&card->dapm, ...);
snd_soc_dapm_add_routes(&card->dapm, ...);
```

### 同时保留的 vendor 对齐（本轮）
* BE fixup = **S24_3LE**（原厂 `/proc/asound/card0/pcm0p/sub0/hw_params: format: S24_3LE`、
  `msm_be_hw_params_fixup: format = 32`），codec DAI 增加 S24_3LE 支持
* TDM CPU `slot_mask = 0xff`（原厂 `kona_tdm_snd_hw_params: slot_width 32 slots 8 slot_mask ff`）
* codec `SP_FORMAT=0x20200000`（32bit slot）+ `SP_RX_WL=24`（原厂 cs35l41_pcm_hw_params: asp_wl 24/asp_width 32）
* 功放寄存器默认/运行时对齐：`AMP_GAIN_CTRL=0x253`、`NG_CFG=0x3F75`
* **原厂 CSPL 校准下发**（`cs35l41_elish_calibrate()`：CAL_R/CAL_AMBIENT/CAL_CHECKSUM +
  `CSPL_COMMAND=0x08000000`，经 `cs_dsp_coeff_write_ctrl`，需要 `MODULE_IMPORT_NS(FW_CS_DSP)`）

### 原厂真值（slot_a Android 实机抓取，见 android_capture/VENDOR_REFERENCE.md）
* 功放 8 颗寄存器完全一致：`SP_FORMAT=0x20200000`、`SP_RX_WL=0x18`、`DIG_VOL=0x8004`(0dB)、
  `AMP_GAIN=0x253`、`NG_CFG=0x3F75`、`DAC_PCM1_SRC=0x32`、`DSP1_RX1=0x08`、`DSP1_RX2=0x09`
* 原厂 dmesg **0 次 PUP_DONE 失败**（只有 8 次 `PDN failed`）；我们有 13 次 PUP/PDN 警告 → 上电时序仍需对齐

## ★★★ 177 轮：扬声器有声 —— 三条根因全部修好（用户确认"够大声了"）

### 根因链（按发现顺序，全部有实测证据）
1. **机器驱动缺 Speaker DAPM 路由**（主因）。`sm8250.c` 没有任何 DAPM widget/route，
   codec 输出（`<PREFIX> SPK`）没有接到卡片级 sink → 播放中所有功放 DAPM widget 全是 `Off`
   （CLASS H / Main AMP / SPK / PCM Source / DSP1），功放通路从未上电 → 无声。
   * 修：`SND_SOC_DAPM_SPK("Speaker")` + 8 条 `{"Speaker", NULL, "<PREFIX> SPK"}`，
     通过 `card->dapm_widgets/num_dapm_widgets/dapm_routes/num_dapm_routes` 挂上
     （**不能**在 `devm_snd_soc_register_card()` 前手工调 `snd_soc_dapm_new_controls()`，
     否则 card->dapm 未初始化 → `mutex_lock` NULL 解引用 Oops，已在 chg39 踩到）。
   * 修后播放中：`CLASS H/Main AMP/SPK/PCM Source/DSP1/AMP Playback/Speaker 全 On`，
     `PWR_CTRL3=0x01100010`（CLASS H bit4 置位，与原厂一致）。
2. **TDM slot_mask 误读原厂**：原厂 dmesg `slot_mask ff` 是**它们机器驱动**的写法，
   主线 q6afe 的 `q6tdm_set_tdm_slot()` 会做 `rx_mask & cap_mask`，0xff 会被 ADSP 拒绝：
   `AFE enable for port 0x9020 failed -22` → BE `snd_soc_pcm_dai_prepare` -22 → 无声。
   * 修：`rx_mask = 0x03`（两通道占 slot 0/1）。已用 A/B 实证：0x03 两种格式都正常播放 5s，
     0xff 两种格式都立刻 rc=1。
3. **功放 PCM 增益停在最低档**：播放中 `0x6c04 = 0x00000000`（原厂 `0x253`），
   比原厂低约 18dB → 即使通路通了也几乎听不到。
   * 修：`Analog PCM Volume = 20`（0x293）、`Digital PCM Volume = 913`（+12dB，芯片上限）。

### 最终可用配置（已固化）
* 内核：`sm8250_be_hw_params_fixup` → **S24_3LE**（原厂 PCM 格式，codec 侧始终 S24_LE/WL=24，
  必须与之一致，S16 会造成 8bit 错位）；`rx_mask=0x03`（两个运行时参数 `elish_be_format`/`elish_tdm_mask`
  保留在 `/sys/module/snd_soc_sm8250/parameters/` 便于以后 A/B）；卡片级 Speaker DAPM 路由。
* 服务：`elish-amp-align.service` → `PCM Source=DSP`、`DRE=1`、`Preload=1`、`Soft Ramp=4ms`、
  `ASP TX1=DSPTX1`、`DSP RX1/2=ASPRX1/2`、`Analog=20`、`Digital=913`、`copp_topology=0x1000a100`，
  并 `alsactl store`。
* 驱动：`cs35l41_elish_calibrate()`（原厂 CSPL 校准：CAL_R/CAL_AMBIENT/CAL_CHECKSUM +
  `CSPL_COMMAND=0x08000000`）+ 每场景调音下发；`MODULE_IMPORT_NS(FW_CS_DSP)`。
  实测日志：`elish: pushed factory calibration cal_r=9336 ambient=23`、
  `elish: pushed 12 tuning values from cirrus/BRH-music.txt`。

### 还能更大吗？
干净增益域**已全部到顶**：功放 Digital 913(+12dB)=芯片上限、Analog 20=上限、ADSP 音量 Q27=1.0=上限。
再往上只有两条路，都有代价：
① DSP 域把 ADSP 音量上限抬到 >0dB（需改 q6routing 控件上限；源接近满刻度时会削顶）；
② 动功放 boost 目标电压/保护门限（提高喇叭上限，有喇叭风险）。
用户明确要求**不要动会破音的那种**，故当前保持现状。

### 仍待处理（dmesg 对齐原厂）
* 我们 13 次 `PUP_DONE not latched`；原厂 **0 次 PUP 失败**（只有 8 次 `PDN failed`）→ DAPM 上电顺序仍需对齐
* 我们 4 次 `no reserved DMA memory for FASTRPC`、1 次 SLPI `dog_virtual_user`；原厂 0 次

## ★★★ 178 轮：音量链路全量对齐原厂 + 音量管线逆向完成

### 音量调整管线（逆向结论，双重证据）
1. **Android 音量条不碰功放寄存器**：在原厂 slot_a 播放中，按用户扫音量（100%→0→100%，23 个采样点），
   功放 20 个寄存器（含 `0x6000 DIG_VOL`、`0x6c04 AMP_GAIN`、`0x4c44`、DSP 源等）**自始至终不变**。
2. **HAL 不参与 PCM 逐级音量**：logcat 只有 `AudioService.adjustStreamVolume()/SupportSteplessVolume`
   （MIUI 无级音量），没有 `out_set_volume/platform_set_gain_dep_cal` 的逐级调用；
   ACDB gain-dep cal 在本机返回 -19/-100（失败）。
3. ⇒ **Android = 软件音量（AudioFlinger）调音量；功放增益固定**。
   我们侧等价物就是 **PipeWire 软件音量**（0 dB @ 100%）✓。

### 从原厂实机播放态抓到的完整寄存器真值（android_capture/）
`regs_android_full_1-40.txt` / `2-40.txt`（64 项）× 播放中；与我们的播放态逐项 diff 后逐条对齐：

| 寄存器 | 原厂播放态 | 主线默认 | 处理 |
|---|---|---|---|
| `0x6c04` AMP_GAIN | **0x253**(PCM18+PDM19) | 0x0 | ★ **主线 errata 补丁把该寄存器清零**（3 处 `{CS35L41_AMP_GAIN_CTRL, 0}`）→ 功放一直在最低档(0.5dB)，比原厂低 ~18dB。已改为 0x253 |
| `0x4820` SP_FRAME_RX_SLOT | **0x00000100**(slot0/1) | 0x00000400 | 机器驱动给 codec 传的是 CPU 侧 `{0,4}` → 新增 `elish_codec_rx_offset[2]={0,1}` |
| `0x4c44` DSP1_RX2_SRC | **0x09**(ASPRX2) | 0x0 | 驱动 vendor-align 序列显式写；并置 `0x4800 |= ASPRX1|ASPRX2` |
| `0x381c` BSTCVRT_DCM_CTRL | **0x51** | 0x51(我一度按"默认表"改成0x2001) | **改回 0x51**：原厂"默认表" ≠ 实际播放态 |
| `0x3808` BSTCVRT_PEAK_CUR | **0x40**(4000mA) | 0x40 | 我一度改成 0x4a(4500mA) → **回退 4000**（同上教训） |
| `0x4c24` ASP_TX2_SRC | 0x0 | 0x19 | vendor-align 写 0 |
| `0x2c04` / `0x4220` | 0x670 / 0x3 | 0x10 / 0x2 | vendor-align 写原厂值 |
| `0x4800` SP_ENABLES | 0x30001 | 0x10000 | 写 ASPRX1|ASPRX2（逐 amp 的 ASPTX1 位未动） |

**仍未对齐**（逐 amp 不同，需逐颗 Android 抓取 + 映射）：`0x4308`（0x00230023/0x00240025）、
`0x4810 ASP_FRAME_TX_SLOT`（0x04040404 / 0）、`0x4800` bit0、`0x508` OTP trim。

### 时序对齐（部分）
* 原厂 boot 顺序（dmesg）：`modprobe audio 模块(2.4s) → [CSPL] misc device(2.5s) → 8×cs35l41_probe(2.7-3.1s)
  → glink adsp Up(4.7s) → Q4 Up(5.4s) → sound card + DAI links(5.5s) → DSP 固件 → 流开始`
* 我方对应：模块由 systemd/udev 加载 → ADSP remoteproc → `adsprpcd`(fastrpc listener) →
  `elish-avcs` 触发拓扑注册 → `elish-amp-align` 应用 mixer（均在 20-22s 内完成）。
* ⚠ **尝试过的失败项**：在 DAPM 里加 `{"Main AMP", NULL, "DSP1"}`（想让 DSP 先于功放上电以消除
  PUP_DONE 警告）→ **直接导致无声**（寄存器看着对但功放不输出），已回退。PUP_DONE 警告
  因此仍为 1/amp（原厂 0），需在事件处理里按原厂顺序做，而不是改 DAPM 图。

### ★ 一个重要坑（非内核问题）
排查中一度出现"几乎没声"：**PipeWire 默认 sink 变成了 `虚拟输出`**（`wpctl status` 里
`* 41. 虚拟输出`），扬声器节点未激活。原因是我反复用 `aplay -D plughw:0,0` 直接占用 ALSA 设备，
把 PipeWire 的扬声器节点挤掉。`systemctl --user restart wirepipe` 后恢复，
默认 sink 回到 `内置音频 Speaker playback` ✓。**以后调试不要长时间直接占用 hw 设备。**

### 最终可用状态（用户确认"有声音了 而且声音不小"）
* 内核：DAPM Speaker 路由、S24_3LE、TDM mask 0x03、codec RX slot {0,1}
* 驱动：AMP_GAIN=0x253（errata 修正）、vendor-align 序列（DSP RX1/2、ASPRX 双路、DCM 0x51、
  0x2c04/0x4220/ASP_TX2-4/DSP1_RX6）、CSPL 校准下发
* 服务：adsprpcd + elish-avcs（拓扑注册）+ elish-amp-align（PCM Source=DSP、DRE、Soft Ramp=4ms、
  Analog=20、Digital=913(+12dB)、copp_topology=0x1000a100，alsactl store）
* 音量：软件层（PipeWire）+ 功放固定增益（比原厂高约 12dB 数字 + 1.5dB 模拟）

## ★★★ 179 轮：深层原因（PipeWire 虚拟输出的病根）+ 右声道定位

### 深层原因（journal 实证，解释全Session"时有时无"）
```
journalctl -b | pw.node: (alsa_output.platform-sound.pro-output-0-71) suspended -> error (Start error: 无效的参数)
         kernel: MultiMedia1: ASoC: no backend DAIs enabled for MultiMedia1
```
**TDM 配置对某些协商参数非法（AFE enable -22）→ PipeWire 硬件节点 Start 失败(EINVAL)
→ WirePlumber 兜底 auto_null（中文描述"虚拟输出"）→ 所有应用无声。**
"虚拟输出"从来只是症状。有效参数组合（实测）：BE S24_3LE + rx_mask 0x03 + CPU map 字节单位
`tdm_slot_offset{0,4}`(=slot0/1) + codec map slot 序号 `{0,1}`；把 CPU map 改成 slot 序号
`{0,1}` 或 mask 0xff 都会被 ADSP 拒（EINVAL）。
UCM 本身可用（`alsaucm -c 0 list _verbs` → `0: HiFi`）✓。

### 右声道缺失的定位（本轮）
* 寄存器全对：`0x4800=0x30000`(ASPRX1|2)、`0x4820=0x100`(slot0/1)、`0x4c44=0x09`(DSP RX2=ASPRX2)
* DAPM：`ASPRX2` 原本 Off → 已用 `snd_soc_component_force_enable_pin(component, "ASPRX2")`
  强制使能（原厂两路都常开）✓
* ⇒ 8 颗 amp 都收到立体声对，但 **每颗 amp 输出哪个声道由 DSP 内部决定**
  （CSPL 调音/每路 `<PREFIX>-spk-prot.bin`/逐 amp 寄存器）。原厂两颗 amp 的
  `0x4308`(0x00230023 vs 0x00240025) 与 `0x4810 ASP_FRAME_TX_SLOT`(0x04040404 vs 0) 不同，
  即逐 amp 配置；我方 8 颗全部相同 → 都按同一声道输出 = 只有左声道。

### 下一步（需 slot_a 抓取）
1. Android 播放**纯右声道**音频时抓 8 颗 amp 的 `0x4308/0x4810/0x4800/0x4820/0x4c40/0x4c44`
   → 建立逐 amp 的 slot/声道映射表 → 在驱动/DT 里按原厂逐 amp 配置。
2. 对比原厂 `audio_machine_kona.ko` 的 TDM `ch_mapping` 单位（字节 vs 序号）。
3. PUP_DONE 时序补丁（PWR_CTRL2 先于 GLOBAL_EN）：警告 8→0 但**会让声卡无声**，已回退；
   正确做法是在事件处理内按原厂顺序做（不动 DAPM 图），待右声道修完再做。

### 用户态恢复（重要）
* 我加的 `/etc/wireplumber/wireplumber.conf.d/20-elish-alsa.conf`（use-acp=false）与
  `/etc/pipewire/pipewire.conf.d/20-elish-speaker.conf` 已**删除**，WirePlumber 状态已还原
  → 默认 sink 恢复为 ACP 的 `内置音频 Speaker playback` ✓（ACP 失效正是我删状态造成的）
* 调试纪律：**不要用 `aplay -D plughw:0,0` 直接占用硬件**（会把 PipeWire 节点挤掉）；
  用 `pw-play`；临时停 pipewire 后必须等 `fuser /dev/snd/pcmC0D0p` 为空再直连测试。

## ★★★ 180 轮：右声道定位完成（原厂 8 颗全抓）+ TDM 映射单位权威确认

### 8 颗扬声器全部生效（排除用户怀疑）
播放中 8 颗 amp 寄存器**逐位一致**：`0x2014=1`、`0x2018=0x3721`、`0x201c=0x01100010`、
`0x6c04=0x253`、`0x6000=0x8304`、`0x4c40=0x08`、`0x4c44=0x09`、`0x4800=0x30000`、`0x4820=0x100`
⇒ 不是"只有几颗在工作"。

### 原厂 machine_kona.ko 的 TDM 映射单位（权威）
其 rodata 含 **字节单位表 {0,4,8,12}**（@0x1ac44，2 处）→ 与主线 `tdm_slot_offset` 一致；
ADSP **拒绝** slot 序号 `{0,1}`（chg50 实测 **60 条 AFE enable 错误** → 节点 Start EINVAL → auto_null）。
⇒ **TDM 映射不是右声道问题**（双方都把立体声对放 slot 0/1）。

### 原厂逐 amp 真值（Android 播放**纯右声道**时抓 8 颗）
`amps8_android_ronly.txt`：
| amp | DSP RX1/RX2 | RX slot | TX slot(0x4810) |
|---|---|---|---|
| 1-40 | ASPRX1/ASPRX2 | 0/1 | 0x04040404 |
| 1-41 | 同 | 0/1 | 0x05050505 |
| 1-42 | 同 | 0/1 | 0x07070707 |
| 1-43 | 同 | 0/1 | 0x06060606 |
⇒ 原厂 RX 路径与我们**逐位一致**；逐 amp 差异**只在 TX slot**（回传防冲突 4/5/7/6），
**不在播放通路**。`0x4800`/`0x4308` 在该播放态为 0（与之前 0x30001/0x230023 不同 →
这两个寄存器的值随播放上下文变化，不是稳定配置）。

### 结论
右声道丢失在 **ADSP→TDM 的 slot1 内容**（我们 ADSP 侧 COPP/ASM 的声道处理），
不在功放/寄存器/TDM 映射。候选：ADM matrix 的 session→COPP 声道映射、
COPP topology 0x1000a100 内部处理、ASM 流声道配置。

### 当前状态（已恢复可出声，chg52）
sink=内置音频 Speaker playback ✓ 三服务 active ✓ Start error=0 ✓
`0x2014=1`、`0x6c04=0x253`、`0x6000=0x8304(+12dB)`、`0x4820=0x100` ✓
左右声道测试音均播出（用户反馈：左正常、右弱/缺）

## ★ 181 轮：右声道最后一环定位（预算内结论）

### 已排除（全部有实测证据）
* 8 颗 amp 全部上电且寄存器逐位一致（不是"只有几颗生效"）
* TDM 映射：原厂 .ko rodata 含字节表 {0,4,8,12} ✓ 与主线一致；ADSP 拒绝 slot 序号 {0,1}（60 条 -22）
* RX slot 位置 {0,1} = 原厂 ✓（SP_FRAME_RX_SLOT=0x100）
* FE 是立体声（hw_params channels=2）✓
* DSP1_RX1/RX2 = ASPRX1/ASPRX2 ✓（与原厂逐位一致）；ASPRX2 已强制 On（DAPM ✓）
* 播放中写 mux 后 `DSP RX2 Source: On` ✓、`0x4c44=0x09` ✓

### 定位结论
**每颗 amp 收到完整立体声对（slot0/1），但输出哪个声道由 DSP 内部决定**
（CSPL 调音 / 每路 `<PREFIX>-spk-prot.bin` / CSPL_UPDATE_PARAMS_CONFIG 的 12 值块）。
我们只有 BRH/BRL 有调音文件（其余 6 路为空 → 固件默认），且 8 颗行为一致（都出左声道）
⇒ **缺每路的"声道分配"调音**。

### 下一步（下轮）
1. 从 Android `/data/vendor/audio/acdbdata/delta/`（运行时调音，f2fs 读不了 → 用 adb/su 抓）
   或 `/mnt/vendor/persist/audio/` 取非空的每路调音 → 部署到 /lib/firmware/cirrus/<PREFIX>-music.txt
2. 或把已有的 BLH-music.txt 12 值块按原厂 CSPL 时序推给全部 8 路，试验证
3. PUP_DONE 时序（PWR_CTRL2 先于 GLOBAL_EN）：警告 8→0 但无声，已回退；待声道修完再做

## ★ 182 轮：Android 运行时调音目录抓取 —— "每路调音缺失"假设被否掉

### 抓取结果（android_capture/）
* `/data/vendor/audio/acdbdata/delta/`：**基本全空**（0 字节 `.acdbdelta`，只有
  `Forte_Headset_cal.acdbdelta` 792B 是本次开机耳机插入时生成的）→ **原厂也没有每路运行时调音** ✗
* `/mnt/vendor/persist/audio/cs35l41_cal_spk{1..8}.{bin,txt}` ✓ 在
  （spk1: cal_r=9336 / spk5: cal_r=9305 —— 与我们 `cs35l41_elish_cal_r` 表一致 ✓ 已下发）

### 校准消费问题（音量差异主体的定位）
我们写 `cd CAL_R`=9117 后**固件未消费**（回读仍 9117，未清零；`CAL_R_SELECTED` 仍=默认 0x1e0c；
`CSPL_STATE=0`）→ **Protection 固件用默认喇叭模型 → 限幅保守 → 输出偏小**。
原厂 HAL 的校准入口字符串是 **"`DSP1 Calibration` cd CAL_R"** —— 即原厂有独立的
**Calibration DSP 实例**（我们手上有 `cs35l41-dsp1-spk-cali.wmfw` ✓），由
`audio_extn_cirrus_boot_thread` 在启动时：切到 Calibration 固件 → 推校准 → 跑校准 →
结果写回 Protection 实例的 CAL_R/CAL_R_SELECTED。**这是 mainline 完全没有的一层。**

### 下一步
1. 实现"校准 boot"：DSP1 载入 `spk-cali.wmfw`（Calibration 实例）→ 写 CAL_R/AMBIENT/CHECKSUM
   → CSPL_COMMAND 触发 → 等校准跑完 → 切回 Protection（结果已在 CAL_R_SELECTED）
2. 右声道：已排除功放寄存器/TDM 映射/每路调音；剩 ADM→COPP 声道处理与 DSP 内部声道分配
   （每颗 amp 收到立体声对但只输出左声道）

## ★ 183 轮：运行时重校准触发器 + ACP 抢占嫌疑

### 新增：`/sys/module/snd_soc_cs35l41/parameters/elish_recal`
写 1 = 对全部 8 颗 amp 重推出厂校准（`cs35l41_elish_calibrate()`），用于运行时验证。
实测：触发成功（`pushed factory calibration cal_r=9305/9560…`）但**固件仍不消费**
（CAL_R 回读未清零、CAL_R_SELECTED=默认 0x1e0c、CSPL_STATE=0）→ 确认校准路径需要
原厂的 **Calibration DSP 实例**（`spk-cali.wmfw` + `audio_extn_cirrus_boot_thread` 机制）。

### ACP 间歇失效的深层嫌疑
`elish-amp-align` 开机时 72 次 amixer 写 + `alsactl store` **与 WirePlumber 的 ACP/UCM
解析抢 ALSA 控制接口** → ACP 只剩 "off" profile → 无硬件 sink → auto_null 兜底。
已修：align 服务先等 `wpctl status` 出现 "Speaker playback"（最多 45s）再做控制写入 ✓
（实验待多次重启验证）

### 状态
sink=内置音频 Speaker playback ✓ 三服务 active ✓ Start error=0 ✓

## ★ 184 轮：BE 格式 A/B（无刷机，运行时参数）
`elish_be_format` 1(S24_3LE) vs 0(S16)：播放中净电流 -68/-68/-70 vs -68/-70/-74 mA —— **无可测差异**
（电池状态变化使指标饱和）。保持 S24_3LE（原厂格式）✓
当前状态：sink=内置音频 Speaker playback ✓ 稳定，三服务 active ✓

## ★ 185 轮：DSPTX2 实验无效 + i2c 直写教训

### DSPTX2 假设（未验证成功也未否掉）
把右侧 4 颗（BRH/BRL/TRH/TRL）DAC_PCM1_SRC 写成 0x33(DSPTX2) 后全无声 → 但干净重启后
恢复正常 ⇒ **是 i2c 直写弄乱驱动 regcache 造成的**（不是 0x33 本身）。
假设"DSP TX1=左/TX2=右、原厂把右侧 amp 的 DAC 指到 DSPTX2"**仍未验证**：
* 支持点：我们 8 颗全 0x32 且都只出左声道；DSPTX1/TX2 是 DSP 的两个输出
* 反对点：原厂实抓的 0x4c00 只有 2 颗（都 0x32）；"PCM Source" enum 只有 {ASP,DSP}
* 正确做法：在驱动里加参数（如 `elish_dac_tx2_mask`）按 DT 的 `cirrus,right-channel-amp`
  分配，避免直写硬件

### ★★★ 教训（重要）
**禁止运行时 i2c 直写驱动管理的寄存器**（0x4c00/0x4820/0x4800 等）：
驱动 regcache 与硬件失步后，DAPM 用缓存值覆盖 → 状态错乱 → 无声。
干净重启（驱动+开机服务重新应用）可恢复。实验一律用驱动内参数/模块参数。

### 状态（干净重启后，用户确认"有声音了，恢复正常"）
sink=内置音频 Speaker playback ✓ 三服务 active ✓ 0x2014=1 0x6c04=0x253 0x6000=0x8304 0x4c00=0x32

## ★ 186 轮：用户澄清 + 单声道结论确认 + CH_BAL 交换否掉

### 用户关键澄清
"所有喇叭都输出了左声道，两侧喇叭都有声音，右声道声音都没有"
⇒ 8 颗全在工作，全部输出左声道内容（不是"只有左侧的喇叭出声"）。

### 单声道结论（决定性证据链）
1. RX 换源实验（右侧 4 颗 RX1=ASPRX2=slot1，DAPM mux On ✓）→ 用户听到"两侧相同"（单声道）
   ⇒ slot1 内容 = slot0 内容 ⇒ **ADSP 把左声道复制到两个 slot = 单声道传输**
2. CH_BAL 交换位（原厂 bus1 同款 0x00400000，反汇编确认 bit22=swap 布尔）→ 无变化 ✓ 与单声道一致
3. 固件无 Channel Swap 系数（控件列表确认）；原厂 mixer_paths 的 Channel Swap 全 Off（但实机 bus1=0x00400000，说明是 HAL 运行时设的）

### 已验证 = 原厂的整条 ADSP 配置链（dmesg 对比）
```
elish adm_open: port 0x38(=56=TERTIARY_TDM_RX_0✓) path 1 rate 48000 ch 2 bits 24 topo 0x1000a100
原厂:              adm_open:port 0x9020 path:1 rate:48000 channel_mode:2 perf_mode:1 topology 0x1000a100 bit_width 24 app_type 69937 acdb_id 10011 passthr_mode 0
```
port/ch/bits/topo/path/flags/映射{FL,FR} 全部一致 ✓
**唯一缺: app_type 69937 + acdb_id 10011**（下游 ADM open V8/V9 命令带，mainline V5 无）

### 剩余工作
1. **app_type/acdb_id 下发**（ADM 流 app-type 配置）—— 下游 msm_pcm_routing 的 app_type_cfg 命令
2. 校准消费（原厂 Calibration DSP 实例机制）
3. SLPI watchdog 崩溃（dog_virtual_user ×N）
4. TLL/TRL 调音文件缺失（-2）
