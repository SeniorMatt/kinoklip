// SPDX-License-Identifier: GPL-3.0-or-later
// EditIndexPanel — the Edit page's Edit Index: a table of the timeline's events or markers.
#pragma once

#include "ui/common/PanelFrame.h"
#include "ui/editindex/EditIndexModel.h"

class QComboBox;
class QMenu;
class QLineEdit;
class QTableView;
class QTimer;

namespace kk {

struct AppContext;
class EditIndexFilter;

/// Resolve-style Edit Index (spec 18.8, 18.9): the events of the active timeline (#, reel, name,
/// track, source in / out, record in / out, duration, notes) or, in Markers mode, every
/// timeline and clip marker (colour, name, timecode, duration, notes, location).
///
/// Clicking a row (or moving the current row with the keyboard) moves the playhead to the
/// event's record in / the marker and selects the clip (clip markers: their clip). The
/// timeline selection is mirrored back into the table. A search field filters on any
/// column, a colour menu filters markers. Marker names and notes are edited in place, one
/// undo step each. The table follows every timeline change (coalesced to one rebuild per
/// event loop pass; skipped while the panel is hidden and done when it is shown).
///
/// Thread-safety: GUI thread only.
class EditIndexPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates the panel for @p ctx's project.
    explicit EditIndexPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Shows the events or the markers.
    void setMode(EditIndexModel::Mode mode);
    [[nodiscard]] EditIndexModel::Mode mode() const { return m_model->mode(); }
    /// The table (tests click and edit through it).
    [[nodiscard]] QTableView* table() const { return m_table; }
    /// The underlying model (unfiltered rows).
    [[nodiscard]] EditIndexModel* model() const { return m_model; }
    /// The search field.
    [[nodiscard]] QLineEdit* searchField() const { return m_search; }
    /// The marker colour filter (index 0 = all colours).
    [[nodiscard]] QComboBox* colorFilter() const { return m_colorFilter; }
    /// Re-reads the timeline now (normally coalesced; tests call it to skip the wait).
    void rebuildNow();
    /// Column chooser (spec 18.10): the header's context menu, one checkable entry per column
    /// of the current mode (the "#" column always stays). Built fresh for each use.
    [[nodiscard]] QMenu* createColumnMenu(QWidget* parent);
    /// Shows or hides @p column of the current mode and stores the choice
    /// (prefs::kEditIndexHiddenColumns). Column 0 ("#") cannot be hidden.
    void setColumnVisible(int column, bool visible);

protected:
    void showEvent(QShowEvent* event) override;
    /// Markers mode: Delete / Backspace in the table delete the marker instead of reaching
    /// the window's Delete commands (which would ripple delete the marker's clip).
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void scheduleRebuild();
    /// Jumps to the row at proxy row @p row: playhead and clip selection.
    void activateRow(int row);
    /// Makes the row of the timeline's current clip (Edits) the table's current row.
    void syncFromSelection();
    /// Hides the columns of the current mode stored in the settings.
    void applyHiddenColumns();
    /// Selects the proxy row whose key matches (clip id, stored marker frame); false if none.
    bool selectKey(quint64 clipId, qint64 storedFrame);

    AppContext& m_ctx;
    EditIndexModel* m_model = nullptr;
    EditIndexFilter* m_filter = nullptr;
    QTableView* m_table = nullptr;
    QComboBox* m_modeBox = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_colorFilter = nullptr;
    QTimer* m_rebuildTimer = nullptr;
    bool m_dirty = true;
    bool m_syncing = false; ///< current row changed by us, not by the user
};

} // namespace kk
