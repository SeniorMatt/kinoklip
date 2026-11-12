// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimWindowNav.h"

#include "ui/common/PanelFrame.h"
#include "ui/common/VimInput.h"

#include <QAbstractItemView>
#include <QRect>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace kk::vimwin {

namespace {

QRect globalRect(const QWidget* w)
{
    return QRect(w->mapToGlobal(QPoint(0, 0)), w->size());
}

int overlap(int a0, int a1, int b0, int b1)
{
    return std::max(0, std::min(a1, b1) - std::max(a0, b0));
}

} // namespace

PanelFrame* frameOf(QWidget* w)
{
    for (; w; w = w->parentWidget()) {
        if (auto* frame = qobject_cast<PanelFrame*>(w))
            return frame;
    }
    return nullptr;
}

QVector<PanelFrame*> visiblePanels(QWidget* window)
{
    QVector<PanelFrame*> panels;
    if (!window)
        return panels;
    for (PanelFrame* frame : window->findChildren<PanelFrame*>()) {
        if (frame->isVisible() && frame->window() == window->window() && !frame->size().isEmpty())
            panels.append(frame);
    }
    std::sort(panels.begin(), panels.end(), [](PanelFrame* a, PanelFrame* b) {
        const QRect ra = globalRect(a);
        const QRect rb = globalRect(b);
        if (std::abs(ra.top() - rb.top()) > 8)
            return ra.top() < rb.top();
        return ra.left() < rb.left();
    });
    return panels;
}

PanelFrame* neighbour(PanelFrame* from, Dir dir, const QVector<PanelFrame*>& panels)
{
    if (!from)
        return nullptr;
    const QRect f = globalRect(from);
    PanelFrame* best = nullptr;
    long long bestKey = 0;
    for (int pass = 0; pass < 2 && !best; ++pass) {
        for (PanelFrame* c : panels) {
            if (c == from)
                continue;
            const QRect r = globalRect(c);
            int gap = 0; // distance across the edge facing @p from
            int across = 0; // how much the two overlap along the other axis
            switch (dir) {
            case Dir::Left:
                gap = f.left() - r.right() - 1;
                across = overlap(f.top(), f.bottom(), r.top(), r.bottom());
                break;
            case Dir::Right:
                gap = r.left() - f.right() - 1;
                across = overlap(f.top(), f.bottom(), r.top(), r.bottom());
                break;
            case Dir::Up:
                gap = f.top() - r.bottom() - 1;
                across = overlap(f.left(), f.right(), r.left(), r.right());
                break;
            case Dir::Down:
                gap = r.top() - f.bottom() - 1;
                across = overlap(f.left(), f.right(), r.left(), r.right());
                break;
            }
            if (gap < -4)
                continue; // not on that side
            if (pass == 0 && across <= 0)
                continue; // first look only at panels that face this one
            // Nearest first; a bigger overlap wins a tie; in the second pass the distance of
            // the centres decides.
            long long key;
            if (pass == 0) {
                key = (long long)std::max(0, gap) * 100000 - across;
            } else {
                const QPoint d = r.center() - f.center();
                key = (long long)d.x() * d.x() + (long long)d.y() * d.y();
            }
            if (!best || key < bestKey) {
                best = c;
                bestKey = key;
            }
        }
    }
    return best;
}

PanelFrame* step(PanelFrame* from, int delta, const QVector<PanelFrame*>& panels)
{
    if (panels.isEmpty())
        return nullptr;
    const int n = int(panels.size());
    const int at = int(panels.indexOf(from));
    if (at < 0)
        return delta >= 0 ? panels.first() : panels.last();
    return panels[((at + delta) % n + n) % n];
}

QWidget* mainViewOf(PanelFrame* frame)
{
    QWidget* best = nullptr;
    long long bestArea = 0;
    for (auto* view : frame->findChildren<QAbstractItemView*>()) {
        if (!view->isVisible() || view->focusPolicy() == Qt::NoFocus)
            continue;
        // Not the editors inside a view or a combo's popup list.
        if (view->parentWidget() && view->parentWidget()->inherits("QComboBoxPrivateContainer"))
            continue;
        const long long area = (long long)view->width() * view->height();
        if (area > bestArea) {
            best = view;
            bestArea = area;
        }
    }
    return best;
}

bool focusPanel(PanelFrame* frame)
{
    if (!frame)
        return false;
    // A panel of rows (the Inspector) takes the focus itself, whatever lists it holds.
    if (frame->property(VimInput::kScopeProperty).toString() == QLatin1String("form")
        && frame->focusPolicy() != Qt::NoFocus) {
        frame->setFocus(Qt::OtherFocusReason);
        return true;
    }
    for (QWidget* child : frame->findChildren<QWidget*>()) {
        if (!child->property(VimInput::kScopeProperty).toString().isEmpty() && child->isVisible()
            && child->focusPolicy() != Qt::NoFocus) {
            child->setFocus(Qt::OtherFocusReason);
            return true;
        }
    }
    if (QWidget* view = mainViewOf(frame)) {
        view->setFocus(Qt::OtherFocusReason);
        return true;
    }
    // A panel of fields and buttons: the panel itself takes the focus (its keys: window moves).
    if (frame->focusPolicy() == Qt::NoFocus)
        frame->setFocusPolicy(Qt::StrongFocus);
    frame->setFocus(Qt::OtherFocusReason);
    return true;
}

} // namespace kk::vimwin
