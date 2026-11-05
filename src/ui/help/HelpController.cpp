// SPDX-License-Identifier: GPL-3.0-or-later
// HelpController implementation.
#include "ui/help/HelpController.h"

#include "ui/common/ActionRegistry.h"
#include "ui/common/PanelFrame.h"
#include "ui/help/Guide.h"
#include "ui/help/GuideDialog.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QMenu>
#include <QUrl>

namespace kk {

namespace {
constexpr char kContextAction[] = "help.guide_context";
constexpr char kGuideAction[] = "help.guide";
constexpr char kGettingStartedAction[] = "help.getting_started";
constexpr char kGettingStartedId[] = "getting-started";
} // namespace

HelpController::HelpController(
    ActionRegistry& actions, QWidget* window, std::function<QString()> currentPage, QObject* parent)
    : QObject(parent)
    , m_actions(actions)
    , m_window(window)
    , m_currentPage(std::move(currentPage))
{
    if (QAction* f1 = actions.action(QLatin1String(kContextAction)))
        connect(f1, &QAction::triggered, this, [this] { openContextHelp(); });
    if (QAction* guide = actions.action(QLatin1String(kGuideAction)))
        connect(guide, &QAction::triggered, this, [this] { showGuideDialog(); });
    if (QAction* start = actions.action(QLatin1String(kGettingStartedAction))) {
        start->setVisible(guide::index().article(QLatin1String(kGettingStartedId)) != nullptr);
        connect(start, &QAction::triggered, this,
            [] { guide::openArticle(guide::index().article(QLatin1String(kGettingStartedId))); });
    }
    decoratePanels();
    // F1 while a menu is open: menus take the keyboard, so shortcuts do not fire there.
    qApp->installEventFilter(this);
}

HelpController::~HelpController()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

QStringList HelpController::contexts(const QWidget* hover, const QWidget* focus) const
{
    QStringList keys;
    auto addAll = [&keys](const QStringList& more) {
        for (const QString& key : more) {
            if (!keys.contains(key))
                keys.append(key);
        }
    };
    // Only widgets of this window count (not a floating tool window of another app, nor a
    // dialog in front).
    auto inWindow = [this](const QWidget* w) {
        return w && m_window && (w == m_window || m_window->isAncestorOf(w));
    };
    if (inWindow(hover))
        addAll(guide::contextsAt(hover));
    if (inWindow(focus))
        addAll(guide::contextsAt(focus));
    if (m_currentPage) {
        if (const QString page = m_currentPage(); !page.isEmpty())
            addAll({QStringLiteral("page:") + page});
    }
    return keys;
}

QStringList HelpController::currentContexts() const
{
    return contexts(QApplication::widgetAt(QCursor::pos()), QApplication::focusWidget());
}

QUrl HelpController::urlFor(const QStringList& contexts) const
{
    const GuideIndex& index = guide::index();
    const GuideArticle* article = index.resolve(contexts);
    return article ? index.url(*article) : index.startUrl();
}

void HelpController::openContextHelp(const QStringList& extra)
{
    guide::openUrl(urlFor(extra + currentContexts()), m_window);
}

GuideDialog* HelpController::showGuideDialog()
{
    if (!m_dialog) {
        m_dialog = new GuideDialog(m_window);
        m_dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_dialog->show();
    m_dialog->raise();
    m_dialog->activateWindow();
    return m_dialog;
}

void HelpController::decoratePanels()
{
    if (!m_window)
        return;
    const auto panels = m_window->findChildren<PanelFrame*>();
    for (PanelFrame* panel : panels) {
        if (panel->panelKey().isEmpty()
            || panel->titleBar()->findChild<guide::HelpButton*>(
                QString(), Qt::FindDirectChildrenOnly))
            continue;
        if (guide::HelpButton* button
            = guide::createHelpButton(QStringLiteral("panel:") + panel->panelKey()))
            panel->addTitleBarWidget(button);
    }
}

bool HelpController::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::KeyPress)
        return false;
    auto* menu = qobject_cast<QMenu*>(watched);
    if (!menu || !menu->isVisible())
        return false;
    const auto* key = static_cast<QKeyEvent*>(event);
    const QKeySequence pressed(key->keyCombination());
    if (!m_actions.shortcuts(QLatin1String(kContextAction)).contains(pressed))
        return false;
    QStringList extra;
    if (const QAction* active = menu->activeAction()) {
        if (m_actions.contains(active->objectName()))
            extra.append(QStringLiteral("action:") + active->objectName());
    }
    // Close the whole menu chain first: the browser should not open behind a popup.
    for (int guard = 0; guard < 16 && QApplication::activePopupWidget(); ++guard)
        QApplication::activePopupWidget()->close();
    openContextHelp(extra);
    return true;
}

} // namespace kk
