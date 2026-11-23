// SPDX-License-Identifier: GPL-3.0-or-later
// ColorPalettes — Qualifier, Windows, LUTs and Key palettes.
#include "ui/color/ColorPalettes.h"

#include "core/GradeMath.h"
#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorSession.h"
#include "ui/common/TextFit.h"
#include "ui/common/VimRowOps.h"
#include "ui/inspector/ValueField.h"
#include "ui/common/UiFonts.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

namespace wk = gradewindowkeys;

constexpr char kLutFoldersKey[] = "color/lutFolders";
constexpr int kFieldWidth = 64; ///< qualifier / window value fields (wider if a value needs it)

QToolButton* toolButton(const QString& text, QWidget* parent, bool checkable = false)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

QString shapeName(GradeWindowShape s)
{
    switch (s) {
    case GradeWindowShape::Circle:
        return WindowsPanel::tr("Circle");
    case GradeWindowShape::Linear:
        return WindowsPanel::tr("Linear");
    case GradeWindowShape::Polygon:
        return WindowsPanel::tr("Polygon");
    case GradeWindowShape::Gradient:
        return WindowsPanel::tr("Gradient");
    case GradeWindowShape::Curve:
        return WindowsPanel::tr("Curve");
    }
    return {};
}

ValueField* makeField(QWidget* parent, double lo, double hi, double step, int decimals)
{
    auto* f = new ValueField(parent);
    f->setRange(lo, hi);
    f->setStep(step);
    f->setDecimals(decimals);
    f->setFixedWidth(std::max(kFieldWidth, f->sizeHint().width())); // the widest value fits
    return f;
}

} // namespace

// ---- QualifierRangeBar ----

QualifierRangeBar::QualifierRangeBar(Channel channel, QWidget* parent)
    : QWidget(parent)
    , m_channel(channel)
{
    setObjectName(channel == Channel::Hue ? QStringLiteral("QualifierHueBar")
            : channel == Channel::Sat     ? QStringLiteral("QualifierSatBar")
                                          : QStringLiteral("QualifierLumBar"));
    setToolTip(tr("Drag the selected range to move it, its ends to resize it"));
}

void QualifierRangeBar::setRange(double a, double b, double soft)
{
    if (a == m_a && b == m_b && soft == m_soft)
        return;
    m_a = a;
    m_b = b;
    m_soft = soft;
    update();
}

QRectF QualifierRangeBar::barRect() const
{
    return QRectF(4, 3, std::max(10, width() - 8), std::max(6, height() - 6));
}

std::pair<double, double> QualifierRangeBar::ends() const
{
    if (m_channel == Channel::Hue)
        return {m_a - m_b / 2, m_a + m_b / 2};
    return {m_a, m_b};
}

void QualifierRangeBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = barRect();
    const int steps = 96;
    for (int i = 0; i < steps; ++i) {
        const double x = (i + 0.5) / steps;
        QColor c = m_channel == Channel::Hue ? QColor::fromHslF(float(x), 0.85f, 0.5f)
            : m_channel == Channel::Sat      ? QColor::fromHslF(0.0f, float(x), 0.5f)
                                             : QColor::fromRgbF(float(x), float(x), float(x));
        c = c.darker(150);
        p.fillRect(
            QRectF(r.left() + r.width() * i / steps, r.top(), r.width() / steps + 1, r.height()),
            c);
    }
    // The selected range (bright) with its soft edges (half bright); hue wraps around.
    auto [lo, hi] = ends();
    auto drawSpan = [&](double a, double b, int alpha) {
        for (const double shift : {-1.0, 0.0, 1.0}) {
            const double x0 = std::clamp(a + shift, 0.0, 1.0);
            const double x1 = std::clamp(b + shift, 0.0, 1.0);
            if (x1 <= x0)
                continue;
            if (m_channel != Channel::Hue && shift != 0.0)
                continue;
            p.fillRect(
                QRectF(r.left() + x0 * r.width(), r.top(), (x1 - x0) * r.width(), r.height()),
                QColor(255, 255, 255, alpha));
        }
    };
    drawSpan(lo - m_soft, lo, 30);
    drawSpan(hi, hi + m_soft, 30);
    drawSpan(lo, hi, 70);
    p.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
    for (const double e : {lo, hi}) {
        double x = e;
        if (m_channel == Channel::Hue)
            x -= std::floor(x);
        x = std::clamp(x, 0.0, 1.0);
        p.drawLine(QPointF(r.left() + x * r.width(), r.top() - 1),
            QPointF(r.left() + x * r.width(), r.bottom() + 1));
    }
    p.setPen(QPen(palette().color(QPalette::Dark), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
}

void QualifierRangeBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !isEnabled())
        return;
    const QRectF r = barRect();
    const double x = (event->position().x() - r.left()) / r.width();
    auto [lo, hi] = ends();
    // Ends within 5 px; hue ends compared on the wrapped bar.
    auto near = [&](double e) {
        double d = std::abs(x - e);
        if (m_channel == Channel::Hue)
            d = std::min({d, std::abs(x - (e - 1)), std::abs(x - (e + 1))});
        return d * r.width() <= 5.0;
    };
    if (near(lo))
        m_drag = Drag::Low;
    else if (near(hi))
        m_drag = Drag::High;
    else
        m_drag = Drag::Move;
    m_pressX = x;
    m_pressA = m_a;
    m_pressB = m_b;
    emit dragStarted();
}

void QualifierRangeBar::mouseMoveEvent(QMouseEvent* event)
{
    if (m_drag == Drag::None)
        return;
    const QRectF r = barRect();
    const double x = (event->position().x() - r.left()) / r.width();
    const double dx = x - m_pressX;
    double a = m_pressA, b = m_pressB;
    if (m_channel == Channel::Hue) {
        // a = centre, b = width.
        if (m_drag == Drag::Move) {
            a = m_pressA + dx;
            a -= std::floor(a);
        } else {
            const double side = m_drag == Drag::High ? 1.0 : -1.0;
            b = std::clamp(m_pressB + 2 * side * dx, 0.0, 1.0);
        }
    } else {
        if (m_drag == Drag::Move) {
            const double d = std::clamp(dx, -m_pressA, 1.0 - m_pressB);
            a = m_pressA + d;
            b = m_pressB + d;
        } else if (m_drag == Drag::Low) {
            a = std::clamp(m_pressA + dx, 0.0, m_pressB);
        } else {
            b = std::clamp(m_pressB + dx, m_pressA, 1.0);
        }
    }
    m_a = a;
    m_b = b;
    update();
    emit rangeChanged(a, b);
}

void QualifierRangeBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_drag == Drag::None)
        return;
    m_drag = Drag::None;
    emit dragFinished();
}

// ---- QualifierPanel ----

QualifierPanel::QualifierPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    markVimForm(this);
    setObjectName(QStringLiteral("QualifierPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(3);

    // ---- Tool row ----
    auto* tools = new QHBoxLayout;
    tools->setSpacing(2);
    m_picks = new QButtonGroup(this);
    m_picks->setExclusive(false);
    const std::pair<qualifierpick::Mode, QString> picks[]
        = {{qualifierpick::Mode::Pick, tr("Pick")}, {qualifierpick::Mode::Add, tr("Pick +")},
            {qualifierpick::Mode::Remove, tr("Pick -")}};
    for (const auto& [mode, text] : picks) {
        QToolButton* b = toolButton(text, this, true);
        b->setObjectName(QStringLiteral("QualifierPick_") + QString::number(int(mode)));
        b->setToolTip(mode == qualifierpick::Mode::Pick
                ? tr("Eyedropper: click or drag over the viewer to select those colours")
                : mode == qualifierpick::Mode::Add
                ? tr("Add the colours you click to the range")
                : tr("Remove the colours you click from the range"));
        m_picks->addButton(b, int(mode));
        tools->addWidget(b);
    }
    connect(m_picks, &QButtonGroup::idToggled, this, [this](int id, bool on) {
        if (on) {
            // One eyedropper at a time.
            for (QAbstractButton* other : m_picks->buttons()) {
                if (m_picks->id(other) != id && other->isChecked()) {
                    const QSignalBlocker block(other);
                    other->setChecked(false);
                }
            }
        }
        emit pickModeChanged();
    });
    tools->addSpacing(10);
    m_mode = new QComboBox(this);
    m_mode->setObjectName(QStringLiteral("QualifierMode"));
    m_mode->setToolTip(tr("What the qualifier selects by: hue / saturation / luminance, red / "
                          "green / blue, luma only, or picked colours in 3D"));
    m_mode->addItem(tr("HSL"), int(QualifierMode::Hsl));
    m_mode->addItem(tr("RGB"), int(QualifierMode::Rgb));
    m_mode->addItem(tr("Luma"), int(QualifierMode::Luma));
    m_mode->addItem(tr("3D"), int(QualifierMode::ThreeD));
    tools->addWidget(m_mode);
    m_enable = new QCheckBox(tr("Qualify"), this);
    m_enable->setObjectName(QStringLiteral("QualifierEnable"));
    m_invert = new QCheckBox(tr("Invert"), this);
    m_invert->setObjectName(QStringLiteral("QualifierInvert"));
    tools->addWidget(m_enable);
    tools->addWidget(m_invert);
    tools->addStretch(1);
    m_highlight = toolButton(tr("Highlight"), this, true);
    m_highlight->setObjectName(QStringLiteral("QualifierHighlight"));
    m_highlight->setToolTip(tr("Show the node's key in the viewer")); // the page adds the key
    m_highlightMode = new QComboBox(this);
    m_highlightMode->setObjectName(QStringLiteral("QualifierHighlightMode"));
    m_highlightMode->addItem(tr("Grey"));
    m_highlightMode->addItem(tr("Black / White"));
    connect(m_highlightMode, &QComboBox::currentIndexChanged, this,
        &QualifierPanel::highlightModeChanged);
    tools->addWidget(m_highlight);
    tools->addWidget(m_highlightMode);
    layout->addLayout(tools);

    // ---- The ranges: HSL (Luma shows the Lum row only), RGB, 3D ----
    m_ranges = new QStackedWidget(this);
    m_ranges->setObjectName(QStringLiteral("QualifierRanges"));
    auto* hslBox = new QWidget(m_ranges);
    auto* grid = new QGridLayout(hslBox);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(3);
    const QString names[] = {tr("Hue"), tr("Sat"), tr("Lum")};
    const QString fieldNames[3][3]
        = {{QStringLiteral("hueCenter"), QStringLiteral("hueWidth"), QStringLiteral("hueSoft")},
            {QStringLiteral("satLow"), QStringLiteral("satHigh"), QStringLiteral("satSoft")},
            {QStringLiteral("lumLow"), QStringLiteral("lumHigh"), QStringLiteral("lumSoft")}};
    const QString fieldLabels[3][3] = {{tr("Center"), tr("Width"), tr("Soft")},
        {tr("Low"), tr("High"), tr("Soft")}, {tr("Low"), tr("High"), tr("Soft")}};
    for (int c = 0; c < 3; ++c) {
        m_use[c] = new QCheckBox(names[c], hslBox);
        m_use[c]->setObjectName(QStringLiteral("QualifierUse_") + names[c]);
        grid->addWidget(m_use[c], c, 0);
        m_rowWidgets[c].append(m_use[c]);
        m_bars[c] = new QualifierRangeBar(QualifierRangeBar::Channel(c), hslBox);
        grid->addWidget(m_bars[c], c, 1);
        m_rowWidgets[c].append(m_bars[c]);
        for (int k = 0; k < 3; ++k) {
            auto* label = new QLabel(fieldLabels[c][k], hslBox);
            ValueField* f = makeField(hslBox, 0.0, 1.0, 0.002, 3);
            f->setObjectName(QStringLiteral("Qualifier_") + fieldNames[c][k]);
            grid->addWidget(label, c, 2 + k * 2, Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(f, c, 3 + k * 2);
            m_rowWidgets[c].append(label);
            m_rowWidgets[c].append(f);
            m_fields.append({fieldNames[c][k], f});
        }
    }
    grid->setColumnStretch(1, 1);
    grid->setHorizontalSpacing(8);
    m_ranges->addWidget(hslBox);

    // RGB: a Low / High / Soft row per channel.
    auto* rgbBox = new QWidget(m_ranges);
    auto* rgbGrid = new QGridLayout(rgbBox);
    rgbGrid->setContentsMargins(0, 0, 0, 0);
    rgbGrid->setHorizontalSpacing(8);
    rgbGrid->setVerticalSpacing(3);
    const QString rgbNames[] = {tr("Red"), tr("Green"), tr("Blue")};
    const QString rgbKeys[] = {QStringLiteral("red"), QStringLiteral("green"), QStringLiteral("blue")};
    const QString rgbLabels[] = {tr("Low"), tr("High"), tr("Soft")};
    const QString rgbSuffix[] = {QStringLiteral("Low"), QStringLiteral("High"), QStringLiteral("Soft")};
    for (int c = 0; c < 3; ++c) {
        m_useRgb[c] = new QCheckBox(rgbNames[c], rgbBox);
        m_useRgb[c]->setObjectName(QStringLiteral("QualifierUse_") + rgbNames[c]);
        rgbGrid->addWidget(m_useRgb[c], c, 0);
        for (int k = 0; k < 3; ++k) {
            auto* label = new QLabel(rgbLabels[k], rgbBox);
            ValueField* f = makeField(rgbBox, 0.0, 1.0, 0.002, 3);
            f->setObjectName(QStringLiteral("Qualifier_") + rgbKeys[c] + rgbSuffix[k]);
            rgbGrid->addWidget(label, c, 1 + k * 2, Qt::AlignRight | Qt::AlignVCenter);
            rgbGrid->addWidget(f, c, 2 + k * 2);
            m_fields.append({rgbKeys[c] + rgbSuffix[k], f});
        }
    }
    rgbGrid->setColumnStretch(7, 1);
    m_ranges->addWidget(rgbBox);

    // 3D: the picked colours and how far around them the selection reaches.
    auto* boxes3d = new QWidget(m_ranges);
    auto* row3d = new QHBoxLayout(boxes3d);
    row3d->setContentsMargins(0, 0, 0, 0);
    row3d->setSpacing(8);
    m_colourCount = new QLabel(boxes3d);
    m_colourCount->setObjectName(QStringLiteral("QualifierColourCount"));
    m_colourCount->setToolTip(tr("Pick, Pick + and Pick - in the viewer add the colours to "
                                 "select (or, with Pick -, to leave out)"));
    row3d->addWidget(m_colourCount);
    m_clearColours = toolButton(tr("Clear"), boxes3d);
    m_clearColours->setObjectName(QStringLiteral("QualifierClearColors"));
    m_clearColours->setToolTip(tr("Forget the picked colours"));
    row3d->addWidget(m_clearColours);
    row3d->addSpacing(12);
    for (const auto& [key, text] : {std::pair {QStringLiteral("colorRange"), tr("Range")},
             std::pair {QStringLiteral("colorSoft"), tr("Soft")}}) {
        auto* label = new QLabel(text, boxes3d);
        ValueField* f = makeField(boxes3d, 0.0, 1.0, 0.002, 3);
        f->setObjectName(QStringLiteral("Qualifier_") + key);
        row3d->addWidget(label);
        row3d->addWidget(f);
        m_fields.append({key, f});
    }
    row3d->addStretch(1);
    m_ranges->addWidget(boxes3d);
    layout->addWidget(m_ranges);

    // ---- Matte Finesse: cleans the picture of the qualifier's key ----
    auto* finesse = new QGridLayout;
    finesse->setHorizontalSpacing(8);
    finesse->setVerticalSpacing(3);
    auto* finesseTitle = new QLabel(tr("Matte Finesse"), this);
    finesseTitle->setObjectName(QStringLiteral("QualifierFinesseTitle"));
    const QFont bold = uifonts::font(uifonts::Role::Emphasis, finesseTitle->font());
    finesseTitle->setFont(bold);
    finesse->addWidget(finesseTitle, 0, 0, 2, 1, Qt::AlignTop);
    struct FinesseSpec
    {
        const char* key;
        QString label;
        QString tip;
        double lo;
    };
    const FinesseSpec finesseSpecs[] = {
        {"cleanBlack", tr("Clean Black"),
            tr("Removes small white specks from the black area of the matte"), 0.0},
        {"cleanWhite", tr("Clean White"),
            tr("Fills small black holes in the white area of the matte"), 0.0},
        {"shrinkGrow", tr("Shrink / Grow"),
            tr("Below 0 the matte shrinks, above 0 it grows"), -1.0},
        {"blurRadius", tr("Blur Radius"), tr("Softens the edge of the matte"), 0.0},
        {"inOutRatio", tr("In / Out Ratio"),
            tr("Above 0 the blur reaches into the matte, below 0 out of it"), -1.0},
        {"denoise", tr("Denoise"), tr("Smooths grain and noise in the matte"), 0.0}};
    for (int i = 0; i < 6; ++i) {
        const FinesseSpec& spec = finesseSpecs[i];
        auto* label = new QLabel(spec.label, this);
        label->setToolTip(spec.tip);
        ValueField* f = makeField(this, spec.lo, 1.0, 0.01, 2);
        f->setObjectName(QStringLiteral("Qualifier_") + QLatin1String(spec.key));
        f->setToolTip(spec.tip);
        finesse->addWidget(label, i / 3, 1 + (i % 3) * 2, Qt::AlignRight | Qt::AlignVCenter);
        finesse->addWidget(f, i / 3, 2 + (i % 3) * 2);
        m_fields.append({QLatin1String(spec.key), f});
    }
    finesse->setColumnStretch(7, 1);
    layout->addLayout(finesse);
    layout->addStretch(1);

    // ---- Wiring ----
    auto setter = [](const QString& name) {
        return [name](GradeQualifier& q, double v) {
            if (name == QLatin1String("hueCenter"))
                q.hueCenter = v;
            else if (name == QLatin1String("hueWidth"))
                q.hueWidth = v;
            else if (name == QLatin1String("hueSoft"))
                q.hueSoft = v;
            else if (name == QLatin1String("satLow"))
                q.satLow = std::min(v, q.satHigh);
            else if (name == QLatin1String("satHigh"))
                q.satHigh = std::max(v, q.satLow);
            else if (name == QLatin1String("satSoft"))
                q.satSoft = v;
            else if (name == QLatin1String("lumLow"))
                q.lumLow = std::min(v, q.lumHigh);
            else if (name == QLatin1String("lumHigh"))
                q.lumHigh = std::max(v, q.lumLow);
            else if (name == QLatin1String("lumSoft"))
                q.lumSoft = v;
            else if (name == QLatin1String("redLow"))
                q.redLow = std::min(v, q.redHigh);
            else if (name == QLatin1String("redHigh"))
                q.redHigh = std::max(v, q.redLow);
            else if (name == QLatin1String("redSoft"))
                q.redSoft = v;
            else if (name == QLatin1String("greenLow"))
                q.greenLow = std::min(v, q.greenHigh);
            else if (name == QLatin1String("greenHigh"))
                q.greenHigh = std::max(v, q.greenLow);
            else if (name == QLatin1String("greenSoft"))
                q.greenSoft = v;
            else if (name == QLatin1String("blueLow"))
                q.blueLow = std::min(v, q.blueHigh);
            else if (name == QLatin1String("blueHigh"))
                q.blueHigh = std::max(v, q.blueLow);
            else if (name == QLatin1String("blueSoft"))
                q.blueSoft = v;
            else if (name == QLatin1String("colorRange"))
                q.colorRange = v;
            else if (name == QLatin1String("colorSoft"))
                q.colorSoft = v;
            else if (name == QLatin1String("cleanBlack"))
                q.cleanBlack = v;
            else if (name == QLatin1String("cleanWhite"))
                q.cleanWhite = v;
            else if (name == QLatin1String("shrinkGrow"))
                q.shrinkGrow = v;
            else if (name == QLatin1String("blurRadius"))
                q.blurRadius = v;
            else if (name == QLatin1String("inOutRatio"))
                q.inOutRatio = v;
            else if (name == QLatin1String("denoise"))
                q.denoise = v;
            else
                return;
            // The Matte Finesse values clean whatever the qualifier selects: they do not
            // switch it on by themselves.
            static const QStringList finesseNames {QStringLiteral("cleanBlack"),
                QStringLiteral("cleanWhite"), QStringLiteral("shrinkGrow"),
                QStringLiteral("blurRadius"), QStringLiteral("inOutRatio"),
                QStringLiteral("denoise")};
            if (!finesseNames.contains(name))
                q.enabled = true;
        };
    };
    for (const auto& [name, f] : std::as_const(m_fields)) {
        auto set = setter(name);
        connect(f, &ValueField::scrubStarted, this, [this] {
            if (m_session)
                m_session->beginGesture(tr("Qualifier"));
        });
        connect(f, &ValueField::scrubFinished, this, [this] {
            if (m_session)
                m_session->endGesture();
        });
        auto change = [this, set](double v) {
            if (!m_refreshing)
                edit(tr("Qualifier"), [&](GradeQualifier& q) { set(q, v); });
        };
        connect(f, &ValueField::valueScrubbed, this, change);
        connect(f, &ValueField::valueCommitted, this, change);
    }
    for (int c = 0; c < 3; ++c) {
        connect(m_bars[c], &QualifierRangeBar::dragStarted, this, [this] {
            if (m_session)
                m_session->beginGesture(tr("Qualifier"));
        });
        connect(m_bars[c], &QualifierRangeBar::dragFinished, this, [this] {
            if (m_session)
                m_session->endGesture();
        });
        connect(m_bars[c], &QualifierRangeBar::rangeChanged, this, [this, c](double a, double b) {
            edit(tr("Qualifier"), [&](GradeQualifier& q) {
                q.enabled = true;
                if (c == 0) {
                    q.hueCenter = a;
                    q.hueWidth = b;
                } else if (c == 1) {
                    q.satLow = a;
                    q.satHigh = b;
                } else {
                    q.lumLow = a;
                    q.lumHigh = b;
                }
            });
        });
        connect(m_use[c], &QCheckBox::toggled, this, [this, c](bool on) {
            if (m_refreshing)
                return;
            edit(tr("Qualifier"), [&](GradeQualifier& q) {
                (c == 0 ? q.useHue : c == 1 ? q.useSat : q.useLum) = on;
            });
        });
    }
    for (int c = 0; c < 3; ++c) {
        connect(m_useRgb[c], &QCheckBox::toggled, this, [this, c](bool on) {
            if (m_refreshing)
                return;
            edit(tr("Qualifier"), [&](GradeQualifier& q) {
                (c == 0 ? q.useRed : c == 1 ? q.useGreen : q.useBlue) = on;
            });
        });
    }
    connect(m_mode, &QComboBox::activated, this, [this](int index) {
        if (m_refreshing)
            return;
        edit(tr("Qualifier Mode"), [&](GradeQualifier& q) {
            q.mode = QualifierMode(m_mode->itemData(index).toInt());
        });
    });
    connect(m_clearColours, &QToolButton::clicked, this, [this] {
        edit(tr("Clear Qualifier Colors"), [&](GradeQualifier& q) { q.colors.clear(); });
    });
    connect(m_enable, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_refreshing)
            edit(tr("Qualifier"), [&](GradeQualifier& q) { q.enabled = on; });
    });
    connect(m_invert, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_refreshing)
            edit(tr("Invert Qualifier"), [&](GradeQualifier& q) { q.invert = on; });
    });
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &QualifierPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &QualifierPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &QualifierPanel::refresh);
    }
    refresh();
}

template<typename Fn>
void QualifierPanel::edit(const QString& label, Fn&& fn)
{
    if (!m_session || !m_session->hasGrade())
        return;
    const GradeNode* n = m_session->currentNode();
    GradeQualifier q = n ? n->qualifier : GradeQualifier {};
    fn(q);
    if (n && q == n->qualifier)
        return;
    m_session->setQualifier(0, q, label);
}

std::optional<qualifierpick::Mode> QualifierPanel::pickMode() const
{
    const int id = m_picks->checkedId();
    if (id < 0) {
        for (QAbstractButton* b : m_picks->buttons()) {
            if (b->isChecked())
                return qualifierpick::Mode(m_picks->id(b));
        }
        return std::nullopt;
    }
    return qualifierpick::Mode(id);
}

void QualifierPanel::setPickMode(std::optional<qualifierpick::Mode> mode)
{
    for (QAbstractButton* b : m_picks->buttons()) {
        const bool on = mode && m_picks->id(b) == int(*mode);
        if (b->isChecked() != on)
            b->setChecked(on);
    }
}

bool QualifierPanel::highlightBlackWhite() const
{
    return m_highlightMode->currentIndex() == 1;
}

ValueField* QualifierPanel::field(const QString& name) const
{
    for (const auto& [n, f] : m_fields) {
        if (n == name)
            return f;
    }
    return nullptr;
}

QualifierRangeBar* QualifierPanel::bar(QualifierRangeBar::Channel c) const
{
    return m_bars[int(c)];
}

void QualifierPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    setEnabled(m_session && m_session->hasGrade() && (!n || !n->isMixer()));
    const GradeQualifier q = n ? n->qualifier : GradeQualifier {};
    m_enable->setChecked(q.enabled);
    m_invert->setChecked(q.invert);
    m_mode->setCurrentIndex(m_mode->findData(int(q.mode)));
    m_ranges->setCurrentIndex(q.mode == QualifierMode::Rgb ? 1 : q.mode == QualifierMode::ThreeD ? 2 : 0);
    for (int c = 0; c < 2; ++c) { // Luma keeps only the Lum row
        for (QWidget* w : std::as_const(m_rowWidgets[c]))
            w->setVisible(q.mode != QualifierMode::Luma);
    }
    m_useRgb[0]->setChecked(q.useRed);
    m_useRgb[1]->setChecked(q.useGreen);
    m_useRgb[2]->setChecked(q.useBlue);
    const int picked = int(std::count_if(q.colors.cbegin(), q.colors.cend(),
        [](const GradeQualifierColor& c) { return !c.subtract; }));
    m_colourCount->setText(tr("%1 colors, %2 taken out")
                               .arg(picked)
                               .arg(int(q.colors.size()) - picked));
    m_clearColours->setEnabled(!q.colors.isEmpty());
    m_use[0]->setChecked(q.useHue);
    m_use[1]->setChecked(q.useSat);
    m_use[2]->setChecked(q.useLum);
    m_bars[0]->setRange(q.hueCenter, q.hueWidth, q.hueSoft);
    m_bars[1]->setRange(q.satLow, q.satHigh, q.satSoft);
    m_bars[2]->setRange(q.lumLow, q.lumHigh, q.lumSoft);
    for (int c = 0; c < 3; ++c)
        m_bars[c]->setEnabled(q.enabled);
    const std::pair<const char*, double> values[]
        = {{"hueCenter", q.hueCenter}, {"hueWidth", q.hueWidth}, {"hueSoft", q.hueSoft},
            {"satLow", q.satLow}, {"satHigh", q.satHigh}, {"satSoft", q.satSoft},
            {"lumLow", q.lumLow}, {"lumHigh", q.lumHigh}, {"lumSoft", q.lumSoft},
            {"redLow", q.redLow}, {"redHigh", q.redHigh}, {"redSoft", q.redSoft},
            {"greenLow", q.greenLow}, {"greenHigh", q.greenHigh}, {"greenSoft", q.greenSoft},
            {"blueLow", q.blueLow}, {"blueHigh", q.blueHigh}, {"blueSoft", q.blueSoft},
            {"colorRange", q.colorRange}, {"colorSoft", q.colorSoft},
            {"cleanBlack", q.cleanBlack}, {"cleanWhite", q.cleanWhite},
            {"shrinkGrow", q.shrinkGrow}, {"blurRadius", q.blurRadius},
            {"inOutRatio", q.inOutRatio}, {"denoise", q.denoise}};
    for (const auto& [name, v] : values) {
        if (ValueField* f = field(QLatin1String(name)); f && !f->isScrubbing())
            f->setValue(v);
    }
    m_refreshing = false;
}

// ---- WindowsPanel ----

WindowsPanel::WindowsPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    markVimForm(this);
    setObjectName(QStringLiteral("WindowsPanel"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(8);

    // Left: create buttons and the node's windows.
    auto* left = new QVBoxLayout;
    left->setSpacing(2);
    auto* shapes = new QHBoxLayout;
    shapes->setSpacing(1);
    for (const GradeWindowShape s : {GradeWindowShape::Circle, GradeWindowShape::Linear,
             GradeWindowShape::Polygon, GradeWindowShape::Curve, GradeWindowShape::Gradient}) {
        QToolButton* b = toolButton(shapeName(s), this);
        b->setObjectName(QStringLiteral("WindowAdd_") + gradeWindowShapeToString(s));
        b->setToolTip(s == GradeWindowShape::Curve
                ? tr("Add a curve window to the current node: a Bezier shape whose points have "
                     "tangent handles. Drag the handles in the viewer; double-click a point to "
                     "switch it between smooth and corner, double-click the outline to add a "
                     "point, right-click a point to delete it.")
                : tr("Add a %1 window to the current node").arg(shapeName(s).toLower()));
        connect(b, &QToolButton::clicked, this, [this, s] {
            if (m_session)
                m_session->addWindow(s);
        });
        m_shapes[int(s)] = b;
        shapes->addWidget(b);
    }
    shapes->addStretch(1);
    left->addLayout(shapes);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("WindowList"));
    m_list->setMinimumWidth(150);
    m_list->setMaximumWidth(300);
    textfit::elideListItems(m_list); // capped width: long names end in "...", full in the tip
    left->addWidget(m_list, 1);
    auto* listTools = new QHBoxLayout;
    listTools->setSpacing(1);
    m_invert = toolButton(tr("Invert"), this, true);
    m_invert->setObjectName(QStringLiteral("WindowInvert"));
    m_delete = toolButton(tr("Delete"), this);
    m_delete->setObjectName(QStringLiteral("WindowDelete"));
    m_invert->setToolTip(tr("Select everything outside the window instead of the inside"));
    m_delete->setToolTip(tr("Delete the selected window"));
    listTools->addWidget(m_invert);
    listTools->addWidget(m_delete);
    // Shape keyframes (Polygon and Curve windows).
    m_keyShape = toolButton(tr("Key Shape"), this);
    m_keyShape->setObjectName(QStringLiteral("WindowKeyShape"));
    m_keyShape->setToolTip(tr("Set a shape keyframe at the playhead. From then on every point "
                              "you move writes a key at the playhead, and the shape changes "
                              "smoothly between keys."));
    m_removeShapeKey = toolButton(tr("Remove Key"), this);
    m_removeShapeKey->setObjectName(QStringLiteral("WindowRemoveShapeKey"));
    m_removeShapeKey->setToolTip(tr("Remove the shape keyframe at the playhead"));
    m_clearShapeKeys = toolButton(tr("Clear Keys"), this);
    m_clearShapeKeys->setObjectName(QStringLiteral("WindowClearShapeKeys"));
    m_clearShapeKeys->setToolTip(
        tr("Remove all shape keyframes; the window keeps the shape it shows at the playhead"));
    listTools->addWidget(m_keyShape);
    listTools->addWidget(m_removeShapeKey);
    listTools->addWidget(m_clearShapeKeys);
    listTools->addStretch(1);
    left->addLayout(listTools);
    layout->addLayout(left, 2);

    // Right: the selected window's values.
    m_fieldsBox = new QWidget(this);
    auto* grid = new QGridLayout(m_fieldsBox);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(3);
    struct Spec
    {
        const QString* key;
        double lo, hi, step;
        int decimals;
    };
    const Spec specs[] = {{&wk::Width, 0.0, 4.0, 0.002, 3}, {&wk::Height, 0.0, 4.0, 0.002, 3},
        {&wk::CenterX, -1.0, 2.0, 0.002, 3}, {&wk::CenterY, -1.0, 2.0, 0.002, 3},
        {&wk::Rotation, -360.0, 360.0, 0.5, 1}, {&wk::Opacity, 0.0, 1.0, 0.005, 2},
        {&wk::SoftIn, 0.0, 1.0, 0.001, 3}, {&wk::SoftOut, 0.0, 1.0, 0.001, 3}};
    int i = 0;
    for (const Spec& s : specs) {
        auto* label = new QLabel(wk::label(*s.key), m_fieldsBox);
        ValueField* f = makeField(m_fieldsBox, s.lo, s.hi, s.step, s.decimals);
        f->setObjectName(QStringLiteral("Window_") + *s.key);
        // Two columns of label + field (columns 0-1 and 3-4 would leave a gap: 0, 1 | 2, 3).
        const int col = (i % 2) * 3;
        grid->addWidget(label, i / 2, col, Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(f, i / 2, col + 1);
        const QString key = *s.key;
        connect(f, &ValueField::scrubStarted, this, [this, key] {
            if (m_session)
                m_session->beginGesture(wk::label(key));
        });
        connect(f, &ValueField::scrubFinished, this, [this] {
            if (m_session)
                m_session->endGesture();
        });
        auto change = [this, key](double v) {
            if (!m_refreshing && m_session)
                m_session->setWindowParams(0, {{key, v}}, wk::label(key));
        };
        connect(f, &ValueField::valueScrubbed, this, change);
        connect(f, &ValueField::valueCommitted, this, change);
        m_fields.append({key, f});
        ++i;
    }
    grid->setColumnMinimumWidth(2, 16);
    grid->setColumnStretch(5, 1);
    grid->setRowStretch(4, 1);
    layout->addWidget(m_fieldsBox, 3);

    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (!m_refreshing && item && m_session)
            m_session->selectWindow(item->data(Qt::UserRole).toULongLong());
    });
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (m_refreshing || !m_session)
            return;
        const quint64 id = item->data(Qt::UserRole).toULongLong();
        if (const GradeWindow* w = m_session->window(id))
            m_session->setWindowFlags(id, item->checkState() == Qt::Checked, w->invert);
    });
    connect(m_invert, &QToolButton::toggled, this, [this](bool on) {
        if (m_refreshing || !m_session)
            return;
        if (const GradeWindow* w = m_session->window())
            m_session->setWindowFlags(w->id, w->enabled, on);
    });
    connect(m_delete, &QToolButton::clicked, this, [this] {
        if (m_session)
            m_session->removeWindow();
    });
    connect(m_keyShape, &QToolButton::clicked, this, [this] {
        if (m_session)
            m_session->keyWindowShape();
    });
    connect(m_removeShapeKey, &QToolButton::clicked, this, [this] {
        if (m_session)
            m_session->removeWindowShapeKey();
    });
    connect(m_clearShapeKeys, &QToolButton::clicked, this, [this] {
        if (m_session)
            m_session->clearWindowShapeKeys();
    });
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &WindowsPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &WindowsPanel::refresh);
        connect(m_session, &ColorSession::selectedWindowChanged, this, &WindowsPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &WindowsPanel::refresh);
    }
    refresh();
}

QToolButton* WindowsPanel::shapeButton(GradeWindowShape shape) const
{
    return m_shapes[int(shape)];
}

ValueField* WindowsPanel::field(const QString& key) const
{
    for (const auto& [k, f] : m_fields) {
        if (k == key)
            return f;
    }
    return nullptr;
}

void WindowsPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    const bool usable = m_session && m_session->hasGrade() && (!n || !n->isMixer());
    for (QToolButton* b : m_shapes)
        b->setEnabled(usable && (!n || n->windows.size() < kMaxGradeWindows));
    m_list->clear();
    const quint64 selected = m_session ? m_session->selectedWindow() : 0;
    if (n) {
        int counts[5] = {0, 0, 0, 0, 0};
        for (const GradeWindow& w : n->windows) {
            const int number = ++counts[int(w.shape)];
            auto* item = new QListWidgetItem(
                QStringLiteral("%1 %2").arg(shapeName(w.shape)).arg(number), m_list);
            item->setData(Qt::UserRole, w.id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(w.enabled ? Qt::Checked : Qt::Unchecked);
            if (w.invert)
                item->setText(
                    item->text() + QStringLiteral(" (") + tr("inverted") + QLatin1Char(')'));
            if (!w.shapeKeys.isEmpty())
                item->setText(item->text() + QStringLiteral(" \u25C6")); // a key diamond
            if (w.id == selected)
                m_list->setCurrentItem(item);
        }
    }
    const GradeWindow* w = m_session ? m_session->window() : nullptr;
    m_fieldsBox->setEnabled(w != nullptr);
    m_invert->setEnabled(w != nullptr);
    m_delete->setEnabled(w != nullptr);
    const bool pointShape
        = w && (w->shape == GradeWindowShape::Polygon || w->shape == GradeWindowShape::Curve);
    m_keyShape->setEnabled(pointShape);
    m_removeShapeKey->setEnabled(pointShape && w->shapeKeys.contains(m_session->gradeFrame()));
    m_clearShapeKeys->setEnabled(pointShape && !w->shapeKeys.isEmpty());
    m_invert->setChecked(w && w->invert);
    const int64_t frame = m_session ? m_session->gradeFrame() : 0;
    for (const auto& [key, f] : std::as_const(m_fields)) {
        if (!f->isScrubbing())
            f->setValue(w ? w->value(key, frame) : wk::defaultValue(key));
    }
    m_refreshing = false;
}

// ---- LutBrowser ----

QString LutBrowser::defaultFolder()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/LUT");
    QDir().mkpath(dir);
    return dir;
}

LutBrowser::LutBrowser(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("LutBrowser"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(3);
    auto* tools = new QHBoxLayout;
    tools->setSpacing(2);
    auto* addFolderButton = toolButton(tr("Add Folder…"), this);
    addFolderButton->setObjectName(QStringLiteral("LutAddFolder"));
    auto* loadFile = toolButton(tr("Load File…"), this);
    loadFile->setObjectName(QStringLiteral("LutLoadFile"));
    auto* refreshButton = toolButton(tr("Refresh"), this);
    m_remove = toolButton(tr("Remove LUT"), this);
    m_remove->setObjectName(QStringLiteral("LutRemove"));
    m_interpolation = new QComboBox(this);
    m_interpolation->setObjectName(QStringLiteral("LutInterpolation"));
    m_interpolation->addItem(tr("Tetrahedral"));
    m_interpolation->addItem(tr("Trilinear"));
    // Two rows: the buttons in a QToolBar (what does not fit goes into its "»" menu), and the
    // interpolation below, so nothing overlaps at the narrowest palette width (spec 89.14).
    auto* bar = new QToolBar(this);
    bar->setObjectName(QStringLiteral("LutToolBar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);
    bar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    bar->layout()->setContentsMargins(0, 0, 0, 0);
    bar->layout()->setSpacing(2);
    bar->addWidget(addFolderButton);
    bar->addWidget(loadFile);
    bar->addWidget(refreshButton);
    bar->addWidget(m_remove);
    layout->addWidget(bar);
    tools->addWidget(new QLabel(tr("Interpolation"), this));
    tools->addWidget(m_interpolation);
    tools->addStretch(1);
    layout->addLayout(tools);
    // A folder of LUTs easily holds hundreds: type part of a name to narrow the list.
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("LutSearch"));
    m_search->setPlaceholderText(tr("Search LUTs"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    connect(m_search, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("LutTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setToolTip(tr("Double-click a LUT to apply it to the current node"));
    layout->addWidget(m_tree, 1);
    m_current = new QLabel(this);
    m_current->setObjectName(QStringLiteral("LutCurrent"));
    m_current->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_current);

    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
        [this](QTreeWidgetItem* item) { applyItem(item); });
    connect(addFolderButton, &QToolButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Add LUT Folder"));
        if (!dir.isEmpty())
            addFolder(dir);
    });
    connect(loadFile, &QToolButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Load LUT"), defaultFolder(), tr("LUT files (*.cube)"));
        if (!file.isEmpty() && m_session)
            m_session->setLut(0, file);
    });
    connect(refreshButton, &QToolButton::clicked, this, &LutBrowser::rescan);
    connect(m_remove, &QToolButton::clicked, this, [this] {
        if (m_session)
            m_session->setLut(0, {});
    });
    connect(m_interpolation, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_refreshing || !m_session || !m_session->currentNode())
            return;
        ClipGrade g = m_session->grade();
        GradeNode* n = g.node(m_session->selectedNode());
        if (!n)
            return;
        n->lutInterpolation
            = index == 1 ? LutInterpolation::Trilinear : LutInterpolation::Tetrahedral;
        if (g != m_session->grade())
            m_session->commit(g, tr("LUT Interpolation"));
    });
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &LutBrowser::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &LutBrowser::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &LutBrowser::refresh);
    }
    rescan();
    refresh();
}

QStringList LutBrowser::folders()
{
    QStringList list = QSettings().value(QLatin1String(kLutFoldersKey)).toStringList();
    const QString def = defaultFolder();
    if (!list.contains(def))
        list.prepend(def);
    return list;
}

void LutBrowser::addFolder(const QString& folder)
{
    QStringList list = QSettings().value(QLatin1String(kLutFoldersKey)).toStringList();
    const QString clean = QDir::cleanPath(folder);
    if (!list.contains(clean)) {
        list.append(clean);
        QSettings().setValue(QLatin1String(kLutFoldersKey), list);
    }
    rescan();
}

void LutBrowser::rescan()
{
    m_tree->clear();
    for (const QString& folder : folders()) {
        auto* root = new QTreeWidgetItem(m_tree, {QFileInfo(folder).fileName()});
        root->setToolTip(0, folder);
        QStringList files;
        QDirIterator it(folder, {QStringLiteral("*.cube"), QStringLiteral("*.CUBE")}, QDir::Files,
            QDirIterator::Subdirectories);
        while (it.hasNext())
            files.append(it.next());
        files.sort(Qt::CaseInsensitive);
        for (const QString& f : std::as_const(files)) {
            auto* item = new QTreeWidgetItem(root, {QDir(folder).relativeFilePath(f)});
            item->setData(0, Qt::UserRole, f);
            item->setToolTip(0, f);
        }
        if (files.isEmpty()) {
            auto* empty = new QTreeWidgetItem(root, {tr("(no .cube files)")});
            empty->setFlags(Qt::NoItemFlags);
        }
        root->setExpanded(true);
    }
    applyFilter();
}

void LutBrowser::applyFilter()
{
    const QStringList words
        = m_search->text().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int r = 0; r < m_tree->topLevelItemCount(); ++r) {
        QTreeWidgetItem* root = m_tree->topLevelItem(r);
        int shown = 0;
        for (int i = 0; i < root->childCount(); ++i) {
            QTreeWidgetItem* item = root->child(i);
            bool hit = true;
            for (const QString& w : words)
                hit = hit && item->text(0).toLower().contains(w);
            item->setHidden(!hit);
            shown += hit ? 1 : 0;
        }
        root->setHidden(!words.isEmpty() && shown == 0); // a folder with no match gives way
    }
}

void LutBrowser::applyItem(QTreeWidgetItem* item)
{
    const QString path = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (!path.isEmpty() && m_session)
        m_session->setLut(0, path);
}

void LutBrowser::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    const bool usable = m_session && m_session->hasGrade() && (!n || !n->isMixer());
    m_tree->setEnabled(usable);
    m_remove->setEnabled(n && !n->lutPath.isEmpty());
    m_interpolation->setEnabled(n && !n->lutPath.isEmpty());
    m_interpolation->setCurrentIndex(
        n && n->lutInterpolation == LutInterpolation::Trilinear ? 1 : 0);
    m_current->setText(n && !n->lutPath.isEmpty()
            ? tr("Node LUT: %1").arg(QFileInfo(n->lutPath).fileName())
            : tr("The current node has no LUT"));
    m_refreshing = false;
}

// ---- KeyPanel ----

KeyPanel::KeyPanel(ColorSession* session, ColorFrameSource* frames, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_frames(frames)
{
    markVimForm(this);
    setObjectName(QStringLiteral("KeyPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(3);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Key Output Gain"), this));
    m_gain = makeField(this, 0.0, 1.0, 0.005, 3);
    m_gain->setObjectName(QStringLiteral("KeyGain"));
    row->addWidget(m_gain, 1);
    m_invert = new QCheckBox(tr("Invert Key"), this);
    m_invert->setObjectName(QStringLiteral("KeyInvert"));
    row->addWidget(m_invert);
    layout->addLayout(row);
    m_matteArea = new QWidget(this);
    m_matteArea->setMinimumHeight(60);
    layout->addWidget(m_matteArea, 1);

    connect(m_gain, &ValueField::scrubStarted, this, [this] {
        if (m_session)
            m_session->beginGesture(tr("Key Output Gain"));
    });
    connect(m_gain, &ValueField::scrubFinished, this, [this] {
        if (m_session)
            m_session->endGesture();
    });
    auto change = [this](double v) {
        if (!m_refreshing && m_session)
            m_session->setParam(0, gradekeys::Mix, v, tr("Key Output Gain"));
    };
    connect(m_gain, &ValueField::valueScrubbed, this, change);
    connect(m_gain, &ValueField::valueCommitted, this, change);
    connect(m_invert, &QCheckBox::toggled, this, [this](bool on) {
        if (m_refreshing || !m_session || !m_session->currentNode())
            return;
        GradeQualifier q = m_session->currentNode()->qualifier;
        q.invert = on;
        m_session->setQualifier(0, q, tr("Invert Key"));
    });
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &KeyPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &KeyPanel::refresh);
    }
    if (m_frames)
        connect(m_frames, &ColorFrameSource::inputChanged, this, &KeyPanel::updateMatte);
    refresh();
}

void KeyPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    setEnabled(n && !n->isMixer());
    if (!m_gain->isScrubbing())
        m_gain->setValue(n ? n->value(gradekeys::Mix, m_session->gradeFrame()) : 1.0);
    m_invert->setChecked(n && n->qualifier.invert);
    m_refreshing = false;
    updateMatte();
}

void KeyPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateMatte();
}

void KeyPanel::updateMatte()
{
    if (!isVisible() || !m_frames || !m_session)
        return;
    m_matte = m_frames->highlight(
        m_session->selectedNode(), ColorFrameSource::HighlightMode::BlackWhite);
    update();
}

void KeyPanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QRect area = m_matteArea->geometry();
    p.fillRect(area, QColor(16, 16, 18));
    if (m_matte.isNull()) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(area, Qt::AlignCenter, tr("Key"));
        return;
    }
    const QSize s = m_matte.size().scaled(area.size(), Qt::KeepAspectRatio);
    const QRect target(area.center() - QPoint(s.width() / 2, s.height() / 2), s);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(target, m_matte);
}

// ---- BlurPanel ----

namespace {

/// The radius keys that the link button of a mode moves together.
const QStringList& radiusGroup(int mode)
{
    static const QStringList blur = {
        gradekeys::BlurRadiusR, gradekeys::BlurRadiusG, gradekeys::BlurRadiusB};
    static const QStringList sharp = {
        gradekeys::SharpRadiusR, gradekeys::SharpRadiusG, gradekeys::SharpRadiusB};
    static const QStringList none;
    return mode == 0 ? blur : (mode == 1 ? sharp : none);
}

} // namespace

BlurPanel::BlurPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    markVimForm(this);
    setObjectName(QStringLiteral("BlurPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(3);
    auto* modes = new QHBoxLayout;
    auto* group = new QButtonGroup(this);
    const QString names[3] = {tr("Blur"), tr("Sharpen"), tr("Mist")};
    for (int i = 0; i < 3; ++i) {
        m_modeButtons[i] = toolButton(names[i], this, true);
        m_modeButtons[i]->setProperty("kkTab", true); // Kinoklip style: red underline when checked
        m_modeButtons[i]->setObjectName(QStringLiteral("BlurMode_") + QString::number(i));
        group->addButton(m_modeButtons[i], i);
        modes->addWidget(m_modeButtons[i]);
    }
    modes->addStretch(1);
    m_modeButtons[0]->setChecked(true);
    layout->addLayout(modes);
    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);
    connect(group, &QButtonGroup::idClicked, m_stack, &QStackedWidget::setCurrentIndex);

    struct Spec
    {
        const QString* key;
        QString label;
        double step;
    };
    namespace gk = gradekeys;
    auto addPage = [&](int mode, const QVector<Spec>& specs, bool radiusRow) {
        auto* page = new QWidget;
        auto* grid = new QGridLayout(page);
        grid->setContentsMargins(0, 2, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(3);
        int row = 0;
        for (const Spec& s : specs) {
            grid->addWidget(new QLabel(s.label, page), row, 0);
            ValueField* f = makeField(page, 0.0, 1.0, s.step, 2);
            f->setObjectName(QStringLiteral("Blur_") + *s.key);
            grid->addWidget(f, row, 1);
            bindField(f, *s.key);
            if (radiusRow && row == 0) {
                m_links[mode] = toolButton(tr("Link"), page, true);
                m_links[mode]->setObjectName(QStringLiteral("BlurLink_") + QString::number(mode));
                m_links[mode]->setChecked(true);
                m_links[mode]->setToolTip(tr("Edit Radius R, G and B together"));
                grid->addWidget(m_links[mode], 0, 2, 3, 1, Qt::AlignTop);
            }
            ++row;
        }
        grid->setColumnStretch(3, 1);
        grid->setRowStretch(row, 1);
        m_stack->addWidget(page);
    };
    addPage(0,
        {{&gk::BlurRadiusR, tr("Radius R"), 0.005}, {&gk::BlurRadiusG, tr("Radius G"), 0.005},
            {&gk::BlurRadiusB, tr("Radius B"), 0.005}, {&gk::BlurRatio, tr("H/V Ratio"), 0.005},
            {&gk::BlurScaling, tr("Scaling"), 0.005}},
        true);
    addPage(1,
        {{&gk::SharpRadiusR, tr("Radius R"), 0.005}, {&gk::SharpRadiusG, tr("Radius G"), 0.005},
            {&gk::SharpRadiusB, tr("Radius B"), 0.005}, {&gk::SharpRatio, tr("H/V Ratio"), 0.005},
            {&gk::SharpScaling, tr("Scaling"), 0.005},
            {&gk::SharpCoring, tr("Coring Softness"), 0.005},
            {&gk::SharpLevel, tr("Level"), 0.005}},
        true);
    addPage(2,
        {{&gk::MistRadius, tr("Radius"), 0.005}, {&gk::MistRatio, tr("H/V Ratio"), 0.005},
            {&gk::MistScaling, tr("Scaling"), 0.005}, {&gk::MistMix, tr("Mix"), 0.005}},
        false);

    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &BlurPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &BlurPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &BlurPanel::refresh);
    }
    refresh();
}

int BlurPanel::mode() const
{
    return m_stack->currentIndex();
}

void BlurPanel::setMode(int mode)
{
    mode = std::clamp(mode, 0, 2);
    m_stack->setCurrentIndex(mode);
    m_modeButtons[mode]->setChecked(true);
}

ValueField* BlurPanel::field(const QString& key) const
{
    for (const auto& [k, f] : m_fields) {
        if (k == key)
            return f;
    }
    return nullptr;
}

void BlurPanel::setValueOf(const QString& key, double value)
{
    if (m_refreshing || !m_session)
        return;
    const QString label = gradekeys::label(key);
    for (int mode = 0; mode < 2; ++mode) {
        const QStringList& group = radiusGroup(mode);
        if (group.contains(key) && m_links[mode] && m_links[mode]->isChecked()) {
            QVector<std::pair<QString, double>> values;
            for (const QString& k : group)
                values.append({k, value});
            m_session->setParams(0, values, label);
            return;
        }
    }
    m_session->setParam(0, key, value, label);
}

void BlurPanel::bindField(ValueField* f, const QString& key)
{
    connect(f, &ValueField::scrubStarted, this, [this, key] {
        if (m_session)
            m_session->beginGesture(gradekeys::label(key));
    });
    connect(f, &ValueField::valueScrubbed, this, [this, key](double v) { setValueOf(key, v); });
    connect(f, &ValueField::scrubFinished, this, [this] {
        if (m_session)
            m_session->endGesture();
    });
    connect(f, &ValueField::valueCommitted, this, [this, key](double v) { setValueOf(key, v); });
    m_fields.append({key, f});
}

void BlurPanel::resetAll()
{
    if (!m_session || !m_session->currentNode())
        return;
    QVector<std::pair<QString, double>> values;
    for (const QString& key : gradekeys::blurKeys())
        values.append({key, gradekeys::defaultValue(key)});
    m_session->setParams(0, values, tr("Reset Blur"));
}

void BlurPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    setEnabled(n && !n->isMixer());
    const int64_t frame = m_session ? m_session->gradeFrame() : 0;
    for (const auto& [key, f] : std::as_const(m_fields)) {
        if (!f->isScrubbing())
            f->setValue(n ? n->value(key, frame) : gradekeys::defaultValue(key));
    }
    m_refreshing = false;
}

// ---- RgbMixerPanel ----

RgbMixerPanel::RgbMixerPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    markVimForm(this);
    setObjectName(QStringLiteral("RgbMixerPanel"));
    namespace gk = gradekeys;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(3);
    auto* top = new QHBoxLayout;
    auto* group = new QButtonGroup(this);
    const QString names[3] = {tr("Red"), tr("Green"), tr("Blue")};
    for (int i = 0; i < 3; ++i) {
        m_tabs[i] = toolButton(names[i], this, true);
        m_tabs[i]->setObjectName(QStringLiteral("RgbMixerTab_") + QString::number(i));
        group->addButton(m_tabs[i], i);
        top->addWidget(m_tabs[i]);
    }
    m_tabs[0]->setChecked(true);
    top->addSpacing(12);
    m_mono = new QCheckBox(tr("Monochrome"), this);
    m_mono->setObjectName(QStringLiteral("RgbMixerMonochrome"));
    m_preserve = new QCheckBox(tr("Preserve Luminance"), this);
    m_preserve->setObjectName(QStringLiteral("RgbMixerPreserve"));
    top->addWidget(m_mono);
    top->addWidget(m_preserve);
    top->addStretch(1);
    layout->addLayout(top);
    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);
    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        m_tab = id;
        refresh();
    });

    struct Spec
    {
        const QString* key;
        QString label;
    };
    auto addPage = [&](const QVector<Spec>& specs) {
        auto* page = new QWidget;
        auto* grid = new QGridLayout(page);
        grid->setContentsMargins(0, 2, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(3);
        int row = 0;
        for (const Spec& s : specs) {
            grid->addWidget(new QLabel(s.label, page), row, 0);
            ValueField* f = makeField(page, -2.0, 2.0, 0.01, 2);
            f->setObjectName(QStringLiteral("RgbMixer_") + *s.key);
            grid->addWidget(f, row, 1);
            bindField(f, *s.key);
            ++row;
        }
        grid->setColumnStretch(2, 1);
        grid->setRowStretch(row, 1);
        m_stack->addWidget(page);
    };
    addPage({{&gk::MixerRR, tr("Red")}, {&gk::MixerRG, tr("Green")}, {&gk::MixerRB, tr("Blue")}});
    addPage({{&gk::MixerGR, tr("Red")}, {&gk::MixerGG, tr("Green")}, {&gk::MixerGB, tr("Blue")}});
    addPage({{&gk::MixerBR, tr("Red")}, {&gk::MixerBG, tr("Green")}, {&gk::MixerBB, tr("Blue")}});
    addPage({{&gk::MixerMonoR, tr("Red")}, {&gk::MixerMonoG, tr("Green")},
        {&gk::MixerMonoB, tr("Blue")}});

    connect(m_mono, &QCheckBox::toggled, this, [this](bool on) {
        setValueOf(gradekeys::MixerMono, on ? 1.0 : 0.0);
    });
    connect(m_preserve, &QCheckBox::toggled, this, [this](bool on) {
        setValueOf(gradekeys::MixerPreserve, on ? 1.0 : 0.0);
    });
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &RgbMixerPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &RgbMixerPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &RgbMixerPanel::refresh);
    }
    refresh();
}

int RgbMixerPanel::page() const
{
    return m_stack->currentIndex();
}

ValueField* RgbMixerPanel::field(const QString& key) const
{
    for (const auto& [k, f] : m_fields) {
        if (k == key)
            return f;
    }
    return nullptr;
}

void RgbMixerPanel::setValueOf(const QString& key, double value)
{
    if (m_refreshing || !m_session)
        return;
    m_session->setParam(0, key, value, gradekeys::label(key));
}

void RgbMixerPanel::bindField(ValueField* f, const QString& key)
{
    connect(f, &ValueField::scrubStarted, this, [this, key] {
        if (m_session)
            m_session->beginGesture(gradekeys::label(key));
    });
    connect(f, &ValueField::valueScrubbed, this, [this, key](double v) { setValueOf(key, v); });
    connect(f, &ValueField::scrubFinished, this, [this] {
        if (m_session)
            m_session->endGesture();
    });
    connect(f, &ValueField::valueCommitted, this, [this, key](double v) { setValueOf(key, v); });
    m_fields.append({key, f});
}

void RgbMixerPanel::resetAll()
{
    if (!m_session || !m_session->currentNode())
        return;
    QVector<std::pair<QString, double>> values;
    for (const QString& key : gradekeys::rgbMixerKeys())
        values.append({key, gradekeys::defaultValue(key)});
    m_session->setParams(0, values, tr("Reset RGB Mixer"));
}

void RgbMixerPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    setEnabled(n && !n->isMixer());
    const int64_t frame = m_session ? m_session->gradeFrame() : 0;
    for (const auto& [key, f] : std::as_const(m_fields)) {
        if (!f->isScrubbing())
            f->setValue(n ? n->value(key, frame) : gradekeys::defaultValue(key));
    }
    const bool mono = n && n->value(gradekeys::MixerMono, frame) >= 0.5;
    m_mono->setChecked(mono);
    m_preserve->setChecked(n && n->value(gradekeys::MixerPreserve, frame) >= 0.5);
    for (QToolButton* t : m_tabs)
        t->setEnabled(!mono);
    m_stack->setCurrentIndex(mono ? 3 : m_tab);
    m_refreshing = false;
}

// ---- AgxPanel ----

AgxPanel::AgxPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    markVimForm(this);
    setObjectName(QStringLiteral("AgxPanel"));
    namespace gk = gradekeys;
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(6, 4, 6, 4);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    m_on = new QCheckBox(tr("Enable AgX"), this);
    m_on->setObjectName(QStringLiteral("AgxEnable"));
    m_on->setToolTip(tr("Tone map this node's picture with AgX, Blender's view transform: it "
                        "compresses bright highlights smoothly instead of clipping them. It runs "
                        "after the node's other corrections."));
    grid->addWidget(m_on, 0, 0, 1, 2);
    const auto addRow = [&](int row, const QString& label, QWidget* w, const QString& tip) {
        auto* l = new QLabel(label, this);
        l->setToolTip(tip);
        w->setToolTip(tip);
        grid->addWidget(l, row, 0);
        grid->addWidget(w, row, 1);
    };
    m_input = new QComboBox(this);
    m_input->setObjectName(QStringLiteral("AgxInput"));
    for (const QString& n : {tr("Rec.709 (gamma 2.4)"), tr("sRGB"), tr("Linear"), tr("Apple Log"),
             tr("S-Log3"), tr("V-Log"), tr("LogC3 (EI 800)")})
        m_input->addItem(n);
    addRow(1, tr("Input"), m_input,
        tr("What the numbers of the picture mean. Rec.709 (gamma 2.4) is ordinary video. A camera "
           "log is undone first; only its curve is handled, the colours are taken as Rec.709."));
    m_exposure = makeField(this, -8.0, 8.0, 0.05, 2);
    m_exposure->setObjectName(QStringLiteral("AgxExposure"));
    m_exposure->setSuffix(tr(" stops"));
    addRow(2, tr("Exposure"), m_exposure,
        tr("Brightens (+) or darkens (-) the linear picture in stops before AgX. Each stop "
           "doubles or halves the light."));
    m_look = new QComboBox(this);
    m_look->setObjectName(QStringLiteral("AgxLook"));
    for (const QString& n : {tr("None"), tr("Punchy"), tr("Golden"), tr("Greyscale")})
        m_look->addItem(n);
    addRow(3, tr("Look"), m_look,
        tr("A creative grade on top of AgX: Punchy adds contrast and colour, Golden warms the "
           "picture, Greyscale removes the colour. None is plain AgX."));
    m_output = new QComboBox(this);
    m_output->setObjectName(QStringLiteral("AgxOutput"));
    for (const QString& n : {tr("Rec.709 (gamma 2.4)"), tr("sRGB")})
        m_output->addItem(n);
    addRow(4, tr("Output"), m_output,
        tr("How the result is encoded: Rec.709 for video shown on a gamma 2.4 screen, sRGB for "
           "pictures and the web."));
    grid->setColumnStretch(2, 1);
    grid->setRowStretch(5, 1);

    connect(m_on, &QCheckBox::toggled, this,
        [this](bool on) { setValueOf(gradekeys::AgxOn, on ? 1.0 : 0.0); });
    const auto bindCombo = [this](QComboBox* box, const QString& key) {
        connect(box, &QComboBox::activated, this,
            [this, key](int i) { setValueOf(key, double(i)); });
    };
    bindCombo(m_input, gk::AgxInput);
    bindCombo(m_look, gk::AgxLook);
    bindCombo(m_output, gk::AgxOutput);
    connect(m_exposure, &ValueField::scrubStarted, this, [this] {
        if (m_session)
            m_session->beginGesture(gradekeys::label(gradekeys::AgxExposure));
    });
    connect(m_exposure, &ValueField::valueScrubbed, this,
        [this](double v) { setValueOf(gradekeys::AgxExposure, v); });
    connect(m_exposure, &ValueField::scrubFinished, this, [this] {
        if (m_session)
            m_session->endGesture();
    });
    connect(m_exposure, &ValueField::valueCommitted, this,
        [this](double v) { setValueOf(gradekeys::AgxExposure, v); });
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &AgxPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &AgxPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &AgxPanel::refresh);
    }
    refresh();
}

void AgxPanel::setValueOf(const QString& key, double value)
{
    if (m_refreshing || !m_session)
        return;
    m_session->setParam(0, key, value, gradekeys::label(key));
}

void AgxPanel::resetAll()
{
    if (!m_session || !m_session->currentNode())
        return;
    QVector<std::pair<QString, double>> values;
    for (const QString& key : gradekeys::agxKeys())
        values.append({key, gradekeys::defaultValue(key)});
    m_session->setParams(0, values, tr("Reset AgX"));
}

void AgxPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    setEnabled(n && !n->isMixer());
    const int64_t frame = m_session ? m_session->gradeFrame() : 0;
    const grademath::AgxValues v = n ? grademath::AgxValues::at(*n, frame) : grademath::AgxValues {};
    m_on->setChecked(v.on);
    m_input->setCurrentIndex(int(v.input));
    m_look->setCurrentIndex(int(v.look));
    m_output->setCurrentIndex(int(v.output));
    if (!m_exposure->isScrubbing())
        m_exposure->setValue(double(v.exposure));
    m_refreshing = false;
}


// ---- WarperGrid ----

namespace {
constexpr double kPinRadius = 5.0;
constexpr double kPinPick = 9.0;
constexpr double kTwoPi = 6.283185307179586;

/// Colour of the Hue - Sat disc at hue @p h (turns) and saturation @p s (0..1).
QColor warpDiscColour(double h, double s)
{
    const double y = 0.6;
    const double ch = s * grademath::kWarpChromaMax;
    const double cb = ch * std::cos(kTwoPi * h);
    const double cr = ch * std::sin(kTwoPi * h);
    const double r = y + grademath::kWarpCrScale * cr;
    const double b = y + grademath::kWarpCbScale * cb;
    const double g = (y - grademath::kLumaR * r - grademath::kLumaB * b) / grademath::kLumaG;
    return QColor::fromRgbF(
        float(std::clamp(r, 0.0, 1.0)), float(std::clamp(g, 0.0, 1.0)), float(std::clamp(b, 0.0, 1.0)));
}
} // namespace

WarperGrid::WarperGrid(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("WarperGrid"));
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(tr("Click a pin to select it and drag it to move that colour; double-click or "
                  "Delete puts it back; arrow keys nudge it; Shift while dragging is fine "
                  "control"));
}

void WarperGrid::setGrid(WarpGrid grid)
{
    if (grid == m_grid)
        return;
    m_grid = grid;
    m_selected = {-1, -1};
    m_dragging = false;
    update();
    Q_EMIT selectionChanged();
}

void WarperGrid::setWarp(const GradeWarp& warp)
{
    if (warp == m_warp)
        return;
    m_warp = warp;
    update();
}

void WarperGrid::selectPin(int col, int row)
{
    const QPoint p = isMovable(col, row) ? QPoint(col, row) : QPoint(-1, -1);
    if (p == m_selected)
        return;
    m_selected = p;
    update();
    Q_EMIT selectionChanged();
}

bool WarperGrid::isMovable(int col, int row) const
{
    const auto [cols, rows] = warpGridSize(m_grid, m_warp.resolution);
    if (col < 0 || row < 0 || col >= cols || row >= rows)
        return false;
    return m_grid == WarpGrid::ChromaLuma || row > 0; // Hue - Sat row 0 is the grey centre
}

QRectF WarperGrid::plotRect() const
{
    const QRectF area = QRectF(rect()).adjusted(10, 10, -10, -10);
    if (m_grid == WarpGrid::ChromaLuma)
        return area;
    const double side = std::max(10.0, std::min(area.width(), area.height()));
    return QRectF(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
}

QPointF WarperGrid::baseUnit(int col, int row) const
{
    const auto [cols, rows] = warpGridSize(m_grid, m_warp.resolution);
    if (m_grid == WarpGrid::HueSat)
        return QPointF(double(col) / cols, double(row) / (rows - 1));
    return QPointF(double(col) / (cols - 1), double(row) / (rows - 1));
}

QPointF WarperGrid::unitToPixel(QPointF u) const
{
    const QRectF r = plotRect();
    if (m_grid == WarpGrid::HueSat) {
        const double radius = r.width() / 2;
        const double a = kTwoPi * u.x();
        // x = Cb to the right, y = Cr up: the same orientation as the colour wheels.
        return QPointF(r.center().x() + radius * u.y() * std::cos(a),
            r.center().y() - radius * u.y() * std::sin(a));
    }
    return QPointF(r.left() + u.x() * r.width(), r.bottom() - u.y() * r.height());
}

QPointF WarperGrid::pixelToUnit(QPointF p) const
{
    const QRectF r = plotRect();
    if (m_grid == WarpGrid::HueSat) {
        const double radius = r.width() / 2;
        const double dx = p.x() - r.center().x();
        const double dy = r.center().y() - p.y();
        double h = std::atan2(dy, dx) / kTwoPi;
        h -= std::floor(h);
        return QPointF(h, std::hypot(dx, dy) / radius);
    }
    return QPointF((p.x() - r.left()) / r.width(), (r.bottom() - p.y()) / r.height());
}

QPointF WarperGrid::maxDisplacement() const
{
    const auto [cols, rows] = warpGridSize(m_grid, m_warp.resolution);
    return m_grid == WarpGrid::HueSat ? QPointF(1.0 / cols, 1.0 / (rows - 1))
                                       : QPointF(1.0 / (cols - 1), 1.0 / (rows - 1));
}

QPointF WarperGrid::pinPosition(int col, int row) const
{
    const auto [dx, dy] = m_warp.pin(m_grid, col, row);
    const QPointF b = baseUnit(col, row);
    return unitToPixel(QPointF(b.x() + dx, b.y() + dy));
}

QPoint WarperGrid::pinAt(QPointF pos) const
{
    const auto [cols, rows] = warpGridSize(m_grid, m_warp.resolution);
    QPoint best(-1, -1);
    double bestDist = kPinPick;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            if (!isMovable(col, row))
                continue;
            const QPointF d = pinPosition(col, row) - pos;
            const double dist = std::hypot(d.x(), d.y());
            if (dist <= bestDist) {
                bestDist = dist;
                best = QPoint(col, row);
            }
        }
    }
    return best;
}

void WarperGrid::setDisplacement(int col, int row, QPointF d, const QString& label)
{
    if (!isMovable(col, row))
        return;
    const QPointF m = maxDisplacement();
    d.setX(m_grid == WarpGrid::ChromaLuma && col == 0 ? 0.0 : std::clamp(d.x(), -m.x(), m.x()));
    d.setY(std::clamp(d.y(), -m.y(), m.y()));
    const auto [ox, oy] = m_warp.pin(m_grid, col, row);
    if (float(d.x()) == ox && float(d.y()) == oy)
        return;
    m_warp.setPin(m_grid, col, row, float(d.x()), float(d.y()));
    update();
    Q_EMIT warpChanged(m_warp, label);
}

void WarperGrid::resetSelectedPin()
{
    if (m_selected.x() >= 0)
        setDisplacement(m_selected.x(), m_selected.y(), QPointF(0, 0), tr("Reset Pin"));
}

void WarperGrid::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    const auto [cols, rows] = warpGridSize(m_grid, m_warp.resolution);
    if (m_grid == WarpGrid::HueSat) {
        const int side = int(r.width());
        if (m_disc.width() != side) {
            m_disc = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
            m_disc.fill(Qt::transparent);
            for (int y = 0; y < side; ++y) {
                for (int x = 0; x < side; ++x) {
                    const double dx = (x + 0.5) - side / 2.0;
                    const double dy = side / 2.0 - (y + 0.5);
                    const double s = std::hypot(dx, dy) / (side / 2.0);
                    if (s > 1.0)
                        continue;
                    double h = std::atan2(dy, dx) / kTwoPi;
                    h -= std::floor(h);
                    QColor c = warpDiscColour(h, s);
                    c.setAlphaF(0.55);
                    m_disc.setPixelColor(x, y, c);
                }
            }
        }
        p.drawImage(r.topLeft(), m_disc);
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(r);
    } else {
        QLinearGradient g(r.bottomLeft(), r.topLeft());
        g.setColorAt(0.0, QColor(10, 10, 10));
        g.setColorAt(1.0, QColor(235, 235, 235));
        p.fillRect(r, g);
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        p.drawRect(r);
    }
    // The undisplaced mesh, faint, then the displaced one.
    const auto vertex = [&](int col, int row, bool moved) {
        if (!moved)
            return unitToPixel(baseUnit(col, row));
        return pinPosition(col, row);
    };
    for (const bool moved : {false, true}) {
        QColor line = palette().color(QPalette::Text);
        line.setAlphaF(moved ? 0.8 : 0.22);
        p.setPen(QPen(line, 1));
        if (!moved && m_warp.isIdentity(m_grid))
            continue; // the displaced mesh is the same: draw it once
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < cols; ++col) {
                const QPointF a = vertex(col, row, moved);
                if (col + 1 < cols || m_grid == WarpGrid::HueSat) {
                    const int nc = (col + 1) % cols;
                    if (row > 0 || m_grid == WarpGrid::ChromaLuma)
                        p.drawLine(a, vertex(nc, row, moved));
                }
                if (row + 1 < rows)
                    p.drawLine(a, vertex(col, row + 1, moved));
            }
        }
    }
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            if (!isMovable(col, row))
                continue;
            const bool selected = m_selected == QPoint(col, row);
            const auto [dx, dy] = m_warp.pin(m_grid, col, row);
            const bool moved = dx != 0.0f || dy != 0.0f;
            p.setPen(QPen(selected ? palette().color(QPalette::Highlight) : QColor(20, 20, 20), selected ? 2 : 1));
            p.setBrush(moved ? palette().color(QPalette::Highlight) : palette().color(QPalette::Base));
            p.drawEllipse(pinPosition(col, row), kPinRadius, kPinRadius);
        }
    }
}

void WarperGrid::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    setFocus();
    const QPoint hit = pinAt(event->position());
    selectPin(hit.x(), hit.y());
    if (hit.x() < 0)
        return;
    m_dragging = true;
    m_pressPos = event->position();
    m_grabOffset = event->position() - pinPosition(hit.x(), hit.y());
    const auto [dx, dy] = m_warp.pin(m_grid, hit.x(), hit.y());
    m_pressDisplacement = QPointF(dx, dy);
    Q_EMIT dragStarted();
}

void WarperGrid::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging || m_selected.x() < 0)
        return;
    const bool fine = event->modifiers() & Qt::ShiftModifier;
    // The pin follows the mouse (a quarter of the travel with Shift).
    const QPointF pressedPin = m_pressPos - m_grabOffset;
    const QPointF pos = pressedPin + (event->position() - m_pressPos) * (fine ? 0.25 : 1.0);
    const QPointF b = baseUnit(m_selected.x(), m_selected.y());
    QPointF target = pixelToUnit(pos);
    double dx = target.x() - b.x();
    if (m_grid == WarpGrid::HueSat) {
        dx -= std::round(dx); // the hue wraps: the short way round
    }
    setDisplacement(m_selected.x(), m_selected.y(), QPointF(dx, target.y() - b.y()),
        m_grid == WarpGrid::HueSat ? tr("Warp Hue - Sat") : tr("Warp Chroma - Luma"));
}

void WarperGrid::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_dragging)
        return;
    m_dragging = false;
    Q_EMIT dragFinished();
}

void WarperGrid::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint hit = pinAt(event->position());
    if (hit.x() < 0)
        return;
    selectPin(hit.x(), hit.y());
    resetSelectedPin();
}

bool WarperGrid::event(QEvent* e)
{
    if (e->type() == QEvent::ShortcutOverride && m_selected.x() >= 0) {
        switch (static_cast<QKeyEvent*>(e)->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            e->accept();
            return true;
        default:
            break;
        }
    }
    return QWidget::event(e);
}

void WarperGrid::keyPressEvent(QKeyEvent* event)
{
    if (m_selected.x() < 0) {
        QWidget::keyPressEvent(event);
        return;
    }
    const QPointF m = maxDisplacement();
    const double step = (event->modifiers() & Qt::ShiftModifier) ? 0.25 : 0.05;
    QPointF delta;
    switch (event->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        resetSelectedPin();
        return;
    case Qt::Key_Left:
        delta = QPointF(-step * m.x(), 0);
        break;
    case Qt::Key_Right:
        delta = QPointF(step * m.x(), 0);
        break;
    case Qt::Key_Up:
        delta = QPointF(0, step * m.y());
        break;
    case Qt::Key_Down:
        delta = QPointF(0, -step * m.y());
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    const auto [dx, dy] = m_warp.pin(m_grid, m_selected.x(), m_selected.y());
    setDisplacement(m_selected.x(), m_selected.y(), QPointF(dx, dy) + delta,
        m_grid == WarpGrid::HueSat ? tr("Warp Hue - Sat") : tr("Warp Chroma - Luma"));
}

// ---- WarperPanel ----

WarperPanel::WarperPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    markVimForm(this);
    setObjectName(QStringLiteral("WarperPanel"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 4, 6, 4);
    row->setSpacing(8);
    m_gridWidget = new WarperGrid(this);
    row->addWidget(m_gridWidget, 1);
    auto* side = new QVBoxLayout;
    side->setSpacing(4);
    const auto tab = [this](const QString& text, const QString& name) {
        auto* b = new QToolButton(this);
        b->setObjectName(name);
        b->setText(text);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setProperty("kkTab", true);
        return b;
    };
    m_hueSat = tab(tr("Hue - Sat"), QStringLiteral("WarperHueSat"));
    m_chromaLuma = tab(tr("Chroma - Luma"), QStringLiteral("WarperChromaLuma"));
    m_hueSat->setToolTip(tr("Move colours by hue and saturation: the angle is the hue, the "
                            "distance from the centre the saturation"));
    m_chromaLuma->setToolTip(
        tr("Move colours by chroma and luma: chroma to the right, brightness up"));
    auto* group = new QButtonGroup(this);
    group->addButton(m_hueSat, 0);
    group->addButton(m_chromaLuma, 1);
    m_hueSat->setChecked(true);
    auto* tabs = new QHBoxLayout;
    tabs->addWidget(m_hueSat);
    tabs->addWidget(m_chromaLuma);
    side->addLayout(tabs);
    auto* resolutionRow = new QHBoxLayout;
    auto* resolutionLabel = new QLabel(tr("Resolution"), this);
    m_resolution = new QComboBox(this);
    m_resolution->setObjectName(QStringLiteral("WarperResolution"));
    for (const QString& n : {tr("Low"), tr("Medium"), tr("High")})
        m_resolution->addItem(n);
    const QString resolutionTip
        = tr("How many pins the grids have: more pins give finer control. Changing it keeps "
             "the colours where they are.");
    resolutionLabel->setToolTip(resolutionTip);
    m_resolution->setToolTip(resolutionTip);
    resolutionRow->addWidget(resolutionLabel);
    resolutionRow->addWidget(m_resolution, 1);
    side->addLayout(resolutionRow);
    m_resetPin = new QToolButton(this);
    m_resetPin->setObjectName(QStringLiteral("WarperResetPin"));
    m_resetPin->setText(tr("Reset Pin"));
    m_resetPin->setToolTip(tr("Put the selected pin back (also Delete or a double-click)"));
    m_resetGrid = new QToolButton(this);
    m_resetGrid->setObjectName(QStringLiteral("WarperResetGrid"));
    m_resetGrid->setText(tr("Reset Grid"));
    m_resetGrid->setToolTip(tr("Put every pin of the shown grid back"));
    for (QToolButton* b : {m_resetPin, m_resetGrid}) {
        b->setAutoRaise(true);
        side->addWidget(b);
    }
    side->addStretch(1);
    row->addLayout(side);

    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        m_gridWidget->setGrid(id == 0 ? WarpGrid::HueSat : WarpGrid::ChromaLuma);
    });
    connect(m_gridWidget, &WarperGrid::selectionChanged, this,
        [this] { m_resetPin->setEnabled(m_gridWidget->selectedPin().x() >= 0); });
    connect(m_gridWidget, &WarperGrid::dragStarted, this, [this] {
        if (m_session)
            m_session->beginGesture(tr("Color Warper"));
    });
    connect(m_gridWidget, &WarperGrid::dragFinished, this, [this] {
        if (m_session)
            m_session->endGesture();
    });
    connect(m_gridWidget, &WarperGrid::warpChanged, this,
        [this](const GradeWarp& warp, const QString& label) { commit(warp, label); });
    connect(m_resolution, &QComboBox::activated, this, [this](int i) {
        if (m_refreshing || !m_session)
            return;
        commit(grademath::resampleWarp(m_gridWidget->warp(), i), tr("Warper Resolution"));
    });
    connect(m_resetPin, &QToolButton::clicked, this, [this] { m_gridWidget->resetSelectedPin(); });
    connect(m_resetGrid, &QToolButton::clicked, this, [this] {
        GradeWarp w = m_gridWidget->warp();
        w.pins(m_gridWidget->grid()).clear();
        commit(w, tr("Reset Grid"));
    });
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &WarperPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &WarperPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &WarperPanel::refresh);
    }
    m_resetPin->setEnabled(false);
    refresh();
}

void WarperPanel::commit(const GradeWarp& warp, const QString& label)
{
    if (m_refreshing || !m_session)
        return;
    m_session->setWarp(0, warp, label);
}

void WarperPanel::resetAll()
{
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    if (!n || n->warp.isIdentity())
        return;
    GradeWarp w = n->warp;
    w.hueSat.clear();
    w.chromaLuma.clear();
    m_session->setWarp(0, w, tr("Reset Color Warper"));
}

void WarperPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    setEnabled(m_session && m_session->hasGrade() && (!n || !n->isMixer()));
    m_gridWidget->setWarp(n ? n->warp : GradeWarp {});
    m_resolution->setCurrentIndex(m_gridWidget->warp().resolution);
    m_refreshing = false;
}

} // namespace kk
