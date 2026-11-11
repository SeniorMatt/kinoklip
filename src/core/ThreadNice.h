// SPDX-License-Identifier: GPL-3.0-or-later
// ThreadNice — makes the calling thread (and every thread it starts afterwards) yield to the
// others. QThread::setPriority() does nothing for the normal Linux scheduling policy, so
// background work (proxy and render cache generation) would compete with the playback threads
// like any other thread; a higher nice value gives it a small share only when cores are busy.
#pragma once

#include <QtGlobal>

#ifdef Q_OS_LINUX
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace kk {

/// Raises the nice value of the calling thread to at least @p nice (0 .. 19). Raising needs no
/// privilege; threads created later by this thread inherit it. No effect off Linux, or with
/// KK_NO_BACKGROUND_NICE set (for comparisons).
inline void yieldToOtherThreads(int nice)
{
#ifdef Q_OS_LINUX
    static const bool off = qEnvironmentVariableIsSet("KK_NO_BACKGROUND_NICE");
    if (off)
        return;
    setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), nice);
#else
    (void)nice;
#endif
}

} // namespace kk
