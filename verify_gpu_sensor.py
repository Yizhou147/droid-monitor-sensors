#!/usr/bin/env python3
"""verify_gpu_sensor.py — 端到端验证 kgsl GPU 传感器是否在出值。

为什么需要它：传感器"注册出来了"不等于"有值"。09-28 实测：插件不实现
SensorPlugin::update() 时，allSensors 里能看到 gpu/all/usage，但 subscribe 之后
sensorData 返回空 ⇒ 界面上依旧是一个空圈。所以必须"订阅 + 读值 + 对照 sysfs"。

注意：订阅是**按连接**生效的，所以 subscribe 与 sensorData 必须在同一条连接上做
（用 busctl 分两次调用验不出来，这是踩过的坑）。

用法：
    python3 verify_gpu_sensor.py            # 空闲态
    vkmark -b clear &  python3 verify_gpu_sensor.py   # 负载态，看 usage 是否上跳
"""
import os
import sys
import time

import dbus

IDS = ["gpu/all/usage", "gpu/all/temperature", "gpu/all/frequency", "gpu/all/speed"]
SYSFS = {
    "usage": "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
    "temperature": "/sys/class/kgsl/kgsl-3d0/temp",
    "frequency": "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
    "speed": "/sys/class/kgsl/kgsl-3d0/clock_mhz",
}


def read_sysfs(key):
    try:
        with open(SYSFS[key]) as f:
            raw = f.read().strip()
    except OSError:
        return None
    n = int("".join(c for c in raw if c.isdigit()) or 0)
    if key == "temperature":
        return n / 1000.0
    if key == "frequency":
        return n / 1000000.0
    return float(n)


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    bus = dbus.SessionBus()
    obj = bus.get_object("org.kde.ksystemstats1", "/org/kde/ksystemstats1")
    iface = dbus.Interface(obj, "org.kde.ksystemstats1")
    iface.subscribe(IDS)
    bad = 0
    try:
        for i in range(rounds):
            time.sleep(1.2)
            got = {str(k).split("/")[-1]: float(v) for k, v in iface.sensorData(IDS)}
            if not got:
                print(f"{i}: 空 —— 插件没有出值（检查 SensorPlugin::update() 是否实现）")
                bad += 1
                continue
            line = []
            for key, val in sorted(got.items()):
                ref = read_sysfs(key)
                delta = "" if ref is None else ("OK" if abs(ref - val) <= max(2.0, ref * 0.15) else f"偏差 sysfs={ref}")
                line.append(f"{key}={val:.1f} {delta}")
            print(f"{i}: " + "  ".join(line))
    finally:
        iface.unsubscribe(IDS)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    if not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        os.environ["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=/run/user/1000/bus"
    main()
