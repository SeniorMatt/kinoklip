// SPDX-License-Identifier: GPL-3.0-or-later
// CubeLut implementation: the .cube parser, interpolation and the file cache.
#include "render/CubeLut.h"

#include "media/MediaSource.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMutex>

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::CubeLut", text);
}

/// Largest .cube file read (a 129³ table as text is about 60 MB).
constexpr qint64 kMaxFileBytes = 128LL << 20;
/// How long a cached LUT is trusted before its file identity is checked again.
constexpr qint64 kRecheckMs = 500;
/// Cached LUT files.
constexpr int kMaxCached = 8;

/// Parses up to three floats from a whitespace-separated line.
int parseFloats(const QByteArray& line, float* out, int max)
{
    int n = 0;
    qsizetype i = 0;
    const qsizetype len = line.size();
    while (i < len && n <= max) {
        while (i < len && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i >= len)
            break;
        qsizetype j = i;
        while (j < len && line[j] != ' ' && line[j] != '\t')
            ++j;
        if (n == max)
            return max + 1; // too many values
        bool ok = false;
        const float v = line.mid(i, j - i).toFloat(&ok);
        if (!ok || !std::isfinite(v))
            return -1;
        out[n++] = v;
        i = j;
    }
    return n;
}

} // namespace

bool CubeLut::parse(const QByteArray& text, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        m_data.clear();
        m_size1d = m_size3d = 0;
        return false;
    };
    m_data.clear();
    m_size1d = m_size3d = 0;
    qsizetype expected = -1;
    int lineNumber = 0;
    for (QByteArray line : text.split('\n')) {
        ++lineNumber;
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const char first = line[0];
        if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z')) {
            // A keyword line (only before the data rows).
            if (!m_data.isEmpty())
                return fail(tr("line %1: keyword after the table rows").arg(lineNumber));
            const qsizetype space = line.indexOf(' ');
            const QByteArray key = space < 0 ? line : line.left(space);
            const QByteArray rest = space < 0 ? QByteArray() : line.mid(space + 1).trimmed();
            if (key == "TITLE") {
                m_title = QString::fromUtf8(rest).remove(QLatin1Char('"'));
            } else if (key == "LUT_1D_SIZE" || key == "LUT_3D_SIZE") {
                bool ok = false;
                const int n = rest.toInt(&ok);
                const bool is3d = key == "LUT_3D_SIZE";
                if (!ok || n < 2 || n > (is3d ? kMax3dSize : kMax1dSize))
                    return fail(tr("line %1: unsupported table size").arg(lineNumber));
                (is3d ? m_size3d : m_size1d) = n;
                expected = is3d ? qsizetype(n) * n * n : n;
            } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
                if (parseFloats(rest, key == "DOMAIN_MIN" ? m_min : m_max, 3) != 3)
                    return fail(tr("line %1: malformed domain").arg(lineNumber));
            }
            // Other keywords (LUT_IN_VIDEO_RANGE, ...) are ignored.
            continue;
        }
        if (expected < 0)
            return fail(tr("line %1: table row before the table size").arg(lineNumber));
        float rgb[3];
        if (parseFloats(line, rgb, 3) != 3)
            return fail(tr("line %1: expected three numbers").arg(lineNumber));
        if (m_data.isEmpty())
            m_data.reserve(expected * 3);
        if (m_data.size() >= expected * 3)
            return fail(tr("line %1: more rows than the table size").arg(lineNumber));
        m_data.append(rgb[0]);
        m_data.append(rgb[1]);
        m_data.append(rgb[2]);
    }
    if (expected < 0)
        return fail(tr("no LUT_1D_SIZE or LUT_3D_SIZE"));
    if (m_size1d > 0 && m_size3d > 0)
        return fail(tr("both a 1D and a 3D size"));
    if (m_data.size() != expected * 3)
        return fail(tr("%1 table rows, expected %2").arg(m_data.size() / 3).arg(expected));
    for (int i = 0; i < 3; ++i) {
        if (!(m_max[i] > m_min[i]))
            return fail(tr("empty domain"));
    }
    return true;
}

void CubeLut::apply(float* rgb) const
{
    const int n = size();
    if (n < 2 || m_data.isEmpty())
        return;
    // Table coordinate per channel, clamped to the domain.
    float t[3];
    for (int i = 0; i < 3; ++i) {
        const float u = std::clamp((rgb[i] - m_min[i]) / (m_max[i] - m_min[i]), 0.f, 1.f);
        t[i] = u * float(n - 1);
    }
    const float* d = m_data.constData();
    if (!is3d()) {
        for (int c = 0; c < 3; ++c) {
            const int i0 = std::min(int(t[c]), n - 2);
            const float f = t[c] - float(i0);
            rgb[c] = d[i0 * 3 + c] * (1.f - f) + d[(i0 + 1) * 3 + c] * f;
        }
        return;
    }
    int i0[3];
    float f[3];
    for (int i = 0; i < 3; ++i) {
        i0[i] = std::min(int(t[i]), n - 2);
        f[i] = t[i] - float(i0[i]);
    }
    auto at = [&](int r, int g, int b, int c) {
        return d[((size_t(b) * n + size_t(g)) * n + size_t(r)) * 3 + size_t(c)];
    };
    for (int c = 0; c < 3; ++c) {
        float acc = 0.f;
        for (int corner = 0; corner < 8; ++corner) {
            const int dr = corner & 1, dg = (corner >> 1) & 1, db = (corner >> 2) & 1;
            const float w
                = (dr ? f[0] : 1.f - f[0]) * (dg ? f[1] : 1.f - f[1]) * (db ? f[2] : 1.f - f[2]);
            acc += w * at(i0[0] + dr, i0[1] + dg, i0[2] + db, c);
        }
        rgb[c] = acc;
    }
}

std::shared_ptr<const CubeLut> loadCubeLut(const QString& path, QString* error)
{
    struct Entry
    {
        QString identity;
        qint64 checkedMs = 0;
        std::shared_ptr<const CubeLut> lut;
        QString error;
    };
    static QMutex mutex;
    static QHash<QString, Entry> cache;
    static QList<QString> order; // least recently used first
    static QElapsedTimer clock;

    if (path.isEmpty()) {
        if (error)
            *error = tr("no LUT file chosen");
        return nullptr;
    }
    QMutexLocker lock(&mutex);
    if (!clock.isValid())
        clock.start();
    const qint64 now = clock.elapsed();
    auto it = cache.find(path);
    if (it != cache.end() && now - it->checkedMs < kRecheckMs) {
        if (error && !it->lut)
            *error = it->error;
        return it->lut;
    }
    const media::MediaSource source = media::MediaSource::resolve(path);
    if (it != cache.end() && source.exists() && source.identity() == it->identity) {
        it->checkedMs = now;
        if (error && !it->lut)
            *error = it->error;
        return it->lut;
    }
    // (Re)load. The file is read under the lock: LUT files are small and rarely change.
    Entry entry;
    entry.identity = source.identity();
    entry.checkedMs = now;
    if (!source.exists()) {
        entry.error = tr("LUT file not found: %1").arg(path);
    } else if (source.size() > kMaxFileBytes) {
        entry.error = tr("LUT file too large: %1").arg(path);
    } else {
        QString why;
        const QByteArray bytes = source.readAll(&why, kMaxFileBytes);
        auto lut = std::make_shared<CubeLut>();
        if (bytes.isNull())
            entry.error = why;
        else if (!lut->parse(bytes, &why))
            entry.error = tr("%1: %2").arg(path, why);
        else
            entry.lut = std::move(lut);
    }
    order.removeAll(path);
    order.append(path);
    while (order.size() > kMaxCached)
        cache.remove(order.takeFirst());
    cache.insert(path, entry);
    if (error && !entry.lut)
        *error = entry.error;
    return entry.lut;
}

} // namespace kk::render
