// SPDX-License-Identifier: GPL-3.0-or-later
// ActionRegistry implementation: registration, keymap files, overrides of the active profile.
// The profile list and profile management live in KeymapProfiles.cpp.
#include "ui/common/ActionRegistry.h"

#include "ui/common/Icons.h"

#include <QAction>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSettings>

#include <optional>

namespace kk {

namespace {

Q_LOGGING_CATEGORY(lcActions, "kk.ui.actions", QtInfoMsg)

constexpr int kKeymapFormatVersion = 1;

QString plainText(const QAction* action)
{
    QString text = action->text();
    text.remove(QLatin1Char('&'));
    text.remove(QStringLiteral("…"));
    return text;
}

// Dynamic property remembering the last generated tool tip, to tell it from a custom one.
constexpr char kAutoToolTipProperty[] = "kkAutoToolTip";

/// Sets the tool tip to "Text  (Shortcut)". Unless @p force is set, a tool tip the owner
/// customised (anything other than the last generated one) is left alone.
void updateToolTip(QAction* action, bool force)
{
    const QString lastAuto = action->property(kAutoToolTipProperty).toString();
    if (!force && action->toolTip() != lastAuto)
        return;
    const QString text = plainText(action);
    const QKeySequence key = action->shortcut();
    const QString tip = key.isEmpty()
        ? text
        : QStringLiteral("%1  (%2)").arg(text, key.toString(QKeySequence::NativeText));
    action->setProperty(kAutoToolTipProperty, tip);
    action->setToolTip(tip);
}

QStringList toStringList(const QList<QKeySequence>& keys)
{
    QStringList list;
    for (const QKeySequence& k : keys)
        list.append(k.toString(QKeySequence::PortableText));
    return list;
}

} // namespace

ActionRegistry::ActionRegistry(QObject* parent, const QString& settingsGroup)
    : QObject(parent)
    , m_settingsGroup(settingsGroup)
{ }

bool ActionRegistry::readKeymap(
    const QString& path, KeyMap& out, QString* error, QString* name, QString* basedOn)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("Cannot open %1: %2").arg(path, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = doc.object();
    const QJsonObject shortcuts = root.value(QLatin1String("shortcuts")).toObject();
    if (doc.isNull() || !root.contains(QLatin1String("shortcuts"))) {
        if (error)
            *error = tr("%1 is not a keymap file: %2").arg(path, parseError.errorString());
        return false;
    }
    if (name)
        *name = root.value(QLatin1String("name")).toString();
    if (basedOn)
        *basedOn = root.value(QLatin1String("basedOn")).toString();
    for (auto it = shortcuts.constBegin(); it != shortcuts.constEnd(); ++it) {
        QList<QKeySequence> keys;
        // Accept a single string or a list of strings.
        const QJsonArray list
            = it.value().isArray() ? it.value().toArray() : QJsonArray {it.value().toString()};
        for (const QJsonValue& v : list) {
            const QKeySequence key
                = QKeySequence::fromString(v.toString(), QKeySequence::PortableText);
            if (!key.isEmpty())
                keys.append(key);
        }
        out.insert(it.key(), keys);
    }
    return true;
}

bool ActionRegistry::writeKeymap(const QString& path, const KeyMap& keys, const QString& name,
    const QString& basedOn, QString* error)
{
    QJsonObject shortcuts;
    for (auto it = keys.cbegin(); it != keys.cend(); ++it)
        shortcuts.insert(it.key(), QJsonArray::fromStringList(toStringList(it.value())));
    QJsonObject root {{QLatin1String("formatVersion"), kKeymapFormatVersion}};
    if (!name.isEmpty())
        root.insert(QLatin1String("name"), name);
    if (!basedOn.isEmpty())
        root.insert(QLatin1String("basedOn"), basedOn);
    root.insert(QLatin1String("shortcuts"), shortcuts);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool ActionRegistry::loadDefaultKeymap(const QString& path, QString* error)
{
    scanProfiles();
    migrateLegacyOverrides();
    const QString stored
        = QSettings()
              .value(QStringLiteral("keymap/profile"), QLatin1String(kDefaultProfile))
              .toString();
    m_activeProfile = profile(stored) ? stored : QLatin1String(kDefaultProfile);
    KeyMap base;
    KeyMap overrides;
    if (!loadProfileMaps(m_activeProfile, base, overrides, error)) {
        // A broken user profile falls back to the default one.
        m_activeProfile = QLatin1String(kDefaultProfile);
        base.clear();
        overrides.clear();
        if (!loadProfileMaps(m_activeProfile, base, overrides, error)) {
            qCWarning(lcActions) << "default keymap not loaded";
            return false;
        }
    }
    if (!path.isEmpty()) {
        base.clear();
        if (!readKeymap(path, base, error)) {
            qCWarning(lcActions) << "keymap not loaded:" << path;
            return false;
        }
    }
    m_defaults = base;
    m_overrides = overrides;
    applyAll();
    return true;
}

void ActionRegistry::applyAll()
{
    for (const Entry& e : std::as_const(m_entries)) {
        const QList<QKeySequence> keys = userOverride(e.id).value_or(m_defaults.value(e.id));
        if (toStringList(e.action->shortcuts()) == toStringList(keys))
            continue;
        applyShortcuts(e.id, keys);
        emit shortcutsChanged(e.id);
    }
}

QAction* ActionRegistry::add(const QString& id, const QString& text, const QString& category,
    const QString& iconName, bool checkable)
{
    if (const auto it = m_index.constFind(id); it != m_index.cend()) {
        qCWarning(lcActions) << "action registered twice:" << id;
        return m_entries[*it].action;
    }
    auto* action = new QAction(text, this);
    action->setObjectName(id);
    action->setCheckable(checkable);
    if (!iconName.isEmpty())
        action->setIcon(icon(iconName));

    m_index.insert(id, int(m_entries.size()));
    m_entries.append(Entry {id, category, action});
    if (!m_categories.contains(category))
        m_categories.append(category);
    m_menuEntries[category].append(id);

    applyShortcuts(id, userOverride(id).value_or(m_defaults.value(id)));
    // Keep the "Text  (Shortcut)" tool tip in sync when an owner changes the text later
    // (e.g. "Undo Move Clip"). setToolTip() with an unchanged value emits nothing, so no loop.
    connect(action, &QAction::changed, this, [action] { updateToolTip(action, false); });
    return action;
}

void ActionRegistry::addSeparator(const QString& category)
{
    QStringList& entries = m_menuEntries[category];
    if (!entries.isEmpty() && entries.back() != QLatin1String(kSeparator))
        entries.append(QLatin1String(kSeparator));
}

void ActionRegistry::addSubmenu(
    const QString& category, const QString& subCategory, const QString& title)
{
    m_submenuTitles.insert(subCategory, title);
    if (!m_categories.contains(category))
        m_categories.append(category);
    m_menuEntries[category].append(QLatin1String(kSubmenuPrefix) + subCategory);
}

QString ActionRegistry::submenuOf(const QString& entry)
{
    return entry.startsWith(QLatin1String(kSubmenuPrefix)) ? entry.mid(int(qstrlen(kSubmenuPrefix)))
                                                           : QString();
}

QAction* ActionRegistry::action(const QString& id) const
{
    const auto it = m_index.constFind(id);
    if (it == m_index.cend()) {
        qCWarning(lcActions) << "unknown action id" << id;
        return nullptr;
    }
    return m_entries[*it].action;
}

QStringList ActionRegistry::ids() const
{
    QStringList list;
    for (const Entry& e : m_entries)
        list.append(e.id);
    return list;
}

QList<QAction*> ActionRegistry::actions() const
{
    QList<QAction*> list;
    for (const Entry& e : m_entries)
        list.append(e.action);
    return list;
}

QString ActionRegistry::category(const QString& id) const
{
    const auto it = m_index.constFind(id);
    return it == m_index.cend() ? QString() : m_entries[*it].category;
}

QStringList ActionRegistry::menuEntries(const QString& category) const
{
    QStringList entries = m_menuEntries.value(category);
    if (!entries.isEmpty() && entries.back() == QLatin1String(kSeparator))
        entries.removeLast();
    return entries;
}

QList<QKeySequence> ActionRegistry::defaultShortcuts(const QString& id) const
{
    return m_defaults.value(id);
}

QList<QKeySequence> ActionRegistry::shortcuts(const QString& id) const
{
    const QAction* a = contains(id) ? m_entries[m_index.value(id)].action : nullptr;
    return a ? a->shortcuts() : QList<QKeySequence> {};
}

std::optional<QList<QKeySequence>> ActionRegistry::userOverride(const QString& id) const
{
    const auto it = m_overrides.constFind(id);
    if (it == m_overrides.cend())
        return std::nullopt;
    return *it;
}

void ActionRegistry::applyShortcuts(const QString& id, const QList<QKeySequence>& shortcuts)
{
    QAction* a = m_entries[m_index.value(id)].action;
    const bool firstTime = !a->property(kAutoToolTipProperty).isValid();
    a->setShortcuts(shortcuts);
    updateToolTip(a, firstTime);
}

void ActionRegistry::setShortcuts(const QString& id, const QList<QKeySequence>& shortcuts)
{
    if (!contains(id))
        return;
    if (toStringList(shortcuts) == toStringList(m_defaults.value(id)))
        m_overrides.remove(id);
    else
        m_overrides.insert(id, shortcuts);
    persistOverrides();
    applyShortcuts(id, shortcuts);
    emit shortcutsChanged(id);
}

void ActionRegistry::resetShortcuts(const QString& id)
{
    setShortcuts(id, m_defaults.value(id));
}

void ActionRegistry::resetAllShortcuts()
{
    // One write of the profile instead of one per command.
    ++m_batch;
    for (const Entry& e : std::as_const(m_entries))
        resetShortcuts(e.id);
    --m_batch;
    persistOverrides();
}

QString ActionRegistry::actionForShortcut(
    const QKeySequence& sequence, const QString& exceptId) const
{
    if (sequence.isEmpty())
        return {};
    for (const Entry& e : m_entries) {
        if (e.id != exceptId && e.action->shortcuts().contains(sequence))
            return e.id;
    }
    return {};
}

bool ActionRegistry::exportKeymap(const QString& path, QString* error) const
{
    KeyMap keys;
    for (const Entry& e : m_entries)
        keys.insert(e.id, e.action->shortcuts());
    const auto info = profile(m_activeProfile);
    return writeKeymap(path, keys, info ? info->name : QString(), {}, error);
}

bool ActionRegistry::importKeymap(const QString& path, QString* error)
{
    KeyMap keymap;
    if (!readKeymap(path, keymap, error))
        return false;
    ++m_batch;
    for (auto it = keymap.cbegin(); it != keymap.cend(); ++it) {
        if (contains(it.key()))
            setShortcuts(it.key(), it.value());
    }
    --m_batch;
    persistOverrides();
    return true;
}

} // namespace kk
