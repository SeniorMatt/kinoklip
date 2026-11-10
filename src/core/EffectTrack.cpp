// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/EffectTrack.h"

#include <QByteArray>
#include <QHash>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace kk::efftrack {

namespace {

constexpr char kMagic[] = "kkt1";

void putLittle(QByteArray& out, const void* data, size_t bytes)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i)
        out.append(char(p[i])); // little-endian hosts only differ in order; stored as host order
}

} // namespace

Table::Table(QString signature, double reference, int stride, std::vector<double> seconds,
    std::vector<float> values)
    : m_signature(std::move(signature))
    , m_reference(reference)
    , m_stride(stride)
    , m_seconds(std::move(seconds))
    , m_values(std::move(values))
{
}

bool operator==(const Table& a, const Table& b)
{
    return a.m_signature == b.m_signature && a.m_reference == b.m_reference
        && a.m_stride == b.m_stride && a.m_seconds == b.m_seconds && a.m_values == b.m_values;
}

void Table::at(double second, float* out) const
{
    const size_t n = m_seconds.size();
    const size_t stride = size_t(m_stride);
    if (n == 0) {
        std::fill(out, out + stride, 0.f);
        return;
    }
    auto copy = [&](size_t i) { std::copy_n(m_values.begin() + i * stride, stride, out); };
    if (second <= m_seconds.front() || n == 1)
        return copy(0);
    if (second >= m_seconds.back())
        return copy(n - 1);
    const size_t hi = size_t(std::upper_bound(m_seconds.begin(), m_seconds.end(), second)
        - m_seconds.begin());
    const size_t lo = hi - 1;
    const double span = m_seconds[hi] - m_seconds[lo];
    const float t = span > 0 ? float((second - m_seconds[lo]) / span) : 0.f;
    for (size_t i = 0; i < stride; ++i)
        out[i] = m_values[lo * stride + i] + (m_values[hi * stride + i] - m_values[lo * stride + i]) * t;
}

std::shared_ptr<const Table> merged(const Table* a, const Table& b)
{
    if (!a || a->signature() != b.signature() || a->reference() != b.reference()
        || a->stride() != b.stride())
        return std::make_shared<Table>(b);
    const size_t stride = size_t(b.stride());
    std::vector<double> seconds;
    std::vector<float> values;
    size_t i = 0, j = 0;
    while (i < a->seconds().size() || j < b.seconds().size()) {
        const bool takeA = j >= b.seconds().size()
            || (i < a->seconds().size() && a->seconds()[i] < b.seconds()[j] - 1e-9);
        const bool same = i < a->seconds().size() && j < b.seconds().size()
            && std::abs(a->seconds()[i] - b.seconds()[j]) <= 1e-9;
        if (same) {
            ++i; // b wins
            continue;
        }
        if (takeA) {
            seconds.push_back(a->seconds()[i]);
            values.insert(values.end(), a->values().begin() + i * stride,
                a->values().begin() + (i + 1) * stride);
            ++i;
        } else {
            seconds.push_back(b.seconds()[j]);
            values.insert(values.end(), b.values().begin() + j * stride,
                b.values().begin() + (j + 1) * stride);
            ++j;
        }
    }
    return std::make_shared<Table>(
        b.signature(), b.reference(), b.stride(), std::move(seconds), std::move(values));
}

QString encode(const Table& t)
{
    QByteArray bytes;
    bytes.reserve(qsizetype(t.seconds().size() * 8 + t.values().size() * 4));
    for (const double s : t.seconds())
        putLittle(bytes, &s, 8);
    for (const float v : t.values())
        putLittle(bytes, &v, 4);
    return QStringLiteral("%1;%2;%3;%4;%5;%6")
        .arg(QLatin1String(kMagic))
        .arg(QString::fromLatin1(t.signature().toUtf8().toBase64()))
        .arg(t.reference(), 0, 'g', 17)
        .arg(t.stride())
        .arg(t.keys())
        .arg(QString::fromLatin1(qCompress(bytes, 6).toBase64()));
}

std::shared_ptr<const Table> decode(const QString& text)
{
    if (text.isEmpty())
        return nullptr;
    static std::mutex mutex;
    static QHash<QString, std::shared_ptr<const Table>> cache;
    {
        const std::lock_guard lock(mutex);
        const auto it = cache.constFind(text);
        if (it != cache.cend())
            return it.value();
    }
    std::shared_ptr<const Table> result;
    const QStringList parts = text.split(QLatin1Char(';'));
    if (parts.size() == 6 && parts[0] == QLatin1String(kMagic)) {
        bool okRef = false, okStride = false, okKeys = false;
        const double reference = parts[2].toDouble(&okRef);
        const int stride = parts[3].toInt(&okStride);
        const qlonglong keys = parts[4].toLongLong(&okKeys);
        if (okRef && okStride && okKeys && std::isfinite(reference) && stride > 0
            && stride <= kMaxStride && keys > 0 && keys <= kMaxKeys) {
            const QByteArray bytes
                = qUncompress(QByteArray::fromBase64(parts[5].toLatin1()));
            const qlonglong expected = keys * 8 + keys * stride * 4;
            if (bytes.size() == expected) {
                std::vector<double> seconds(static_cast<size_t>(keys));
                std::vector<float> values(static_cast<size_t>(keys) * static_cast<size_t>(stride));
                std::memcpy(seconds.data(), bytes.constData(), size_t(keys) * 8);
                std::memcpy(values.data(), bytes.constData() + keys * 8, values.size() * 4);
                const bool finite = std::all_of(seconds.begin(), seconds.end(),
                                        [](double s) { return std::isfinite(s); })
                    && std::all_of(values.begin(), values.end(),
                        [](float v) { return std::isfinite(v); })
                    && std::is_sorted(seconds.begin(), seconds.end());
                if (finite)
                    result = std::make_shared<Table>(
                        QString::fromUtf8(QByteArray::fromBase64(parts[1].toLatin1())),
                        reference, stride, std::move(seconds), std::move(values));
            }
        }
    }
    const std::lock_guard lock(mutex);
    if (cache.size() > 16)
        cache.clear();
    cache.insert(text, result);
    return result;
}

} // namespace kk::efftrack
