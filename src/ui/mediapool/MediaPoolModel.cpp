// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolModel / MediaPoolFilterModel implementation.
#include "ui/mediapool/MediaPoolModel.h"

#include "core/ClipColors.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SmartBins.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"

#include <QDateTime>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>
#include <QMimeData>
#include <QUndoStack>

namespace kk {

namespace {
constexpr int kPosterCacheKiB = 48 * 1024; ///< ~48 MB of scaled posters (~1,700 tiles)
constexpr Rational kFallbackFps {25, 1};
/// Usage is recounted this long after the last timeline / pool change (a drag changes the
/// timeline on every mouse move; one count at the end is enough).
constexpr int kUsageDelayMs = 150;

// Keys of the columns (settings, project bin views). Order = MediaPoolModel::Column.
const char* const kColumnKeys[]
    = {"name", "duration", "resolution", "fps", "codec", "audio", "proxy", "optimized", "clipColor",
        "flags", "usage", "fileName", "path", "dateAdded", "dateModified", "description",
        "comments", "keywords", "scene", "shot", "take", "camera", "angle", "goodTake", "rating"};
static_assert(std::size(kColumnKeys) == MediaPoolModel::ColumnCount);

QString dateText(qint64 ms)
{
    return ms > 0 ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms), QLocale::ShortFormat)
                  : QString();
}

QString stars(int rating)
{
    return QString(rating, QChar(0x2605)); // BLACK STAR
}

/// Drag payload of media pool items. Project ids are stored directly; power bin ids are
/// offered as kPowerItemsMime and resolved to project ids only when a target asks for
/// mime::MediaItems (QMimeData::retrieveData() is called for every data() request; in-process
/// drags hand this very object to the drop target, also on Wayland).
class MediaDragData final : public QMimeData
{
public:
    MediaDragData(QStringList projectIds, QStringList powerIds, PowerResolver resolver)
        : m_projectIds(std::move(projectIds))
        , m_powerIds(std::move(powerIds))
        , m_resolver(std::move(resolver))
    { }

    [[nodiscard]] bool hasFormat(const QString& mimeType) const override
    {
        return formats().contains(mimeType);
    }

    [[nodiscard]] QStringList formats() const override
    {
        QStringList f {QLatin1String(mime::MediaItems)};
        if (!m_powerIds.isEmpty())
            f.append(QLatin1String(kPowerItemsMime));
        return f;
    }

protected:
    QVariant retrieveData(const QString& mimeType, QMetaType type) const override
    {
        if (mimeType == QLatin1String(kPowerItemsMime))
            return m_powerIds.join(QLatin1Char('\n')).toUtf8();
        if (mimeType != QLatin1String(mime::MediaItems))
            return QMimeData::retrieveData(mimeType, type);
        if (!m_resolved) {
            m_resolved = true;
            m_all = m_projectIds;
            if (!m_powerIds.isEmpty() && m_resolver)
                m_all += m_resolver(m_powerIds);
        }
        return m_all.join(QLatin1Char('\n')).toUtf8();
    }

private:
    QStringList m_projectIds;
    QStringList m_powerIds;
    PowerResolver m_resolver;
    mutable QStringList m_all;
    mutable bool m_resolved = false;
};
} // namespace

// ---- MediaPoolModel ----

MediaPoolModel::MediaPoolModel(Project* project, QObject* parent)
    : QAbstractTableModel(parent)
    , m_project(project)
{
    m_posters.setMaxCost(kPosterCacheKiB);
    connectPool(project->mediaPool(), false);

    m_usageTimer.setSingleShot(true);
    m_usageTimer.setInterval(kUsageDelayMs);
    connect(&m_usageTimer, &QTimer::timeout, this, &MediaPoolModel::refreshUsage);
    connect(project->timeline(), &Timeline::changed, this, &MediaPoolModel::scheduleUsage);
    connect(project, &Project::timelinesChanged, this, &MediaPoolModel::scheduleUsage);
    connect(project, &Project::projectReset, this, &MediaPoolModel::refreshUsage);
    connect(project->undoStack(), &QUndoStack::indexChanged, this, &MediaPoolModel::scheduleUsage);
    m_usage = usage::compute(*project);
    reload();
}

void MediaPoolModel::connectPool(MediaPool* pool, bool power)
{
    connect(pool, &MediaPool::itemAdded, this, &MediaPoolModel::onItemAdded);
    connect(pool, &MediaPool::itemRemoved, this, &MediaPoolModel::onItemRemoved);
    connect(pool, &MediaPool::itemChanged, this, &MediaPoolModel::onItemChanged);
    connect(pool, &MediaPool::reset, this, &MediaPoolModel::reload);
    if (!power)
        connect(pool, &MediaPool::changed, this, &MediaPoolModel::scheduleUsage);
}

void MediaPoolModel::setPowerBins(MediaPool* power)
{
    if (m_power == power)
        return;
    if (m_power)
        disconnect(m_power, nullptr, this, nullptr);
    m_power = power;
    if (m_power)
        connectPool(m_power, true);
    reload();
}

MediaPool* MediaPoolModel::poolOf(const QString& id) const
{
    const int row = m_rowById.value(id, -1);
    if (row < 0)
        return nullptr;
    return m_rows[row].power ? m_power : m_project->mediaPool();
}

const MediaItem* MediaPoolModel::item(const QString& id) const
{
    const MediaPool* pool = poolOf(id);
    return pool ? pool->item(id) : nullptr;
}

void MediaPoolModel::scheduleUsage()
{
    m_usageTimer.start();
}

void MediaPoolModel::refreshUsage()
{
    m_usageTimer.stop();
    m_usage = usage::compute(*m_project);
    for (int r = 0; r < m_rows.size(); ++r) {
        Row& row = m_rows[r];
        const int count = row.power ? 0 : m_usage.value(row.id, 0);
        if (count == row.usage)
            continue;
        row.usage = count;
        emit dataChanged(
            index(r, 0), index(r, ColumnCount - 1)); // all roles: re-filters smart bins
    }
}

QString MediaPoolModel::columnKey(int column)
{
    return column >= 0 && column < ColumnCount ? QLatin1String(kColumnKeys[column]) : QString();
}

int MediaPoolModel::columnForKey(const QString& key)
{
    for (int c = 0; c < ColumnCount; ++c) {
        if (key == QLatin1String(kColumnKeys[c]))
            return c;
    }
    return -1;
}

bool MediaPoolModel::columnVisibleByDefault(int column)
{
    return column <= UsageColumn;
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

MediaPoolModel::Row MediaPoolModel::makeRow(const MediaItem& item, bool power)
{
    const media::MediaInfo& info = item.info;
    Row row;
    row.id = item.id;
    row.name = item.name;
    row.path = item.path;
    row.binId = item.binId;
    row.power = power;
    row.kind = info.isStillImage ? ImageKind : (info.hasVideo ? VideoKind : AudioKind);
    row.durationUs = info.durationUs;
    if (info.durationUs > 0) {
        const Rational fps = info.frameRate.isValid() ? info.frameRate : kFallbackFps;
        row.durationText = timecode::format(usToFrames(info.durationUs, fps), fps);
    }
    if (info.hasVideo && info.width > 0) {
        row.resolutionText = QStringLiteral("%1 x %2").arg(info.width).arg(info.height);
        row.pixels = qint64(info.width) * info.height;
    }
    if (info.hasVideo && !info.isStillImage && info.frameRate.isValid()) {
        row.fps = info.frameRate.toDouble();
        row.fpsText = QString::number(row.fps, 'g', 5); // 25, 29.97, 23.976
    }
    row.codecText = info.hasVideo ? info.videoCodec : info.audioCodec;
    if (info.hasAudio) {
        row.audioText = QStringLiteral("%1 ch, %2 kHz")
                            .arg(info.audioChannels)
                            .arg(info.audioSampleRate / 1000.0, 0, 'g', 3);
    }
    row.clipColorId = findClipColor(item.clipColor) ? item.clipColor : QString();
    row.clipColor = clipColorValue(row.clipColorId);
    row.colorText = clipColorName(row.clipColorId);
    row.flags = item.flags;
    QStringList flagNames;
    for (int i = 0; i < int(flagPalette().size()); ++i) {
        if (item.flags & (1u << i))
            flagNames.append(flagName(i));
    }
    row.flagsText = flagNames.join(QStringLiteral(", "));
    row.metadata = item.metadata;
    row.keywordsText = item.metadata.keywords.join(QStringLiteral(", "));
    row.addedMs = item.addedMs;
    row.modifiedMs = item.fileModifiedMs;
    row.addedText = dateText(item.addedMs);
    row.modifiedText = dateText(item.fileModifiedMs);
    if (!item.isTimeline())
        row.fileName = QFileInfo(item.path).fileName();
    // Search covers the name, the path and every piece of user metadata (colour and flag
    // names included, so "teal" or "red" finds coloured / flagged items).
    const MediaMetadata& md = item.metadata;
    row.searchText
        = QStringList {item.name, item.path, row.colorText, row.flagsText, row.keywordsText,
            md.description, md.comments, md.scene, md.shot, md.take, md.camera, md.angle}
              .join(QLatin1Char(' '))
              .toLower();
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
    m_rows.reserve(items.size() + (m_power ? m_power->items().size() : 0));
    for (const MediaItem& item : items)
        m_rows.append(makeRow(item, false));
    if (m_power) {
        for (const MediaItem& item : m_power->items())
            m_rows.append(makeRow(item, true));
    }
    for (Row& row : m_rows) {
        fillLinks(row);
        row.usage = row.power ? 0 : m_usage.value(row.id, 0);
    }
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
    const bool power = m_power && sender() == m_power;
    const MediaPool* pool = power ? m_power : m_project->mediaPool();
    const MediaItem* item = pool->item(id);
    if (!item || m_rowById.contains(id))
        return;
    // Project items stay before the power bin items (rows are in import order per pool).
    int row = int(m_rows.size());
    if (!power) {
        row = 0;
        while (row < m_rows.size() && !m_rows[row].power)
            ++row;
    }
    beginInsertRows({}, row, row);
    m_rows.insert(row, makeRow(*item, power));
    fillLinks(m_rows[row]);
    m_rows[row].usage = power ? 0 : m_usage.value(id, 0);
    reindex(row);
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
    const MediaItem* item = row >= 0 ? this->item(id) : nullptr;
    if (!item)
        return;
    const int usage = m_rows[row].usage;
    m_rows[row] = makeRow(*item, m_rows[row].power);
    m_rows[row].usage = usage;
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
    row.links = m_linkStatus && !row.power ? m_linkStatus(row.id) : MediaLinkStatus {};
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

QVariant MediaPoolModel::displayText(const Row& row, int column) const
{
    const MediaMetadata& md = row.metadata;
    switch (column) {
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
    case ClipColorColumn:
        return row.colorText;
    case FlagsColumn:
        return row.flagsText;
    case UsageColumn:
        return row.power ? QString() : QString::number(row.usage);
    case FileNameColumn:
        return row.fileName;
    case PathColumn:
        return row.kind == TimelineKind || row.kind == CompoundKind ? QString() : row.path;
    case DateAddedColumn:
        return row.addedText;
    case DateModifiedColumn:
        return row.modifiedText;
    case DescriptionColumn:
        return md.description;
    case CommentsColumn:
        return md.comments;
    case KeywordsColumn:
        return row.keywordsText;
    case SceneColumn:
        return md.scene;
    case ShotColumn:
        return md.shot;
    case TakeColumn:
        return md.take;
    case CameraColumn:
        return md.camera;
    case AngleColumn:
        return md.angle;
    case GoodTakeColumn:
        return md.goodTake ? tr("Yes") : QString();
    case RatingColumn:
        return stars(md.rating);
    default:
        return {};
    }
}

QVariant MediaPoolModel::sortValue(const Row& row, int column) const
{
    // Raw values: numbers sort numerically (25 < 100 fps), dates by time, text lower-case.
    switch (column) {
    case DurationColumn:
        return row.durationUs;
    case ResolutionColumn:
        return row.pixels;
    case FpsColumn:
        return row.fps;
    case UsageColumn:
        return row.usage;
    case DateAddedColumn:
        return row.addedMs;
    case DateModifiedColumn:
        return row.modifiedMs;
    case GoodTakeColumn:
        return int(row.metadata.goodTake);
    case RatingColumn:
        return row.metadata.rating;
    case FlagsColumn:
        return row.flags == 0 ? QString() : row.flagsText.toLower();
    default:
        return displayText(row, column).toString().toLower();
    }
}

QVariant MediaPoolModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row& row = m_rows[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        return displayText(row, index.column());
    case Qt::DecorationRole:
        if (index.column() == ClipColorColumn)
            return row.clipColor.isValid() ? QVariant(row.clipColor) : QVariant();
        if (index.column() == FlagsColumn) {
            // The first flag's colour (the text names them all).
            for (int i = 0; i < int(flagPalette().size()); ++i) {
                if (row.flags & (1u << i))
                    return flagColor(i);
            }
            return {};
        }
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
    case Qt::ToolTipRole: {
        QString tip;
        if (row.kind == MulticamKind)
            tip = tr("%1\nMulticam clip: double-click to show its angles in the source viewer")
                      .arg(row.name);
        else if (row.kind == TimelineKind || row.kind == CompoundKind)
            tip = row.kind == TimelineKind
                ? tr("%1\nTimeline: double-click to open it in a tab").arg(row.name)
                : tr("%1\nCompound clip: double-click to open it in a tab").arg(row.name);
        else
            tip = row.offline ? tr("%1\nMedia offline: the file is missing.").arg(row.path)
                              : row.path;
        if (row.power)
            tip += QLatin1Char('\n') + tr("Power bin item (shared by every project)");
        else if (row.usage > 0)
            tip += QLatin1Char('\n') + tr("Used %n time(s) in timelines", nullptr, row.usage);
        else
            tip += QLatin1Char('\n') + tr("Not used in any timeline");
        return tip;
    }
    case Qt::ForegroundRole:
        if (row.offline)
            return schemeColors().negative;
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
    case ClipColorRole:
        return row.clipColor;
    case ClipColorIdRole:
        return row.clipColorId;
    case FlagsRole:
        return uint(row.flags);
    case UsageRole:
        return row.usage;
    case PowerRole:
        return row.power;
    case SortRole:
        return sortValue(row, index.column());
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
    case ClipColorColumn:
        return tr("Clip Color");
    case FlagsColumn:
        return tr("Flags");
    case UsageColumn:
        return tr("Usage");
    case FileNameColumn:
        return tr("File Name");
    case PathColumn:
        return tr("File Path");
    case DateAddedColumn:
        return tr("Date Added");
    case DateModifiedColumn:
        return tr("Date Modified");
    case DescriptionColumn:
        return tr("Description");
    case CommentsColumn:
        return tr("Comments");
    case KeywordsColumn:
        return tr("Keywords");
    case SceneColumn:
        return tr("Scene");
    case ShotColumn:
        return tr("Shot");
    case TakeColumn:
        return tr("Take");
    case CameraColumn:
        return tr("Camera");
    case AngleColumn:
        return tr("Angle");
    case GoodTakeColumn:
        return tr("Good Take");
    case RatingColumn:
        return tr("Rating");
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
    if (row.power) {
        m_power->renameItem(row.id, name); // power bins: saved at once, not undoable
        return true;
    }
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
    QStringList powerIds;
    for (const QModelIndex& index : indexes) {
        const int r = index.row();
        const QString id = idAt(r);
        if (id.isEmpty() || ids.contains(id) || powerIds.contains(id))
            continue;
        (m_rows[r].power ? powerIds : ids).append(id);
    }
    if (ids.isEmpty() && powerIds.isEmpty())
        return nullptr;
    return new MediaDragData(ids, powerIds, m_powerResolver);
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
    , m_defaults(smartbins::defaultSmartBins())
{
    setSortRole(MediaPoolModel::SortRole);
    updateBinSet();
}

bool MediaPoolFilterModel::isSmartKey(const QString& key)
{
    return key.startsWith(QLatin1String(kSmartPrefix));
}

bool MediaPoolFilterModel::isPowerKey(const QString& key)
{
    return key.startsWith(QLatin1String(kPowerPrefix));
}

QString MediaPoolFilterModel::idOfKey(const QString& key)
{
    if (isSmartKey(key))
        return key.mid(int(qstrlen(kSmartPrefix)));
    if (isPowerKey(key))
        return key.mid(int(qstrlen(kPowerPrefix)));
    return key;
}

const SmartBin* MediaPoolFilterModel::smartBinFor(const QString& key) const
{
    if (!isSmartKey(key))
        return nullptr;
    const QString id = idOfKey(key);
    if (id.startsWith(QLatin1String(smartbins::kDefaultIdPrefix))) {
        for (const SmartBin& b : m_defaults) {
            if (b.id == id)
                return &b;
        }
        return nullptr;
    }
    return m_project->mediaPool()->smartBin(id);
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

void MediaPoolFilterModel::setBin(const QString& binKey)
{
    if (binKey == m_bin)
        return;
    m_bin = binKey;
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

void MediaPoolFilterModel::setColorFilter(const QString& colorId)
{
    if (colorId == m_color)
        return;
    m_color = colorId;
    refilter();
}

void MediaPoolFilterModel::binsChanged()
{
    // The current bin was deleted: back to Master.
    if (!isSmartKey(m_bin) && !isPowerKey(m_bin) && !m_bin.isEmpty()
        && !m_project->mediaPool()->bin(m_bin))
        m_bin.clear();
    if (isPowerKey(m_bin) && !idOfKey(m_bin).isEmpty()) {
        const auto* model = qobject_cast<const MediaPoolModel*>(sourceModel());
        if (!model || !model->powerBins() || !model->powerBins()->bin(idOfKey(m_bin)))
            m_bin = QLatin1String(kPowerPrefix);
    }
    updateBinSet();
    refilter();
}

void MediaPoolFilterModel::smartBinsChanged()
{
    if (isSmartKey(m_bin) && !smartBinFor(m_bin))
        m_bin.clear(); // the smart bin was deleted
    refilter();
}

void MediaPoolFilterModel::updateBinSet()
{
    m_bins = {idOfKey(m_bin)};
    if (m_search.isEmpty() || isSmartKey(m_bin))
        return;
    // Searching covers the whole sub-tree: add descendants until no new bin is found.
    const MediaPool* pool = m_project->mediaPool();
    if (isPowerKey(m_bin)) {
        const auto* model = qobject_cast<const MediaPoolModel*>(sourceModel());
        pool = model ? model->powerBins() : nullptr;
        if (!pool)
            return;
    }
    const QVector<Bin>& bins = pool->bins();
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
    const bool power = index.data(MediaPoolModel::PowerRole).toBool();
    if (const SmartBin* smart = smartBinFor(m_bin)) {
        // Smart bins collect the project's items, wherever they are.
        if (power)
            return false;
        const auto* model = static_cast<const MediaPoolModel*>(sourceModel());
        const MediaItem* item = model->item(index.data(MediaPoolModel::IdRole).toString());
        const smartbins::Context ctx {&model->usageCounts(), 0};
        if (!item || !smartbins::matches(*smart, *item, ctx))
            return false;
    } else {
        if (power != isPowerKey(m_bin)
            || !m_bins.contains(index.data(MediaPoolModel::BinRole).toString()))
            return false;
    }
    if (!m_color.isEmpty()) {
        const QString color = index.data(MediaPoolModel::ClipColorIdRole).toString();
        if (m_color == QLatin1String("none") ? !color.isEmpty() : color != m_color)
            return false;
    }
    return m_search.isEmpty()
        || index.data(MediaPoolModel::SearchTextRole).toString().contains(m_search);
}

} // namespace kk
