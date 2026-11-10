// SPDX-License-Identifier: GPL-3.0-or-later
// EmojiText — colour emoji in titles and captions (spec section 69). Qt turns text into outlines
// for the stroke and shadow of a title, and a colour emoji font has no outlines, so emoji are cut
// out of the text here and drawn as glyphs with QPainter::drawText; the rest of the text keeps
// its outline path.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace kk::render::emoji {

/// A piece of one line of text: plain text or one emoji sequence (a flag, a family, a skin
/// tone, a digit in a keycap ... is one run).
struct Run
{
    QString text;
    bool emoji = false;
};

/// True when @p text holds at least one emoji.
[[nodiscard]] bool containsEmoji(const QString& text);

/// Splits @p line into runs, in order; joining the run texts gives @p line back.
[[nodiscard]] QVector<Run> split(const QString& line);

/// The font families to draw @p family with: the family itself, then the colour emoji fonts
/// Kinoklip knows (the bundled Noto Color Emoji first).
[[nodiscard]] QStringList familiesWithEmoji(const QString& family);

/// Makes the bundled Noto Color Emoji (OFL 1.1; AppImage: usr/share/kinoklip/fonts, or the
/// folder named by KK_EMOJI_FONT) known to Qt when the system has no colour emoji font of its
/// own. Safe to call from any thread and more than once. Needs a QGuiApplication.
void registerBundledFont();

/// True when Qt knows a colour emoji font (after registerBundledFont()).
[[nodiscard]] bool hasColourFont();

} // namespace kk::render::emoji
