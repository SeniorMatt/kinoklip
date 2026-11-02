// SPDX-License-Identifier: GPL-3.0-or-later
// DeliverPage — page layout, queueing, the playhead preview and hardware detection.
#include "ui/deliver/DeliverPage.h"

#include "core/Project.h"
#include "export/EncoderCatalog.h"
#include "export/ExportPresets.h"
#include "export/RenderQueue.h"
#include "render/Compositor.h"
#include "render/ProxySelection.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelSplitter.h"
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

    m_topRow = new PanelSplitter(Qt::Horizontal);
    m_topRow->setChildrenCollapsible(false);
    m_topRow->addWidget(m_settingsPanel);
    m_topRow->addWidget(m_viewer);
    m_topRow->addWidget(m_queuePanel);
    m_topRow->setStretchFactor(1, 1);
    m_topRow->setSizes({kSettingsWidth, kViewerWidth, kQueueWidth});

    m_rangeBar = new RenderRangeBar(ctx.project->timeline());
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
    connect(
        m_settingsPanel, &RenderSettingsPanel::settingsChanged, this, &DeliverPage::updateRange);

    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &DeliverPage::schedulePreview);
    connect(timeline, &Timeline::changed, this, &DeliverPage::updateRange);
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
    updateRange();
}

DeliverPage::~DeliverPage()
{
    // The preview request must finish before its compositor goes away (futures complete on
    // the render thread; the Compositor destructor drains the queue anyway).
    m_previewCompositor.reset();
}

quint64 DeliverPage::addCurrentToQueue(bool confirmOverwrite)
{
    const ExportSettings s = m_settingsPanel->settings();
    QString error;
    if (!s.validate(&error)) {
        QMessageBox::warning(this, tr("Add to Render Queue"), error);
        return 0;
    }
    const auto timeline = m_ctx.project->timeline()->snapshot();
    const auto [first, end] = s.frameRange(*timeline);
    if (end <= first) {
        QMessageBox::warning(
            this, tr("Add to Render Queue"), tr("Nothing to render: the timeline is empty."));
        return 0;
    }
    const bool sequence = s.kind() == OutputKind::ImageSequence;
    const QString existing
        = sequence ? QString::asprintf(qPrintable(sequencePattern(s.outputPath)), 0) : s.outputPath;
    if (confirmOverwrite && QFileInfo::exists(existing)
        && QMessageBox::question(this, tr("Add to Render Queue"),
               tr("%1 already exists. Overwrite it when the job renders?")
                   .arg(QFileInfo(existing).fileName()))
            != QMessageBox::Yes) {
        return 0;
    }
    const QString name = QStringLiteral("%1 - %2").arg(
        timeline->name.isEmpty() ? m_ctx.project->name() : timeline->name,
        QFileInfo(s.outputPath).fileName());
    // Export renders the camera originals unless Advanced Settings asks for proxies /
    // optimized media.
    std::shared_ptr<const ProxyMap> proxies;
    if (m_settingsPanel->useProxyMedia() || m_settingsPanel->useOptimizedMedia()) {
        proxies = makeProxyMap(m_ctx.project->mediaPool()->state(), *m_ctx.project->mediaLinks(),
            m_settingsPanel->useProxyMedia() ? ProxyHandling::PreferProxies
                                             : ProxyHandling::Disabled,
            m_settingsPanel->useOptimizedMedia());
    }
    return m_queue->addJob(name, s, timeline, makeMediaPathMap(m_ctx.project->mediaPool()->state()),
        std::move(proxies));
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
    if (!m_layoutRestored && !m_proportionsApplied && m_topRow->width() > 0) {
        // QSplitter alone gives the whole size difference to the viewer; keep Resolve's
        // proportions instead (render settings and queue about a quarter each).
        m_proportionsApplied = true;
        const int width = m_topRow->width();
        const int total = kSettingsWidth + kViewerWidth + kQueueWidth;
        const int settings = std::max(m_settingsPanel->minimumSizeHint().width(),
            int(qint64(kSettingsWidth) * width / total));
        const int queue = int(qint64(kQueueWidth) * width / total);
        m_topRow->setSizes({settings, width - settings - queue, queue});
    }
    m_viewer->setPosition(
        m_ctx.project->timeline()->playhead(), m_ctx.project->timeline()->data().fps);
    schedulePreview();
    // Detection opens GPU encoders (NVENC init ~0.5 s each); start it after the first preview
    // so both do not compete for the GPU driver when the page opens.
    QTimer::singleShot(kHwDetectionDelayMs, this, &DeliverPage::detectHardware);
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
