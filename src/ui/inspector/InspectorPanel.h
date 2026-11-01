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
/// ParamGroupBox whose rows come from a descriptor: Transform / Cropping / Audio groups
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

    /// Toggles a keyframe on the parameter the user touched last (Add Keyframe shortcut).
    void toggleKeyframeOnActiveParam();
    /// Moves the playhead to the previous / next keyframe of any parameter of the shown clips.
    void goToKeyframe(bool forward);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
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
        QString transitionIn; ///< type, empty = none
        QString transitionOut;

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

    // ---- Building ----
    [[nodiscard]] Structure computeStructure(quint64 clipId) const;
    void onTimelineChanged();
    void rebuild();
    void clearTabs();
    QVBoxLayout* tabLayout(Tab tab) const;
    void addPlaceholder(Tab tab, const QString& text);
    ParamGroupBox* addGroup(Tab tab, const QString& key, const QString& title, quint64 clipId);
    void addDescriptorGroup(
        Tab tab, const EffectDescriptor& desc, ParamScope scope, quint64 clipId);
    void buildEffectsTab(const ClipData& clip);
    void buildTransitionTab(const ClipData& clip);
    void addTransitionGroup(const ClipData& clip, bool atHead);
    void moveEffect(quint64 effectId, int delta);
    void removeEffect(quint64 effectId);

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
    QVector<ParamGroupBox*> m_groups;
    QVector<quint64> m_groupClip; ///< clip id bound to m_groups[i]
    QHash<QString, bool> m_collapsed; ///< group key → collapsed, survives rebuilds
    int m_rebuildCount = 0;
    int m_openEdits = 0; ///< beginParamEdit() calls not yet closed

    std::optional<ParamRef> m_activeRef;
    ParamDescriptor m_activeDesc;
    std::function<QColor(const QColor&, const QString&)> m_colorPicker;
};

} // namespace kk
