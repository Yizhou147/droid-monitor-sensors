#!/bin/bash
# install.sh — 构建并安装 kgsl GPU 传感器插件到 ksystemstats 插件目录。
#
# 为什么要把上游 gpu 插件改名停用：本插件的 providerName() 也返回 "gpu"
# （页面绑定的 id 是 gpu/all/usage，provider 必须是 gpu 才对得上）。
# 上游插件在本机注册不出任何容器（找不到 AMD/Intel），但它仍然占着同名 provider，
# 两个同名 provider 同时注册会撞 DBus 路径 ⇒ 停用它，且**保留 .disabled 以便随时回退**。
#
# 卸载：sudo ./install.sh uninstall
set -e
DIR=/usr/lib/aarch64-linux-gnu/qt6/plugins/ksystemstats
HERE=$(cd "$(dirname "$0")" && pwd)
SUDO=${SUDO:-sudo}

if [ "$1" = "uninstall" ]; then
    $SUDO rm -f "$DIR/ksystemstats_plugin_kgsl.so"
    [ -f "$DIR/ksystemstats_plugin_gpu.so.disabled" ] && $SUDO mv "$DIR/ksystemstats_plugin_gpu.so.disabled" "$DIR/ksystemstats_plugin_gpu.so"
    pkill -x ksystemstats 2>/dev/null || true
    echo "已卸载并恢复上游 gpu 插件；ksystemstats 会在下次被调用时重启"
    exit 0
fi

cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$HERE/build" -- -j2

$SUDO install -m 0644 "$HERE/build/libksystemstats_plugin_kgsl.so" "$DIR/ksystemstats_plugin_kgsl.so"
# 只有本机确实是 kgsl（本插件会真正注册传感器）时才停用上游 gpu 插件；
# 否则上游插件要留着，不然 AMD/Intel 机器上 GPU 反而整个消失。
if [ -e /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage ] && [ -f "$DIR/ksystemstats_plugin_gpu.so" ]; then
    $SUDO mv "$DIR/ksystemstats_plugin_gpu.so" "$DIR/ksystemstats_plugin_gpu.so.disabled"
    echo "已停用上游 gpu 插件（回退：把 .disabled 改回原名）"
elif [ ! -e /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage ]; then
    echo "注意：本机没有 /sys/class/kgsl ⇒ 本插件不会注册任何传感器，上游 gpu 插件保持启用。"
fi

# ksystemstats 是 DBus 可激活服务，杀掉即可由下一次请求拉起并加载新插件
pkill -x ksystemstats 2>/dev/null || true
echo "安装完成。若系统监视器已开着，重开一次（或等它自己重连）即可看到 GPU 传感器。"
