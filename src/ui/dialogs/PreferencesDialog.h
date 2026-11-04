// SPDX-License-Identifier: GPL-3.0-or-later
// PreferencesDialog — Resolve-style preferences window: System / User tabs, a section list
// on the left, the registered settings pages of the selected section on the right.
#pragma once

#include "core/Preferences.h"

#include <QDialog>
#include <QStringList>
#include <QVector>

class QListWidget;
class QStackedWidget;
class QTabBar;

namespace kk {

class ActionRegistry;
class PlaybackEngine;
class SettingsPage;
class SettingsPageRegistry;

namespace prefs {
/// UI scale factor applied at start-up ("auto" or a factor such as "1.25"). Stored here until
/// core/Preferences.h gains it; main() must read it before creating QApplication
/// (QT_SCALE_FACTOR) — see src/ui/dialogs/readme.md.
inline constexpr char kUiScale[] = "ui/scaleFactor";
} // namespace prefs

/// The Resolve-style preferences window. Two groups at the top ("System": this machine,
/// "User": personal settings), each with a list of sections on the left; a section shows the
/// SettingsPages registered for it (ui/common/SettingsPages.h, SettingsPageInfo::group /
/// section), one after another under small headings, in a scroll area. Cancel / Save below.
///
/// Standard pages (registerStandardSettingsPages(), src/ui/dialogs/PreferencePages.cpp):
/// - System: "memory" (still image cache, GPU processing mode), "storage" (cache folder),
///   "decode" (hardware decoding with the working APIs in a table), "audio_io" (output device).
/// - User: "ui" (interface scale, animations, render notifications; the theme adds
///   "appearance" to this section), "project"
///   (autosave), "editing" (EditBehaviour durations and conventions, zoom anchor), "playback"
///   (preview quality, pre/post-roll, play-around return, restart at end), "keyboard" (opens
///   Keyboard Customization).
///
/// Save applies every page and emits preferencesChanged().
class PreferencesDialog : public QDialog
{
    Q_OBJECT
public:
    /// Creates the dialog with the pages of @p registry (null: only the standard pages).
    explicit PreferencesDialog(const SettingsPageRegistry* registry, QWidget* parent = nullptr);

    /// Applies (stores) every page.
    void apply();
    void accept() override;
    /// Shows the section @p sectionId (switching the System / User tab); false if unknown.
    bool showSection(const QString& sectionId);

signals:
    /// The preferences were stored (listeners re-read them, e.g. the autosave timer).
    void preferencesChanged();

private:
    void showGroup(int group);

    QVector<SettingsPage*> m_pages;
    QTabBar* m_groups = nullptr;
    QListWidget* m_sections = nullptr;
    QStackedWidget* m_stack = nullptr;
    QStringList m_groupIds; ///< "system", "user" (tab order)
};

/// Registers the standard pages with @p registry. @p actions (optional) provides the
/// Keyboard Customization action for the "keyboard" page's button.
void registerStandardSettingsPages(
    SettingsPageRegistry& registry, ActionRegistry* actions = nullptr);

/// Applies preferences that must be set before the playback engine / compositor exist
/// (GPU processing mode). Call once in main() before creating them.
void applyStartupPreferences();

/// Applies the stored preferences to the running application: hardware decoding, still
/// cache budget, cache folder, playback behaviour and default preview quality. Call at
/// start-up and whenever the Preferences dialog saved.
void applyRuntimePreferences(PlaybackEngine* playback);

/// Makes the media module use the stored hardware acceleration preference for new decoders
/// (QSV maps to VA-API: Intel's Linux decode path). Call at start-up and after changes.
void applyHwAccelPreference();

} // namespace kk
