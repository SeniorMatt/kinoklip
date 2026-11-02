// SPDX-License-Identifier: GPL-3.0-or-later
// SettingsPages — registry of preference pages, so any dialog layout can show them.
#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

namespace kk {

/// One page of preferences (a plain widget with an apply step).
///
/// Pages are created fresh every time a settings dialog opens, read their values from
/// QSettings (or the module that owns them) in the constructor and store them in apply().
/// They never assume which dialog shows them: today the Resolve-style PreferencesDialog shows
/// them in a System / User section list, a future Plasma profile can put them into a
/// KConfigDialog-style sidebar (plan.md 5.9, rule 4).
class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    using QWidget::QWidget;

    /// Stores the values shown on the page and applies them to the running application.
    virtual void apply() = 0;
};

/// Description of a registered page: identity, title, icon and a factory.
struct SettingsPageInfo
{
    QString id; ///< stable id ("playback", "appearance", ...)
    QString title; ///< translated title shown as group/sidebar label
    QString iconName; ///< freedesktop / Breeze icon name for a sidebar (may be empty)
    int order = 0; ///< sort key: lower comes first
    std::function<SettingsPage*(QWidget* parent)> create; ///< builds a new page widget
    /// Preferences group: "system" (machine: memory, storage, decoding, audio devices) or
    /// "user" (personal: interface, editing, playback, keyboard), like Resolve's two tabs.
    QString group = QStringLiteral("user");
    /// Section of the group that shows this page; pages with the same section are stacked
    /// in one section (titled by the page whose id equals the section, else the first page).
    /// Empty = a section of its own (= id).
    QString section;
};

/// All settings pages of the application, contributed by the modules that own the settings
/// (dialogs register the standard ones, src/app/Theme registers "appearance", ...).
///
/// Thread-safety: GUI thread only. The registry outlives every dialog that uses it.
class SettingsPageRegistry
{
public:
    /// Adds @p page; an existing page with the same id is replaced.
    void add(SettingsPageInfo page);
    /// Pages sorted by SettingsPageInfo::order (stable for equal orders).
    [[nodiscard]] QVector<SettingsPageInfo> pages() const;
    /// True if a page with @p id is registered.
    [[nodiscard]] bool contains(const QString& id) const;

private:
    QVector<SettingsPageInfo> m_pages;
};

} // namespace kk
