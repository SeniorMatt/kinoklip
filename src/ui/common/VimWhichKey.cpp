// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimWhichKey.h"

#include "ui/common/Metrics.h"
#include "ui/common/VimKeys.h"

#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {
constexpr int kRowsPerColumn = 8;
constexpr int kMaxEntries = 64;
} // namespace

VimWhichKey::VimWhichKey(QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("VimWhichKey"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_ShowWithoutActivating);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceS, metrics::kSpaceM, metrics::kSpaceS);
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("VimWhichKeyTitle"));
    m_title->setStyleSheet(QStringLiteral("QLabel { font-weight: 600; }"));
    m_body = new QWidget(this);
    m_grid = new QGridLayout(m_body);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setHorizontalSpacing(metrics::kSpaceL);
    layout->addWidget(m_title);
    layout->addWidget(m_body);
    hide();
}

QString VimWhichKey::title() const
{
    return m_title->text();
}

void VimWhichKey::hideEntries()
{
    hide();
}

void VimWhichKey::showEntries(
    const QString& prefix, const QList<VimContinuation>& entries, int bottomOffset)
{
    while (QLayoutItem* item = m_grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_rows.clear();
    m_title->setText(prefix);
    int index = 0;
    for (const VimContinuation& e : entries) {
        if (index >= kMaxEntries)
            break;
        const QString key = vimkeys::displayToken(e.token);
        const QString text = e.more && !e.binding ? QStringLiteral("+") + e.desc : e.desc;
        m_rows.append(key + QStringLiteral("  ") + text);
        auto* cell = new QLabel(
            QStringLiteral("<b>%1</b> &rarr; %2").arg(key.toHtmlEscaped(), text.toHtmlEscaped()),
            m_body);
        cell->setTextFormat(Qt::RichText);
        m_grid->addWidget(cell, index % kRowsPerColumn, index / kRowsPerColumn);
        ++index;
    }
    if (m_rows.isEmpty()) {
        hide();
        return;
    }
    QWidget* host = parentWidget();
    if (!host)
        return;
    adjustSize();
    const int width
        = std::min(host->width() - 2 * metrics::kSpaceM, std::max(sizeHint().width(), 360));
    resize(width, sizeHint().height());
    move(metrics::kSpaceM, host->height() - bottomOffset - height() - metrics::kSpaceS);
    raise();
    show();
}

} // namespace kk
