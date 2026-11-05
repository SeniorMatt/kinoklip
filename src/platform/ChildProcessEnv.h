// SPDX-License-Identifier: GPL-3.0-or-later
// ChildProcessEnv — gives external programs the user's environment back when running bundled.
#pragma once

#include <QProcessEnvironment>
#include <QStringList>

class QProcess;

namespace kk::platform {

/// The portable Linux bundle's launcher (packaging/linux/apprun.sh) points LD_LIBRARY_PATH,
/// QT_PLUGIN_PATH, XDG_DATA_DIRS, ... at the bundle. Programs we start (xdg-open, a file
/// manager, ffprobe of the host, ...) must not inherit that: they would load our Qt/KF6 or
/// see our data dirs and crash or misbehave. The launcher therefore records:
///   KINOKLIP_ORIG_VARS            space-separated names of the variables it changed
///   KINOKLIP_ORIG_<NAME>          the original value (absent = the variable was unset)
/// These functions undo exactly those changes for child processes.
///
/// Not bundled (no KINOKLIP_ORIG_VARS) everything is a no-op / the plain system environment.
/// Thread-safety: childEnvironment() is reentrant; ScopedOriginalEnvironment changes the
/// process environment and must only be used on the GUI thread around a synchronous spawn.

/// Names of the variables the launcher changed (empty when not bundled).
[[nodiscard]] QStringList launcherChangedVariables();

/// Environment for a child process: the current one with the launcher's changes undone and
/// the KINOKLIP_* bookkeeping variables removed.
[[nodiscard]] QProcessEnvironment childEnvironment();

/// Sets childEnvironment() on @p process (call before start()).
void prepareChildProcess(QProcess& process);

/// RAII: restores the original values in this process' environment for its lifetime, so
/// APIs that spawn helpers internally and synchronously pass them on (not enough for
/// QDesktopServices on Wayland, which spawns later: use platform::openUrl, OpenUrl.h). Re-applies the bundle
/// values afterwards.
class ScopedOriginalEnvironment
{
public:
    ScopedOriginalEnvironment();
    ~ScopedOriginalEnvironment();
    ScopedOriginalEnvironment(const ScopedOriginalEnvironment&) = delete;
    ScopedOriginalEnvironment& operator=(const ScopedOriginalEnvironment&) = delete;

private:
    struct Saved
    {
        QByteArray name;
        QByteArray value;
        bool wasSet = false;
    };
    QList<Saved> m_saved;
};

} // namespace kk::platform
