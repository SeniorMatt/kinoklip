// SPDX-License-Identifier: GPL-3.0-or-later
// EditIndexModel — rows, display text and marker editing of the Edit Index.
#include "ui/editindex/EditIndexModel.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"

namespace kk {

namespace {
/// Markers are single-frame (no ranged markers): the Duration column shows one frame.
constexpr int64_t kMarkerDurationFrames = 1;
} // namespace

EditIndexModel::EditIndexModel(Project* project, QObject* parent)
    : QAbstractTableModel(parent)
    , m_project(project)
{ }

void EditIndexModel::setMode(Mode mode)
{
    if (mode == m_mode)
        return;
    beginResetModel();
    m_mode = mode;
    endResetModel();
}

void EditIndexModel::rebuild()
{
    beginResetModel();
    const TimelineData& data = m_project->timeline()->data();
    m_events = editindex::events(data, m_project->mediaPool());
    m_markers = editindex::markers(data);
    endResetModel();
}

int EditIndexModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return int(m_mode == Mode::Edits ? m_events.size() : m_markers.size());
}

int EditIndexModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return m_mode == Mode::Edits ? int(EditColumnCount) : int(MarkerColumnCount);
}

QVariant EditIndexModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};
    if (m_mode == Mode::Edits)
        return eventData(m_events[size_t(index.row())], index.column(), role);
    return markerData(m_markers[size_t(index.row())], index.column(), role);
}

QVariant EditIndexModel::eventData(const editindex::EventRow& row, int column, int role) const
{
    const Rational fps = m_project->timeline()->data().fps;
    switch (role) {
    case ClipIdRole:
        return row.clipId;
    case FrameRole:
        return qint64(row.recordIn);
    case Qt::ForegroundRole:
        return row.enabled ? QVariant() : QVariant(schemeColors().textDisabled);
    case SortRole:
        switch (column) {
        case EditNumber:
            return row.number;
        case EditRecordIn:
            return qint64(row.recordIn);
        case EditRecordOut:
            return qint64(row.recordOut);
        case EditDuration:
            return qint64(row.recordOut - row.recordIn);
        default:
            return eventData(row, column, Qt::DisplayRole);
        }
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
        switch (column) {
        case EditNumber:
            return QString::number(row.number).rightJustified(3, QLatin1Char('0'));
        case EditReel:
            return row.reel;
        case EditName:
            return row.name;
        case EditTrack:
            return row.trackName;
        case EditSourceIn:
            return row.sourceIn;
        case EditSourceOut:
            return row.sourceOut;
        case EditRecordIn:
            return timecode::format(row.recordIn, fps);
        case EditRecordOut:
            return timecode::format(row.recordOut, fps);
        case EditDuration:
            return timecode::format(row.recordOut - row.recordIn, fps);
        case EditNotes:
            return row.notes;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return {};
}

QVariant EditIndexModel::markerData(const editindex::MarkerRow& row, int column, int role) const
{
    const Rational fps = m_project->timeline()->data().fps;
    switch (role) {
    case ClipIdRole:
        return row.clipId;
    case FrameRole:
        return qint64(row.frame);
    case StoredFrameRole:
        return qint64(row.storedFrame);
    case MarkerColorRole:
        return row.color;
    case Qt::DecorationRole:
        return column == MarkerColor ? QVariant(row.color) : QVariant();
    case SortRole:
        switch (column) {
        case MarkerNumber:
            return row.number;
        case MarkerColor:
            return row.color.name();
        case MarkerTimecode:
            return qint64(row.frame);
        default:
            return markerData(row, column, Qt::DisplayRole);
        }
    case Qt::EditRole:
        if (column == MarkerName)
            return row.name;
        if (column == MarkerNotes)
            return row.note;
        return {};
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
        switch (column) {
        case MarkerNumber:
            return QString::number(row.number).rightJustified(3, QLatin1Char('0'));
        case MarkerName:
            return row.name;
        case MarkerTimecode:
            return timecode::format(row.frame, fps);
        case MarkerDuration:
            return timecode::format(kMarkerDurationFrames, fps);
        case MarkerNotes:
            // One line in the table; the tool tip keeps the line breaks.
            return role == Qt::ToolTipRole ? row.note
                                           : QString(row.note).replace(QLatin1Char('\n'), u' ');
        case MarkerLocation:
            return row.location;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return {};
}

QVariant EditIndexModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    if (m_mode == Mode::Edits) {
        switch (section) {
        case EditNumber:
            return tr("#");
        case EditReel:
            return tr("Reel");
        case EditName:
            return tr("Name");
        case EditTrack:
            return tr("Track");
        case EditSourceIn:
            return tr("Source In");
        case EditSourceOut:
            return tr("Source Out");
        case EditRecordIn:
            return tr("Record In");
        case EditRecordOut:
            return tr("Record Out");
        case EditDuration:
            return tr("Duration");
        case EditNotes:
            return tr("Notes");
        default:
            return {};
        }
    }
    switch (section) {
    case MarkerNumber:
        return tr("#");
    case MarkerColor:
        return tr("Color");
    case MarkerName:
        return tr("Name");
    case MarkerTimecode:
        return tr("Timecode");
    case MarkerDuration:
        return tr("Duration");
    case MarkerNotes:
        return tr("Notes");
    case MarkerLocation:
        return tr("Location");
    default:
        return {};
    }
}

Qt::ItemFlags EditIndexModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (m_mode == Mode::Markers && index.isValid() && index.row() < rowCount()
        && (index.column() == MarkerName || index.column() == MarkerNotes)
        && !m_markers[size_t(index.row())].locked)
        f |= Qt::ItemIsEditable;
    return f;
}

bool EditIndexModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || !(flags(index) & Qt::ItemIsEditable))
        return false;
    const editindex::MarkerRow row = m_markers[size_t(index.row())];
    const bool isName = index.column() == MarkerName;
    const QString text = value.toString();
    if (text == (isName ? row.name : row.note))
        return false;
    // One undo step per cell edit; endEdit() emits Timeline::changed(), which rebuilds us.
    m_project->beginEdit(isName ? tr("Rename Marker") : tr("Modify Marker"));
    Marker* marker
        = editindex::findMarker(m_project->timeline()->mutableData(), row.clipId, row.storedFrame);
    if (!marker) {
        m_project->cancelEdit();
        return false;
    }
    (isName ? marker->name : marker->note) = text;
    m_project->endEdit();
    return true;
}

bool EditIndexModel::deleteMarker(int row)
{
    if (m_mode != Mode::Markers || row < 0 || row >= int(m_markers.size())
        || m_markers[size_t(row)].locked)
        return false;
    const editindex::MarkerRow marker = m_markers[size_t(row)];
    m_project->beginEdit(tr("Delete Marker"));
    TimelineData& data = m_project->timeline()->mutableData();
    QVector<Marker>* list = &data.markers;
    if (marker.clipId) {
        ClipData* clip = editops::clipById(data, marker.clipId);
        list = clip ? &clip->markers : nullptr;
    }
    const qsizetype removed = list
        ? list->removeIf([&marker](const Marker& m) { return m.frame == marker.storedFrame; })
        : 0;
    if (removed == 0) {
        m_project->cancelEdit();
        return false;
    }
    m_project->endEdit();
    return true;
}

} // namespace kk
