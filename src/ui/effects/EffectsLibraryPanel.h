// SPDX-License-Identifier: GPL-3.0-or-later
// EffectsLibraryPanel — browsable, searchable catalogue of transitions, titles, generators and
// effects; drag source for mime::Effect, double-click applies.
#pragma once

#include "ui/common/PanelFrame.h"

class QLineEdit;
class QTreeWidget;

namespace kk {

struct AppContext;

/// Lists the EffectRegistry catalogue in sections (Video Transitions, Titles, Generators,
/// Video Effects, Audio Effects), with a search field filtering by name and description.
///
/// - Dragging an entry starts a drag with mime::Effect (payload = descriptor id), dropped on
///   the timeline (timeline agent) or the inspector.
/// - Double-click (or Enter) runs effectactions::activate(): effects go onto the selected
///   clips, transitions onto the current clip, titles/generators are added at the playhead.
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

signals:
    /// The user activated descriptor @p descriptorId (after it was applied).
    void effectActivated(const QString& descriptorId);

private:
    void populate();
    void applyFilters();
    void activate(const QString& descriptorId);

    AppContext& m_ctx;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
    QTreeWidget* m_categories = nullptr; ///< Resolve's sidebar: Toolbox > categories, Audio FX
    int m_category = -1; ///< EffectCategory shown, -1 = all
};

} // namespace kk
