// SPDX-License-Identifier: GPL-3.0-or-later
// TrimTwoUpView — the timeline viewer's two-up layer while an edit is trimmed (spec 8.15):
// the outgoing frame on the left, the incoming frame on the right, with clip names and
// timecodes.
#pragma once

#include "ui/timeline/DragInfo.h"

#include <QImage>
#include <QWidget>

namespace kk {

class MediaPool;
class ViewerSurface;

/// Opaque child of a viewer surface covering it while a trim, roll, slip or slide is dragged.
///
/// Pictures are the source frames of the two clips (MediaCache::frame(): exact frame, decoded
/// on the media cache's worker threads). Decision (spec 8.15): the clips' own pictures,
/// without effects or transforms, so the view never competes with the program renderer.
/// Requests are throttled to one in flight per side (a fast drag does not queue a decode per
/// mouse move); until the wanted frame arrives the side keeps its previous picture. Titles,
/// generators and empty sides (gap, timeline edge) are drawn black with their name.
///
/// Thread-safety: GUI thread only.
class TrimTwoUpView : public QWidget
{
    Q_OBJECT
public:
    /// @p pool resolves media ids to file paths (not owned, must outlive the view).
    TrimTwoUpView(ViewerSurface* surface, const MediaPool* pool);

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
    };
    /// Picture height requested from the media cache for the current widget size.
    [[nodiscard]] int requestHeight() const;
    /// Requests / picks up the wanted frame of side @p i.
    void fetch(int i);
    void onFrameReady(const QString& path);
    void paintSide(QPainter& p, const QRect& r, const TrimPreviewSide& side, const Side& s);

    ViewerSurface* m_surface;
    const MediaPool* m_pool;
    TrimPreview m_preview;
    Rational m_fps;
    Side m_sides[2];
};

} // namespace kk
