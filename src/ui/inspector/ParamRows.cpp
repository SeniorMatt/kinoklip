// SPDX-License-Identifier: GPL-3.0-or-later
// ParamRows implementation: the concrete row types and the factory.
#include "ui/inspector/ParamRows.h"

#include "core/CurveParam.h"
#include "core/ParamKeys.h"
#include "core/VideoEffectKeys.h"
#include "ui/common/SchemeColors.h"
#include "ui/inspector/CurveEditor.h"
#include "ui/inspector/InspectorGlyphs.h"
#include "ui/inspector/KeyframeButton.h"
#include "ui/inspector/ParamEditHost.h"
#include "ui/inspector/ValueField.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QFontComboBox>
#include <QGuiApplication>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSlider>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
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

/// The viewer eyedropper button of ParamDescriptor::viewerPicker parameters: while checked,
/// a click (or a drag over an area) in the timeline viewer picks the colour
/// (ColorPickOverlay); it unchecks itself after the pick.
QToolButton* makePickerButton(QWidget* parent, const QString& tip)
{
    auto* picker = new QToolButton(parent);
    picker->setObjectName(QStringLiteral("ColorPickerButton"));
    picker->setCheckable(true);
    picker->setAutoRaise(true);
    // The icon theme's eyedropper (Breeze has one); a text button otherwise.
    const QIcon dropper = QIcon::fromTheme(QStringLiteral("color-picker"));
    if (!dropper.isNull())
        picker->setIcon(dropper);
    else
        picker->setText(QCoreApplication::translate("kk::ParamRow", "Pick"));
    picker->setToolTip(tip);
    return picker;
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
        m_main = m_field;

        connect(m_field, &ValueField::scrubStarted, this, [this] {
            m_host.setActiveParam(m_ref, m_desc);
            m_host.beginParamEdit(changeLabel());
            m_inGesture = true;
        });
        connect(m_field, &ValueField::valueScrubbed, this, [this](double v) { apply(v); });
        connect(m_field, &ValueField::scrubFinished, this, [this] { finishGesture(); });
        connect(
            m_field, &ValueField::valueCommitted, this, [this](double v) { applyScalarOnce(v); });

        if (desc.max - desc.min <= kMaxSliderRange) {
            m_slider = new QSlider(Qt::Horizontal, parentWidget);
            m_slider->setRange(0, kSliderSteps);
            m_slider->setFocusPolicy(Qt::NoFocus);
            grid->addWidget(m_slider, gridRow, paramgrid::kSlider);
            m_extra.append(m_slider);
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
                    applyScalarOnce(v); // page step / keyboard: one undo step each
            });
            connect(m_slider, &QSlider::sliderReleased, this, [this] { finishGesture(); });
        }

        // Transition parameters are not keyframed (the transition has its own time base).
        if (keyable()) {
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
            if (desc.viewerPicker && ref.scope == ParamScope::Effect) {
                // A hue parameter (HSL Key): the eyedropper picks the hue of a colour.
                m_picker = makePickerButton(box,
                    tr("Pick the hue in the timeline viewer: click a pixel or drag over an "
                       "area to average it"));
                layout->addWidget(m_picker);
                connect(m_picker, &QToolButton::toggled, this,
                    [this](bool on) { m_host.setColorPicking(m_ref, on); });
            }
            grid->addWidget(box, gridRow, paramgrid::kKeyframe);
            m_extra.append(box);
            connect(m_diamond, &QAbstractButton::clicked, this, [this] { toggleKey(); });
            // Right-click on a key: interpolation of the segment that starts there (spec 15.6).
            m_diamond->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(m_diamond, &QWidget::customContextMenuRequested, this,
                [this](const QPoint& pos) { showInterpMenu(m_diamond->mapToGlobal(pos)); });
            connect(m_prev, &QAbstractButton::clicked, this, [this] { jump(false); });
            connect(m_next, &QAbstractButton::clicked, this, [this] { jump(true); });
        }
    }

    ~NumberRow() override { finishGesture(); }

    void refresh(const ClipData& clip, int64_t localFrame, bool playheadInside) override
    {
        // Shown value (Position Y is shown +up, spec 24.16); apply() takes shown values too.
        const double v = m_desc.displayed(paramaccess::scalar(clip, m_ref, m_desc, localFrame));
        if (!m_field->isScrubbing()) {
            m_field->setValue(v);
            m_field->setMixed(m_host.isMixed(m_ref, m_desc));
        }
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
        if (m_picker) {
            const bool picking = m_host.isColorPicking(m_ref);
            if (m_picker->isChecked() != picking) {
                const QSignalBlocker block(m_picker);
                m_picker->setChecked(picking);
            }
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

    void apply(double shown)
    {
        const double value = m_desc.displayed(shown);
        const quint64 clipId = m_ref.clipId;
        m_host.modifyClip(clipId, [this, clipId, value](ClipData& clip) {
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

    void showInterpMenu(const QPoint& globalPos)
    {
        const ClipData* clip = m_host.clip(m_ref.clipId);
        if (!clip)
            return;
        const int64_t frame = m_host.localFrame(m_ref.clipId);
        const std::optional<Interp> current = paramaccess::keyInterp(*clip, m_ref, frame);
        if (!current)
            return; // only a key has an interpolation
        QMenu menu;
        const std::pair<Interp, QString> modes[] = {
            {Interp::Linear, tr("Linear")},
            {Interp::EaseIn, tr("Ease In")},
            {Interp::EaseOut, tr("Ease Out")},
            {Interp::EaseInOut, tr("Ease In and Out")},
            {Interp::Smooth, tr("Smooth (Bezier)")},
            {Interp::Hold, tr("Static (Hold)")},
        };
        for (const auto& [mode, text] : modes) {
            QAction* a = menu.addAction(text);
            a->setCheckable(true);
            a->setChecked(mode == *current);
            const Interp m = mode;
            connect(a, &QAction::triggered, this, [this, frame, m] {
                m_host.editClip(tr("Change Keyframe Interpolation"), m_ref.clipId,
                    [this, frame, m](
                        ClipData& c) { paramaccess::setKeyInterp(c, m_ref, frame, m); });
            });
        }
        menu.exec(globalPos);
    }

    ValueField* m_field = nullptr;
    QSlider* m_slider = nullptr;
    KeyframeButton* m_diamond = nullptr;
    KeyframeArrowButton* m_prev = nullptr;
    KeyframeArrowButton* m_next = nullptr;
    QToolButton* m_picker = nullptr; ///< hue eyedropper (ParamDescriptor::viewerPicker)
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
        m_main = m_box;
        // clicked (not toggled): only user input creates undo steps, never refresh().
        connect(m_box, &QCheckBox::clicked, this, [this](bool on) {
            m_host.editClip(changeLabel(), m_ref.clipId,
                [this, on](ClipData& clip) { paramaccess::setStatic(clip, m_ref, on); });
        });
    }

    void refresh(const ClipData& clip, int64_t localFrame, bool) override
    {
        const bool on = paramaccess::staticValue(clip, m_ref, m_desc).toBool();
        // Mixed across the selected clips (spec 24.1): the partial check mark; a click
        // then sets every clip to the box's new state.
        const Qt::CheckState state = m_host.isMixed(m_ref, m_desc)
            ? Qt::PartiallyChecked
            : (on ? Qt::Checked : Qt::Unchecked);
        if (m_box->checkState() != state) {
            // Tristate only while mixed: a click from the partial mark goes to checked.
            m_box->setTristate(state == Qt::PartiallyChecked);
            m_box->setCheckState(state);
        }
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
        m_combo->setObjectName(QStringLiteral("choice:") + desc.key); // tests, guide screenshots
        makeCompact(m_combo);
        for (int i = 0; i < desc.choices.size(); ++i) {
            const QString label
                = i < desc.choiceLabels.size() ? desc.choiceLabels[i] : desc.choices[i];
            m_combo->addItem(label, desc.choices[i]);
        }
        grid->addWidget(m_combo, gridRow, paramgrid::kEditor, 1, 2);
        m_main = m_combo;
        connect(m_combo, &QComboBox::activated, this, [this](int index) {
            const QString value = m_combo->itemData(index).toString();
            m_host.editClip(changeLabel(), m_ref.clipId,
                [this, value](ClipData& clip) { paramaccess::setStatic(clip, m_ref, value); });
        });
    }

    void refresh(const ClipData& clip, int64_t localFrame, bool) override
    {
        // Mixed across the selected clips (spec 24.1): no current entry.
        const int index = m_host.isMixed(m_ref, m_desc)
            ? -1
            : m_desc.choices.indexOf(paramaccess::staticValue(clip, m_ref, m_desc).toString());
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
        m_main = m_swatch;
        if (desc.viewerPicker && ref.scope == ParamScope::Effect) {
            // Eyedropper: while checked, a click (or a drag over an area) in the timeline
            // viewer picks the colour (ColorPickOverlay); it unchecks itself after the pick.
            m_picker = makePickerButton(parentWidget,
                tr("Pick the color in the timeline viewer: click a pixel or drag over an area "
                   "to average it"));
            grid->addWidget(m_picker, gridRow, paramgrid::kKeyframe, Qt::AlignLeft);
            m_extra.append(m_picker);
            connect(m_picker, &QToolButton::toggled, this,
                [this](bool on) { m_host.setColorPicking(m_ref, on); });
        }
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
        if (m_picker) {
            const bool picking = m_host.isColorPicking(m_ref);
            if (m_picker->isChecked() != picking) {
                const QSignalBlocker block(m_picker);
                m_picker->setChecked(picking);
            }
        }
        refreshReset(clip, localFrame);
    }

private:
    QToolButton* m_swatch = nullptr;
    QToolButton* m_picker = nullptr; ///< viewer eyedropper (ParamDescriptor::viewerPicker)
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
        m_edit->setObjectName(QStringLiteral("text:") + desc.key); // tests, guide screenshots
        grid->addWidget(m_edit, gridRow, paramgrid::kEditor, 1, 2);
        m_main = m_edit;
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
        m_main = m_combo;
        // A font installed while the program runs (render/FontReload.h): the combo's list
        // follows by itself, the chosen family is selected again (it may be the new one).
        connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this, [this] {
            if (m_family.isEmpty())
                return;
            const QSignalBlocker block(m_combo);
            m_combo->setCurrentFont(QFont(m_family));
        });
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
        m_edit->setObjectName(QStringLiteral("text:") + desc.key); // tests, guide screenshots
        m_edit->setTabChangesFocus(true);
        const int lineHeight = m_edit->fontMetrics().lineSpacing();
        m_edit->setFixedHeight(lineHeight * kTextEditLines + 12);
        m_edit->installEventFilter(this);
        grid->addWidget(m_edit, gridRow, paramgrid::kEditor, 1, 3);
        m_main = m_edit;
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

    void finishPendingEdit() override { finishTyping(); }

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

// ---- Curve: the tone-curve graph (Curves effect) ----

class CurveRow final : public ParamRow
{
public:
    CurveRow(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
        QGridLayout* grid, int gridRow, QWidget* parentWidget)
        : ParamRow(ref, desc, host, parentWidget)
    {
        addCommonWidgets(grid, gridRow, parentWidget);
        m_label->setAlignment(Qt::AlignRight | Qt::AlignTop);
        m_editor = new CurveEditor(parentWidget);
        m_editor->setObjectName(QStringLiteral("curve:") + desc.key);
        // The channel curves in their channel's colour (the scheme's red / green / blue).
        const SchemeColors& sc = schemeColors();
        if (desc.key == fxkeys::RedCurve)
            m_editor->setCurveColor(sc.negative);
        else if (desc.key == fxkeys::GreenCurve)
            m_editor->setCurveColor(sc.positive);
        else if (desc.key == fxkeys::BlueCurve)
            m_editor->setCurveColor(sc.link);
        grid->addWidget(m_editor, gridRow, paramgrid::kEditor, 1, 3);
        m_main = m_editor;
        connect(m_editor, &CurveEditor::editStarted, this, [this] {
            m_host.beginParamEdit(changeLabel());
            m_inGesture = true;
        });
        connect(m_editor, &CurveEditor::pointsEdited, this, [this](const curve::Points& points) {
            const QString text = curve::format(points);
            m_host.modifyClip(m_ref.clipId,
                [this, text](ClipData& clip) { paramaccess::setStatic(clip, m_ref, text); });
        });
        connect(m_editor, &CurveEditor::editFinished, this, [this] { finishGesture(); });
        connect(m_editor, &CurveEditor::pointsCommitted, this, [this](const curve::Points& points) {
            const QString text = curve::format(points);
            m_host.editClip(changeLabel(), m_ref.clipId,
                [this, text](ClipData& clip) { paramaccess::setStatic(clip, m_ref, text); });
        });
    }

    ~CurveRow() override { finishGesture(); }

    void refresh(const ClipData& clip, int64_t localFrame, bool) override
    {
        if (!m_inGesture) {
            const QString text = paramaccess::staticValue(clip, m_ref, m_desc).toString();
            if (text != m_text) {
                m_text = text;
                m_editor->setPoints(curve::parse(text));
            }
        }
        refreshReset(clip, localFrame);
    }

private:
    void finishGesture()
    {
        if (!m_inGesture)
            return;
        m_inGesture = false;
        m_host.endParamEdit();
    }

    CurveEditor* m_editor = nullptr;
    QString m_text;
    bool m_inGesture = false;
};

// ---- File: file name + Browse button ----

class FileRow final : public ParamRow
{
public:
    FileRow(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
        QGridLayout* grid, int gridRow, QWidget* parentWidget)
        : ParamRow(ref, desc, host, parentWidget)
    {
        addCommonWidgets(grid, gridRow, parentWidget);
        auto* box = new QWidget(parentWidget);
        auto* layout = new QHBoxLayout(box);
        layout->setContentsMargins(0, 0, 0, 0);
        m_name = new QLineEdit(box);
        m_name->setObjectName(QStringLiteral("file:") + desc.key);
        m_name->setReadOnly(true);
        m_name->setPlaceholderText(tr("No file"));
        m_browse = new QToolButton(box);
        m_browse->setObjectName(QStringLiteral("browse:") + desc.key);
        m_browse->setText(tr("Browse…"));
        m_browse->setToolTip(tr("Choose the file"));
        layout->addWidget(m_name, 1);
        layout->addWidget(m_browse);
        grid->addWidget(box, gridRow, paramgrid::kEditor, 1, 3);
        m_main = m_browse;
        m_extra.append(box);
        connect(m_browse, &QToolButton::clicked, this, [this] {
            const QString picked = m_host.pickFile(m_path, m_desc.label, m_desc.fileFilter);
            if (picked.isEmpty() || picked == m_path)
                return;
            const QString absolute = QFileInfo(picked).absoluteFilePath();
            m_host.editClip(changeLabel(), m_ref.clipId, [this, absolute](ClipData& clip) {
                paramaccess::setStatic(clip, m_ref, absolute);
            });
        });
    }

    void refresh(const ClipData& clip, int64_t localFrame, bool) override
    {
        const QString path = paramaccess::staticValue(clip, m_ref, m_desc).toString();
        if (path != m_path || m_name->text().isEmpty() != path.isEmpty()) {
            m_path = path;
            const QFileInfo info(path);
            m_name->setText(path.isEmpty() ? QString() : info.fileName());
            const bool missing = !path.isEmpty() && !info.exists();
            m_name->setToolTip(missing ? tr("Missing: %1").arg(path) : path);
            QFont font = m_name->font();
            font.setItalic(missing);
            m_name->setFont(font);
        }
        refreshReset(clip, localFrame);
    }

private:
    QLineEdit* m_name = nullptr;
    QToolButton* m_browse = nullptr;
    QString m_path;
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
    m_label->setProperty(VimRowOps::kRowProperty, QVariant::fromValue<QObject*>(this));
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

// ---- Keyboard access (VimRowOps) ----

bool ParamRow::keyable() const
{
    return m_desc.animatable && m_desc.isScalar() && m_ref.scope != ParamScope::TransitionIn
        && m_ref.scope != ParamScope::TransitionOut;
}

void ParamRow::applyScalarOnce(double shown)
{
    const double value = m_desc.displayed(shown);
    m_host.setActiveParam(m_ref, m_desc);
    const quint64 clipId = m_ref.clipId;
    m_host.editClip(changeLabel(), clipId, [this, clipId, value](ClipData& clip) {
        paramaccess::setScalar(clip, m_ref, m_desc, m_host.localFrame(clipId), value);
    });
}

void ParamRow::toggleKey()
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

void ParamRow::jump(bool forward)
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

QList<QWidget*> ParamRow::vimWidgets() const
{
    QList<QWidget*> widgets {m_label, m_reset};
    if (m_main)
        widgets.append(m_main);
    widgets.append(m_extra);
    return widgets;
}

bool ParamRow::vimStep(int steps, double scale)
{
    const ClipData* clip = m_host.clip(m_ref.clipId);
    if (!clip || steps == 0)
        return false;
    if (m_desc.isScalar()) {
        const int64_t frame = m_host.localFrame(m_ref.clipId);
        double shown = m_desc.displayed(paramaccess::scalar(*clip, m_ref, m_desc, frame));
        double step = m_desc.step * scale;
        if (m_desc.type == ParamType::Int)
            step = std::max(1.0, std::round(step));
        // Rounded to the decimals the field shows (one more for the fine step), so 0.1 added
        // ten times is 1.0, not 0.99999.
        const int decimals = decimalsForStep(m_desc) + (scale < 1.0 ? 1 : 0);
        const double unit = std::pow(10.0, decimals);
        shown = std::clamp(shown + steps * step, m_desc.min, m_desc.max);
        shown = std::round(shown * unit) / unit;
        applyScalarOnce(shown);
        return true;
    }
    if (m_desc.type == ParamType::Bool) {
        const bool on = !paramaccess::staticValue(*clip, m_ref, m_desc).toBool();
        m_host.editClip(changeLabel(), m_ref.clipId,
            [this, on](ClipData& c) { paramaccess::setStatic(c, m_ref, on); });
        return true;
    }
    if (m_desc.type == ParamType::Choice && !m_desc.choices.isEmpty()) {
        const int n = int(m_desc.choices.size());
        const int at = std::max<qsizetype>(
            0, m_desc.choices.indexOf(paramaccess::staticValue(*clip, m_ref, m_desc).toString()));
        const QString value = m_desc.choices[((at + steps) % n + n) % n];
        m_host.editClip(changeLabel(), m_ref.clipId,
            [this, value](ClipData& c) { paramaccess::setStatic(c, m_ref, value); });
        return true;
    }
    return false;
}

bool ParamRow::vimReset()
{
    if (!m_reset || !m_reset->isEnabled())
        return false;
    m_reset->click();
    return true;
}

bool ParamRow::vimEdit()
{
    if (!m_main)
        return false;
    if (auto* field = qobject_cast<ValueField*>(m_main)) {
        field->setFocus(Qt::OtherFocusReason);
        field->startTextEdit();
        return true;
    }
    if (auto* line = qobject_cast<QLineEdit*>(m_main)) {
        if (line->isReadOnly())
            return false;
        line->setFocus(Qt::OtherFocusReason);
        line->end(false);
        return true;
    }
    if (auto* text = qobject_cast<QPlainTextEdit*>(m_main)) {
        text->setFocus(Qt::OtherFocusReason);
        text->moveCursor(QTextCursor::End);
        return true;
    }
    return vimActivate();
}

bool ParamRow::vimActivate()
{
    if (!m_main)
        return false;
    if (qobject_cast<ValueField*>(m_main) || qobject_cast<QLineEdit*>(m_main)
        || qobject_cast<QPlainTextEdit*>(m_main))
        return vimEdit();
    if (auto* button = qobject_cast<QAbstractButton*>(m_main)) {
        if (!button->isEnabled())
            return false;
        button->click();
        return true;
    }
    if (auto* combo = qobject_cast<QComboBox*>(m_main)) {
        combo->setFocus(Qt::OtherFocusReason);
        combo->showPopup();
        return true;
    }
    return false;
}

QVariant ParamRow::vimValue() const
{
    const ClipData* clip = m_host.clip(m_ref.clipId);
    if (!clip || m_desc.type == ParamType::Curve)
        return {};
    QVariant value;
    if (m_desc.isScalar()) {
        const int64_t frame = m_host.localFrame(m_ref.clipId);
        value = m_desc.displayed(paramaccess::scalar(*clip, m_ref, m_desc, frame));
    } else {
        value = paramaccess::staticValue(*clip, m_ref, m_desc);
    }
    return QVariantList {int(m_desc.type), value};
}

bool ParamRow::vimSetValue(const QVariant& packed)
{
    const QVariantList list = packed.toList();
    if (list.size() != 2 || list[0].toInt() != int(m_desc.type) || m_desc.type == ParamType::Curve)
        return false;
    if (m_desc.isScalar()) {
        applyScalarOnce(std::clamp(list[1].toDouble(), m_desc.min, m_desc.max));
        return true;
    }
    const QVariant value = list[1];
    m_host.editClip(changeLabel(), m_ref.clipId,
        [this, value](ClipData& c) { paramaccess::setStatic(c, m_ref, value); });
    return true;
}

bool ParamRow::vimToggleKeyframe()
{
    if (!keyable())
        return false;
    toggleKey();
    return true;
}

bool ParamRow::vimJumpKeyframe(bool forward)
{
    if (!keyable())
        return false;
    jump(forward);
    return true;
}

bool ParamRow::vimDeleteKeyframe()
{
    const ClipData* clip = m_host.clip(m_ref.clipId);
    if (!keyable() || !clip)
        return false;
    const AnimatedValue* anim = paramaccess::animated(*clip, m_ref);
    if (!anim || !anim->hasKeyAt(m_host.localFrame(m_ref.clipId)))
        return false;
    toggleKey();
    return true;
}

bool ParamRow::vimSetInterp(Ease ease)
{
    const ClipData* clip = m_host.clip(m_ref.clipId);
    if (!keyable() || !clip)
        return false;
    const int64_t frame = m_host.localFrame(m_ref.clipId);
    if (!paramaccess::keyInterp(*clip, m_ref, frame))
        return false;
    kk::Interp mode = kk::Interp::Linear;
    switch (ease) {
    case Ease::Linear:
        mode = kk::Interp::Linear;
        break;
    case Ease::EaseIn:
        mode = kk::Interp::EaseIn;
        break;
    case Ease::EaseOut:
        mode = kk::Interp::EaseOut;
        break;
    case Ease::EaseInOut:
        mode = kk::Interp::EaseInOut;
        break;
    case Ease::Smooth:
        mode = kk::Interp::Smooth;
        break;
    case Ease::Hold:
        mode = kk::Interp::Hold;
        break;
    }
    m_host.editClip(tr("Change Keyframe Interpolation"), m_ref.clipId,
        [this, frame, mode](ClipData& c) { paramaccess::setKeyInterp(c, m_ref, frame, mode); });
    return true;
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
    case ParamType::Curve:
        return new CurveRow(ref, desc, host, grid, gridRow, parentWidget);
    case ParamType::File:
        return new FileRow(ref, desc, host, grid, gridRow, parentWidget);
    }
    return nullptr;
}

} // namespace kk
