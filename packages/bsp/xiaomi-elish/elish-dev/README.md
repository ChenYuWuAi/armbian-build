# elish 开发工作归档（mainline / Armbian 6.12.58）

Mi Pad 5 Pro（elish, SM8250-AC / 骁龙 870）在 mainline 内核上的全部开发工作。
`work/` 下**保留 PC 现场 `~/axis_rnd/` 的相对路径**，方便逐条对照。

- 上游：`armbian/build`
- 本分支：`elish-charging-6.12`
- 内核补丁另见 `patch/kernel/archive/sm8250-6.12/`（0001–0055）
- `dsh-sessions/` = 原始 AI 会话导出（JSONL，`.zip`），`(4)` 为最新最全

## 工作流索引

### 1. 音频 / ADSP 管线（主线目标：把 Android 的 DSP 声音管线移植到 Armbian）
现场症状：**只有左声道、声音小、会破音**。最终定位到 **ADSP 固件里没有 Android 的
`0x1000a100` topology**，必须先下发 `ADM_CMD_ADD_TOPOLOGIES`。

- `work/kernel/AGENT_STATE.md` —— **主状态文档（4379 行，含 143-147 轮充电 bringup）**
  - 第 22 轮：`q6adm_open(0x1000a100)` 返回 `cmd=0x10326 err=0x3` ⇒ 固件无此 topology；
    NULL topology `0x10312` 可开（对照实验）
  - 第 23 轮：**ADD_TOPOLOGIES 载荷已提取**——不在 `Forte_Speaker_cal.acdb`，而在
    `Forte_Global_cal.acdb` 的 DATAPOOL 尾部，绝对偏移 `0x89da`，长 5444 B
    → `work/acdb/core_custom_topologies.bin`（md5 `c7f33c304aa1f36d9277b206a6ea4bf5`）
  - 内核侧做法：`q6adm_probe()` 5 秒后 `request_firmware("elish_topologies.bin")`，
    文件不存在则跳过（其它板子无副作用）；opt-in 安全镜像 `boot_b_optin.img` 已验证不崩 ADSP
- `work/kernel/elish_adsp_vol/` —— `elish_adsp_vol.c` 内核模块 + `NOTES.md` + `build_and_test.sh`
- `work/kernel/patches/` —— 0001/0002/0003/0003b（q6adm COPP gain、q6asm stream volume、
  q6routing 可配 COPP topology、elish ADM topologies）+ 0054（cs35l41 PUP_DONE 状态轮询）
  + `NOTES.md` / `SERIES.md`
- `work/kernel/patched/` —— 我们改过的 q6adm/q6routing 源码（权威版本是 `patches/` 里的补丁）
- `work/acdb/` —— ACDB 解析（`parse_acdb.py`, `acdb_tables.py`）、`Forte_*.acdb`、
  `persist/cs35l41_cal_spk{1..8}.{bin,txt}`（每颗放大器的工厂标定值）
- `amp_fw/` —— 8 颗 CS35L41 的 `*-spk-{cali,prot}.bin` 固件
- `work/armbian/`、`work/audio/`、`work/android/` —— `amp-fix.sh(.device)` 守护脚本
  （**必须保留**：mainline 驱动不写 `0x4808`/`0x6c04`，靠脚本补）、DSP 路由、tinymix 对照dump
- 报告：`ELISH_AMP_TDM_FIX.md`、`ELISH_AMP_ANDROID_DIFF.md`、
  `work/android/ANDROID_PIPELINE_RE_2026-09-22.md`、`ACDB_PORT_PLAN.md`

### 2. 充电（PPS 快充 / 双电荷泵 + 泵结温降额）
- `work/kernel/elish_chg/` —— `bq2597x_elish.c`（双泵驱动，master/slave 使能顺序）、
  `elish-fc2.c`（原厂 FC2 算法复刻）、`elish-charged.c`（PPS 策略守护）、
  `ampreg.c`、`*.service`、`deploy-m2.sh` / `verify-m2.sh`、`test_curves.c` / `dryrun.c` / `elish-fc2`
- `from-tablet/charging/` —— 平板侧同一套源码的另一份快照（含 Makefile / 头文件）
- 结论速览：APDO 钳制（EINVAL 根因）→ 三环控制（vbat/ibat/ibus）→ **master 先于 slave 使能**
  → 电池电流 1.28 A 提升到 ~7.85 A（~43–53 W 输入，40 °C）

### 3. 键盘
- `work/kernel/KEYBOARD_FIX.md` + `work/kernel/patches/0055-elish-keyboard-pinctrl-under-lpass-tlmm.patch`

### 4. 蓝牙
- 记录见 `ELISH_FIX_REPORT.md` / `SESSION_SUMMARY.md` / `ELISH_AMP_TDM_FIX.md`
- 修复镜像 `elish_boot/boot-btfix.img`、原始分区 `backup/partbackup/bluetooth_{a,b}.img`
  **体积原因未入库**（现场保留在 PC 的 `~/axis_rnd/`）

### 5. fastrpc（ADSP 用户态）
- `work/fastrpc_userspace/src/` —— 从 AOSPA `android_external_fastrpc` 改的 `apps_std_imp.c` /
  `fastrpc_apps_user.c`（含 `adsprpcd` 部署产物说明）

### 6. 刷机 / 分区 / 救砖工具
- `build_gpt_elish.py`、`raw2sparse.py`、`sparse2raw.py`、`sparse_to_raw.py`、`grow_userdata.py`
- `lp_extract.py` / `lp_extract_local.py` / `lp_list.py`
- `ELISH_RECOVERY_2026-09-21.md`、`ELISH_UNBRICK_GUIDE.md`、`SESSION_SUMMARY.md`

### 7. 电池功率聚合（btop / upower 显示整包功率）
`work/kernel/elish_batt/` —— `elish_batt_agg.c`：把两颗 bq27z561 聚合成一个
`battery` power_supply：

- `power_now = V0*I0 + V1*I1`（整包功率，btop 直接取这个值显示瓦数）
- `voltage_now` = 两芯相加（2S 整包电压），`current_now` = 两芯平均
- `capacity` = 两芯取小，`temp` = 两芯取大，`status` 按充电/放电/满/未充优先级

原因：mainline 没有原厂 `dual_fuel_gauge_class` 那样的聚合节点，btop 只会在
两颗表计里挑到一颗，显示的功率只有整包的一半（实测单芯 16.6W vs 整包 34W）。
**不需要改 DTB**（按名字找已有的表计节点）。btop 侧在 `~/.config/btop/btop.conf`
设 `selected_battery = "battery"`（btop 只在退出时写回配置，改完要重启 btop）。

GNOME Shell 侧（Vitals 扩展 85）：见 `work/gnome/vitals-battery/`（补丁 + `install.sh`）。
Vitals 的电池区块只认 `BAT0/BAT1/BAT2/BATT/CMB0/CMB1/CMB2/macsmc-battery` 八个硬编码
名字（读 `<name>/uevent`），本机节点叫 `battery`；而且它的电池区块**原本不显示温度**。
补丁：`BATTERY_PATHS` 增加 `8: 'battery'`（prefs.ui 下拉同步加一项），电池区块新增
`Temperature`（uevent TEMP 是 0.1℃，`temp` 渲染按毫摄氏度要先 ×100）与 `Current`
（uevent CURRENT_NOW 是 µA，交给 `milliamp` 渲染除 1000）两行，再设
`battery-slot=8` 并重载扩展。实测显示：`Charging / 47% / 8.465V / 42.6℃ / 4179mA / +35.4W`。

### 8. 运行期约束（重要，踩过的坑）
- 当前运行内核把模块的退出段丢掉了，`/proc/modules` 里**所有模块都是
  `[permanent]`**（`mod->init && !mod->exit`）⇒ **任何模块都 rmmod 不掉（EBUSY）**。
  驱动 `.ko` 的改动只有**重启**后才生效；用户态（daemon）改动随时可切换。
  之前"改了驱动但 `tdie_raw` 还是 0"就是这个原因——模块根本没重载成功。
- 因此 `bq2597x_elish.c` 里使能 TDIE 的改动本次无法立即生效，另加
  `elish-pump-tdie.service`（oneshot，`i2cset` 清 REG_15 bit0）在运行期补上。
- 泵结温降额（`fc2_tune()`）：两颗泵取更高结温，>65℃ 起每 1℃ 降 300mA、
  下限 3000mA，≥80℃ 直接退出 FC2 并退避 10s。实测空载 44~50℃、
  双泵满载（~34W 电池功率）54~60℃；芯片自身热调节阈值是 125/145℃（DT）。

### 9. 会话历史
- `dsh-sessions/*.zip` —— 5 份导出，`(4)`（2026-09-25 22:53）为最新；
  内部为 `session.v3.jsonl` + `subagents/*/session.v3.jsonl`

## 不在本库中的内容

- **小米原厂内核源码**（`MiCode/Xiaomi_Kernel_OpenSource`，分支 `elish-r-oss`）以及从中提取的
  **厂版 ADSP 源码**（`work/kernel/msm-extra-dsp/`）、**未改动的上游 mainline 源码基线**
  （`work/kernel/src/`）：均属未做任何修改的原厂/上游代码，不需要入库，也从上游直接获取。
  我们对内核的改动**全部以 `patch/kernel/archive/sm8250-6.12/` 的补丁 + 本目录下的自有源码**
  体现。

## 未入库的大文件（现场在 PC `~/axis_rnd/`）
| 路径 | 说明 |
|---|---|
| `work/kernel/artifacts/boot_b_chg*.img` | 12 G，充电实验 boot 镜像 |
| `work/boot_b_*.img`、`work/kernel/boot_b_*.img` | 各轮 boot_b 镜像（192 MB/个） |
| `work/kernel/build/linux-6.12.58`、`work/kernel/elish-kernel` | 内核源码树 |
| `work/dtb/`、`work/dt_android/`、`work/vendor_dtb/` | DTB 工作集 |
| `~/axis_rnd/*.img`、`rom/`、`magisk/`、`platform-tools/` | 原厂镜像与刷机工具 |
