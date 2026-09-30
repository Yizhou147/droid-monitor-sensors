#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 XiaomiPad8Pro-drm-display 项目
# SPDX-License-Identifier: GPL-3.0-only
"""verify_io_sensor.py — 端到端验证 containerio 的 磁盘 / 温度 传感器是否在出值。

和 verify_gpu_sensor.py 同理：传感器"注册出来了"不等于"有值"（上游 disk 就是只注册出
disk/all/* 的空壳，subscribe 读回 MISSING）。所以这里做"订阅 + 读值 + 对照内核来源"。

注意：订阅是**按连接**生效的 ⇒ subscribe 与 sensorData 必须在同一条连接上做。

网络不在这里验证：实测上游 ksystemstats_plugin_network 在本机已正常出值，未做移植。

用法：
    python3 verify_io_sensor.py
"""
import os
import re
import sys
import time

import dbus


def discover(all_ids):
    disk = [i for i in all_ids if re.fullmatch(r"disk/all/used", i)]
    thermal = [i for i in all_ids if re.fullmatch(r"thermal/(cpu|battery)/temperature", i)]
    return disk, thermal


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    bus = dbus.SessionBus()
    obj = bus.get_object("org.kde.ksystemstats1", "/org/kde/ksystemstats1")
    proxy = dbus.Interface(obj, "org.kde.ksystemstats1")

    all_ids = [str(s) for s in proxy.allSensors()]
    disk, thermal = discover(all_ids)
    ids = disk + thermal
    if not ids:
        print("allSensors 里找不到 disk/all/used 或 thermal/*/temperature ⇒ 插件没加载或 providerName 键缺失")
        sys.exit(2)
    print(f"发现 {len(disk)} disk / {len(thermal)} thermal 传感器: {ids}")

    proxy.subscribe(ids)
    bad = 0
    try:
        for i in range(rounds):
            time.sleep(1.2)
            data = {str(k): v for k, v in proxy.sensorData(ids)}
            if not data:
                print(f"{i}: 空 —— 插件没出值（检查 SensorPlugin::update() 是否实现）")
                bad += 1
                continue
            ok = True
            line = []
            for sid in disk:
                v = float(data.get(sid, 0))
                mark = "OK" if v > 0 else "无值!"
                ok = ok and v > 0
                line.append(f"{sid}={v/1e9:.1f}GB {mark}")
            for sid in thermal:
                v = float(data.get(sid, 0))
                mark = "OK" if 0 < v < 120 else "越界!"
                ok = ok and 0 < v < 120
                line.append(f"{sid}={v:.1f}C {mark}")
            print(f"{i}: " + "  ".join(line))
            if not ok:
                bad += 1
    finally:
        proxy.unsubscribe(ids)

    if bad:
        print(f"有 {bad} 轮异常")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    if not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        os.environ["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=/run/user/1000/bus"
    main()
