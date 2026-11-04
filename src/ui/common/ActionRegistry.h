// SPDX-License-Identifier: GPL-3.0-or-later
// ActionRegistry — every user command as a QAction with a stable string id and remappable keys.
#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

class QAction;
class QWidget;

namespace kk {

/// Central catalogue of user commands ("edit.split", "transport.play_pause", ...).
///
/// - Commands are registered once at start-up (see StandardActions.cpp) with a display text,
///   a menu category and an optional icon. Owners of the behaviour connect later:
///   `connect(ctx.actions->action("edit.split"), &QAction::triggered, ...)`.
/// - Default shortcuts come from the active **keymap profile** (plan.md 5.9 rule 1):
///   - built-in profiles are the read-only files `resources/keymaps/<id>.json` (`:/keymaps/`);
///     the default is kDefaultProfile ("resolve-windows", Resolve's Windows defaults);
///   - user profiles are JSON files in `<AppData>/keymaps/` (userProfileDirectory()); each one
///     is based on a built-in profile and stores its own overrides.
///   Rebinding a key always changes the **active** profile: a built-in profile keeps its
///   overrides in QSettings (group "<settingsGroup>/<profile id>"), a user profile in its file.
///   Reset restores the profile's base (the built-in file) for that command.
/// - The active profile id is stored in QSettings key "keymap/profile".
/// - The main window adds all actions to itself so shortcuts work application-wide.
///
/// Keymap JSON format: `{"formatVersion": 1, "name": "...", "basedOn": "<built-in id>",
/// "shortcuts": {"<id>": ["Ctrl+B", ...], ...}}` using QKeySequence::PortableText. "name" and
/// "basedOn" are optional (built-in files have no "basedOn"; in a user profile "shortcuts"
/// holds only the overrides of its base).
///
/// Thread-safety: GUI thread only.
class ActionRegistry : public QObject
{
    Q_OBJECT
public:
    /// Separator marker returned by menuEntries().
    static constexpr char kSeparator[] = "-";
    /// Prefix of a submenu marker returned by menuEntries(): ">settings.color_scheme" means
    /// "a submenu here, filled with the entries of category settings.color_scheme".
    static constexpr char kSubmenuPrefix[] = ">";

    /// Creates an empty registry. @p settingsGroup is the QSettings group for user overrides.
    explicit ActionRegistry(
        QObject* parent = nullptr, const QString& settingsGroup = QStringLiteral("shortcuts"));

    /// Id of the built-in default profile (Resolve's Windows shortcuts).
    static constexpr char kDefaultProfile[] = "resolve-windows";

    /// One keymap profile as listed by profiles().
    struct KeymapProfile
    {
        QString id; ///< stable id ("resolve-windows", "user-my-keys", ...)
        QString name; ///< display name
        bool builtIn = false; ///< read-only file shipped in the resources
        QString basedOn; ///< user profiles: the built-in profile providing the defaults
    };

    /// Loads the active keymap profile (QSettings "keymap/profile", else kDefaultProfile) and
    /// applies it to the registered actions; actions registered later pick it up as well.
    /// With a non-empty @p path that file is used as the defaults instead (tests, tools); the
    /// overrides of the active profile still apply. Returns false and sets @p error if the
    /// file is unreadable.
    bool loadDefaultKeymap(const QString& path = {}, QString* error = nullptr);

    // ---- Keymap profiles ----

    /// Built-in profiles first (sorted by name), then user profiles (sorted by name).
    [[nodiscard]] QVector<KeymapProfile> profiles() const;
    /// The profile with @p id, if it exists.
    [[nodiscard]] std::optional<KeymapProfile> profile(const QString& id) const;
    /// Id of the active profile.
    [[nodiscard]] QString activeProfile() const { return m_activeProfile; }
    /// Makes @p id the active profile (persisted) and applies its shortcuts. Returns false
    /// (and keeps the current one) if it does not exist or cannot be read.
    bool setActiveProfile(const QString& id, QString* error = nullptr);
    /// Creates a user profile named @p name with the current shortcuts of profile @p sourceId
    /// (defaults and overrides). Returns the new id, or empty on failure.
    QString duplicateProfile(
        const QString& sourceId, const QString& name, QString* error = nullptr);
    /// Renames user profile @p id. Built-in profiles cannot be renamed.
    bool renameProfile(const QString& id, const QString& name, QString* error = nullptr);
    /// Deletes user profile @p id (its file). Deleting the active profile activates the
    /// built-in profile it was based on. Built-in profiles cannot be deleted.
    bool deleteProfile(const QString& id, QString* error = nullptr);
    /// Creates a user profile from keymap file @p path (an exported profile or any keymap
    /// JSON; commands missing in the file keep their defaults). @p name empty: the file's
    /// "name", else its base name. Returns the new id, or empty on failure.
    QString importProfile(const QString& path, const QString& name = {}, QString* error = nullptr);
    /// Writes the complete shortcuts of profile @p id (defaults + overrides) to @p path.
    bool exportProfile(const QString& id, const QString& path, QString* error = nullptr) const;
    /// True if profile @p id has shortcuts that differ from its base.
    [[nodiscard]] bool isProfileModified(const QString& id) const;
    /// Folder holding the user profiles (default `<AppDataLocation>/keymaps`).
    [[nodiscard]] QString userProfileDirectory() const;
    /// Overrides the user profile folder (tests). Rescans the profiles.
    void setUserProfileDirectory(const QString& dir);

    /// Registers a command. @p category is a menu id ("file", "edit", ...); @p iconName names
    /// an icon from resources/icons (may be empty). Registering an existing id returns the
    /// existing action unchanged.
    QAction* add(const QString& id, const QString& text, const QString& category,
        const QString& iconName = {}, bool checkable = false);
    /// Appends a menu separator to @p category (after the last added action).
    void addSeparator(const QString& category);
    /// Appends a submenu to @p category whose entries are those of @p subCategory (which may be
    /// filled later, e.g. by the theme with one action per colour scheme). Menus are data: the
    /// main window builds the nested menu from this marker (plan.md 5.9).
    void addSubmenu(const QString& category, const QString& subCategory, const QString& title);
    /// Title of a submenu category added with addSubmenu(), or empty.
    [[nodiscard]] QString submenuTitle(const QString& subCategory) const
    {
        return m_submenuTitles.value(subCategory);
    }
    /// True if @p category is shown as a submenu of another category (not as a top-level menu).
    [[nodiscard]] bool isSubmenu(const QString& category) const
    {
        return m_submenuTitles.contains(category);
    }
    /// The category of a menuEntries() submenu marker, or empty if @p entry is none.
    [[nodiscard]] static QString submenuOf(const QString& entry);

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
    /// Where the keys of @p id act, for the conflict check: empty = the whole window (the
    /// default), "page:<id>" = only while that page is shown, "widget:<name>" = only while that
    /// widget has focus (it claims its keys before the window shortcuts, see NodeEditor).
    void setShortcutScope(const QString& id, const QString& scope);
    /// The scope of @p id (empty = window-wide, also for unknown ids).
    [[nodiscard]] QString shortcutScope(const QString& id) const;
    /// True if one key bound in scopes @p a and @p b is a real conflict: the same scope, or a
    /// window-wide key and a page key (both act while the page is shown). A widget scope
    /// overrides every other scope while it has focus, and two different pages are never shown
    /// at once, so those never conflict.
    [[nodiscard]] static bool scopesConflict(const QString& a, const QString& b);
    /// Id of the action (other than @p exceptId) bound to @p sequence whose scope conflicts
    /// with the scope of @p exceptId (window-wide when empty), or empty (conflict check).
    [[nodiscard]] QString actionForShortcut(
        const QKeySequence& sequence, const QString& exceptId = {}) const;

    /// The first shortcut of @p id as shown to the user (NativeText), empty when it has none.
    [[nodiscard]] QString shortcutText(const QString& id) const;
    /// Calls @p apply with shortcutText(@p id) now and whenever the shortcuts of @p id change,
    /// while @p context lives: a tooltip or label that names a key follows Keyboard
    /// Customization.
    void watchShortcut(
        QObject* context, const QString& id, const std::function<void(const QString&)>& apply);
    /// Keeps @p widget's tooltip at "@p text (key of @p id)" ("@p text" without a key).
    void bindToolTip(QWidget* widget, const QString& id, const QString& text);

    /// Writes the current shortcuts of all actions as a keymap JSON file.
    bool exportKeymap(const QString& path, QString* error = nullptr) const;
    /// Applies a keymap JSON file as user overrides (unknown ids are ignored).
    bool importKeymap(const QString& path, QString* error = nullptr);

signals:
    /// The shortcuts of @p id changed (rebind, reset, import, profile switch).
    void shortcutsChanged(const QString& id);
    /// A profile was added, renamed or deleted.
    void profilesChanged();
    /// Another profile became active (its shortcuts are already applied).
    void activeProfileChanged(const QString& id);

private:
    struct Entry
    {
        QString id;
        QString category;
        QAction* action = nullptr;
        QString scope; ///< setShortcutScope() (empty = window-wide)
    };

    using KeyMap = QHash<QString, QList<QKeySequence>>;
    /// A user profile file as found by scanProfiles().
    struct UserProfile
    {
        KeymapProfile info;
        QString path;
    };

    static bool readKeymap(const QString& path, KeyMap& out, QString* error,
        QString* name = nullptr, QString* basedOn = nullptr);
    static bool writeKeymap(const QString& path, const KeyMap& keys, const QString& name,
        const QString& basedOn, QString* error);
    void applyShortcuts(const QString& id, const QList<QKeySequence>& shortcuts);
    [[nodiscard]] std::optional<QList<QKeySequence>> userOverride(const QString& id) const;
    /// Re-reads the built-in and user profile lists.
    void scanProfiles();
    [[nodiscard]] const UserProfile* userProfile(const QString& id) const;
    [[nodiscard]] QString builtInPath(const QString& id) const;
    /// Base (defaults) and overrides of profile @p id.
    bool loadProfileMaps(const QString& id, KeyMap& base, KeyMap& overrides, QString* error) const;
    /// Writes m_overrides of the active profile to its store (QSettings group or user file).
    void persistOverrides();
    /// Moves overrides saved before profiles existed (directly in the settings group) into
    /// the default profile's group.
    void migrateLegacyOverrides();
    /// Applies m_defaults + m_overrides to every registered action.
    void applyAll();
    [[nodiscard]] QString uniqueUserId(const QString& name) const;

    QString m_settingsGroup;
    QVector<Entry> m_entries;
    QHash<QString, int> m_index;
    QStringList m_categories;
    QHash<QString, QStringList> m_menuEntries;
    KeyMap m_defaults; ///< base shortcuts of the active profile
    KeyMap m_overrides; ///< overrides of the active profile (empty list = unbound)
    QHash<QString, QString> m_submenuTitles;
    QString m_activeProfile = QLatin1String(kDefaultProfile);
    QVector<KeymapProfile> m_builtIns;
    QVector<UserProfile> m_userProfiles;
    QString m_userDir;
    bool m_profilesScanned = false;
    int m_batch = 0; ///< > 0: persistOverrides() is deferred to the end of a batch
};

} // namespace kk
