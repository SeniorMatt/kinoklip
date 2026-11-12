// SPDX-License-Identifier: GPL-3.0-or-later
// VimTimelineHost — connects the vim key layer (ui/common/VimInput.h) to a timeline (spec
// section 83). It answers the layer's questions about the timeline as a buffer (tracks as
// lines, clips as words), keeps the cursor track and the registers, draws the VISUAL overlay
// through the controller, and carries out the operators with the edit functions of
// core/EditOps.h, each as one undo step.
#pragma once

#include "core/EditOps.h"
#include "ui/common/VimInput.h"

#include <QMap>

#include <functional>

namespace kk {

struct AppContext;
class TimelineController;

class VimTimelineHost final : public VimHost
{
public:
    VimTimelineHost(AppContext& ctx, TimelineController* controller);

    bool trigger(const QString& actionId) override;
    [[nodiscard]] int64_t playhead() const override;
    void setPlayhead(int64_t frame) override;
    void clearSelection() override;
    void openCommandLine() override;
    void openSearch() override;
    void runEx(const QString& line) override;

    [[nodiscard]] int trackCount() const override;
    [[nodiscard]] int cursorTrack() const override;
    void setCursorTrack(int index) override;
    [[nodiscard]] QVector<VimClipSpan> clipSpans(int track) const override;
    [[nodiscard]] QVector<quint64> linkedClips(quint64 clipId) const override;
    [[nodiscard]] QVector<int64_t> markerFrames() const override;
    [[nodiscard]] QVector<int64_t> keyframeFrames() const override;
    [[nodiscard]] int64_t timelineEnd() const override;
    [[nodiscard]] double framesPerSecond() const override;
    [[nodiscard]] std::pair<int64_t, int64_t> markedRange() const override;
    [[nodiscard]] bool trackLocked(int track) const override;
    [[nodiscard]] QVector<quint64> selectedClips() const override;
    [[nodiscard]] QString clipName(quint64 id) const override;
    bool searchFor(const QString& pattern, bool forward) override { return search(pattern, forward); }

    void beginUndoGroup() override;
    void endUndoGroup() override;
    bool operate(const VimOperation& operation) override;
    bool pasteRegister(QChar reg, int count, bool insert) override;
    [[nodiscard]] bool registerHasContent(QChar reg) const override;
    void showVisual(const VimVisual& visual) override;
    bool selectCursorClip() override;
    void restoreSelection(bool selectedByCursor) override;
    void copyToClipboard(const QString& text) override;
    [[nodiscard]] QString timecode(int64_t frame) const override;
    void showMessage(const QString& text) override;
    bool searchStep(bool forward, int count) override;

    /// Display index <-> track (video tracks from the highest to V1, then A1, A2, ...).
    [[nodiscard]] TrackRef trackAt(int index) const;
    [[nodiscard]] int indexOf(TrackRef ref) const;
    /// The contents of register @p reg ('a'..'z', '0'); the unnamed one is the controller's
    /// clipboard.
    [[nodiscard]] editops::Clipboard registerContent(QChar reg) const;
    /// `/pattern`: remembers it and jumps to the first match after the cursor. False when
    /// nothing matches.
    bool search(const QString& pattern, bool forward = true);
    /// `:noh`: forgets the pattern.
    void clearSearch() { m_pattern.clear(); }
    /// Draws or clears the cursor track highlight (Vim mode on / off).
    void showCursor(bool on);

    /// Called for `:` and `/` (set by the window; empty = ignored).
    std::function<void()> onCommandLine;
    std::function<void()> onSearch;
    /// Called for a key bound to an ex command (set by the window).
    std::function<void(const QString&)> onEx;
    /// Called with a message for the mode line (set by the window).
    std::function<void(const QString&)> onMessage;

private:
    [[nodiscard]] const TimelineData& data() const;
    /// The tracks an operation on @p range covers, widened by linked partners when linked
    /// selection is on; empty when they are all locked.
    [[nodiscard]] std::vector<TrackRef> tracksOf(const VimRange& range) const;
    void store(QChar reg, const editops::Clipboard& board, VimOp op, bool large);
    void updateOverlay();

    AppContext& m_ctx;
    TimelineController* m_controller;
    int m_cursorTrack = -1; ///< display index; -1 until the first use (the target track)
    bool m_cursorShown = false;
    VimVisual m_visual;
    QMap<QChar, editops::Clipboard> m_registers;
    QString m_pattern;
    int m_undoDepth = 0;
};

} // namespace kk
