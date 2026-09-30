/*
    SPDX-FileCopyrightText: 2026 XiaomiPad8Pro-drm-display 项目
    SPDX-License-Identifier: GPL-3.0-only

    ksystemstats 的 kgsl(Adreno) GPU 传感器后端。

    为什么需要它：上游 ksystemstats_plugin_gpu 只实现了两个后端
    （LinuxAmdGpu / LinuxIntelGpu，见该 .so 里的符号），它们靠 libdrm 枚举
    `drm_minor` 再去读 amdgpu 风格的 `gpu_busy_percent` 与 hwmon 温度。
    本机是 SM8750 + **kgsl**（不是 msm/upstream DRM GPU 路径），而且这颗 vendor
    内核**一个 hwmon 设备都不注册**（/sys/class/hwmon 为空，安卓侧同样为空）
    ⇒ 上游插件找不到任何 GPU ⇒ plasma-systemmonitor 的 GPU 面空白，并提示
    "此页面缺少了部分传感器"。

    数据其实都在，只是接口不同名：
      /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage  -> "21 %"
      /sys/class/kgsl/kgsl-3d0/temp                 -> 71800 (毫摄氏度)
      /sys/class/kgsl/kgsl-3d0/clock_mhz            -> 222
      /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq     -> 222000000

    传感器 id 的拼法是 <container>/<object>/<property>，页面里绑定的
    `gpu/all/usage` 就是这么来的（overview.page 的 highPrioritySensorIds）。
*/

#include <QFileInfo>
#include <QList>
#include <QString>

#include <KPluginFactory>

#include <systemstats/SensorContainer.h>
#include <systemstats/SensorObject.h>
#include <systemstats/SensorPlugin.h>
#include <systemstats/SensorProperty.h>
#include <systemstats/SysFsSensor.h>

#include <formatter/Unit.h>

using namespace KSysGuard;

class KgslGpuPlugin : public SensorPlugin
{
    Q_OBJECT
public:
    explicit KgslGpuPlugin(QObject *parent, const QVariantList &args)
        : SensorPlugin(parent, args)
    {
        const QString base = QStringLiteral("/sys/class/kgsl/kgsl-3d0");
        // 不是 kgsl 机器（或节点没透传进容器）⇒ 一个传感器都不注册，
        // 让上游 gpu 插件继续处理 AMD/Intel 的情况。
        if (!QFileInfo::exists(base + QLatin1String("/gpu_busy_percentage"))) {
            return;
        }

        auto *container = new SensorContainer(QStringLiteral("gpu"), QStringLiteral("GPU"), this);

        // "all" 这一组就是概览页那个圈要的东西（gpu/all/usage）
        auto *all = new SensorObject(QStringLiteral("all"), tr("All GPUs"), container);
        addBusy(all, base, /*aggregate=*/true);

        // 再给一份单卡视图（gpu/Adreno/...），供"应用程序/历史记录"等页面按设备绑定
        auto *dev = new SensorObject(QStringLiteral("Adreno"), tr("Adreno GPU"), container);
        addBusy(dev, base, /*aggregate=*/false);

        addContainer(container);
    }

    QString providerName() const override
    {
        // 上游 gpu 插件也叫 gpu；本插件在它之前注册（它在本机注册不出容器），
        // 若与上游同时加载会撞 DBus 路径 ⇒ 安装脚本会把上游的 .so 改名停用。
        return QStringLiteral("gpu");
    }

    /**
     * ksystemstats 的刷新定时器会调这里（"before an update will be sent to the user"）。
     * **SysFsSensor 不会自己读 sysfs** —— 必须在这里逐个刷新，否则传感器有元数据但没有值
     * （09-28 实测：不实现 update() 时 subscribe + sensorData 返回空）。
     */
    void update() override
    {
        for (auto *sensor : m_sensors) {
            sensor->update();
        }
    }

private:
    QList<SensorProperty *> m_sensors;

    void addBusy(SensorObject *parent, const QString &base, bool /*aggregate*/)
    {
        auto *usage = new SysFsSensor(QStringLiteral("usage"), base + QLatin1String("/gpu_busy_percentage"), parent);
        usage->setName(tr("GPU Busy"));
        usage->setShortName(tr("Busy"));
        usage->setUnit(KSysGuard::UnitPercent);

        auto *temp = new SysFsSensor(QStringLiteral("temperature"), base + QLatin1String("/temp"), parent);
        temp->setName(tr("Temperature"));
        temp->setShortName(tr("Temp"));
        // kgsl 的 temp 是毫摄氏度（71800 = 71.8°C）
        temp->setConvertFunction([](const QByteArray &value) -> QVariant {
            return value.toLongLong() / 1000.0;
        });

        auto *speed = new SysFsSensor(QStringLiteral("speed"), base + QLatin1String("/clock_mhz"), parent);
        speed->setName(tr("Memory Frequency"));
        speed->setShortName(tr("Frequency"));
        speed->setUnit(KSysGuard::UnitMegaHertz);

        auto *curFreq = new SysFsSensor(QStringLiteral("frequency"), base + QLatin1String("/devfreq/cur_freq"), parent);
        curFreq->setName(tr("GPU Clock"));
        curFreq->setShortName(tr("Clock"));
        curFreq->setUnit(KSysGuard::UnitMegaHertz);
        curFreq->setConvertFunction([](const QByteArray &value) -> QVariant {
            return value.toLongLong() / 1000000.0;
        });

        m_sensors << usage << temp << speed << curFreq;
    }
};

K_PLUGIN_CLASS_WITH_JSON(KgslGpuPlugin, "kgsl_gpu_plugin.json")

#include "kgsl_gpu_plugin.moc"
