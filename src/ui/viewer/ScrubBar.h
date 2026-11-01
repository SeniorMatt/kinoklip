// SPDX-License-Identifier: GPL-3.0-or-later
// ScrubBar — the viewer's jog/scrub strip: duration, in/out range, playhead; click or drag to
// move the playhead.
#pragma once

#include <QWidget>

#include <cstdint>

namespace kk {

/// Thin horizontal bar under the viewer picture.
///
/// Draws the whole duration as a track, the marked in/out range as a tinted band with
/// brackets at its ends, and the playhead as a vertical line with a small head. Pressing and
/// dragging emits scrubbed() with the frame under the pointer (the engine coalesces renders, so
/// emitting on every mouse move is fine); the wheel steps one frame per notch.
///
/// Thread-safety: GUI thread only.
class ScrubBar : public QWidget
{
    Q_OBJECT
public:
    explicit ScrubBar(QWidget* parent = nullptr);

    /// Number of frames represented (0 = empty bar).
    void setDuration(int64_t frames);
    [[nodiscard]] int64_t duration() const { return m_duration; }
    /// Playhead frame.
    void setPosition(int64_t frame);
    [[nodiscard]] int64_t position() const { return m_position; }
    /// Marks (-1 = unset; out exclusive).
    void setMarks(int64_t in, int64_t out);
    [[nodiscard]] int64_t markIn() const { return m_in; }
    [[nodiscard]] int64_t markOut() const { return m_out; }
    /// Frame at widget x coordinate @p x (clamped to [0, duration - 1]).
    [[nodiscard]] int64_t frameAt(int x) const;
    /// Widget x coordinate of the left edge of @p frame.
    [[nodiscard]] double xOf(int64_t frame) const;

    [[nodiscard]] QSize sizeHint() const override;

Q_SIGNALS:
    /// The user moved the playhead to @p frame.
    void scrubbed(int64_t frame);
    /// Scrubbing started / ended (mouse press / release).
    void scrubStarted();
    void scrubFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    [[nodiscard]] QRect trackRect() const;

    int64_t m_duration = 0;
    int64_t m_position = 0;
    int64_t m_in = -1;
    int64_t m_out = -1;
    bool m_dragging = false;
};

} // namespace kk
