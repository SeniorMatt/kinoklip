// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolModel / MediaPoolFilterModel implementation.
#include "ui/mediapool/MediaPoolModel.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/common/TimecodeFormat.h"

#include <QFileInfo>
#include <QIcon>
#include <QMimeData>

namespace kk {

namespace {
constexpr int kPosterCacheKiB = 48 * 1024; ///< ~48 MB of scaled posters (~1,700 tiles)
constexpr Rational kFallbackFps {25, 1};
} // namespace

// ---- MediaPoolModel ----

MediaPoolModel::MediaPoolModel(Project* project, QObject* parent)
    : QAbstractTableModel(parent)
    , m_project(project)
{
    m_posters.setMaxCost(kPosterCacheKiB);
    MediaPool* pool = project->mediaPool();
    connect(pool, &MediaPool::itemAdded, this, &MediaPoolModel::onItemAdded);
    connect(pool, &MediaPool::itemRemoved, this, &MediaPoolModel::onItemRemoved);
    connect(pool, &MediaPool::itemChanged, this, &MediaPoolModel::onItemChanged);
    connect(pool, &MediaPool::reset, this, &MediaPoolModel::reload);
    reload();
}

void MediaPoolModel::setThumbnailProvider(PosterProvider provider)
{
    m_provider = std::move(provider);
    m_posters.clear();
    m_pending.clear();
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, 0), {PosterRole});
}

void MediaPoolModel::thumbnailReady(const QString& path)
{
    if (!m_pending.remove(path))
        return; // not ours (another view asked the cache) or already delivered
    for (int r = 0; r < m_rows.size(); ++r) {
        if (m_rows[r].path == path)
            emit dataChanged(index(r, 0), index(r, 0), {PosterRole});
    }
}

void MediaPoolModel::recheckOffline()
{
    for (int r = 0; r < m_rows.size(); ++r) {
        if (m_rows[r].kind == TimelineKind || m_rows[r].kind == CompoundKind
            || m_rows[r].kind == MulticamKind)
            continue; // no file behind them
        const bool offline = !QFileInfo::exists(m_rows[r].path);
        if (offline != m_rows[r].offline) {
            m_rows[r].offline = offline;
            emit dataChanged(index(r, 0), index(r, ColumnCount - 1));
        }
    }
}

QString MediaPoolModel::idAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows[row].id : QString();
}

MediaPoolModel::Row MediaPoolModel::makeRow(const MediaItem& item)
{
    const media::MediaInfo& info = item.info;
    Row row;
    row.id = item.id;
    row.name = item.name;
    row.path = item.path;
    row.binId = item.binId;
    row.kind = info.isStillImage ? ImageKind : (info.hasVideo ? VideoKind : AudioKind);
    if (info.durationUs > 0) {
        const Rational fps = info.frameRate.isValid() ? info.frameRate : kFallbackFps;
        row.durationText = timecode::format(usToFrames(info.durationUs, fps), fps);
    }
    if (info.hasVideo && info.width > 0)
        row.resolutionText = QStringLiteral("%1 x %2").arg(info.width).arg(info.height);
    if (info.hasVideo && !info.isStillImage && info.frameRate.isValid())
        row.fpsText = QString::number(info.frameRate.toDouble(), 'g', 5); // 25, 29.97, 23.976
    row.codecText = info.hasVideo ? info.videoCodec : info.audioCodec;
    if (info.hasAudio) {
        row.audioText = QStringLiteral("%1 ch, %2 kHz")
                            .arg(info.audioChannels)
                            .arg(info.audioSampleRate / 1000.0, 0, 'g', 3);
    }
    row.searchText = (item.name + QLatin1Char(' ') + item.path).toLower();
    if (item.isTimeline()) {
        row.kind = item.kind == MediaItemKind::MulticamClip ? MulticamKind
            : item.kind == MediaItemKind::CompoundClip      ? CompoundKind
                                                            : TimelineKind;
        row.codecText = item.kind == MediaItemKind::MulticamClip ? tr("Multicam Clip")
            : item.kind == MediaItemKind::CompoundClip           ? tr("Compound Clip")
                                                                 : tr("Timeline");
        row.audioText.clear();
        row.offline = false;
        return row;
    }
    if (item.isSubtitle()) {
        row.kind = SubtitleKind;
        row.codecText = item.info.container == QLatin1String("vtt") ? tr("WebVTT Subtitle")
                                                                    : tr("SubRip Subtitle");
        row.audioText = tr("%n caption(s)", nullptr, int(item.cues.size()));
    }
    // One stat() per item on add/reset, never while painting.
    row.offline = !QFileInfo::exists(item.path);
    return row;
}

void MediaPoolModel::reload()
{
    beginResetModel();
    m_rows.clear();
    m_rowById.clear();
    const QVector<MediaItem>& items = m_project->mediaPool()->items();
    m_rows.reserve(items.size());
    for (const MediaItem& item : items)
        m_rows.append(makeRow(item));
    for (Row& row : m_rows)
        fillLinks(row);
    reindex(0);
    endResetModel();
}

void MediaPoolModel::reindex(int fromRow)
{
    for (int r = fromRow; r < m_rows.size(); ++r)
        m_rowById.insert(m_rows[r].id, r);
}

void MediaPoolModel::onItemAdded(const QString& id)
{
    const MediaItem* item = m_project->mediaPool()->item(id);
    if (!item || m_rowById.contains(id))
        return;
    const int row = int(m_rows.size());
    beginInsertRows({}, row, row);
    m_rows.append(makeRow(*item));
    fillLinks(m_rows.back());
    m_rowById.insert(id, row);
    endInsertRows();
}

void MediaPoolModel::onItemRemoved(const QString& id)
{
    const int row = m_rowById.value(id, -1);
    if (row < 0)
        return;
    beginRemoveRows({}, row, row);
    m_rows.remove(row);
    m_rowById.remove(id);
    reindex(row);
    endRemoveRows();
}

void MediaPoolModel::onItemChanged(const QString& id)
{
    const int row = m_rowById.value(id, -1);
    const MediaItem* item = m_project->mediaPool()->item(id);
    if (row < 0 || !item)
        return;
    m_rows[row] = makeRow(*item);
    fillLinks(m_rows[row]);
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void MediaPoolModel::setLinkStatusProvider(
    std::function<MediaLinkStatus(const QString& id)> provider)
{
    m_linkStatus = std::move(provider);
    linkStatusChanged();
}

void MediaPoolModel::fillLinks(Row& row) const
{
    // A few stat() calls per item, only when its status may have changed (never per paint).
    row.links = m_linkStatus ? m_linkStatus(row.id) : MediaLinkStatus {};
}

void MediaPoolModel::linkStatusChanged(const QString& id)
{
    if (m_rows.isEmpty())
        return;
    if (id.isEmpty()) {
        for (Row& row : m_rows)
            fillLinks(row);
        emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, ColumnCount - 1));
        return;
    }
    const int row = m_rowById.value(id, -1);
    if (row < 0)
        return;
    fillLinks(m_rows[row]);
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

int MediaPoolModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int MediaPoolModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QPixmap MediaPoolModel::poster(const Row& row) const
{
    if (row.kind == AudioKind || row.kind == TimelineKind || row.kind == CompoundKind
        || row.kind == MulticamKind || row.kind == SubtitleKind || !m_provider)
        return {};
    if (const QPixmap* cached = m_posters.object(row.path))
        return *cached;
    if (m_pending.contains(row.path))
        return {};
    const QImage image = m_provider(row.path, kPosterSize.height());
    if (image.isNull()) {
        m_pending.insert(row.path); // thumbnailReady() will clear it
        return {};
    }
    // Scale once to the tile, so painting is a plain blit.
    auto* scaled = new QPixmap(QPixmap::fromImage(
        image.scaled(kPosterSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    const QPixmap result = *scaled;
    const int costKiB = std::max(1, int(qint64(scaled->width()) * scaled->height() * 4 / 1024));
    m_posters.insert(row.path, scaled, costKiB);
    return result;
}

QVariant MediaPoolModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row& row = m_rows[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        switch (index.column()) {
        case NameColumn:
            return row.name;
        case DurationColumn:
            return row.durationText;
        case ResolutionColumn:
            return row.resolutionText;
        case FpsColumn:
            return row.fpsText;
        case CodecColumn:
            return row.codecText;
        case AudioColumn:
            return row.audioText;
        case ProxyColumn:
            return row.links.proxyText;
        case OptimizedColumn:
            return row.links.optimizedText;
        default:
            return {};
        }
    case Qt::DecorationRole:
        if (index.column() != NameColumn)
            return {};
        // Icons are cached by kk::icon(); three shared instances for all rows.
        switch (row.kind) {
        case ImageKind:
            return icon(QStringLiteral("image"));
        case AudioKind:
            return icon(QStringLiteral("audio_file"));
        case TimelineKind:
            return icon(QStringLiteral("timeline"));
        case CompoundKind:
            return icon(QStringLiteral("compound_clip"));
        case MulticamKind:
            return icon(QStringLiteral("multicam_clip"));
        case SubtitleKind:
            return icon(QStringLiteral("subtitle"));
        default:
            return icon(QStringLiteral("film"));
        }
    case Qt::ToolTipRole:
        if (row.kind == MulticamKind)
            return tr("%1\nMulticam clip: double-click to show its angles in the source viewer")
                .arg(row.name);
        if (row.kind == TimelineKind || row.kind == CompoundKind)
            return row.kind == TimelineKind
                ? tr("%1\nTimeline: double-click to open it in a tab").arg(row.name)
                : tr("%1\nCompound clip: double-click to open it in a tab").arg(row.name);
        return row.offline ? tr("%1\nMedia offline: the file is missing.").arg(row.path) : row.path;
    case Qt::ForegroundRole:
        if (row.offline)
            return QColor(0xe0, 0x60, 0x60);
        return {};
    case IdRole:
        return row.id;
    case BinRole:
        return row.binId;
    case OfflineRole:
        return row.offline;
    case PosterRole:
        return poster(row);
    case KindRole:
        return int(row.kind);
    case DurationTextRole:
        return row.durationText;
    case SearchTextRole:
        return row.searchText;
    case LinkBadgeRole:
        return row.links.badge;
    case LinkProblemRole:
        return row.links.problem;
    default:
        return {};
    }
}

QVariant MediaPoolModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case NameColumn:
        return tr("Name");
    case DurationColumn:
        return tr("Duration");
    case ResolutionColumn:
        return tr("Resolution");
    case FpsColumn:
        return tr("FPS");
    case CodecColumn:
        return tr("Codec");
    case AudioColumn:
        return tr("Audio");
    case ProxyColumn:
        return tr("Proxy");
    case OptimizedColumn:
        return tr("Optimized Media");
    default:
        return {};
    }
}

Qt::ItemFlags MediaPoolModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (index.column() == NameColumn)
        f |= Qt::ItemIsEditable;
    return f;
}

bool MediaPoolModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || index.column() != NameColumn || !index.isValid())
        return false;
    const QString name = value.toString().trimmed();
    const Row& row = m_rows[index.row()];
    if (name.isEmpty() || name == row.name)
        return false;
    m_project->beginEdit(tr("Rename Media"));
    m_project->mediaPool()->renameItem(row.id, name);
    m_project->endEdit();
    return true;
}

QStringList MediaPoolModel::mimeTypes() const
{
    return {QLatin1String(mime::MediaItems)};
}

QMimeData* MediaPoolModel::mimeData(const QModelIndexList& indexes) const
{
    QStringList ids;
    for (const QModelIndex& index : indexes) {
        const QString id = idAt(index.row());
        if (!id.isEmpty() && !ids.contains(id))
            ids.append(id);
    }
    if (ids.isEmpty())
        return nullptr;
    auto* data = new QMimeData;
    data->setData(QLatin1String(mime::MediaItems), ids.join(QLatin1Char('\n')).toUtf8());
    return data;
}

Qt::DropActions MediaPoolModel::supportedDragActions() const
{
    // Copy only: a drag never removes items (with Move, QAbstractItemView deletes the rows a
    // target accepted), and the Wayland compositor negotiates a single, unambiguous action.
    return Qt::CopyAction;
}

// ---- MediaPoolFilterModel ----

MediaPoolFilterModel::MediaPoolFilterModel(Project* project, QObject* parent)
    : QSortFilterProxyModel(parent)
    , m_project(project)
{
    updateBinSet();
}

void MediaPoolFilterModel::refilter()
{
    // Qt 6.12 replaces invalidateFilter() by the begin/endFilterChange() pair (Qt >= 6.9);
    // Qt 6.4 only has invalidateFilter(). The filter state was already updated by the caller,
    // which is fine: endFilterChange() re-evaluates every row like invalidateFilter() did.
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    beginFilterChange();
    endFilterChange();
#else
    invalidateFilter();
#endif
}

void MediaPoolFilterModel::setBin(const QString& binId)
{
    if (binId == m_bin)
        return;
    m_bin = binId;
    updateBinSet();
    refilter();
}

void MediaPoolFilterModel::setSearchText(const QString& text)
{
    const QString lower = text.trimmed().toLower();
    if (lower == m_search)
        return;
    const bool scopeChanged = lower.isEmpty() != m_search.isEmpty();
    m_search = lower;
    if (scopeChanged)
        updateBinSet();
    refilter();
}

void MediaPoolFilterModel::binsChanged()
{
    if (!m_bin.isEmpty() && !m_project->mediaPool()->bin(m_bin))
        m_bin.clear(); // the current bin was deleted
    updateBinSet();
    refilter();
}

void MediaPoolFilterModel::updateBinSet()
{
    m_bins = {m_bin};
    if (m_search.isEmpty())
        return;
    // Searching covers the whole sub-tree: add descendants until no new bin is found.
    const QVector<Bin>& bins = m_project->mediaPool()->bins();
    bool grew = true;
    while (grew) {
        grew = false;
        for (const Bin& b : bins) {
            if (!m_bins.contains(b.id) && m_bins.contains(b.parentId)) {
                m_bins.insert(b.id);
                grew = true;
            }
        }
    }
}

bool MediaPoolFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!m_bins.contains(index.data(MediaPoolModel::BinRole).toString()))
        return false;
    return m_search.isEmpty()
        || index.data(MediaPoolModel::SearchTextRole).toString().contains(m_search);
}

} // namespace kk
