// SPDX-License-Identifier: GPL-3.0-or-later
// RenderQueuePanel — job cards and queue controls.
#include "ui/deliver/RenderQueuePanel.h"

#include "core/Plural.h"
#include "export/RenderQueue.h"
#include "platform/OpenUrl.h"
#include "ui/common/Icons.h"
#include "ui/common/PanelInfo.h"
#include "ui/common/ThinProgressBar.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace kk {

/// One job of the queue.
class RenderJobCard : public QFrame
{
    Q_DECLARE_TR_FUNCTIONS(kk::RenderJobCard)

public:
    RenderJobCard(RenderQueue* queue, quint64 id, std::function<void(quint64)> onEdit,
        QWidget* parent = nullptr)
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
        m_edit = toolButton(QStringLiteral("edit"), tr("Edit this job"));
        m_edit->setObjectName(QStringLiteral("RenderJobEdit"));
        m_cancel = toolButton(QStringLiteral("stop"), tr("Cancel this render"));
        m_reveal = toolButton(QStringLiteral("folder"), tr("Show the output folder"));
        m_remove = toolButton(QStringLiteral("remove"), tr("Remove from the queue"));
        top->addWidget(m_edit);
        top->addWidget(m_cancel);
        top->addWidget(m_reveal);
        top->addWidget(m_remove);
        layout->addLayout(top);
        m_details = new QLabel;
        m_details->setEnabled(false); // dimmed secondary text
        layout->addWidget(m_details);
        m_progress = new ThinProgressBar;
        m_progress->setObjectName(QStringLiteral("RenderJobProgress"));
        m_progress->setRange(0, 1000);
        layout->addWidget(m_progress);
        m_status = new QLabel;
        m_status->setWordWrap(true);
        layout->addWidget(m_status);

        connect(m_edit, &QToolButton::clicked, this,
            [this, onEdit = std::move(onEdit)] { onEdit(m_id); });
        connect(m_cancel, &QToolButton::clicked, this, [this] { m_queue->cancel(m_id); });
        connect(m_remove, &QToolButton::clicked, this, [this] { m_queue->removeJob(m_id); });
        connect(m_reveal, &QToolButton::clicked, this, [this] {
            if (const RenderQueue::Job* job = m_queue->job(m_id)) {
                const QString path = job->result.outputPath.isEmpty() ? job->settings.outputPath
                                                                      : job->result.outputPath;
                // platform::openUrl: the file manager gets the user's environment
                platform::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
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
        details += plural(end - first, tr(" · %n frame"), tr(" · %n frames"));
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
        m_edit->setEnabled(job->status != RenderQueue::Status::Rendering);
    }

    /// Highlights the card while its settings are being edited on the left.
    void setEditing(bool editing)
    {
        setProperty("editing", editing);
        setFrameShadow(editing ? QFrame::Sunken : QFrame::Plain);
        m_edit->setDown(editing);
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
    ThinProgressBar* m_progress = nullptr;
    QToolButton* m_edit = nullptr;
    QToolButton* m_cancel = nullptr;
    QToolButton* m_reveal = nullptr;
    QToolButton* m_remove = nullptr;
};

RenderQueuePanel::RenderQueuePanel(RenderQueue* queue, QWidget* parent)
    : PanelFrame(tr("Render Queue"), parent)
    , m_queue(queue)
{
    setPanelInfo(this, tr("Render Queue"),
        tr("One card for each render job with its progress; Render All, Stop and Clear Finished."));
    setPanelKey(QStringLiteral("render_queue")); // in-app help: panel:render_queue
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

    // What the last run did (Resolve's "render complete" moment), shown when the queue stops.
    m_summary = new QLabel;
    m_summary->setObjectName(QStringLiteral("RenderSummary"));
    m_summary->setWordWrap(true);
    m_summary->hide();
    layout->addWidget(m_summary);

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
    connect(m_queue, &RenderQueue::runningChanged, this, &RenderQueuePanel::onRunningChanged);
    for (const RenderQueue::Job& job : m_queue->jobs()) {
        addCard(job.id);
    }
    updateButtons();
}

QString RenderQueuePanel::summaryText() const
{
    return m_summary->isVisible() || !m_summary->text().isEmpty() ? m_summary->text() : QString();
}

void RenderQueuePanel::onRunningChanged(bool running)
{
    // The queue may pause between jobs: a run lasts until nothing is queued or rendering.
    bool pending = false;
    for (const RenderQueue::Job& job : m_queue->jobs()) {
        const bool open = job.status == RenderQueue::Status::Queued
            || job.status == RenderQueue::Status::Rendering;
        if (open && !m_runStarted.contains(job.id) && (running || m_queue->isRunning()))
            m_runStarted.append(job.id);
        pending |= open;
    }
    if (running) {
        m_summary->hide();
        return;
    }
    if (pending || m_runStarted.isEmpty())
        return;
    int done = 0;
    int failed = 0;
    int cancelled = 0;
    for (const RenderQueue::Job& job : m_queue->jobs()) {
        if (!m_runStarted.contains(job.id))
            continue;
        done += job.status == RenderQueue::Status::Done;
        failed += job.status == RenderQueue::Status::Failed;
        cancelled += job.status == RenderQueue::Status::Cancelled;
    }
    m_runStarted.clear();
    QStringList parts;
    if (done > 0)
        parts << (done == 1 ? tr("1 job rendered") : tr("%1 jobs rendered").arg(done));
    if (failed > 0)
        parts << tr("%1 failed").arg(failed);
    if (cancelled > 0)
        parts << tr("%1 stopped").arg(cancelled);
    if (parts.isEmpty())
        return;
    const QString text
        = (failed > 0 ? tr("Render finished with errors: %1") : tr("Render complete: %1"))
              .arg(parts.join(QStringLiteral(", ")));
    m_summary->setText(text);
    m_summary->show();
    // Long renders run in the background: flash the task bar entry when the window is not
    // active (KDE / Windows "needs attention"), as Resolve signals a finished render.
    QApplication::alert(window());
    emit runFinished(text);
}

void RenderQueuePanel::addCard(quint64 id)
{
    auto* card = new RenderJobCard(m_queue, id, [this](quint64 job) { emit editRequested(job); });
    m_cards.insert(id, card);
    m_list->insertWidget(m_list->count() - 1, card); // before the stretch
    updateButtons();
}

void RenderQueuePanel::setEditingJob(quint64 id)
{
    for (auto it = m_cards.cbegin(); it != m_cards.cend(); ++it)
        it.value()->setEditing(it.key() == id);
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
