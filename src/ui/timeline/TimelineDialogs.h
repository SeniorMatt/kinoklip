// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineDialogs — the small modal questions of the timeline commands: insufficient
// transition handles (spec 13.7), the marker editor (18.5) and paste attributes (16.4).
#pragma once

#include "core/TimelineData.h"
#include "ui/timeline/TimelineController.h"

#include <QColor>
#include <QList>
#include <QString>

#include <optional>
#include <utility>

class QWidget;

namespace kk {
class Project;
}

namespace kk::timelinedialogs {

/// The marker colours offered by the marker editor (name, colour), the usual NLE palette.
[[nodiscard]] QList<std::pair<QString, QColor>> markerColors();

/// "Not enough media for the transition": Trim Clips / Shorten Transition (if @p canShorten)
/// / Cancel. @p missingFrames is shown in the message.
[[nodiscard]] HandleChoice askHandles(QWidget* parent, int64_t missingFrames, bool canShorten);

/// Marker editor: name, colour and note of @p marker. Returns true on OK (marker updated).
bool editMarker(QWidget* parent, Marker& marker, bool clipMarker);

/// Paste attributes: check boxes per attribute group. std::nullopt when cancelled.
[[nodiscard]] std::optional<PasteAttributeSet> askPasteAttributes(QWidget* parent);

/// A name question in Resolve's style (New Compound Clip, New Timeline, Rename): one field
/// prefilled with @p suggestion and selected, Cancel / @p okText. std::nullopt when cancelled.
[[nodiscard]] std::optional<QString> askName(QWidget* parent, const QString& title,
    const QString& label, const QString& suggestion, const QString& okText);

/// Timeline Settings of timeline @p uid: use the project settings, or its own resolution and
/// frame rate. Applies the change (one undo step) on OK.
void editTimelineSettings(QWidget* parent, Project& project, quint64 uid);

} // namespace kk::timelinedialogs
