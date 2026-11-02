// SPDX-License-Identifier: GPL-3.0-or-later
// MenuMetricsStyle — a thin proxy over the widget style that fixes menu item metrics.
#pragma once

#include <QProxyStyle>

namespace kk {

/// Proxy around the real widget style (Breeze, Fusion, ...) that reserves room for the
/// submenu arrow in menu items. With an application style sheet active (our panel chrome),
/// Breeze sized submenu items without the arrow column, so the arrow was drawn over the end
/// of long titles ("Color Theme" in the Settings menu, owner feedback on alpha.1). Drawing is
/// left entirely to the base style.
///
/// Thread-safety: GUI thread only (like every QStyle).
class MenuMetricsStyle : public QProxyStyle
{
    Q_OBJECT
public:
    /// Wraps @p base (takes ownership, as QProxyStyle does).
    explicit MenuMetricsStyle(QStyle* base);

    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
        const QWidget* widget) const override;
};

} // namespace kk
