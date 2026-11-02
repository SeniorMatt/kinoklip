// SPDX-License-Identifier: GPL-3.0-or-later
// CutOverview — the upper half of the Cut page's dual timeline: the whole timeline at a glance,
// the playhead and the range the lower (zoomed) timeline shows.
#pragma once

#include <QWidget>

#include <cstdint>

namespace kk {

struct AppContext;

/// The overview timeline of the Cut page.
///
/// Draws every track of the active timeline compressed into a short strip (video tracks above,
/// compact audio below), always fitted to the width, so the whole edit is visible; a frame
/// band marks the range the lower timeline shows (setDetailRange()) and the playhead line
/// moves with playback. Clicking or dragging anywhere moves the playhead (scrubbing), which
/// the lower timeline follows. All colours come from kk::schemeColors().
///
/// Geometry is exposed (frameToX(), xToFrame()) so tests can check that both timelines stay in
/// sync. Thread-safety: GUI thread only.
class CutOverview : public QWidget
{
    Q_OBJECT
public:
    /// Creates the overview of the active timeline of @p ctx.project.
    explicit CutOverview(AppContext& ctx, QWidget* parent = nullptr);

    /// The frame range [@p first, @p last] the lower timeline shows (fractional frames).
    void setDetailRange(double first, double last);
    [[nodiscard]] double detailFirst() const { return m_detailFirst; }
    [[nodiscard]] double detailLast() const { return m_detailLast; }

    /// Frames shown across the width (timeline length plus a small margin, at least 10 s).
    [[nodiscard]] int64_t visibleFrames() const;
    /// x of timeline frame @p frame in widget coordinates.
    [[nodiscard]] double frameToX(double frame) const;
    /// Timeline frame at widget x @p x (clamped to >= 0).
    [[nodiscard]] int64_t xToFrame(double x) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void scrubTo(double x);

    AppContext& m_ctx;
    double m_detailFirst = 0.0;
    double m_detailLast = 0.0;
    bool m_scrubbing = false;
};

} // namespace kk
