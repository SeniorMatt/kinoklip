// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshot mode: where things are in the saved picture, for annotated documentation shots.
#pragma once

#include <QPoint>
#include <QString>

#include <cstdint>

class QWidget;

namespace kk {

struct TimelineData;

/// Writes a JSON sidecar describing the visible parts of @p root as they appear in its grab
/// (scripts/guide_screenshots.py draws its red boxes from it, so annotations follow the layout).
/// @p offset is added to every rectangle (the root's position inside the saved picture).
/// Entries, each with a rect `[x, y, w, h]`:
/// - `widget`: every visible widget with an object name, a button with an action (the
///   action id, `action`) or a text (`text`), and every panel (`class` ending in "Panel");
/// - `tab`: each tab of a visible tab bar (`text`);
/// - `item`: each visible row of an item view (`text`, and `data` = its Qt::UserRole string,
///   e.g. the effect id in the Effects library);
/// - `clip`: each clip of @p timeline drawn by the main timeline widget (`track` "V1", "A2", "ST1",
///   `index` on its track, `text` = clip name), `transition` (the frames a clip's transition
///   covers; `track`, `index`, `head` true for the transition at its start), `track` (each
///   track header) and `playhead` (a 1 px wide rect).
/// Returns false when the file cannot be written.
bool writeScreenshotGeometry(QWidget* root, const QString& jsonPath, QPoint offset = {},
    const TimelineData* timeline = nullptr, int64_t playhead = -1);

} // namespace kk
