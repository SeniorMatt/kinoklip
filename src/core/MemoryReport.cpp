// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/MemoryReport.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QSet>

namespace kk::memreport {

namespace {

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/// Value of "Key:   123 kB" in @p text, in MiB; -1 if missing.
double kibLine(const QByteArray& text, const QByteArray& key)
{
    const QByteArray prefix = key + ':';
    for (const QByteArray& line : text.split('\n')) {
        if (line.startsWith(prefix))
            return line.mid(prefix.size()).simplified().split(' ').value(0).toDouble() / 1024.0;
    }
    return -1;
}

/// "123 KiB" / "4 MiB" / "7" (bytes) -> MiB.
double toMiB(const QByteArray& value)
{
    const QList<QByteArray> parts = value.simplified().split(' ');
    const double n = parts.value(0).toDouble();
    const QByteArray unit = parts.value(1);
    if (unit == "KiB")
        return n / 1024.0;
    if (unit == "MiB")
        return n;
    if (unit == "GiB")
        return n * 1024.0;
    return n / (1024.0 * 1024.0);
}

} // namespace

QStringList drmLines(const QList<QByteArray>& fdinfoTexts)
{
    QStringList out;
    QSet<QByteArray> seen;
    QMap<QString, double> sums; // "driver region-key" -> MiB
    for (const QByteArray& text : fdinfoTexts) {
        QByteArray driver;
        QByteArray client;
        QList<QPair<QByteArray, QByteArray>> values;
        for (const QByteArray& line : text.split('\n')) {
            const int colon = int(line.indexOf(':'));
            if (colon < 0)
                continue;
            const QByteArray key = line.left(colon);
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (key == "drm-driver")
                driver = value;
            else if (key == "drm-client-id")
                client = value;
            else if (key.startsWith("drm-total-") || key.startsWith("drm-resident-")
                || key.startsWith("drm-memory-"))
                values.append({key, value});
        }
        if (driver.isEmpty())
            continue;
        if (!client.isEmpty() && seen.contains(driver + '/' + client))
            continue; // the same DRM client through another descriptor
        seen.insert(driver + '/' + client);
        for (const auto& [key, value] : values)
            sums[QString::fromLatin1(driver + ' ' + key.mid(4))] += toMiB(value);
    }
    for (auto it = sums.cbegin(); it != sums.cend(); ++it)
        out.append(QStringLiteral("drm %1: %2 MiB").arg(it.key()).arg(it.value(), 0, 'f', 0));
    return out;
}

QStringList lines()
{
    QStringList out;
#ifdef Q_OS_LINUX
    const QByteArray status = readAll(QStringLiteral("/proc/self/status"));
    const QByteArray meminfo = readAll(QStringLiteral("/proc/meminfo"));
    auto add = [&](const char* label, const QByteArray& text, const char* key) {
        const double v = kibLine(text, key);
        if (v >= 0)
            out.append(QStringLiteral("%1: %2 MiB").arg(QLatin1String(label)).arg(v, 0, 'f', 0));
    };
    add("RSS", status, "VmRSS");
    add("RssShmem", status, "RssShmem");
    add("MemAvailable", meminfo, "MemAvailable");
    add("Shmem (system)", meminfo, "Shmem");
    const double swapTotal = kibLine(meminfo, "SwapTotal");
    const double swapFree = kibLine(meminfo, "SwapFree");
    if (swapTotal >= 0 && swapFree >= 0)
        out.append(QStringLiteral("SwapUsed: %1 MiB").arg(swapTotal - swapFree, 0, 'f', 0));
    QList<QByteArray> fdinfo;
    const QDir dir(QStringLiteral("/proc/self/fdinfo"));
    for (const QString& name : dir.entryList(QDir::Files | QDir::NoDotAndDotDot)) {
        const QByteArray text = readAll(dir.filePath(name));
        if (text.contains("drm-driver:"))
            fdinfo.append(text);
    }
    out += drmLines(fdinfo);
#endif
    return out;
}

QString oneLine()
{
    return lines().join(QStringLiteral(", "));
}

} // namespace kk::memreport
