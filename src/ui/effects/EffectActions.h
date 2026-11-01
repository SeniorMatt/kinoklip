// SPDX-License-Identifier: GPL-3.0-or-later
// EffectActions — undoable "apply this descriptor" operations shared by the effects library
// (double-click), the inspector (Add Effect, drops) and, if it wants, the timeline (drops).
#pragma once

#include "core/EffectRegistry.h"

#include <QString>
#include <QVector>

#include <cstdint>

namespace kk {

class Project;
struct AppContext;

namespace effectactions {

/// Default transition length: one second at the timeline rate (at least one frame).
[[nodiscard]] int64_t defaultTransitionFrames(Rational fps);
/// Default length of a new title / generator clip: five seconds at the timeline rate.
[[nodiscard]] int64_t defaultGeneratorFrames(Rational fps);

/// True if an effect of @p category can be applied to a clip on a track of @p type
/// (video effects on video tracks, audio effects on audio tracks).
[[nodiscard]] bool effectFitsTrack(EffectCategory category, TrackType type);

/// Appends a new instance of effect @p descriptorId to every clip in @p clipIds whose track
/// type fits the effect. One undo step. Returns the number of clips changed (0 = nothing done,
/// e.g. unknown id or no fitting clip).
int applyEffect(Project& project, const QVector<quint64>& clipIds, const QString& descriptorId);

/// Sets transition @p descriptorId on the head (@p atHead) or tail of clip @p clipId, with
/// @p durationFrames (< 1 = default, always clamped to the clip length). Replaces an existing
/// transition there but keeps its length when @p durationFrames < 1. One undo step.
/// Returns false if the clip or descriptor is unknown.
bool applyTransition(Project& project, quint64 clipId, const QString& descriptorId, bool atHead,
    int64_t durationFrames = -1);

/// Removes the head (@p atHead) or tail transition of clip @p clipId. One undo step.
bool removeTransition(Project& project, quint64 clipId, bool atHead);

/// Creates a title or generator clip (descriptor category Title or Generator) starting at
/// timeline frame @p frame, placed on the lowest video track above everything that already
/// occupies that range (a new track is added if needed). One undo step.
/// Returns the new clip id, or 0 on failure.
quint64 addGeneratorClip(
    Project& project, const QString& descriptorId, int64_t frame, int64_t durationFrames = -1);

/// "Double-click in the effects library": effects go onto the selected clips, transitions onto
/// the nearer end of the current clip, titles/generators are added at the playhead (and the
/// new clip becomes the selection). Returns true if something changed.
bool activate(AppContext& ctx, const QString& descriptorId);

} // namespace effectactions

} // namespace kk
