// SPDX-License-Identifier: GPL-3.0-or-later
// NodeLibrary — the Compositing page's Nodes library (every node by category, drag onto the
// node editor) and the searchable "Select Node" popup (Shift+Space).
#pragma once

#include "ui/common/PanelFrame.h"

#include <QFrame>
#include <QPointer>

class QLineEdit;
class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace kk {

class CompSession;

/// Left panel of the Compositing page: a search field over a tree of the node catalogue
/// (compadapter::categories()). Items drag as mime::CompNodeType onto the node editor;
/// a double-click adds the node after the selected one.
///
/// Thread-safety: GUI thread only.
class NodeLibraryPanel : public PanelFrame
{
    Q_OBJECT
public:
    explicit NodeLibraryPanel(CompSession* session, QWidget* parent = nullptr);

    /// The tree (tests).
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }
    /// Shows only the nodes whose name contains @p text (case-insensitive).
    void setFilter(const QString& text);

private:
    QPointer<CompSession> m_session;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
};

/// The "Select Node" popup: a search field over a flat list of every node, best matches
/// first. Enter (or a click) adds the chosen node after the current one (CompSession::addNode())
/// and closes; Esc closes.
///
/// Thread-safety: GUI thread only.
class SelectNodePopup : public QFrame
{
    Q_OBJECT
public:
    explicit SelectNodePopup(CompSession* session, QWidget* parent = nullptr);

    /// Shows the popup centred on @p globalPos with an empty search, kept inside the window
    /// and the screen like a menu (popupBounds()).
    void popup(const QPoint& globalPos);
    /// Top-left of a popup of @p size opened at @p globalPos: centred horizontally on it, its
    /// search field under it, moved to stay inside @p bounds (left / top win when it is
    /// larger; empty bounds: not moved). Pure, for tests.
    [[nodiscard]] static QPoint placement(
        const QPoint& globalPos, const QSize& size, const QRect& bounds);
    /// Global rectangle popup() keeps the popup in: the parent's window within the available
    /// geometry of the screen at @p globalPos (either alone if they do not overlap).
    [[nodiscard]] QRect popupBounds(const QPoint& globalPos) const;
    /// The search field (tests type into it).
    [[nodiscard]] QLineEdit* searchField() const { return m_search; }
    /// The list (tests).
    [[nodiscard]] QListWidget* list() const { return m_list; }
    /// Node types in list order for @p text (matching names; names starting with the text
    /// first). Pure, for tests.
    [[nodiscard]] static QStringList matches(const QString& text);
    /// Adds the highlighted node and closes. Returns the new node id (0 = none).
    quint64 accept();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void fill(const QString& text);

    QPointer<CompSession> m_session;
    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
};

} // namespace kk
