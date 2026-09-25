# 键盘修复：`keyboard-default-state` 必须挂在 `&lpass_tlmm`

**问题**：小米平板 5 Pro（elish）的磁吸键盘（USB HID `3206:3ffc`）每 1–2 秒断电重连。

**根因（源码级，2026-09-23 定位）**：内核 DTS 里键盘的 pinctrl 状态挂错了 pinctrl 控制器。

```dts
/* 错误（我们本地树被改坏的状态） */
&tlmm {                                  /* = pinctrl@f100000，主 TLMM */
	keyboard_en_state: keyboard-default-state {
		pins = "gpio9";
		function = "i2s1_data";          /* ✗ 主 TLMM 没有 i2s1_data 这个 function */
		drive-strength = <8>;
		bias-pull-up;
	};
};

/* 正确（Armbian 上游补丁 0016 的原意） */
&lpass_tlmm {                            /* = pinctrl@33c0000，LPASS LPI */
	keyboard_en_state: keyboard-default-state {
		pins = "gpio9";
		function = "i2s1_data";          /* ✓ LPASS LPI 有这个 function */
		drive-strength = <8>;
		bias-pull-up;
	};
};
```

**故障链**：
1. `keyboard-default-state` 挂在主 TLMM（`f100000`）下 ⇒ 内核启动时
   `sm8250-pinctrl f100000.pinctrl: invalid function i2s1_data in map table` ✗
2. 该状态永不生效 ⇒ 键盘的 `gpio9`（I2S1 数据线）从未被正确 mux ✗
3. 消费者 `usb_2_hsphy`（`phy@88e4000`，`pinctrl-0 = <&keyboard_en_state>, <&keyboard_vdd_pin>`）
   拿不到有效引脚配置 ⇒ USB2 PHY 侧电气不稳 ⇒ **键盘 D+ 上拉消失、端口级重枚举** ✓
4. 实测 dmesg（每次都在 6–7 秒后断开）：
   ```
   [    0.976706] usb 1-1: new full-speed USB device number 2 using xhci-hcd
   [    7.729929] usb 1-1: USB disconnect, device number 2
   [   71.262798] usb 1-1: new full-speed USB device number 3 using xhci-hcd
   [   77.992764] usb 1-1: USB disconnect, device number 3
   ```

**旁证（Android 原版对照）**：Android 的 `dtbo.img` 用 DTBO overlay 配置键盘，
其 `lpi_i2s1_sd1` 状态明确位于 `lpi_pinctrl@33c0000`（LPASS LPI）✓ ——
**Android 从来没把它放到主 TLMM 上**。另有专用驱动 `xiaomi,keyboard`
（rst=141 / irq=83 / vdd=127）走主 TLMM 的普通 GPIO，与 I2S1 数据线是两回事。

---

## 修复

**一行改动**：`sm8250-xiaomi-elish-common.dtsi` 里把 `&tlmm` 改回 `&lpass_tlmm`。

补丁：`work/kernel/patches/0055-elish-keyboard-pinctrl-under-lpass-tlmm.patch`

### 可复现构建流程

```bash
K=/home/axis/axis_rnd/work/kernel/build/linux-6.12.58

# 1) 确认源码是修好的状态（应为 &lpass_tlmm）
grep -n -A3 '^&lpass_tlmm {' $K/arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-common.dtsi

# 2) 用内核构建系统编译 DTB（不要用 dtc 反编译再编译——见下方"坑"）
cd $K && make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    qcom/sm8250-xiaomi-elish-csot.dtb

# 3) 校验键盘节点归属（必须是 pinctrl@33c0000）
dtc -I dtb -O dts $K/arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-csot.dtb \
  | grep -B5 keyboard-default-state | grep 'pinctrl@'

# 4) 打成 ABL 能接受的启动镜像（l10c 内核+ramdisk + 新 DTB）
python3 work/kernel/build_image.py     # 见下方说明
```

产物（已存档）：
- `work/kernel/artifacts/sm8250-xiaomi-elish-csot.kbd-fixed.dtb`（130436 B）
- `work/kernel/artifacts/boot_b_kbd_fixed.img`（192 MiB，md5 `e96de85a19463c34a4410aae53ea4b22`）

镜像内部组合（**关键：内核/ramdisk 保持"已证明能启动"的那一套，只换 DTB**）：

| 组件 | 来源 | 校验 |
|---|---|---|
| 内核 | `/tmp/l10c/Image`（l10c 构建，42.5 MB） | 与原镜像逐字节一致 ✓ |
| ramdisk | `/tmp/l10c/ramdisk.gz`（43.7 MB，与内核同构建） | 与原镜像逐字节一致 ✓ |
| DTB | 内核源码新编译（键盘在 `pinctrl@33c0000`） | 与编译产物一致 ✓ |
| cmdline | `root=UUID=21ce0d2d-… slot_suffix=_b` | ✓ |

布局必须是 `gzip(Image) || raw DTB` 且 DTB **计入 `kernel_size`**（ABL 的 `Image.gz-dtb` 约定）。

---

## 踩过的坑（重要，避免重复）

1. **不要用 `dtc` 反编译 DTB 再改再编译** ✗
   实测：把 l10c DTB 反编译→挪一个节点→重编译，编译干净、结构正确，
   但刷进去**无法启动** ✗。推测是 phandle 重编号 / 格式变化破坏了 ABL 的 DT fixup。
   **正确做法：改 DTS 源码，用内核构建系统编译** ✓。

2. **不要往 `dtbo_b` 塞自建 DTBO** ✗
   实测：把自定义 DTBO 写进 `dtbo_b` 后，**任何镜像都起不来**（含原本能启动的 l10c）✗。
   正确状态是 **`dtbo_b` 全零**（`fastboot erase dtbo_b`）✓ —— 恢复全零后立刻能启动 ✓。

3. **不要混合不同构建的组件** ✗
   `boot_b_kbd2/kbd3` 用 rootfs 内核 + l10c ramdisk ⇒ 起不来 ✗。
   内核与 ramdisk 必须同构建。

4. **`fastboot boot` 对 192 MB 镜像不可用** ✗
   必然报 `usb_read failed (31)`，设备会落回正常启动流程（表现为"怎么启动到 Android 去了"）。
   验证镜像请用 `fastboot flash` + `reboot`。

5. **`logfs`（ABL 日志）会被 `fastboot boot ofrp.img` 重写** ✗
   要读某次失败启动的 ABL 日志，必须"**先让它失败 → 再进 recovery 读**"。

---

## 修复后的验证判据

```bash
# 1) 那条报错应消失
dmesg | grep "invalid function"          # 期望：无输出

# 2) 键盘应稳定在线（连续采样不消失）
for i in $(seq 1 10); do lsusb | grep -qi 3206 && echo "在线"; sleep 2; done

# 3) 不再有周期性断连
dmesg | grep "usb 1-1" | tail   # 期望：只有一次枚举，无 disconnect 循环

# 4) 敲键有事件
evtest /dev/input/eventX          # 或
cat /dev/input/eventX | xxd | head
```
