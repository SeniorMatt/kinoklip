// SPDX-License-Identifier: GPL-3.0-or-later
// HistoryWindow — Edit > History > Open History Window: the undo steps of the project by
// name; clicking one jumps there (spec 17.6).
#pragma once

#include <QDialog>

class QUndoView;

namespace kk {

class Project;

/// A non-modal tool window listing the project's undo steps (QUndoView over
/// Project::undoStack()): the first row is the opened / new project ("Original State"), then
/// one row per named step, the current one selected. Clicking a row undoes or redoes every
/// step up to it, as Resolve's History window does; the steps after the current one are drawn
/// in the scheme's disabled text colour (they are the redo steps).
///
/// While an edit is open (a drag in the timeline) clicks and keys in the list are ignored, so
/// the undo stack is never moved under an open edit.
///
/// Thread-safety: GUI thread only.
class HistoryWindow : public QDialog
{
    Q_OBJECT
public:
    /// Creates the window for @p project, parented to (and floating over) @p parent.
    explicit HistoryWindow(Project* project, QWidget* parent = nullptr);

    /// The list (tests click through it).
    [[nodiscard]] QUndoView* view() const { return m_view; }

protected:
    bool event(QEvent* event) override;
    /// Swallows clicks and keys in the list while the project has an open edit.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyColors();

    Project* m_project;
    QUndoView* m_view;
};

} // namespace kk
