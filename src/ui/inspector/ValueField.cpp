// SPDX-License-Identifier: GPL-3.0-or-later
// ValueField implementation: self-painted value, drag scrubbing, inline text editing.
#include "ui/inspector/ValueField.h"

#include <QApplication>
#include <QDoubleValidator>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr int kDragThresholdPx = 3; ///< movement before a press turns into a scrub
constexpr double kFineFactor = 0.1; ///< Shift
constexpr double kCoarseFactor = 10.0; ///< Ctrl
constexpr int kFieldHeight = 20;
constexpr int kFieldMinWidth = 64;
constexpr int kHorizontalPadding = 6;
} // namespace

ValueField::ValueField(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::SizeHorCursor);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_text = textForValue(m_value);
}

void ValueField::setRange(double min, double max)
{
    m_min = std::min(min, max);
    m_max = std::max(min, max);
    setValue(m_value);
}

void ValueField::setStep(double step)
{
    m_step = step > 0 ? step : 1.0;
}

void ValueField::setDecimals(int decimals)
{
    m_decimals = std::clamp(decimals, 0, 6);
    m_text = textForValue(m_value);
    update();
}

void ValueField::setSuffix(const QString& suffix)
{
    m_suffix = suffix;
    m_text = textForValue(m_value);
    updateGeometry();
    update();
}

void ValueField::setValue(double value)
{
    value = clamp(value);
    // Exact comparison is intended: we only want to skip work when nothing changed at all.
    if (value == m_value && !m_text.isEmpty())
        return;
    m_value = value;
    m_text = textForValue(value);
    update();
}

QString ValueField::textForValue(double value) const
{
    // Values that round to zero are shown as "0", never as "-0.00".
    const double scale = std::pow(10.0, m_decimals);
    if (std::abs(value) * scale < 0.5)
        value = 0.0;
    QString text = QString::number(value, 'f', m_decimals);
    if (!m_suffix.isEmpty()) {
        // Degree signs and percent hug the number, word-like units get a space.
        const bool tight = m_suffix == QLatin1String("%") || m_suffix == QStringLiteral("°");
        text += tight ? m_suffix : QLatin1Char(' ') + m_suffix;
    }
    return text;
}

double ValueField::clamp(double value) const
{
    if (!std::isfinite(value))
        return m_min;
    return std::clamp(value, m_min, m_max);
}

double ValueField::modifierStep(Qt::KeyboardModifiers modifiers) const
{
    if (modifiers & Qt::ShiftModifier)
        return m_step * kFineFactor;
    if (modifiers & Qt::ControlModifier)
        return m_step * kCoarseFactor;
    return m_step;
}

QSize ValueField::sizeHint() const
{
    const QFontMetrics fm(font());
    const int width = fm.horizontalAdvance(textForValue(m_max)) + 2 * kHorizontalPadding;
    return {std::max(width, kFieldMinWidth), std::max(kFieldHeight, fm.height() + 4)};
}

QSize ValueField::minimumSizeHint() const
{
    return {kFieldMinWidth, sizeHint().height()};
}

void ValueField::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QPalette& pal = palette();
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.fillRect(rect(), pal.color(QPalette::Window));
    p.setRenderHint(QPainter::Antialiasing);

    QColor background = pal.color(QPalette::Base);
    if (underMouse() || m_scrubbing)
        background = background.lighter(125);
    QColor border = hasFocus() ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid);
    if (!isEnabled())
        background = pal.color(QPalette::Window);
    p.setPen(border);
    p.setBrush(background);
    p.drawRoundedRect(r, 3, 3);

    if (m_editor && m_editor->isVisible())
        return;
    // Accent-coloured value text marks it as interactive, like most inspector fields.
    p.setPen(isEnabled() ? pal.color(QPalette::Highlight).lighter(130)
                         : pal.color(QPalette::Disabled, QPalette::Text));
    p.drawText(rect().adjusted(kHorizontalPadding, 0, -kHorizontalPadding, 0),
        Qt::AlignVCenter | Qt::AlignRight, m_text);
}

void ValueField::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    m_pressed = true;
    m_scrubbing = false;
    m_pressX = m_lastX = qRound(event->position().x());
    m_dragValue = m_value;
}

void ValueField::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_pressed)
        return;
    const int x = qRound(event->position().x());
    if (!m_scrubbing) {
        if (std::abs(x - m_pressX) < kDragThresholdPx)
            return;
        m_scrubbing = true;
        m_lastX = x;
        emit scrubStarted();
        update();
        return;
    }
    // Incremental: each pixel since the last event counts with the *current* modifiers.
    m_dragValue = clamp(m_dragValue + (x - m_lastX) * modifierStep(event->modifiers()));
    m_lastX = x;
    const double before = m_value;
    setValue(m_dragValue);
    if (m_value != before)
        emit valueScrubbed(m_value);
}

void ValueField::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_pressed)
        return;
    m_pressed = false;
    if (m_scrubbing) {
        m_scrubbing = false;
        update();
        emit scrubFinished();
    }
}

void ValueField::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        startTextEdit();
}

void ValueField::wheelEvent(QWheelEvent* event)
{
    // Only a focused field reacts, so scrolling the inspector never changes values by accident.
    if (!hasFocus()) {
        event->ignore();
        return;
    }
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0)
        return;
    const double before = m_value;
    setValue(m_value + notches * modifierStep(event->modifiers()));
    if (m_value != before)
        emit valueCommitted(m_value);
    event->accept();
}

void ValueField::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        startTextEdit();
        return;
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const double sign = event->key() == Qt::Key_Up ? 1.0 : -1.0;
        const double before = m_value;
        setValue(m_value + sign * modifierStep(event->modifiers()));
        if (m_value != before)
            emit valueCommitted(m_value);
        return;
    }
    default:
        QWidget::keyPressEvent(event);
    }
}

void ValueField::startTextEdit()
{
    if (!m_editor) {
        m_editor = new QLineEdit(this);
        m_editor->setFrame(false);
        m_editor->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* validator = new QDoubleValidator(m_editor);
        validator->setNotation(QDoubleValidator::StandardNotation);
        validator->setLocale(QLocale::c());
        m_editor->setValidator(validator);
        connect(m_editor, &QLineEdit::editingFinished, this, [this] { finishTextEdit(true); });
        m_editor->installEventFilter(this);
    }
    auto* validator
        = static_cast<QDoubleValidator*>(const_cast<QValidator*>(m_editor->validator()));
    validator->setRange(m_min, m_max, m_decimals);
    m_editor->setGeometry(rect().adjusted(2, 2, -2, -2));
    m_editor->setText(QString::number(m_value, 'f', m_decimals));
    m_editor->selectAll();
    m_editor->show();
    m_editor->setFocus(Qt::OtherFocusReason);
    update();
}

void ValueField::finishTextEdit(bool commit)
{
    if (!m_editor || !m_editor->isVisible())
        return;
    // Hide first: hiding moves the focus, which would re-enter editingFinished.
    const QString text = m_editor->text();
    m_editor->hide();
    setFocus(Qt::OtherFocusReason);
    update();
    if (!commit)
        return;
    bool ok = false;
    const double typed = QLocale::c().toDouble(text, &ok);
    if (!ok)
        return;
    const double before = m_value;
    setValue(typed);
    if (m_value != before)
        emit valueCommitted(m_value);
}

bool ValueField::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_editor && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        finishTextEdit(false);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace kk
