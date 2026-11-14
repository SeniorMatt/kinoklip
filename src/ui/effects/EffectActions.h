// SPDX-License-Identifier: GPL-3.0-or-later
// EffectActions — undoable "apply this descriptor" operations shared by the effects library
// (double-click), the inspector (Add Effect, drops) and, if it wants, the timeline (drops).
#pragma once

#include "core/EffectRegistry.h"

#include <QSizeF>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>

namespace kk {

class Project;
struct AppContext;

namespace effectactions {

/// Default transition length: the project's standard transition duration (Project Settings;
/// at least one frame).
[[nodiscard]] int64_t defaultTransitionFrames(const Project& project);
/// Default length of a new title / generator / adjustment clip: Preferences > Editing >
/// generator duration (EditBehaviour::generatorSeconds, 5 s by default) at the timeline rate.
[[nodiscard]] int64_t defaultGeneratorFrames(Rational fps);

/// A new clip (no id, start 0, default length) for a Title / Generator / Effects descriptor:
/// a Title, Solid, Generator or Adjustment clip with the descriptor's default parameters.
/// std::nullopt for other descriptors.
/// Region Blur (generators::RegionBlur) is an adjustment clip with a strong Gaussian Blur and
/// a centred rectangle mask a third of @p frameSize (timeline px) wide and high; their ids are
/// 0 and are assigned when the clip is placed (editops).
[[nodiscard]] std::optional<ClipData> makeGeneratorClip(
    const QString& descriptorId, Rational fps, QSizeF frameSize = QSizeF(1920, 1080));

/// True if an effect of @p category can be applied to a clip on a track of @p type
/// (video effects on video tracks, audio effects on audio tracks).
[[nodiscard]] bool effectFitsTrack(EffectCategory category, TrackType type);

/// True if a transition of @p category can sit on a clip on a track of @p type: video
/// transitions on video tracks, audio transitions (EffectCategory::AudioTransition) on audio
/// tracks. Other categories never fit.
[[nodiscard]] bool transitionFitsTrack(EffectCategory category, TrackType type);

/// The project's standard transition for a track of @p type (Project Settings; Cross Dissolve
/// on video and Cross Fade +3 dB on audio unless changed).
[[nodiscard]] QString defaultTransitionId(const Project& project, TrackType type);

/// The clips an effect added "to @p anchor" goes onto (owner request 1.8.1): when @p anchor is
/// one of several @p selected clips, all of them plus their linked partners (applyEffect() keeps
/// those on a fitting, unlocked track); else just @p anchor.
[[nodiscard]] QVector<quint64> effectTargets(
    const Project& project, const QVector<quint64>& selected, quint64 anchor);

/// Appends a new instance of effect @p descriptorId to every clip in @p clipIds whose track
/// type fits the effect and is not locked. One undo step. Returns the number of clips changed (0 = nothing done,
/// e.g. unknown id or no fitting clip).
int applyEffect(Project& project, const QVector<quint64>& clipIds, const QString& descriptorId);

/// Sets transition @p descriptorId on the head (@p atHead) or tail of clip @p clipId, with
/// @p durationFrames (< 1 = default, always clamped to the clip length). Replaces an existing
/// transition there but keeps its length when @p durationFrames < 1. One undo step.
/// Returns false if the clip or descriptor is unknown, or the transition does not fit the
/// clip's track (transitionFitsTrack()).
bool applyTransition(Project& project, quint64 clipId, const QString& descriptorId, bool atHead,
    int64_t durationFrames = -1);

/// Removes the head (@p atHead) or tail transition of clip @p clipId. One undo step.
bool removeTransition(Project& project, quint64 clipId, bool atHead);

/// Creates a title, generator or adjustment clip (descriptor category Title, Generator or
/// Effects) starting at
/// timeline frame @p frame, placed on the lowest video track above everything that already
/// occupies that range (a new track is added if needed). One undo step.
/// Returns the new clip id, or 0 on failure.
/// @p videoTrack >= 0 puts the clip on that video track (0 = V1; anything under it is overwritten;
/// 0 is returned for a track that does not exist or is locked) instead of the track above
/// everything busy. @p prepare, when set, may change the new clip (its parameters) before it is
/// placed, so the whole creation stays one undo step (the commands use it for title text).
quint64 addGeneratorClip(Project& project, const QString& descriptorId, int64_t frame,
    int64_t durationFrames = -1, int videoTrack = -1,
    const std::function<void(ClipData&)>& prepare = {});

/// "Double-click in the effects library": effects go onto the selected clips, transitions onto
/// the nearer end of the current clip, titles/generators are added at the playhead (and the
/// new clip becomes the selection). Returns true if something changed.
bool activate(AppContext& ctx, const QString& descriptorId);

} // namespace effectactions

} // namespace kk
