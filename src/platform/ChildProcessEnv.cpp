// SPDX-License-Identifier: GPL-3.0-or-later
// ChildProcessEnv implementation.
#include "platform/ChildProcessEnv.h"

#include <QDesktopServices>
#include <QProcess>
#include <QUrl>

namespace kk::platform {

namespace {
constexpr char kVarsKey[] = "KINOKLIP_ORIG_VARS";
constexpr char kOrigPrefix[] = "KINOKLIP_ORIG_";
} // namespace

QStringList launcherChangedVariables()
{
    return QString::fromLocal8Bit(qgetenv(kVarsKey)).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

QProcessEnvironment childEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QStringList changed = launcherChangedVariables();
    for (const QString& name : changed) {
        const QString origKey = QLatin1String(kOrigPrefix) + name;
        if (env.contains(origKey))
            env.insert(name, env.value(origKey));
        else
            env.remove(name);
    }
    // Our bookkeeping is meaningless to other programs (and a child Kinoklip started from
    // a file manager must not think it runs from this bundle).
    const QStringList keys = env.keys();
    for (const QString& key : keys) {
        if (key.startsWith(QLatin1String("KINOKLIP_")))
            env.remove(key);
    }
    return env;
}

void prepareChildProcess(QProcess& process)
{
    process.setProcessEnvironment(childEnvironment());
}

bool openUrl(const QUrl& url)
{
    const ScopedOriginalEnvironment original;
    return QDesktopServices::openUrl(url);
}

ScopedOriginalEnvironment::ScopedOriginalEnvironment()
{
    const QStringList changed = launcherChangedVariables();
    for (const QString& name : changed) {
        const QByteArray key = name.toLocal8Bit();
        Saved saved {key, qgetenv(key.constData()), qEnvironmentVariableIsSet(key.constData())};
        m_saved.append(saved);
        const QByteArray origKey = QByteArray(kOrigPrefix) + key;
        if (qEnvironmentVariableIsSet(origKey.constData()))
            qputenv(key.constData(), qgetenv(origKey.constData()));
        else
            qunsetenv(key.constData());
    }
}

ScopedOriginalEnvironment::~ScopedOriginalEnvironment()
{
    for (const Saved& saved : std::as_const(m_saved)) {
        if (saved.wasSet)
            qputenv(saved.name.constData(), saved.value);
        else
            qunsetenv(saved.name.constData());
    }
}

} // namespace kk::platform
