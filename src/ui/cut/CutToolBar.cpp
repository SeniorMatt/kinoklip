// SPDX-License-Identifier: GPL-3.0-or-later
// CutToolBar implementation.
#include "ui/cut/CutToolBar.h"

#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"

#include <QAction>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QToolButton>

namespace kk {

namespace {

constexpr int kMessageMs = 5000; ///< how long a refused-edit message stays

/// Left-hand groups in Resolve's Cut page order; true = show the name next to the icon.
struct Entry
{
    const char* id;
    bool withText;
};
constexpr Entry kSmartEdits[]
    = {{"cut.smart_insert", true}, {"cut.append", true}, {"cut.ripple_overwrite", true},
        {"cut.close_up", true}, {"cut.place_on_top", true}, {"cut.source_overwrite", true}};
constexpr Entry kTools[]
    = {{"edit.split", false}, {"edit.ripple_delete", false}, {"timeline.snapping", false}};

QFrame* separator()
{
    auto* line = new QFrame;
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

} // namespace

CutToolBar::CutToolBar(AppContext& ctx, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    setObjectName(QStringLiteral("CutToolBar"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceS, metrics::kSpaceXs, metrics::kSpaceS, metrics::kSpaceXs);
    layout->setSpacing(metrics::kSpaceXs);
    for (const Entry& e : kSmartEdits) {
        if (QToolButton* b = addActionButton(e.id, e.withText))
            layout->addWidget(b);
    }
    layout->addWidget(separator());
    for (const Entry& e : kTools) {
        if (QToolButton* b = addActionButton(e.id, e.withText))
            layout->addWidget(b);
    }
    // The quick dissolve button: Add Default Transition (a cross dissolve at the edit nearest
    // to the playhead) with a dissolve icon; a plain button so the action keeps its own icon.
    if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("timeline.add_transition"))) {
        QAction* transition = m_ctx.actions->action(QStringLiteral("timeline.add_transition"));
        auto* b = new QToolButton(this);
        b->setObjectName(QStringLiteral("CutDissolveButton"));
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setIcon(icon(QStringLiteral("dissolve")));
        b->setIconSize(QSize(metrics::kTransportIconSize, metrics::kTransportIconSize));
        // The key follows Keyboard Customization.
        m_ctx.actions->bindToolTip(b, QStringLiteral("timeline.add_transition"), tr("Dissolve"));
        b->setEnabled(transition->isEnabled());
        connect(b, &QToolButton::clicked, transition, &QAction::trigger);
        connect(transition, &QAction::changed, b,
            [b, transition] { b->setEnabled(transition->isEnabled()); });
        layout->addWidget(b);
    }
    layout->addWidget(separator());
    if (QToolButton* review = addActionButton("cut.fast_review", true))
        layout->addWidget(review);

    m_message = new QLabel(this);
    m_message->setObjectName(QStringLiteral("CutToolBarMessage"));
    m_message->setContentsMargins(metrics::kSpaceM, 0, metrics::kSpaceM, 0);
    layout->addWidget(m_message, 1);
    m_messageTimer = new QTimer(this);
    m_messageTimer->setSingleShot(true);
    m_messageTimer->setInterval(kMessageMs);
    connect(m_messageTimer, &QTimer::timeout, m_message, &QLabel::clear);

    // Zoom of the lower timeline (the registry zoom actions also reach the page).
    for (const auto& [name, zoomIn] : {std::pair {"zoom_out", false}, {"zoom_in", true}}) {
        auto* b = new QToolButton(this);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setIcon(icon(QLatin1String(name)));
        b->setIconSize(QSize(metrics::kTransportIconSize, metrics::kTransportIconSize));
        b->setToolTip(
            zoomIn ? tr("Zoom the lower timeline in") : tr("Zoom the lower timeline out"));
        connect(b, &QToolButton::clicked, this,
            zoomIn ? &CutToolBar::zoomInRequested : &CutToolBar::zoomOutRequested);
        layout->addWidget(b);
    }
}

QToolButton* CutToolBar::addActionButton(const char* actionId, bool withText)
{
    if (!m_ctx.actions || !m_ctx.actions->contains(QLatin1String(actionId)))
        return nullptr;
    QAction* action = m_ctx.actions->action(QLatin1String(actionId));
    auto* b = new QToolButton(this);
    b->setObjectName(QStringLiteral("CutButton_") + QLatin1String(actionId));
    b->setDefaultAction(action);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setIconSize(QSize(metrics::kTransportIconSize, metrics::kTransportIconSize));
    b->setToolButtonStyle(withText ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    // The tooltip is the action's own: its text and current shortcut, kept up to date by the
    // registry (QToolButton copies it again whenever the action changes).
    return b;
}

QToolButton* CutToolBar::button(const QString& actionId) const
{
    return findChild<QToolButton*>(QStringLiteral("CutButton_") + actionId);
}

void CutToolBar::showMessage(const QString& text)
{
    m_message->setText(text);
    if (text.isEmpty())
        m_messageTimer->stop();
    else
        m_messageTimer->start();
}

} // namespace kk
