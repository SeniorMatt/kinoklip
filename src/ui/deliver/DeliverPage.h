// SPDX-License-Identifier: GPL-3.0-or-later
// DeliverPage — the render page: Render Settings (left) | timeline viewer (centre) | Render Queue
// (right), over a mini timeline showing the render range.
#pragma once

#include "render/RenderTypes.h"
#include "ui/common/Page.h"

#include <QTimer>

#include <future>
#include <memory>

class QSplitter;

namespace kk {

struct AppContext;
class Compositor;
class ExportPresets;
class RenderQueue;
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
    /// The settings panel.
    [[nodiscard]] RenderSettingsPanel* settingsPanel() const { return m_settingsPanel; }

    /// Queues a job with the current settings (what "Add to Render Queue" does). Asks before
    /// overwriting an existing file unless @p confirmOverwrite is false. @return the job id,
    /// 0 if nothing was queued.
    quint64 addCurrentToQueue(bool confirmOverwrite = true);

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
    void detectHardware();

    AppContext& m_ctx;
    std::unique_ptr<ExportPresets> m_presets;
    RenderQueue* m_queue = nullptr;
    RenderSettingsPanel* m_settingsPanel = nullptr;
    RenderQueuePanel* m_queuePanel = nullptr;
    RenderRangeBar* m_rangeBar = nullptr;
    ViewerPanel* m_viewer = nullptr;
    QSplitter* m_topRow = nullptr;
    QSplitter* m_main = nullptr;

    std::unique_ptr<Compositor> m_previewCompositor;
    std::future<RenderedFramePtr> m_previewFuture;
    bool m_previewPending = false; ///< a request is in flight
    bool m_previewDirty = false; ///< something changed while it was in flight
    QTimer m_previewDebounce;
    QTimer m_previewPoll;
    bool m_hwDetectionStarted = false;
};

} // namespace kk
