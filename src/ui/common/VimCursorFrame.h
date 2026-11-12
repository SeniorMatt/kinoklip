// SPDX-License-Identifier: GPL-3.0-or-later
// VimCursorFrame — the frame the Vim layer draws round the row, strip or tile the keyboard cursor
// is on (spec section 83). A child of the widget it frames; the mouse goes through it.
#pragma once

#include <QPainter>
#include <QWidget>

namespace kk {

class VimCursorFrame final : public QWidget
{
public:
    explicit VimCursorFrame(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setObjectName(QStringLiteral("VimRowCursor"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QColor accent = palette().color(QPalette::Highlight);
        QColor fill = accent;
        fill.setAlpha(28);
        p.fillRect(rect(), fill);
        p.setPen(QPen(accent, 2));
        p.drawRect(rect().adjusted(1, 1, -1, -1));
    }
};

} // namespace kk
