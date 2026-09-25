# Armbian 内核补丁序列（elish 音频）

目标树：`work/kernel/build/linux-6.12.58/`（pristine，v6.12.58）
**必须按序应用**（0001 与 0002 都改 `q6routing.c`）：

```sh
cd <kernel-tree>
patch -p1 < 0001-q6adm-add-copp-master-gain-volume.patch
patch -p1 < 0002-q6asm-add-stream-volume.patch
patch -p1 < 0003-q6routing-configurable-copp-topology.patch
```

验证结果（实测）：

| 补丁 | 单独 dry-run | 序列内 |
|---|---|---|
| 0001 | PASS | PASS |
| 0002 | **FAIL**（依赖 0001 在 q6routing.c 的改动） | PASS |
| 0003 | PASS | PASS |

---

## 0001 — ADM COPP 主音量
`q6adm.c/h` + `q6routing.c`：新增 `q6adm_set_volume()`（`ADM_CMD_SET_PP_PARAMS_V5/V6`，
module `0x10BFE`=`AUDPROC_MODULE_ID_VOL_CTRL`，param `0x10BFF`=`VOL_CTRL_MASTER_GAIN`，
payload `{u16 gain_q13, u16 rsvd}`，Q13：`0x2000`=1.0=0dB），并导出 ALSA 控件。
**注意：只能衰减，0dB 是上限**（实测 `0x0100`⇒静音，`0x4000/0x6000/0x8000`⇒无变化）。

## 0002 — ASM 流音量
`q6asm.c/h` + `q6asm-dai.c` + `q6routing.c/h`：给 mainline 补上**完全缺失的 ASM 流音量**
（下游 `q6asm.c:8850 __q6asm_set_volume()` / `SOFT_VOLUME_INSTANCE_1` → module `0x10BFE` instance 0；
opcode `ASM_STREAM_CMD_SET_PP_PARAMS_V3 = 0x0001320D`）。
**寻址已实测确认**：`src_port = dest_port = (session<<8)|stream_id`（session1/stream1 ⇒ `0x0101`），
`token = session`；必须走 `aprsvc:service:4:7`（ASM），ADM 走 `aprsvc:service:4:8`。
> 这条是"AUDIO 流级增益"的来源；mainline 原本一点都没有。

## 0003 — 【新】COPP topology 可配置（关键）
`q6routing.c`：mainline 把 COPP topology **硬编码**为 `NULL_COPP_TOPOLOGY`（`q6routing.c:393`，
`q6adm.h:8 = 0x00010312`），而 **Android 用真实 topology `0x1000a100`** 打开扬声器 COPP
（实测 dmesg：`adm_open:port 0x9020 … topology 0x1000a100 bit_width 24 app_type 69940 acdb_id 10011`）。

硬编码 NULL 意味着 ADSP 里**从不实例化**扬声器那套 audproc 链（音量/保护/IIR/CS35L41 相关处理）。
本补丁加一个**运行时可调的模块参数**，默认值不变（**零回归风险**）：

```sh
# 测试（播放前设置即可）
echo 0x1000a100 > /sys/module/q6routing/parameters/copp_topology
# 或内核 cmdline：q6routing.copp_topology=0x1000a100
# 复位回上游行为：
echo 0 > /sys/module/q6routing/parameters/copp_topology
```

### 0003 的测试顺序（重要）
`topology 0x1000a100` 的定义本应由 ACDB 的 `ADM_TOPOLOGY_CAL`(cal_type 9) 经
`ADM_CMD_ADD_TOPOLOGIES (0x00010335)` 在运行时"添加"进 ADSP。Armbian 没有这一层，
所以存在两种可能，**必须实测区分**：
1. ADSP 固件内置了该 topology ⇒ 直接 `echo 0x1000a100 …` + 播放即可出声/变响；
2. ADSP 不认识该 id ⇒ COPP open 失败 ⇒ **会更彻底地没声音**（此时 `echo 0` 立即回滚）。
若为 (2)，则必须先由 `elish_acdb_cal` 模块下发 `ADM_CMD_ADD_TOPOLOGIES`，再开 COPP。

**所以 0003 与 ACDB 下发是耦合的，且 0003 提供了一个"一分钟就能判定"的实验开关。**

## 0054-cs35l41-do-not-consume-one-shot-pup-pdn-status.patch  【第 68/79 轮，已验证】
**问题**：8 颗 CS35L41 全部 `Enable(1) failed: -110`，`PRE_PMU ... failed`，
`PWR_CTRL1(0x2014)` 播放中仍为 0 ⇒ 放大器不上电 ⇒ 完全没声。
**根因（在设备自己的硅片上实测）**：`CS35L41_IRQ1_STATUS1` 的 PUP_DONE/PDN_DONE 是
**一次性粘滞位**：被清掉一次之后，后续 `GLOBAL_EN 0->1` **不会再置位**。
主线在每次 enable 成功后都 `regmap_write(IRQ1_STATUS1, mask)` 清掉它
⇒ 之后每次 enable 都在等一个永不出现的边沿 ⇒ 必然 `-110`。
Android 厂商驱动**从不清除**该位，所以它的轮询永远"已满足"。
**改法**：`SHD_BOOST` 与 `INT_BOOST` 两分支 —— ①不再清除粘滞位；②轮询超时由
`dev_err + 上抛 -110` 改为 `dev_warn` + `ret=0` 继续；③超时 100 ms → 20 ms
（避免 8 颗串行 8 秒）。
**效果（实测）**：`Enable(1) failed` 24 → **0**；`PRE_PMU failed` 8 颗全失败 → **0**；
播放中 `0x2014` 0/8 → **8/8 = 1**。
**产物**：`snd-soc-cs35l41-lib.ko`（模块，仅需换 .ko + `depmod -a` + 重启，无需刷内核）。
