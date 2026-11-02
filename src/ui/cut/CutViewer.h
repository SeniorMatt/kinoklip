// SPDX-License-Identifier: GPL-3.0-or-later
// CutViewer — the Cut page's single large viewer with its three modes: Source Clip, Source Tape
// and Timeline.
#pragma once

#include "core/SourceTape.h"
#include "ui/timeline/TimelineController.h"

#include <QStringList>
#include <QWidget>

#include <cstdint>

class QStackedWidget;
class QToolButton;

namespace kk {

struct AppContext;
class SourceTapeBar;
class ViewerPanel;

/// One viewer that switches between three pictures, like Resolve's Cut page viewer:
/// - **Source Clip**: one media pool clip with its own in / out marks (a source ViewerPanel);
/// - **Source Tape**: every clip of the current bin back to back as one long tape (a source
///   ViewerPanel showing SourceTape::toTimeline()), with the SourceTapeBar of clip boundaries
///   below it; in / out marks work across clips;
/// - **Timeline**: the program (a timeline ViewerPanel on the program engine).
///
/// Each mode is its own ViewerPanel in a stack, so switching keeps the clip, the tape position
/// and the marks of the others. The mode buttons in the viewer header are the registry actions
/// cut.viewer_source_clip / _source_tape / _timeline (an exclusive group). The active viewer
/// becomes the transport target (ViewerTransport) whenever the mode changes.
///
/// sourceMaterial() turns what the source modes show into the ranges the smart edits place.
/// Thread-safety: GUI thread only.
class CutViewer : public QWidget
{
    Q_OBJECT
public:
    /// What the viewer shows.
    enum class Mode {
        SourceClip,
        SourceTape,
        Timeline
    };
    Q_ENUM(Mode)

    /// Creates the three viewers for @p ctx.
    explicit CutViewer(AppContext& ctx, QWidget* parent = nullptr);

    [[nodiscard]] Mode mode() const { return m_mode; }
    /// Switches the picture (and the transport target); keeps the mode actions checked.
    void setMode(Mode mode);
    /// The last source mode used (Q toggles between it and Timeline).
    [[nodiscard]] Mode lastSourceMode() const { return m_lastSource; }

    [[nodiscard]] ViewerPanel* clipViewer() const { return m_clip; }
    [[nodiscard]] ViewerPanel* tapeViewer() const { return m_tape; }
    [[nodiscard]] ViewerPanel* timelineViewer() const { return m_timeline; }
    /// The viewer of the current mode.
    [[nodiscard]] ViewerPanel* currentViewer() const;
    [[nodiscard]] SourceTapeBar* tapeBar() const { return m_tapeBar; }

    /// The clips of the source tape: media ids in view order, for bin @p binKey named
    /// @p binName. Rebuilt at once in Source Tape mode, else on the next switch to it.
    void setTapeItems(const QStringList& mediaIds, const QString& binKey, const QString& binName);
    /// The current tape (after the last rebuild).
    [[nodiscard]] const SourceTape& tape() const { return m_sourceTape; }
    /// Rebuilds the tape now (keeps position and marks for the same bin).
    void rebuildTape();
    /// Source Tape mode: moves the tape to the first frame of clip @p mediaId. Returns whether
    /// the clip is on the tape.
    bool showTapeClip(const QString& mediaId);
    /// Media id of the clip under the tape position (empty if none).
    [[nodiscard]] QString tapeClipAtPosition() const;

    /// The ranges the smart edits take: Source Clip mode its marked range (whole clip where
    /// unmarked); Source Tape mode the marked tape range split at the clip boundaries (only an
    /// in mark: to the end of that clip; only an out mark: from the start of its clip; no
    /// marks: the whole clip under the tape position). Empty in Timeline mode and when nothing
    /// is loaded.
    [[nodiscard]] QVector<SourceSelection> sourceMaterial() const;
    /// sourceMaterial() for @p mode instead of the current one.
    [[nodiscard]] QVector<SourceSelection> sourceMaterial(Mode mode) const;

signals:
    /// The mode changed.
    void modeChanged(kk::CutViewer::Mode mode);

private:
    /// Adds the three mode buttons to @p viewer's header.
    void addModeButtons(ViewerPanel* viewer);
    void syncTapeBar();

    AppContext& m_ctx;
    Mode m_mode = Mode::Timeline;
    Mode m_lastSource = Mode::SourceClip;
    QStackedWidget* m_stack = nullptr;
    ViewerPanel* m_clip = nullptr;
    ViewerPanel* m_tape = nullptr;
    ViewerPanel* m_timeline = nullptr;
    SourceTapeBar* m_tapeBar = nullptr;
    QWidget* m_tapePage = nullptr; ///< tape viewer + tape bar
    SourceTape m_sourceTape;
    QStringList m_tapeIds;
    QString m_tapeKey;
    QString m_tapeName;
    bool m_tapeDirty = true;
};

} // namespace kk
