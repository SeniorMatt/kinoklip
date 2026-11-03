// SPDX-License-Identifier: GPL-3.0-or-later
// CompTransport — the Compositing page's time ruler and transport over the open clip's frames.
#pragma once

#include <QPointer>
#include <QWidget>

#include <cstdint>

class QLabel;
class QToolButton;

namespace kk {

struct AppContext;
class CompSession;

/// Ruler over the comp frames (0 .. clip duration - 1) with the playhead and the keyframe
/// ticks of the current node; click or drag to seek.
///
/// Thread-safety: GUI thread only.
class CompTimeRuler : public QWidget
{
    Q_OBJECT
public:
    explicit CompTimeRuler(CompSession* session, QWidget* parent = nullptr);

    /// Widget x of comp frame @p frame.
    [[nodiscard]] double xForFrame(int64_t frame) const;
    /// Comp frame at widget x @p x (clamped).
    [[nodiscard]] int64_t frameAt(double x) const;
    /// Comp frames of the current node's keyframes (sorted, unique).
    [[nodiscard]] QVector<int64_t> keyFrames() const;
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    QPointer<CompSession> m_session;
};

/// Play / stop, the ruler, and the comp frame with the timeline timecode. Play runs the
/// program PlaybackEngine over the clip's range of the timeline and loops back to the clip's
/// first frame at its end (the composition's render range); stop pauses.
///
/// Thread-safety: GUI thread only.
class CompTransport : public QWidget
{
    Q_OBJECT
public:
    CompTransport(AppContext& ctx, CompSession* session, QWidget* parent = nullptr);

    [[nodiscard]] CompTimeRuler* ruler() const { return m_ruler; }
    /// "Frame 12 / 01:00:00:12" (tests).
    [[nodiscard]] QString positionText() const;
    /// Plays the clip range from the playhead (from the clip start if outside).
    void play();
    /// Pauses.
    void stop();
    /// True while the transport plays the clip range.
    [[nodiscard]] bool isPlaying() const { return m_playing; }

private:
    void updatePosition();
    void onEnginePosition(int64_t frame);

    AppContext& m_ctx;
    QPointer<CompSession> m_session;
    CompTimeRuler* m_ruler = nullptr;
    QToolButton* m_play = nullptr;
    QToolButton* m_stop = nullptr;
    QLabel* m_position = nullptr;
    bool m_playing = false;
};

} // namespace kk
