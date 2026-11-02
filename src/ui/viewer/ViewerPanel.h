// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerPanel — source / timeline viewer: picture (GL or raster), header with clip name, zoom,
// quality and timecode, jog/scrub bar with in/out marks, transport bar.
#pragma once

#include "core/Rational.h"
#include "playback/SourcePlayer.h"
#include "ui/common/PanelFrame.h"

#include <QImage>
#include <QPointer>

#include <cstdint>
#include <functional>
#include <optional>

class QComboBox;
class QKeyEvent;
class QHBoxLayout;
class QLabel;
class QTimer;
class QToolButton;

namespace kk {

struct AppContext;
class PlaybackEngine;
class MaskOverlay;
class MulticamOverlay;
class EditOverlay;
class TrimTwoUpView;
struct TrimPreview;
class ScrubBar;
class Timeline;
class ViewerSurface;

/// A video viewer.
///
/// - **Mode::Timeline** shows the program: it displays the frames of AppContext::playback
///   (the program PlaybackEngine). Without an engine (tests) it only follows the playhead.
/// - **Mode::Source** shows one media item (setSourceMedia()) played by its own SourcePlayer
///   (sharing the program engine's Compositor), with its own in/out marks. The picture can be
///   dragged out as a mime::SourceRange (marked range of the clip) onto the timeline.
///
/// Layout: title bar "Source — clip" / "Timeline — name" with zoom (Fit/50/100/200 %), preview
/// quality (Auto/Full/Half/Quarter) and timecode; the picture; a ScrubBar (duration, in/out
/// band, playhead; drag to scrub); the transport bar (mark in/out, start, previous frame,
/// play reverse, stop, play/pause, next frame, end, loop, play in-to-out, shuttle rate, marked
/// duration).
///
/// Keyboard: global transport actions are routed to the *active* viewer by ViewerTransport
/// (the last one that had focus, see activate()). With focus inside a Source viewer, the
/// mark.* keys (I, O, Alt+I/O/X, Shift+I/O) and Home/End act on the source clip: the panel
/// claims them via QEvent::ShortcutOverride, so the timeline's global actions do not fire.
///
/// Thread-safety: GUI thread only.
class ViewerPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Which picture the viewer shows.
    enum class Mode {
        Source,
        Timeline
    };
    /// Picture zoom.
    enum class Zoom {
        Fit,
        Percent50,
        Percent100,
        Percent200
    };

    /// Creates a viewer in @p mode.
    ViewerPanel(Mode mode, AppContext& ctx, QWidget* parent = nullptr);
    ~ViewerPanel() override;

    /// The viewer's mode.
    [[nodiscard]] Mode mode() const { return m_mode; }
    /// Displays @p frame (any size; ARGB32_Premultiplied preferred), scaled to fit.
    void showFrame(const QImage& frame);
    /// Shows black (no picture).
    void clearFrame();
    /// Updates the timecode display to @p frame at @p fps.
    void setPosition(int64_t frame, Rational fps);
    /// Source mode: shows MediaItem @p mediaId (empty = none). Updates the title.
    void setSourceMedia(const QString& mediaId);
    /// Source mode: shows the private timeline @p content as if it were clip @p item (the Cut
    /// page's source tape: every clip of a bin back to back). The player keeps marks and
    /// position per item id, so rebuilding the content for the same @p item.id keeps them.
    /// sourceMedia() stays empty (the item is not in the media pool); the picture cannot be
    /// dragged out (its range is not one media item). An empty @p content unloads.
    void setSourceContent(const MediaItem& item, TimelineData content);
    /// Source mode: offers the Source / Multicam mode menu (default true). Off for viewers that
    /// only ever show one kind of content (the Cut page's).
    void setMulticamAllowed(bool allowed);
    /// Source mode: current MediaItem id.
    [[nodiscard]] QString sourceMedia() const { return m_sourceMediaId; }
    /// Size in device-independent pixels available for the picture (to choose decode size).
    [[nodiscard]] QSize pictureSize() const;
    /// The picture surface (GlViewerSurface when OpenGL is available, for GPU playback).
    [[nodiscard]] ViewerSurface* surface() const;

    /// The engine whose frames this viewer shows (program engine, or the source player's);
    /// nullptr when there is none (Timeline mode without AppContext::playback, Source mode
    /// before the first clip).
    [[nodiscard]] PlaybackEngine* engine() const;
    /// Source mode: the player (created on first use; nullptr in Timeline mode).
    [[nodiscard]] SourcePlayer* sourcePlayer() const { return m_player; }
    /// Source mode: the marked range of the loaded clip (whole clip where unmarked).
    [[nodiscard]] std::optional<SourceRange> sourceSelection() const;

    /// Picture zoom.
    void setZoom(Zoom zoom);
    [[nodiscard]] Zoom zoom() const { return m_zoom; }
    /// Shows/hides header, scrub bar and transport (full-screen viewer shows only the picture).
    void setChromeVisible(bool visible);

    /// Makes this viewer the target of the global transport actions (ViewerTransport).
    void activate();
    /// The active viewer (last activated, may be hidden), or nullptr.
    [[nodiscard]] static ViewerPanel* activeViewer();
    /// All existing viewers, in creation order.
    [[nodiscard]] static QList<ViewerPanel*> allViewers();

    /// Source mode key handling: the action ids claimed while focus is inside the viewer.
    [[nodiscard]] static QStringList sourceKeyActionIds();

    // ---- Editing aids (Timeline mode; ViewerEditAids.cpp) ----

    /// Shows @p text (a timecode being typed, spec 2.10) in the timecode display instead of
    /// the position; empty = the position again.
    void setTimecodeEntryText(const QString& text);
    /// The two-up trim view (spec 8.15): shows @p preview over the picture while it is active.
    void setTrimPreview(const TrimPreview& preview);
    /// The two-up layer (nullptr in Source mode).
    [[nodiscard]] TrimTwoUpView* trimView() const { return m_twoUp; }
    /// The edit overlay shown while media is dragged over the viewer (nullptr in Source mode).
    [[nodiscard]] EditOverlay* editOverlay() const { return m_editOverlay; }
    /// Offers the edit overlay to drags (default off: only a page that handles
    /// sourceEditDropped() turns it on).
    void setEditOverlayEnabled(bool enabled);

    // ---- Multicam viewer (Source mode; ViewerMulticam.cpp) ----

    /// Shows the angle grid instead of the source clip (View > Multicam Viewer, the header's
    /// mode menu): the multicam clip under the timeline playhead (following the timeline,
    /// also while it plays), else the loaded multicam clip.
    void setMulticamMode(bool on);
    [[nodiscard]] bool multicamMode() const { return m_multicam; }
    /// Grid columns (2, 3, 4; 0 = automatic from the angle count).
    void setMulticamColumns(int columns);
    /// Shows grid page @p page (0-based, clamped).
    void setMulticamPage(int page);
    /// The angle choice of a click on cell @p angle or a number key: emits
    /// multicamAngleChosen() when following a timeline clip, else changes the angle the loaded
    /// multicam clip shows here.
    void chooseMulticamAngle(int angle);
    /// The grid overlay (nullptr in Timeline mode).
    [[nodiscard]] MulticamOverlay* multicamOverlay() const { return m_multicamOverlay; }
    /// Uid of the multicam timeline shown in the grid (0 = none).
    [[nodiscard]] quint64 multicamUid() const { return m_mc.uid; }
    /// True while the grid follows the multicam clip under the timeline playhead.
    [[nodiscard]] bool multicamFollowsTimeline() const { return m_mc.followClip != 0; }

Q_SIGNALS:
    /// Source mode transport requests (kept for compatibility; the buttons also drive the
    /// source player directly).
    void playPauseRequested();
    void stopRequested();
    void stepRequested(int frames);
    void goToStartRequested();
    void goToEndRequested();
    /// This viewer became the active one.
    void activated();
    /// Source mode: a different clip was loaded.
    void sourceMediaChanged(const QString& mediaId);
    /// Multicam viewer following the timeline: the user chose @p angle (0-based) to cut or
    /// switch to at the playhead (EditPage: TimelineController::chooseMulticamAngle()).
    void multicamAngleChosen(int angle);
    /// Timeline mode: media was dropped on a tile of the edit overlay (spec 4.14);
    /// @p actionId names the edit ("edit.insert", "edit.overwrite", ...), @p sources are the
    /// dropped ranges in order (media pool items: the whole clip).
    void sourceEditDropped(const QString& actionId, const QList<kk::SourceRange>& sources);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    QToolButton* addButton(QHBoxLayout* layout, const QString& actionId, const QIcon& icon,
        const QString& tip, const std::function<void()>& handler);
    void ensurePlayer();
    /// Creates the Timeline mode editing aids (edit overlay, two-up view).
    void setupEditingAids();
    /// The overlay's tiles with their current keys.
    void updateEditTiles();
    void bindEngine(PlaybackEngine* engine);
    void bindTimeline(Timeline* timeline);
    void onTimelineChanged();
    void onPositionChanged(int64_t frame);
    void onRateChanged(double rate);
    void updateOutputClient();
    void updateTitle();
    void updateMarksLabel();
    void startSourceDrag();
    bool handleSourceKey(const QString& actionId);
    [[nodiscard]] QString matchSourceKey(const QKeyEvent* key) const;
    [[nodiscard]] Timeline* boundTimeline() const { return m_boundTimeline; }
    void setupMulticam();
    void updateMulticam();
    void leaveMulticamContent();

    /// What the multicam grid currently shows.
    struct MulticamShown
    {
        quint64 uid = 0; ///< multicam timeline (0 = no grid)
        QString itemId; ///< its media pool item
        quint64 followClip = 0; ///< the timeline clip followed (0 = the loaded source)
        std::shared_ptr<const TimelineData> data; ///< version of the multicam timeline used
        int columns = 0;
        int page = 0;
        int audioAngle = -1;
    };

    AppContext& m_ctx;
    Mode m_mode;
    QString m_sourceMediaId;
    SourcePlayer* m_player = nullptr;
    QPointer<PlaybackEngine> m_engine;
    Timeline* m_boundTimeline = nullptr;
    QMetaObject::Connection m_tlChanged, m_tlPlayhead;
    ViewerSurface* m_surface = nullptr;
    MaskOverlay* m_maskOverlay = nullptr; ///< Timeline mode: on-screen mask controls
    MulticamOverlay* m_multicamOverlay = nullptr; ///< Source mode: angle grid chrome
    EditOverlay* m_editOverlay = nullptr; ///< Timeline mode: edit types while dragging media
    TrimTwoUpView* m_twoUp = nullptr; ///< Timeline mode: two-up trim view
    QString m_timecodeEntry; ///< setTimecodeEntryText()
    bool m_editOverlayEnabled = false; ///< setEditOverlayEnabled()
    QComboBox* m_modeBox = nullptr; ///< Source mode: Source / Multicam
    QComboBox* m_gridBox = nullptr; ///< multicam grid size (Auto, 2x2, 3x3, 4x4)
    bool m_multicam = false;
    bool m_multicamAllowed = true; ///< setMulticamAllowed()
    bool m_customContent = false; ///< setSourceContent() is shown
    bool m_multicamPrefersSource = false; ///< the loaded multicam clip wins over the timeline
    int m_multicamColumns = 0;
    int m_multicamPage = 0;
    int m_localVideoAngle = 0; ///< angles of the loaded multicam clip (not following)
    int m_localAudioAngle = 0;
    MulticamShown m_mc;
    QWidget* m_body = nullptr;
    ScrubBar* m_scrub = nullptr;
    QWidget* m_transport = nullptr;
    QLabel* m_proxyLabel = nullptr; ///< "Proxy" indicator in the title bar
    QLabel* m_timecode = nullptr;
    QLabel* m_rateLabel = nullptr;
    QLabel* m_marksLabel = nullptr;
    QComboBox* m_zoomBox = nullptr;
    QComboBox* m_qualityBox = nullptr;
    QToolButton* m_playButton = nullptr;
    Zoom m_zoom = Zoom::Fit;
    QPoint m_dragStart;
    bool m_dragArmed = false;
    QTimer* m_resizeSettle = nullptr; ///< coalesces output-size updates while resizing
    bool m_outputSized = false; ///< the engine has an output size for this viewer
    Rational m_fps;
};

} // namespace kk
