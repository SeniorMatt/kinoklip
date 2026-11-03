// SPDX-License-Identifier: GPL-3.0-or-later
// CubeLut — 1D and 3D colour look-up tables from .cube files (the common text format:
// TITLE, LUT_1D_SIZE / LUT_3D_SIZE, DOMAIN_MIN / DOMAIN_MAX, then one "r g b" row per
// entry, red changing fastest), their CPU evaluation (the reference of the Apply LUT shader)
// and a small thread-safe cache of parsed files read through media::MediaSource (plain
// files and entries of single-file projects).
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <memory>

namespace kk::render {

/// A parsed .cube table. Immutable once loaded; shared between threads.
class CubeLut
{
public:
    /// Largest accepted LUT_3D_SIZE (129³ entries = 26 MB of floats) and LUT_1D_SIZE.
    static constexpr int kMax3dSize = 129;
    static constexpr int kMax1dSize = 65536;

    /// Parses .cube text. @return false with a reason in @p error (malformed line, missing
    /// size, wrong number of rows, size out of range).
    bool parse(const QByteArray& text, QString* error);

    [[nodiscard]] bool is3d() const { return m_size3d > 0; }
    /// Entries per axis (3D) or entries (1D).
    [[nodiscard]] int size() const { return m_size3d > 0 ? m_size3d : m_size1d; }
    /// Rows as r, g, b floats (3D: index r + N·g + N²·b).
    [[nodiscard]] const QVector<float>& data() const { return m_data; }
    [[nodiscard]] const float* domainMin() const { return m_min; }
    [[nodiscard]] const float* domainMax() const { return m_max; }
    [[nodiscard]] QString title() const { return m_title; }

    /// Looks up straight colour @p rgb (in/out, 3 floats): trilinear (3D) or per-channel
    /// linear (1D) interpolation; inputs outside the domain clamp to it.
    void apply(float* rgb) const;

private:
    QString m_title;
    int m_size1d = 0;
    int m_size3d = 0;
    float m_min[3] = {0.f, 0.f, 0.f};
    float m_max[3] = {1.f, 1.f, 1.f};
    QVector<float> m_data;
};

/// Loads (or returns the cached) LUT of @p path. The file is re-read when its identity
/// (time, size) changes; the identity is re-checked at most every few hundred milliseconds,
/// so calling this per frame costs a hash look-up. Thread-safe. @return nullptr with a reason
/// in @p error if the file cannot be read or parsed.
[[nodiscard]] std::shared_ptr<const CubeLut> loadCubeLut(
    const QString& path, QString* error = nullptr);

} // namespace kk::render
