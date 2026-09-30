/*
    SPDX-FileCopyrightText: 2026 XiaomiPad8Pro-drm-display 项目
    SPDX-License-Identifier: GPL-3.0-only

    实现思路参考了 gpu-monitor-fix 项目（未能找到其公开地址），本文件为其重写。

    ksystemstats 的 container-safe 磁盘 / 温度传感器后端。

    范围（按 09-30 实测收敛，不是照搬 gpu-monitor-fix）：
    - 磁盘：上游 ksystemstats_plugin_disk 依赖 Solid(fstab/devices backend)，本容器里没有
      Solid ⇒ 它只注册出空的 disk/all/* 壳子（subscribe 读回是 MISSING）⇒ 这里用 statvfs +
      /proc/diskstats 重做出真值。
    - 温度：本机 /sys/class/hwmon 为空 ⇒ 上游 lmsensors 注册不出任何 chip（thermal 整个不存在）
      ⇒ 这里从 /sys/class/thermal/thermal_zone 节点补 CPU / 电池温度。
    - 网络：**不在本插件范围内**。实测上游 ksystemstats_plugin_network 在本机是好的
      （wlan0 的 download/upload/ipv4address 都有值，还带 dns/gateway/signal），
      所以既不移植、也不停用上游 network，避免把能用的换成更弱的。

    传感器 id 的拼法是 <container>/<object>/<property>，只要容器叫 disk/thermal、对象叫 all/
    cpu/battery、属性叫 used/temperature 等，就天然对得上 overview.page 里现有的磁盘绑定
    （disk/all/used），**不需要改页面**。温度面页面本就没有，传感器在“所有传感器”里可见，
    需要上屏时用户在 UI 自行加一个温度 face。
*/

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <KPluginFactory>

#include <systemstats/AggregateSensor.h>
#include <systemstats/SensorContainer.h>
#include <systemstats/SensorObject.h>
#include <systemstats/SensorPlugin.h>
#include <systemstats/SensorProperty.h>

#include <formatter/Unit.h>

#include <sys/statvfs.h>

using namespace KSysGuard;

namespace
{
// 只统计这些“真实”文件系统，过滤掉 tmpfs/overlay/squashfs 等
const QSet<QString> kRealFs = {QStringLiteral("ext2"), QStringLiteral("ext3"), QStringLiteral("ext4"),
                               QStringLiteral("btrfs"), QStringLiteral("xfs"), QStringLiteral("f2fs"),
                               QStringLiteral("reiserfs")};

QString readOneLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll()).trimmed();
}
} // namespace

// ============================================================================
// 磁盘：每个挂载卷一个 SensorObject，容量走 statvfs，IO 走 /proc/diskstats
// ============================================================================

class DiskVolume : public SensorObject
{
    Q_OBJECT
public:
    DiskVolume(const QString &deviceId, const QString &mountPoint, SensorContainer *parent)
        : SensorObject(deviceId, mountPoint, parent)
        , m_deviceId(deviceId)
        , m_mountPoint(mountPoint)
    {
        new SensorProperty(QStringLiteral("name"), mountPoint, this);

        m_total = new SensorProperty(QStringLiteral("total"), tr("Total Space"), QVariant(0.0), this);
        m_total->setUnit(UnitByte);
        m_total->setVariantType(QVariant::ULongLong);
        m_used = new SensorProperty(QStringLiteral("used"), tr("Used Space"), QVariant(0.0), this);
        m_used->setUnit(UnitByte);
        m_used->setVariantType(QVariant::ULongLong);
        m_free = new SensorProperty(QStringLiteral("free"), tr("Free Space"), QVariant(0.0), this);
        m_free->setUnit(UnitByte);
        m_free->setVariantType(QVariant::ULongLong);

        auto *usedPercent = new PercentageSensor(this, QStringLiteral("usedPercent"), tr("Percentage Used"));
        usedPercent->setBaseSensor(m_used);
        auto *freePercent = new PercentageSensor(this, QStringLiteral("freePercent"), tr("Percentage Free"));
        freePercent->setBaseSensor(m_free);

        m_read = new SensorProperty(QStringLiteral("read"), tr("Read Rate"), QVariant(0.0), this);
        m_read->setUnit(UnitByteRate);
        m_read->setVariantType(QVariant::Double);
        m_write = new SensorProperty(QStringLiteral("write"), tr("Write Rate"), QVariant(0.0), this);
        m_write->setUnit(UnitByteRate);
        m_write->setVariantType(QVariant::Double);
    }

    // statvfs 读容量（不依赖 Solid/udev）
    void updateUsage()
    {
        struct statvfs buf;
        if (::statvfs(m_mountPoint.toLocal8Bit().constData(), &buf) != 0 || buf.f_blocks == 0) {
            return;
        }
        const qulonglong total = static_cast<qulonglong>(buf.f_blocks) * buf.f_frsize;
        const qulonglong free = static_cast<qulonglong>(buf.f_bfree) * buf.f_frsize;
        m_total->setValue(total);
        m_total->setMax(total);
        m_used->setMax(total);
        m_free->setMax(total);
        m_free->setValue(free);
        m_used->setValue(total - free);
    }

    void setIORates(double readRate, double writeRate)
    {
        m_read->setValue(readRate);
        m_write->setValue(writeRate);
    }

    QString deviceId() const
    {
        return m_deviceId;
    }

private:
    QString m_deviceId;
    QString m_mountPoint;
    SensorProperty *m_total = nullptr;
    SensorProperty *m_used = nullptr;
    SensorProperty *m_free = nullptr;
    SensorProperty *m_read = nullptr;
    SensorProperty *m_write = nullptr;
};

// ============================================================================
// 温度：多热区取值的辅助记录（cpu_therm 单值优先，否则逐核取最大；battery 单值）
// ============================================================================

namespace
{
struct ThermalSensor {
    SensorProperty *prop = nullptr;
    QStringList zoneTempPaths; // 一个或多个 thermal_zone<N>/temp
    bool takeMax = false;
};
} // namespace

// ============================================================================
// ContainerIoPlugin
// ============================================================================

class ContainerIoPlugin : public SensorPlugin
{
    Q_OBJECT
public:
    ContainerIoPlugin(QObject *parent, const QVariantList &args)
        : SensorPlugin(parent, args)
    {
        initDisk();
        initThermal();
    }

    /**
     * ksystemstats 的刷新定时器会调这里。plain SensorProperty 不会自己读 /proc、/sys，
     * 必须在这里主动刷新，否则传感器只有元数据没有值（droid 实测坑 #2）。
     * 用 isSubscribed() 门控：没人看的传感器不做无谓的 statvfs / 解析。
     */
    void update() override
    {
        updateDisk();
        updateThermal();
    }

    QString providerName() const override
    {
        // 与上游 disk 的 provider 不撞（它叫 disk，我们叫 containerio）；但本插件产出的
        // disk **容器**会和上游撞同名 DBus 路径 ⇒ 安装脚本把上游 disk.so 停用。
        // 不产 network 容器，故不动上游 network。
        return QStringLiteral("containerio");
    }

private:
    // ----- 磁盘 -----
    SensorContainer *m_diskContainer = nullptr;
    QList<DiskVolume *> m_diskVolumes;
    qint64 m_lastDiskTs = 0;
    QMap<QString, QPair<quint64, quint64>> m_lastDiskIo;

    void initDisk()
    {
        m_diskContainer = new SensorContainer(QStringLiteral("disk"), tr("Disks"), this);
        m_diskVolumes.clear();

        QFile mounts(QStringLiteral("/proc/self/mounts"));
        if (mounts.open(QIODevice::ReadOnly)) {
            const auto lines = QString::fromUtf8(mounts.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const auto &raw : lines) {
                const auto parts = raw.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if (parts.size() < 3) {
                    continue;
                }
                const QString device = parts[0];
                const QString mountPoint = parts[1];
                const QString fsType = parts[2];
                if (!kRealFs.contains(fsType) || !device.startsWith(QLatin1String("/dev/"))) {
                    continue;
                }
                if (mountPoint.startsWith(QLatin1String("/run"))) {
                    continue;
                }
                const QString id = QFileInfo(device).fileName();
                const bool dup = std::any_of(m_diskVolumes.cbegin(), m_diskVolumes.cend(), [&](DiskVolume *v) {
                    return v->deviceId() == id;
                });
                if (dup) {
                    continue; // 同一设备多挂载点，只留一个卷对象
                }
                m_diskVolumes.append(new DiskVolume(id, mountPoint, m_diskContainer));
            }
        }
        addDiskAggregates();
        addContainer(m_diskContainer);
    }

    void addDiskAggregates()
    {
        auto *all = new SensorObject(QStringLiteral("all"), tr("All Disks"), m_diskContainer);
        auto notAll = [](const SensorProperty *s) {
            return s->parentObject()->id() != QStringLiteral("all");
        };

        auto *total = new AggregateSensor(all, QStringLiteral("total"), tr("Total Space"));
        total->setUnit(UnitByte);
        total->setVariantType(QVariant::ULongLong);
        total->setMatchSensors(QRegularExpression(QStringLiteral(".*")), QStringLiteral("total"));
        total->setFilterFunction(notAll);

        auto *free = new AggregateSensor(all, QStringLiteral("free"), tr("Free Space"));
        free->setUnit(UnitByte);
        free->setVariantType(QVariant::ULongLong);
        free->setMatchSensors(QRegularExpression(QStringLiteral(".*")), QStringLiteral("free"));
        free->setFilterFunction(notAll);

        // 页面绑的 disk/all/used 就是它
        auto *used = new AggregateSensor(all, QStringLiteral("used"), tr("Used Space"));
        used->setUnit(UnitByte);
        used->setVariantType(QVariant::ULongLong);
        used->setMatchSensors(QRegularExpression(QStringLiteral(".*")), QStringLiteral("used"));
        used->setFilterFunction(notAll);

        auto *read = new AggregateSensor(all, QStringLiteral("read"), tr("Read Rate"), QVariant(0.0));
        read->setUnit(UnitByteRate);
        read->setVariantType(QVariant::Double);
        read->setMatchSensors(QRegularExpression(QStringLiteral("^(?!all).*$")), QStringLiteral("read"));
        read->setFilterFunction(notAll);

        auto *write = new AggregateSensor(all, QStringLiteral("write"), tr("Write Rate"), QVariant(0.0));
        write->setUnit(UnitByteRate);
        write->setVariantType(QVariant::Double);
        write->setMatchSensors(QRegularExpression(QStringLiteral("^(?!all).*$")), QStringLiteral("write"));
        write->setFilterFunction(notAll);

        auto *freePct = new PercentageSensor(all, QStringLiteral("freePercent"), tr("Percentage Free"));
        freePct->setBaseSensor(free);
        auto *usedPct = new PercentageSensor(all, QStringLiteral("usedPercent"), tr("Percentage Used"));
        usedPct->setBaseSensor(used);
    }

    void updateDisk()
    {
        for (auto *vol : m_diskVolumes) {
            if (vol->isSubscribed()) {
                vol->updateUsage();
            }
        }

        // 让聚合的 used/free 的 max 跟 total 对齐（横条图用它当量程）
        if (auto *all = m_diskContainer->object(QStringLiteral("all"))) {
            SensorProperty *total = all->sensor(QStringLiteral("total"));
            if (total && total->value().isValid()) {
                const qulonglong t = total->value().toULongLong();
                if (auto *used = all->sensor(QStringLiteral("used")))
                    used->setMax(t);
                if (auto *free = all->sensor(QStringLiteral("free")))
                    free->setMax(t);
            }
        }

        // IO 速率：/proc/diskstats 两次采样求增量。字段（含 major minor name）：
        // [2]=名字 [5]=读扇区 [9]=写扇区，扇区固定 512B
        QFile ds(QStringLiteral("/proc/diskstats"));
        if (!ds.open(QIODevice::ReadOnly)) {
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QMap<QString, QPair<quint64, quint64>> current;
        const auto lines = QString::fromUtf8(ds.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const auto &raw : lines) {
            const auto fields = raw.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (fields.size() < 10) {
                continue;
            }
            current[fields[2]] = {fields[5].toULongLong() * 512, fields[9].toULongLong() * 512};
        }

        if (m_lastDiskTs != 0) {
            const double elapsed = (now - m_lastDiskTs) / 1000.0;
            if (elapsed > 0) {
                for (auto *vol : m_diskVolumes) {
                    if (!vol->isSubscribed()) {
                        continue;
                    }
                    auto it = current.constFind(vol->deviceId());
                    if (it == current.constEnd()) {
                        continue;
                    }
                    const auto prev = m_lastDiskIo.value(vol->deviceId());
                    vol->setIORates(qMax(0.0, (it->first - prev.first) / elapsed), qMax(0.0, (it->second - prev.second) / elapsed));
                }
            }
        }
        m_lastDiskTs = now;
        m_lastDiskIo = current;
    }

    // ----- 温度 -----
    SensorContainer *m_thermalContainer = nullptr;
    QList<ThermalSensor> m_thermalSensors;

    void initThermal()
    {
        QString cpuPackageTempPath; // type=cpu_therm，封装级，优先
        QStringList cpuCoreTempPaths; // type 以 cpu- / cpuss- 开头，取最大兜底
        QString batteryTempPath; // type=battery

        QDir thermalDir(QStringLiteral("/sys/class/thermal"));
        for (const auto &zone : thermalDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!zone.startsWith(QLatin1String("thermal_zone"))) {
                continue;
            }
            const QString zonePath = thermalDir.filePath(zone);
            const QString type = readOneLine(zonePath + QLatin1String("/type"));
            const QString tempPath = zonePath + QLatin1String("/temp");
            if (!QFileInfo::exists(tempPath)) {
                continue;
            }
            if (type == QLatin1String("cpu_therm")) {
                cpuPackageTempPath = tempPath;
            } else if (type == QLatin1String("battery")) {
                batteryTempPath = tempPath;
            } else if (type.startsWith(QLatin1String("cpu-")) || type.startsWith(QLatin1String("cpuss-"))) {
                cpuCoreTempPaths << tempPath;
            }
        }

        if (cpuPackageTempPath.isEmpty() && cpuCoreTempPaths.isEmpty() && batteryTempPath.isEmpty()) {
            return; // 一个可用温度源都没有 ⇒ 不注册 thermal 容器
        }

        m_thermalContainer = new SensorContainer(QStringLiteral("thermal"), tr("Thermal"), this);

        if (!cpuPackageTempPath.isEmpty() || !cpuCoreTempPaths.isEmpty()) {
            auto *cpu = new SensorObject(QStringLiteral("cpu"), tr("CPU"), m_thermalContainer);
            auto *prop = new SensorProperty(QStringLiteral("temperature"), tr("CPU Temperature"), QVariant(0.0), cpu);
            prop->setUnit(UnitCelsius);
            prop->setVariantType(QVariant::Double);
            ThermalSensor s;
            s.prop = prop;
            if (!cpuPackageTempPath.isEmpty()) {
                s.zoneTempPaths = {cpuPackageTempPath};
                s.takeMax = false;
            } else {
                s.zoneTempPaths = cpuCoreTempPaths;
                s.takeMax = true;
            }
            m_thermalSensors << s;
        }

        if (!batteryTempPath.isEmpty()) {
            auto *batt = new SensorObject(QStringLiteral("battery"), tr("Battery"), m_thermalContainer);
            auto *prop = new SensorProperty(QStringLiteral("temperature"), tr("Battery Temperature"), QVariant(0.0), batt);
            prop->setUnit(UnitCelsius);
            prop->setVariantType(QVariant::Double);
            ThermalSensor s;
            s.prop = prop;
            s.zoneTempPaths = {batteryTempPath};
            s.takeMax = false;
            m_thermalSensors << s;
        }

        addContainer(m_thermalContainer);
    }

    void updateThermal()
    {
        for (const auto &s : m_thermalSensors) {
            if (!s.prop->isSubscribed()) {
                continue;
            }
            double result = 0.0;
            bool have = false;
            for (const auto &path : s.zoneTempPaths) {
                const QString v = readOneLine(path);
                if (v.isEmpty()) {
                    continue;
                }
                const double celsius = v.toDouble() / 1000.0; // 毫摄氏度 -> °C
                if (!have || (s.takeMax && celsius > result)) {
                    result = celsius;
                    have = true;
                }
            }
            if (have) {
                s.prop->setValue(result);
            }
        }
    }
};

K_PLUGIN_CLASS_WITH_JSON(ContainerIoPlugin, "container_io_plugin.json")

#include "container_io_plugin.moc"
