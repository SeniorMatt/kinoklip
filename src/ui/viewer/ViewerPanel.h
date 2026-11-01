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
class QToolButton;

namespace kk {

struct AppContext;
class PlaybackEngine;
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

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QToolButton* addButton(QHBoxLayout* layout, const QString& actionId, const QIcon& icon,
        const QString& tip, const std::function<void()>& handler);
    void ensurePlayer();
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

    AppContext& m_ctx;
    Mode m_mode;
    QString m_sourceMediaId;
    SourcePlayer* m_player = nullptr;
    QPointer<PlaybackEngine> m_engine;
    Timeline* m_boundTimeline = nullptr;
    QMetaObject::Connection m_tlChanged, m_tlPlayhead;
    ViewerSurface* m_surface = nullptr;
    QWidget* m_body = nullptr;
    ScrubBar* m_scrub = nullptr;
    QWidget* m_transport = nullptr;
    QLabel* m_timecode = nullptr;
    QLabel* m_rateLabel = nullptr;
    QLabel* m_marksLabel = nullptr;
    QComboBox* m_zoomBox = nullptr;
    QComboBox* m_qualityBox = nullptr;
    QToolButton* m_playButton = nullptr;
    Zoom m_zoom = Zoom::Fit;
    QPoint m_dragStart;
    bool m_dragArmed = false;
    Rational m_fps;
};

} // namespace kk
