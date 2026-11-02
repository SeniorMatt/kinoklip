// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeRuler — the time ruler of the Keyframes panel: timecode ticks on the panel's time
// map, the shown clips' spans and the playhead; pressing or dragging scrubs the playhead.
#pragma once

#include <QVector>
#include <QWidget>

namespace kk {

struct AppContext;
class KeyframeTimeMap;

/// Ruler above the panel's dope sheet and curves (same x origin: labelWidth() column first).
/// Scrubbing moves the timeline playhead, so the timeline, the viewers and the inspector
/// follow (time is shared with the timeline).
/// Thread-safety: GUI thread only.
class KeyframeRuler : public QWidget
{
    Q_OBJECT
public:
    KeyframeRuler(AppContext& ctx, const KeyframeTimeMap& map, QWidget* parent = nullptr);

    /// Width of the left column (aligned with the views below).
    void setLabelWidth(int px);
    /// Clips whose spans are marked.
    void setClips(const QVector<quint64>& clipIds);
    /// True while the user drags in the ruler.
    [[nodiscard]] bool isScrubbing() const { return m_scrubbing; }

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void scrubTo(double x);

    AppContext& m_ctx;
    const KeyframeTimeMap& m_map;
    QVector<quint64> m_clips;
    int m_labelWidth = 0;
    bool m_scrubbing = false;
};

} // namespace kk
