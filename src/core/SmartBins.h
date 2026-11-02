// SPDX-License-Identifier: GPL-3.0-or-later
// Smart bins: rule evaluation against media pool items, field / operator catalogue, stable ids
// for project files and the default smart bins (Video, Audio, Stills, Timelines).
#pragma once

#include "core/MediaItem.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace kk::smartbins {

/// Kind of value a field holds; decides the operators and the value editor.
enum class ValueKind {
    Text,
    Choice, ///< one of choicesFor() (type, clip colour, usage)
    Number, ///< frame rate, duration in seconds
    Resolution, ///< "WIDTHxHEIGHT"
    Date, ///< "yyyy-MM-dd", or a number of days for InLast
    Flags, ///< a flag id or "any"
    Keywords ///< a keyword (Has / HasNot) or a part of one (Contains / NotContains)
};

/// What a rule needs besides the item.
struct Context
{
    /// Media item id -> number of timeline uses (core/MediaUsage.h); null = everything unused.
    const QHash<QString, int>* usage = nullptr;
    /// "Now" for InLast (ms since the epoch); 0 = QDateTime::currentMSecsSinceEpoch().
    qint64 nowMs = 0;
};

/// Every field, in dialog order.
[[nodiscard]] QVector<SmartField> fields();
/// Value kind of @p field.
[[nodiscard]] ValueKind valueKind(SmartField field);
/// Operators offered for @p field, in menu order (the first one is the default).
[[nodiscard]] QVector<SmartOp> operatorsFor(SmartField field);
/// Choice fields: (value id, translated label) pairs, in menu order.
[[nodiscard]] QVector<std::pair<QString, QString>> choicesFor(SmartField field);

/// Translated field label ("Clip Name", "Frame Rate", ...).
[[nodiscard]] QString fieldLabel(SmartField field);
/// Translated operator label ("contains", "is greater than", ...).
[[nodiscard]] QString operatorLabel(SmartOp op);

/// Stable ids for project files ("name", "frameRate", ...; "contains", "inLast", ...).
[[nodiscard]] QString fieldId(SmartField field);
[[nodiscard]] QString operatorId(SmartOp op);
/// Parse fieldId() / operatorId(); false for an unknown id.
[[nodiscard]] bool fieldFromId(const QString& id, SmartField* field);
[[nodiscard]] bool operatorFromId(const QString& id, SmartOp* op);

/// Type value of @p item for SmartField::Type: "video", "audio", "still", "timeline",
/// "compound" or "multicam".
[[nodiscard]] QString itemType(const MediaItem& item);

/// True if @p item satisfies @p rule. Text comparisons ignore case; unknown dates never match
/// (except "is not" style negations); an operator the field does not offer never matches.
[[nodiscard]] bool ruleMatches(const SmartRule& rule, const MediaItem& item, const Context& ctx);
/// True if @p item belongs in @p bin: all (or any, per SmartBin::matchAll) rules match. A smart
/// bin without rules shows nothing.
[[nodiscard]] bool matches(const SmartBin& bin, const MediaItem& item, const Context& ctx);

/// Id prefix of the default smart bins ("default.video", ...); never stored in a project.
inline constexpr char kDefaultIdPrefix[] = "default.";
/// The default smart bins (Preferences > Editing: "Show default smart bins"): Video, Audio,
/// Stills, Timelines, each one Type rule.
[[nodiscard]] QVector<SmartBin> defaultSmartBins();

} // namespace kk::smartbins
