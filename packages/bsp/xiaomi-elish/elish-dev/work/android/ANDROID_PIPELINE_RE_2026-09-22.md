# Android 音频管线逆向 —— 本轮实测确证（2026-09-22）

设备：elish（M2105K81AC），**Android 13 在 slot a**，adb root（Magisk，uid=0 context=u:r:magisk:s0）✓
所有结论均来自设备实测（dmesg / logcat / 寄存器 dump），非推测。

---

## 1. 【新】可脚本化的 Android 播放通路（重大突破）

之前无法按需让 Android 出声，现在有了确定配方：

```sh
# 1) 打开 FE→BE 路由（Android 命名：<BACKEND> Audio Mixer <FE>）
su -c 'tinymix "TERT_TDM_RX_0 Audio Mixer MultiMedia1" 1 1'
# 2) 直接播放（card0 dev0 = MultiMedia1）
su -c 'tinyplay /sdcard/Download/sine1k.wav -D 0 -d 0'
```

* `cat /proc/asound/pcm` → `00-00: MultiMedia1` ⇒ `-D 0 -d 0` 正确。
* 只做第 1 步不够：**不设路由**会报
  `KONA Media1: ASoC: no backend DAIs enabled for KONA Media1`
  + `__q6asm_cmd: DSP returned error[ADSP_EFAILED] opcode 68557`。
* 设了路由后内核日志干净：`adm_open … 0x9020` → `adm_close: port_id=0x9020` / `afe_close: port_id = 0x9020`。

### 1.1 tinyplay 通路的**唯一缺口**：放大器不上电
播放中 8 颗 CS35L41 仍是空闲态（`0x2014=0 0x2018=0x20 0x2084=0x2f1aa3`），
即 **ADSP 已在 TDM 上出数据，但放大器的 AMP Enable 没被拉起** ⇒ 无声。
手动补上参考值即可（实测有效，`0x2014=1 0x2018=0x3721` 全部 8 颗）：

```sh
for b in 1 2; do for a in 0x40 0x41 0x42 0x43; do
  ampreg $b $a 0x2014 1; ampreg $b $a 0x2018 0x3721
done; done
```
> 结论：**Android 的 AMP Enable 由 HAL/CSPL（libspkrprot / audio_hw_cspl_cal）拉起，tinyplay 绕过 HAL 所以不会拉。**
> Armbian 侧必须自己拉（或复刻 HAL 的这一步）。

---

## 2. Android 工作态管线的**精确参数**（dmesg 实测）

| 环节 | 实测值 |
|---|---|
| BE hw_params | `msm_be_hw_params_fixup: dai_id= 87, format = 6, rate = 48000`（format 6 = `PCM_24_BIT_PACKED`） |
| 8×CS35L41 | `pcm_startup` / `set_dai_fmt: fmt = 0x4004` / `hw_params: rate=48000, asp_wl=24, asp_width=32` / `set_sysclk freq=12288000` |
| ADM COPP | `adm_open:port 0x9020 path:1 rate:48000 channel_mode:2 perf_mode:0 topology 0x1000a100 bit_width 24 app_type 69940 acdb_id 10011 session_type 0 passthr_mode 0` |
| ASM | `q6asm_get_asm_topology_apptype: popp using topology 0x10be4 app_type 69936` |
| AFE | `afe_get_cal_topology_id: cal_type 8 not initialized for this port 36896`；`AFE set topology id 0x0 enable for port 0x9020 ret -22`；`send_afe_cal_type cal_block not found!!` |
| 关闭 | `ASM_STREAM_CMD_CLOSE (0x10BCD)` 返回 `ADSP_EFAILED` —— 无害（关闭期竞态） |

### 2.1 ★ 结论：**AFE 校准不是必需的**
Android **AFE cal 完全没下发成功**（`cal_type 8 not initialized`、`ret -22`、`cal_block not found`），
但 Android 有声音 ⇒ **AFE_COMMON cal（cal_type 16 / topology cal_type 8）不是出声的必要条件**。
→ 这一整条分支可以从"缺失清单"里划掉，不用再花力气。

### 2.2 `Audio Stream N App Type Cfg` 字段顺序（确证）
源码：`downstream/msm-pcm-q6-v2.c:1967-1988`
```
[0] app_type  [1] acdb_dev_id  [2] sample_rate  [3] be_id  [4] channels
```
Android 实测值：`Audio Stream 0 App Type Cfg = 69940 10011 48000 87 2`
其中 **`be_id = 87` 就是 TDM backend 的 dai_id**（与 §2 的 `dai_id= 87` 互相印证）；
`channels` 写进 `cfg_data.channel`。**注意 be_id 不是端口号**，是 backend DAI 索引。

---

## 3. 【新】Android 每颗放大器的**完整控件清单（46 个/颗）**

`work/android/tm_android_working.txt`（空闲态取值）。BRH 一例：

```
BRH DSP Set CAL_AMBIENT              30
BRH DSP Set CAL_R                    9305     ← 扬声器直流阻抗标定！
BRH DSP Set CAL_STATUS               1
BRH DSP Set CAL_CHECKSUM             9306
BRH Fast Use Case Delta File         BRH-music.txt   ← 固件调音文件
BRH Boost Class-H Tracking Enable    On
BRH Boost Target Voltage             0
BRH Hibernate Force Wake             Off
BRH Digital PCM Volume               0
BRH AMP PCM Gain                     18       ← 增益控件
BRH ASPTX1..4 Slot Position          4 / 4 / 4 / 4
BRH ASPRX1/2 Slot Position           0 / 1
BRH PCM Soft Ramp                    4ms
BRH DSP Booted                       On
BRH CCM Reset                        Off
BRH Force Interrupt                  Off
BRH Fast Use Case Switch Enable      On
BRH Firmware Reload Tuning           Off
BRH Channel Swap                     On
BRH VPBR Config                      33575688
BRH Noise Gate Config                16245
BRH GLOBAL_EN from GPIO Control      Off
BRH Boost Converter Enable           2
BRH AMP Enable                       Off      ← 播放时 = On（= 寄存器 0x2014）
BRH DSP1 Firmware                    Protection
BRH Safety Volume Ramp Status        Off
BRH Manual Ramp Control              Off
BRH Initial Ramp Volume Attenuation  0
BRH Knee Ramp Volume Attenuation     0
BRH Ramp Knee Time                   0
BRH Ramp End Time                    0
BRH Auto Ramp Safety Timeout         0
BRH Audio Output Device              Speaker
BRH DSP1 Preload Switch              On
```
> 八颗前缀：`BRH BLH BRL BLL`（bus1）+ `TRH TLH TRL TLL`（bus2）。
> 这批控件是 **Armbian 侧逐项对齐的检查表**；尤其 `DSP Booted`/`DSP1 Firmware`/`Fast Use Case Delta File`/`DSP Set CAL_R`
> 关系到 CS35L41 内置 DSP 是否真正跑起来（mainline 若无 firmware-name/tuning 就会缺这一层）。

### 3.1 放大器电源态与寄存器的对应（实测）
| 寄存器 | 空闲 | 播放 |
|---|---|---|
| `0x2014` | 0 | **1** |
| `0x2018` | `0x00000020` | **`0x00003721`** |
| `0x2084` | `0x002f1aa3` | `0x002f1aa0` |
| `0x4808` / `0x4840` / `0x6c04` / `0x6808` / `0x8004` | `0x20200000` / 24 / `0x253` / `0x3f75` / 0 | **完全相同** |

---

## 4. 【新·关键】原厂 Android **自己的校准下发是失败的**

logcat（实际播放时，`audio_hw_primary` 走 `low-latency-playback speaker`）：
```
D ACDB-LOADER: ACDB -> ACDB_CMD_GET_AFE_TOPOLOGY_ID
D ACDB-LOADER: ACDB -> GET_AFE_TOPOLOGY_ID for adcd_id 10011, Topology Id 112fc
D ACDB-LOADER: Error: ACDB_CMD_GET_AFE_INSTANCE_COMMON_TABLE_SIZE Returned = -19
E ACDB-LOADER: [acdb_loader_adsp_set_audio_cal] active device/stream not found
               (result=-100) for topology 0x1000a100 and apptype 0x11134   ← 69940
E ACDB-LOADER: [acdb_loader_adsp_set_audio_cal] set parameters failed with status -100
D msm8974_platform: Setting audio calibration for snd_device(2) acdb_id(10011)
D audio_hw_primary: out_write: retry previous failed cal level set
```
* `apptype 0x11134 = 69940`、`0x11131 = 69937`，拓扑 `0x1000a100 / 0x1000a106`。
* 失败原因：**"active device/stream not found"** —— HAL 在流真正 active 之前就下发 level cal。
* HAL 用 `out_write: retry previous failed cal level set` 在后续 write 里重试。

⇒ 两条重要推论：
1. **"声音小"很可能就出在这里**：level cal（音量标定）没设上，DSP 用的是默认档。
2. 这条失败发生在**原厂 Android 上**，不是我们改坏的 —— 属于原生缺陷（或时序竞态）。

---

## 5. 本轮产出文件
| 文件 | 说明 |
|---|---|
| `work/android/dump_amps.sh` | dump 8 颗 × 27 关键寄存器 |
| `work/android/play_and_sample.sh` | 播放中采样寄存器 + dmesg |
| `work/android/play_pup.sh` | 播放 + 手动拉 AMP Enable |
| `work/android/audible_test.sh` | 长音频可听测试（nice.wav） |
| `work/android/reg_android_idle_now.txt` | 空闲态实测 |
| `work/android/reg_playing_now.txt` | tinyplay 播放中实测 |
| `work/android/dmesg_playing.txt` | 播放期完整内核日志 |

## 6. 待确认（需要人耳）
按 §1 配方 + 手动 AMP Enable 播放 `nice.wav` 时，**扬声器是否真的出声**？
- 若**有声** ⇒ ADSP→TDM→CS35L41 全链路在 Android 上可脚本复现，Armbian 只差"HAL 那几步"。
- 若**无声** ⇒ 缺口在 CSPL/DSP 保护链路（`libspkrprot` / `audio_hw_cspl_cal`），需继续逆 CSPL。
