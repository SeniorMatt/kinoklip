// SPDX-License-Identifier: GPL-3.0-or-later
// PreferencesDialog — user preferences: hardware acceleration, autosave, appearance.
#pragma once

#include "core/Preferences.h"

#include <QDialog>
#include <QVector>

namespace kk {

class SettingsPage;
class SettingsPageRegistry;

namespace prefs {
/// UI scale factor applied at start-up ("auto" or a factor such as "1.25"). Stored here until
/// core/Preferences.h gains it; main() must read it before creating QApplication
/// (QT_SCALE_FACTOR) — see src/ui/dialogs/readme.md.
inline constexpr char kUiScale[] = "ui/scaleFactor";
} // namespace prefs

/// The Resolve-style preferences window: every registered SettingsPage (see
/// ui/common/SettingsPages.h) as a titled group, OK / Cancel below.
///
/// Standard pages (registerStandardSettingsPages()):
/// - "playback": hardware acceleration (Auto / Off / VA-API / CUDA / QSV / Vulkan, key
///   `playback/hwaccel`), with the backends that work on this machine listed next to it
///   (media::hwCapabilities(), probed on a worker thread because opening devices is slow).
/// - "project": autosave interval in minutes (0 = off).
/// - "interface": UI scale (applies after a restart).
/// Other modules add their own (src/app/Theme: "appearance" = colour theme and widget style).
///
/// OK applies every page and emits preferencesChanged().
class PreferencesDialog : public QDialog
{
    Q_OBJECT
public:
    /// Creates the dialog with the pages of @p registry (null: only the standard pages).
    explicit PreferencesDialog(const SettingsPageRegistry* registry, QWidget* parent = nullptr);

    /// Applies (stores) every page.
    void apply();
    void accept() override;

signals:
    /// The preferences were stored (listeners re-read them, e.g. the autosave timer).
    void preferencesChanged();

private:
    QVector<SettingsPage*> m_pages;
};

/// Registers the standard pages (playback, project, interface) with @p registry.
void registerStandardSettingsPages(SettingsPageRegistry& registry);

/// Makes the media module use the stored hardware acceleration preference for new decoders
/// (QSV maps to VA-API: Intel's Linux decode path). Call at start-up and after changes.
void applyHwAccelPreference();

} // namespace kk
