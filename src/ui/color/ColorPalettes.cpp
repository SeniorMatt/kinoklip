// SPDX-License-Identifier: GPL-3.0-or-later
// ColorPalettes — Qualifier, Windows, LUTs and Key palettes.
#include "ui/color/ColorPalettes.h"

#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorSession.h"
#include "ui/common/TextFit.h"
#include "ui/common/VimRowOps.h"
#include "ui/inspector/ValueField.h"

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
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
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

    // ---- The three channels ----
    auto* grid = new QGridLayout;
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
        m_use[c] = new QCheckBox(names[c], this);
        m_use[c]->setObjectName(QStringLiteral("QualifierUse_") + names[c]);
        grid->addWidget(m_use[c], c, 0);
        m_bars[c] = new QualifierRangeBar(QualifierRangeBar::Channel(c), this);
        grid->addWidget(m_bars[c], c, 1);
        for (int k = 0; k < 3; ++k) {
            auto* label = new QLabel(fieldLabels[c][k], this);
            ValueField* f = makeField(this, 0.0, 1.0, 0.002, 3);
            f->setObjectName(QStringLiteral("Qualifier_") + fieldNames[c][k]);
            grid->addWidget(label, c, 2 + k * 2, Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(f, c, 3 + k * 2);
            m_fields.append({fieldNames[c][k], f});
        }
    }
    grid->setColumnStretch(1, 1);
    grid->setHorizontalSpacing(8);
    layout->addLayout(grid);
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
            {"lumLow", q.lumLow}, {"lumHigh", q.lumHigh}, {"lumSoft", q.lumSoft}};
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
             GradeWindowShape::Polygon, GradeWindowShape::Gradient}) {
        QToolButton* b = toolButton(shapeName(s), this);
        b->setObjectName(QStringLiteral("WindowAdd_") + gradeWindowShapeToString(s));
        b->setToolTip(tr("Add a %1 window to the current node").arg(shapeName(s).toLower()));
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
    listTools->addWidget(m_invert);
    listTools->addWidget(m_delete);
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
        int counts[4] = {0, 0, 0, 0};
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
            if (w.id == selected)
                m_list->setCurrentItem(item);
        }
    }
    const GradeWindow* w = m_session ? m_session->window() : nullptr;
    m_fieldsBox->setEnabled(w != nullptr);
    m_invert->setEnabled(w != nullptr);
    m_delete->setEnabled(w != nullptr);
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
    tools->addWidget(addFolderButton);
    tools->addWidget(loadFile);
    tools->addWidget(refreshButton);
    tools->addStretch(1);
    tools->addWidget(new QLabel(tr("Interpolation"), this));
    tools->addWidget(m_interpolation);
    tools->addWidget(m_remove);
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

QStringList LutBrowser::folders() const
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

} // namespace kk
