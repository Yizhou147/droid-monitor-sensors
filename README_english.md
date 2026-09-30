[中文](README.md) | English

# droid-monitor-sensors — Show the Adreno GPU / Disk / Temperature in KDE System Monitor

On the Xiaomi Pad 8 Pro (SM8750 / kgsl), the GPU circle in **KDE System Monitor** is always
empty, and the page header warns "Some sensors are missing from this page". **The symptom is
identical in both anland and DRM takeover modes** (same rootfs, same ksystemstats).

> Besides the GPU, a second plugin in this repo also fills in **disk** and **CPU/battery
> temperature** (network is left alone — the upstream one already works here). See
> "Extension: disk + thermal" below.

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

## Extension: disk + thermal (containerio)

Beyond the GPU, this repo ships a second plugin, `ksystemstats_plugin_containerio.so`, that also
fills in **disk** and **CPU/battery temperature** (network is deliberately excluded — see below).

**Disk**: the upstream `ksystemstats_plugin_disk.so` depends on Solid (fstab/devices backend),
which is absent in this container ⇒ it only registers **empty** `disk/all/*` shells (visible in
`allSensors`, but a subscribe returns MISSING and the bars stay blank). The data is all there, just
from a different source: capacity via `statvfs()`, I/O via `/proc/diskstats` (fields `[5]`/`[9]`
sectors × 512).

**Temperature**: `/sys/class/hwmon` is empty here ⇒ the upstream `lmsensors` plugin (which enumerates
hwmon via libsensors) registers no chip at all, so `thermal` simply does not exist. But the data is
in `/sys/class/thermal/thermal_zone*` (values confirmed on-device):

| thermal zone type | Meaning |
| --- | --- |
| `cpu_therm` | Package CPU temperature (preferred; falls back to the max of all `cpu-*`/`cpuss-*` zones) |
| `battery` | Battery temperature |

Millidegrees Celsius (`42100` = 42.1°C), exposed as `thermal/cpu/temperature` and
`thermal/battery/temperature`.

**Network: intentionally not ported.** Measured on 09-30, the upstream `ksystemstats_plugin_network.so`
**works** on this device (`network/wlan0/download`, `ipv4address` all have real values, plus
dns/gateway/signal). Porting our own would only **replace something that works with something weaker**,
so the io plugin **creates no network container and the install script does not disable upstream
network**. If upstream network ever breaks, add it following the disk path.

> The disk id (`disk/all/used`) already matches the disk face binding in the existing overview.page,
> so **no page edit is needed**; there is no temperature face on the page, so the new temperature
> sensors show up under "All sensors / History" — add a temperature face in the UI to surface them.

## Build / install / uninstall

```
./install.sh                  # build + install both plugins + disable the matching upstream + restart ksystemstats
./install.sh uninstall        # remove our plugins and restore whatever upstream plugins were disabled
python3 verify_gpu_sensor.py  # verify GPU values (optionally run vkmark concurrently to watch usage climb)
python3 verify_io_sensor.py   # verify disk + temperature values
```

Dependencies: `libksysguard-dev`, `qt6-base-dev`, `kf6-coreaddons-dev`, `kf6-i18n-dev`, `cmake`,
and **`libsensors-dev`** (KSysGuard's transitive dependency needs the `libsensors.so` symlink;
the error only surfaces at link time).

Disable policy in the install script (all are reversible **renames to `.disabled`**, restored verbatim
by `uninstall`):

- `gpu.so`: disabled only when the kgsl nodes actually exist here (our plugin's provider is also
  `gpu`, so the two would collide).
- `disk.so`: the io plugin always produces the real values in this container, so the same-named
  upstream `disk` container is disabled.
- `network.so` and `lmsensors.so`: **both left untouched** — upstream network works here, and
  lmsensors registers nothing (empty hwmon) and does not occupy the `thermal` container.

## Scope

This changes the container's rootfs, which is **shared by both anland and DRM takeover modes** —
no per-mode handling needed. If System Monitor is already open after installing, reopen it once
(ksystemstats was restarted; old windows hold a stale sensor list).

## License and provenance

- All code in this repo is released under **GPL-3.0-only**; full text in [`LICENSE`](LICENSE).
  Each source file carries the matching SPDX identifier; the two plugin `*.json` files are the
  exception — a comment there makes the metadata fail to parse, so the plugin is silently filtered
  out by `findPlugins` (pitfall #1 above).
- The disk/thermal plugin was informed by the **gpu-monitor-fix** project (the version here is a
  rewrite). No public address for that project could be found.
