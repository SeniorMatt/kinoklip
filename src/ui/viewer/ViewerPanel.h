// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerPanel — PLACEHOLDER source/timeline viewer (picture area, timecode, transport buttons).
// The playback agent replaces the implementation; keep this public API (extend as needed).
#pragma once

#include "core/Rational.h"
#include "ui/common/PanelFrame.h"

#include <QImage>

#include <cstdint>
#include <functional>

class QHBoxLayout;
class QLabel;

namespace kk {

struct AppContext;
class ViewerSurface;

/// A video viewer: a picture area (ViewerSurface — OpenGL, or raster where GL is unavailable)
/// showing the latest frame (letterboxed), a timecode in the title bar and a transport bar.
///
/// - Mode::Timeline shows the program; its transport buttons trigger the global `transport.*`
///   actions, and the timecode follows Timeline::playheadChanged.
/// - Mode::Source shows one media item (setSourceMedia()); its buttons emit the *Requested
///   signals so a source-side PlaybackEngine can react.
class ViewerPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Which picture the viewer shows.
    enum class Mode {
        Source,
        Timeline
    };

    /// Creates a viewer in @p mode.
    ViewerPanel(Mode mode, AppContext& ctx, QWidget* parent = nullptr);

    /// The viewer's mode.
    [[nodiscard]] Mode mode() const { return m_mode; }
    /// Displays @p frame (any size; ARGB32_Premultiplied preferred), scaled to fit.
    void showFrame(const QImage& frame);
    /// Shows black (no picture).
    void clearFrame();
    /// Updates the timecode display to @p frame at @p fps.
    void setPosition(int64_t frame, Rational fps);
    /// Source mode: the MediaItem id shown (empty = none). Updates the title.
    void setSourceMedia(const QString& mediaId);
    /// Source mode: current MediaItem id.
    [[nodiscard]] QString sourceMedia() const { return m_sourceMediaId; }
    /// Size in device-independent pixels available for the picture (to choose decode size).
    [[nodiscard]] QSize pictureSize() const;
    /// The picture surface (GlViewerSurface when OpenGL is available, for GPU playback).
    [[nodiscard]] ViewerSurface* surface() const;

signals:
    /// Source mode transport requests (Timeline mode uses the global actions instead).
    void playPauseRequested();
    void stopRequested();
    void stepRequested(int frames);
    void goToStartRequested();
    void goToEndRequested();

private:
    void addTransportButton(const QString& actionId, const std::function<void()>& sourceHandler);

    AppContext& m_ctx;
    Mode m_mode;
    QString m_sourceMediaId;
    ViewerSurface* m_surface = nullptr;
    QLabel* m_timecode = nullptr;
    QHBoxLayout* m_transportButtons = nullptr;
};

} // namespace kk
