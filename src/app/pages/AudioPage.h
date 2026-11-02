// SPDX-License-Identifier: GPL-3.0-or-later
// AudioPage — mixing on its own page: the monitoring meters, a small (floatable) viewer, the
// timeline with tall audio tracks and the large mixer.
#pragma once

#include "ui/common/Page.h"

class QSplitter;

namespace kk {

struct AppContext;
class LoudnessPanel;
class MeterModel;
class MixerController;
class MixerPanel;
class TimelineController;
class TimelineWidget;
class ViewerPanel;

/// Audio page layout (Resolve's audio page, own name and look):
/// ```
/// | Viewer (small, can float) | Meters: output, buses, loudness (LUFS), true peak     |
/// | Timeline: audio tracks tall with waveforms, video collapsed  | Mixer (strips)    |
/// ```
/// It edits the same Project, active timeline, playhead, selection and undo stack as the
/// Edit and Cut pages; the timeline is a TimelineWidget driven by the Edit page's
/// TimelineController (same tool, snapping and targets) with the Audio page view options
/// (audio rows twice as tall, video rows collapsed, headers with record arm, channel format
/// and a level meter). Meters come from the program PlaybackEngine through a MeterModel.
/// The audio.* actions that only make sense here are enabled while the page is shown.
///
/// Thread-safety: GUI thread only.
class AudioPage : public Page
{
    Q_OBJECT
public:
    /// Builds the page; @p controller is the Edit page's timeline controller (not owned).
    AudioPage(AppContext& ctx, TimelineController* controller, QWidget* parent = nullptr);
    ~AudioPage() override;

    [[nodiscard]] QString pageId() const override { return QStringLiteral("audio"); }
    [[nodiscard]] bool hasPanel(PanelId id) const override { return id == PanelId::Mixer; }
    void setPanelVisible(PanelId id, bool visible) override;
    [[nodiscard]] bool isPanelVisible(PanelId id) const override;
    void saveLayout(QSettings& settings) const override;
    void restoreLayout(const QSettings& settings) override;

    [[nodiscard]] TimelineWidget* timeline() const { return m_timeline; }
    [[nodiscard]] MixerPanel* mixer() const { return m_mixer; }
    [[nodiscard]] LoudnessPanel* meters() const { return m_meters; }
    [[nodiscard]] ViewerPanel* viewer() const { return m_viewer; }
    [[nodiscard]] MixerController* mixerController() const { return m_mixerController; }
    [[nodiscard]] MeterModel* meterModel() const { return m_meterModel; }

    /// Floats the viewer in its own small window (or docks it back).
    void setViewerFloating(bool floating);
    [[nodiscard]] bool isViewerFloating() const;

    /// Height factor of the audio rows of the page's timeline.
    static constexpr double kAudioRowScale = 2.0;
    /// Height factor of the video rows (collapsed to the minimum row height).
    static constexpr double kVideoRowScale = 0.1;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void connectActions();
    void setPageActionsEnabled(bool enabled);

    AppContext& m_ctx;
    MixerController* m_mixerController = nullptr;
    MeterModel* m_meterModel = nullptr;
    ViewerPanel* m_viewer = nullptr;
    QWidget* m_viewerDock = nullptr; ///< the viewer's place in the layout
    QWidget* m_viewerWindow = nullptr; ///< floating viewer window (created on demand)
    LoudnessPanel* m_meters = nullptr;
    TimelineWidget* m_timeline = nullptr;
    MixerPanel* m_mixer = nullptr;
    QSplitter* m_main = nullptr; ///< vertical: top row | bottom row
    QSplitter* m_topRow = nullptr; ///< viewer | meters
    QSplitter* m_bottomRow = nullptr; ///< timeline | mixer
};

} // namespace kk
