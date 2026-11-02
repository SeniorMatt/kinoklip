// SPDX-License-Identifier: GPL-3.0-or-later
// CurveEditorWidget — the curve editor lane below a clip on the timeline: a CurveView of one
// clip with the parameter list and the interpolation tool bar in the track header column.
#pragma once

#include "core/KeyframeOps.h"
#include "ui/keyframes/CurveView.h"

#include <QRect>

class QToolButton;

namespace kk {

/// Curve editor lane of one clip (spec 15.10 - 15.12).
///
/// The header column lists the animated parameters (click toggles a curve; the last one turned
/// on is the active curve whose value axis is shown) and holds the tool bar: Linear, Smooth,
/// Ease In, Ease Out, Ease In and Out, Static, Flatten, Fit. Time follows the timeline zoom;
/// every curve is scaled to its own value range (CurveView's Normalised mode) so curves with
/// different units share the lane. Editing is CurveView's (shared with the Keyframes panel).
/// Thread-safety: GUI thread only.
class CurveEditorWidget : public CurveView
{
    Q_OBJECT
public:
    /// Lane of clip @p clipId on the timeline time map @p map, child of the timeline viewport.
    CurveEditorWidget(AppContext& ctx, const KeyframeTimeMap& map, quint64 clipId, QWidget* parent);

    /// Default lane height in px.
    static constexpr int kDefaultHeight = 170;

    /// The clip whose keys this lane edits.
    [[nodiscard]] quint64 clipId() const { return m_clipId; }
    /// Places the lane at logical viewport rect @p lane, cut to the area below @p minTop.
    void placeLane(const QRect& lane, int minTop);
    /// Logical lane height (independent of the visible cut).
    [[nodiscard]] int laneHeight() const { return logicalHeight(); }

    /// Paths of the curves currently shown.
    [[nodiscard]] QVector<QString> shownPaths() const;
    using CurveView::setCurveShown;
    /// Shows or hides the curve of @p path.
    void setCurveShown(const QString& path, bool shown);
    /// Widget-space position of the key of @p path at clip-local @p localFrame (tests).
    [[nodiscard]] QPointF keyPos(const QString& path, int64_t localFrame) const;
    /// Widget-space position of a handle tip of that key (tests).
    [[nodiscard]] QPointF handlePos(
        const QString& path, int64_t localFrame, keyframeops::HandleSide side) const;

protected:
    void paintLabelColumn(QPainter& p) override;
    bool pressLabelColumn(QPointF pos) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void refreshCurves();
    void layoutButtons();

    quint64 m_clipId = 0;
    QVector<QToolButton*> m_buttons;
};

} // namespace kk
