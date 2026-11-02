// SPDX-License-Identifier: GPL-3.0-or-later
// kk_kwinshot -- saves a screenshot of a KWin session (org.kde.KWin.ScreenShot2
// CaptureWorkspace) as PNG, for the nested Wayland checks: KWin's virtual backend renders
// off screen, so this is the only way to look at it. KWin must run with
// KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 (scripts/wayland_session.sh sets it).
//
//   kk_kwinshot out.png
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QImage>
#include <QTextStream>
#include <QVariantMap>

#include <unistd.h>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        QTextStream(stderr) << "usage: kk_kwinshot out.png\n";
        return 2;
    }
    int fds[2];
    if (pipe(fds) != 0)
        return 1;
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
        QStringLiteral("/org/kde/KWin/ScreenShot2"), QStringLiteral("org.kde.KWin.ScreenShot2"),
        QStringLiteral("CaptureWorkspace"));
    QVariantMap options;
    options.insert(QStringLiteral("include-cursor"), true);
    options.insert(QStringLiteral("native-resolution"), true);
    call << options << QVariant::fromValue(QDBusUnixFileDescriptor(fds[1]));
    const QDBusReply<QVariantMap> reply = QDBusConnection::sessionBus().call(call);
    ::close(fds[1]);
    if (!reply.isValid()) {
        QTextStream(stderr) << "kk_kwinshot: " << reply.error().message() << "\n";
        return 1;
    }
    const QVariantMap info = reply.value();
    QFile in;
    in.open(fds[0], QIODevice::ReadOnly, QFileDevice::AutoCloseHandle);
    const QByteArray data = in.readAll();
    const int width = info.value(QStringLiteral("width")).toInt();
    const int height = info.value(QStringLiteral("height")).toInt();
    const int stride = info.value(QStringLiteral("stride")).toInt();
    const auto format = QImage::Format(info.value(QStringLiteral("format")).toInt());
    if (data.size() < qsizetype(stride) * height) {
        QTextStream(stderr) << "kk_kwinshot: short read\n";
        return 1;
    }
    const QImage image(
        reinterpret_cast<const uchar*>(data.constData()), width, height, stride, format);
    return image.copy().save(QString::fromLocal8Bit(argv[1])) ? 0 : 1;
}
