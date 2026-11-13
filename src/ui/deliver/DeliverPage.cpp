// SPDX-License-Identifier: GPL-3.0-or-later
// DeliverPage — page layout, queueing, the playhead preview and hardware detection.
#include "ui/deliver/DeliverPage.h"

#include "core/Project.h"
#include "export/EncoderCatalog.h"
#include "export/ExportPresets.h"
#include "export/IndividualClips.h"
#include "export/RenderQueue.h"
#include "render/Compositor.h"
#include "render/ProxySelection.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelInfo.h"
#include "ui/common/PanelSplitter.h"
#include "ui/common/TimelineCommandSuspension.h"
#include "ui/deliver/RenderFinishNotifier.h"
#include "ui/deliver/RenderQueuePanel.h"
#include "ui/deliver/RenderRangeBar.h"
#include "ui/deliver/RenderSettingsPanel.h"
#include "ui/viewer/ViewerPanel.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QPointer>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>

namespace kk {

namespace {
constexpr int kPreviewDebounceMs = 40; ///< coalesce bursts of playhead/timeline changes
constexpr int kPreviewPollMs = 15; ///< check the in-flight preview at ~60 Hz
constexpr int kHwDetectionDelayMs = 1500; ///< see showEvent()
// Default column widths for a 1920 px window, scaled to the real width on the first show.
constexpr int kSettingsWidth = 440;
constexpr int kViewerWidth = 1060;
constexpr int kQueueWidth = 420;
} // namespace

DeliverPage::DeliverPage(AppContext& ctx, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
    , m_presets(std::make_unique<ExportPresets>())
{
    m_queue = new RenderQueue(this);
    m_settingsPanel = new RenderSettingsPanel(ctx, m_presets.get());
    m_viewer = new ViewerPanel(ViewerPanel::Mode::Timeline, ctx);
    m_queuePanel = new RenderQueuePanel(m_queue);
    m_finishNotifier = new RenderFinishNotifier(m_queue, this, this); // spec 45.5

    m_topRow = new PanelSplitter(Qt::Horizontal);
    m_topRow->setChildrenCollapsible(false);
    m_topRow->addWidget(m_settingsPanel);
    m_topRow->addWidget(m_viewer);
    m_topRow->addWidget(m_queuePanel);
    m_topRow->setStretchFactor(1, 1);
    m_topRow->setSizes({kSettingsWidth, kViewerWidth, kQueueWidth});

    m_rangeBar = new RenderRangeBar(ctx.project->timeline());
    setPanelInfo(m_rangeBar, tr("Render Range"),
        tr("The timeline overview under the viewer, showing the range that will be rendered."));
    m_main = new PanelSplitter(Qt::Vertical);
    m_main->setChildrenCollapsible(false);
    m_main->addWidget(m_topRow);
    m_main->addWidget(m_rangeBar);
    m_main->setStretchFactor(0, 1);
    m_main->setSizes({900, 80});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_main);

    connect(m_settingsPanel, &RenderSettingsPanel::addToQueueRequested, this,
        [this] { addCurrentToQueue(); });
    connect(m_settingsPanel, &RenderSettingsPanel::updateJobRequested, this,
        [this] { updateEditedJob(); });
    connect(
        m_settingsPanel, &RenderSettingsPanel::cancelEditRequested, this, &DeliverPage::cancelEdit);
    connect(m_queuePanel, &RenderQueuePanel::editRequested, this, &DeliverPage::editJob);
    connect(m_queue, &RenderQueue::jobRemoved, this, [this](quint64 id) {
        m_jobMediaChoice.remove(id);
        if (id == m_editingJob)
            leaveEditMode(false);
    });
    connect(
        m_settingsPanel, &RenderSettingsPanel::settingsChanged, this, &DeliverPage::updateRange);

    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &DeliverPage::schedulePreview);
    connect(timeline, &Timeline::changed, this, &DeliverPage::followMarks);
    connect(timeline, &Timeline::changed, this, &DeliverPage::updateRange);
    // A new timeline format (Project Settings, the first import's frame rate question)
    // updates the "Match timeline" and frame rate labels at once.
    connect(timeline, &Timeline::changed, this, [this] {
        const TimelineData& d = m_ctx.project->timeline()->data();
        const QString format = QStringLiteral("%1x%2@%3/%4")
                                   .arg(d.width)
                                   .arg(d.height)
                                   .arg(d.fps.num)
                                   .arg(d.fps.den);
        if (format == m_shownFormat)
            return;
        m_shownFormat = format;
        m_settingsPanel->setSettings(m_settingsPanel->settings());
    });
    connect(timeline, &Timeline::playheadChanged, this, &DeliverPage::schedulePreview);
    connect(timeline, &Timeline::playheadChanged, this, [this](int64_t frame) {
        m_viewer->setPosition(frame, m_ctx.project->timeline()->data().fps);
    });
    connect(ctx.project, &Project::projectReset, this, [this] {
        const QString path = m_ctx.project->filePath();
        m_settingsPanel->setDefaultOutput(
            path.isEmpty() ? QString() : QFileInfo(path).absolutePath(), m_ctx.project->name());
        m_settingsPanel->setSettings(m_settingsPanel->settings()); // timeline size/fps labels
        schedulePreview();
    });

    m_previewDebounce.setSingleShot(true);
    m_previewDebounce.setInterval(kPreviewDebounceMs);
    connect(&m_previewDebounce, &QTimer::timeout, this, &DeliverPage::renderPreview);
    m_previewPoll.setInterval(kPreviewPollMs);
    connect(&m_previewPoll, &QTimer::timeout, this, &DeliverPage::pollPreview);
    followMarks();
    updateRange();

    // The timeline cannot be edited here: its commands are off while the page is shown
    // (spec 42b.26). The marks stay, they set the render range ("In/Out Range").
    new TimelineCommandSuspension(this, ctx.actions,
        {QStringLiteral("mark.in"), QStringLiteral("mark.out"), QStringLiteral("mark.clip"),
            QStringLiteral("mark.clear_in"), QStringLiteral("mark.clear_out"),
            QStringLiteral("mark.clear_both"), QStringLiteral("mark.go_to_in"),
            QStringLiteral("mark.go_to_out")});
}

DeliverPage::~DeliverPage()
{
    // The preview request must finish before its compositor goes away (futures complete on
    // the render thread; the Compositor destructor drains the queue anyway).
    m_previewCompositor.reset();
}

std::optional<DeliverPage::PreparedJob> DeliverPage::prepareJob(
    const QString& title, bool confirmOverwrite)
{
    const ExportSettings s = m_settingsPanel->settings();
    QString error;
    if (!s.validate(&error)) {
        QMessageBox::warning(this, title, error);
        return std::nullopt;
    }
    const auto timeline = m_ctx.project->timeline()->snapshot();
    const auto [first, end] = s.frameRange(*timeline);
    if (end <= first) {
        QMessageBox::warning(this, title, tr("Nothing to render: the timeline is empty."));
        return std::nullopt;
    }
    const bool sequence = s.kind() == OutputKind::ImageSequence;
    const QString existing
        = sequence ? QString::asprintf(qPrintable(sequencePattern(s.outputPath)), 0) : s.outputPath;
    if (confirmOverwrite && QFileInfo::exists(existing)
        && QMessageBox::question(this, title,
               tr("%1 already exists. Overwrite it when the job renders?")
                   .arg(QFileInfo(existing).fileName()))
            != QMessageBox::Yes) {
        return std::nullopt;
    }
    PreparedJob job;
    job.name = QStringLiteral("%1 - %2").arg(
        timeline->name.isEmpty() ? m_ctx.project->name() : timeline->name,
        QFileInfo(s.outputPath).fileName());
    // Export renders the camera originals unless Advanced Settings asks for proxies /
    // optimized media.
    if (m_settingsPanel->useProxyMedia() || m_settingsPanel->useOptimizedMedia()) {
        job.proxies
            = makeProxyMap(m_ctx.project->mediaPool()->state(), *m_ctx.project->mediaLinks(),
                m_settingsPanel->useProxyMedia() ? ProxyHandling::PreferProxies
                                                 : ProxyHandling::Disabled,
                m_settingsPanel->useOptimizedMedia());
    }
    job.settings = s;
    job.timeline = timeline;
    job.media = makeMediaPathMap(m_ctx.project->mediaPool()->state());
    job.useProxies = m_settingsPanel->useProxyMedia();
    job.useOptimized = m_settingsPanel->useOptimizedMedia();
    return job;
}

quint64 DeliverPage::addCurrentToQueue(bool confirmOverwrite)
{
    if (m_settingsPanel->settings().individualClips)
        return addIndividualClipsToQueue(confirmOverwrite);
    std::optional<PreparedJob> job = prepareJob(tr("Add to Render Queue"), confirmOverwrite);
    if (!job)
        return 0;
    const quint64 id = m_queue->addJob(
        job->name, job->settings, job->timeline, job->media, std::move(job->proxies));
    m_jobMediaChoice.insert(id, {job->useProxies, job->useOptimized});
    return id;
}

quint64 DeliverPage::addIndividualClipsToQueue(bool confirmOverwrite)
{
    const QString title = tr("Add to Render Queue");
    std::optional<PreparedJob> job = prepareJob(title, false);
    if (!job)
        return 0;
    const QList<ClipRenderJob> clips
        = individualClipJobs(*job->timeline, job->settings, *job->media);
    if (clips.isEmpty()) {
        QMessageBox::warning(this, title, tr("There are no clips in the render range."));
        return 0;
    }
    if (confirmOverwrite) {
        QStringList existing;
        for (const ClipRenderJob& c : clips) {
            const QString path = c.settings.kind() == OutputKind::ImageSequence
                ? QString::asprintf(qPrintable(sequencePattern(c.settings.outputPath)), 0)
                : c.settings.outputPath;
            if (QFileInfo::exists(path))
                existing << QFileInfo(path).fileName();
        }
        if (!existing.isEmpty()
            && QMessageBox::question(this, title,
                   tr("%n file(s) already exist (%1). Overwrite them when the jobs render?",
                       nullptr, int(existing.size()))
                       .arg(existing.first()))
                != QMessageBox::Yes) {
            return 0;
        }
    }
    // One job per clip: the queue shows the progress of every clip.
    quint64 first = 0;
    for (const ClipRenderJob& c : clips) {
        const quint64 id
            = m_queue->addJob(c.name, c.settings, job->timeline, job->media, job->proxies);
        m_jobMediaChoice.insert(id, {job->useProxies, job->useOptimized});
        if (first == 0)
            first = id;
    }
    return first;
}

void DeliverPage::editJob(quint64 id)
{
    const RenderQueue::Job* job = m_queue->job(id);
    if (!job || job->status == RenderQueue::Status::Rendering)
        return;
    if (m_editingJob == 0)
        m_settingsBeforeEdit = m_settingsPanel->settings();
    m_editingJob = id;
    m_settingsPanel->setSettings(job->settings);
    const auto choice = m_jobMediaChoice.value(id);
    m_settingsPanel->setUseProxyMedia(choice.first);
    m_settingsPanel->setUseOptimizedMedia(choice.second);
    m_settingsPanel->setEditingJob(true);
    m_queuePanel->setEditingJob(id);
    updateRange();
}

bool DeliverPage::updateEditedJob(bool confirmOverwrite)
{
    if (m_editingJob == 0)
        return false;
    const RenderQueue::Job* current = m_queue->job(m_editingJob);
    if (!current || current->status == RenderQueue::Status::Rendering) {
        QMessageBox::warning(this, tr("Update Job"),
            current ? tr("The job is rendering and cannot be changed.")
                    : tr("The job is no longer in the render queue."));
        if (!current)
            cancelEdit();
        return false;
    }
    std::optional<PreparedJob> job = prepareJob(tr("Update Job"), confirmOverwrite);
    if (!job)
        return false;
    // The job renders the timeline it was queued from: if another one is open now, it keeps
    // its own snapshot and media.
    if (current->timeline && current->timeline->uid != job->timeline->uid) {
        job->timeline = current->timeline;
        job->media = current->media;
        job->name = QStringLiteral("%1 - %2").arg(
            current->timeline->name.isEmpty() ? m_ctx.project->name() : current->timeline->name,
            QFileInfo(job->settings.outputPath).fileName());
    }
    const quint64 id = m_editingJob;
    if (!m_queue->updateJob(
            id, job->name, job->settings, job->timeline, job->media, std::move(job->proxies)))
        return false;
    m_jobMediaChoice.insert(id, {job->useProxies, job->useOptimized});
    leaveEditMode(false);
    return true;
}

void DeliverPage::cancelEdit()
{
    if (m_editingJob != 0)
        leaveEditMode(true);
}

void DeliverPage::leaveEditMode(bool restoreSettings)
{
    m_editingJob = 0;
    m_settingsPanel->setEditingJob(false);
    m_queuePanel->setEditingJob(0);
    if (restoreSettings)
        m_settingsPanel->setSettings(m_settingsBeforeEdit);
    updateRange();
}

void DeliverPage::followMarks()
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const QPair<qint64, qint64> marks {data.markIn, data.markOut};
    if (marks == m_seenMarks)
        return;
    m_seenMarks = marks;
    // One mark is a range too (as in Resolve): Out alone runs from the start, In alone to the
    // end (ExportSettings::frameRange keeps the edge of an unset mark). Spec 45.4.
    const bool hasRange = (data.markIn >= 0 || data.markOut >= 0)
        && (data.markIn < 0 || data.markOut < 0 || data.markOut > data.markIn);
    ExportSettings s = m_settingsPanel->settings();
    const RangeMode wanted = hasRange ? RangeMode::InOut : RangeMode::Entire;
    if (s.range == wanted)
        return;
    s.range = wanted;
    m_settingsPanel->setSettings(s);
    updateRange();
}

void DeliverPage::updateRange()
{
    const ExportSettings s = m_settingsPanel->settings();
    const auto [first, end] = s.frameRange(m_ctx.project->timeline()->data());
    m_rangeBar->setRange(first, end);
}

void DeliverPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    if (!m_layoutRestored && !m_proportionsApplied)
        applyDefaultWidths();
    m_viewer->setPosition(
        m_ctx.project->timeline()->playhead(), m_ctx.project->timeline()->data().fps);
    schedulePreview();
    // Detection opens GPU encoders (NVENC init ~0.5 s each); start it after the first preview
    // so both do not compete for the GPU driver when the page opens.
    QTimer::singleShot(kHwDetectionDelayMs, this, &DeliverPage::detectHardware);
}

void DeliverPage::applyDefaultWidths()
{
    const int width = m_topRow->width();
    if (width <= 0)
        return;
    // QSplitter alone gives the whole size difference to the viewer; keep Resolve's
    // proportions instead (render settings and queue about a quarter each).
    m_proportionsApplied = true;
    const int total = kSettingsWidth + kViewerWidth + kQueueWidth;
    const int settings = std::max(m_settingsPanel->minimumSizeHint().width(),
        int(qint64(kSettingsWidth) * width / total));
    const int queue = int(qint64(kQueueWidth) * width / total);
    m_topRow->setSizes({settings, width - settings - queue, queue});
}

void DeliverPage::resetLayout()
{
    Page::resetLayout();
    m_layoutRestored = false;
    m_proportionsApplied = false;
    if (isVisible())
        applyDefaultWidths();
}

void DeliverPage::detectHardware()
{
    if (m_hwDetectionStarted) {
        return;
    }
    m_hwDetectionStarted = true;
    // Opening GPU encoders takes up to a second or two: off the GUI thread.
    auto* watcher = new QFutureWatcher<QList<HwEncoderStatus>>(this);
    connect(watcher, &QFutureWatcher<QList<HwEncoderStatus>>::finished, this, [this, watcher] {
        m_settingsPanel->setHardwareStatus(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([] { return EncoderCatalog::hardwareEncoders(); }));
}

void DeliverPage::schedulePreview()
{
    if (m_ctx.playback || !isVisible()) {
        return; // a playback engine feeds the viewer, or nobody looks at it
    }
    m_previewDebounce.start();
}

void DeliverPage::renderPreview()
{
    if (m_previewPending) {
        m_previewDirty = true; // re-render once the current request is back
        return;
    }
    const auto timeline = m_ctx.project->timeline()->snapshot();
    if (timeline->duration() == 0) {
        m_viewer->clearFrame();
        return;
    }
    if (!m_previewCompositor) {
        m_previewCompositor = std::make_unique<Compositor>();
    }
    // Render at the viewer's size (keeping the timeline aspect), not at full resolution.
    QSize size(timeline->width, timeline->height);
    const QSize area = m_viewer->pictureSize() * m_viewer->devicePixelRatioF();
    if (area.width() > 16 && area.height() > 16) {
        size = size.scaled(area, Qt::KeepAspectRatio).boundedTo(size);
    }
    RenderRequest request {timeline, makeMediaPathMap(m_ctx.project->mediaPool()->state()),
        m_ctx.project->timeline()->playhead(), size, Quality::Full, OutputImage};
    m_previewFuture = m_previewCompositor->renderAsync(std::move(request));
    m_previewPending = true;
    m_previewDirty = false;
    m_previewPoll.start();
}

void DeliverPage::pollPreview()
{
    if (!m_previewPending
        || m_previewFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    m_previewPoll.stop();
    m_previewPending = false;
    const RenderedFramePtr frame = m_previewFuture.get();
    if (frame && frame->ok && !frame->image.isNull()) {
        m_viewer->showFrame(frame->image);
        Q_EMIT previewShown(frame->frame);
    }
    if (m_previewDirty) {
        renderPreview();
    }
}

void DeliverPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("topRow"), m_topRow->saveState());
    settings.setValue(QStringLiteral("main"), m_main->saveState());
}

void DeliverPage::restoreLayout(const QSettings& settings)
{
    m_layoutRestored = settings.contains(QStringLiteral("topRow"));
    m_topRow->restoreState(settings.value(QStringLiteral("topRow")).toByteArray());
    m_main->restoreState(settings.value(QStringLiteral("main")).toByteArray());
}

} // namespace kk
