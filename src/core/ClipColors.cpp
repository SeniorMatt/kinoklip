// SPDX-License-Identifier: GPL-3.0-or-later
// Clip colour and flag palettes: tables + lookups.
#include "core/ClipColors.h"

#include <QCoreApplication>

#include <bit>

namespace kk {

namespace {

const char* const kContext = "kk::ClipColors";

// Saturated mid tones, readable with white clip names on top (the timeline draws the name
// band in this colour) and distinct from each other in the dark and the light schemes.
// clang-format off
const PaletteColor kClipColors[] = {
    {"orange",  QT_TRANSLATE_NOOP("kk::ClipColors", "Tangerine"), qRgb(0xe2, 0x7a, 0x2c)},
    {"apricot", QT_TRANSLATE_NOOP("kk::ClipColors", "Peach"),     qRgb(0xe0, 0x9a, 0x5a)},
    {"yellow",  QT_TRANSLATE_NOOP("kk::ClipColors", "Yellow"),    qRgb(0xc9, 0xa8, 0x1e)},
    {"lime",    QT_TRANSLATE_NOOP("kk::ClipColors", "Lime"),      qRgb(0x8d, 0xb3, 0x2a)},
    {"olive",   QT_TRANSLATE_NOOP("kk::ClipColors", "Olive"),     qRgb(0x6b, 0x7d, 0x2c)},
    {"green",   QT_TRANSLATE_NOOP("kk::ClipColors", "Green"),     qRgb(0x3c, 0x9a, 0x4e)},
    {"teal",    QT_TRANSLATE_NOOP("kk::ClipColors", "Teal"),      qRgb(0x23, 0x91, 0x8c)},
    {"navy",    QT_TRANSLATE_NOOP("kk::ClipColors", "Navy"),      qRgb(0x2e, 0x4a, 0x8a)},
    {"blue",    QT_TRANSLATE_NOOP("kk::ClipColors", "Blue"),      qRgb(0x3a, 0x7b, 0xd5)},
    {"purple",  QT_TRANSLATE_NOOP("kk::ClipColors", "Plum"),      qRgb(0x7a, 0x4a, 0xb0)},
    {"violet",  QT_TRANSLATE_NOOP("kk::ClipColors", "Violet"),    qRgb(0xa1, 0x5c, 0xd6)},
    {"pink",    QT_TRANSLATE_NOOP("kk::ClipColors", "Pink"),      qRgb(0xd4, 0x5f, 0x9e)},
    {"tan",     QT_TRANSLATE_NOOP("kk::ClipColors", "Sand"),      qRgb(0xb3, 0x92, 0x6c)},
    {"beige",   QT_TRANSLATE_NOOP("kk::ClipColors", "Cream"),     qRgb(0xc4, 0xb5, 0x96)},
    {"brown",   QT_TRANSLATE_NOOP("kk::ClipColors", "Brown"),     qRgb(0x8a, 0x5a, 0x36)},
    {"chocolate", QT_TRANSLATE_NOOP("kk::ClipColors", "Cocoa"),   qRgb(0x63, 0x40, 0x2a)},
};

// Flags are small markers, so they may be brighter than the clip colours.
const PaletteColor kFlags[] = {
    {"blue",     QT_TRANSLATE_NOOP("kk::ClipColors", "Blue"),     qRgb(0x3d, 0x8b, 0xf2)},
    {"cyan",     QT_TRANSLATE_NOOP("kk::ClipColors", "Cyan"),     qRgb(0x2c, 0xc6, 0xd8)},
    {"green",    QT_TRANSLATE_NOOP("kk::ClipColors", "Green"),    qRgb(0x46, 0xc0, 0x5a)},
    {"yellow",   QT_TRANSLATE_NOOP("kk::ClipColors", "Yellow"),   qRgb(0xf2, 0xcc, 0x2e)},
    {"red",      QT_TRANSLATE_NOOP("kk::ClipColors", "Red"),      qRgb(0xe8, 0x45, 0x3c)},
    {"pink",     QT_TRANSLATE_NOOP("kk::ClipColors", "Pink"),     qRgb(0xf0, 0x7a, 0xb8)},
    {"purple",   QT_TRANSLATE_NOOP("kk::ClipColors", "Purple"),   qRgb(0x9a, 0x5c, 0xe0)},
    {"fuchsia",  QT_TRANSLATE_NOOP("kk::ClipColors", "Magenta"),  qRgb(0xd8, 0x3c, 0xc8)},
    {"rose",     QT_TRANSLATE_NOOP("kk::ClipColors", "Rose"),     qRgb(0xe8, 0x8a, 0x8a)},
    {"lavender", QT_TRANSLATE_NOOP("kk::ClipColors", "Lilac"),    qRgb(0xb8, 0xa2, 0xe8)},
    {"sky",      QT_TRANSLATE_NOOP("kk::ClipColors", "Sky"),      qRgb(0x8a, 0xc8, 0xf0)},
    {"mint",     QT_TRANSLATE_NOOP("kk::ClipColors", "Mint"),     qRgb(0x8a, 0xe0, 0xb4)},
    {"lemon",    QT_TRANSLATE_NOOP("kk::ClipColors", "Lemon"),    qRgb(0xee, 0xe8, 0x7a)},
    {"sand",     QT_TRANSLATE_NOOP("kk::ClipColors", "Sand"),     qRgb(0xd8, 0xb8, 0x88)},
    {"cocoa",    QT_TRANSLATE_NOOP("kk::ClipColors", "Cocoa"),    qRgb(0x9a, 0x6a, 0x48)},
    {"cream",    QT_TRANSLATE_NOOP("kk::ClipColors", "Cream"),    qRgb(0xee, 0xe2, 0xc8)},
};
// clang-format on

static_assert(std::size(kFlags) <= 32, "flags are bits of a quint32");

} // namespace

std::span<const PaletteColor> clipColorPalette()
{
    return kClipColors;
}

std::span<const PaletteColor> flagPalette()
{
    return kFlags;
}

const PaletteColor* findClipColor(const QString& id)
{
    if (id.isEmpty())
        return nullptr;
    for (const PaletteColor& c : kClipColors) {
        if (id == QLatin1String(c.id))
            return &c;
    }
    return nullptr;
}

QColor clipColorValue(const QString& id)
{
    const PaletteColor* c = findClipColor(id);
    return c ? QColor(c->rgb) : QColor();
}

QString clipColorIdOf(const QColor& color)
{
    if (!color.isValid())
        return {};
    const QRgb rgb = color.rgb();
    for (const PaletteColor& c : kClipColors) {
        if (c.rgb == rgb)
            return QLatin1String(c.id);
    }
    return {};
}

QString clipColorName(const QString& id)
{
    const PaletteColor* c = findClipColor(id);
    return c ? QCoreApplication::translate(kContext, c->name) : QString();
}

int flagIndex(const QString& id)
{
    for (int i = 0; i < int(std::size(kFlags)); ++i) {
        if (id == QLatin1String(kFlags[i].id))
            return i;
    }
    return -1;
}

QString flagName(int index)
{
    if (index < 0 || index >= int(std::size(kFlags)))
        return {};
    return QCoreApplication::translate(kContext, kFlags[index].name);
}

QColor flagColor(int index)
{
    if (index < 0 || index >= int(std::size(kFlags)))
        return {};
    return QColor(kFlags[index].rgb);
}

QStringList flagIds(quint32 flags)
{
    QStringList ids;
    for (int i = 0; i < int(std::size(kFlags)); ++i) {
        if (flags & (1u << i))
            ids.append(QLatin1String(kFlags[i].id));
    }
    return ids;
}

quint32 flagsFromIds(const QStringList& ids)
{
    quint32 flags = 0;
    for (const QString& id : ids) {
        if (const int i = flagIndex(id); i >= 0)
            flags |= 1u << i;
    }
    return flags;
}

int flagCount(quint32 flags)
{
    return std::popcount(flags);
}

} // namespace kk
