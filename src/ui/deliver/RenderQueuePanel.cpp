// SPDX-License-Identifier: GPL-3.0-or-later
// RenderQueuePanel — job cards and queue controls.
#include "ui/deliver/RenderQueuePanel.h"

#include "export/RenderQueue.h"
#include "ui/common/Icons.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace kk {

/// One job of the queue.
class RenderJobCard : public QFrame
{
public:
    RenderJobCard(RenderQueue* queue, quint64 id, QWidget* parent = nullptr)
        : QFrame(parent)
        , m_queue(queue)
        , m_id(id)
    {
        setObjectName(QStringLiteral("RenderJobCard"));
        setFrameShape(QFrame::StyledPanel);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 6, 8, 6);
        layout->setSpacing(3);
        auto* top = new QHBoxLayout;
        m_name = new QLabel;
        QFont bold = m_name->font();
        bold.setBold(true);
        m_name->setFont(bold);
        top->addWidget(m_name, 1);
        m_cancel = toolButton(QStringLiteral("stop"), tr("Cancel this render"));
        m_reveal = toolButton(QStringLiteral("folder"), tr("Show the output folder"));
        m_remove = toolButton(QStringLiteral("remove"), tr("Remove from the queue"));
        top->addWidget(m_cancel);
        top->addWidget(m_reveal);
        top->addWidget(m_remove);
        layout->addLayout(top);
        m_details = new QLabel;
        m_details->setEnabled(false); // dimmed secondary text
        layout->addWidget(m_details);
        m_progress = new QProgressBar;
        m_progress->setRange(0, 1000);
        m_progress->setTextVisible(false);
        m_progress->setMaximumHeight(8);
        layout->addWidget(m_progress);
        m_status = new QLabel;
        m_status->setWordWrap(true);
        layout->addWidget(m_status);

        connect(m_cancel, &QToolButton::clicked, this, [this] { m_queue->cancel(m_id); });
        connect(m_remove, &QToolButton::clicked, this, [this] { m_queue->removeJob(m_id); });
        connect(m_reveal, &QToolButton::clicked, this, [this] {
            if (const RenderQueue::Job* job = m_queue->job(m_id)) {
                const QString path = job->result.outputPath.isEmpty() ? job->settings.outputPath
                                                                      : job->result.outputPath;
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
            }
        });
        refresh();
    }

    void refresh()
    {
        const RenderQueue::Job* job = m_queue->job(m_id);
        if (!job) {
            return;
        }
        m_name->setText(job->name);
        const ExportSettings& s = job->settings;
        const QSize size = s.outputSize(*job->timeline);
        const auto [first, end] = s.frameRange(*job->timeline);
        QString details = QFileInfo(s.outputPath).fileName();
        if (s.hasVideo()) {
            details += QStringLiteral(" · %1x%2").arg(size.width()).arg(size.height());
        }
        details += tr(" · %n frame(s)", nullptr, int(end - first));
        m_details->setText(details);
        m_details->setToolTip(QDir::toNativeSeparators(s.outputPath));
        m_status->setText(RenderQueue::statusText(*job));
        m_status->setToolTip(
            job->status == RenderQueue::Status::Failed ? job->result.error : QString());
        const bool finished = job->status != RenderQueue::Status::Queued
            && job->status != RenderQueue::Status::Rendering;
        int value = int(job->progress.fraction() * 1000);
        if (job->status == RenderQueue::Status::Done) {
            value = 1000;
        }
        m_progress->setValue(value);
        m_progress->setVisible(job->status != RenderQueue::Status::Queued);
        m_cancel->setVisible(!finished);
        m_reveal->setVisible(job->status == RenderQueue::Status::Done);
        m_remove->setEnabled(job->status != RenderQueue::Status::Rendering);
    }

private:
    QToolButton* toolButton(const QString& iconName, const QString& tip)
    {
        auto* b = new QToolButton;
        b->setIcon(icon(iconName));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    }

    RenderQueue* m_queue;
    quint64 m_id;
    QLabel* m_name = nullptr;
    QLabel* m_details = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QToolButton* m_cancel = nullptr;
    QToolButton* m_reveal = nullptr;
    QToolButton* m_remove = nullptr;
};

RenderQueuePanel::RenderQueuePanel(RenderQueue* queue, QWidget* parent)
    : PanelFrame(tr("Render Queue"), parent)
    , m_queue(queue)
{
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* listHost = new QWidget;
    m_list = new QVBoxLayout(listHost);
    m_list->setContentsMargins(0, 0, 0, 0);
    m_list->setSpacing(6);
    m_empty = new QLabel(tr("No jobs. Choose render settings and press \"Add to Render Queue\"."));
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setEnabled(false);
    m_list->addWidget(m_empty);
    m_list->addStretch(1);
    scroll->setWidget(listHost);
    layout->addWidget(scroll, 1);

    auto* buttons = new QHBoxLayout;
    m_renderAll = new QPushButton(icon(QStringLiteral("play")), tr("Render All"));
    m_stop = new QPushButton(icon(QStringLiteral("stop")), tr("Stop"));
    m_clear = new QPushButton(tr("Clear Finished"));
    buttons->addWidget(m_renderAll, 1);
    buttons->addWidget(m_stop);
    buttons->addWidget(m_clear);
    layout->addLayout(buttons);
    setContentWidget(body);

    connect(m_renderAll, &QPushButton::clicked, m_queue, &RenderQueue::startAll);
    connect(m_stop, &QPushButton::clicked, m_queue, &RenderQueue::stop);
    connect(m_clear, &QPushButton::clicked, m_queue, &RenderQueue::clearFinished);
    connect(m_queue, &RenderQueue::jobAdded, this, &RenderQueuePanel::addCard);
    connect(m_queue, &RenderQueue::jobRemoved, this, &RenderQueuePanel::removeCard);
    connect(m_queue, &RenderQueue::jobChanged, this, &RenderQueuePanel::updateCard);
    connect(m_queue, &RenderQueue::runningChanged, this, &RenderQueuePanel::updateButtons);
    for (const RenderQueue::Job& job : m_queue->jobs()) {
        addCard(job.id);
    }
    updateButtons();
}

void RenderQueuePanel::addCard(quint64 id)
{
    auto* card = new RenderJobCard(m_queue, id);
    m_cards.insert(id, card);
    m_list->insertWidget(m_list->count() - 1, card); // before the stretch
    updateButtons();
}

void RenderQueuePanel::removeCard(quint64 id)
{
    if (RenderJobCard* card = m_cards.take(id)) {
        card->deleteLater();
    }
    updateButtons();
}

void RenderQueuePanel::updateCard(quint64 id)
{
    if (RenderJobCard* card = m_cards.value(id)) {
        card->refresh();
    }
    updateButtons();
}

void RenderQueuePanel::updateButtons()
{
    bool queued = false;
    bool finished = false;
    for (const RenderQueue::Job& job : m_queue->jobs()) {
        queued |= job.status == RenderQueue::Status::Queued;
        finished |= job.status == RenderQueue::Status::Done
            || job.status == RenderQueue::Status::Failed
            || job.status == RenderQueue::Status::Cancelled;
    }
    m_empty->setVisible(m_cards.isEmpty());
    m_renderAll->setEnabled(queued && !m_queue->isRunning());
    m_stop->setEnabled(m_queue->isRunning());
    m_clear->setEnabled(finished);
}

} // namespace kk
