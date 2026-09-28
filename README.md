# droid-gpu-sensor — 让 KDE 系统监视器显示 Adreno GPU

小米 Pad 8 Pro（SM8750 / kgsl）上，KDE **系统监视器**的 GPU 圈一直是空的，页面顶部还提示
"此页面缺少了部分传感器"。**anland 与 DRM 接管两种模式下现象相同**（同一个 rootfs、同一个 ksystemstats）。

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

## 构建 / 安装 / 卸载

```
./install.sh                 # 构建 + 安装 + 重启 ksystemstats
./install.sh uninstall       # 卸载并恢复上游 gpu 插件
python3 verify_gpu_sensor.py # 验证出值（可选：同时用 vkmark 压 GPU 看 usage 上跳）
```

依赖：`libksysguard-dev`、`qt6-base-dev`、`kf6-coreaddons-dev`、`kf6-i18n-dev`、`cmake`、
以及 **`libsensors-dev`**（KSysGuard 的传递依赖需要 `libsensors.so` 符号链接，缺了链接期才报错）。

安装脚本只在**本机确实存在 kgsl 节点**时才把上游 `ksystemstats_plugin_gpu.so` 改名为 `.disabled`
（两者 `providerName` 都是 `gpu`，同时注册会撞）；非 kgsl 机器上本插件自己就 `return` 不注册，上游保持启用。

## 生效范围

改的是容器内的 rootfs，**anland 与 DRM 接管两种模式共用**，无需分别处理。装完若系统监视器已开着，
重开一次即可（ksystemstats 被重启过，旧窗口持有的是失效的传感器列表）。
