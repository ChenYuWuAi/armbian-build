# Xiaomi Pad 5 Pro (elish) 功放：mainline 驱动 vs Android 原厂 —— 差异全清单与 Android 音频框架逆向

设备：`M2105K81AC` / `elish` / CSOT 屏 / sm8250 / 8× CS35L41 智能功放
内核：`6.12.58-current-sm8250`（Armbian），对比 Android 13 原厂（slot b）
日期：本轮逆向的完整结论

---

## 0. 结论摘要（先看这里）

| # | 差异 | 影响 | 状态 |
|---|------|------|------|
| 1 | **首次上电必然失败**：DAPM 冷启动首个 pass 把 `Main AMP` 排在 `DSP1` 之前，GLOBAL_EN 下发时 DSP 未启动、`PWR_CTRL2` 监视器未使能 → 芯片上电序列无法完成 → `PUP_DONE` 不置位 → `Enable(1) failed: -110`。第二次起正常 | 开机/长时间空闲后**第一次播放无声** | 已定位；静音预热可绕过；驱动级修复尝试失败已回退 |
| 2 | **出厂喇叭校准从未下发**：Android HAL 把 `cs35l41_cal_spk1..8.bin` 写进 DSP，mainline 完全没有这条路径，`CAL_R` 恒为 0 | 保护算法用内置默认喇叭模型（`CAL_R_SELECTED=0x1e0c`） | **已在本轮把校准 hardcode 进驱动模块** |
| 3 | 原厂 `cirrus,fast-switch` 每场景调音（delta 文件）mainline 不支持 | 不同 usecase（music/voice/voip/misc）无独立调音 | 未实现 |
| 4 | `cs35l41_reg` 缓存默认值与芯片实际值不符，`regcache_sync` 每次 resume 会写错 | 每次 resume 后 `GPIO1/2_CTRL1`、`AMP_GAIN_CTRL`、`ASP_TX3/4_SRC` 被写成 mainline 假设值 | 未修 |
| 5 | 固件命名规则不同（前缀位置） | 需 shim 文件 | 已绕过 |
| 6 | 若干原厂控件缺失（`Boost Target Voltage`、`Boost Class-H Tracking Enable`、`Channel Swap`、`Ramp *`、`Hibernate Force Wake` 等） | Android HAL 会写这些；其中 `Boost Target Voltage` 两边实际都是 0 | 影响有限 |

**关键测量**：`ATTENUATION = 0x7FFFFF (≈1.0)`、`REDUCE_POWER = 0` → **保护算法并没有在限幅**，所以"声音小"**不是**校准缺失造成的。校准下发是"对齐 Android 状态"的正确性补课，不是音量问题的解药。

---

## 1. 拓扑与寻址

```
I2C bus 1: 0x40=BRH  0x41=BLH  0x42=BRL  0x43=BLL
I2C bus 3: 0x40=TRH  0x41=TLH  0x42=TLL  0x43=TRL
```
> 注意：设备树节点名是 `speaker-amp@40`（**不是** `cs35l41@40`），grep 时容易漏。

寄存器访问（32 位大端寄存器地址 + 32 位大端数据，共 8 字节）：

```bash
# 读  (示例: 读 1-0040 的 PWR_CTRL1=0x2014)
sudo i2ctransfer -f -y 1 w4@0x40 0x00 0x00 0x20 0x14 r4@0x40
# 写  (示例: 置 GLOBAL_EN)
sudo i2ctransfer -f -y 1 w8@0x40 0x00 0x00 0x20 0x14 0x00 0x00 0x00 0x01
```
`-f` 是关键：不加会因驱动占用而 `EBUSY`。

---

## 2. 内核驱动层差异（mainline ←→ 原厂 `audio_cs35l41.ko`）

### 2.1 设备树属性

Android DT（`dtb_dtbo12`，fragment@136 起）每个 amp 节点：

| Android 属性 | mainline 支持 | 说明 |
|---|---|---|
| `cirrus,temp-warn_threshold = <3>` | ❌（mainline 只认 `cirrus,temp-warn-threshold`，且未实现） | 温度告警阈值，实测 mainline 下 `DTEMP_WARN_THLD=2` |
| `cirrus,boost-peak-milliamp = <0xfa0>` | ✅ | 4000 mA ✓ 生效（`BSTCVRT_PEAK_CUR=0x40`） |
| `cirrus,boost-ind-nanohenry = <0x3e8>` | ✅ | 1000 nH ✓ |
| `cirrus,boost-cap-microfarad = <0x0f>` | ✅ | 15 µF ✓ |
| `cirrus,asp-sdout-hiz = <3>` | ✅ | |
| `cirrus,tuning-has-prefix` | ❌ | 决定固件文件名 `<PREFIX>-...bin`（见 2.3） |
| `cirrus,fast-switch = "TRH-music.txt",...` | ❌ | 每 usecase 调音文件 |
| `cirrus,gpio-config2 { cirrus,gpio-src-select=<4>; cirrus,gpio-output-enable; }` | ❌ 子节点形式不支持（mainline 用 `cirrus,gpio2-src-select`) | **4 = `INT_PUSH_PULL_LOW`（中断输出），不是扬声器开关** |
| `cirrus,right-channel-amp`（TLL/BLH 上有） | ❌ | 声道分配 |
| `cirrus,boost-ctl-millivolt` | ❌ | **Android DT 里也没有** → 两边都不写 `BSTCVRT_VCTRL1`，实测 `VCTRL1=0x00000000`（此前的"目标电压=0 导致声小"推测因此不成立） |
| `cirrus,boost-type` | 我们自行加了 `<0>`（INT_BOOST） | **Android 完全没有此属性**，两边缺省都是 INT_BOOST |

原厂 .ko 里存在但 mainline 未实现的属性字符串（可用于对照）：
`cirrus,sclk-force-output` `cirrus,lrclk-force-output` `cirrus,amp-gain-zc` `cirrus,invert-pcm`
`cirrus,fwname-use-revid` `cirrus,dsp-noise-gate-*` `cirrus,hw-noise-gate-*` `cirrus,classh-*`
`cirrus,hibernate-enable` `cirrus,boost-ctl-millivolt`

### 2.2 `reg_defaults` 逐项 diff（`regcache_sync` 每次 resume 都会写回去！）

mainline `cs35l41_reg[]`（47 项）vs 原厂（97 项）：

| 寄存器 | mainline 默认 | 原厂默认 | 备注 |
|---|---|---|---|
| `0x11008 GPIO1_CTRL1` | `0x81000001` | **`0xe1000001`** | 原厂多 bit30/29 |
| `0x1100c GPIO2_CTRL1` | `0x81000001` | **`0xe1000001`** | 同上 |
| `0x06c04 AMP_GAIN_CTRL` | `0x00000000` | **`0x00000273`** | 低 5 位=19（Android 写 18） |
| `0x04c28 ASP_TX3_SRC` | `0` | `0x20` | |
| `0x04c2c ASP_TX4_SRC` | `0` | `0x21` | |
| `0x04c5c DSP1_RX8_SRC` | `0x3b` | `0x01` | |
| 仅 mainline 有 | `PWR_CTRL2=0`、`TST_FS_MON0=0x00020016`、`IRQ1_MASK1..4`、`DSP1_CCM_CORE_CTRL=0x101` | — | |
| 仅原厂有（54 项） | — | `PWR_CTRL3=0x01000010`、`CTRL_OVRRIDE=2`、`BSTCVRT_VCTRL1/2`、`DTEMP_WARN_THLD=2`、`CLASSH/WKFET/NG` 等 | mainline 缓存里没有就不会在 resume 时恢复 |

### 2.3 固件命名

- 原厂（`cirrus,tuning-has-prefix`）：`<PREFIX>-cs35l41-dsp1-spk-prot.wmfw` / `<PREFIX>-...-prot.bin`
- mainline（DT、无 ACPI `system_name`）：`cs35l41-dsp1-spk-prot.wmfw` / `cs35l41-dsp1-spk-prot-<prefix>.bin`
- 现状：已用同名 shim 文件绕过，`DSP1: Firmware: 400a4 vendor: 0x2 v0.33.0, 2 algorithms` 正常加载，且 `Protection:` 行显示的正是 Android 的原始文件名 ✓

### 2.4 缺失的 ALSA 控件（原厂有 / mainline 无）

```
Boost Target Voltage        Boost Class-H Tracking Enable   Boost Converter Enable
AMP Enable / AMP PCM Gain(≈mainline "Analog PCM Volume")    Channel Swap
ASPTX Ref / ASPRXn Slot Position    DSP Block Bypass        Fast Use Case Delta File
Fast Use Case Switch Enable         DRE Switch(≈mainline "DRE")
Hibernate / Hibernate Force Wake / DSP Booted / CCM Reset / Force Interrupt
Manual Ramp Control / Ramp End Time / Ramp Knee Time / Auto Ramp Safety Timeout
Initial Ramp Volume Attenuation / Knee Ramp Volume Attenuation
DSP Set CAL_R / CAL_AMBIENT / CAL_CHECKSUM / CAL_STATUS      ← 校准入口
```

### 2.5 首次上电失败（本设备最实际的 bug）

**证据（ftrace `snd_soc_dapm_widget_event_start/done`）**

失败（冷启动首播）：
```
26.759  TLH Main AMP      val=1 (PRE_PMU → GLOBAL_EN)   → -110
26.911  TLH DSP1 Preloader val=1                        ← DSP 竟然在功放之后才上电
```
成功（第二次起）：
```
232.215 TLH DSP1 Preloader val=1
232.224 TLH DSP1           val=2 (POST_PMU → cs_dsp_run 释放 DSP 内核)
232.245 TLH Main AMP       val=1 (PRE_PMU → GLOBAL_EN)  ✓
```

**寄存器实证**

| 寄存器 | 冷态/失败 | 成功 |
|---|---|---|
| `PWR_CTRL2` | `0x20`（仅 BST_EN） | `0x3721`（+AMP_EN +VMON/IMON/VPMON/VBSTMON/TEMPMON） |
| `PWR_CTRL3` | `0x01100000` | `0x01100010` |
| `DSP1_CCM_CORE_CTRL` | `0x00000000` | `0x00000001` |
| `IRQ1_RAW_STATUS1` | `0x40406000`（bit24 PUP_DONE=0） | `0x41406000`（**bit24=1**） |

**已证实**：芯片本身完全正常（手工置 `PWR_CTRL1=1` 后 100ms 内 `PUP_DONE` 必置位）；`cs35l41_runtime_suspend` 空闲 3s 后 `cs35l41_enter_hibernate()`，唤醒路径 `exit_hibernate + regcache_sync` 后首次上电即命中该问题。

**试过并失败的驱动级修复（已回退，勿重犯）**：在 `cs35l41_main_amp_event` PRE_PMU 里提前 `cs_dsp_run()` + 预置 `PWR_CTRL2` 监视器位 → 不但没修好，连"第二次能成功"都破坏了（连续多次 -110）。原版模块已回滚（md5 `bf792ef12c85bde07ed1a9d43ffe671f`）。

**当前可用绕过**：开机后先做一次静音播放把首次失败吸收掉，之后播放正常：
```bash
aplay -q -t raw -f S16_LE -r 48000 -c 2 -d 1 /dev/zero
```

---

## 3. Android 音频框架逆向

### 3.1 涉及组件

| 文件 | 作用 |
|---|---|
| `/vendor/lib64/hw/audio.primary.kona.so` | 主 HAL，含全部 Cirrus 校准逻辑（字符串可证） |
| `/vendor/lib64/libcirrusspkrprot.so` | HAL 引用但 vendor 分区内不存在（应在 system 侧） |
| `/vendor/etc/mixer_paths_overlay_static.xml` | 8 个 amp 的静态控件设置 |
| `/mnt/vendor/persist/audio/cs35l41_cal_spk{1..8}.{bin,txt}` | 出厂校准（持久分区） |

### 3.2 出厂校准数据（已从 `persist.img` 解出）

```
spk1: status=1, Impedance=6.68, cal_r=9336, ambient=23, checksum=9337
spk2: status=1, Impedance=6.52, cal_r=9117, ambient=23, checksum=9118
spk3: status=1, Impedance=6.84, cal_r=9564, ambient=23, checksum=9565
spk4: status=1, Impedance=6.80, cal_r=9509, ambient=23, checksum=9510
spk5: status=1, Impedance=6.65, cal_r=9305, ambient=23, checksum=9306
spk6: status=1, Impedance=6.68, cal_r=9338, ambient=23, checksum=9339
spk7: status=1, Impedance=6.84, cal_r=9560, ambient=23, checksum=9561
spk8: status=1, Impedance=7.03, cal_r=9826, ambient=23, checksum=9827
```
`.bin` = 小端 u32 的 `cal_r`；**`checksum = cal_r + 1`**。

HAL 相关字符串：
```
"Read left cal_r: %d" / "Read right cal_r: %d" / "Invalid calibration cal_r : %d"
"BLH status = %d, cal_r=%d, ambient=%d, checksum=%d"
"Failed to open Cirrus calibration result file"
"BLH DSP Set CAL_R" / "BLH DSP Set CAL_AMBIENT"
"BLH DSP1 Calibration cd CAL_R / CAL_AMBIENT / CAL_CHECKSUM / CAL_STATUS"
```

### 3.3 ⭐ 完整 CSPL 参数下发时序（从原厂 `cs35l41_fast_switch_file_put` 反汇编，0x4340–0x4618）

这是 Android 把校准/调音真正"生效"的关键，**缺任何一步 DSP 都不会应用参数**：

```c
u32 params[N];
/* 0. HAL 把文本调音文件内容以字节写进控件；驱动逐行 kstrtoint 解析成 u32 数组 */

/* 1. 写入参数块（校准则是逐个写 CAL_R / CAL_AMBIENT / CAL_CHECKSUM） */
wm_adsp_write_ctl(dsp, "CSPL_UPDATE_PARAMS_CONFIG", type, alg, params, N * 4);

/* 2. 回读校验 */
wm_adsp_read_ctl(dsp, "CSPL_STATE", type, alg, &state, 4);
while (state != 0 && retry < 4) { usleep_range(100, 110); wm_adsp_read_ctl(...); }

/* 3. ★触发★ 写 CSPL_COMMAND = 0x08000000 (大端字节 08 00 00 00) */
u32 cmd = 0x08000000;
wm_adsp_write_ctl(dsp, "CSPL_COMMAND", type, alg, &cmd, 4);

/* 4. 轮询等到 CSPL_STATE == 0 → "CSPL STATE == RUNNING (%u attempt)" */
```

对应 mainline 控件名（**这些控件 mainline 全都有**，只是从没人调用）：
```
<PREFIX> DSP1 Protection cd CSPL_UPDATE_PARAMS_CONFIG
<PREFIX> DSP1 Protection cd CSPL_STATE
<PREFIX> DSP1 Protection cd CSPL_COMMAND
<PREFIX> DSP1 Protection cd CAL_R / CAL_AMBIENT / CAL_CHECKSUM / CAL_STATUS
```
- mainline **没有** `wm_adsp_cal_controls`（原厂 wm_adsp 扩展），但**有** `wm_adsp_write_ctl()` / `wm_adsp_read_ctl()`（均已 `EXPORT_SYMBOL_GPL`）→ 可直接复刻。
- 实测：写入 `CAL_R` 后**回读为 0**是正常现象（固件消费后清零）；判断是否生效要看 `CAL_R_SELECTED`。

### 3.4 mixer_paths（Android 实际下发的控件值，每 amp 都有）

```xml
<PREFIX> DSP1 Firmware = "Protection"     <PREFIX> DSP1 Preload Switch = 1
<PREFIX> DRE DRE Switch = 1               <PREFIX> PCM Source = "DSP"
<PREFIX> AMP PCM Gain = 18                <PREFIX> PCM Soft Ramp = "4ms"
<PREFIX> ASP TX1 Source = "DSPTX1"        <PREFIX> DSP RX1 Source = "ASPRX1"
<PREFIX> Boost Class-H Tracking Enable = 1    <PREFIX> Boost Target Voltage = 0
<PREFIX> Fast Use Case Delta File = "<PREFIX>-music.txt"   ... Switch Enable = 1
```
**实测 mainline 侧已经一致**：`Analog PCM Volume=18 (+18.5 dB)`、`PCM Source=1(DSP)`、`DRE=on`、`Preload=on`、`ASP TX1 Source=7(DSPTX1)`、`Digital PCM Volume=817 (0 dB)`。
→ **混音器配置不是"声音小"的原因。**

### 3.5 其他 HAL 行为

```
"Cirrus SP Usecase"            "Cirrus SP Volume Attenuation"
"Enable volume boost mode"     "Disable volume boost mode"
```
HAL 还按 usecase 切换（music/voice/voip/misc）并做 `Usecase (%s) is active on (%s) - disabling ..` 的互斥管理；delta 文件来自 `/data/vendor/audio/acdbdata/delta/`。mainline 完全没有这一层。

---

## 4. 本轮实施：把校准 hardcode 进驱动

补丁位置：`sound/soc/codecs/cs35l41.c`
- 新增 `cs35l41_elish_cal_r[bus][addr-0x40]` 表（上表 8 个 cal_r，按 I2C 总线/地址索引）
- 新增 `cs35l41_elish_calibrate()`：按 `dev_name()`（`"1-0040"` 形式）解析出 bus/addr，写 `CAL_R` + `CAL_AMBIENT=23` + `CAL_CHECKSUM=cal_r+1`，再写 `CSPL_COMMAND=0x08000000` 触发
- 调用点：`cs35l41_dsp_audio_ev()` 的 `SND_SOC_DAPM_POST_PMU`（DSP 已 `cs_dsp_run()` 之后），即每次 DSP 上电都下发
- 需要 `MODULE_IMPORT_NS(FW_CS_DSP)`（`cs_dsp_coeff_write_ctrl` 属于该 namespace）

编译/安装：
```bash
# 设备上 /root/ampbuild2
sudo sh -c 'cd /root/ampbuild2 && make -C /lib/modules/$(uname -r)/build M=/root/ampbuild2 modules'
sudo cp snd-soc-cs35l41.ko /lib/modules/$(uname -r)/kernel/sound/soc/codecs/
sudo depmod -a && sudo reboot -f
```
回滚：
```bash
sudo cp /root/amp_backup/modules/snd-soc-cs35l41.ko.orig \
        /lib/modules/$(uname -r)/kernel/sound/soc/codecs/snd-soc-cs35l41.ko
sudo depmod -a && sudo reboot -f
```
备份：`/root/amp_backup/modules/`（原版 md5 `bf792ef12c85bde07ed1a9d43ffe671f`）

---

## 5. 尚未解决 / 待确认

1. **首次上电失败的驱动级正解**：根因是 DAPM 首次 pass 的 widget 顺序（`Main AMP` 早于 `DSP1`）。两个候选方向未验证成功：
   - 在 `Main AMP` PRE_PMU 里主动把 DSP 路径拉起来（**已失败，破坏恢复能力**）
   - 让 `Main AMP` 的 enable 延后到 POST_PMU（POST_PMU 时 `PWR_CTRL2` 已写）
   目前靠静音预热绕过。
2. **"声音小"的最终归因**：校准/混音器/DSP 限幅三条都已排除（`ATTENUATION≈1.0`、`REDUCE_POWER=0`）。需要主观确认当前音量是否已正常；若仍小，下一步应查 Codec 外的链路：`va_macro`/`rx_macro`、TERT TDM 的 slot/增益、以及 `MultiMedia1` 后端路由（dmesg 曾出现 `no backend DAIs enabled for MultiMedia1`）。
3. **delta 调音文件**（`cirrus,fast-switch`）未实现，若 Android 的音量差异来自每场景调音，则需移植该机制（时序已在 §3.3）。
