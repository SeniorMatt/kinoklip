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

} // namespace kk::timelinedialogs
