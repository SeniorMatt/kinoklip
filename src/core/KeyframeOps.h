// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeOps — pure functions on the keyframes of clips: parameter paths, the grouped list of
// animated parameters, navigation, move / delete / interpolation / copy and paste.
#pragma once

#include "core/Animation.h"
#include "core/SelectionModel.h"
#include "core/TimelineData.h"

#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>
#include <vector>

namespace kk::keyframeops {

// ---- Parameter paths ----
//
// A path addresses one AnimatedValue of a clip (SelectionModel's KeyframeRef::paramPath):
//   "transform/<key>", "audio/<key>", "title/<key>", "effect/<instance id>/<key>",
//   "mask/<mask id>/<key>" (MaskData::params) and "mask/<mask id>/shape" (the animated path:
//   its AnimatedPath::timing value; the operations below keep the shapes in step).
//   "retime/retimeFrame" (the retime curve, core/Retime.h; group id "retime", listed last).
//   "grade/<node id>/<key>" (GradeNode::params of the Color page grade, gradekeys::*) and
//   "gradewin/<window id>/<key>" (GradeWindow::params, gradewindowkeys::*).

/// Path of a parameter stored in a clip ParamSet member ("transform", "audio", "title",
/// "retime").
[[nodiscard]] QString memberPath(const QString& member, const QString& key);
/// Path of a parameter of effect instance @p effectId.
[[nodiscard]] QString effectPath(quint64 effectId, const QString& key);
/// Path of a parameter of the extra title layer @p layerId (ClipData::titleLayers).
[[nodiscard]] QString layerPath(quint64 layerId, const QString& key);
/// Path of a parameter of grade node @p nodeId (Color page, gradekeys::*).
[[nodiscard]] QString gradePath(quint64 nodeId, const QString& key);
/// Path of a parameter of grade window @p windowId (gradewindowkeys::*).
[[nodiscard]] QString gradeWindowPath(quint64 windowId, const QString& key);
/// Path of a parameter of mask @p maskId (maskkeys::Shape for the animated shape).
[[nodiscard]] QString maskPath(quint64 maskId, const QString& key);
/// True if @p path addresses a mask shape (its key values are shape indices, not numbers).
[[nodiscard]] bool isShapePath(const QString& path);
/// The mask with id @p maskId of @p clip, or nullptr.
[[nodiscard]] const MaskData* findMask(const ClipData& clip, quint64 maskId);
/// Mutable overload of findMask().
[[nodiscard]] MaskData* findMask(ClipData& clip, quint64 maskId);
/// The animated value at @p path, or nullptr (unknown path, missing effect, key never set).
[[nodiscard]] const AnimatedValue* findValue(const ClipData& clip, const QString& path);
/// Mutable overload of findValue().
[[nodiscard]] AnimatedValue* findValue(ClipData& clip, const QString& path);

/// One animated parameter of a clip as the keyframe and curve editors list it.
struct AnimatedParam
{
    QString path; ///< see above
    QString groupId; ///< "builtin.transform", "builtin.cropping", "builtin.masks", "title",
                     ///< "builtin.audio", or "effect/<instance id>"
    QString groupName; ///< translated group title (descriptor name)
    QString label; ///< translated parameter label (descriptor label, else the key)
    double min = 0.0; ///< descriptor range (curve editor fallback range)
    double max = 1.0;
    bool displayNegated = false; ///< ParamDescriptor::displayNegated (Position Y, shown +up)
};

/// The parameters of @p clip that have keyframes, in inspector order: Transform, Cropping,
/// Masks (one group for all masks, labels "<mask name>: <parameter>"), Title, Audio, then the
/// effects in stack order (each in its descriptor's order). Parameters without a descriptor entry
/// come last in their group.
[[nodiscard]] QVector<AnimatedParam> animatedParams(const ClipData& clip);

/// Sorted, unique clip-local frames of every key of @p clip (the "master" keyframe row).
[[nodiscard]] QVector<int64_t> allKeyFrames(const ClipData& clip);
/// allKeyFrames() without the retime curve: the frames marked on the clip in the timeline, and
/// whether it offers the keyframe and curve toggles (spec 15.8, 49.3). The retime curve has its
/// own speed bar and its Ctrl+Shift+R command; its edge keys would mark every ramped clip.
[[nodiscard]] QVector<int64_t> clipMarkFrames(const ClipData& clip);
/// @p candidates (editops::snapCandidates()) plus the timeline frames of the keys of @p clipIds,
/// sorted and unique, for editops::snapToNearest(): keys are snap targets like clip markers
/// (spec 11.9). Like the marks on a clip, this leaves out the retime curve; keys trimmed out of
/// the clip's visible part and the keys in @p excludeKeys (those being dragged) do not count.
[[nodiscard]] std::vector<int64_t> withKeySnapTargets(std::vector<int64_t> candidates,
    const TimelineData& data, const QVector<quint64>& clipIds,
    const QVector<KeyframeRef>& excludeKeys = {});
/// Nearest key frame of any parameter strictly after (@p forward) or before @p localFrame.
[[nodiscard]] std::optional<int64_t> adjacentKeyFrame(
    const ClipData& clip, int64_t localFrame, bool forward);
/// Every key of @p clip at clip-local @p localFrame (one ref per parameter that has one).
[[nodiscard]] QVector<KeyframeRef> keysAt(const ClipData& clip, int64_t localFrame);
/// Every key of @p clip.
[[nodiscard]] QVector<KeyframeRef> allKeys(const ClipData& clip);
/// Keys of @p clip with frames in [@p fromFrame, @p toFrame] restricted to @p paths (empty =
/// all parameters). Used by rubber-band selection.
[[nodiscard]] QVector<KeyframeRef> keysInRange(
    const ClipData& clip, int64_t fromFrame, int64_t toFrame, const QVector<QString>& paths = {});

// ---- Editing (all take the whole timeline; refs of missing clips/keys are ignored) ----

/// Clamps @p deltaFrames so that the keys @p refs stay inside their clips ([0, duration - 1]),
/// unless a key already lies outside (keys beyond a trimmed edge may stay there).
[[nodiscard]] int64_t clampMoveDelta(
    const TimelineData& data, const QVector<KeyframeRef>& refs, int64_t deltaFrames);
/// Moves keys @p refs by @p deltaFrames. A moved key landing on an unmoved key of the same
/// parameter replaces it. Returns the refs at their new frames (same order, missing dropped).
QVector<KeyframeRef> moveKeys(
    TimelineData& data, const QVector<KeyframeRef>& refs, int64_t deltaFrames);
/// Adds @p deltaValue to the value of every key @p refs (curve editor vertical drag; callers
/// restore the drag origin first, like every timeline drag). Mask shape keys are skipped.
/// Returns the number changed.
int offsetKeyValues(TimelineData& data, const QVector<KeyframeRef>& refs, double deltaValue);
/// Removes keys @p refs. Returns the number removed. A parameter that loses its last key keeps
/// a constant value instead of falling back to its default: the value it showed at timeline
/// frame @p playhead before the deletion (what the Inspector showed; clamped into the clip),
/// or without a playhead the value of the last key removed.
int deleteKeys(TimelineData& data, const QVector<KeyframeRef>& refs,
    std::optional<int64_t> playhead = std::nullopt);
/// Applies preset @p interp to keys @p refs. Returns the number changed.
int setInterp(TimelineData& data, const QVector<KeyframeRef>& refs, Interp interp);
/// "Flatten": makes both handles of keys @p refs horizontal, keeping their lengths (curve
/// editor). Returns the number changed.
int flattenHandles(TimelineData& data, const QVector<KeyframeRef>& refs);

/// Which handle of a key the curve editor drags.
enum class HandleSide {
    In,
    Out
};
/// Sets handle @p side of the key @p ref to the Free geometry (@p dtFrames > 0, @p dv). When
/// @p keepSmooth is set and the key's other handle is not Linear, that handle is made Free too
/// and turned to stay collinear (keeping its length), so the curve stays smooth through the
/// key (hold Alt in the curve editor to break the tangent). Returns false if there is no key.
bool setHandle(TimelineData& data, const KeyframeRef& ref, HandleSide side, double dtFrames,
    double dv, bool keepSmooth);

/// Copied keyframes, relative to the earliest copied key.
struct KeyClipboard
{
    struct Entry
    {
        QString path;
        int64_t offset = 0; ///< frames after the earliest copied key
        Keyframe key; ///< frame field unused (offset is authoritative)
        MaskPath shape; ///< mask shape keys: the path of the key (key.value is meaningless)
    };
    QVector<Entry> entries;
    [[nodiscard]] bool isEmpty() const { return entries.isEmpty(); }
};
/// Copies keys @p refs of one clip (refs of other clips than the first ref's are ignored).
[[nodiscard]] KeyClipboard copyKeys(const TimelineData& data, const QVector<KeyframeRef>& refs);
/// Pastes @p board into clip @p clipId with the earliest key at clip-local @p localFrame.
/// Parameters the clip does not have yet are created (effects and masks only if the instance
/// exists; shape keys only into a mask whose path has the same number of points).
/// Returns the pasted refs.
QVector<KeyframeRef> pasteKeys(
    TimelineData& data, quint64 clipId, const KeyClipboard& board, int64_t localFrame);

} // namespace kk::keyframeops
