# Xiaomi Mi Pad 5 Pro (elish / SM8250-AC) 电池·电源模式·频率 排查报告

设备：`Xiaomi Mi Pad 5 Pro (CSOT)`，Armbian 26.8.1，内核 `6.12.58-current-sm8250`，
GNOME Shell 50.1，upower 1.91.1，power-profiles-daemon 0.30。

## 结论速览

| 你期望的 | 现状 | 根因 |
|---|---|---|
| 电池功率可见 | **已可用**：upower 汇总两节电芯 `0.40 W` | — |
| 充电状态 / 充电功率 | **缺失** | DT 里 `charger@1000` 是 `status = "disabled"`，SMB5 充电器驱动永远不 probe |
| Settings 里有 performance 电源模式 | **缺失**（只有 balanced / power-saver，且都是空实现） | ppd 在 ARM64 上只能回退到 `placeholder` 后端 |
| 超大核跑 3.2GHz | **被砍到 2.84GHz** | DT `cpu7_opp_table` 缺 3187200 档，被 cpufreq 驱动交叉校验丢弃 |

---

## 1. 电池：驱动是好的，缺的是充电器

两片 TI BQ27Z561 电量计挂在 I2C 上，`bq27xxx_battery` 正常驱动：

```
/sys/class/power_supply/bq27z561-0   POWER_SUPPLY_TYPE=Battery  CAPACITY=66  CURRENT_NOW=-109000
/sys/class/power_supply/bq27z561-1   POWER_SUPPLY_TYPE=Battery  CAPACITY=65  CURRENT_NOW=-111000
```

upower 两个电芯 + DisplayDevice 都在，合计 `energy 18.29 Wh / energy-rate 0.40 W`，
`/sys/class/power_supply/bq27z561-0/` 下**没有** `power_now`，只有 `power_avg`，
upower 的 `energy-rate` 是它用 `voltage_now × current_now` 算出来的。

> 顺带纠正一个概念：`power_supply` 是 **sysfs class + uevent**，不是字符设备。
> GNOME/upower 走的是 `org.freedesktop.UPower` D-Bus，不存在 `/dev/battery*` 这种节点。
> 另外 GNOME 自带界面只显示电量和百分比，不显示瓦数；瓦数要在 `upower -d` 或
> Power Statistics 里看。

真正缺的是**充电器**：

```
# 设备树里节点存在，但被显式关掉
/sys/.../pmic@2/charger@1000: compatible=qcom,pm8150b-charger   status=disabled

# 驱动其实编好了（CONFIG_CHARGER_QCOM_SMB5=m），但没设备就不会 autoload
/lib/modules/$(uname -r)/kernel/drivers/power/supply/qcom_pm8150b_charger.ko
alias: of:N*T*Cqcom,pm8150b-charger
$ lsmod | grep pm8150b     -> 空

# 后果
upowerd: Conflicting charge/discharge state between batteries!
upower daemon: on-battery: no      # 但两节电芯都在放电
```

上游 `pm8150b.dtsi` 默认把该节点标为 disabled，需要板级 DTS 打开；elish 的 DTS 从没打开过
（上游一直到 2026 年才在 LKML 上把 SMB5 支持合进 `qcom_smbx`：
<https://lkml.org/lkml/2026/9/5/106>，此前没有驱动可用，所以没人开这个节点）。

**要补齐充电信息需要**（两处都要）：
1. `charger@1000` 改 `status = "okay"`；
2. 给它加 `monitored-battery`，否则旧版 `qcom_pm8150b_charger` 里这一句会失败：
   `power_supply_get_battery_info(chg_psy, ...)` → `Failed to get battery info` → probe 退出。

> 注：`battery-l` / `battery-r` 这两个 `simple-battery` 节点只有 `voltage-min-design`、
> `energy-full-design`、`charge-full-design`，**没有** `constant_charge_voltage_max`，
> 所以驱动会跳过浮充电压编程（打印 "No battery float voltage; preserving hardware setting"），
> 不会乱改充电上限 —— 这点对安全是有利的。

---

## 2. 电源模式：ppd 在 ARM64 上没有可用后端

```
$ powerprofilesctl
* balanced:      PlatformDriver: placeholder
  power-saver:   PlatformDriver: placeholder
# 注意：performance 根本没出现

$ busctl --system get-property net.hadess.PowerProfiles /net/hadess/PowerProfiles \
      net.hadess.PowerProfiles Profiles
aa{sv} 2 ...   # 只有两个 profile
```

`power-profiles-daemon` 0.30 只认这三种后端，二进制里的路径可以佐证：

```
/sys/firmware/acpi/platform_profile        <- 需要 ACPI，ARM64 没有
/sys/devices/system/cpu/intel_pstate/...   <- Intel 专用
/sys/devices/system/cpu/amd_pstate/status  <- AMD 专用
```

本机 `scaling_driver = qcom-cpufreq-hw`、`scaling_governor = schedutil`、
`energy_performance_preference` 为空（无 EPP），`/sys/class/platform_profile` 不存在，
所以 ppd 只能选 **placeholder**：对外只提供 balanced / power-saver，且**什么都不做**。
GNOME 设置里的「电源模式」因此没有 Performance，选项也是摆设。

可用修法（推荐第 1 个）：

1. **换 tuned + tuned-ppd**（仓库里有 `tuned-ppd 2.25.1`，`Conflicts: power-profiles-daemon`、
   `Provides: power-profiles-daemon`）：它对外提供同一个 `net.hadess.PowerProfiles` D-Bus 接口，
   GNOME 设置里就会出现 Performance / Balanced / Power Saver，`throughput-performance`
   会把 governor 切到 `performance`。
2. 自己写一个小的 ppd 兼容 D-Bus 服务，直接按 profile 写 `scaling_governor`。
3. 不做 UI，只手动 `cpufreq-set -g performance`（或开机用 tuned/cpufrequtils 固定）。

已验证 `performance` governor 真的有效（改完即还原）：

```
policy0: gov=performance cur=1804800   # 1.80GHz
policy4: gov=performance cur=2419200   # 2.42GHz
policy7: gov=performance cur=2841600   # 2.84GHz  <-- 注意，不是 3.2GHz
```

---

## 3. 超大核为什么只能到 2.84GHz

`dmesg` 里的决定性证据：

```
[    0.256699] cpu cpu7: failed to update OPP for freq=3187200
[    0.256851] cpu cpu7: failed to update OPP for freq=3187200
```

也就是说**硬件频率 LUT 里有 3187200 kHz（3.1872GHz，即骁龙 870 标称的 3.2GHz）**，
是内核把它丢掉的。

机制在 `drivers/cpufreq/qcom-cpufreq-hw.c`：

```c
ret = dev_pm_opp_of_add_table(cpu_dev);
if (!ret) {
        /* Disable all opps and cross-validate against LUT later */
        ...
}
...
        if (freq != prev_freq && core_count != LUT_TURBO_IND) {
                if (!qcom_cpufreq_update_opp(cpu_dev, freq, volt)) {
                        table[i].frequency = freq;
                } else {
                        dev_warn(cpu_dev, "failed to update OPP for freq=%d\n", freq);
                        table[i].frequency = CPUFREQ_ENTRY_INVALID;
                }
        }
```

即：**频点表 = 硬件 LUT ∩ DT OPP 表**。DT 里没有的频点连电压都不用，直接 INVALID。
电压本身来自硬件 LUT（`volt = FIELD_GET(LUT_VOLT, ...)`），DT 的 OPP 条目里
一个 `opp-microvolt` 都没有。

而上游 `sm8250.dtsi` 的 `cpu7_opp_table` 止于 `opp-2841600000`，是 **SM8250（骁龙 865）
的档位**；上游没有任何 `sm8250-ac` 变体表（6.12 和 6.18 都一样），
所以骁龙 870 的机器在主线上都会丢掉这一档：

```
cpu7_opp1  = 844800000
...
cpu7_opp20 = 2841600000     <- 表到此为止，缺 3187200000
```

SoC 自报也印证了这点：`/sys/devices/soc0/{machine=SM8250, revision=2.1}`，
而 870 的硬件 LUT 有第 21 档。可用频点实测：

```
policy0 max = 1804800 (1.80G)   # 银核，符合 870
policy4 max = 2419200 (2.42G)   # 金核，符合 870
policy7 max = 2841600 (2.84G)   # 超大核，被砍（应为 3187200）
```

### 补丁

只往 `cpu7_opp_table` 里加一段，电压交给硬件 LUT（和其余 20 档保持一致）：

```dts
opp-3187200000 {
        opp-hz = <0x00 0xbdf8d000>;              /* 3187200000 */
        opp-peak-kBps = <0x7faf80 0x3138000>;    /* 与 2.84G 档相同 */
};
```

本目录里的 `sm8250-xiaomi-elish-{csot,boe}.dtb` 就是打好这个补丁的 DTB
（csot/boe 两个变体各 87 → 88 个 OPP 条目，diff 只有这一个节点）。
DTS 全文见 `csot-补丁后.dts.txt`。

### 应用 / 回滚

```
sudo ./apply-3g2-opp.sh      # 备份 + 覆盖 DTB + 重新生成 boot.img + 写入 boot_b
sudo reboot
```

开机后验证：

```
cat /sys/devices/system/cpu/cpufreq/policy7/scaling_available_frequencies | tr ' ' '\n' | tail -3
# 期望看到 ... 2745600 2841600 3187200
sudo dmesg | grep 'failed to update OPP'     # 期望无输出
```

### 注意

* 刷的是 `boot_b`（`armbianEnv.txt: abl_boot_partition_label=boot_b`），
  脚本会先备份 DTB；回滚方法见脚本结尾输出。
* 这一步改的是 `boot` 分区，必须重启才生效；重启前无法验证。
* 3.1872GHz 是该 SoC 为该 bin 熔丝写好的合法档位、电压也由硬件 LUT 给出，
  但整机功耗/温度会上去。cpu7 有 `cpu7-top/bottom-thermal` 热区和
  `cpufreq-cpu7` 冷却设备在做温控，属于受管状态。
* 系统 OTA 升级内核/DTB 包会覆盖 `/usr/lib/linux-image-*/qcom/*.dtb`，补丁会丢，
  需要重新执行脚本。

---

## 4. 已实施：ppd → tuned-ppd（2026-09-23 02:45）

只做了这一项，3.2GHz 与充电器补丁**未**应用。

```
tuned       2.25.1-1ubuntu1   已安装 / enabled / active
tuned-ppd   2.25.1-1ubuntu1   已安装 / enabled / active
power-profiles-daemon 0.30-2  已移除（包状态 rc，仅残留配置）

net.hadess.PowerProfiles  ->  tuned-ppd (PID 20128)
Profiles: power-saver / balanced / performance      Driver = tuned
```

实测三种模式（`busctl --system set-property ... ActiveProfile s <name>`）：

| GNOME 选项 | tuned profile | policy0 / 4 / 7 governor |
|---|---|---|
| performance | throughput-performance | `performance`（跑满 1.80 / 2.42 / 2.84 GHz） |
| balanced | balanced | `schedutil`（内核默认） |
| power-saver | powersave | 仍是 `schedutil` ⚠️ |

`performance` 是真生效的。`power-saver` 在 ARM 上基本只有 sysctl 效果：tuned 的
`powersave` profile 写的是 `governor=schedutil|conservative|powersave`，候选表把
`schedutil` 排在第一位，而本机既无 EPP、又无 ACPI `platform_profile`、也没有
`boost` 旋钮，所以 CPU 侧没有变化。要让它真的降频，加一个覆盖：

```ini
# /etc/tuned/powersave/tuned.conf
[cpu]
governor=powersave
```

选择结果会持久化，重启后 tuned 自动恢复：

```
/etc/tuned/active_profile   = balanced
/etc/tuned/ppd_base_profile = balanced
/etc/tuned/profile_mode     = manual
```

两点附带影响：

* `powerprofilesctl` 命令随 ppd 包一起消失；改用 `tuned-adm active`，或
  `busctl --system get-property net.hadess.PowerProfiles /net/hadess/PowerProfiles \
   net.hadess.PowerProfiles ActiveProfile`。
* 日志里有一条无害报错：切换 profile 时 tuned 执行 `modprobe -r cpufreq_conservative`
  失败，该模块是 builtin（见 `/var/log/tuned/tuned.log`）。

### 回滚

```bash
sudo apt-get install -y power-profiles-daemon   # Conflicts 会自动移除 tuned-ppd
sudo apt-get purge -y tuned tuned-ppd
sudo systemctl enable --now power-profiles-daemon
```

---

## 5. 右上角调速器 + 关屏按钮

### 5.1 右上角菜单里的调速器：GNOME 自带，现在可用了

GNOME Shell 50 **本来就有**这个控件，JS 在
`libshell-18.so` 的 gresource 里：`/org/gnome/shell/ui/status/powerProfiles.js`，
由 `panel.js` 无条件创建并塞进 Quick Settings。关键三行：

```js
const BUS_NAME = 'org.freedesktop.UPower.PowerProfiles';   // 注意不是 net.hadess
this.visible = this._proxy.g_name_owner !== null;          // 名字有 owner 就显示
this.menuEnabled = this._profileItems.size > 2;             // >2 个 profile 才能展开
```

也就是说：**它需要至少 3 个 profile 才能展开菜单**。换 tuned-ppd 之前 ppd 只给
`balanced` / `power-saver` 两个，所以那一格点不开；现在有
`performance` / `balanced` / `power-saver`，展开就是三个选项。

运行中的 shell 确实在同步（旁证）：

```
$ gsettings get org.gnome.shell last-selected-power-profile
'power-saver'     # 我们测试时切过，shell 侧把它记下来了
```

为了让**当前这个** session 立刻拿到新的 profile 列表（不必注销），额外重启了一次
tuned-ppd：它的 D-Bus 名字消失再出现，会迫使 shell 的 GDBusProxy 重新拉一遍
`Profiles`，从而重跑 `_syncProfiles()`。

### 5.2 三个档位现在都有真实作用

```
GNOME=performance  -> tuned=throughput-performance
    policy0  gov=performance  cur=1804800  max=1804800
    policy4  gov=performance  cur=2419200  max=2419200
    policy7  gov=performance  cur=2841600  max=2841600
GNOME=balanced     -> tuned=balanced
    policy0/4/7  gov=schedutil   （内核默认，动态调频）
GNOME=power-saver  -> tuned=powersave
    policy0  gov=powersave  cur=300000     （银核压到 300MHz）
    policy4  gov=powersave  cur=710400
    policy7  gov=powersave  cur=844800
```

原来 `power-saver` 是**空转**：系统自带的 `/usr/lib/tuned/profiles/powersave/tuned.conf`
写的是 `governor=schedutil|conservative|powersave`，tuned 取**第一个可用**的，
本机有 `schedutil` 所以永远选它；而 SM8250-AC 上既没有 EPP
（`energy_performance_preference` 为空）、也没有 ACPI `platform_profile`、
也没有 `boost` 旋钮，所以 CPU 侧完全没有变化。

修法：tuned **不合并** `/etc/tuned/profiles/<name>` 和 `/usr/lib/tuned/profiles/<name>`
（`profiles/locator.py` 的 `get_config()` 只返回找到的第一个 `tuned.conf`），
所以直接覆盖同名 profile，内容照抄系统版、只改 governor：

```
/etc/tuned/profiles/powersave/tuned.conf     # [cpu] governor=powersave
```

（`/etc/tuned/profiles/` 是 tuned 的用户 profile 目录，见
`tuned/consts.py: CFG_DEF_PROFILE_DIRS`。校验：`tuned-adm profile_info powersave`。）

### 5.3 关屏按钮

GNOME 50 的 Quick Settings 底部一排是「电池 / 截图 / 设置 / 锁屏 / 关机」（`ui/status/system.js`
里的 `SystemItem`），**没有关屏**；而且本机
`org.gnome.desktop.session idle-delay = 0`（从不自动关屏），所以这个按钮确实有用。

机制（已实测到 KMS 层）——写 mutter 的 `org.gnome.Mutter.DisplayConfig.PowerSaveMode`：

```
PowerSaveMode=1 -> /sys/class/drm/card0-DSI-1/dpms=Off, enabled=disabled
PowerSaveMode=0 -> dpms=On, enabled=enabled
```

**坑（实测出来的）**：从**外部**置 1 之后，mutter 和 gsd-power 都**不会**在输入时把它恢复，
屏幕会一直黑着 —— 用 `/dev/uinput` 注入真实按键验证过，3 秒后 `dpms` 仍是 `Off`。
（gsd-power 里有 `gsd_display_config_set_power_save_mode`，但它只在自己空闲状态发生
跳变时才写这个属性，我们的外部写入没经过它的状态机。）

所以唤醒必须自己做：注册 `org.gnome.Mutter.IdleMonitor.AddUserActiveWatch`，
输入一到就把 `PowerSaveMode` 置回 0。这条路径也用真实按键验证过：

```
$ screen-off            # 关屏
$ /sys/class/drm/card0-DSI-1/dpms = Off
# —— 按一下键 ——
$ /sys/class/drm/card0-DSI-1/dpms = On      # 自动恢复
```

交付物：

| 东西 | 位置 | 状态 |
|---|---|---|
| GNOME 扩展（Quick Settings 里的「关屏」磁贴） | `~/.local/share/gnome-shell/extensions/elish-screen-off@axis.local/` | 已装、已写进 `enabled-extensions`，**需注销后生效** |
| 命令行版 | `/usr/local/bin/screen-off` | **现在就能用**（已实测） |
| 兜底恢复 | `/usr/local/bin/screen-on` | 已实测 |

扩展里 `Main.panel.statusArea.quickSettings.addExternalIndicator()` 是公开 API；
图标 `video-display-symbolic`；标题按 `GLib.get_language_names()` 在
`关屏` / `Screen Off` 之间切换（本机没装 msgfmt，没用 gettext）。

⚠️ **扩展必须注销/重登录才会加载**：GNOME 50 的 `ExtensionManager` 只在启动时
`_loadExtensions()` 扫描一次目录，`EnableExtension` 对未知 uuid 直接
`return false`（实测 `gnome-extensions enable elish-screen-off@axis.local`
报「扩展不存在」）。Wayland 下没有原地重启 shell 的办法。

⚠️ 注销会一并结束 `dsh web`（`loginctl show-user axis -p Linger` = `no`），
重登后需要重新启动它。

### 5.4 「注销」按钮默认是隐藏的 —— 已打开

GNOME 50 的关机菜单里，注销项的可见性由 `misc/systemActions.js` 的
`_updateLogout()` 决定：

```js
const visible = allowLogout &&
                (alwaysShow || multiUser || multiSession || systemAccount || !localAccount) &&
                shouldShowInMode;
```

单本地用户 + 单会话时后面那一串全为假，而 `alwaysShow` 取自
`org.gnome.shell always-show-log-out`（默认 **false**），
所以菜单里**看不到「注销」**，只剩 挂起 / 重启 / 关机。
（`org.gnome.desktop.lockdown disable-log-out` 是 false，不是它挡的。）

修法：

```bash
gsettings set org.gnome.shell always-show-log-out true
```

`systemActions.js:187` 对 `changed::always-show-log-out` 有监听并调 `_updateLogout()`，
所以**不用重启/注销，立刻生效**。位置：Quick Settings 右下角的 ⏻ 按钮 →
菜单分隔线下面就是「注销 / 切换用户」。

命令行等价物（按钮之外的兜底）：

```bash
gnome-session-quit --logout          # 加 --no-prompt 跳过确认
```

### 5.5 卸载

```bash
gsettings set org.gnome.shell enabled-extensions \
  "$(gsettings get org.gnome.shell enabled-extensions | sed "s/'elish-screen-off@axis.local', //")"
gsettings set org.gnome.shell always-show-log-out false   # 若想恢复默认
rm -rf ~/.local/share/gnome-shell/extensions/elish-screen-off@axis.local
sudo rm -f /usr/local/bin/screen-off /usr/local/bin/screen-on
```

---

## 6. 与下游（小米 OSS / LineageOS）的对照

### 6.1 下载了什么

* 小米官方开源：`MiCode/Xiaomi_Kernel_OpenSource`，分支 `elish-r-oss`，
  commit `52a2f18`「Kernel: Xiaomi kernel changes for Xiaomi Pad 5 Pro and Xiaomi
  Pad 5 Pro 5G Android R」。
  用 blob 过滤 + 稀疏检出（只要 `drivers/power/supply`、`drivers/usb/pd`、
  `drivers/usb/typec`、`drivers/cpufreq`、`arch/arm64/configs/vendor`），29 MB：
  ```
  /home/axis/src/xiaomi-elish-kernel
  ```
  ⚠️ **小米这份没有发布 vendor DTS**：`arch/arm64/boot/dts/vendor/` 整个不存在
  （`arch/arm64/boot/dts/` 只有 424 个上游文件，没有一个叫 elish/enuma）。
  所以 DTS 对照用的是 LineageOS `android_kernel_xiaomi_sm8250` (lineage-21) 的
  `elish-sm8250.dtsi`（它就是 vendor 树里的那份）：
  ```
  /home/axis/src/elish-downstream-refs/{elish-sm8250.dtsi,downstream-pm8150b.dtsi,
                                        xiaomi-qcom-cpufreq-hw.c,lineageos-qcom-cpufreq-hw.c}
  ```
* 关键 defconfig：`arch/arm64/configs/vendor/elish_user_defconfig`。

### 6.2 PD 充电：我们缺的是「执行侧」

小米 elish 的充电栈（defconfig）：

```
CONFIG_QPNP_SMB5=y                     # PM8150B SMB5 主充电器
CONFIG_QPNP_USB_PDPHY=y                # PM8150B PD PHY (qcom,qpnp-pdphy@1700)
CONFIG_DUAL_FUEL_GAUGE_BQ27Z561=y      # 双 BQ27Z561 电量计（小米双 gauge 胶水）
CONFIG_DUAL_BQ2597X=y
CONFIG_BQ2597X_CHARGE_PUMP=y           # BQ2597x 充电泵
CONFIG_CHARGER_LN8000=y                # LN8000 充电泵
CONFIG_QPNP_QNOVO5=y
```

| 环节 | 下游（小米 / LineageOS） | 我们（主线 6.12） |
|---|---|---|
| 主充电器 SMB5 | `&pm8150b_charger { status="ok"; qcom,usb-icl-ua=<6200000>; qcom,fcc-max-ua=<12400000>; qcom,fv-max-uv=<4500000>; qcom,step-charging-enable; qcom,sw-jeita-enable; qcom,thermal-mitigation… }` | 驱动是额外补进来的（`qcom_pm8150b_charger.ko`，**vanilla 6.12/6.18 里都没有**），但 DT `charger@1000` = **disabled** → SPMI 设备节点都不创建 → 模块永不加载 |
| PD PHY | `qcom,qpnp-pdphy@1700`，靠 `qcom,usbpd-phandle = <&pm8150b_pdphy>` 绑到充电器 | ✅ **有**：主线把 PD PHY 并进 `qcom_pmic_typec`（`reg=<0x1500>,<0x1700>`），实测 `port0/usb_power_delivery_revision = 3.0`，PD 3.0 在正常协商 |
| PD→充电 联动 | `smb5-lib.c:6671` 读 `qcom,usbpd-phandle`，按 `POWER_SUPPLY_PD_PPS_ACTIVE` 调 ICL；`pd_policy_manager.c`（`xiaomi,usbpd-pm`）做 PD 策略 | ❌ 我们的 SMB5 模块里连 `usbpd` 字符串都没有；**上游 6.18 的 `qcom_smbx.c` 也没有** PD 集成 |
| 高功率快充 | 2× `lionsemi,ln8000`（primary/secondary）+ `ti,bq2597x-master/slave` 充电泵，`charger_name="tertiary_chg"/"quaternary_chg"` | ❌ **完全没有驱动**，主线一行都没有 |
| 充电曲线 | `qcom,battery-data = <&enuma_batterydata>`（fv 4.5 V / fcc 12.4 A / JEITA 阶梯 / thermal-mitigation 表） | ❌ 我们的 `simple-battery` 只有 design 数据（无 fv/fcc/jeita） |
| 电量计 | 小米双 gauge（`dual_fuel_gauge_class.c` + `bq27z561_fg.c`） | ✅ `bq27xxx_battery` 两个 gauge 都能读 |
| 其它 | `cp_qc30.c`(QC3.0)、`qnovo5`、`mi,use-bq-pump`、`mi,support-ffc` | ❌ 无 |

**结论**：PD 协商本身我们有；缺的是把协商结果变成充电电流的那一侧 ——

1. SMB5 节点被关 → 没有充电状态/电流/功率上报（upower 看不到充电功率），也没有 ICL 控制；
2. 67 W 级 PD PPS 快充靠 **2×LN8000 + BQ2597x 充电泵**，主线没有任何驱动 →
   就算把 SMB5 打开，也只能走 SMB5 自身那点功率（十几 W 量级）；
3. 没有充电曲线数据 → 浮充电压/快充电流/温度阶梯不可编程
   （旧版 `qcom_pm8150b_charger` 在这种情况下会跳过编程，保留固件值，这点对安全反而有利）；
4. 没有「充电器 ↔ PD PHY」绑定与 PD 策略层。

### 6.3 3.2 GHz：机制差异（不是硬件问题，也不是初始化失败）

用**小米自己的** `drivers/cpufreq/qcom-cpufreq-hw.c` 验证：

```
$ grep -c 'dev_pm_opp_of_add_table\|operating-points' \
        /home/axis/src/xiaomi-elish-kernel/drivers/cpufreq/qcom-cpufreq-hw.c
0
$ sed -n '524,530p' .../qcom-cpufreq-hw.c
	for_each_cpu(cpu, &c->related_cpus) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev)
			continue;
		dev_pm_opp_add(cpu_dev, c->table[i].frequency * 1000, volt);
	}
```

* **下游**：不用 DT OPP 表，直接读硬件 LUT，逐条 `dev_pm_opp_add()`。
  `LUT_MAX_ENTRIES = 40`，`lut_max_entries` 还能用模块参数覆盖。
  **硬件 LUT 就是权威** → 3187200 自然生效。
* **主线**：`dev_pm_opp_of_add_table()` 成功后会「先把所有 OPP 禁用，再用 LUT 逐条交叉校验」，
  DT 表里没有的频点 `dev_pm_opp_adjust_voltage()` 返回 -ENOENT →
  `CPUFREQ_ENTRY_INVALID` + `dev_warn("failed to update OPP for freq=%d")`。
  **DT OPP 表成了允许列表**。

我们的情况：

```
[    0.256699] cpu cpu7: failed to update OPP for freq=3187200
[    0.256851] cpu cpu7: failed to update OPP for freq=3187200
```

驱动**读到了** 3187200，是后面「DT 里没有这一档」才失败 —— 所以**硬件没问题、
也不是 fuse/初始化失败**。上游 `sm8250.dtsi` 的 `cpu7_opp_table` 止于
`opp-2841600000`（那是 SD865 的档位），上游也没有 `sm8250-ac` 变体表。

两条 warning 正好对应驱动对 `LUT_TURBO_IND`（`core_count == 1`）那一档的特殊处理：
第一次是普通表项失败，第二次是它想把前一档提升为 `CPUFREQ_BOOST_FREQ` 又失败。
补上 OPP 之后，3187200 会作为一个普通频点生效（电压由硬件 LUT 提供）。

修补丁见第 3 节：`sm8250-xiaomi-elish-csot.dtb` + `apply-3g2-opp.sh`。

---

## 7. 传感器 / 相机 / 麦克风 体检

| 项目 | 结论 | 关键证据 |
|---|---|---|
| 加速度计·陀螺·磁力计·光照·接近 | ❌ **完全不可用** | 硬件挂 SLPI，主线没有 SSC 客户端 |
| 前置/后置摄像头 | ❌ **完全不可用** | `camss`/`cci` 全 disabled，且没有任何 sensor 节点 |
| 麦克风 | ❌ **录不到** | `no backend DAIs enabled for MultiMedia1` |
| 扬声器播放 | ✅ 可用 | 走 out-of-tree 的 CS35L41 功放，不依赖 ADSP AFE |

### 7.1 传感器：硬件有 5 类，但全在 SLPI 后面

从 SLPI 固件配置能直接读出硬件清单
（`/lib/firmware/qcom/sm8250/xiaomi/elish/sensors/config/`）：

| 芯片 | 类型 |
|---|---|
| `lsm6dso` | 加速度计 + 陀螺仪（ST 6 轴） |
| `ak991x` | 磁力计 / 霍尔（AKM） |
| `bu27030_back` | **背面**环境光（ROHM BU27030） |
| `tcs3701` | **前面**环境光 + 接近（AMS TCS3701） |
| `sx932x` ×2 | SAR / 握持检测（Semtech） |

`Sensors_list.txt` 里也有 `accel / gyro / ambient_light / ambient_light_back / …`。

**为什么主线拿不到**：这些芯片挂在 **SLPI（Sensor Low Power Island）自己的 I2C**
上，AP 侧能看到的总线只有电池、功放、背光：

```
i2c-0 : 0-0055 bq27z561        i2c-11: 11-0011 ktz8866(背光)
i2c-13: 13-0055 bq27z561       i2c-1 / i2c-3: cs35l41 功放 ×8
```

下游靠 `CONFIG_SENSORS_SSC=y`（Android 的 `/dev/sensors` 通道）+ SLPI 固件把数据
送出来；**主线没有 SSC / SLPI 传感器客户端**，也不存在 `/dev/sensors`。
所以：

* `/sys/bus/iio/devices/` 只有 3 个 PMIC ADC（`pmic@0/@2/@4 adc@3100`），
  没有任何运动/光照/接近传感器；
* `iio-sensor-proxy` 装了但没东西可服务（inactive）；
* 而且 **SLPI 本身在崩**：`sensor_process` PD 初始化超时 → 看门狗每 ~30 s 重启一次
  固件（dmesg 里 `remoteproc0: stopped/is now up` 反复出现）。
  你自己写的 `/usr/local/bin/slpi-off.sh` 就是停掉它来消除这个循环
  （注释里也写了「Armbian 没有传感器栈」）。

### 7.2 摄像头：一条都不通

* 我们的 DT：`camss@ac6a000`、`cci@ac4f000`、`cci@ac50000` **全部 `disabled`**，
  而且**没有任何 camera sensor 节点**；
* 没有 `/dev/media*`、没有 `/dev/v4l-subdev*`；
  `/dev/video0` `/dev/video1` 是 **Venus 视频编解码器**，不是摄像头：
  ```
  $ v4l2-ctl --list-devices
  Qualcomm Venus video encoder (platform:qcom-venus):  /dev/video0  /dev/video1
  ```
* 硬件（下游 DTS）：4 路 —— wide（注释写着 `K81 Wide sensor ov48 & ov13`，OV48B 级
  48MP）+ ultra-wide + macro + depth，另有 `qcom,actuator`、`qcom,eeprom`、
  PM8150L 闪光灯，全部走 Qualcomm 私有 `qcom,cam-sensor` / CCI 框架；
* 有意思的是**主线 6.12 的 CAMSS 已经有 `qcom,sm8250-camss`**
  （`drivers/media/platform/qcom/camss/camss.c:2451`），SoC 侧驱动是在的 ——
  缺的是 DTS 启用 + CSIPHY/CSID 描述 + 每颗 sensor 的 V4L2 驱动（OV48B 等主线没有）。

### 7.3 麦克风：声卡枚举通了，但采集链路没建立

```
$ cat /proc/asound/cards
 0 [Pro]: sm8250 - Xiaomi Mi Pad 5 Pro
$ arecord -D hw:0,0 --dump-hw-params /dev/null
FORMAT: S16_LE S24_LE   CHANNELS: [1 4]   RATE: [8000 48000]     # PCM 是通的
```

但任何参数都装不上（`arecord: set_params: 无法安装hw参数`），dmesg 里的根因：

```
MultiMedia1: ASoC: no backend DAIs enabled for MultiMedia1,
             possibly missing ALSA mixer-based routing or UCM profile
qcom-q6afe aprsvc:service:4:4: elish: afe topologies disabled
```

即 **FE（MultiMedia1）找不到可用的 backend DAI**，ADSP 的 AFE topology 也是关的，
所以采集路径根本没建立；PipeWire 里也**没有任何 source**（只有扬声器 sink 的 monitor）。

播放之所以好使，是因为它走的是 **CS35L41 功放**（你装的 out-of-tree
`snd_soc_cs35l41`，dmesg 有 `loading out-of-tree module taints kernel`）
+ AP 侧 LPASS，不依赖 ADSP AFE —— 所以“有声音”并不代表麦克风通。
