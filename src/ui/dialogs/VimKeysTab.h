// SPDX-License-Identifier: GPL-3.0-or-later
// VimKeysTab — the "Vim keys" tab of Keyboard Customization (spec section 83): the key table of
// the Vim layer as a tree by context (Leader, Global, Timeline, Viewer) with the keys, the
// command and a description; add, change, remove and reset rows, the conflict and prefix
// checks, import and export of the user's changes, and a button for the vimrc text file. It
// edits the VimKeymap directly: every change is stored at once.
#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace kk {

class ActionRegistry;
class VimKeymap;

class VimKeysTab : public QWidget
{
    Q_OBJECT
public:
    VimKeysTab(ActionRegistry& actions, VimKeymap& keymap, QWidget* parent = nullptr);

    /// What an edit of a row asks for.
    struct Edit
    {
        QString context = QStringLiteral("global");
        QString modes = QStringLiteral("n");
        QString keys;
        QString command; ///< "edit.undo", "action:edit.undo", "vim:visual_start", ":w"
        QString description;
    };
    enum class Result {
        Saved,
        SavedWithWarning,
        Invalid,
        Conflict
    };
    /// Saves a row: a new one (no @p original) or a change of @p original (its keys move).
    /// Conflict: the keys are bound to something else in that context and mode; nothing is
    /// changed unless @p replace. @p message tells the reason or the warning (a prefix).
    Result saveBinding(const Edit& edit, const Edit* original = nullptr, bool replace = false,
        QString* message = nullptr);
    /// Removes the keys of a row (a default is unmapped; one of the user's own is dropped).
    bool removeBinding(const QString& context, const QString& modes, const QString& keys);
    /// Brings back the default of a row (drops the user's changes of those keys).
    void resetBinding(const QString& context, const QString& modes, const QString& keys);
    void resetAll();
    bool exportTo(const QString& path, QString* error = nullptr) const;
    bool importFrom(const QString& path, QString* error = nullptr);

    /// Filters the rows (keys, command, description).
    void setFilterText(const QString& text);
    /// The rows as "Group | keys | modes | command" (tests).
    [[nodiscard]] QStringList rowTexts() const;
    /// The lines of the check list ("Error: ...", "Warning: ...", "Info: ...").
    [[nodiscard]] QStringList issueLines() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }

public slots:
    void refresh();

private:
    void editCurrent(bool addNew);
    void updateButtons();
    void openVimrc();
    void importDialog();
    void exportDialog();

    ActionRegistry& m_actions;
    VimKeymap& m_keymap;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
    QListWidget* m_issues = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_edit = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_reset = nullptr;
};

} // namespace kk
