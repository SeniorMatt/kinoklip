// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorPanel — edits the parameters of the selected clip (Video / Audio / Effects / Title /
// Transition tabs), with editors built generically from EffectRegistry descriptors.
#pragma once

#include "ui/common/PanelFrame.h"
#include "ui/inspector/ParamEditHost.h"

#include <QHash>
#include <QPointer>
#include <QVector>

#include <functional>
#include <optional>

class QLabel;
class QScrollArea;
class QTabWidget;
class QToolButton;
class QVBoxLayout;

namespace kk {

class ParamGroupBox;
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

    /// Toggles a keyframe on the parameter the user touched last (Add Keyframe shortcut).
    void toggleKeyframeOnActiveParam();
    /// Moves the playhead to the previous / next keyframe of any parameter of the shown clips.
    void goToKeyframe(bool forward);

protected:
    void changeEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    /// What makes a rebuild necessary (see the class comment).
    struct Structure
    {
        quint64 clipId = 0;
        quint64 audioClipId = 0;
        ClipKind kind = ClipKind::Media;
        bool onVideoTrack = true;
        QVector<quint64> effectIds; ///< in order
        QVector<quint64> maskIds; ///< in order (a mask's shape type never changes)
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
    void addSectionKeyframeButton(
        ParamGroupBox* group, const EffectDescriptor& desc, ParamScope scope, quint64 clipId);
    void addDescriptorGroup(
        Tab tab, const EffectDescriptor& desc, ParamScope scope, quint64 clipId);
    /// Subtitle clips: the Captions tab (caption text, own style, track style).
    void buildCaptionsTab(const ClipData& c);
    void buildEffectsTab(const ClipData& clip);
    void buildTransitionTab(const ClipData& clip);
    void addTransitionGroup(const ClipData& clip, bool atHead);
    void moveEffect(quint64 effectId, int delta);
    void removeEffect(quint64 effectId);
    // ---- Masks section (InspectorMasks.cpp) ----
    void buildMasksSection(const ClipData& clip);
    void addMask(MaskShape shape);
    void moveMask(quint64 maskId, int delta);
    void removeMask(quint64 maskId);

    // ---- Refreshing ----
    void refreshValues();
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
    Structure m_structure;
    /// Stand-in clip for the style of a subtitle track (virtual clip id, see
    /// trackStyleClipId()): its `title` mirrors TrackData::subtitleStyle, so the generic
    /// parameter rows edit the track style. Refreshed by clip().
    mutable ClipData m_trackStyleProxy;
    QVector<ParamGroupBox*> m_groups;
    QVector<quint64> m_groupClip; ///< clip id bound to m_groups[i]
    QHash<QString, bool> m_collapsed; ///< group key → collapsed, survives rebuilds
    int m_rebuildCount = 0;
    int m_openEdits = 0; ///< beginParamEdit() calls not yet closed

    std::optional<ParamRef> m_activeRef;
    ParamDescriptor m_activeDesc;
    std::function<QColor(const QColor&, const QString&)> m_colorPicker;
    std::function<QString(const QString&, const QString&, const QString&)> m_filePicker;
    bool m_updatingStyleSheet = false; ///< re-entrancy guard of changeEvent()
};

} // namespace kk
