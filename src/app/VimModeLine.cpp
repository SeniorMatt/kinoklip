// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/VimModeLine.h"

#include "ui/common/Metrics.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>

namespace kk {

namespace {

QString chipStyle(const QString& mode)
{
    // Fixed colours with enough contrast on both light and dark schemes.
    QString background = QStringLiteral("palette(highlight)");
    QString text = QStringLiteral("palette(highlighted-text)");
    if (mode == QLatin1String("INSERT"))
        background = QStringLiteral("#2e7d32"), text = QStringLiteral("#ffffff");
    else if (mode == QLatin1String("VISUAL"))
        background = QStringLiteral("#b26a00"), text = QStringLiteral("#ffffff");
    else if (mode == QLatin1String("COMMAND"))
        background = QStringLiteral("#6a3d9a"), text = QStringLiteral("#ffffff");
    return QStringLiteral("QLabel#VimModeChip { font-weight: 600; padding: 1px 8px; "
                          "border-radius: 3px; background: %1; color: %2; }")
        .arg(background, text);
}

} // namespace

VimModeLine::VimModeLine(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("VimModeLine"));
    setToolTip(tr("Vim mode: the mode, the keys typed so far and the focused panel"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(metrics::kSpaceM);
    m_chip = new QLabel(this);
    m_chip->setObjectName(QStringLiteral("VimModeChip"));
    m_chip->setMinimumWidth(78);
    m_chip->setAlignment(Qt::AlignCenter);
    m_pending = new QLabel(this);
    m_pending->setObjectName(QStringLiteral("VimPending"));
    m_pending->setStyleSheet(
        QStringLiteral("QLabel { font-family: monospace; font-weight: 600; }"));
    m_pending->setMinimumWidth(40);
    m_panel = new QLabel(this);
    m_panel->setObjectName(QStringLiteral("VimPanelName"));
    m_message = new QLabel(this);
    m_message->setObjectName(QStringLiteral("VimMessage"));
    layout->addWidget(m_chip);
    layout->addWidget(m_pending);
    layout->addWidget(m_panel);
    layout->addWidget(m_message, 1);
    m_messageTimer = new QTimer(this);
    m_messageTimer->setSingleShot(true);
    connect(m_messageTimer, &QTimer::timeout, this, &VimModeLine::clearMessage);
    setMode(tr("NORMAL"), QStringLiteral("NORMAL"));
    setMinimumWidth(330);
}

void VimModeLine::setMode(const QString& text, const QString& mode)
{
    m_chip->setText(text);
    m_chip->setStyleSheet(chipStyle(mode));
}

void VimModeLine::setPending(const QString& keys)
{
    m_pending->setText(keys);
}

void VimModeLine::setPanel(const QString& name)
{
    m_panel->setText(name);
}

void VimModeLine::showMessage(const QString& text, int ms)
{
    m_message->setText(text);
    if (ms > 0)
        m_messageTimer->start(ms);
    else
        m_messageTimer->stop();
}

void VimModeLine::clearMessage()
{
    m_message->clear();
}

QString VimModeLine::modeText() const
{
    return m_chip->text();
}

QString VimModeLine::pendingText() const
{
    return m_pending->text();
}

QString VimModeLine::panelText() const
{
    return m_panel->text();
}

QString VimModeLine::messageText() const
{
    return m_message->text();
}

} // namespace kk
