// SPDX-License-Identifier: GPL-3.0-or-later
// Stabilization (the Inspector's Video tab, spec section 58): a clip's shake is measured once
// (compositing/tracking/Stabilizer.h) and the measurement is stored with the clip as the camera
// path, one entry per SOURCE frame. Everything the user can still change afterwards (Smooth,
// Camera Lock, Strength, Zoom, Cropping Ratio) is worked out from that path when a frame is drawn,
// so trimming, slipping, changing the speed or reversing a clip keeps the right frames steady.
//
// Maths: the camera path P(t) maps the first analysed frame onto frame t (a move; a move, turn
// and scale; or a homography, by Mode). The smoothed path S(t) is P(t) low-pass filtered (or the
// identity for Camera Lock). A frame is corrected by M(t) = S(t) * P(t)^-1: it is moved to where
// the smooth camera would have seen the same scene. Strength scales M(t) in parameter space (0 =
// off, 1 = full, negative = the opposite way); Zoom is the smallest scale that keeps the picture
// covering its own frame in the worst corrected frame, never more than 1 + Cropping Ratio.
// Coordinates are "normalised picture coordinates": origin at the picture centre, y down, one unit
// = the picture width.
//
// Pure maths: no widgets, no threads (the lazy caches are mutex-guarded), no FFmpeg.
#pragma once

#include "core/Rational.h"

#include <QJsonObject>
#include <QSizeF>
#include <QString>
#include <QTransform>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace kk {

struct ClipData;

namespace stabilize {

/// What kind of camera movement is measured and corrected.
enum class Mode {
    Perspective, ///< full perspective (homography, 8 numbers per frame)
    Similarity, ///< pan, tilt, rotation and zoom (4 numbers per frame)
    Translation ///< pan and tilt only (2 numbers per frame)
};

[[nodiscard]] QString modeId(Mode mode); ///< "perspective", "similarity", "translation"
[[nodiscard]] Mode modeFromId(const QString& id, Mode fallback = Mode::Perspective);
/// Numbers stored per frame for @p mode.
[[nodiscard]] int paramCount(Mode mode);

/// EffectRegistry id of the Inspector section (its name, help key and catalogue entry).
inline constexpr char kDescriptorId[] = "builtin.stabilization";

inline constexpr double kMinCroppingRatio = 0.25;
inline constexpr double kMaxCroppingRatio = 1.0;
inline constexpr double kMinSmooth = 0.0;
inline constexpr double kMaxSmooth = 1.0;
inline constexpr double kMinStrength = -1.0;
inline constexpr double kMaxStrength = 1.0;
/// Smooth 1 filters the path with a Gaussian of this many seconds (sigma).
inline constexpr double kSmoothSeconds = 2.0;
/// Largest zoom the cover search looks for (Cropping Ratio 1 allows 1 + 1 = 2).
inline constexpr double kSearchZoom = 4.0;

/// A 3x3 matrix, row-major, acting on column vectors (x, y, 1).
struct Mat3
{
    std::array<double, 9> m {1, 0, 0, 0, 1, 0, 0, 0, 1};

    [[nodiscard]] Mat3 operator*(const Mat3& o) const;
    /// Singular input gives the identity and sets @p ok false (when given).
    [[nodiscard]] Mat3 inverse(bool* ok = nullptr) const;
    /// (x, y) through the matrix with the perspective division; w <= 0 gives the far point.
    void apply(double x, double y, double* ox, double* oy) const;
    [[nodiscard]] static Mat3 translation(double tx, double ty);
};

/// What the user chooses. Part of the clip; saved with it.
struct Settings
{
    bool enabled = false;
    Mode mode = Mode::Perspective;
    bool cameraLock
        = false; ///< remove all camera movement (a tripod); Smooth and Cropping Ratio idle
    bool zoom = false; ///< scale up to hide the empty edges
    double croppingRatio = 1.0; ///< 0.25 .. 1: how far Zoom may crop (up to 1 + ratio times)
    double smooth = 0.25; ///< 0 .. 1: how smooth the new camera path is
    double strength = 1.0; ///< -1 .. 1: 0 = off, negative = the opposite way

    friend bool operator==(const Settings&, const Settings&) = default;
};

/// Keeps every value of @p s in its range.
[[nodiscard]] Settings clamped(Settings s);

/// The measured camera path of a clip's source, one entry per source frame
/// [firstFrame, firstFrame + frames). Immutable once built; shared between copies of a clip
/// (undo steps, render snapshots).
class Data
{
public:
    /// @p path holds paramCount(mode) floats per frame: the camera path P(t) of Mode @p mode
    /// (the first entry is the identity). @p aspect is picture height / width.
    Data(Mode mode, Rational fps, int64_t firstFrame, double aspect, std::vector<float> path);
    Data(const Data&) = delete;
    Data& operator=(const Data&) = delete;

    [[nodiscard]] Mode mode() const { return m_mode; }
    /// Frame rate the frame numbers refer to (the source's).
    [[nodiscard]] Rational fps() const { return m_fps; }
    [[nodiscard]] int64_t firstFrame() const { return m_first; }
    [[nodiscard]] int64_t frames() const { return m_frames; }
    [[nodiscard]] int64_t lastFrame() const { return m_first + m_frames - 1; }
    [[nodiscard]] double aspect() const { return m_aspect; }
    [[nodiscard]] const std::vector<float>& path() const { return m_path; }
    [[nodiscard]] bool covers(int64_t sourceFrame) const
    {
        return sourceFrame >= m_first && sourceFrame < m_first + m_frames;
    }

    /// The correction M(t) of source frame @p sourceFrame for @p s (Smooth, Camera Lock and
    /// Strength applied; the identity outside the analysed frames).
    [[nodiscard]] Mat3 correction(const Settings& s, int64_t sourceFrame) const;
    /// The scale Zoom applies for @p s: 1 when Zoom is off, else the smallest scale that covers
    /// the picture in every analysed frame, at most 1 + Cropping Ratio.
    [[nodiscard]] double zoomFactor(const Settings& s) const;

    friend bool operator==(const Data& a, const Data& b);

private:
    using Corrections = std::vector<float>; ///< paramCount() numbers per frame: M(t) as parameters

    std::shared_ptr<const Corrections> corrections(double smooth, bool lock) const;
    [[nodiscard]] Mat3 matrixOf(const float* params, double strength) const;

    Mode m_mode;
    Rational m_fps;
    int64_t m_first = 0;
    int64_t m_frames = 0;
    double m_aspect = 9.0 / 16.0;
    std::vector<float> m_path;

    // Lazily computed, keyed by the settings that went into them (one entry each).
    mutable std::mutex m_mutex;
    mutable bool m_haveCorrections = false;
    mutable double m_corrSmooth = 0;
    mutable bool m_corrLock = false;
    mutable std::shared_ptr<const Corrections> m_corr;
    mutable bool m_haveZoom = false;
    mutable double m_zoomSmooth = 0;
    mutable bool m_zoomLock = false;
    mutable double m_zoomStrength = 0;
    mutable double m_zoomNeeded = 1;
};

/// Stabilization of one clip: the settings and, after Stabilize was pressed, the data.
struct ClipStabilization
{
    Settings settings;
    std::shared_ptr<const Data> data;

    /// True when the settings or the data differ from a clip that never had stabilization.
    [[nodiscard]] bool isDefault() const { return settings == Settings {} && !data; }
    friend bool operator==(const ClipStabilization& a, const ClipStabilization& b);
};

/// True if the clip is corrected when drawn: switched on, has data of the chosen Mode, and
/// Strength is not 0. A Mode changed since the analysis counts as "needs Stabilize again".
[[nodiscard]] bool isActive(const ClipStabilization& s);
/// True if the stored data was measured for a Mode other than the chosen one.
[[nodiscard]] bool needsAnalysis(const ClipStabilization& s);

/// Source frame number of media time @p timeUs at frame rate @p fps (the frame shown then).
[[nodiscard]] int64_t sourceFrameAt(int64_t timeUs, Rational fps);

/// The correction of @p clip at clip-local frame @p localFrame as a transform in the picture
/// coordinates of render::clipTransform() (timeline px, origin at the content centre), applied to
/// the clip's picture before Dynamic Zoom and the clip's own Transform. @p fps is the timeline's
/// frame rate (needed to find the source frame). Identity when the clip is not stabilized.
[[nodiscard]] QTransform transform(
    const ClipData& clip, int64_t localFrame, QSizeF frameSize, Rational fps);
/// Zoom factor Zoom applies to @p clip (1 when none); used to size layers.
[[nodiscard]] double zoomOf(const ClipData& clip);

// ---- Project file ----

/// {"enabled","mode","cameraLock","zoom","croppingRatio","smooth","strength", "data"?}; the path
/// is compressed float32 little-endian in base64.
[[nodiscard]] QJsonObject toJson(const ClipStabilization& s);
[[nodiscard]] ClipStabilization fromJson(const QJsonObject& o);

} // namespace stabilize
} // namespace kk
