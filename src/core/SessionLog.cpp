// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SessionLog.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QMutexLocker>

#include <atomic>

namespace kk::sessionlog {

namespace {

QtMessageHandler g_previous = nullptr;
std::atomic<bool> g_installed {false};

void handler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    // First the log, so a fatal message is kept before the previous handler aborts.
    if (wanted(type, context.category))
        buffer().append(
            type, QString::fromLatin1(context.category ? context.category : "default"), message);
    if (g_previous)
        g_previous(type, context, message);
    else
        qt_message_output(type, context, message); // Qt's own: stderr, journal, as without us
}

qint64 textBytes(const Entry& e)
{
    return e.text.toUtf8().size() + e.category.toUtf8().size();
}

} // namespace

RingBuffer::RingBuffer(int maxLines, qint64 maxBytes)
    : m_maxLines(maxLines)
    , m_maxBytes(maxBytes)
    , m_startMs(QDateTime::currentMSecsSinceEpoch())
{ }

void RingBuffer::append(QtMsgType type, const QString& category, const QString& text)
{
    Entry entry;
    entry.type = type;
    entry.category = category;
    entry.text = text.size() > kMaxMessageChars
        ? text.left(kMaxMessageChars) + QStringLiteral("...")
        : text;
    const qint64 size = textBytes(entry);
    QMutexLocker lock(&m_mutex);
    entry.milliseconds = QDateTime::currentMSecsSinceEpoch() - m_startMs;
    m_entries.push_back(std::move(entry));
    m_bytes += size;
    while (!m_entries.empty() && (int(m_entries.size()) > m_maxLines || m_bytes > m_maxBytes)) {
        m_bytes -= textBytes(m_entries.front());
        m_entries.pop_front();
        ++m_dropped;
    }
}

QList<Entry> RingBuffer::entries() const
{
    QMutexLocker lock(&m_mutex);
    return QList<Entry>(m_entries.begin(), m_entries.end());
}

qint64 RingBuffer::dropped() const
{
    QMutexLocker lock(&m_mutex);
    return m_dropped;
}

qint64 RingBuffer::bytes() const
{
    QMutexLocker lock(&m_mutex);
    return m_bytes;
}

qint64 RingBuffer::elapsedMilliseconds() const
{
    return QDateTime::currentMSecsSinceEpoch() - m_startMs;
}

void RingBuffer::clear()
{
    QMutexLocker lock(&m_mutex);
    m_entries.clear();
    m_bytes = 0;
    m_dropped = 0;
}

RingBuffer& buffer()
{
    static RingBuffer instance;
    return instance;
}

bool wanted(QtMsgType type, const char* category)
{
    switch (type) {
    case QtWarningMsg:
    case QtCriticalMsg:
    case QtFatalMsg:
        return true;
    case QtInfoMsg: {
        const QByteArray name(category ? category : "");
        return name.startsWith("kk.") || name.startsWith("kinoklip.");
    }
    case QtDebugMsg:
        return false;
    }
    return false;
}

void record(QtMsgType type, const char* category, const QString& text)
{
    if (wanted(type, category))
        buffer().append(type, QString::fromLatin1(category), text);
}

void installMessageHandler()
{
    if (g_installed.exchange(true))
        return;
    buffer(); // start the clock now
    g_previous = qInstallMessageHandler(handler);
}

} // namespace kk::sessionlog
