// SPDX-License-Identifier: GPL-3.0-or-later
// ScopesWindow — floating window with two video scopes.
#include "ui/scopes/ScopesWindow.h"

#include "ui/scopes/ScopeFeeder.h"
#include "ui/scopes/ScopeView.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QSettings>

namespace kk {

ScopesWindow::ScopesWindow(PlaybackEngine* engine, QWidget* parent)
    : QWidget(parent, Qt::Tool)
{
    setObjectName(QStringLiteral("ScopesWindow"));
    setWindowTitle(tr("Video Scopes"));
    resize(760, 300);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    m_left = new ScopesPanel(QStringLiteral("scopesWindow/left"), this);
    m_right = new ScopesPanel(QStringLiteral("scopesWindow/right"), this);
    if (!QSettings().contains(QStringLiteral("scopesWindow/left")))
        m_left->setType(scopes::ScopeType::Parade);
    if (!QSettings().contains(QStringLiteral("scopesWindow/right")))
        m_right->setType(scopes::ScopeType::Vectorscope);
    layout->addWidget(m_left);
    layout->addWidget(m_right);
    m_feeder = new ScopeFeeder(engine, this);
    connect(m_feeder, &ScopeFeeder::imageReady, m_left, &ScopesPanel::setImage);
    connect(m_feeder, &ScopeFeeder::imageReady, m_right, &ScopesPanel::setImage);
}

void ScopesWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_feeder->setActive(true);
}

void ScopesWindow::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_feeder->setActive(false);
}

void ScopesWindow::closeEvent(QCloseEvent* event)
{
    QWidget::closeEvent(event);
    emit closed();
}

} // namespace kk
