#!/bin/bash
# SPDX-FileCopyrightText: 2026 XiaomiPad8Pro-drm-display 项目
# SPDX-License-Identifier: GPL-3.0-only
#
# install.sh — 构建并安装两个 ksystemstats 插件到插件目录：
#   1) ksystemstats_plugin_kgsl.so         — kgsl(Adreno) GPU
#   2) ksystemstats_plugin_containerio.so  — container-safe 磁盘 / 温度
#
# 为什么要停用上游插件：本自研插件产出的**容器 id** 会和上游同名，两个同名容器同时注册会撞
# DBus 对象路径 ⇒ 把上游 .so 改名成 .disabled 停用，**保留 .disabled 以便 uninstall 时原样还原**。
#   - gpu   : 仅当本机确实是 kgsl（本插件会真正注册）才停用，避免在非 kgsl 机器上把 GPU 整个弄没。
#   - disk  : 上游 disk 依赖 Solid，本容器里没有 ⇒ 只注册出空的 disk/all/* 壳，由 io 插件出真值，停用之。
#   - network : **不停用**。实测上游 network 在本机正常出值（wlan0 的 download/upload/ipv4 都有值，
#               还带 dns/gateway/signal），保留上游、io 插件也不产 network 容器，避免倒退。
#   - lmsensors : 不动（本机 /sys/class/hwmon 为空，它注册不出 chip，也不占 thermal 容器）。
#
# 卸载：sudo ./install.sh uninstall
set -e
DIR=/usr/lib/aarch64-linux-gnu/qt6/plugins/ksystemstats
HERE=$(cd "$(dirname "$0")" && pwd)
SUDO=${SUDO:-sudo}

# 把某上游插件停用（存在且未停用才动），可逆
disable() {
    local p="$1"
    if [ -f "$DIR/$p.so" ] && [ ! -f "$DIR/$p.so.disabled" ]; then
        $SUDO mv "$DIR/$p.so" "$DIR/$p.so.disabled"
        echo "已停用上游 $p.so（回退：把 .disabled 改回原名，或 ./install.sh uninstall）"
    fi
}

# 把某上游插件从 .disabled 还原（只还原我们被动过的）
restore() {
    local p="$1"
    [ -f "$DIR/$p.so.disabled" ] && $SUDO mv "$DIR/$p.so.disabled" "$DIR/$p.so" && echo "已还原上游 $p.so"
}

if [ "$1" = "uninstall" ]; then
    $SUDO rm -f "$DIR/ksystemstats_plugin_kgsl.so" "$DIR/ksystemstats_plugin_containerio.so"
    restore ksystemstats_plugin_gpu
    restore ksystemstats_plugin_disk
    pkill -x ksystemstats 2>/dev/null || true
    echo "已卸载自研插件并恢复上游；ksystemstats 会在下次被调用时重启"
    exit 0
fi

cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$HERE/build" -- -j2

$SUDO install -m 0644 "$HERE/build/libksystemstats_plugin_kgsl.so" "$DIR/ksystemstats_plugin_kgsl.so"
$SUDO install -m 0644 "$HERE/build/libksystemstats_plugin_containerio.so" "$DIR/ksystemstats_plugin_containerio.so"

# 只有本机确实是 kgsl（GPU 插件会真正注册传感器）时才停用上游 gpu 插件；
# 否则上游插件要留着，不然 AMD/Intel 机器上 GPU 反而整个消失。
if [ -e /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage ]; then
    disable ksystemstats_plugin_gpu
else
    echo "注意：本机没有 /sys/class/kgsl ⇒ GPU 插件不注册传感器，上游 gpu 插件保持启用。"
fi

# 磁盘：本容器总会由 io 插件出真值，停用上游同名 disk 插件
disable ksystemstats_plugin_disk

# ksystemstats 是 DBus 可激活服务，杀掉即可由下一次请求拉起并加载新插件
pkill -x ksystemstats 2>/dev/null || true
echo "安装完成。若系统监视器已开着，重开一次（或等它自己重连）即可看到 GPU/磁盘传感器；"
echo "温度传感器出现在“所有传感器”里，需要的话在概览页手动加一个温度 face。网络由上游插件负责（已实测可用）。"
