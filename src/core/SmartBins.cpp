// SPDX-License-Identifier: GPL-3.0-or-later
// Smart bin rule evaluation and catalogue.
#include "core/SmartBins.h"

#include "core/ClipColors.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QFileInfo>
#include <QRegularExpression>

#include <cmath>

namespace kk::smartbins {

namespace {

const char* const kContext = "kk::SmartBins";

QString tr(const char* text)
{
    return QCoreApplication::translate(kContext, text);
}

constexpr qint64 kMsPerDay = 24LL * 60 * 60 * 1000;
constexpr double kFpsTolerance = 0.01; ///< "29.97" equals 30000/1001
constexpr double kSecondsTolerance = 0.05; ///< durations typed with two decimals

struct FieldInfo
{
    SmartField field;
    const char* id;
    const char* label;
    ValueKind kind;
};

// clang-format off
const FieldInfo kFields[] = {
    {SmartField::Name,         "name",         QT_TRANSLATE_NOOP("kk::SmartBins", "Clip Name"),     ValueKind::Text},
    {SmartField::FileName,     "fileName",     QT_TRANSLATE_NOOP("kk::SmartBins", "File Name"),     ValueKind::Text},
    {SmartField::Path,         "path",         QT_TRANSLATE_NOOP("kk::SmartBins", "File Path"),     ValueKind::Text},
    {SmartField::Type,         "type",         QT_TRANSLATE_NOOP("kk::SmartBins", "Type"),          ValueKind::Choice},
    {SmartField::Codec,        "codec",        QT_TRANSLATE_NOOP("kk::SmartBins", "Codec"),         ValueKind::Text},
    {SmartField::Resolution,   "resolution",   QT_TRANSLATE_NOOP("kk::SmartBins", "Resolution"),    ValueKind::Resolution},
    {SmartField::FrameRate,    "frameRate",    QT_TRANSLATE_NOOP("kk::SmartBins", "Frame Rate"),    ValueKind::Number},
    {SmartField::Duration,     "duration",     QT_TRANSLATE_NOOP("kk::SmartBins", "Duration (seconds)"), ValueKind::Number},
    {SmartField::DateAdded,    "dateAdded",    QT_TRANSLATE_NOOP("kk::SmartBins", "Date Added"),    ValueKind::Date},
    {SmartField::DateModified, "dateModified", QT_TRANSLATE_NOOP("kk::SmartBins", "Date Modified"), ValueKind::Date},
    {SmartField::ClipColor,    "clipColor",    QT_TRANSLATE_NOOP("kk::SmartBins", "Clip Color"),    ValueKind::Choice},
    {SmartField::Flags,        "flags",        QT_TRANSLATE_NOOP("kk::SmartBins", "Flags"),         ValueKind::Flags},
    {SmartField::Keywords,     "keywords",     QT_TRANSLATE_NOOP("kk::SmartBins", "Keywords"),      ValueKind::Keywords},
    {SmartField::Comments,     "comments",     QT_TRANSLATE_NOOP("kk::SmartBins", "Comments"),      ValueKind::Text},
    {SmartField::Description,  "description",  QT_TRANSLATE_NOOP("kk::SmartBins", "Description"),   ValueKind::Text},
    {SmartField::Usage,        "usage",        QT_TRANSLATE_NOOP("kk::SmartBins", "Usage"),         ValueKind::Choice},
};

const std::pair<SmartOp, const char*> kOps[] = {
    {SmartOp::Contains,    "contains"},
    {SmartOp::NotContains, "notContains"},
    {SmartOp::Is,          "is"},
    {SmartOp::IsNot,       "isNot"},
    {SmartOp::StartsWith,  "startsWith"},
    {SmartOp::EndsWith,    "endsWith"},
    {SmartOp::Greater,     "greater"},
    {SmartOp::Less,        "less"},
    {SmartOp::Before,      "before"},
    {SmartOp::After,       "after"},
    {SmartOp::InLast,      "inLast"},
    {SmartOp::Has,         "has"},
    {SmartOp::HasNot,      "hasNot"},
};
// clang-format on

const FieldInfo& info(SmartField field)
{
    for (const FieldInfo& f : kFields) {
        if (f.field == field)
            return f;
    }
    return kFields[0];
}

/// Text value of a text field.
QString textOf(SmartField field, const MediaItem& item)
{
    switch (field) {
    case SmartField::Name:
        return item.name;
    case SmartField::FileName:
        return item.isTimeline() ? QString() : QFileInfo(item.path).fileName();
    case SmartField::Path:
        return item.isTimeline() ? QString() : item.path;
    case SmartField::Codec:
        return item.info.hasVideo ? item.info.videoCodec : item.info.audioCodec;
    case SmartField::Comments:
        return item.metadata.comments;
    case SmartField::Description:
        return item.metadata.description;
    default:
        return {};
    }
}

bool textMatches(SmartOp op, const QString& text, const QString& value)
{
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
    switch (op) {
    case SmartOp::Contains:
        return text.contains(value, cs);
    case SmartOp::NotContains:
        return !text.contains(value, cs);
    case SmartOp::Is:
        return text.compare(value, cs) == 0;
    case SmartOp::IsNot:
        return text.compare(value, cs) != 0;
    case SmartOp::StartsWith:
        return text.startsWith(value, cs);
    case SmartOp::EndsWith:
        return text.endsWith(value, cs);
    default:
        return false;
    }
}

bool parseNumber(const QString& text, double* out)
{
    bool ok = false;
    *out = QString(text).replace(QLatin1Char(','), QLatin1Char('.')).trimmed().toDouble(&ok);
    return ok && std::isfinite(*out);
}

bool numberMatches(SmartOp op, double actual, double value, double tolerance)
{
    switch (op) {
    case SmartOp::Is:
        return std::abs(actual - value) <= tolerance;
    case SmartOp::IsNot:
        return std::abs(actual - value) > tolerance;
    case SmartOp::Greater:
        return actual > value + tolerance;
    case SmartOp::Less:
        return actual < value - tolerance;
    default:
        return false;
    }
}

/// "1920x1080", "1920 x 1080", "1920×1080".
bool parseResolution(const QString& text, int* w, int* h)
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*(\\d+)\\s*[xX\\x{00D7}]\\s*(\\d+)\\s*$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return false;
    *w = m.captured(1).toInt();
    *h = m.captured(2).toInt();
    return true;
}

bool resolutionMatches(SmartOp op, const MediaItem& item, const QString& value)
{
    int w = 0;
    int h = 0;
    if (!parseResolution(value, &w, &h))
        return false;
    const int iw = item.info.hasVideo ? item.info.width : 0;
    const int ih = item.info.hasVideo ? item.info.height : 0;
    const bool same = iw == w && ih == h;
    switch (op) {
    case SmartOp::Is:
        return same;
    case SmartOp::IsNot:
        return !same;
    case SmartOp::Greater:
        return iw > 0 && qint64(iw) * ih > qint64(w) * h;
    case SmartOp::Less:
        return iw > 0 && qint64(iw) * ih < qint64(w) * h;
    default:
        return false;
    }
}

bool dateMatches(SmartOp op, qint64 ms, const QString& value, qint64 nowMs)
{
    if (op == SmartOp::InLast) {
        double days = 0;
        if (ms <= 0 || !parseNumber(value, &days) || days < 0)
            return false;
        return nowMs - ms <= qint64(days * kMsPerDay) && ms <= nowMs + kMsPerDay;
    }
    const QDate date = QDate::fromString(value.trimmed(), Qt::ISODate);
    if (!date.isValid())
        return false;
    if (ms <= 0)
        return false; // unknown dates match no comparison
    // Calendar days in local time: what the user sees in the date column.
    const QDate itemDate = QDateTime::fromMSecsSinceEpoch(ms).date();
    switch (op) {
    case SmartOp::Is:
        return itemDate == date;
    case SmartOp::Before:
        return itemDate < date;
    case SmartOp::After:
        return itemDate > date;
    default:
        return false;
    }
}

bool keywordsMatch(SmartOp op, const QStringList& keywords, const QString& value)
{
    const QString v = value.trimmed();
    switch (op) {
    case SmartOp::Has:
        return keywords.contains(v, Qt::CaseInsensitive);
    case SmartOp::HasNot:
        return !keywords.contains(v, Qt::CaseInsensitive);
    case SmartOp::Contains:
        return std::any_of(keywords.cbegin(), keywords.cend(),
            [&](const QString& k) { return k.contains(v, Qt::CaseInsensitive); });
    case SmartOp::NotContains:
        return std::none_of(keywords.cbegin(), keywords.cend(),
            [&](const QString& k) { return k.contains(v, Qt::CaseInsensitive); });
    default:
        return false;
    }
}

bool choiceMatches(SmartOp op, const QString& actual, const QString& value)
{
    if (op == SmartOp::Is)
        return actual == value;
    if (op == SmartOp::IsNot)
        return actual != value;
    return false;
}

} // namespace

QVector<SmartField> fields()
{
    QVector<SmartField> out;
    for (const FieldInfo& f : kFields)
        out.append(f.field);
    return out;
}

ValueKind valueKind(SmartField field)
{
    return info(field).kind;
}

QVector<SmartOp> operatorsFor(SmartField field)
{
    using O = SmartOp;
    switch (valueKind(field)) {
    case ValueKind::Text:
        return {O::Contains, O::NotContains, O::Is, O::IsNot, O::StartsWith, O::EndsWith};
    case ValueKind::Choice:
        return {O::Is, O::IsNot};
    case ValueKind::Number:
    case ValueKind::Resolution:
        return {O::Is, O::IsNot, O::Greater, O::Less};
    case ValueKind::Date:
        return {O::InLast, O::Is, O::Before, O::After};
    case ValueKind::Flags:
        return {O::Has, O::HasNot};
    case ValueKind::Keywords:
        return {O::Has, O::HasNot, O::Contains, O::NotContains};
    }
    return {};
}

QVector<std::pair<QString, QString>> choicesFor(SmartField field)
{
    QVector<std::pair<QString, QString>> out;
    switch (field) {
    case SmartField::Type:
        out = {{QStringLiteral("video"), tr("Video")}, {QStringLiteral("audio"), tr("Audio")},
            {QStringLiteral("still"), tr("Still")}, {QStringLiteral("timeline"), tr("Timeline")},
            {QStringLiteral("compound"), tr("Compound Clip")},
            {QStringLiteral("multicam"), tr("Multicam Clip")}};
        break;
    case SmartField::ClipColor:
        out.append({QStringLiteral("none"), tr("None")});
        for (const PaletteColor& c : clipColorPalette())
            out.append({QLatin1String(c.id), clipColorName(QLatin1String(c.id))});
        break;
    case SmartField::Flags:
        out.append({QStringLiteral("any"), tr("Any Flag")});
        for (int i = 0; i < int(flagPalette().size()); ++i)
            out.append({QLatin1String(flagPalette()[i].id), flagName(i)});
        break;
    case SmartField::Usage:
        out = {{QStringLiteral("used"), tr("Used in a timeline")},
            {QStringLiteral("unused"), tr("Not used in any timeline")}};
        break;
    default:
        break;
    }
    return out;
}

QString fieldLabel(SmartField field)
{
    return tr(info(field).label);
}

QString operatorLabel(SmartOp op)
{
    switch (op) {
    case SmartOp::Contains:
        return tr("contains");
    case SmartOp::NotContains:
        return tr("does not contain");
    case SmartOp::Is:
        return tr("is");
    case SmartOp::IsNot:
        return tr("is not");
    case SmartOp::StartsWith:
        return tr("starts with");
    case SmartOp::EndsWith:
        return tr("ends with");
    case SmartOp::Greater:
        return tr("is greater than");
    case SmartOp::Less:
        return tr("is less than");
    case SmartOp::Before:
        return tr("is before");
    case SmartOp::After:
        return tr("is after");
    case SmartOp::InLast:
        return tr("is in the last (days)");
    case SmartOp::Has:
        return tr("has");
    case SmartOp::HasNot:
        return tr("does not have");
    }
    return {};
}

QString fieldId(SmartField field)
{
    return QLatin1String(info(field).id);
}

QString operatorId(SmartOp op)
{
    for (const auto& [o, id] : kOps) {
        if (o == op)
            return QLatin1String(id);
    }
    return {};
}

bool fieldFromId(const QString& id, SmartField* field)
{
    for (const FieldInfo& f : kFields) {
        if (id == QLatin1String(f.id)) {
            *field = f.field;
            return true;
        }
    }
    return false;
}

bool operatorFromId(const QString& id, SmartOp* op)
{
    for (const auto& [o, name] : kOps) {
        if (id == QLatin1String(name)) {
            *op = o;
            return true;
        }
    }
    return false;
}

QString itemType(const MediaItem& item)
{
    switch (item.kind) {
    case MediaItemKind::Timeline:
        return QStringLiteral("timeline");
    case MediaItemKind::CompoundClip:
        return QStringLiteral("compound");
    case MediaItemKind::File:
        break;
    }
    if (item.info.isStillImage)
        return QStringLiteral("still");
    return item.info.hasVideo ? QStringLiteral("video") : QStringLiteral("audio");
}

bool ruleMatches(const SmartRule& rule, const MediaItem& item, const Context& ctx)
{
    if (!operatorsFor(rule.field).contains(rule.op))
        return false;
    switch (rule.field) {
    case SmartField::Name:
    case SmartField::FileName:
    case SmartField::Path:
    case SmartField::Codec:
    case SmartField::Comments:
    case SmartField::Description:
        return textMatches(rule.op, textOf(rule.field, item), rule.value);
    case SmartField::Type:
        return choiceMatches(rule.op, itemType(item), rule.value);
    case SmartField::Resolution:
        return resolutionMatches(rule.op, item, rule.value);
    case SmartField::FrameRate: {
        double v = 0;
        if (!parseNumber(rule.value, &v))
            return false;
        const double fps = item.info.frameRate.isValid() && !item.info.isStillImage
            ? item.info.frameRate.toDouble()
            : 0.0;
        return numberMatches(rule.op, fps, v, kFpsTolerance);
    }
    case SmartField::Duration: {
        double v = 0;
        if (!parseNumber(rule.value, &v))
            return false;
        return numberMatches(rule.op, item.info.durationUs / 1e6, v, kSecondsTolerance);
    }
    case SmartField::DateAdded:
    case SmartField::DateModified: {
        const qint64 now = ctx.nowMs > 0 ? ctx.nowMs : QDateTime::currentMSecsSinceEpoch();
        return dateMatches(rule.op,
            rule.field == SmartField::DateAdded ? item.addedMs : item.fileModifiedMs, rule.value,
            now);
    }
    case SmartField::ClipColor:
        return choiceMatches(rule.op,
            findClipColor(item.clipColor) ? item.clipColor : QStringLiteral("none"), rule.value);
    case SmartField::Flags: {
        const bool any = rule.value == QLatin1String("any");
        const int index = flagIndex(rule.value);
        if (!any && index < 0)
            return false;
        const bool has = any ? item.flags != 0 : (item.flags & (1u << index)) != 0;
        return rule.op == SmartOp::Has ? has : !has;
    }
    case SmartField::Keywords:
        return keywordsMatch(rule.op, item.metadata.keywords, rule.value);
    case SmartField::Usage: {
        const bool used = ctx.usage && ctx.usage->value(item.id, 0) > 0;
        return choiceMatches(
            rule.op, used ? QStringLiteral("used") : QStringLiteral("unused"), rule.value);
    }
    }
    return false;
}

bool matches(const SmartBin& bin, const MediaItem& item, const Context& ctx)
{
    if (bin.rules.isEmpty())
        return false;
    for (const SmartRule& rule : bin.rules) {
        const bool ok = ruleMatches(rule, item, ctx);
        if (bin.matchAll && !ok)
            return false;
        if (!bin.matchAll && ok)
            return true;
    }
    return bin.matchAll;
}

QVector<SmartBin> defaultSmartBins()
{
    auto make = [](const char* key, const QString& name, const char* type) {
        SmartBin b;
        b.id = QLatin1String(kDefaultIdPrefix) + QLatin1String(key);
        b.name = name;
        b.rules = {SmartRule {SmartField::Type, SmartOp::Is, QLatin1String(type)}};
        return b;
    };
    return {make("video", tr("Video"), "video"), make("audio", tr("Audio"), "audio"),
        make("stills", tr("Stills"), "still"), make("timelines", tr("Timelines"), "timeline")};
}

} // namespace kk::smartbins
