// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/keyframes/VimKeyframeHost.h"

#include "ui/keyframes/CurveView.h"
#include "ui/keyframes/DopeSheetView.h"
#include "ui/keyframes/KeyframeParamTree.h"
#include "ui/keyframes/KeyframesPanel.h"

#include <QScrollBar>

#include <algorithm>

namespace kk {

VimKeyframeHost::VimKeyframeHost(ActionRegistry* actions)
    : VimPanelHost(actions)
{ }

KeyframeViewBase* VimKeyframeHost::view() const
{
    if (!m_panel)
        return nullptr;
    return m_panel->view() == KeyframesPanel::View::Curves
        ? static_cast<KeyframeViewBase*>(m_panel->curveView())
        : static_cast<KeyframeViewBase*>(m_panel->dopeSheet());
}

void VimKeyframeHost::focusChanged(QWidget* focus)
{
    for (QWidget* w = focus; w; w = w->parentWidget()) {
        if (auto* panel = qobject_cast<KeyframesPanel*>(w)) {
            m_panel = panel;
            break;
        }
    }
    if (KeyframeViewBase* v = view(); v && !v->hasCursor())
        v->moveCursorToRow(0);
}

void VimKeyframeHost::setCursorVisible(bool visible)
{
    if (visible || !m_panel)
        return;
    m_panel->dopeSheet()->clearCursor();
    m_panel->curveView()->clearCursor();
}

/// Scrolls the parameter tree (and with it the dope sheet) so the cursor row is on screen.
void VimKeyframeHost::reveal()
{
    KeyframeViewBase* v = view();
    if (!v || !v->hasCursor() || m_panel->view() != KeyframesPanel::View::Keyframes)
        return;
    QScrollBar* bar = m_panel->tree()->verticalScrollBar();
    const int top = v->cursorRow() * KeyframeParamTree::kRowHeight;
    const int page = m_panel->tree()->viewport()->height();
    if (top < bar->value())
        bar->setValue(top);
    else if (top + KeyframeParamTree::kRowHeight > bar->value() + page)
        bar->setValue(top + KeyframeParamTree::kRowHeight - page);
}

bool VimKeyframeHost::hostCommand(const QString& name, int count, bool hasCount)
{
    if (!name.startsWith(QLatin1String("kf_")))
        return false;
    KeyframeViewBase* v = view();
    if (!v)
        return true;
    const QString cmd = name.mid(3);
    const int n = std::max(1, count);
    bool ok = true;
    if (cmd == QLatin1String("prev")) {
        ok = v->moveCursorKeys(-n);
    } else if (cmd == QLatin1String("next")) {
        ok = v->moveCursorKeys(n);
    } else if (cmd == QLatin1String("row_down")) {
        ok = v->moveCursorRows(n);
    } else if (cmd == QLatin1String("row_up")) {
        ok = v->moveCursorRows(-n);
    } else if (cmd == QLatin1String("first")) {
        ok = v->moveCursorToEdgeKey(false);
    } else if (cmd == QLatin1String("last")) {
        ok = v->moveCursorToEdgeKey(true);
    } else if (cmd == QLatin1String("row_first")) {
        ok = v->moveCursorToRow(hasCount ? count - 1 : 0);
    } else if (cmd == QLatin1String("row_last")) {
        ok = v->moveCursorToRow(hasCount ? count - 1 : 1 << 20);
    } else if (cmd == QLatin1String("select_toggle")) {
        ok = v->selectCursor(true);
    } else if (cmd == QLatin1String("select_row")) {
        ok = v->selectCursorRow();
    } else if (cmd == QLatin1String("select_none")) {
        v->unselectKeys();
    } else if (cmd == QLatin1String("delete")) {
        ok = v->deleteTargets();
    } else if (cmd == QLatin1String("copy")) {
        ok = v->copyTargets();
        if (ok)
            showMessage(QObject::tr("Keyframes copied"));
    } else if (cmd == QLatin1String("paste")) {
        ok = v->pasteAtPlayhead();
    } else if (cmd == QLatin1String("add")) {
        ok = v->addKeyAtPlayhead();
    } else if (cmd == QLatin1String("inc") || cmd == QLatin1String("dec")) {
        const int steps = cmd == QLatin1String("inc") ? n : -n;
        ok = m_panel->view() == KeyframesPanel::View::Curves ? v->nudgeValue(steps)
                                                             : v->nudgeTime(steps);
    } else if (cmd == QLatin1String("later") || cmd == QLatin1String("earlier")) {
        ok = v->nudgeTime(cmd == QLatin1String("later") ? n : -n);
    } else if (cmd == QLatin1String("jump")) {
        ok = v->jumpToCursor();
    } else if (cmd == QLatin1String("toggle_view")) {
        m_panel->setView(m_panel->view() == KeyframesPanel::View::Curves
                ? KeyframesPanel::View::Keyframes
                : KeyframesPanel::View::Curves);
        if (KeyframeViewBase* now = view()) {
            now->setFocus(Qt::OtherFocusReason);
            if (!now->hasCursor())
                now->moveCursorToRow(0);
        }
    } else if (cmd == QLatin1String("fit")) {
        m_panel->setViewMode(KeyframesPanel::ViewMode::FitClip);
        m_panel->curveView()->fitView();
    } else if (cmd.startsWith(QLatin1String("interp_"))) {
        static const QList<std::pair<QString, Interp>> presets {
            {QStringLiteral("linear"), Interp::Linear},
            {QStringLiteral("ease_in"), Interp::EaseIn},
            {QStringLiteral("ease_out"), Interp::EaseOut},
            {QStringLiteral("ease_in_out"), Interp::EaseInOut},
            {QStringLiteral("smooth"), Interp::Smooth},
            {QStringLiteral("hold"), Interp::Hold},
        };
        ok = false;
        for (const auto& [id, interp] : presets) {
            if (cmd.mid(7) == id)
                ok = v->interpTargets(interp);
        }
    } else {
        return false;
    }
    if (!ok)
        showMessage(QObject::tr("Nothing there"));
    reveal();
    return true;
}

} // namespace kk
