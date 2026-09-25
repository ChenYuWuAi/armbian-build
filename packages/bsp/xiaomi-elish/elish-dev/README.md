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
`BAT0` power_supply（标准名，Vitals/upower 才认得）：

- `power_now = V0*I0 + V1*I1`（整包功率，btop 直接取这个值显示瓦数）
- `voltage_now` = 两芯相加（2S 整包电压），`current_now` = 两芯平均
- `capacity` = 两芯取小，`temp` = 两芯取大，`status` 按充电/放电/满/未充优先级

原因：mainline 没有原厂 `dual_fuel_gauge_class` 那样的聚合节点，btop 只会在
两颗表计里挑到一颗，显示的功率只有整包的一半（实测单芯 16.6W vs 整包 34W）。
**不需要改 DTB**（按名字找已有的表计节点）。btop 侧在 `~/.config/btop/btop.conf`
设 `selected_battery = "battery"`（btop 只在退出时写回配置，改完要重启 btop）。

GNOME Shell 侧（Vitals 扩展 85）：`work/gnome/vitals-battery/`（补丁 + `install.sh`）。
节点改成标准名 `BAT0` 后，Vitals 的电池区块本身就能列出 State/Percentage/Voltage/
Power Rate（它只认 BAT0..BAT2/BATT/CMB*/macsmc-battery 这些硬编码名字）；补丁只额外
加 `Temperature`（uevent TEMP 是 0.1℃，`temp` 渲染按毫摄氏度要先 ×100）与 `Current`
（uevent CURRENT_NOW 是 µA，交给 `milliamp` 渲染除 1000）两行。实测：
`Charging / 47% / 8.465V / 42.6℃ / 4179mA / +35.4W`。
**注意**：Wayland 会话下 GNOME Shell 不热重载扩展 JS（disable/enable 无效，实测加了
探测 `log()` 一直不出现），改完扩展必须重新登录/重启会话才生效。

### 7b. 充电温控与原厂对齐（以电池温度为核心）

原厂（Xiaomi/Qualcomm `pd_policy_manager` + SMB5 + Android thermal HAL）做的是
**电池温度驱动**的热控，芯片结温只靠芯片自身的硬件热调节。本实现逐项对齐：

| 原厂逻辑 | 出处 | 本实现 |
|---|---|---|
| 电荷泵 JEITA：warm 48.0℃ / cool 10.0℃ / 滞回 2.0℃，**锁存**（越界禁泵，回到带内才恢复） | `pd_policy_manager.c: pd_disable_cp_by_jeita_status()`；DT `mi,pd-battery-warm-th = <480>` 覆盖默认 450 | `cp_jeita_out_of_range()`，FC2 进入与维持都走它 |
| thermal level ≥ 12 退出 FC2 | DT `mi,therm-level-threshold = <12>`（默认 13） | `thermal_level() >= THERM_LEVEL_THRESHOLD` |
| 从泵只在 level < 9 且容量 < 80% 时允许 | `MAX_THERMAL_LEVEL_FOR_DUAL_BQ = 9`、`CAPACITY_HIGH_THR_NORMAL = 80` | `ST_FC2_ENTRY_3` 的从泵条件 |
| JEITA FCC/FV 分档 | 原厂 DTBO `jeita-fcc-ranges` / `jeita-fv-ranges` | `jeita_fcc[]` / `jeita_fv[]` |
| FCC 阶梯 | DTS `qcom,thermal-fcc-pps-bq`（16 级） | `thermal_fcc_pps_bq[]`（逐值一致） |
| ICL 阶梯 | DTS `qcom,thermal-mitigation-pd-base`（16 级） | `thermal_icl_pd[]` |
| thermal level 来自 Android thermal HAL 写 SMB5 的 `CHARGE_CONTROL_LIMIT` | 用户态 HAL，Linux 侧没有 | **唯一近似项**：`level = (电池温度 - 43.0) / 1.0`（clamp 0..15） |
| 泵芯片结温只靠芯片自身热调节 125℃/145℃ | 厂版 DT | 另加一道软件兜底 75/85℃（正常快充结温 53~60℃，不会触发） |
| daemon 意外退出 | — | `ExecStopPost=/usr/local/sbin/elish-charged-safety`：关两颗泵 + 恢复 SW 路径 |

daemon 启动时会打印生效阈值，便于现场核对：

```
[chg] thermal: 泵窗口 100..480 (滞回 20, 锁存), ladder 起 43.0C, level 阈值 12, 双泵需 level<9, 结温兜底 75/85C
```

`elish-charged --selftest` 可在不接硬件的情况下验证结温兜底曲线的边界（10 个用例）。

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

### 10. CPU 频率：解锁 SM8250-AC 超大核 3.1872 GHz（原厂标称 3.2 GHz）

**现象**：`policy7`（cpu7 超大核）最高只有 `2841600`，开机日志：

```
[    0.220023] cpu cpu7: failed to update OPP for freq=3187200
[    0.220049] cpu cpu7: failed to update OPP for freq=3187200
```

**根因（机制差异，不是硬件问题、不是升频失败）**

`drivers/cpufreq/qcom-cpufreq-hw.c` 的 `qcom_cpufreq_hw_read_lut()` 读硬件 EPSS
频率 LUT 后，会拿 **DT 的 `operating-points-v2` 逐条交叉校验**：

```c
/* OPP 表带 icc 带宽时走这条分支 */
ret = dev_pm_opp_adjust_voltage(cpu_dev, freq_hz, volt, volt, volt);
if (ret) {                                       /* 频点不在 DT 表里 -> -ENOENT */
        dev_warn(cpu_dev, "failed to update OPP for freq=%d\n", freq_khz);
        table[i].frequency = CPUFREQ_ENTRY_INVALID;    /* 直接丢弃该档 */
}
```

上游 `sm8250.dtsi` 的 `cpu7_opp_table` 是按 **SM8250（骁龙 865）**写的，止于
`opp-2841600000`；本机是 **SM8250-AC（骁龙 870）**，LUT 多一档 3187200，于是被丢掉。
原厂内核不做这个校验（下游 `qcom-cpufreq-hw.c` 直接把 LUT 逐条 `dev_pm_opp_add()`，
DT 表无关），所以原厂能跑满。

**硬件证据**（root 下 `/dev/mem` 直读 EPSS 寄存器；domain2 = cpu7 @ `0x18593000`，
`reg_freq_lut=0x100`、row=4 B、xo=19.2 MHz、`freq = xo*lval/1000`）：

| index | raw | lval | core_count | freq |
|---|---|---|---|---|
| 19 | `0x40040094` | 148 | 4 | 2841600 kHz |
| **20** | `0x400300a6` | 166 | 3 | **3187200 kHz** ← DT 缺的就是这一档 |
| 21+ | `0x400400a6` | 166 | 4 | 3187200 kHz（重复 = 表尾，驱动在此 `break`） |

LUT 里**没有** 2995200/3091200（否则日志会出现更多条 `failed to update OPP`；
上游 6.18 给 cpu7 加的 `opp-3091200000` 对这台机器的 LUT 不生效）。
三簇完整 LUT dump 见 `from-tablet/fixes/` 的排查报告与本次会话记录。

**修改**：`patch/kernel/archive/sm8250-6.12/0056-arm64-dts-qcom-sm8250-xiaomi-elish-add-3.2GHz-prime-OPP.patch`
——只在**板级** `arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-common.dtsi` 末尾给
`&cpu7_opp_table` 追加一档，不动 `sm8250.dtsi`（避免影响真正的 865 机型）：

```dts
&cpu7_opp_table {
	opp-3187200000 {
		opp-hz = /bits/ 64 <3187200000>;
		opp-peak-kBps = <8368000 51609600>;   /* 与 2.8416 GHz 档一致 */
	};
};
```

做法与 Armbian 里同为 SM8250-AC 的 Lenovo Xiaoxin Pad Pro 12.6 一致
（`patch/kernel/archive/sm8250-6.18/0020-arm64-dts-qcom-add-device-tree-for-Xiaoxin-Pad-Pro-1.patch`）。

**换 DTB 的附带风险 = 0（已逐节点验证）**：把实际在跑的 DTB 从 `boot_b` 里抽出来反编译，
与新编 DTB 反编译对比，**语义差异只有这一个 OPP 节点**（其余 7 行是 phandle 重编号）：

```
sudo dd if=/dev/disk/by-partlabel/boot_b of=/tmp/bootlive.img bs=1M count=32
# 在镜像里找 FDT magic \xd0\x0d\xfe\xed，按头部 totalsize 导出 DTB
dtc -I dtb -O dts -o live.dts   /tmp/live-real.dtb
dtc -I dtb -O dts -o new.dts    arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-csot.dtb
diff live.dts new.dts            # 只有 opp-3187200000
```

安装/验证/回滚（ABL 钩子用的是 `/usr/lib/linux-image-$KVER/qcom/*.dtb`，不是 `/boot/dtb-*`）：

```
sudo install -m644 <新 dtb> /usr/lib/linux-image-$KVER/qcom/sm8250-xiaomi-elish-csot.dtb
sudo /etc/kernel/postinst.d/zz-update-abl-kernel $KVER     # 重生成镜像并 dd 到 boot_b
sudo reboot
```

重启后 `elish-3g2-verify.service`（一次性）把自检写到 `~/3g2-verify.txt`；
判定：`policy7/scaling_available_frequencies` 末尾出现 `3187200`，且 dmesg 无
`failed to update OPP`。回滚脚本 `~/rollback-3g2.sh`（恢复 `.bak-3g2-*` 的 DTB 并重写 boot_b）。

**LUT 读取复现**（root，三簇域名 0x18591000 / 0x18592000 / 0x18593000）：

```python
import mmap, struct
f = open('/dev/mem', 'r+b')
m = mmap.mmap(f.fileno(), 0x1000, offset=0x18593000)   # domain2 = cpu7
for i in range(24):
    w = struct.unpack_from('<I', m, 0x100 + i*4)[0]
    print(i, hex(w), 'src=%d lval=%d cc=%d' % ((w>>30)&3, w & 0xff, (w>>16)&7),
          '%d kHz' % (19200000 * (w & 0xff) // 1000))
```

**换 DTB 时最容易踩的坑：`/boot/vmlinuz-$KVER` 不是当前跑的内核**
- 现场 `/boot/vmlinuz-6.12.58-current-sm8250` 是**原厂 Armbian 内核**
  （`build@armbian` #1，2025-11-13，gcc 11.4，49.5 MB），而实际在跑的是自制的
  `#59 SMP Fri Sep 25 14:02:09 CST 2026`（`axis@CHENYU-GEEKPRO`，gcc-12，
  PC 上 `build/linux-6.12.58/arch/arm64/boot/Image` = 42 463 744 B）。
  ABL 钩子 `zz-update-abl-kernel` 取的是 `/boot/vmlinuz` ⇒ **直接跑钩子会把启动镜像
  悄悄换回原厂内核**（bq2597x/elish 自制模块就不是配套内核了）。
  正确顺序：先把自制 `Image` 装到 `/boot/vmlinuz-$KVER`（原厂那份先备份），再跑钩子。
- 校验方法（Android boot 头里 `kernel_addr=0x8000` 是**加载地址**，文件偏移是
  `page_size`=0x1000；kernel blob = `Image.gz` + 紧随其后的 DTB）：

  ```python
  d = open('/boot/armbian-kernel-csot.img','rb').read()
  ksize = int.from_bytes(d[8:12],'little'); page = int.from_bytes(d[36:40],'little')
  blob = d[page:page+ksize]                      # kernel blob
  fdt  = blob.find(b'\xd0\x0d\xfe\xed')          # DTB 紧跟 gzip 之后
  open('/tmp/k.gz','wb').write(blob[:fdt]); open('/tmp/k.dtb','wb').write(blob[fdt:])
  # gzip -dc /tmp/k.gz | md5sum  应等于自制 Image 的 md5
  ```

  本次实测：镜像内解出的 Image md5 = `0dd8dc6a4e702078e9b68953c847f609`
  = PC 上 `Image`（#59）= 重启前 live 内核 blob，三者一致。

**构建环境注意（本次踩到）**
- 远端 `/home/axis/axis_rnd/work/kernel/build/linux-6.12.58` 是**脏树**（之前手工改过
  `pm8150b.dtsi` 等），再跑一遍补丁序列会出现 `charger@1000` / `fuel-gauge@4000`
  **重复节点**，DTC 报 `ERROR (duplicate_node_names)`，其它 qcom 板的 DTB 全都编不过。
  要么从 `linux-6.12.58-gh.tar.gz` 重新解包再按序打补丁，要么先 `patch -R` 掉 0034/0035。
  （不改动任何 `drivers/` 内核源码就无法解锁频率——**只改 DTS 即可**，本补丁即如此。）
- 新宿主 gcc 会把 `tools/bpf/resolve_btfids` 的 libbpf 因
  `-Werror=discarded-qualifiers` 编挂；需要
  `EXTRA_CFLAGS=-Wno-error=discarded-qualifiers` 或换旧版宿主 gcc。

### 11. 开发流程 skill：`elish-kernel-bringup`

把第 10 节这套流程（远程机编译 → 补丁入库 fork → 只换 DTB 的落地 / 开机验收 / 回滚）固化成了 skill：

- 现场（DSH 用户级 skill 根，本会话已加载可用）：`~/.dsh/skills/elish-kernel-bringup/SKILL.md`
- 入库副本：`packages/bsp/xiaomi-elish/elish-dev/skills/elish-kernel-bringup/SKILL.md`

内容：现场拓扑核对；根因定位（读机制 → `/dev/mem` 硬件寄存器证据 → 对比原厂内核）；
补丁生成（`diff -u` 机械生成 + 纯净树应用验证 + 板级 dtsi 优先 + 对齐同类机型做法）；
只改 DTB 时的"逐节点 diff 证明只差预期改动"落地法；ABL 钩子刷入 +
systemd 一次性开机自检 + 回滚脚本；以及必踩的坑清单（`/boot/vmlinuz` 可能不是在跑的内核、
Android boot 头 `kernel_addr` 是加载地址、模块 `[permanent]` 不能 rmmod、脏树重打补丁会重复节点、
宿主新 gcc 的 libbpf `-Werror`、ssh 里跑 git 会吃 stdin / 跟踪引用不更新、大文件不入库）。

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
