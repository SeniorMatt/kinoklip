#pragma once

#include <QProcess>
#include <QString>
#include <QStringList>

namespace kk {

/// Starts one of Kinoklip's own helper programs (kinoklip-ofx-probe, kinoklip-audio-probe,
/// kinoklip-hwenc-probe). In an AppImage that runs on its bundled glibc (packaging/linux/apprun.sh
/// exports KINOKLIP_LDSO and KINOKLIP_LDSO_PATH) the helper is started through the same loader as
/// the app; started directly it would get the host's glibc, which can be older than the one the
/// bundled libraries and plugins need ("version `GLIBC_2.43' not found"). Never use this for host
/// programs (Python, a file manager): they need the host's glibc.
inline void startHelperProcess(QProcess& process, const QString& program, const QStringList& args)
{
    const QString loader = qEnvironmentVariable("KINOKLIP_LDSO");
    if (loader.isEmpty()) {
        process.start(program, args);
        return;
    }
    QStringList loaderArgs{QStringLiteral("--library-path"), qEnvironmentVariable("KINOKLIP_LDSO_PATH"),
        QStringLiteral("--argv0"), program, program};
    process.start(loader, loaderArgs + args);
}

} // namespace kk
