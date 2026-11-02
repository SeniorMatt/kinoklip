// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeItemModel — what the Keyframes panel shows for a clip selection: the items (a clip,
// or a linked video + audio pair as one item, spec 24.1) and the property rows grouped like the
// Inspector (master, item, group and parameter rows). Pure function of the timeline data.
#pragma once

#include "core/EffectRegistry.h"
#include "ui/inspector/ParamAccess.h"
#include "ui/keyframes/DopeSheetView.h"

#include <QString>
#include <QVector>

namespace kk {

/// One row of the Keyframes panel (parameter tree on the left, dope sheet row on the right).
struct KeyframeEntry
{
    KeyRow::Kind kind = KeyRow::Kind::Param;
    QString label;
    /// Stable id ("master", "item:<clip>", "group:<clip>/<group id>", "param:<clip>/<path>"):
    /// collapse state and structure comparison.
    QString key;
    QString parentKey; ///< enclosing item / group ("" for top level)
    int depth = 0; ///< indentation level in the tree
    QVector<ParamId> params; ///< parameters summarised by the row (one for Param)
    // ---- Param rows only ----
    ParamRef ref; ///< inspector address (value editing, linked zoom)
    ParamDescriptor desc; ///< range, step, unit
    int colorIndex = -1; ///< curve colour (CurveView::curveColor)

    /// Same row identity and contents (structure comparison; values are not part of it).
    [[nodiscard]] bool sameRow(const KeyframeEntry& o) const
    {
        return key == o.key && label == o.label && params == o.params;
    }
};

/// The model of the panel for one selection.
struct KeyframeItems
{
    /// Items: the clip ids of each (video members first, then audio).
    QVector<QVector<quint64>> items;
    /// Every row (also the ones under collapsed groups), in display order.
    QVector<KeyframeEntry> entries;

    /// Every clip of every item, in item order.
    [[nodiscard]] QVector<quint64> allClips() const;
    /// Display name of the first item ("" without items).
    [[nodiscard]] QString firstItemName(const TimelineData& data) const;
};

/// Pure functions building and filtering the panel model.
namespace keyframeitems {

/// Maximum number of items shown at once (a big selection would make an unreadable sheet).
inline constexpr int kMaxItems = 8;

/// Builds the model for the selected clips @p selectedClips (selection order). A clip and the
/// clips linked to it form one item. With @p showAll every animatable parameter of the
/// Inspector is listed (to key a new one), else only parameters that have keys.
/// Video members contribute Transform, Cropping, Title (title clips) and their effects; audio
/// members the Audio group and their effects.
[[nodiscard]] KeyframeItems build(
    const TimelineData& data, const QVector<quint64>& selectedClips, bool showAll);

/// Entries not hidden under a collapsed item / group (@p collapsed holds entry keys).
[[nodiscard]] QVector<int> visibleRows(
    const QVector<KeyframeEntry>& entries, const QSet<QString>& collapsed);

} // namespace keyframeitems

} // namespace kk
