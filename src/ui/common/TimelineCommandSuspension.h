// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineCommandSuspension — switches the Edit page timeline's commands off while a page
// without that timeline is shown.
#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QAction;
class QWidget;

namespace kk {

class ActionRegistry;

/// Disables the hidden Edit page timeline's editing commands while @p page is shown, so a
/// window-wide key (Delete, B, Ctrl+B, Ctrl+C, the multicam number keys, ...) does nothing
/// there instead of editing a timeline the user cannot see (docs/behavior-spec.md 42b.22,
/// 42b.26).
///
/// - The commands are those of the Edit, Trim, Timeline, Clip and Mark menus (submenus
///   included), minus alwaysKept() (undo / redo, history, keyboard customization, preferences,
///   audio scrubbing) and the page's own @p kept ids. Transport, view and page keys are in
///   other menus and never touched.
/// - Show (not spontaneous): every listed command that is enabled is disabled and remembered.
///   Hide (not spontaneous, i.e. not minimising): exactly those are enabled again, so a
///   command another owner had disabled stays disabled. Destroying the helper also restores.
///   Several helpers may hold one command (a page switch can show the next page before it
///   hides the previous one); it comes back when the last one lets go.
/// - Adoption is one line in the page's constructor (it watches the page's show / hide
///   events itself):
///   `new TimelineCommandSuspension(this, ctx.actions, {"mark.in", ...});`
///
/// Thread-safety: GUI thread only.
class TimelineCommandSuspension : public QObject
{
    Q_OBJECT
public:
    /// Watches @p page (also the parent). @p registry may be null (then it does nothing).
    TimelineCommandSuspension(
        QWidget* page, ActionRegistry* registry, const QStringList& kept = {});
    ~TimelineCommandSuspension() override;

    /// Ids kept by every page: they make sense without the timeline.
    static const QStringList& alwaysKept();
    /// Ids of the commands disabled for a page that keeps @p kept (registration order).
    [[nodiscard]] static QStringList commandIds(
        const ActionRegistry& registry, const QStringList& kept = {});
    /// commandIds() of this page.
    [[nodiscard]] QStringList commandIds() const;

    /// Disables the commands (no-op if already suspended).
    void suspend();
    /// Enables the commands suspend() disabled.
    void resume();
    [[nodiscard]] bool isSuspended() const { return m_suspendedState; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QPointer<ActionRegistry> m_registry;
    QStringList m_kept;
    QList<QPointer<QAction>> m_suspended; ///< the commands suspend() disabled
    bool m_suspendedState = false;
};

} // namespace kk
