// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/color/ColorMatchPanel.h"

#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorSession.h"
#include "ui/common/VimRowOps.h"
#include "ui/inspector/ValueField.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include <vector>

namespace kk {

namespace {

namespace cm = colormatch;

QToolButton* matchButton(const QString& text, QWidget* parent, bool checkable = false)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

QComboBox* makeCombo(QWidget* parent, const QString& name)
{
    auto* c = new QComboBox(parent);
    c->setObjectName(name);
    c->setMinimumContentsLength(16);
    c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    return c;
}

} // namespace

ColorMatchPanel::ColorMatchPanel(ColorSession* session, ColorFrameSource* frames, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_frames(frames)
{
    markVimForm(this);
    setObjectName(QStringLiteral("ColorMatchPanel"));
    m_quad = cm::defaultChartQuad(16.0 / 9.0);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(18);

    // Chart column: the chart and its overlay.
    auto* chartBox = new QWidget(this);
    auto* chartGrid = new QGridLayout(chartBox);
    chartGrid->setContentsMargins(0, 0, 0, 0);
    chartGrid->setHorizontalSpacing(8);
    chartGrid->setVerticalSpacing(3);
    chartGrid->addWidget(new QLabel(tr("Color Chart"), chartBox), 0, 0, 1, 2);
    m_chart = makeCombo(chartBox, QStringLiteral("ColorMatchChart"));
    for (const cm::ChartType t : {cm::ChartType::ColorCheckerClassic, cm::ChartType::ColorCheckerPassport})
        m_chart->addItem(cm::chartName(t), int(t));
    chartGrid->addWidget(m_chart, 1, 0, 1, 2);
    m_show = matchButton(tr("Show Chart"), chartBox, true);
    m_show->setObjectName(QStringLiteral("ColorMatchShowChart"));
    m_show->setProperty("kkTab", true);
    m_show->setToolTip(tr("Show the chart outline in the viewer; drag its corners onto the "
                          "chart in the picture, or drag inside to move it"));
    m_resetChart = matchButton(tr("Reset Chart"), chartBox);
    m_resetChart->setObjectName(QStringLiteral("ColorMatchResetChart"));
    m_resetChart->setToolTip(tr("Put the chart outline back in the middle of the picture"));
    chartGrid->addWidget(m_show, 2, 0);
    chartGrid->addWidget(m_resetChart, 2, 1);
    chartGrid->setRowStretch(3, 1);
    layout->addWidget(chartBox);

    // Target column: gammas, colour space and white point.
    auto* targetBox = new QWidget(this);
    auto* grid = new QGridLayout(targetBox);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(3);
    m_sourceGamma = makeCombo(targetBox, QStringLiteral("ColorMatchSourceGamma"));
    m_targetGamma = makeCombo(targetBox, QStringLiteral("ColorMatchTargetGamma"));
    for (int g = 0; g <= int(cm::Gamma::Rec709); ++g) {
        m_sourceGamma->addItem(cm::gammaName(cm::Gamma(g)), g);
        m_targetGamma->addItem(cm::gammaName(cm::Gamma(g)), g);
    }
    m_sourceGamma->setCurrentIndex(int(cm::Gamma::Rec709));
    m_targetGamma->setCurrentIndex(int(cm::Gamma::Rec709));
    m_space = makeCombo(targetBox, QStringLiteral("ColorMatchColorSpace"));
    for (int s = 0; s <= int(cm::ColorSpace::Rec2020); ++s)
        m_space->addItem(cm::colorSpaceName(cm::ColorSpace(s)), s);
    m_white = makeCombo(targetBox, QStringLiteral("ColorMatchWhitePoint"));
    for (int w = 0; w <= int(cm::WhitePoint::Dci); ++w)
        m_white->addItem(cm::whitePointName(cm::WhitePoint(w)), w);
    const QPair<QString, QComboBox*> rows[] = {{tr("Source Gamma"), m_sourceGamma},
        {tr("Target Gamma"), m_targetGamma}, {tr("Target Color Space"), m_space},
        {tr("White Point"), m_white}};
    int row = 0;
    for (const auto& [label, combo] : rows) {
        grid->addWidget(new QLabel(label, targetBox), row, 0);
        grid->addWidget(combo, row, 1);
        ++row;
    }
    grid->setRowStretch(row, 1);
    layout->addWidget(targetBox);

    // Match column: the button, the result, and the Shot Match strength.
    auto* matchBox = new QWidget(this);
    auto* mv = new QVBoxLayout(matchBox);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(4);
    m_match = matchButton(tr("Match"), matchBox);
    m_match->setObjectName(QStringLiteral("ColorMatchMatch"));
    m_match->setToolTip(tr("Measure the chart under the outline and set the selected node's "
                           "lift, gamma, gain and RGB Mixer so the chart gets its reference "
                           "colours"));
    m_match->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; padding: 3px 18px; }"));
    mv->addWidget(m_match, 0, Qt::AlignLeft);
    m_status = new QLabel(matchBox);
    m_status->setObjectName(QStringLiteral("ColorMatchStatus"));
    m_status->setWordWrap(true);
    mv->addWidget(m_status);
    auto* shot = new QHBoxLayout;
    shot->addWidget(new QLabel(tr("Shot Match Strength"), matchBox));
    m_strength = new ValueField(matchBox);
    m_strength->setObjectName(QStringLiteral("ColorMatchStrength"));
    m_strength->setRange(0, 100);
    m_strength->setStep(1);
    m_strength->setDecimals(0);
    m_strength->setSuffix(QStringLiteral(" %"));
    m_strength->setValue(m_session ? m_session->shotMatchStrength() * 100.0 : 100.0);
    m_strength->setFixedWidth(std::max(72, m_strength->sizeHint().width()));
    m_strength->setToolTip(tr("How far Shot Match goes. Select clips in the clips strip, then "
                              "right-click the clip to match them to: Shot Match to This Clip"));
    shot->addWidget(m_strength);
    shot->addStretch(1);
    mv->addLayout(shot);
    mv->addStretch(1);
    layout->addWidget(matchBox, 1);

    connect(m_show, &QToolButton::toggled, this, [this](bool on) {
        emit chartShownChanged(on);
        emit chartChanged();
    });
    connect(m_resetChart, &QToolButton::clicked, this, [this] {
        m_quadMoved = false;
        setQuad(cm::defaultChartQuad(pictureAspect()));
        m_quadMoved = false;
    });
    connect(m_chart, &QComboBox::currentIndexChanged, this, [this] { emit chartChanged(); });
    connect(m_match, &QToolButton::clicked, this, [this] { match(); });
    connect(m_strength, &ValueField::valueCommitted, this, [this](double v) {
        if (m_session)
            m_session->setShotMatchStrength(v / 100.0);
    });
    if (m_session) {
        connect(m_session, &ColorSession::clipChanged, this, [this] {
            if (!m_quadMoved)
                m_quad = cm::defaultChartQuad(pictureAspect());
            emit chartChanged();
        });
    }
}

double ColorMatchPanel::pictureAspect() const
{
    if (m_frames && !m_frames->input().isNull() && m_frames->input().height() > 0)
        return double(m_frames->input().width()) / double(m_frames->input().height());
    return 16.0 / 9.0;
}

bool ColorMatchPanel::chartShown() const
{
    return m_show->isChecked();
}

void ColorMatchPanel::setChartShown(bool shown)
{
    m_show->setChecked(shown);
}

cm::ChartType ColorMatchPanel::chartType() const
{
    return cm::ChartType(m_chart->currentData().toInt());
}

cm::ChartTarget ColorMatchPanel::target() const
{
    cm::ChartTarget t;
    t.sourceGamma = cm::Gamma(m_sourceGamma->currentData().toInt());
    t.targetGamma = cm::Gamma(m_targetGamma->currentData().toInt());
    t.space = cm::ColorSpace(m_space->currentData().toInt());
    t.white = cm::WhitePoint(m_white->currentData().toInt());
    return t;
}

void ColorMatchPanel::setQuad(const cm::ChartQuad& quad)
{
    if (quad == m_quad)
        return;
    m_quad = quad;
    m_quadMoved = true;
    emit chartChanged();
}

QString ColorMatchPanel::statusText() const
{
    return m_status->text();
}

void ColorMatchPanel::setStatus(const QString& text)
{
    m_status->setText(text);
}

bool ColorMatchPanel::match()
{
    if (!m_session || !m_frames || !m_session->hasGrade()) {
        setStatus(tr("Open a clip to match."));
        return false;
    }
    const GradeNode* node = m_session->currentNode();
    if (node && node->isMixer()) {
        setStatus(tr("Select a corrector node."));
        return false;
    }
    const quint64 nodeId = node ? node->id : 0;
    const QImage picture = m_frames->nodeInput(nodeId).convertToFormat(QImage::Format_ARGB32);
    if (picture.isNull()) {
        setStatus(tr("No picture yet."));
        return false;
    }
    std::vector<float> rgb(size_t(picture.width()) * size_t(picture.height()) * 3);
    for (int y = 0; y < picture.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(picture.constScanLine(y));
        float* out = rgb.data() + size_t(y) * size_t(picture.width()) * 3;
        for (int x = 0; x < picture.width(); ++x) {
            out[x * 3] = float(qRed(line[x])) / 255.0f;
            out[x * 3 + 1] = float(qGreen(line[x])) / 255.0f;
            out[x * 3 + 2] = float(qBlue(line[x])) / 255.0f;
        }
    }
    cm::PatchColors patches {};
    if (!cm::measureChart(rgb.data(), picture.width(), picture.height(), m_quad, patches)) {
        setStatus(tr("The chart outline is outside the picture or not a four-sided shape."));
        return false;
    }
    const cm::ChartMatch result = cm::solveChart(patches, chartType(), target());
    if (!result.valid) {
        setStatus(tr("The chart is too flat to match: put the outline on the chart."));
        return false;
    }
    if (!m_session->applyChartMatch(nodeId, result, tr("Color Match")))
        return false;
    setStatus(tr("Chart matched: error %1 to %2")
                  .arg(result.errorBefore, 0, 'f', 3)
                  .arg(result.errorAfter, 0, 'f', 3));
    return true;
}

void ColorMatchPanel::resetAll()
{
    m_sourceGamma->setCurrentIndex(int(cm::Gamma::Rec709));
    m_targetGamma->setCurrentIndex(int(cm::Gamma::Rec709));
    m_space->setCurrentIndex(int(cm::ColorSpace::Rec709));
    m_white->setCurrentIndex(int(cm::WhitePoint::D65));
    m_chart->setCurrentIndex(0);
    m_quadMoved = false;
    setQuad(cm::defaultChartQuad(pictureAspect()));
    m_quadMoved = false;
    m_strength->setValue(100.0);
    if (m_session)
        m_session->setShotMatchStrength(1.0);
    setStatus({});
}

} // namespace kk
