// SPDX-License-Identifier: GPL-3.0-or-later
// RenderRangeBar — the Deliver page's mini timeline: the whole timeline squeezed into one strip
// (one lane per track, clips as blocks), the render range highlighted, the playhead; click or
// drag to move the playhead (the viewer previews that frame).
#pragma once

#include <QWidget>

#include <cstdint>

namespace kk {

class Timeline;

/// Compact read-only timeline overview with a draggable playhead.
class RenderRangeBar : public QWidget
{
    Q_OBJECT
public:
    /// @p timeline must outlive the widget.
    explicit RenderRangeBar(Timeline* timeline, QWidget* parent = nullptr);

    /// Highlights [first, end) as the render range (end <= first: none).
    void setRange(int64_t first, int64_t end);

    [[nodiscard]] QSize sizeHint() const override { return {600, 72}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {120, 40}; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] int64_t visibleFrames() const;
    [[nodiscard]] double frameToX(int64_t frame) const;
    void seekTo(int x);

    Timeline* m_timeline;
    int64_t m_first = 0;
    int64_t m_end = 0;
};

} // namespace kk
