// SPDX-License-Identifier: GPL-3.0-or-later
// Clip colour and flag palettes of the media pool and the timeline (Resolve-like: sixteen clip
// colours, sixteen flag colours; our own names and values).
#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

#include <span>

namespace kk {

/// One named palette entry. `id` is the stable string stored in project files, `name` the
/// untranslated display name (translate with QCoreApplication::translate("kk::ClipColors", ..)).
struct PaletteColor
{
    const char* id;
    const char* name;
    QRgb rgb;
};

/// The sixteen clip colours (media pool items and timeline clips), in menu order.
[[nodiscard]] std::span<const PaletteColor> clipColorPalette();
/// The sixteen marker colours (marker editor, Edit Index colour filter, EDL / CSV export),
/// in menu order. Markers store the colour value itself (Marker::color).
[[nodiscard]] std::span<const PaletteColor> markerColorPalette();
/// The marker colour closest to @p color (exact for colours picked in the marker editor).
[[nodiscard]] const PaletteColor& nearestMarkerColor(const QColor& color);
/// The sixteen flag colours, in menu order. Flag i is bit i of MediaItem::flags.
[[nodiscard]] std::span<const PaletteColor> flagPalette();

/// Clip colour with id @p id, or nullptr (also for the empty id = no colour).
[[nodiscard]] const PaletteColor* findClipColor(const QString& id);
/// Colour value of clip colour @p id; invalid QColor for an unknown / empty id.
[[nodiscard]] QColor clipColorValue(const QString& id);
/// Id of the clip colour whose value is exactly @p color (timeline clips store the value in
/// ClipData::label), or empty when it is not a palette colour.
[[nodiscard]] QString clipColorIdOf(const QColor& color);
/// Translated display name of clip colour @p id (empty for none / unknown).
[[nodiscard]] QString clipColorName(const QString& id);

/// Index of flag @p id in flagPalette(), or -1.
[[nodiscard]] int flagIndex(const QString& id);
/// Translated display name of flag @p index (empty if out of range).
[[nodiscard]] QString flagName(int index);
/// Colour of flag @p index (invalid if out of range).
[[nodiscard]] QColor flagColor(int index);
/// Ids of the flags set in @p flags, in palette order.
[[nodiscard]] QStringList flagIds(quint32 flags);
/// Bit set of the flag ids in @p ids (unknown ids are ignored).
[[nodiscard]] quint32 flagsFromIds(const QStringList& ids);
/// Number of flags (bits) set in @p flags.
[[nodiscard]] int flagCount(quint32 flags);

} // namespace kk
