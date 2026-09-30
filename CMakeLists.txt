# SPDX-FileCopyrightText: 2026 XiaomiPad8Pro-drm-display 项目
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.16)
project(droid_monitor_sensors LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6I18n REQUIRED)
find_package(KSysGuard REQUIRED)

add_library(ksystemstats_plugin_kgsl MODULE)

target_sources(ksystemstats_plugin_kgsl PRIVATE
    src/kgsl_gpu_plugin.cpp
)

target_link_libraries(ksystemstats_plugin_kgsl PRIVATE
    Qt6::Core
    KF6::CoreAddons
    KF6::I18n
    KSysGuard::SystemStats
    KSysGuard::Formatter
)

# container-safe 磁盘 / 温度后端（用 statvfs + /proc/diskstats + /sys/class/thermal，
# 不依赖 Solid/hwmon）。网络不在范围内：实测上游 network 在本机已正常出值，保留它。
add_library(ksystemstats_plugin_containerio MODULE)

target_sources(ksystemstats_plugin_containerio PRIVATE
    src/container_io_plugin.cpp
)

target_link_libraries(ksystemstats_plugin_containerio PRIVATE
    Qt6::Core
    KF6::CoreAddons
    KF6::I18n
    KSysGuard::SystemStats
    KSysGuard::Formatter
)

# 装到 ksystemstats 的插件目录（与上游 gpu 插件同目录，它按目录扫描加载）。
# 默认取本机实测路径；换机器时用 -DPLUGIN_INSTALL_DIR=... 覆盖（不要写死架构）。
set(PLUGIN_INSTALL_DIR "/usr/lib/aarch64-linux-gnu/qt6/plugins/ksystemstats"
    CACHE STRING "ksystemstats 插件目录")

install(TARGETS ksystemstats_plugin_kgsl ksystemstats_plugin_containerio DESTINATION ${PLUGIN_INSTALL_DIR})
