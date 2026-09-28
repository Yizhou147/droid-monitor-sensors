cmake_minimum_required(VERSION 3.16)
project(ksystemstats_kgsl LANGUAGES CXX)

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

# 装到 ksystemstats 的插件目录（与上游 gpu 插件同目录，它按目录扫描加载）。
# 默认取本机实测路径；换机器时用 -DPLUGIN_INSTALL_DIR=... 覆盖（不要写死架构）。
set(PLUGIN_INSTALL_DIR "/usr/lib/aarch64-linux-gnu/qt6/plugins/ksystemstats"
    CACHE STRING "ksystemstats 插件目录")

install(TARGETS ksystemstats_plugin_kgsl DESTINATION ${PLUGIN_INSTALL_DIR})
