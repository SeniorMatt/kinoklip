// SPDX-License-Identifier: GPL-3.0-or-later
// ColorMatchPanel — the Color page's Color Match palette (docs/color-page.md, spec 153). Two jobs:
// the colour chart match (pick the chart, source gamma, target gamma, target colour space and
// white point, drag the chart overlay over the chart in the viewer, press Match) and the Shot
// Match settings (the strength; the command itself is in the clips strip's right-click menu).
// The result goes into the selected node as ordinary Primaries and RGB Mixer values through
// ColorSession (one undo step).
#pragma once

#include "core/ColorMatch.h"

#include <QPointer>
#include <QWidget>

class QComboBox;
class QLabel;
class QToolButton;

namespace kk {

class ColorFrameSource;
class ColorSession;
class ValueField;

class ColorMatchPanel : public QWidget
{
    Q_OBJECT
public:
    ColorMatchPanel(ColorSession* session, ColorFrameSource* frames, QWidget* parent = nullptr);

    [[nodiscard]] bool chartShown() const;
    void setChartShown(bool shown);
    [[nodiscard]] colormatch::ChartType chartType() const;
    [[nodiscard]] colormatch::ChartTarget target() const;
    [[nodiscard]] const colormatch::ChartQuad& quad() const { return m_quad; }
    /// Moves the chart outline (the viewer overlay's corner handles call this).
    void setQuad(const colormatch::ChartQuad& quad);

    /// Measures the chart in the picture the selected node receives and writes the match into
    /// the node. False (with the reason in statusText()) when nothing could be matched.
    bool match();
    /// The result line under the Match button ("Chart matched: error 0.081 to 0.012").
    [[nodiscard]] QString statusText() const;

    [[nodiscard]] QToolButton* showChartButton() const { return m_show; }
    [[nodiscard]] QToolButton* matchButton() const { return m_match; }
    [[nodiscard]] QComboBox* chartCombo() const { return m_chart; }
    [[nodiscard]] QComboBox* sourceGammaCombo() const { return m_sourceGamma; }
    [[nodiscard]] QComboBox* targetGammaCombo() const { return m_targetGamma; }
    [[nodiscard]] QComboBox* colorSpaceCombo() const { return m_space; }
    [[nodiscard]] QComboBox* whitePointCombo() const { return m_white; }
    [[nodiscard]] ValueField* strengthField() const { return m_strength; }

    /// The palette bar's Reset: the chart back to its default place and the selectors to Rec.709.
    void resetAll();

Q_SIGNALS:
    /// The chart overlay in the viewer is shown or hidden.
    void chartShownChanged(bool shown);
    /// The chart outline or the chart type changed (the overlay repaints).
    void chartChanged();

private:
    void setStatus(const QString& text);
    [[nodiscard]] double pictureAspect() const;

    QPointer<ColorSession> m_session;
    QPointer<ColorFrameSource> m_frames;
    colormatch::ChartQuad m_quad;
    bool m_quadMoved = false; ///< the user placed the chart: keep it when the aspect changes
    QToolButton* m_show = nullptr;
    QToolButton* m_resetChart = nullptr;
    QToolButton* m_match = nullptr;
    QComboBox* m_chart = nullptr;
    QComboBox* m_sourceGamma = nullptr;
    QComboBox* m_targetGamma = nullptr;
    QComboBox* m_space = nullptr;
    QComboBox* m_white = nullptr;
    ValueField* m_strength = nullptr;
    QLabel* m_status = nullptr;
};

} // namespace kk
