[中文](README.md) | English

# droid-gpu-sensor — Show the Adreno GPU in KDE System Monitor

On the Xiaomi Pad 8 Pro (SM8750 / kgsl), the GPU circle in **KDE System Monitor** is always
empty, and the page header warns "Some sensors are missing from this page". **The symptom is
identical in both anland and DRM takeover modes** (same rootfs, same ksystemstats).

## Root cause

The sensor chain is:
`plasma-systemmonitor → libksysguardsystemstats2 → D-Bus org.kde.ksystemstats1 → ksystemstats plugins`.

The upstream `ksystemstats_plugin_gpu.so` only has two backends (symbols `LinuxAmdGpu` /
`LinuxIntelGpu` are visible). They enumerate `drm_minor` via libdrm, then read **amdgpu-style**
`gpu_busy_percent` and **hwmon** temperatures. On this device:

- the GPU goes through **kgsl** (not the upstream MSM DRM path); there is no `gpu_busy_percent`
  under `/sys/class/drm/card0/device/`;
- this vendor kernel **registers no hwmon at all** (`/sys/class/hwmon` is empty — same on the
  Android side).

⇒ The plugin finds no GPU, the id `gpu/all/usage` never exists, and the face binds to nothing.

The data is all there — just behind a different interface:

| kgsl node | Meaning |
| --- | --- |
| `/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage` | Utilization (text like `21 %`) |
| `/sys/class/kgsl/kgsl-3d0/temp` | Millidegrees Celsius (`71800` = 71.8°C) |
| `/sys/class/kgsl/kgsl-3d0/clock_mhz` | GPU frequency level |
| `/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq` | Current frequency (Hz) |

## Approach

Add a new ksystemstats plugin, `ksystemstats_plugin_kgsl.so`, that registers these nodes via
`KSysGuard::SysFsSensor` as `gpu/all/usage`, `gpu/all/temperature`, `gpu/all/frequency`,
`gpu/all/speed` (plus a single-GPU view `gpu/Adreno/*`). Ids are **hierarchical
concatenations**: `<container>/<object>/<property>`, so the result is exactly the `gpu/all/usage`
the page already binds to — the user does not have to edit the page.

## Two pits you must get right (both measured)

1. **The plugin metadata must contain a `providerName` key.** ksystemstats scans plugins with
   `KPluginMetaData::findPlugins` plus a filter; a metadata file with only `Id`/`Name` is
   **silently filtered out** (symptom: installed fine, `allSensors` shows nothing, no error
   anywhere). Only comparing against the upstream `.so`'s `.note.qt.metadata` section revealed it
   carries `{"providerName":"gpu"}`.
2. **You must implement `SensorPlugin::update()` to refresh each `SysFsSensor`.** Otherwise
   sensors have metadata but **no values**: `gpu/all/usage` appears in `allSensors` while the UI
   circle stays empty. Also note when verifying that **subscriptions are per-connection** — two
   separate `busctl` calls (one subscribe, one sensorData) cannot prove anything; it must be one
   connection, hence `verify_gpu_sensor.py`.

## Build / install / uninstall

```
./install.sh                 # build + install + restart ksystemstats
./install.sh uninstall       # uninstall and restore the upstream gpu plugin
python3 verify_gpu_sensor.py # verify values appear (optionally run vkmark concurrently to watch usage climb)
```

Dependencies: `libksysguard-dev`, `qt6-base-dev`, `kf6-coreaddons-dev`, `kf6-i18n-dev`, `cmake`,
and **`libsensors-dev`** (KSysGuard's transitive dependency needs the `libsensors.so` symlink;
the error only surfaces at link time).

The install script renames the upstream `ksystemstats_plugin_gpu.so` to `.disabled` **only when
the kgsl nodes actually exist on this machine** (both plugins use `providerName` = `gpu` and
would collide if registered together). On non-kgsl machines this plugin simply `return`s without
registering anything, and the upstream plugin stays enabled.

## Scope

This changes the container's rootfs, which is **shared by both anland and DRM takeover modes** —
no per-mode handling needed. If System Monitor is already open after installing, reopen it once
(ksystemstats was restarted; old windows hold a stale sensor list).
