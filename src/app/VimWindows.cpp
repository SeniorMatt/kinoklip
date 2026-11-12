// SPDX-License-Identifier: GPL-3.0-or-later
// Panels, windows, pages and lists in Vim mode (spec section 83): <C-h/j/k/l> and <C-w> move the
// focus between the panels by their place on the screen, <leader>w{x} focuses a panel by name,
// gt / gT change the page, and the list keys (`a r h l -`, folds) of the generic list host.
// Neovim's windows.txt and tabpage.txt are the model.
#include "app/MainWindow.h"
#include "app/PageBar.h"
#include "app/VimController.h"
#include "app/VimEx.h"
#include "app/VimModeLine.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelFrame.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimListHost.h"
#include "ui/common/VimWindowNav.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QTimer>
#include <QTreeView>

#include <algorithm>

namespace kk {

namespace {

constexpr int kMessageMs = 4000;

/// A panel's key from one of the names `:panel` and <leader>w use (or a key itself).
QStringList keysFor(const QString& name)
{
    if (name == QLatin1String("media"))
        return {QStringLiteral("media_pool")};
    if (name == QLatin1String("index"))
        return {QStringLiteral("edit_index")};
    if (name == QLatin1String("nodes"))
        return {
            QStringLiteral("nodes"), QStringLiteral("color_nodes"), QStringLiteral("comp_nodes")};
    if (name == QLatin1String("gallery"))
        return {QStringLiteral("color_gallery")};
    if (name == QLatin1String("scopes"))
        return {QStringLiteral("color_scopes")};
    if (name == QLatin1String("queue"))
        return {QStringLiteral("render_queue")};
    return {name};
}

/// The action that shows or hides a panel (the interface bar's buttons).
const char* toggleAction(const QString& key)
{
    if (key == QLatin1String("media_pool"))
        return "view.toggle_media_pool";
    if (key == QLatin1String("effects"))
        return "view.toggle_effects";
    if (key == QLatin1String("edit_index"))
        return "view.toggle_edit_index";
    if (key == QLatin1String("keyframes"))
        return "view.toggle_keyframes";
    if (key == QLatin1String("inspector"))
        return "view.toggle_inspector";
    if (key == QLatin1String("mixer"))
        return "view.toggle_mixer";
    if (key == QLatin1String("metadata"))
        return "view.toggle_metadata";
    if (key == QLatin1String("nodes") || key == QLatin1String("color_nodes"))
        return "view.toggle_nodes";
    if (key == QLatin1String("color_gallery"))
        return "view.toggle_gallery";
    if (key == QLatin1String("color_scopes"))
        return "view.toggle_scopes";
    return nullptr;
}

} // namespace

PanelFrame* VimController::currentFrame() const
{
    if (PanelFrame* frame = vimwin::frameOf(QApplication::focusWidget()))
        return frame;
    return m_framed.data();
}

PanelFrame* VimController::findPanel(const QString& name, bool show)
{
    const QStringList keys = keysFor(name);
    const auto find = [&]() -> PanelFrame* {
        PanelFrame* found = nullptr;
        for (PanelFrame* frame : vimwin::visiblePanels(m_window)) {
            if (!keys.contains(frame->panelKey()))
                continue;
            // Two viewers: the Timeline Viewer is meant by "viewer".
            if (!found || frame->title().contains(QStringLiteral("Timeline"), Qt::CaseInsensitive))
                found = frame;
        }
        return found;
    };
    if (PanelFrame* frame = find())
        return frame;
    if (!show)
        return nullptr;
    for (const QString& key : keys) {
        const char* id = toggleAction(key);
        if (!id || !m_ctx.actions || !m_ctx.actions->contains(QLatin1String(id)))
            continue;
        QAction* toggle = m_ctx.actions->action(QLatin1String(id));
        if (toggle->isEnabled() && !toggle->isChecked()) {
            toggle->trigger();
            // The panel got its place from the splitters in the layout pass that follows.
            QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            if (QLayout* layout = m_window->layout())
                layout->activate();
            if (PanelFrame* frame = find())
                return frame;
        }
    }
    return nullptr;
}

bool VimController::focusPanelKey(const QString& name)
{
    // A zoomed layout may have hidden it.
    PanelFrame* frame = findPanel(name, false);
    if (!frame && !m_zoomHidden.isEmpty())
        restoreZoom();
    if (!frame)
        frame = findPanel(name, true);
    if (!frame)
        return false;
    return vimwin::focusPanel(frame);
}

void VimController::navigate(int direction)
{
    const QVector<PanelFrame*> panels = vimwin::visiblePanels(m_window);
    if (panels.isEmpty())
        return;
    PanelFrame* from = currentFrame();
    if (!from) {
        vimwin::focusPanel(panels.first());
        return;
    }
    // 0 left, 1 down, 2 up, 3 right (the order of VimEx and the key table).
    static const vimwin::Dir dirs[]
        = {vimwin::Dir::Left, vimwin::Dir::Down, vimwin::Dir::Up, vimwin::Dir::Right};
    if (PanelFrame* to = vimwin::neighbour(from, dirs[direction], panels))
        vimwin::focusPanel(to);
}

void VimController::restoreZoom()
{
    for (const QPointer<PanelFrame>& frame : std::as_const(m_zoomHidden)) {
        if (frame)
            frame->setVisible(true);
    }
    m_zoomHidden.clear();
}

void VimController::zoomToggle()
{
    if (!m_zoomHidden.isEmpty()) {
        restoreZoom();
        return;
    }
    PanelFrame* keep = currentFrame();
    if (!keep)
        return;
    for (PanelFrame* frame : vimwin::visiblePanels(m_window)) {
        if (frame != keep) {
            m_zoomHidden.append(frame);
            frame->setVisible(false);
        }
    }
    vimwin::focusPanel(keep);
    m_modeLine->showMessage(tr("Zoomed: <C-w>o again to bring the other panels back"), kMessageMs);
}

void VimController::afterPageChange()
{
    restoreZoom();
    // The old focus is on a page that is hidden now: land on the same kind of panel here, else
    // the first one.
    const QString key = m_framed ? m_framed->panelKey() : QString();
    QTimer::singleShot(0, this, [this, key] {
        const QWidget* focus = QApplication::focusWidget();
        if (focus && focus->isVisible() && vimwin::frameOf(const_cast<QWidget*>(focus)))
            return;
        const QVector<PanelFrame*> panels = vimwin::visiblePanels(m_window);
        PanelFrame* target = nullptr;
        for (PanelFrame* frame : panels) {
            if (!key.isEmpty() && frame->panelKey() == key)
                target = frame;
        }
        if (!target && !panels.isEmpty())
            target = panels.first();
        if (target)
            vimwin::focusPanel(target);
    });
}

bool VimController::windowCommand(const QString& name, int count, bool hasCount, const QString& arg)
{
    if (name == QLatin1String("win_left"))
        navigate(0);
    else if (name == QLatin1String("win_down"))
        navigate(1);
    else if (name == QLatin1String("win_up"))
        navigate(2);
    else if (name == QLatin1String("win_right"))
        navigate(3);
    else if (name == QLatin1String("win_next") || name == QLatin1String("win_prev")) {
        const QVector<PanelFrame*> panels = vimwin::visiblePanels(m_window);
        if (panels.isEmpty())
            return true;
        PanelFrame* target = nullptr;
        if (hasCount) // the Nth panel in reading order, the last one for a number too big
            target = panels[std::clamp(count, 1, int(panels.size())) - 1];
        else
            target
                = vimwin::step(currentFrame(), name == QLatin1String("win_next") ? 1 : -1, panels);
        vimwin::focusPanel(target);
    } else if (name == QLatin1String("win_last")) {
        if (m_previousFrame && m_previousFrame->isVisible())
            vimwin::focusPanel(m_previousFrame);
    } else if (name == QLatin1String("win_only")) {
        zoomToggle();
    } else if (name == QLatin1String("win_reset")) {
        restoreZoom();
        if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("view.reset_ui_layout")))
            m_ctx.actions->action(QStringLiteral("view.reset_ui_layout"))->trigger();
    } else if (name == QLatin1String("win_close")) {
        PanelFrame* frame = currentFrame();
        const char* id = frame ? toggleAction(frame->panelKey()) : nullptr;
        if (!id || !m_ctx.actions || !m_ctx.actions->contains(QLatin1String(id))) {
            m_modeLine->showError(tr("This panel cannot be hidden"), kMessageMs);
            return true;
        }
        QAction* toggle = m_ctx.actions->action(QLatin1String(id));
        if (toggle->isChecked())
            toggle->trigger();
        // The focus goes to a panel that is still there.
        const QVector<PanelFrame*> panels = vimwin::visiblePanels(m_window);
        if (!panels.isEmpty())
            vimwin::focusPanel(panels.first());
    } else if (name == QLatin1String("win_panel")) {
        if (!focusPanelKey(arg))
            return false;
    } else if (name.startsWith(QLatin1String("focus_"))) {
        if (!focusPanelKey(name.mid(6)))
            m_modeLine->showError(tr("No %1 panel on this page").arg(name.mid(6)), kMessageMs);
    } else if (name == QLatin1String("page_goto") || name == QLatin1String("page_prev")) {
        // {n}gt is page n, gt the next page; gT and {n}gT go back (Neovim tabpage.txt).
        QString line;
        if (name == QLatin1String("page_goto") && hasCount) {
            line = QStringLiteral("page %1").arg(count);
        } else {
            const int steps = name == QLatin1String("page_prev") ? std::max(1, count) : 1;
            bool ok = true;
            for (int i = 0; i < steps; ++i) {
                const QString message
                    = runCommand(name == QLatin1String("page_prev") ? QStringLiteral("page prev")
                                                                    : QStringLiteral("page next"),
                        &ok);
                if (!ok) {
                    showResult(message, false);
                    return true;
                }
            }
            afterPageChange();
            return true;
        }
        bool ok = true;
        const QString message = runCommand(line, &ok);
        showResult(ok ? QString() : message, ok);
        afterPageChange();
    } else {
        return false;
    }
    return true;
}

bool VimController::upOneBin()
{
    auto* pool = qobject_cast<MediaPoolPanel*>(vimwin::frameOf(QApplication::focusWidget()));
    if (!pool)
        return false;
    const QString here = pool->currentBin();
    if (here.isEmpty()) {
        m_modeLine->showMessage(tr("Already in Master"), kMessageMs);
        return true;
    }
    const Bin* bin = m_ctx.project->mediaPool()->bin(here);
    pool->setCurrentBin(bin ? bin->parentId : QString());
    return true;
}

void VimController::searchInList()
{
    PanelFrame* frame = vimwin::frameOf(QApplication::focusWidget());
    if (frame && frame->searchField() && frame->searchField()->isVisible()) {
        frame->focusSearchField(); // the panel filters its rows; n and N then walk them
        m_listHost->clearSearch();
        return;
    }
    m_searchInList = true;
    showCommandLine(true);
}

bool VimController::newBin()
{
    auto* pool = qobject_cast<MediaPoolPanel*>(vimwin::frameOf(QApplication::focusWidget()));
    if (!pool)
        return false;
    const QString key = pool->createBin(tr("New Bin"), pool->currentBinKey());
    if (!key.isEmpty())
        pool->setCurrentBinKey(key);
    m_modeLine->showMessage(tr("New bin: r renames it"), kMessageMs);
    return true;
}

bool VimController::handleCommand(const QString& name, int count, bool hasCount, const QString& arg)
{
    if (name.startsWith(QLatin1String("picker_"))) {
        QTimer::singleShot(0, this, [this, kind = name.mid(7)] { openPicker(kind); });
        return true;
    }
    if (name.startsWith(QLatin1String("transition_")))
        return transitionCommand(name, count, hasCount);
    return windowCommand(name, count, hasCount, arg);
}

bool VimController::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_input->active() || event->type() != QEvent::KeyPress)
        return false;
    auto* key = static_cast<QKeyEvent*>(event);
    const Qt::KeyboardModifiers mods = key->modifiers() & ~Qt::KeypadModifier;
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || widget == m_line || widget->property(VimInput::kCommandLineProperty).toBool())
        return false;
    const bool down = key->key() == Qt::Key_J && mods == Qt::ControlModifier;
    const bool up = key->key() == Qt::Key_K && mods == Qt::ControlModifier;
    const bool enter
        = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && mods == Qt::NoModifier;
    // The search field of a panel: Ctrl+J / Ctrl+K move the selection of its list (Telescope's
    // keys), Return gives the focus to the list (the filter stays).
    if (auto* edit = qobject_cast<QLineEdit*>(widget)) {
        PanelFrame* frame = vimwin::frameOf(edit);
        if (frame && frame->searchField() == edit) {
            QWidget* view = vimwin::mainViewOf(frame);
            if (view && (down || up)) {
                VimListHost::step(qobject_cast<QAbstractItemView*>(view), down ? 1 : -1);
                return true;
            }
            if (view && enter) {
                QTimer::singleShot(0, view, [view] { view->setFocus(Qt::OtherFocusReason); });
                return false; // the field's own Return still runs
            }
        }
    }
    // Dialogs keep the Qt keys; Ctrl+J / Ctrl+K also move in a list there (or, from the filter
    // field above it, in the only list).
    if ((down || up) && widget->window() && qobject_cast<QDialog*>(widget->window())) {
        QAbstractItemView* view = qobject_cast<QAbstractItemView*>(widget);
        if (!view && qobject_cast<QLineEdit*>(widget)) {
            const auto views = widget->window()->findChildren<QAbstractItemView*>();
            QAbstractItemView* only = nullptr;
            int visible = 0;
            for (QAbstractItemView* v : views) {
                if (v->isVisible() && v->parentWidget()
                    && !v->parentWidget()->inherits("QComboBoxPrivateContainer")) {
                    only = v;
                    ++visible;
                }
            }
            if (visible == 1)
                view = only;
        }
        if (view) {
            QKeyEvent press(QEvent::KeyPress, down ? Qt::Key_Down : Qt::Key_Up, Qt::NoModifier);
            QApplication::sendEvent(view, &press);
            return true;
        }
    }
    return false;
}

} // namespace kk
