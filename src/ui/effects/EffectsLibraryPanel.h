// SPDX-License-Identifier: GPL-3.0-or-later
// EffectsLibraryPanel — browsable, searchable catalogue of transitions, titles, generators and
// effects; drag source for mime::Effect, double-click applies.
#pragma once

#include "ui/common/PanelFrame.h"

#include <functional>

class QAction;
class QLineEdit;
class QMenu;
class QTreeWidget;

namespace kk {

struct AppContext;

/// Lists the EffectRegistry catalogue in sections (Video Transitions, Titles, Generators,
/// Video Effects, Audio Effects), with a search field filtering by name and description.
///
/// - Dragging an entry starts a drag with mime::Effect (payload = descriptor id), dropped on
///   the timeline (timeline agent) or the inspector.
/// - Right-click offers "How to use <name>…" when the user guide has an article for the entry;
///   the selected entry is the help context (`effect:<id>`) for F1.
/// - Double-click (or Enter) runs effectactions::activate(): effects go onto the selected
///   clips, transitions onto the current clip, titles/generators are added at the playhead.
/// - Live Preview (options menu "…", on by default, Preferences key effects/livePreview):
///   while the pointer rests on an entry, livePreviewRequested() names it with the pointer's
///   horizontal place over the row (0..1, scrubbing); leaving the entry, the panel, starting
///   a drag, searching or hiding the panel emit livePreviewEnded(). The Edit page feeds them
///   to an EffectsLivePreview of its timeline viewer (spec 47).
///
/// Thread-safety: GUI thread only.
class EffectsLibraryPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates the panel and fills it from EffectRegistry::instance().
    EffectsLibraryPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Applies a search filter (case-insensitive substring of name or description).
    void setFilterText(const QString& text);
    /// Ids of the entries currently visible (tests).
    [[nodiscard]] QStringList visibleDescriptorIds() const;
    /// The tree (tests).
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }

    /// Shows only the section of EffectCategory @p category in the list (-1 = all); the
    /// sidebar calls it. A search text looks through every category.
    void setCategoryFilter(int category);

    /// The context menu of entry @p descriptorId ("How to use <name>…" when the user guide has
    /// an article for it), or nullptr when it would be empty. The caller owns it.
    [[nodiscard]] QMenu* createContextMenu(const QString& descriptorId);

    /// Double-clicked transitions go to @p handler first (the Edit page: the selected edit
    /// point, else the edit nearest the playhead, spec 13.18); it returns whether it applied
    /// the transition, else effectactions::activate() does.
    void setTransitionHandler(std::function<bool(const QString&)> handler)
    {
        m_transitionHandler = std::move(handler);
    }

    /// The options menu's checkable "Live Preview" entry.
    [[nodiscard]] QAction* livePreviewAction() const { return m_livePreviewAction; }
    /// True while Live Preview is on.
    [[nodiscard]] bool livePreviewEnabled() const;
    /// The pointer rests at @p viewportPos of the tree's viewport (the viewport's mouse moves
    /// come here; tests call it directly).
    void hoverAt(QPoint viewportPos);
    /// The pointer left the entries: ends a live preview, if one was asked for.
    void endHover();

signals:
    /// The user activated descriptor @p descriptorId (after it was applied).
    void effectActivated(const QString& descriptorId);
    /// Live Preview: the pointer rests on @p descriptorId at @p position (0 = the row's left
    /// edge, 1 = its right edge).
    void livePreviewRequested(const QString& descriptorId, double position);
    /// Live Preview: the hover ended; the viewer shows the normal picture again.
    void livePreviewEnded();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void populate();
    void applyFilters();
    void activate(const QString& descriptorId);

    AppContext& m_ctx;
    std::function<bool(const QString&)> m_transitionHandler;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
    QTreeWidget* m_categories = nullptr; ///< Resolve's sidebar: Toolbox > categories, Audio FX
    int m_category = -1; ///< EffectCategory shown, -1 = all
    QAction* m_livePreviewAction = nullptr; ///< options menu: Live Preview
    QString m_hoveredId; ///< entry a live preview was asked for (empty = none)
};

} // namespace kk
