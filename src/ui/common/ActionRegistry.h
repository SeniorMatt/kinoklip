// SPDX-License-Identifier: GPL-3.0-or-later
// ActionRegistry — every user command as a QAction with a stable string id and remappable keys.
#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QVector>

#include <optional>

class QAction;

namespace cw {

/// Central catalogue of user commands ("edit.split", "transport.play_pause", ...).
///
/// - Commands are registered once at start-up (see StandardActions.cpp) with a display text,
///   a menu category and an optional icon. Owners of the behaviour connect later:
///   `connect(ctx.actions->action("edit.split"), &QAction::triggered, ...)`.
/// - Default shortcuts come from a keymap JSON (resources/keymap_default.json); user overrides
///   are stored in QSettings (group "shortcuts") and applied on registration.
/// - The main window adds all actions to itself so shortcuts work application-wide.
///
/// Keymap JSON format: `{"formatVersion": 1, "shortcuts": {"<id>": ["Ctrl+B", ...], ...}}`
/// using QKeySequence::PortableText.
///
/// Thread-safety: GUI thread only.
class ActionRegistry : public QObject
{
    Q_OBJECT
public:
    /// Separator marker returned by menuEntries().
    static constexpr char kSeparator[] = "-";

    /// Creates an empty registry. @p settingsGroup is the QSettings group for user overrides.
    explicit ActionRegistry(
        QObject* parent = nullptr, const QString& settingsGroup = QStringLiteral("shortcuts"));

    /// Loads default shortcuts from a keymap file and applies them to registered actions that
    /// have no user override. Returns false and sets @p error if the file is unreadable.
    bool loadDefaultKeymap(
        const QString& path = QStringLiteral(":/keymap_default.json"), QString* error = nullptr);

    /// Registers a command. @p category is a menu id ("file", "edit", ...); @p iconName names
    /// an icon from resources/icons (may be empty). Registering an existing id returns the
    /// existing action unchanged.
    QAction* add(const QString& id, const QString& text, const QString& category,
        const QString& iconName = {}, bool checkable = false);
    /// Appends a menu separator to @p category (after the last added action).
    void addSeparator(const QString& category);

    /// The action with id @p id, or nullptr (with a warning) if it is unknown.
    [[nodiscard]] QAction* action(const QString& id) const;
    /// True if @p id is registered.
    [[nodiscard]] bool contains(const QString& id) const { return m_index.contains(id); }
    /// All ids in registration order.
    [[nodiscard]] QStringList ids() const;
    /// All actions in registration order.
    [[nodiscard]] QList<QAction*> actions() const;
    /// Category ids in order of first use.
    [[nodiscard]] QStringList categories() const { return m_categories; }
    /// Category of action @p id (empty if unknown).
    [[nodiscard]] QString category(const QString& id) const;
    /// Ids and kSeparator markers of a category, in menu order.
    [[nodiscard]] QStringList menuEntries(const QString& category) const;

    /// Default shortcuts of @p id (from the keymap file).
    [[nodiscard]] QList<QKeySequence> defaultShortcuts(const QString& id) const;
    /// Current shortcuts of @p id.
    [[nodiscard]] QList<QKeySequence> shortcuts(const QString& id) const;
    /// Rebinds @p id and persists the override (removed again if equal to the defaults).
    void setShortcuts(const QString& id, const QList<QKeySequence>& shortcuts);
    /// Restores the default shortcuts of @p id.
    void resetShortcuts(const QString& id);
    /// Restores the defaults of every action.
    void resetAllShortcuts();
    /// Id of the action (other than @p exceptId) bound to @p sequence, or empty (conflict check).
    [[nodiscard]] QString actionForShortcut(
        const QKeySequence& sequence, const QString& exceptId = {}) const;

    /// Writes the current shortcuts of all actions as a keymap JSON file.
    bool exportKeymap(const QString& path, QString* error = nullptr) const;
    /// Applies a keymap JSON file as user overrides (unknown ids are ignored).
    bool importKeymap(const QString& path, QString* error = nullptr);

signals:
    /// The shortcuts of @p id changed (rebind, reset, import).
    void shortcutsChanged(const QString& id);

private:
    struct Entry
    {
        QString id;
        QString category;
        QAction* action = nullptr;
    };

    static bool readKeymap(
        const QString& path, QHash<QString, QList<QKeySequence>>& out, QString* error);
    void applyShortcuts(const QString& id, const QList<QKeySequence>& shortcuts);
    [[nodiscard]] std::optional<QList<QKeySequence>> userOverride(const QString& id) const;

    QString m_settingsGroup;
    QVector<Entry> m_entries;
    QHash<QString, int> m_index;
    QStringList m_categories;
    QHash<QString, QStringList> m_menuEntries;
    QHash<QString, QList<QKeySequence>> m_defaults;
};

} // namespace cw
