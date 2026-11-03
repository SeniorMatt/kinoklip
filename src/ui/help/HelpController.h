// SPDX-License-Identifier: GPL-3.0-or-later
// HelpController — wires the in-app help into the main window: F1 (help.guide_context),
// Help > Guide…, Help > Getting Started, the "?" buttons of panel title bars.
#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

class QUrl;
class QWidget;

namespace kk {

class ActionRegistry;
class GuideDialog;

/// Created by the main window once its pages exist.
///
/// - `help.guide_context` (F1) opens the article for what the user is on: the contexts of the
///   widget under the pointer (inside @p window), then of the focus widget, then the current
///   page, resolved by GuideIndex::resolve() (effect > action > panel > page); no match opens
///   the guide's start page. F1 over an open menu adds `action:<id>` of its highlighted entry.
/// - `help.guide` shows a GuideDialog; `help.getting_started` opens the "getting-started"
///   article and is hidden when the index has none.
/// - Every PanelFrame of the window whose `panel:<key>` has an article gets a "?" button at
///   the right of its title bar.
class HelpController : public QObject
{
    Q_OBJECT
public:
    /// @p currentPage returns the id of the visible page ("edit", "deliver", ...).
    HelpController(ActionRegistry& actions, QWidget* window, std::function<QString()> currentPage,
        QObject* parent = nullptr);
    ~HelpController() override;

    /// The contexts F1 would use for this pointer / focus situation, most specific first
    /// within each source (pure apart from reading the widgets).
    [[nodiscard]] QStringList contexts(const QWidget* hover, const QWidget* focus) const;
    /// The contexts right now (QApplication's focus widget and the widget under the pointer).
    [[nodiscard]] QStringList currentContexts() const;
    /// The URL F1 opens for @p contexts: the resolved article or the start page.
    [[nodiscard]] QUrl urlFor(const QStringList& contexts) const;

    /// F1: opens the article for currentContexts() (plus @p extra, e.g. a menu command).
    void openContextHelp(const QStringList& extra = {});
    /// Help > Guide…: shows (or raises) the guide dialog.
    GuideDialog* showGuideDialog();
    /// Adds the "?" buttons to the window's panels (done once by the constructor; call again
    /// after adding panels).
    void decoratePanels();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ActionRegistry& m_actions;
    QWidget* m_window = nullptr;
    std::function<QString()> m_currentPage;
    QPointer<GuideDialog> m_dialog;
};

} // namespace kk
