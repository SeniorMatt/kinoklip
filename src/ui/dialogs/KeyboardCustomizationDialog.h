// SPDX-License-Identifier: GPL-3.0-or-later
// KeyboardCustomizationDialog — searchable table of every command with its shortcuts; record
// new keys, resolve conflicts, reset, import/export keymaps (on top of ActionRegistry).
#pragma once

#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QLineEdit>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace kk {

class ActionRegistry;

/// A line edit that records one key chord (e.g. Ctrl+Shift+K) instead of typing text.
/// Escape without modifiers cancels; plain modifier presses are ignored.
class ShortcutRecorder : public QLineEdit
{
    Q_OBJECT
public:
    /// Creates an idle recorder.
    explicit ShortcutRecorder(QWidget* parent = nullptr);

signals:
    /// A chord was pressed.
    void recorded(const QKeySequence& sequence);
    /// Recording was cancelled with Escape.
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
};

/// Keyboard customisation for every ActionRegistry command.
///
/// - Commands are grouped by menu category; the search matches command names, ids and keys.
/// - Select a command and press "Record" (or double-click it) to capture a new key. The new key
///   replaces the command's primary shortcut (secondary ones are kept); "Clear" removes all.
/// - If the key is used by another command the dialog asks whether to reassign it (the other
///   command loses that key).
/// - A profile combo at the top switches the keymap profile (built-in presets and user
///   copies); its menu button duplicates, renames and deletes user profiles. Import creates a
///   user profile from a keymap file, Export writes the active profile.
/// - Changes apply immediately to the active profile (so they can be tried out); Cancel
///   reactivates the profile the dialog was opened with and restores its shortcuts. Reset /
///   Reset All restore the profile's defaults.
class KeyboardCustomizationDialog : public QDialog
{
    Q_OBJECT
public:
    /// How assignShortcut() treats a key that another command already uses.
    enum class ConflictPolicy {
        Ask, ///< show a question box (interactive use)
        Reassign, ///< take the key away from the other command
        Cancel ///< leave everything unchanged
    };

    /// Creates the dialog for @p registry (which must outlive it).
    explicit KeyboardCustomizationDialog(ActionRegistry& registry, QWidget* parent = nullptr);

    /// Command currently using @p sequence other than @p id, or empty (conflict check).
    [[nodiscard]] QString conflictFor(const QString& id, const QKeySequence& sequence) const;
    /// Makes @p sequence the primary shortcut of @p id. On a conflict, @p policy decides.
    /// Returns true if the shortcut was assigned.
    bool assignShortcut(const QString& id, const QKeySequence& sequence,
        ConflictPolicy policy = ConflictPolicy::Ask);
    /// Removes all shortcuts of @p id.
    void clearShortcuts(const QString& id);
    /// Filters the table (case-insensitive: name, id or key text).
    void setFilterText(const QString& text);
    /// Ids of the rows currently visible (tests).
    [[nodiscard]] QStringList visibleIds() const;
    /// Id of the selected command, or empty.
    [[nodiscard]] QString currentId() const;
    /// Selects command @p id.
    void setCurrentId(const QString& id);
    /// The profile combo (tests).
    [[nodiscard]] QComboBox* profileCombo() const { return m_profiles; }

    void reject() override;

protected:
    /// Enter in the search field goes to the first match and Enter on a command records a
    /// new key, instead of closing the dialog (spec 23.10).
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void populate();
    void refreshRow(const QString& id);
    void refreshAllRows();
    void updateButtons();
    void startRecording();
    void finishRecording(const QKeySequence& sequence);
    void resetCurrent();
    void resetAll();
    void importKeymap();
    void exportKeymap();
    void fillProfiles();
    void duplicateProfile();
    void renameProfile();
    void deleteProfile();
    [[nodiscard]] QString commandName(const QString& id) const;

    ActionRegistry& m_registry;
    QHash<QString, QList<QKeySequence>> m_original; ///< shortcuts when the dialog opened
    QString m_originalProfile; ///< profile active when the dialog opened
    QStringList m_createdProfiles; ///< imported / duplicated while open (Cancel deletes them)
    QHash<QString, QString> m_originalNames; ///< user profiles renamed while open: first name
    QComboBox* m_profiles = nullptr;
    QPushButton* m_profileMenu = nullptr;
    QHash<QString, QTreeWidgetItem*> m_rows;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
    ShortcutRecorder* m_recorder = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_record = nullptr;
    QPushButton* m_clear = nullptr;
    QPushButton* m_reset = nullptr;
};

} // namespace kk
