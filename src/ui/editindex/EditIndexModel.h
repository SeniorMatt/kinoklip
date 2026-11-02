// SPDX-License-Identifier: GPL-3.0-or-later
// EditIndexModel — table model of the Edit Index panel: the timeline's events or its markers.
#pragma once

#include "ui/editindex/EditIndexRows.h"

#include <QAbstractTableModel>

#include <vector>

namespace kk {

class Project;

/// Lists the active timeline's events (one row per clip, like an EDL) or its markers
/// (timeline ruler and clip markers). rebuild() re-reads the timeline; the panel calls it on
/// every Timeline::changed() while it is visible.
///
/// In Markers mode the Name and Notes cells are editable: each change is one undo step
/// ("Modify Marker"). Clip markers on locked tracks are read-only.
///
/// Thread-safety: GUI thread only.
class EditIndexModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    /// What the table lists.
    enum class Mode {
        Edits,
        Markers
    };
    /// Edits mode columns.
    enum EditColumn {
        EditNumber,
        EditReel,
        EditName,
        EditTrack,
        EditSourceIn,
        EditSourceOut,
        EditRecordIn,
        EditRecordOut,
        EditDuration,
        EditNotes,
        EditColumnCount
    };
    /// Markers mode columns.
    enum MarkerColumn {
        MarkerNumber,
        MarkerColor,
        MarkerName,
        MarkerTimecode,
        MarkerDuration,
        MarkerNotes,
        MarkerLocation,
        MarkerColumnCount
    };
    /// Extra item data roles.
    enum Role {
        SortRole = Qt::UserRole + 1, ///< numeric sort key of the cell (frames, numbers)
        ClipIdRole, ///< clip id of the row (0 for a timeline marker)
        FrameRole, ///< timeline frame the row jumps to (record in / marker frame)
        MarkerColorRole, ///< QColor of a marker row
        StoredFrameRole ///< Marker::frame of a marker row (clip-local for clip markers)
    };

    /// Creates the model for @p project's active timeline (no rows until rebuild()).
    explicit EditIndexModel(Project* project, QObject* parent = nullptr);

    /// Switches between events and markers (resets the model).
    void setMode(Mode mode);
    [[nodiscard]] Mode mode() const { return m_mode; }
    /// Re-reads the active timeline (model reset).
    void rebuild();

    /// The event rows (Edits mode data; also filled in Markers mode).
    [[nodiscard]] const std::vector<editindex::EventRow>& eventRows() const { return m_events; }
    /// The marker rows.
    [[nodiscard]] const std::vector<editindex::MarkerRow>& markerRows() const { return m_markers; }

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

private:
    [[nodiscard]] QVariant eventData(const editindex::EventRow& row, int column, int role) const;
    [[nodiscard]] QVariant markerData(const editindex::MarkerRow& row, int column, int role) const;

    Project* m_project;
    Mode m_mode = Mode::Edits;
    std::vector<editindex::EventRow> m_events;
    std::vector<editindex::MarkerRow> m_markers;
};

} // namespace kk
