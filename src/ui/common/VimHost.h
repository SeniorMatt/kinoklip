// SPDX-License-Identifier: GPL-3.0-or-later
// VimHost — what the Vim layer needs from an editor (spec section 83), and the small value types
// the grammar speaks: positions, ranges and operations on the timeline "buffer".
//
// The model: time is the line (columns are frames), tracks are the lines (top to bottom: the
// video tracks from the highest to V1, then A1, A2, ...), clips are words, gaps are white space.
// The cursor is (playhead frame, cursor track). The Vim machine (VimInput.h) parses keys and
// computes ranges from the queries below; the host only answers questions and carries out
// operations, each as one undo step.
#pragma once

#include <QChar>
#include <QString>
#include <QVector>

class QWidget;

#include <cstdint>
#include <utility>

namespace kk {

/// One clip of a track: its id and the frames [start, end).
struct VimClipSpan
{
    quint64 id = 0;
    int64_t start = 0;
    int64_t end = 0;
};

/// A place in the timeline: a frame on a track (display index, 0 = the top track).
struct VimPos
{
    int64_t frame = 0;
    int track = 0;
    friend bool operator==(const VimPos&, const VimPos&) = default;
};

/// What an operator works on. Frames: the frames [start, end) of the tracks trackFirst ..
/// trackLast (display indexes), or of every track with allTracks; the clips are cut at the
/// edges. Clips: whole clips (ids).
struct VimRange
{
    enum class Kind {
        Frames,
        Clips
    };
    Kind kind = Kind::Frames;
    int64_t start = 0;
    int64_t end = 0;
    int trackFirst = 0;
    int trackLast = 0;
    bool allTracks = false;
    QVector<quint64> clips;

    [[nodiscard]] bool isEmpty() const
    {
        return kind == Kind::Clips ? clips.isEmpty() : end <= start;
    }
};

enum class VimOp {
    Delete, ///< d: ripple delete (later material closes up)
    Change, ///< c: delete leaving a gap, then INSERT
    Lift, ///< X: delete leaving a gap
    Yank ///< y: copy
};

struct VimOperation
{
    VimOp op = VimOp::Delete;
    /// The register: 0 = the unnamed one, 'a'..'z', '0' = the last yank, '_' = the black hole
    /// (nothing is stored).
    QChar reg;
    VimRange range;
};

/// The VISUAL selection as the host shows it.
struct VimVisual
{
    enum class Kind {
        None,
        Char, ///< v: frames on one track
        Line, ///< V: whole clips
        Block ///< <C-v>: frames on several tracks
    };
    Kind kind = Kind::None;
    VimPos anchor;
    VimPos cursor;
};

/// What the vim layer needs from the editor. Implemented for the timeline in
/// ui/timeline/VimTimelineHost.h; tests use fakes. Queries have harmless defaults so a small
/// fake only overrides what its test uses.
class VimHost
{
public:
    virtual ~VimHost() = default;
    /// Triggers the action @p actionId if it exists and is enabled. Returns whether it ran.
    virtual bool trigger(const QString& actionId) = 0;
    [[nodiscard]] virtual int64_t playhead() const = 0;
    virtual void setPlayhead(int64_t frame) = 0;
    virtual void clearSelection() = 0;
    /// `:` was typed.
    virtual void openCommandLine() = 0;
    /// Runs an ex command line (a key bound to ":w<CR>" or "ex:marker add"); the window shows
    /// the result. Default: nothing.
    virtual void runEx(const QString& line) { Q_UNUSED(line); }
    /// `/` was typed. The host opens a search if the editor has one.
    virtual void openSearch() { }

    // ---- The timeline as a buffer (queries) ----

    /// Video and audio tracks together; display index 0 is the top track.
    [[nodiscard]] virtual int trackCount() const { return 1; }
    [[nodiscard]] virtual int cursorTrack() const { return 0; }
    /// Moves the cursor track (also the destination of source edits).
    virtual void setCursorTrack(int index) { Q_UNUSED(index); }
    /// The clips of a track, sorted by start.
    [[nodiscard]] virtual QVector<VimClipSpan> clipSpans(int track) const
    {
        Q_UNUSED(track);
        return {};
    }
    /// Ids of the clips linked to @p clipId, itself included.
    [[nodiscard]] virtual QVector<quint64> linkedClips(quint64 clipId) const { return {clipId}; }
    /// Sorted timeline frames of the markers.
    [[nodiscard]] virtual QVector<int64_t> markerFrames() const { return {}; }
    /// Sorted timeline frames of the keyframes of the clip under the cursor (else of the
    /// selected clip).
    [[nodiscard]] virtual QVector<int64_t> keyframeFrames() const { return {}; }
    /// The end of the last clip on any track.
    [[nodiscard]] virtual int64_t timelineEnd() const { return 0; }
    [[nodiscard]] virtual double framesPerSecond() const { return 25.0; }
    /// The In and Out marks (-1 when not set); out is exclusive.
    [[nodiscard]] virtual std::pair<int64_t, int64_t> markedRange() const { return {-1, -1}; }
    /// True when the track cannot be edited.
    [[nodiscard]] virtual bool trackLocked(int track) const
    {
        Q_UNUSED(track);
        return false;
    }

    /// The clips of the selection (linked ones included when linked selection is on).
    [[nodiscard]] virtual QVector<quint64> selectedClips() const { return {}; }

    // ---- Operations (each one undo step) ----

    /// A Vim command that edits several times (a count, a register paste after a delete) is
    /// one undo step: the machine wraps it in begin / end. Nesting is allowed.
    virtual void beginUndoGroup() { }
    virtual void endUndoGroup() { }

    /// Carries out an operator on a range. False when nothing happened (empty range, locked
    /// tracks).
    virtual bool operate(const VimOperation& operation)
    {
        Q_UNUSED(operation);
        return false;
    }
    /// Pastes register @p reg @p count times at the playhead: inserting (ripple) or
    /// overwriting. False when the register is empty.
    virtual bool pasteRegister(QChar reg, int count, bool insert)
    {
        Q_UNUSED(reg);
        Q_UNUSED(count);
        Q_UNUSED(insert);
        return false;
    }
    /// Whether register @p reg holds something to paste.
    [[nodiscard]] virtual bool registerHasContent(QChar reg) const
    {
        Q_UNUSED(reg);
        return true;
    }
    /// Shows the VISUAL selection (an overlay, and the clips of V mode selected).
    virtual void showVisual(const VimVisual& visual) { Q_UNUSED(visual); }
    /// Selects the clip under the cursor on the cursor track when nothing is selected. Returns
    /// whether it did (then restoreSelection(true) clears it again).
    virtual bool selectCursorClip() { return false; }
    virtual void restoreSelection(bool selectedByCursor) { Q_UNUSED(selectedByCursor); }
    /// Copies @p text to the system clipboard.
    virtual void copyToClipboard(const QString& text) { Q_UNUSED(text); }
    /// The timecode of @p frame.
    [[nodiscard]] virtual QString timecode(int64_t frame) const { return QString::number(frame); }
    /// A command of `vim:<name>` that only this host knows (the list host's `list_open`);
    /// false when it is not one of its own. @p count is the typed count.
    virtual bool hostCommand(const QString& name, int count, bool hasCount)
    {
        Q_UNUSED(name);
        Q_UNUSED(count);
        Q_UNUSED(hasCount);
        return false;
    }
    /// Like hostCommand() for a command that takes the typed character of a class key ("{a-z}");
    /// the default ignores it.
    virtual bool hostCommandArg(const QString& name, int count, bool hasCount, const QString& arg)
    {
        Q_UNUSED(arg);
        return hostCommand(name, count, hasCount);
    }
    /// How many rows a list shows (VimListHost): `<C-d>` / `<C-u>` move half of it.
    [[nodiscard]] virtual int pageRows() const { return 20; }
    /// The focus moved to @p focus, in the context this host serves (a list host finds its
    /// view).
    virtual void focusChanged(QWidget* focus) { Q_UNUSED(focus); }
    /// A message for the mode line ("3 clips yanked", "track is locked").
    virtual void showMessage(const QString& text) { Q_UNUSED(text); }
    /// Jumps to the next / previous (@p forward) search match of the last search; false when
    /// there is none.
    virtual bool searchStep(bool forward, int count)
    {
        Q_UNUSED(forward);
        Q_UNUSED(count);
        return false;
    }
};

} // namespace kk
