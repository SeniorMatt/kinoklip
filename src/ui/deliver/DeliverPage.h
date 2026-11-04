// SPDX-License-Identifier: GPL-3.0-or-later
// DeliverPage — the render page: Render Settings (left) | timeline viewer (centre) | Render Queue
// (right), over a mini timeline showing the render range.
#pragma once

#include "export/ExportJob.h"
#include "render/RenderTypes.h"
#include "ui/common/Page.h"

#include <QHash>
#include <QPair>
#include <QTimer>

#include <future>
#include <memory>
#include <optional>

class QSplitter;

namespace kk {

struct AppContext;
class Compositor;
class ExportPresets;
class RenderQueue;
class RenderFinishNotifier;
class RenderQueuePanel;
class RenderRangeBar;
class RenderSettingsPanel;
class ViewerPanel;

/// The Deliver page (PLAN §4, §5.6).
///
/// Owns the export presets, the RenderQueue (jobs keep rendering in the background while the
/// user works on other pages) and — while no PlaybackEngine drives the viewers — a preview
/// Compositor that renders the frame under the playhead into the viewer (debounced, one
/// request in flight, at the viewer's size).
class DeliverPage : public Page
{
    Q_OBJECT
public:
    /// Builds the page.
    DeliverPage(AppContext& ctx, QWidget* parent = nullptr);
    ~DeliverPage() override;

    [[nodiscard]] QString pageId() const override { return QStringLiteral("deliver"); }
    void saveLayout(QSettings& settings) const override;
    void restoreLayout(const QSettings& settings) override;

    /// The page's program viewer.
    [[nodiscard]] ViewerPanel* viewer() const { return m_viewer; }
    /// The render queue (owned by the page).
    [[nodiscard]] RenderQueue* renderQueue() const { return m_queue; }
    /// The "render finished" desktop notification of the queue (spec 45.5).
    [[nodiscard]] RenderFinishNotifier* finishNotifier() const { return m_finishNotifier; }
    /// The settings panel.
    [[nodiscard]] RenderSettingsPanel* settingsPanel() const { return m_settingsPanel; }

    /// Queues a job with the current settings (what "Add to Render Queue" does). Asks before
    /// overwriting an existing file unless @p confirmOverwrite is false. @return the job id,
    /// 0 if nothing was queued.
    quint64 addCurrentToQueue(bool confirmOverwrite = true);
    /// Loads job @p id's settings into the render settings for editing ("Update Job" mode;
    /// the pencil on a job card). Not while the job renders.
    void editJob(quint64 id);
    /// The job being edited (0: none).
    [[nodiscard]] quint64 editedJob() const { return m_editingJob; }
    /// "Update Job": writes the current settings into the edited job (queued again if it had
    /// finished) and leaves the mode. Asks before overwriting unless @p confirmOverwrite is
    /// false. @return true if the job changed.
    bool updateEditedJob(bool confirmOverwrite = true);
    /// Leaves the edit mode without changing the job; the settings shown before come back.
    void cancelEdit();

Q_SIGNALS:
    /// The preview of timeline frame @p frame reached the viewer (tests, status).
    void previewShown(int64_t frame);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void schedulePreview();
    void renderPreview();
    void pollPreview();
    void updateRange();
    /// Render range follows the timeline's In / Out marks as a Resolve user expects: when
    /// both marks are set (or move) the range becomes In/Out Range, when they are cleared it
    /// goes back to Entire Timeline. A manual choice stays until the marks change again.
    void followMarks();
    void detectHardware();

    /// A job built from the current settings, not queued yet.
    struct PreparedJob
    {
        QString name;
        ExportSettings settings;
        std::shared_ptr<const TimelineData> timeline;
        std::shared_ptr<const MediaPathMap> media;
        std::shared_ptr<const ProxyMap> proxies;
        bool useProxies = false;
        bool useOptimized = false;
    };
    /// Validates the settings (messages titled @p title) and snapshots the timeline.
    std::optional<PreparedJob> prepareJob(const QString& title, bool confirmOverwrite);
    void leaveEditMode(bool restoreSettings);

    AppContext& m_ctx;
    std::unique_ptr<ExportPresets> m_presets;
    RenderQueue* m_queue = nullptr;
    RenderSettingsPanel* m_settingsPanel = nullptr;
    RenderQueuePanel* m_queuePanel = nullptr;
    RenderFinishNotifier* m_finishNotifier = nullptr;
    RenderRangeBar* m_rangeBar = nullptr;
    QPair<qint64, qint64> m_seenMarks {-2, -2}; ///< marks followMarks() last reacted to
    ViewerPanel* m_viewer = nullptr;
    QSplitter* m_topRow = nullptr;
    QSplitter* m_main = nullptr;

    std::unique_ptr<Compositor> m_previewCompositor;
    std::future<RenderedFramePtr> m_previewFuture;
    bool m_previewPending = false; ///< a request is in flight
    bool m_previewDirty = false; ///< something changed while it was in flight
    QTimer m_previewDebounce;
    /// Timeline format the Render Settings labels show ("Match timeline (W x H)", "N fps").
    QString m_shownFormat;
    QTimer m_previewPoll;
    bool m_hwDetectionStarted = false;
    quint64 m_editingJob = 0; ///< editJob(): the job whose settings are shown
    ExportSettings m_settingsBeforeEdit; ///< restored by cancelEdit()
    /// Advanced Settings "use proxy / optimized media" of each job, for editJob().
    QHash<quint64, QPair<bool, bool>> m_jobMediaChoice;
    bool m_layoutRestored = false; ///< restoreLayout() found a stored splitter state
    bool m_proportionsApplied = false;
};

} // namespace kk
