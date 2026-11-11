// SPDX-License-Identifier: GPL-3.0-or-later
// VimKeys — key tokens of the Vim layer (spec section 83). A key is one token: a printable
// character ("g", "G", "{", "5"), or a name in angle brackets ("<Space>", "<CR>", "<Esc>",
// "<Tab>", "<BS>", "<Del>", "<Left>", "<F5>", "<C-h>", "<A-x>", "<C-S-h>"). A key sequence is a
// list of tokens, written as in Vim mappings: "gg", "<leader>ff", "<C-w>h". "<lt>" is the
// literal "<". The token of a key event follows the physical key position when the keyboard
// layout is not Latin, so a Russian layout works in NORMAL without switching.
//
// The idea of keys as strings with modifier prefixes and of a parser that turns a key string
// into a list follows qutebrowser's keyutils (https://github.com/qutebrowser/qutebrowser,
// GPL-3.0-or-later); the code is written for Kinoklip. See attribution.md.
#pragma once

#include <QString>
#include <QStringList>
#include <Qt>

#include <cstdint>

class QKeyEvent;

namespace kk::vimkeys {

/// The leader as a token in key strings: "<leader>".
inline constexpr char kLeaderName[] = "leader";

/// Splits @p spec ("<leader>ff", "gg", "<C-w>h", "m{a-z}") into tokens. "<leader>" becomes
/// @p leader (a token, "<Space>" by default). Returns an empty list and sets @p error for text
/// that is not a key sequence. A character class such as "{a-z}" is one token that matches
/// any one typed character of the class.
[[nodiscard]] QStringList parseKeys(const QString& spec,
    const QString& leader = QStringLiteral("<Space>"), QString* error = nullptr);

/// Writes tokens back as a key string ("<Space>f" stays "<Space>f"; the leader is not
/// re-substituted).
[[nodiscard]] QString joinKeys(const QStringList& tokens);

/// A short display of a token for the mode line and which-key: "<Space>" -> "SPC",
/// "<CR>" -> "CR", others as written.
[[nodiscard]] QString displayToken(const QString& token);

/// Whether @p token is a printable character (not a named key) - the layer takes those even
/// when unbound.
[[nodiscard]] bool isCharToken(const QString& token);

/// Whether the character class token (e.g. "{a-z}") matches the one-character token @p typed.
[[nodiscard]] bool classMatches(const QString& classToken, const QString& typed);
[[nodiscard]] bool isClassToken(const QString& token);

/// What the layer needs to know about a key event. Built from a QKeyEvent, or by hand in tests.
struct KeyInfo
{
    int key = 0; ///< Qt::Key (may be a character code for hand-made input)
    Qt::KeyboardModifiers modifiers;
    QString text;
    quint32 nativeScanCode = 0; ///< QKeyEvent::nativeScanCode (X11/Wayland: evdev + 8)
    quint32 nativeVirtualKey = 0;
    static KeyInfo from(const QKeyEvent* event);
};

/// The US-QWERTY character at the physical key with evdev code @p evdev (KEY_Q = 16 ...), with
/// or without Shift; empty for keys outside the main block.
[[nodiscard]] QString usCharForEvdev(int evdev, bool shift);
/// The evdev code of @p nativeScanCode on this platform (X11/Wayland: scan code - 8, Windows
/// set-1 scan codes equal evdev codes for the main block); -1 if unknown.
[[nodiscard]] int evdevFromNative(quint32 nativeScanCode);

/// Turns key events into tokens. Keys match by their position on the keyboard when the layout
/// is not Latin: the layout counts as non-Latin from the moment a letter key produced a
/// non-ASCII character until a letter key produces an ASCII one (so the digit row, whose
/// shifted characters differ between layouts, follows). Off = the typed character.
class KeyTranslator
{
public:
    void setPhysical(bool on) { m_physical = on; }
    [[nodiscard]] bool physical() const { return m_physical; }
    /// The token of @p key, or an empty string for a key the layer has no name for (a bare
    /// modifier, Meta chords).
    [[nodiscard]] QString token(const KeyInfo& key);
    [[nodiscard]] bool nonLatinLayout() const { return m_nonLatin; }

private:
    bool m_physical = true;
    bool m_nonLatin = false;
};

} // namespace kk::vimkeys
