// SPDX-License-Identifier: GPL-3.0-or-later
// SessionLog — what the application did in this session, kept in memory for Help > Export Log
// File (spec section 64). A message handler (installMessageHandler(), called first thing in
// main()) copies warnings and errors of every category and the info messages of the kk.* and
// kinoklip.* categories into a bounded ring buffer and then passes the message on to the
// handler that was there before, so what reaches stderr does not change. Nothing is written
// to disk; the buffer is read only when the user exports the log (app/LogExport.h).
#pragma once

#include <QList>
#include <QMutex>
#include <QString>

#include <deque>

namespace kk::sessionlog {

/// One kept message.
struct Entry
{
    qint64 milliseconds = 0; ///< since the log started (the process, in practice)
    QtMsgType type = QtInfoMsg;
    QString category; ///< "kk.media", "qt.qpa.xcb", "default"
    QString text;
};

/// How much is kept: the last kMaxLines messages and at most kMaxBytes of text, whichever limit
/// is reached first; the oldest are dropped and counted.
inline constexpr int kMaxLines = 20000;
inline constexpr qint64 kMaxBytes = 4 * 1024 * 1024;
/// A single message longer than this many characters is cut (and ends in "...").
inline constexpr int kMaxMessageChars = 4096;

/// A thread-safe ring buffer of messages. The message handler may run on any thread.
class RingBuffer
{
public:
    explicit RingBuffer(int maxLines = kMaxLines, qint64 maxBytes = kMaxBytes);

    /// Keeps a message, dropping the oldest ones while a limit is exceeded.
    void append(QtMsgType type, const QString& category, const QString& text);
    /// The kept messages, oldest first.
    [[nodiscard]] QList<Entry> entries() const;
    /// How many earlier messages were dropped because a limit was reached.
    [[nodiscard]] qint64 dropped() const;
    /// Bytes of text kept (UTF-8).
    [[nodiscard]] qint64 bytes() const;
    /// Milliseconds since the buffer was made.
    [[nodiscard]] qint64 elapsedMilliseconds() const;
    void clear();

private:
    mutable QMutex m_mutex;
    std::deque<Entry> m_entries;
    qint64 m_bytes = 0;
    qint64 m_dropped = 0;
    int m_maxLines;
    qint64 m_maxBytes;
    qint64 m_startMs;
};

/// The session's buffer (made on first use).
[[nodiscard]] RingBuffer& buffer();

/// True if a message of this type and category belongs in the log: every warning, critical and
/// fatal message, and info messages of the "kk." and "kinoklip." categories. Debug messages
/// never do.
[[nodiscard]] bool wanted(QtMsgType type, const char* category);

/// Keeps a message straight in the session buffer, for lines that are not printed (the
/// per-second playback statistics when KK_PLAYBACK_STATS is not set). Applies wanted().
void record(QtMsgType type, const char* category, const QString& text);

/// Installs the message handler described above. Safe to call twice (the second call does
/// nothing).
void installMessageHandler();

} // namespace kk::sessionlog
