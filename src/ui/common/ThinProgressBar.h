// SPDX-License-Identifier: GPL-3.0-or-later
// ThinProgressBar — a slim progress line drawn by us, not by the widget style.
#pragma once

#include <QProgressBar>

namespace kk {

/// A QProgressBar a few pixels high (the Render Queue job rows). Painted directly: some
/// styles draw an empty groove when the bar is shorter than their own minimum (owner report on
/// 0.4.0: the Render Queue showed no progress at 84 %). Same API as QProgressBar.
class ThinProgressBar : public QProgressBar
{
    Q_OBJECT
public:
    explicit ThinProgressBar(QWidget* parent = nullptr);

    /// Fraction 0..1 shown by the fill (value relative to the range).
    [[nodiscard]] double fraction() const;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace kk
