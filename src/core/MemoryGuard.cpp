// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/MemoryGuard.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <chrono>
#include <signal.h>
#include <thread>

namespace kk::memguard {

int64_t availableBytesFromMeminfo(const QByteArray& meminfo)
{
    for (const QByteArray& line : meminfo.split('\n')) {
        if (!line.startsWith("MemAvailable:"))
            continue;
        const QByteArray number = line.mid(13).trimmed().split(' ').value(0);
        bool ok = false;
        const qint64 kib = number.toLongLong(&ok);
        return ok ? int64_t(kib) * 1024 : -1;
    }
    return -1;
}

int64_t availableBytes()
{
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    return availableBytesFromMeminfo(file.readAll());
}

bool shouldPause(int64_t available, bool paused)
{
    if (available < 0)
        return false;
    return paused ? available < kResumeBytes : available < kPauseBytes;
}

bool waitForMemory(const std::function<bool()>& cancelled,
    const std::function<void(bool)>& onChange, const std::function<int64_t()>& availableFn)
{
    const auto available = [&] { return availableFn ? availableFn() : availableBytes(); };
    if (!shouldPause(available(), false))
        return true;
    if (onChange)
        onChange(true);
    while (shouldPause(available(), true)) {
        if (cancelled && cancelled())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (onChange)
        onChange(false);
    return true;
}

QString renderMarkerPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("render-running.txt"));
}

void markRenderStart(const QString& outputPath)
{
    const QString path = renderMarkerPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QByteArray::number(qint64(QCoreApplication::applicationPid())) + '\n'
            + outputPath.toUtf8());
    }
}

void markRenderEnd()
{
    QFile::remove(renderMarkerPath());
}

QString takeInterruptedRender(const QString& markerPath)
{
    const QString path = markerPath.isEmpty() ? renderMarkerPath() : markerPath;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QList<QByteArray> lines = file.readAll().split('\n');
    file.close();
    const qint64 pid = lines.value(0).toLongLong();
#ifdef Q_OS_UNIX
    if (pid > 0 && pid != QCoreApplication::applicationPid() && ::kill(pid_t(pid), 0) == 0)
        return {}; // another process is rendering right now
#endif
    QFile::remove(path);
    const QString output = QString::fromUtf8(lines.value(1));
    return output.isEmpty() ? QStringLiteral("?") : output;
}

} // namespace kk::memguard
