// SPDX-License-Identifier: GPL-3.0-or-later
// EffectTrack — the stored result of a tracking effect (Match Move, Surface Tracker; spec 70).
// A track is a table of values (a position, the corners of a region, the points of a mesh)
// keyed by the time of the clip's SOURCE in seconds, so trimming, splitting, slipping or changing
// the speed of the clip keeps every frame's own value, like Stabilization (spec 58.9). The table
// is written into a hidden static String parameter of the effect ("kkt1;..."), so it is saved,
// copied, split and undone with the effect and needs no change of the project format.
// Pure maths and text: no widgets, no threads (the parse cache is mutex-guarded).
#pragma once

#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace kk::efftrack {

/// Largest table accepted from a project file (keys).
inline constexpr int64_t kMaxKeys = 400000;
/// Largest number of floats per key accepted (a mesh of 9 x 9 points has 162).
inline constexpr int kMaxStride = 256;

/// Immutable table of values at keys.
class Table
{
public:
    /// @p seconds ascending, @p values holds @p stride floats per key. @p signature names what
    /// was tracked (see signatureMatches()); @p reference is the key (seconds) at which the
    /// tracked thing sat where the user put it.
    Table(QString signature, double reference, int stride, std::vector<double> seconds,
        std::vector<float> values);

    [[nodiscard]] const QString& signature() const { return m_signature; }
    [[nodiscard]] double reference() const { return m_reference; }
    [[nodiscard]] int stride() const { return m_stride; }
    [[nodiscard]] int64_t keys() const { return int64_t(m_seconds.size()); }
    [[nodiscard]] const std::vector<double>& seconds() const { return m_seconds; }
    [[nodiscard]] const std::vector<float>& values() const { return m_values; }
    [[nodiscard]] double firstSecond() const { return m_seconds.front(); }
    [[nodiscard]] double lastSecond() const { return m_seconds.back(); }

    /// The values at @p second, linear between neighbouring keys and held at the ends, written
    /// to @p out (stride floats).
    void at(double second, float* out) const;
    friend bool operator==(const Table& a, const Table& b);

private:
    QString m_signature;
    double m_reference = 0;
    int m_stride = 0;
    std::vector<double> m_seconds;
    std::vector<float> m_values;
};

/// A new table holding the keys of @p a and @p b (@p b wins at equal seconds). Both must have
/// the same signature, reference and stride, else @p b alone is returned.
[[nodiscard]] std::shared_ptr<const Table> merged(const Table* a, const Table& b);

/// The text form stored in the effect's parameter.
[[nodiscard]] QString encode(const Table& table);
/// The table of @p text, or null when it is empty or damaged. Parsed tables are cached.
[[nodiscard]] std::shared_ptr<const Table> decode(const QString& text);

} // namespace kk::efftrack
