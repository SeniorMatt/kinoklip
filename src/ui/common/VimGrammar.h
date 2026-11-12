// SPDX-License-Identifier: GPL-3.0-or-later
// VimGrammar — the motions and text objects of the timeline (spec section 83), as pure
// functions over the questions a VimHost answers. No widgets, no editing.
//
// Time is the line and clips are the words: `w` goes to the start of the next clip on the
// cursor track, `e` to the last frame of the clip, `b` back; `W B E` look at the edit points of
// every track. `j k gg G` move the cursor between tracks. The Neovim documents (motion.txt) are
// the model for exclusive / inclusive / linewise motions and for the text objects.
#pragma once

#include "ui/common/VimHost.h"

#include <QString>

#include <optional>

namespace kk::vimgrammar {

struct Motion
{
    enum class Type {
        Exclusive, ///< the range ends before the target frame
        Inclusive, ///< the range includes the target frame
        Lines ///< works on tracks: the clips under the cursor frame on the tracks passed over
    };
    VimPos to;
    Type type = Type::Exclusive;
    bool allTracks = false; ///< with an operator: every track, not only the cursor track
};

/// True for a motion this module knows (`vim:<name>`).
[[nodiscard]] bool isMotion(const QString& name);
/// True for a text object this module knows.
[[nodiscard]] bool isObject(const QString& name);

/// Where the motion @p name leads from @p from; std::nullopt when it cannot move (no clip,
/// no marker ...). @p hasCount tells whether the count was typed (`G` without a count goes
/// to the last track, `5G` to the fifth).
/// @p arg is the typed character of `find_f find_F find_t find_T`.
[[nodiscard]] std::optional<Motion> motion(const VimHost& host, const QString& name, VimPos from,
    int count, bool hasCount, const QString& arg = QString());

/// `f F t T` ( @p kind ): the clip of the cursor track whose name starts with @p ch (any case),
/// the @p count th one after (f t) or before (F T) @p from. f lands on its first frame, t on the
/// frame before it, F on the first frame of the one before, T on the frame after the end of
/// the one before. @p again is `;` / `,`: t and T skip a clip they already stand next to.
/// std::nullopt when there is no such clip. The motions are exclusive, except t, which is
/// inclusive: with an operator, `f` and `t` both leave the target clip alone.
[[nodiscard]] std::optional<Motion> findClip(const VimHost& host, QChar kind, QChar ch,
    VimPos from, int count, bool again);

/// The range an operator gets from a motion typed at @p from. Empty when nothing is covered.
[[nodiscard]] VimRange rangeOfMotion(const VimHost& host, VimPos from, const Motion& motion);

/// The range of the text object @p name at @p at; std::nullopt when there is no such object.
[[nodiscard]] std::optional<VimRange> object(
    const VimHost& host, const QString& name, VimPos at, int count);

/// The clip of track @p track covering @p frame, or nullptr-like empty span (id 0).
[[nodiscard]] VimClipSpan clipAt(const VimHost& host, int track, int64_t frame);

/// The clips under @p frame on the tracks @p firstTrack .. @p lastTrack (either order).
[[nodiscard]] QVector<quint64> clipsUnder(
    const VimHost& host, int firstTrack, int lastTrack, int64_t frame);

/// The clips of tracks @p firstTrack .. @p lastTrack that touch the frames [start, end).
[[nodiscard]] QVector<quint64> clipsTouching(
    const VimHost& host, int firstTrack, int lastTrack, int64_t start, int64_t end);

/// Sorted, unique clip starts and ends of every track.
[[nodiscard]] QVector<int64_t> editPoints(const VimHost& host);

} // namespace kk::vimgrammar
