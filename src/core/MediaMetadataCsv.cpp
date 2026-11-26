// SPDX-License-Identifier: GPL-3.0-or-later
// MediaMetadataCsv implementation.
#include "core/MediaMetadataCsv.h"

#include "core/ClipColors.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <algorithm>

namespace kk::mediametadata {

namespace {

const QString kFileName = QStringLiteral("File Name");
const QString kDirectory = QStringLiteral("Clip Directory");
const QString kDescription = QStringLiteral("Description");
const QString kComments = QStringLiteral("Comments");
const QString kKeywords = QStringLiteral("Keywords");
const QString kScene = QStringLiteral("Scene");
const QString kShot = QStringLiteral("Shot");
const QString kTake = QStringLiteral("Take");
const QString kCamera = QStringLiteral("Camera #");
const QString kAngle = QStringLiteral("Angle");
const QString kGoodTake = QStringLiteral("Good Take");
const QString kRating = QStringLiteral("Rating");
const QString kClipColor = QStringLiteral("Clip Color");
const QString kFlags = QStringLiteral("Flags");

QString csvField(const QString& s)
{
    if (!s.contains(QLatin1Char(',')) && !s.contains(QLatin1Char('"'))
        && !s.contains(QLatin1Char('\n')) && !s.contains(QLatin1Char('\r')))
        return s;
    QString q = s;
    q.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + q + QLatin1Char('"');
}

bool isMediaFile(const MediaItem& item)
{
    return item.kind == MediaItemKind::File;
}

QString cleanDirectory(const QString& dir)
{
    return QDir::cleanPath(dir);
}

bool yes(const QString& text)
{
    const QString t = text.trimmed().toLower();
    return t == QLatin1String("yes") || t == QLatin1String("true") || t == QLatin1String("1")
        || t == QLatin1String("x");
}

/// Clip colour id for a typed name or id ("Orange", "orange"); empty text = none (id empty),
/// @p known false for text that names no colour.
QString colorIdOf(const QString& text, bool* known)
{
    *known = true;
    const QString t = text.trimmed();
    if (t.isEmpty() || t.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0)
        return {};
    for (const PaletteColor& c : clipColorPalette()) {
        if (t.compare(QLatin1String(c.id), Qt::CaseInsensitive) == 0
            || t.compare(QLatin1String(c.name), Qt::CaseInsensitive) == 0
            || t.compare(clipColorName(QLatin1String(c.id)), Qt::CaseInsensitive) == 0)
            return QLatin1String(c.id);
    }
    *known = false;
    return {};
}

quint32 flagsOf(const QString& text)
{
    quint32 bits = 0;
    for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString t = part.trimmed();
        for (size_t i = 0; i < flagPalette().size(); ++i) {
            const PaletteColor& f = flagPalette()[i];
            if (t.compare(QLatin1String(f.id), Qt::CaseInsensitive) == 0
                || t.compare(QLatin1String(f.name), Qt::CaseInsensitive) == 0)
                bits |= 1u << i;
        }
    }
    return bits;
}

QString flagNames(quint32 flags)
{
    QStringList names;
    for (size_t i = 0; i < flagPalette().size(); ++i) {
        if (flags & (1u << i))
            names << QLatin1String(flagPalette()[i].name);
    }
    return names.join(QStringLiteral(", "));
}

QString colorName(const QString& id)
{
    const PaletteColor* c = findClipColor(id);
    return c ? QLatin1String(c->name) : QString();
}

} // namespace

QStringList columnNames()
{
    return {kFileName, kDirectory, kDescription, kComments, kKeywords, kScene, kShot, kTake,
        kCamera, kAngle, kGoodTake, kRating, kClipColor, kFlags};
}

QString toCsv(const MediaPool& pool)
{
    const QString crlf = QStringLiteral("\r\n");
    QString out = columnNames().join(QLatin1Char(',')) + crlf;
    for (const MediaItem& item : pool.items()) {
        if (!isMediaFile(item))
            continue;
        const QFileInfo info(item.path);
        const MediaMetadata& m = item.metadata;
        const QStringList row {csvField(info.fileName()), csvField(info.absolutePath()),
            csvField(m.description), csvField(m.comments),
            csvField(m.keywords.join(QStringLiteral(", "))), csvField(m.scene), csvField(m.shot),
            csvField(m.take), csvField(m.camera), csvField(m.angle),
            m.goodTake ? QStringLiteral("Yes") : QStringLiteral("No"),
            m.rating > 0 ? QString::number(m.rating) : QString(), csvField(colorName(item.clipColor)),
            csvField(flagNames(item.flags))};
        out += row.join(QLatin1Char(',')) + crlf;
    }
    return out;
}

QList<QStringList> parseCsv(const QString& textIn)
{
    QString text = textIn;
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    QList<QStringList> records;
    QStringList record;
    QString field;
    bool quoted = false;
    bool fieldStarted = false; // a record that is only a line break stays out
    auto endField = [&] {
        record << field;
        field.clear();
        fieldStarted = false;
    };
    auto endRecord = [&] {
        if (fieldStarted || !record.isEmpty()) {
            endField();
            records << record;
        }
        record.clear();
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < text.size() && text[i + 1] == QLatin1Char('"')) {
                    field += QLatin1Char('"');
                    ++i;
                } else
                    quoted = false;
            } else
                field += c;
        } else if (c == QLatin1Char('"')) {
            quoted = true;
            fieldStarted = true;
        } else if (c == QLatin1Char(',')) {
            fieldStarted = true;
            endField();
        } else if (c == QLatin1Char('\r') || c == QLatin1Char('\n')) {
            if (c == QLatin1Char('\r') && i + 1 < text.size() && text[i + 1] == QLatin1Char('\n'))
                ++i;
            endRecord();
        } else {
            field += c;
            fieldStarted = true;
        }
    }
    endRecord();
    return records;
}

ImportReport applyCsv(MediaPool& pool, const QString& csv)
{
    ImportReport report;
    const QList<QStringList> records = parseCsv(csv);
    if (records.isEmpty()) {
        report.error = QStringLiteral("The file is empty.");
        return report;
    }
    QHash<QString, int> column; // lower-case header -> index
    for (int i = 0; i < records[0].size(); ++i)
        column.insert(records[0][i].trimmed().toLower(), i);
    const int nameCol = column.value(kFileName.toLower(), -1);
    if (nameCol < 0) {
        report.error = QStringLiteral("The file has no \"File Name\" column.");
        return report;
    }
    const int dirCol = column.value(kDirectory.toLower(), -1);
    auto cell = [&](const QStringList& row, const QString& name, bool* present) {
        const int i = column.value(name.toLower(), -1);
        *present = i >= 0 && i < row.size() && !row[i].trimmed().isEmpty();
        return *present ? row[i] : QString();
    };

    for (int r = 1; r < records.size(); ++r) {
        const QStringList& row = records[r];
        const QString fileName = nameCol < row.size() ? row[nameCol].trimmed() : QString();
        if (fileName.isEmpty())
            continue;
        ++report.rows;
        const QString dir = dirCol >= 0 && dirCol < row.size()
            ? cleanDirectory(row[dirCol].trimmed())
            : QString();
        // Find the clip: file name, and the folder when the file names one.
        QList<const MediaItem*> matches;
        for (const MediaItem& item : pool.items()) {
            if (!isMediaFile(item) || QFileInfo(item.path).fileName() != fileName)
                continue;
            matches << &item;
        }
        if (matches.size() > 1 && !dir.isEmpty()) {
            QList<const MediaItem*> inDir;
            for (const MediaItem* item : matches) {
                if (cleanDirectory(QFileInfo(item->path).absolutePath()) == dir)
                    inDir << item;
            }
            if (!inDir.isEmpty())
                matches = inDir;
        }
        if (matches.isEmpty()) {
            report.unmatched << fileName;
            continue;
        }
        if (matches.size() > 1) {
            report.ambiguous << fileName;
            continue;
        }
        const MediaItem& item = *matches.front();
        const QString id = item.id;
        MediaMetadata md = item.metadata;
        QString colorId = item.clipColor;
        quint32 flags = item.flags;
        bool has = false;
        QString v;
        v = cell(row, kDescription, &has);
        if (has)
            md.description = v.trimmed();
        v = cell(row, kComments, &has);
        if (has)
            md.comments = v;
        v = cell(row, kKeywords, &has);
        if (has)
            md.keywords = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
        v = cell(row, kScene, &has);
        if (has)
            md.scene = v.trimmed();
        v = cell(row, kShot, &has);
        if (has)
            md.shot = v.trimmed();
        v = cell(row, kTake, &has);
        if (has)
            md.take = v.trimmed();
        v = cell(row, kCamera, &has);
        if (has)
            md.camera = v.trimmed();
        v = cell(row, kAngle, &has);
        if (has)
            md.angle = v.trimmed();
        v = cell(row, kGoodTake, &has);
        if (has)
            md.goodTake = yes(v);
        v = cell(row, kRating, &has);
        if (has) {
            bool ok = false;
            const int stars = v.trimmed().toInt(&ok);
            if (ok)
                md.rating = std::clamp(stars, 0, 5);
        }
        v = cell(row, kClipColor, &has);
        if (has) {
            bool known = false;
            const QString id2 = colorIdOf(v, &known);
            if (known)
                colorId = id2;
        }
        v = cell(row, kFlags, &has);
        if (has)
            flags = flagsOf(v);

        const MediaItem before = item; // pool.set* below invalidates `item`
        pool.setMetadata(id, md);
        if (colorId != before.clipColor)
            pool.setClipColor(id, colorId);
        if (flags != before.flags)
            pool.setFlags(id, flags);
        const MediaItem* after = pool.item(id);
        if (after && (after->metadata != before.metadata || after->clipColor != before.clipColor
                         || after->flags != before.flags))
            ++report.updated;
        else
            ++report.unchanged;
    }
    return report;
}

} // namespace kk::mediametadata
