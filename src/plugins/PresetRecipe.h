// SPDX-License-Identifier: GPL-3.0-or-later
// PresetRecipe — a preset of a plugin is a recipe: a list of command-layer calls (the same
// commands the command line and the MCP server use) with placeholders for the clip or the
// playhead it is applied to. This file expands a recipe for one target; running the steps is
// the application's job (CommandRegistry::executeLiveBatch), so presets, scripts, the command
// line and MCP all go through the same commands.
//
//   {"commands": [
//      {"command": "effect.add", "args": {"clip": "$clip", "type": "gaussian_blur",
//                                         "set": ["radius=12"]}},
//      {"command": "keyframe.set", "args": {"clip": "$clip", "effect": "$last_effect",
//                                           "param": "radius", "at": "$start", "value": "0"}}]}
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace kk::plugins {

/// What a recipe is applied to.
struct RecipeTarget
{
    qint64 clip = 0; ///< $clip: clip id (target "clip")
    qint64 start = 0; ///< $start: first timeline frame of the clip
    qint64 end = 0; ///< $end: last timeline frame of the clip
    qint64 duration = 0; ///< $duration: clip length in frames
    qint64 playhead = 0; ///< $playhead: timeline frame of the playhead
    // Target "edit_point": a cut between two clips of one track (either side may be missing).
    // $clip, $start, $end and $duration are those of the right clip (else the left one).
    bool edgePoint = false;
    qint64 left = 0; ///< $left: id of the clip that ends at the edit point (0: none)
    qint64 right = 0; ///< $right: id of the clip that starts at it (0: none)
    qint64 edit = 0; ///< $edit: timeline frame of the edit point (where the right clip starts)
};

/// One call of the command layer.
struct RecipeStep
{
    QString command;
    QJsonObject args;
};

/// The steps of @p recipe for @p target. Placeholders are replaced in every string of the
/// arguments: a string that is exactly "$clip", "$start", "$end", "$duration" or "$playhead",
/// optionally followed by +N or -N ("$end-10"), becomes a number; the same words inside longer
/// text are replaced by the number as text. "$last_effect" is left for the runner (it is the
/// id of the effect the previous steps added); see substituteResult(). An empty list with
/// @p error set when the recipe is malformed.
[[nodiscard]] QVector<RecipeStep> expandRecipe(
    const QJsonObject& recipe, const RecipeTarget& target, QString* error);

/// Replaces "$last_effect" in @p args by @p effectId (a number for an exact match).
[[nodiscard]] QJsonObject substituteResult(const QJsonObject& args, qint64 effectId);

/// True if @p args mention "$last_effect" anywhere.
[[nodiscard]] bool usesLastEffect(const QJsonObject& args);

} // namespace kk::plugins
