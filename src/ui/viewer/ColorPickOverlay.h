// SPDX-License-Identifier: GPL-3.0-or-later
// ColorPickOverlay — the timeline viewer's eyedropper: while an Inspector colour parameter's
// picker button is checked, a click picks the colour under the mouse and a drag averages the
// dragged rectangle, written into that parameter as one undo step.
#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>

namespace kk {

struct AppContext;
struct TimelineData;
class ViewerSurface;

/// Transparent child of a timeline viewer's picture surface (an additive layer like
/// MaskOverlay). Active while SelectionModel::colorPicker() names an effect of a video clip
/// under the playhead of the current timeline: it then takes the mouse (crosshair cursor) and
/// is raised above the other overlays; otherwise it is hidden and transparent for the mouse.
///
/// What is sampled: the timeline frame at the playhead rendered with the target effect and
/// every effect after it on that clip switched off, so the eyedropper sees the clip's picture
/// as it enters the keyer (not the already keyed result). The colour is the mean of the
/// straight (un-premultiplied) pixels: a click averages 3 x 3 frame pixels, a drag the dragged
/// rectangle. Writing it: the parameter is set, and if the effect has a "screen" choice with a
/// "custom" value (Chroma Key) that is selected too, in one undo step "Pick Color". The
/// eyedropper then turns itself off (one pick per button press); Esc cancels it.
///
/// Thread-safety: GUI thread only. The pick renders synchronously (one frame).
class ColorPickOverlay : public QWidget
{
    Q_OBJECT
public:
    /// Renders timeline @p tl at @p frame as a premultiplied image (any size; it is mapped
    /// onto the whole frame). Default: the program engine's compositor at the timeline
    /// resolution with its media and proxy tables.
    using FrameSource
        = std::function<QImage(std::shared_ptr<const TimelineData> tl, int64_t frame)>;

    ColorPickOverlay(AppContext& ctx, ViewerSurface* surface);

    /// Replaces the frame renderer (tests; the default needs AppContext::playback).
    void setFrameSource(FrameSource source) { m_source = std::move(source); }
    /// True while the eyedropper is active (shown, takes the mouse).
    [[nodiscard]] bool isPicking() const { return m_active; }
    /// Re-evaluates whether the eyedropper is active and repaints.
    void refresh();

    /// Mean straight colour of the pixels of premultiplied @p image inside @p area (clipped to
    /// the image; transparent pixels ignored). Invalid colour if nothing is covered.
    [[nodiscard]] static QColor averageColor(const QImage& image, const QRect& area);

Q_SIGNALS:
    /// The user pressed on the overlay (the viewer activates itself).
    void pressed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    /// Picks over the widget rectangle spanned by @p a and @p b and writes the result.
    void pick(QPoint a, QPoint b);
    [[nodiscard]] QImage renderSource(int64_t frame) const;

    AppContext& m_ctx;
    ViewerSurface* m_surface;
    FrameSource m_source;
    bool m_active = false;
    bool m_dragging = false;
    QPoint m_pressPos;
    QPoint m_currentPos;
};

} // namespace kk
