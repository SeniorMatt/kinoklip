// SPDX-License-Identifier: GPL-3.0-or-later
// VimOptions — the settings of the Vim mode (spec section 83): the on/off option (QSettings
// "vim/enabled", default off, independent of the keyboard preset), the leader key, the key
// timeout, the physical-key switch, and the migration of the old "Vim" keyboard preset.
#pragma once

#include <QString>

namespace kk::vimopts {

inline constexpr char kEnabledKey[] = "vim/enabled";
inline constexpr char kLeaderKey[] = "vim/leader";
inline constexpr char kTimeoutKey[] = "vim/timeoutlen";
inline constexpr char kPhysicalKeysKey[] = "vim/physicalKeys";
/// Set by the migration of the old Vim preset; the mode line shows a one-time notice, then
/// clears it.
inline constexpr char kMigrationNoticeKey[] = "vim/migrationNotice";
/// Id of the action that toggles Vim mode (Kinoklip menu).
inline constexpr char kActionId[] = "settings.vim_mode";

inline constexpr int kDefaultTimeoutMs = 400;
inline constexpr int kMinTimeoutMs = 50;
inline constexpr int kMaxTimeoutMs = 5000;

/// Whether Vim mode is on (default false).
[[nodiscard]] bool enabled();
void setEnabled(bool on);

/// The leader key as a key token ("<Space>" by default).
[[nodiscard]] QString leader();
void setLeader(const QString& token);

/// Milliseconds to wait for the next key of an ambiguous sequence (default 400).
[[nodiscard]] int timeoutMs();
void setTimeoutMs(int ms);

/// Keys match by their position on the keyboard when the layout is not Latin (default true).
[[nodiscard]] bool physicalKeys();
void setPhysicalKeys(bool on);

/// Moves the old "Vim" keyboard preset to the Vim mode option (settings of 1.2 - 1.4):
/// - keymap/profile "vim": vim/enabled = true, keymap/profile = resolve-windows, the per-key
///   changes of group "<shortcutsGroup>/vim" move to "<shortcutsGroup>/resolve-windows" (a key
///   already changed there keeps its value) and the "vim" group is removed;
/// - user profile files in @p userKeymapDir with "basedOn": "vim" are rebased to
///   resolve-windows; when such a profile is the active one, vim/enabled = true.
/// Safe to call again and with no old settings. Returns whether anything was migrated.
bool migrateLegacyVimProfile(
    const QString& userKeymapDir, const QString& shortcutsGroup = QStringLiteral("shortcuts"));

} // namespace kk::vimopts
