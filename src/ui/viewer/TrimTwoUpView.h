// SPDX-License-Identifier: GPL-3.0-or-later
// TrimTwoUpView — the timeline viewer's two-up layer while an edit is trimmed (spec 8.15):
// the outgoing frame on the left, the incoming frame on the right, with clip names and
// timecodes.
#pragma once

#include "render/RenderTypes.h"
#include "ui/timeline/DragInfo.h"

#include <QImage>
#include <QWidget>

#include <future>

class QTimer;

namespace kk {

class MediaPool;
class PlaybackEngine;
class Timeline;
class ViewerSurface;

/// Opaque child of a viewer surface covering it while a trim, roll, slip or slide is dragged.
///
/// Two picture modes (spec 8.15, 8.19):
/// - Source: the source frames of the two clips (MediaCache::frame(): exact frame, decoded on
///   the media cache's worker threads), without effects or transforms. Titles and generators
///   are drawn black with their name.
/// - Program: each side is the clip alone rendered by the program compositor (effects,
///   transform, crop, masks, titles and generators; not its transitions and fades, and no
///   other tracks) at the two-up's reduced size, through the engine's proxies. Used when
///   setProgramSource() gave a GPU compositor and the mode is Program.
/// Requests are throttled to one in flight per side (a fast drag does not queue a decode per
/// mouse move); until the wanted frame arrives the side keeps its previous picture. Empty
/// sides (gap, timeline edge) are drawn black.
///
/// Thread-safety: GUI thread only.
class TrimTwoUpView : public QWidget
{
    Q_OBJECT
public:
    /// @p pool resolves media ids to file paths (not owned, must outlive the view).
    TrimTwoUpView(ViewerSurface* surface, const MediaPool* pool);

    /// How the sides are pictured.
    enum class Pictures {
        Source,
        Program
    };
    /// Renders Program pictures through @p engine's compositor (shared; GPU only, else the
    /// view stays in Source mode) from @p timeline's snapshot. Either null = Source only.
    void setProgramSource(PlaybackEngine* engine, Timeline* timeline);
    /// Chooses the picture mode (Program falls back to Source without a GPU compositor).
    void setPictures(Pictures pictures);
    /// The mode in effect.
    [[nodiscard]] Pictures pictures() const;

    /// Shows @p preview (hidden when it is inactive). @p fps formats the timecodes.
    void setPreview(const TrimPreview& preview, Rational fps);
    [[nodiscard]] const TrimPreview& preview() const { return m_preview; }
    /// Whether side 0 (left) or 1 (right) currently shows its wanted frame (tests).
    [[nodiscard]] bool hasPicture(int side) const;
    /// Follows the surface size.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Side
    {
        QString path; ///< media file of the wanted frame (empty: none)
        int64_t wantedUs = -1;
        int64_t pendingUs = -1; ///< requested, not yet ready (-1 = none in flight)
        int64_t shownUs = -1; ///< time of `image`
        QImage image;
        // Program mode: the key is (clip, timeline frame, timeline revision).
        quint64 wantedClip = 0;
        int64_t wantedFrame = -1;
        quint64 wantedRevision = 0;
        bool rendering = false; ///< `future` is in flight
        std::future<RenderedFramePtr> future;
        quint64 renderingClip = 0;
        int64_t renderingFrame = -1;
        quint64 renderingRevision = 0;
        quint64 shownClip = 0;
        int64_t shownFrame = -1;
        quint64 shownRevision = 0;
    };
    /// Picture height requested from the media cache for the current widget size.
    [[nodiscard]] int requestHeight() const;
    /// Requests / picks up the wanted frame of side @p i.
    void fetch(int i);
    void onFrameReady(const QString& path);
    /// Program mode: starts the render of side @p i's wanted frame unless one is in flight.
    void renderSide(int i);
    /// Program mode: collects finished renders (polled while any is in flight).
    void collectRenders();
    void paintSide(QPainter& p, const QRect& r, const TrimPreviewSide& side, const Side& s);

    ViewerSurface* m_surface;
    const MediaPool* m_pool;
    TrimPreview m_preview;
    Rational m_fps;
    Side m_sides[2];
    PlaybackEngine* m_engine = nullptr; ///< Program mode source (not owned)
    Timeline* m_timeline = nullptr;
    Pictures m_pictures = Pictures::Source; ///< EditBehaviour::twoUpProgramLook
    QTimer* m_poll = nullptr; ///< checks the render futures
};

} // namespace kk
