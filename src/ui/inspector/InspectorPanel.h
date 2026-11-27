// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorPanel — edits the parameters of the selected clip (Video / Audio / Effects / Title /
// Transition tabs), with editors built generically from EffectRegistry descriptors.
#pragma once

#include "compositing/tracking/MaskTrackJob.h"
#include "core/EffectRegistry.h"
#include "ui/common/PanelFrame.h"
#include "ui/inspector/ParamEditHost.h"

#include <QHash>
#include <QPointer>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

class QLabel;
class QScrollArea;
class QTabWidget;
class QToolButton;
class QVBoxLayout;

namespace kk {

class ParamGroupBox;
namespace tracking {
class StabilizeRunner;
class TrackerRunner;
enum class TrackOutcome;
} // namespace tracking
namespace retimeops {
struct SpeedChange;
}
struct AppContext;

/// Shows and edits the current clip (SelectionModel::currentClip()).
///
/// Layout: a header (clip name, "N clips selected") above five tabs. Every section is a
/// ParamGroupBox whose rows come from a descriptor: Transform / Cropping / Masks (one group per
/// mask, InspectorMasks.cpp) / Audio groups
/// (EffectCategory::ClipProperties), one group per effect instance, the title parameters and
/// the head/tail transitions. Adding a descriptor to the registry is enough for it to appear.
///
/// Update strategy (performance):
/// - The widgets are rebuilt only when the clip's *structure* changes (another clip, effects
///   added/removed/reordered, transitions added/removed/retyped).
/// - Playhead moves and value edits only call refresh() on the existing rows: O(params), no
///   widget creation, no allocation when the values did not change.
///
/// Undo: every user change is one undo step through Project::beginEdit()/endEdit(); a drag
/// on a value field or slider is one step from press to release.
///
/// Audio of a video clip: the Audio tab edits the first linked clip on an audio track.
///
/// Thread-safety: GUI thread only.
class InspectorPanel : public PanelFrame, private ParamEditHost
{
    Q_OBJECT
public:
    /// Creates the inspector; it follows ctx.selection, the playhead and timeline changes.
    InspectorPanel(AppContext& ctx, QWidget* parent = nullptr);
    ~InspectorPanel() override;

    /// Tab indices (in display order).
    enum Tab {
        VideoTab,
        AudioTab,
        EffectsTab,
        TitleTab,
        TransitionTab
    };

    /// Clip currently shown (0 = none).
    [[nodiscard]] quint64 clipId() const { return m_clipId; }
    /// Clip edited by the Audio tab (the clip itself or its linked audio clip; 0 = none).
    [[nodiscard]] quint64 audioClipId() const { return m_audioClipId; }
    /// Shows the clip with id @p clipId (0 = empty state).
    void showClip(quint64 clipId);
    /// Shows the effects of audio track @p trackId (the track's effect slots, spec 133) on the
    /// Effects tab: a selected mixer strip or track. A selected clip (showClip() with an id)
    /// takes the panel back; 0 = nothing. Same Add Effect, bypass, reorder, remove and
    /// parameter controls as a clip's audio effects.
    void showTrack(quint64 trackId);
    /// Track shown by showTrack() (0 = none).
    [[nodiscard]] quint64 trackId() const { return m_trackId; }
    /// showTrack() and open the controls of slot @p effectId (expanded and scrolled into view).
    void inspectTrackEffect(quint64 trackId, quint64 effectId);
    /// Shows @p clipId on tab @p tab (a selected transition, the fx badge on the timeline).
    void inspect(quint64 clipId, Tab tab);

    /// The tab widget (tests switch tabs and inspect the groups).
    [[nodiscard]] QTabWidget* tabs() const { return m_tabs; }
    /// Groups currently built, in display order across all tabs.
    [[nodiscard]] const QVector<ParamGroupBox*>& groups() const { return m_groups; }
    /// Number of times the editor widgets were rebuilt (tests check playhead moves don't).
    [[nodiscard]] int rebuildCount() const { return m_rebuildCount; }

    /// Replaces the colour dialog (tests inject a non-modal picker). Returns an invalid colour
    /// to cancel.
    void setColorPicker(std::function<QColor(const QColor&, const QString&)> picker)
    {
        m_colorPicker = std::move(picker);
    }

    /// Replaces the file dialog of File parameters (tests). Returns an empty path to cancel.
    void setFilePicker(
        std::function<QString(const QString&, const QString&, const QString&)> picker)
    {
        m_filePicker = std::move(picker);
    }

    /// Title clips: edits layer @p layerId in the Title tab (0 = the first layer).
    void selectTitleLayer(quint64 layerId);
    [[nodiscard]] quint64 selectedTitleLayer() const { return m_titleLayer; }
    /// Toggles a keyframe on the parameter the user touched last (Add Keyframe shortcut).
    void toggleKeyframeOnActiveParam();
    /// Moves the playhead to the previous / next keyframe of any parameter of the shown clips.
    void goToKeyframe(bool forward);

protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    /// What makes a rebuild necessary (see the class comment).
    struct Structure
    {
        quint64 clipId = 0;
        quint64 audioClipId = 0;
        quint64 trackId = 0; ///< track effects mode (clipId = 0)
        QVector<quint64> trackEffectIds; ///< that track's effect slots, in order
        ClipKind kind = ClipKind::Media;
        bool onVideoTrack = true;
        QVector<quint64> effectIds; ///< in order
        QVector<quint64> audioEffectIds; ///< the linked audio clip's effects, in order
        QVector<quint64> maskIds; ///< in order (a mask's shape type never changes)
        QVector<quint64> titleOrder; ///< Title clips: layer ids bottom to top (0 = first layer)
        QStringList titleLayerLabels; ///< Title clips: name and visibility of each layer
        quint64 titleLayer = 0; ///< Title clips: the layer the controls edit
        QString transitionIn; ///< type, empty = none
        QString transitionOut;
        quint64 subtitleTrackId = 0; ///< Subtitle clips: TrackData::id of their track
        bool ownStyle = false; ///< Subtitle clips: subtitles::OwnStyle

        friend bool operator==(const Structure&, const Structure&) = default;
    };

    // ---- ParamEditHost ----
    [[nodiscard]] const ClipData* clip(quint64 clipId) const override;
    [[nodiscard]] int64_t localFrame(quint64 clipId) const override;
    void beginParamEdit(const QString& label) override;
    void endParamEdit() override;
    void modifyClip(quint64 clipId, const std::function<void(ClipData&)>& mutate) override;
    void editClip(const QString& label, quint64 clipId,
        const std::function<void(ClipData&)>& mutate) override;
    void seekLocalFrame(quint64 clipId, int64_t localFrame) override;
    void setActiveParam(const ParamRef& ref, const ParamDescriptor& desc) override;
    [[nodiscard]] QColor pickColor(const QColor& initial, const QString& title) override;
    [[nodiscard]] QString pickFile(
        const QString& current, const QString& title, const QString& filter) override;
    void setColorPicking(const ParamRef& ref, bool on) override;
    [[nodiscard]] bool isColorPicking(const ParamRef& ref) const override;
    [[nodiscard]] bool isMixed(const ParamRef& ref, const ParamDescriptor& desc) const override;
    /// The selected clips an edit of @p clipId also changes (MultiClipEdit.h).
    [[nodiscard]] QVector<quint64> editPeers(quint64 clipId) const;

    // ---- Building ----
    [[nodiscard]] Structure computeStructure(quint64 clipId) const;
    void onTimelineChanged();
    void onSelectionChanged();
    /// True while the window is shown but this panel is not (its page is not the current
    /// one): the work waits for showEvent(), which brings the panel up to date once.
    [[nodiscard]] bool deferWhileHidden();
    /// Switches between the Video and Audio tab for the part of a pair that was clicked.
    void followClickedPart(quint64 clickedId);
    /// Selected clips, counting a linked group once.
    [[nodiscard]] int selectedItemCount() const;
    quint64 m_lastCurrent = 0; ///< SelectionModel::currentClip() at the last selection change
    void rebuild();
    void clearTabs();
    QVBoxLayout* tabLayout(Tab tab) const;
    void addPlaceholder(Tab tab, const QString& text);
    ParamGroupBox* addGroup(Tab tab, const QString& key, const QString& title, quint64 clipId);
    /// Adds the section keyframe diamond (keys every animatable parameter of the group).
    /// In-app help of a section showing EffectRegistry entry @p effectId: the help context
    /// for F1, a "?" header button and "How to use <name>…" in the header menu when the
    /// user guide has an article for it.
    void attachHelp(ParamGroupBox* group, const QString& effectId, const QString& name);
    void addSectionKeyframeButton(ParamGroupBox* group, const EffectDescriptor& desc,
        ParamScope scope, quint64 clipId, quint64 effectId = 0);
    void addDescriptorGroup(Tab tab, const EffectDescriptor& desc, ParamScope scope, quint64 clipId,
        quint64 effectId = 0);
    /// Subtitle clips: the Captions tab (caption text, own style, track style).
    void buildCaptionsTab(const ClipData& c);
    void buildEffectsTab(const ClipData& clip);
    /// One effect stack (toolbar with Add Effect, then one group per effect) of clip @p clipId;
    /// @p heading titles it when a linked pair shows two stacks.
    void buildEffectSection(
        const ClipData& clip, quint64 clipId, EffectCategory category, const QString& heading);
    void buildTransitionTab(const ClipData& clip);
    /// Type of the track holding clip @p clipId (Video if it is not found).
    [[nodiscard]] TrackType trackTypeOfClip(quint64 clipId) const;
    void addTransitionGroup(const ClipData& clip, bool atHead);
    void moveEffect(quint64 clipId, quint64 effectId, int delta);
    void removeEffect(quint64 clipId, quint64 effectId);
    /// Adds audio effect @p descriptorId to the end of track @p trackId's slots (one undo step).
    bool addTrackEffect(quint64 trackId, const QString& descriptorId);
    // ---- Dynamic Zoom, Speed Change, Retime and Scaling (InspectorSpeed.cpp) ----
    void buildDynamicZoomSection(quint64 clipId);
    void buildSpeedSections(const ClipData& clip);
    // ---- Tracking effects (InspectorTrack.cpp, spec 70) ----
    /// Adds the Track buttons, progress and status line under the parameters of a tracking
    /// effect (Match Move; Surface Tracker); other effects get nothing.
    void buildTrackingControls(ParamGroupBox* group, const EffectInstance& fx, quint64 clipId);
    /// The AI label, model status and buttons under Background Removal (spec 78); other
    /// effects get nothing (InspectorBackgroundRemoval.cpp).
    void buildBackgroundRemovalControls(
        ParamGroupBox* group, const EffectInstance& fx, quint64 clipId);
    /// The AI label, Learn / Clear Profile buttons and status under Noise Reduction (spec 132);
    /// other effects get nothing (InspectorNoiseReduction.cpp).
    void buildNoiseReductionControls(ParamGroupBox* group, const EffectInstance& fx, quint64 clipId);
    /// The Voice Isolation section of the Audio tab: a switch and an Amount (spec 141).
    void buildVoiceIsolationSection(quint64 audioClipId);
    void startEffectTrack(quint64 clipId, quint64 effectId, bool forward);
    /// "Follow Subject" of a mask: tracks forward from the playhead, then back, and keys the
    /// mask from the result (InspectorTrack.cpp, spec 187).
    void startMaskTrack(quint64 clipId, quint64 maskId);
    bool startMaskRun(bool forward);
    void finishMaskTrack(kk::tracking::TrackOutcome outcome, const QString& reason);
    void onEffectTrackFinished(kk::tracking::TrackOutcome outcome, const QString& reason);
    // ---- Stabilization (InspectorStabilize.cpp) ----
    void buildStabilizationSection(const ClipData& clip);
    void onStabilizeFinished(quint64 clipId);
    /// Changes the speed of @p clipId (and the selected peers) like the Change Clip Speed
    /// dialog: @p edit adjusts the clip's current speed state.
    void applySpeed(quint64 clipId, const std::function<void(retimeops::SpeedChange&)>& edit);
    // ---- Title layers (InspectorTitleLayers.cpp) ----
    void buildTitleLayersSection(const ClipData& clip);
    void addTitleLayer(TitleLayerKind kind, const QString& shape = {});
    void duplicateTitleLayer(quint64 layerId);
    void removeTitleLayer(quint64 layerId);
    void moveTitleLayer(quint64 layerId, int delta);
    // ---- Masks section (InspectorMasks.cpp) ----
    void buildMasksSection(const ClipData& clip);
    void addMask(MaskShape shape);
    void moveMask(quint64 maskId, int delta);
    void removeMask(quint64 maskId);

    // ---- Refreshing ----
    void refreshValues();
    void refreshTitleLayerNames(); ///< layer names follow their text without a rebuild
    void updateHeader();

    AppContext& m_ctx;
    QLabel* m_clipName = nullptr;
    QLabel* m_clipInfo = nullptr;
    QTabWidget* m_tabs = nullptr;
    QVector<QScrollArea*> m_tabScroll; ///< one per Tab
    QVector<QPointer<QWidget>> m_retiredPages; ///< old tab pages awaiting deleteLater()
    QToolButton* m_addEffect = nullptr;

    quint64 m_clipId = 0;
    quint64 m_audioClipId = 0;
    quint64 m_titleLayer = 0; ///< the title layer being edited (0 = the first layer)
    Structure m_structure;
    /// Stand-in clip for the style of a subtitle track (virtual clip id, see
    /// trackStyleClipId()): its `title` mirrors TrackData::subtitleStyle, so the generic
    /// parameter rows edit the track style. Refreshed by clip().
    mutable ClipData m_trackStyleProxy;
    /// Stand-in clip of the track shown by showTrack() (virtual clip id, see trackFxClipId()):
    /// its `effects` mirror TrackMix::effects, so the generic effect section edits the slots.
    mutable ClipData m_trackFxProxy;
    quint64 m_trackId = 0; ///< audio track shown in track mode (0 = none)
    QVector<ParamGroupBox*> m_groups;
    QVector<quint64> m_groupClip; ///< clip id bound to m_groups[i]
    QHash<QString, bool> m_collapsed; ///< group key → collapsed, survives rebuilds
    int m_rebuildCount = 0;
    bool m_stale = false; ///< a change arrived while the panel was hidden
    int m_openEdits = 0; ///< beginParamEdit() calls not yet closed

    std::optional<ParamRef> m_activeRef;
    ParamDescriptor m_activeDesc;
    std::function<QColor(const QColor&, const QString&)> m_colorPicker;
    std::function<QString(const QString&, const QString&, const QString&)> m_filePicker;
    bool m_updatingStyleSheet = false; ///< re-entrancy guard of changeEvent()
    /// Speed Change's Ripple Sequence (on by default, like Resolve; kept while the panel lives).
    bool m_speedRipple = true;
    /// The Stabilize button's worker (one run at a time; survives rebuilds of the sections).
    tracking::StabilizeRunner* m_stabilizer = nullptr;
    /// The Track buttons of the tracking effects: one run at a time.
    tracking::TrackerRunner* m_effectTracker = nullptr;
    std::shared_ptr<tracking::EffectTrackPlan> m_trackPlan;
    /// The mask being followed (the same runner): the plans of the runs, what they found.
    struct MaskTrackState
    {
        quint64 clipId = 0;
        quint64 maskId = 0;
        int64_t playhead = 0;
        bool forward = true; ///< the run that is going
        std::shared_ptr<tracking::MaskTrackPlan> plan; ///< the first plan (the table's frame)
        std::shared_ptr<tracking::MaskTrackPlan> current;
        std::vector<tracking::TrackSample> samples;
    };
    std::shared_ptr<MaskTrackState> m_maskTrack;
    QString m_maskTrackMessage; ///< last result line, shown in the mask m_maskTrackMessageMask
    quint64 m_maskTrackMessageMask = 0;
    QString m_trackMessage; ///< last result line, shown in the effect m_trackPlan names
    quint64 m_trackMessageEffect = 0;
    QString
        m_stabilizeMessage; ///< last result line, shown in the section of m_stabilizeMessageClip
    quint64 m_stabilizeMessageClip = 0;
};

} // namespace kk
