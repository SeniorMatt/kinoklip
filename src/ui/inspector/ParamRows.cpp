// SPDX-License-Identifier: GPL-3.0-or-later
// ParamRows implementation: the concrete row types and the factory.
#include "ui/inspector/ParamRows.h"

#include "core/ParamKeys.h"
#include "ui/inspector/InspectorGlyphs.h"
#include "ui/inspector/KeyframeButton.h"
#include "ui/inspector/ParamEditHost.h"
#include "ui/inspector/ValueField.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFontComboBox>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSlider>
#include <QTimer>
#include <QToolButton>

#include <cmath>

namespace kk {

namespace {

constexpr int kSliderSteps = 1000; ///< slider resolution across the parameter range
/// Ranges wider than this get no slider (position, anchor, crop: a slider would be useless).
constexpr double kMaxSliderRange = 1000.0;
constexpr int kTextEditIdleMs = 800; ///< typing pause that closes a title-text undo step
constexpr int kTextEditLines = 3;
constexpr int kSwatchWidth = 44;
constexpr int kSwatchHeight = 16;
constexpr int kToolButtonSize = 18;

/// Decimals to show for a step size (1 → 0, 0.1 → 1, 0.01 → 2).
int decimalsForStep(const ParamDescriptor& desc)
{
    if (desc.type == ParamType::Int || desc.step >= 1.0)
        return 0;
    return std::clamp(int(std::ceil(-std::log10(desc.step) - 1e-9)), 0, 4);
}

QToolButton* makeToolButton(QWidget* parent, InspectorGlyph glyph, const QString& tip)
{
    auto* button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setIcon(inspectorGlyph(glyph));
    button->setIconSize(QSize(14, 14));
    button->setFixedSize(kToolButtonSize, kToolButtonSize);
    button->setToolTip(tip);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

/// Lets a combo box shrink with the inspector instead of forcing a horizontal overflow
/// (QFontComboBox in particular asks for the width of its longest font name).
void makeCompact(QComboBox* combo)
{
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(6);
}

QIcon swatchIcon(const QColor& color)
{
    QPixmap pixmap(kSwatchWidth, kSwatchHeight);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    QColor border = QGuiApplication::palette().color(QPalette::WindowText);
    border.setAlpha(80); // a soft outline in the text colour: visible on light and dark schemes
    p.setPen(QPen(border, 1));
    p.setBrush(color);
    p.drawRoundedRect(QRectF(0.5, 0.5, kSwatchWidth - 1, kSwatchHeight - 1), 2, 2);
    return QIcon(pixmap);
}

// ---- Number (Double / Int): value field, optional slider, keyframe controls ----

class NumberRow final : public ParamRow
{
public:
    NumberRow(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
        QGridLayout* grid, int gridRow, QWidget* parentWidget)
        : ParamRow(ref, desc, host, parentWidget)
    {
        addCommonWidgets(grid, gridRow, parentWidget);
        m_field = new ValueField(parentWidget);
        m_field->setObjectName(QStringLiteral("value:") + desc.key); // stable handle for tests
        m_field->setRange(desc.min, desc.max);
        m_field->setStep(desc.step);
        m_field->setDecimals(decimalsForStep(desc));
        m_field->setSuffix(desc.unit);
        m_field->setToolTip(tr("Drag to change (Shift: fine, Ctrl: coarse), double-click to type"));
        grid->addWidget(m_field, gridRow, paramgrid::kEditor);

        connect(m_field, &ValueField::scrubStarted, this, [this] {
            m_host.setActiveParam(m_ref, m_desc);
            m_host.beginParamEdit(changeLabel());
            m_inGesture = true;
        });
        connect(m_field, &ValueField::valueScrubbed, this, [this](double v) { apply(v); });
        connect(m_field, &ValueField::scrubFinished, this, [this] { finishGesture(); });
        connect(m_field, &ValueField::valueCommitted, this, [this](double v) { applyOnce(v); });

        if (desc.max - desc.min <= kMaxSliderRange) {
            m_slider = new QSlider(Qt::Horizontal, parentWidget);
            m_slider->setRange(0, kSliderSteps);
            m_slider->setFocusPolicy(Qt::NoFocus);
            grid->addWidget(m_slider, gridRow, paramgrid::kSlider);
            connect(m_slider, &QSlider::sliderPressed, this, [this] {
                m_host.setActiveParam(m_ref, m_desc);
                m_host.beginParamEdit(changeLabel());
                m_inGesture = true;
            });
            connect(m_slider, &QSlider::valueChanged, this, [this](int position) {
                const double v = fromSlider(position);
                if (m_inGesture)
                    apply(v);
                else
                    applyOnce(v); // page step / keyboard: one undo step each
            });
            connect(m_slider, &QSlider::sliderReleased, this, [this] { finishGesture(); });
        }

        // Transition parameters are not keyframed (the transition has its own time base).
        const bool keyable = desc.animatable && ref.scope != ParamScope::TransitionIn
            && ref.scope != ParamScope::TransitionOut;
        if (keyable) {
            auto* box = new QWidget(parentWidget);
            auto* layout = new QHBoxLayout(box);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(0);
            m_prev = new KeyframeArrowButton(false, box);
            m_diamond = new KeyframeButton(box);
            m_diamond->setObjectName(QStringLiteral("keyframe:") + desc.key);
            m_next = new KeyframeArrowButton(true, box);
            layout->addWidget(m_prev);
            layout->addWidget(m_diamond);
            layout->addWidget(m_next);
            grid->addWidget(box, gridRow, paramgrid::kKeyframe);
            connect(m_diamond, &QAbstractButton::clicked, this, [this] { toggleKey(); });
            connect(m_prev, &QAbstractButton::clicked, this, [this] { jump(false); });
            connect(m_next, &QAbstractButton::clicked, this, [this] { jump(true); });
        }
    }

    ~NumberRow() override { finishGesture(); }

    void refresh(const ClipData& clip, int64_t localFrame, bool playheadInside) override
    {
        const double v = paramaccess::scalar(clip, m_ref, m_desc, localFrame);
        if (!m_field->isScrubbing())
            m_field->setValue(v);
        if (m_slider && !m_slider->isSliderDown()) {
            const int position = toSlider(v);
            if (position != m_slider->value()) {
                const QSignalBlocker block(m_slider);
                m_slider->setValue(position);
            }
        }
        if (m_diamond) {
            const AnimatedValue* anim = paramaccess::animated(clip, m_ref);
            const bool animated = anim && anim->isAnimated();
            using S = KeyframeButton::State;
            m_diamond->setState(
                !animated ? S::NotAnimated : (anim->hasKeyAt(localFrame) ? S::OnKey : S::Animated));
            m_diamond->setEnabled(playheadInside);
            m_prev->setEnabled(animated && anim->previousKeyFrame(localFrame).has_value());
            m_next->setEnabled(animated && anim->nextKeyFrame(localFrame).has_value());
        }
        refreshReset(clip, localFrame);
    }

private:
    [[nodiscard]] double fromSlider(int position) const
    {
        const double t = double(position) / kSliderSteps;
        return m_desc.min + t * (m_desc.max - m_desc.min);
    }

    [[nodiscard]] int toSlider(double value) const
    {
        const double range = m_desc.max - m_desc.min;
        return range > 0 ? int(std::lround((value - m_desc.min) / range * kSliderSteps)) : 0;
    }

    void apply(double value)
    {
        const quint64 clipId = m_ref.clipId;
        m_host.modifyClip(clipId, [this, clipId, value](ClipData& clip) {
            paramaccess::setScalar(clip, m_ref, m_desc, m_host.localFrame(clipId), value);
        });
    }

    void applyOnce(double value)
    {
        m_host.setActiveParam(m_ref, m_desc);
        const quint64 clipId = m_ref.clipId;
        m_host.editClip(changeLabel(), clipId, [this, clipId, value](ClipData& clip) {
            paramaccess::setScalar(clip, m_ref, m_desc, m_host.localFrame(clipId), value);
        });
    }

    void finishGesture()
    {
        if (!m_inGesture)
            return;
        m_inGesture = false;
        m_host.endParamEdit();
    }

    void toggleKey()
    {
        const ClipData* clip = m_host.clip(m_ref.clipId);
        if (!clip)
            return;
        m_host.setActiveParam(m_ref, m_desc);
        const int64_t frame = m_host.localFrame(m_ref.clipId);
        const AnimatedValue* anim = paramaccess::animated(*clip, m_ref);
        const QString label
            = anim && anim->hasKeyAt(frame) ? tr("Remove Keyframe") : tr("Add Keyframe");
        m_host.editClip(label, m_ref.clipId,
            [this, frame](ClipData& c) { paramaccess::toggleKey(c, m_ref, m_desc, frame); });
    }

    void jump(bool forward)
    {
        const ClipData* clip = m_host.clip(m_ref.clipId);
        const AnimatedValue* anim = clip ? paramaccess::animated(*clip, m_ref) : nullptr;
        if (!anim)
            return;
        const int64_t frame = m_host.localFrame(m_ref.clipId);
        const auto target = forward ? anim->nextKeyFrame(frame) : anim->previousKeyFrame(frame);
        if (target)
            m_host.seekLocalFrame(m_ref.clipId, *target);
    }

    ValueField* m_field = nullptr;
    QSlider* m_slider = nullptr;
    KeyframeButton* m_diamond = nullptr;
    KeyframeArrowButton* m_prev = nullptr;
    KeyframeArrowButton* m_next = nullptr;
    bool m_inGesture = false; ///< an undo group is open for a drag (field or slider)
};

// ---- Bool: checkbox ----

class BoolRow final : public ParamRow
{
public:
    BoolRow(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
        QGridLayout* grid, int gridRow, QWidget* parentWidget)
        : ParamRow(ref, desc, host, parentWidget)
    {
        addCommonWidgets(grid, gridRow, parentWidget);
        m_box = new QCheckBox(parentWidget);
        grid->addWidget(m_box, gridRow, paramgrid::kEditor, 1, 2);
        // clicked (not toggled): only user input creates undo steps, never refresh().
        connect(m_box, &QCheckBox::clicked, this, [this](bool on) {
            m_host.editClip(changeLabel(), m_ref.clipId,
                [this, on](ClipData& clip) { paramaccess::setStatic(clip, m_ref, on); });
        });
    }

    void refresh(const ClipData& clip, int64_t localFrame, bool) override
    {
        const bool on = paramaccess::staticValue(clip, m_ref, m_desc).toBool();
        if (m_box->isChecked() != on)
            m_box->setChecked(on);
        refreshReset(clip, localFrame);
    }

private:
    QCheckBox* m_box = nullptr;
};

// ---- Choice: combo box ----

class ChoiceRow final : public ParamRow
{
public:
    ChoiceRow(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
        QGridLayout* grid, int gridRow, QWidget* parentWidget)
        : ParamRow(ref, desc, host, parentWidget)
    {
        addCommonWidgets(grid, gridRow, parentWidget);
        m_combo = new QComboBox(parentWidget);
        makeCompact(m_combo);
        for (int i = 0; i < desc.choices.size(); ++i) {
            const QString label
                = i < desc.choiceLabels.size() ? desc.choiceLabels[i] : desc.choices[i];
            m_combo->addItem(label, desc.choices[i]);
        }
        grid->addWidget(m_combo, gridRow, paramgrid::kEditor, 1, 2);
        connect(m_combo, &QComboBox::activated, this, [this](int index) {
            const QString value = m_combo->itemData(index).toString();
            m_host.editClip(changeLabel(), m_ref.clipId,
                [this, value](ClipData& clip) { paramaccess::setStatic(clip, m_ref, value); });
        });
    }

    void refresh(const ClipData& clip, int64_t localFrame, bool) override
    {
        const int index
            = m_desc.choices.indexOf(paramaccess::staticValue(clip, m_ref, m_desc).toString());
        if (index != m_combo->currentIndex())
            m_combo->setCurrentIndex(index);
        refreshReset(clip, localFrame);
    }

private:
    QComboBox* m_combo = nullptr;
};

// ---- Color: swatch button opening a colour dialog ----

class ColorRow final : public ParamRow
{
public:
    ColorRow(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
        QGridLayout* grid, int gridRow, QWidget* parentWidget)
        : ParamRow(ref, desc, host, parentWidget)
    {
        addCommonWidgets(grid, gridRow, parentWidget);
        m_swatch = new QToolButton(parentWidget);
        m_swatch->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_swatch->setIconSize(QSize(kSwatchWidth, kSwatchHeight));
        m_swatch->setAutoRaise(true);
        m_swatch->setToolTip(tr("Click to choose a color"));
        grid->addWidget(m_swatch, gridRow, paramgrid::kEditor, 1, 2, Qt::AlignLeft);
        connect(m_swatch, &QToolButton::clicked, this, [this] {
            const QColor picked = m_host.pickColor(m_color, m_desc.label);
            if (!picked.isValid() || picked == m_color)
                return;
            m_host.editClip(changeLabel(), m_ref.clipId,
                [this, picked](ClipData& clip) { paramaccess::setStatic(clip, m_ref, picked); });
        });
    }

    void refresh(const ClipData& clip, int64_t localFrame, bool) override
    {
        const QColor color = paramaccess::staticValue(clip, m_ref, m_desc).value<QColor>();
        if (color != m_color || m_swatch->icon().isNull()) {
            m_color = color;
            m_swatch->setIcon(swatchIcon(color));
            m_swatch->setText(color.name(QColor::HexRgb).toUpper());
        }
        refreshReset(clip, localFrame);
    }

private:
    QToolButton* m_swatch = nullptr;
    QColor m_color;
};

// ---- String: single-line edit ----

class LineRow final : public ParamRow
{
public:
    LineRow(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
        QGridLayout* grid, int gridRow, QWidget* parentWidget)
        : ParamRow(ref, desc, host, parentWidget)
    {
        addCommonWidgets(grid, gridRow, parentWidget);
        m_edit = new QLineEdit(parentWidget);
        grid->addWidget(m_edit, gridRow, paramgrid::kEditor, 1, 2);
        connect(m_edit, &QLineEdit::editingFinished, this, [this] {
            const QString text = m_edit->text();
            const ClipData* clip = m_host.clip(m_ref.clipId);
            if (!clip || paramaccess::staticValue(*clip, m_ref, m_desc).toString() == text)
                return;
            m_host.editClip(changeLabel(), m_ref.clipId,
                [this, text](ClipData& c) { paramaccess::setStatic(c, m_ref, text); });
        });
    }

    void refresh(const ClipData& clip, int64_t localFrame, bool) override
    {
        const QString text = paramaccess::staticValue(clip, m_ref, m_desc).toString();
        if (!m_edit->hasFocus() && m_edit->text() != text)
            m_edit->setText(text);
        refreshReset(clip, localFrame);
    }

private:
    QLineEdit* m_edit = nullptr;
};

// ---- Font family: font combo box (String parameter with key keys::Font) ----

class FontRow final : public ParamRow
{
public:
    FontRow(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
        QGridLayout* grid, int gridRow, QWidget* parentWidget)
        : ParamRow(ref, desc, host, parentWidget)
    {
        addCommonWidgets(grid, gridRow, parentWidget);
        m_combo = new QFontComboBox(parentWidget);
        m_combo->setEditable(false);
        makeCompact(m_combo);
        grid->addWidget(m_combo, gridRow, paramgrid::kEditor, 1, 2);
        connect(m_combo, &QComboBox::activated, this, [this] {
            const QString family = m_combo->currentFont().family();
            m_host.editClip(changeLabel(), m_ref.clipId,
                [this, family](ClipData& clip) { paramaccess::setStatic(clip, m_ref, family); });
        });
    }

    void refresh(const ClipData& clip, int64_t localFrame, bool) override
    {
        const QString family = paramaccess::staticValue(clip, m_ref, m_desc).toString();
        if (family != m_family) {
            m_family = family;
            const QSignalBlocker block(m_combo);
            m_combo->setCurrentFont(QFont(family));
        }
        refreshReset(clip, localFrame);
    }

private:
    QFontComboBox* m_combo = nullptr;
    QString m_family;
};

// ---- Title text: multi-line edit, one undo step per typing burst ----

class TextRow final : public ParamRow
{
public:
    TextRow(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
        QGridLayout* grid, int gridRow, QWidget* parentWidget)
        : ParamRow(ref, desc, host, parentWidget)
    {
        addCommonWidgets(grid, gridRow, parentWidget);
        m_edit = new QPlainTextEdit(parentWidget);
        m_edit->setTabChangesFocus(true);
        const int lineHeight = m_edit->fontMetrics().lineSpacing();
        m_edit->setFixedHeight(lineHeight * kTextEditLines + 12);
        m_edit->installEventFilter(this);
        grid->addWidget(m_edit, gridRow, paramgrid::kEditor, 1, 3);
        m_idle.setSingleShot(true);
        m_idle.setInterval(kTextEditIdleMs);
        connect(&m_idle, &QTimer::timeout, this, [this] { finishTyping(); });
        connect(m_edit, &QPlainTextEdit::textChanged, this, [this] {
            if (m_refreshing)
                return;
            if (!m_typing) {
                m_host.beginParamEdit(changeLabel());
                m_typing = true;
            }
            const QString text = m_edit->toPlainText();
            m_host.modifyClip(m_ref.clipId,
                [this, text](ClipData& clip) { paramaccess::setStatic(clip, m_ref, text); });
            m_idle.start();
        });
    }

    ~TextRow() override { finishTyping(); }

    void refresh(const ClipData& clip, int64_t localFrame, bool) override
    {
        if (!m_typing) {
            const QString text = paramaccess::staticValue(clip, m_ref, m_desc).toString();
            if (m_edit->toPlainText() != text) {
                m_refreshing = true;
                m_edit->setPlainText(text);
                m_refreshing = false;
            }
        }
        refreshReset(clip, localFrame);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_edit && event->type() == QEvent::FocusOut)
            finishTyping();
        return ParamRow::eventFilter(watched, event);
    }

private:
    void finishTyping()
    {
        m_idle.stop();
        if (!m_typing)
            return;
        m_typing = false;
        m_host.endParamEdit();
    }

    QPlainTextEdit* m_edit = nullptr;
    QTimer m_idle;
    bool m_typing = false; ///< an undo group is open for the current typing burst
    bool m_refreshing = false;
};

} // namespace

// ---- ParamRow base ----

ParamRow::ParamRow(
    const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host, QObject* parent)
    : QObject(parent)
    , m_ref(ref)
    , m_desc(desc)
    , m_host(host)
{ }

void ParamRow::addCommonWidgets(QGridLayout* grid, int gridRow, QWidget* parentWidget)
{
    m_label = new QLabel(m_desc.label, parentWidget);
    m_label->setObjectName(QStringLiteral("InspectorParamLabel"));
    m_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(m_label, gridRow, paramgrid::kLabel,
        m_desc.type == ParamType::String && m_desc.key == keys::Text
            ? Qt::AlignRight | Qt::AlignTop
            : Qt::AlignRight | Qt::AlignVCenter);

    m_reset = makeToolButton(parentWidget, InspectorGlyph::Reset, tr("Reset to default"));
    grid->addWidget(m_reset, gridRow, paramgrid::kReset);
    connect(m_reset, &QToolButton::clicked, this, [this] {
        m_host.setActiveParam(m_ref, m_desc);
        m_host.editClip(tr("Reset %1").arg(m_desc.label), m_ref.clipId,
            [this](ClipData& clip) { paramaccess::reset(clip, m_ref, m_desc); });
    });
}

void ParamRow::refreshReset(const ClipData& clip, int64_t localFrame)
{
    const bool modified = paramaccess::isModified(clip, m_ref, m_desc, localFrame);
    if (modified != m_resetEnabled) {
        m_resetEnabled = modified;
        m_reset->setEnabled(modified);
    }
}

QString ParamRow::changeLabel() const
{
    return tr("Change %1").arg(m_desc.label);
}

ParamRow* ParamRow::create(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
    QGridLayout* grid, int gridRow, QWidget* parentWidget)
{
    switch (desc.type) {
    case ParamType::Double:
    case ParamType::Int:
        return new NumberRow(ref, desc, host, grid, gridRow, parentWidget);
    case ParamType::Bool:
        return new BoolRow(ref, desc, host, grid, gridRow, parentWidget);
    case ParamType::Color:
        return new ColorRow(ref, desc, host, grid, gridRow, parentWidget);
    case ParamType::Choice:
        return new ChoiceRow(ref, desc, host, grid, gridRow, parentWidget);
    case ParamType::String:
        // Well-known string keys get specialised editors; everything else a line edit.
        if (desc.key == keys::Text)
            return new TextRow(ref, desc, host, grid, gridRow, parentWidget);
        if (desc.key == keys::Font)
            return new FontRow(ref, desc, host, grid, gridRow, parentWidget);
        return new LineRow(ref, desc, host, grid, gridRow, parentWidget);
    }
    return nullptr;
}

} // namespace kk
