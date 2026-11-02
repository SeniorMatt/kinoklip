// SPDX-License-Identifier: GPL-3.0-or-later
// SourceTapeBar — the clip-boundary scroll bar under the Cut page viewer in Source Tape mode.
#pragma once

#include "core/SourceTape.h"

#include <QWidget>

#include <cstdint>

namespace kk {

/// A thin bar showing the source tape's clips as blocks (names where they fit), the in / out
/// band and the tape position. Clicking or dragging moves the tape position (positionRequested
/// is emitted; the page seeks the tape player), so the whole bin can be skimmed quickly.
/// Colours come from kk::schemeColors(). Thread-safety: GUI thread only.
class SourceTapeBar : public QWidget
{
    Q_OBJECT
public:
    /// Creates an empty bar.
    explicit SourceTapeBar(QWidget* parent = nullptr);

    /// Shows @p tape (copied).
    void setTape(const SourceTape& tape);
    [[nodiscard]] const SourceTape& tape() const { return m_tape; }
    /// The tape position in tape frames.
    void setPosition(int64_t frame);
    /// Marked range in tape frames (-1 = unset).
    void setMarks(int64_t in, int64_t out);
    /// Tape frame at widget x @p x (clamped into the tape).
    [[nodiscard]] int64_t frameAtX(double x) const;
    /// x of tape frame @p frame.
    [[nodiscard]] double xOfFrame(double frame) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    /// The user clicked or dragged to tape frame @p frame.
    void positionRequested(int64_t frame);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    SourceTape m_tape;
    int64_t m_position = 0;
    int64_t m_in = -1;
    int64_t m_out = -1;
};

} // namespace kk
