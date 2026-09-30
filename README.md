中文 | [English](README_english.md)

# droid-monitor-sensors — 让 KDE 系统监视器显示 Adreno GPU / 磁盘 / 温度

小米 Pad 8 Pro（SM8750 / kgsl）上，KDE **系统监视器**的 GPU 圈一直是空的，页面顶部还提示
"此页面缺少了部分传感器"。**anland 与 DRM 接管两种模式下现象相同**（同一个 rootfs、同一个 ksystemstats）。

> 除 GPU 外，本仓库第二个插件还补齐了**磁盘**与 **CPU/电池温度**（网络实测上游已可用，故不动）——
> 见下文「扩展：磁盘 + 温度」。

## 根因

传感器链路是：`plasma-systemmonitor → libksysguardsystemstats2 → D-Bus org.kde.ksystemstats1 → ksystemstats 插件`。

上游 `ksystemstats_plugin_gpu.so` 只有两个后端（符号里可见 `LinuxAmdGpu` / `LinuxIntelGpu`），
它们用 libdrm 枚举 `drm_minor`，再去读 **amdgpu 风格**的 `gpu_busy_percent` 与 **hwmon** 温度。本机：

- GPU 走 **kgsl**（不是 upstream msm DRM 路径），`/sys/class/drm/card0/device/` 下没有 `gpu_busy_percent`；
- 这颗 vendor 内核**一个 hwmon 都不注册**（`/sys/class/hwmon` 为空，安卓侧同样为空）。

⇒ 插件找不到任何 GPU，`gpu/all/usage` 这个 id 不存在，face 绑不到东西。

而数据其实全在，只是换了接口：

| kgsl 节点 | 含义 |
| --- | --- |
| `/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage` | 利用率（文本形如 `21 %`） |
| `/sys/class/kgsl/kgsl-3d0/temp` | 毫摄氏度（`71800` = 71.8°C） |
| `/sys/class/kgsl/kgsl-3d0/clock_mhz` | GPU 频率档 |
| `/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq` | 当前频率（Hz） |

## 做法

新增一个 ksystemstats 插件 `ksystemstats_plugin_kgsl.so`，用 `KSysGuard::SysFsSensor`
把这些节点注册成 `gpu/all/usage`、`gpu/all/temperature`、`gpu/all/frequency`、`gpu/all/speed`
（另给一份单卡视图 `gpu/Adreno/*`）。id 是**层级拼接**的：`<container>/<object>/<property>`，
所以拼出来正好是页面里绑定的 `gpu/all/usage` —— 用户不需要改页面。

## 两个必须踩对的坑（都实测过）

1. **插件元数据里必须有 `providerName` 键**。ksystemstats 用 `KPluginMetaData::findPlugins` 带过滤器扫描，
   只写 `Id`/`Name` 会被**静默过滤掉**（现象：装好了、`allSensors` 里啥也没有、也没有报错）。
   对比上游 `.so` 的 `.note.qt.metadata` 段才看出来它带的是 `{"providerName":"gpu"}`。
2. **必须实现 `SensorPlugin::update()` 去逐个刷新 `SysFsSensor`**。否则传感器**有元数据、没值**：
   `allSensors` 里能看到 `gpu/all/usage`，但界面还是空圈。
   另外验证时注意**订阅是按连接生效的** —— 用 `busctl` 分两次调用（一次 subscribe 一次 sensorData）验不出来，
   必须同一条连接，故有 `verify_gpu_sensor.py`。

## 扩展：磁盘 + 温度（containerio）

除了 GPU，本仓库还带第二个插件 `ksystemstats_plugin_containerio.so`，把**磁盘**和 **CPU/电池温度**
也补出来（网络不在其中，见下）。

**磁盘**：上游 `ksystemstats_plugin_disk.so` 依赖 Solid（fstab/devices backend），本容器里没有 Solid
⇒ 它只注册出**空的** `disk/all/*` 壳子（`allSensors` 里能看到，但 subscribe 读回是 MISSING，横条图空白）。
数据其实都在，只是来源不同：容量走 `statvfs()`，IO 走 `/proc/diskstats`（字段 `[5]`/`[9]` 扇区 ×512）。

**温度**：本机 `/sys/class/hwmon` 为空 ⇒ 上游 `lmsensors`（靠 libsensors 枚举 hwmon）一个 chip 都注册不出，
`thermal` 整个不存在。但温度在 `/sys/class/thermal/thermal_zone*`（实测有值）：

| 热区 type | 含义 |
| --- | --- |
| `cpu_therm` | 封装级 CPU 温度（首选；缺失时回退取所有 `cpu-*`/`cpuss-*` 的最大值） |
| `battery` | 电池温度 |

单位毫摄氏度（`42100` = 42.1°C）。注册成 `thermal/cpu/temperature`、`thermal/battery/temperature`。

**网络：有意不做**。09-30 实测上游 `ksystemstats_plugin_network.so` 在本机是**好的**（`network/wlan0/download`、
`ipv4address` 都有真值，还带 dns/gateway/signal），移植自己的反而会**停用能用的、换成更弱的**。故 io 插件
**不产 network 容器、install 脚本也不停用上游 network**。若哪天上游 network 也失效，再按 disk 的路子补。

> 磁盘 id（`disk/all/used`）与现有 overview.page 的磁盘面绑定天然对得上，**不需要改页面**；温度面页面本就没有，
> 新传感器出现在「所有传感器 / 历史」里，要上屏就在 UI 手动加一个温度 face。

## 构建 / 安装 / 卸载

```
./install.sh                  # 构建 + 安装两个插件 + 停用相应上游 + 重启 ksystemstats
./install.sh uninstall        # 卸载自研插件并还原被停用的上游插件
python3 verify_gpu_sensor.py  # 验证 GPU 出值（可并发 vkmark 压 GPU 看 usage 上跳）
python3 verify_io_sensor.py   # 验证 磁盘 + 温度 出值
```

依赖：`libksysguard-dev`、`qt6-base-dev`、`kf6-coreaddons-dev`、`kf6-i18n-dev`、`cmake`、
以及 **`libsensors-dev`**（KSysGuard 的传递依赖需要 `libsensors.so` 符号链接，缺了链接期才报错）。

安装脚本的停用策略（都是**改名成 `.disabled`、可逆**，`uninstall` 原样还原）：

- `gpu.so`：仅当本机确实有 kgsl 节点时才停（本插件 provider 也叫 `gpu`，会同名撞车）。
- `disk.so`：本容器总会由 io 插件出真值，停用上游同名 `disk` 容器。
- `network.so`、`lmsensors.so`：**都不动**——上游 network 本机可用；lmsensors 因 hwmon 空注册不出东西、
  也不占 `thermal` 容器。

## 生效范围

改的是容器内的 rootfs，**anland 与 DRM 接管两种模式共用**，无需分别处理。装完若系统监视器已开着，
重开一次即可（ksystemstats 被重启过，旧窗口持有的是失效的传感器列表）。

## 许可与出处

- 本仓库代码以 **GPL-3.0-only** 发布，全文见 [`LICENSE`](LICENSE)。各源文件头部带对应 SPDX 标识；
  两个插件的 `*.json` 除外——JSON 里写注释会让元数据解析失败，插件直接被 `findPlugins` 静默过滤（即上面第一个坑）。
- 磁盘/温度插件参考了 **gpu-monitor-fix** 项目的实现思路（本仓库那份是重写）。该项目未能找到公开地址。
