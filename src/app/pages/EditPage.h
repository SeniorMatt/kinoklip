// SPDX-License-Identifier: GPL-3.0-or-later
// EditPage — the main editing page, laid out like PLAN §4 with nested splitters.
#pragma once

#include "ui/common/Page.h"

#include <QMap>

class QSplitter;

namespace kk {

struct AppContext;
class EffectsLibraryPanel;
class InspectorPanel;
class MediaPoolPanel;
class MetadataPanel;
class PanelFrame;
class TimelinePanel;
class ViewerPanel;

/// Edit page layout:
/// ```
/// | Media Pool / Effects / Edit Index | Source | Timeline viewer | Inspector / Metadata |
/// |                 Timeline                                      |       Mixer          |
/// ```
/// Toggleable panels are shown/hidden from the interface toolbar; a column disappears when all
/// its panels are hidden. "Single viewer" mode hides the source viewer.
class EditPage : public Page
{
    Q_OBJECT
public:
    /// Builds the page and all its panels.
    EditPage(AppContext& ctx, QWidget* parent = nullptr);

    [[nodiscard]] QString pageId() const override { return QStringLiteral("edit"); }
    [[nodiscard]] bool hasPanel(PanelId id) const override { return m_panels.contains(id); }
    void setPanelVisible(PanelId id, bool visible) override;
    [[nodiscard]] bool isPanelVisible(PanelId id) const override;
    void saveLayout(QSettings& settings) const override;
    void restoreLayout(const QSettings& settings) override;

    /// Hides the source viewer so the timeline viewer gets the whole width.
    void setSingleViewer(bool single);
    /// True in single-viewer mode.
    [[nodiscard]] bool isSingleViewer() const;

    // Panel accessors for modules that wire behaviour (playback, timeline, panels agents).
    [[nodiscard]] MediaPoolPanel* mediaPool() const { return m_mediaPool; }
    [[nodiscard]] EffectsLibraryPanel* effectsLibrary() const { return m_effects; }
    [[nodiscard]] ViewerPanel* sourceViewer() const { return m_sourceViewer; }
    [[nodiscard]] ViewerPanel* timelineViewer() const { return m_timelineViewer; }
    [[nodiscard]] InspectorPanel* inspector() const { return m_inspector; }
    [[nodiscard]] MetadataPanel* metadata() const { return m_metadata; }
    [[nodiscard]] TimelinePanel* timeline() const { return m_timeline; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    void updateColumns();
    /// Resolve's default proportions for the page's actual size (first show without a
    /// stored layout): viewers about half the height, side columns about a fifth of the width.
    void applyDefaultProportions();

    AppContext& m_ctx;
    MediaPoolPanel* m_mediaPool = nullptr;
    EffectsLibraryPanel* m_effects = nullptr;
    PanelFrame* m_editIndex = nullptr;
    ViewerPanel* m_sourceViewer = nullptr;
    ViewerPanel* m_timelineViewer = nullptr;
    InspectorPanel* m_inspector = nullptr;
    MetadataPanel* m_metadata = nullptr;
    TimelinePanel* m_timeline = nullptr;
    PanelFrame* m_mixer = nullptr;

    QSplitter* m_main = nullptr; ///< vertical: top row over bottom row
    QSplitter* m_topRow = nullptr; ///< left column | source | timeline viewer | right column
    QSplitter* m_leftColumn = nullptr; ///< media pool / effects / edit index
    QSplitter* m_rightColumn = nullptr; ///< inspector / metadata
    QSplitter* m_bottomRow = nullptr; ///< timeline | mixer
    QMap<PanelId, QWidget*> m_panels;
    bool m_layoutRestored = false; ///< restoreLayout() found stored splitter states
    bool m_proportionsApplied = false;
};

} // namespace kk
