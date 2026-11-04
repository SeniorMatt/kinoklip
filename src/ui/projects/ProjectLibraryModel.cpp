// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectLibraryModel / ProjectSortProxy implementation.
#include "ui/projects/ProjectLibraryModel.h"

#include "media/MediaCache.h"
#include "platform/DroppedFiles.h"
#include "ui/common/Icons.h"
#include "ui/common/ProgressTask.h"
#include "ui/common/TimecodeFormat.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMimeData>
#include <QTimer>
#include <QUrl>

#include <cmath>

namespace kk {

namespace {

QString fpsText(Rational fps)
{
    if (fps.num <= 0 || fps.den <= 0)
        return {};
    const double value = fps.toDouble();
    if (value == std::floor(value))
        return QLocale().toString(value, 'f', 0); // 25 -> "25"
    // 29.97 -> "29.97", 23.976 -> "23.976" (3 decimals, trailing zeros dropped)
    QString text = QLocale().toString(value, 'f', 3);
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    return text;
}

QString resolutionText(int width, int height)
{
    return width > 0 && height > 0 ? QStringLiteral("%1 x %2").arg(width).arg(height) : QString();
}

QString modifiedText(qint64 ms)
{
    if (ms <= 0)
        return {};
    return QLocale().toString(
        QDateTime::fromMSecsSinceEpoch(ms).toLocalTime(), QLocale::ShortFormat);
}

} // namespace

ProjectLibraryModel::ProjectLibraryModel(ProjectLibrary* library, QObject* parent)
    : QAbstractTableModel(parent)
    , m_library(library)
    , m_folder(library->root())
{
    connect(library, &ProjectLibrary::metadataReady, this, &ProjectLibraryModel::rowChanged);
    connect(library, &ProjectLibrary::posterReady, this, &ProjectLibraryModel::rowChanged);
    connect(library, &ProjectLibrary::changed, this, &ProjectLibraryModel::refresh);
    connect(&media::MediaCache::instance(), &media::MediaCache::thumbnailReady, this,
        &ProjectLibraryModel::mediaThumbnailReady);
    reload();
}

void ProjectLibraryModel::showFolder(const QString& folder)
{
    m_recent = false;
    m_folder = folder.isEmpty() ? m_library->root() : QDir::cleanPath(folder);
    reload();
}

void ProjectLibraryModel::showRecent(const QStringList& paths)
{
    m_recent = true;
    m_recentPaths = paths;
    m_folder = m_library->root();
    reload();
}

void ProjectLibraryModel::refresh()
{
    if (!m_recent && !QFileInfo(m_folder).isDir())
        m_folder = m_library->root(); // the shown folder was renamed / trashed
    reload();
}

void ProjectLibraryModel::reload()
{
    beginResetModel();
    if (m_recent) {
        m_entries = ProjectLibrary::entriesFor(m_recentPaths);
    } else {
        m_entries = m_library->list(m_folder);
    }
    m_rowOf.clear();
    m_rowOf.reserve(m_entries.size());
    for (int r = 0; r < m_entries.size(); ++r)
        m_rowOf.insert(m_entries[r].path, r);
    m_waitingThumbnails.clear();
    endResetModel();
}

void ProjectLibraryModel::rowChanged(const QString& path)
{
    const int row = rowOf(path);
    if (row >= 0)
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void ProjectLibraryModel::mediaThumbnailReady(const QString& mediaPath)
{
    const QSet<QString> projects = m_waitingThumbnails.take(mediaPath);
    for (const QString& p : projects)
        rowChanged(p); // data(PosterRole) fetches the thumbnail now and stores the poster
}

QImage ProjectLibraryModel::posterFor(const ProjectEntry& e) const
{
    ProjectLibrary::PosterState state = ProjectLibrary::PosterState::Absent;
    QImage image = m_library->poster(e, &state);
    if (state != ProjectLibrary::PosterState::Absent)
        return image; // Ready, or Loading (posterReady() follows)
    const std::optional<ProjectMetadata> meta = m_library->metadata(e);
    if (!meta || meta->posterMediaPath.isEmpty())
        return {};
    // Nothing cached for this version of the project: ask the media cache (itself disk
    // cached per media file) for the frame and keep it as the project's poster.
    image = media::MediaCache::instance().thumbnail(
        meta->posterMediaPath, meta->posterTimeUs, kPosterHeight);
    if (image.isNull()) {
        m_waitingThumbnails[meta->posterMediaPath].insert(e.path);
        return {};
    }
    m_library->storePoster(e, image);
    return image;
}

int ProjectLibraryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

int ProjectLibraryModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ProjectLibraryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const ProjectEntry& e = m_entries[index.row()];
    switch (role) {
    case PathRole:
        return e.path;
    case IsFolderRole:
        return e.isFolder;
    case ExistsRole:
        return e.exists;
    case IsOpenRole:
        return !e.isFolder && e.path == m_library->openProjectPath();
    case ModifiedTextRole:
        return e.exists ? modifiedText(e.modifiedMs) : tr("File not found");
    case PosterRole:
        return e.isFolder ? QImage() : posterFor(e);
    case KindBadgeRole:
        if (const ProjectKind* k = e.isFolder ? nullptr : projectkinds::byId(e.kindId))
            return k->badge;
        return QString();
    default:
        break;
    }
    if (role == Qt::ToolTipRole)
        return QDir::toNativeSeparators(e.path);
    if (role == Qt::DecorationRole && index.column() == NameColumn)
        return icon(e.isFolder ? QStringLiteral("folder") : QStringLiteral("film"));

    if (role == Qt::EditRole)
        return e.name;
    const bool display = role == Qt::DisplayRole;
    const bool sort = role == SortRole;
    // Columns known from the listing: no metadata needed (sorting by name or date never
    // opens a project).
    if (display || sort) {
        switch (index.column()) {
        case NameColumn:
            return e.name;
        case ModifiedColumn:
            return sort ? QVariant(e.modifiedMs) : QVariant(modifiedText(e.modifiedMs));
        case LocationColumn:
            return QDir::toNativeSeparators(QFileInfo(e.path).absolutePath());
        case KindColumn:
            if (const ProjectKind* k = e.isFolder ? nullptr : projectkinds::byId(e.kindId))
                return k->badge;
            return e.isFolder ? tr("Folder") : QString();
        default:
            break;
        }
    }
    if (!display && !sort && role != ErrorRole && role != MissingMediaRole && role != DetailsRole
        && role != LoadedRole)
        return {};
    if (e.isFolder || !e.exists) {
        if (role == LoadedRole)
            return true;
        if (role == ErrorRole)
            return e.exists ? QString() : tr("The project file is missing.");
        if (sort)
            return -1.0;
        return {};
    }
    const std::optional<ProjectMetadata> meta = m_library->metadata(e);
    if (role == LoadedRole)
        return meta.has_value();
    if (role == ErrorRole)
        return meta ? meta->error : QString();
    if (role == MissingMediaRole)
        return meta ? meta->missingMediaCount : 0;
    if (role == DetailsRole) {
        if (!meta)
            return tr("Loadingâ¦");
        if (!meta->isValid())
            return meta->error;
        QStringList parts;
        if (meta->timelineFps.num > 0)
            parts << timecode::format(meta->durationFrames, meta->timelineFps);
        if (const QString res = resolutionText(meta->width, meta->height); !res.isEmpty())
            parts << res;
        if (const QString fps = fpsText(meta->fps); !fps.isEmpty())
            parts << tr("%1 fps").arg(fps);
        return parts.join(QStringLiteral("  ·  "));
    }
    switch (index.column()) {
    case DurationColumn:
        if (!meta || !meta->isValid())
            return sort ? QVariant(-1.0) : QVariant();
        return sort
            ? QVariant(double(meta->durationFrames) / std::max(0.001, meta->timelineFps.toDouble()))
            : QVariant(timecode::format(meta->durationFrames, meta->timelineFps));
    case ResolutionColumn:
        if (!meta || !meta->isValid())
            return sort ? QVariant(-1.0) : QVariant();
        return sort ? QVariant(double(meta->width) * meta->height)
                    : QVariant(resolutionText(meta->width, meta->height));
    case FpsColumn:
        if (!meta || !meta->isValid())
            return sort ? QVariant(-1.0) : QVariant();
        return sort ? QVariant(meta->fps.toDouble()) : QVariant(fpsText(meta->fps));
    default:
        return {};
    }
}

bool ProjectLibraryModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || index.column() != NameColumn)
        return false;
    const ProjectEntry e = m_entries.value(index.row());
    const QString name = value.toString().trimmed();
    if (name == e.name)
        return false;
    // Renamed after the editor has closed: the library's changed() resets the model, which
    // must not happen while the view is still committing the editor's data.
    QTimer::singleShot(0, this, [this, path = e.path, name] {
        QString error;
        if (m_library->rename(path, name, &error).isEmpty())
            emit operationFailed(error);
    });
    return true;
}

QVariant ProjectLibraryModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case NameColumn:
        return tr("Name");
    case ModifiedColumn:
        return tr("Date Modified");
    case DurationColumn:
        return tr("Duration");
    case ResolutionColumn:
        return tr("Resolution");
    case FpsColumn:
        return tr("Frame Rate");
    case KindColumn:
        return tr("Kind");
    case LocationColumn:
        return tr("Location");
    default:
        return {};
    }
}

Qt::ItemFlags ProjectLibraryModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return m_recent ? Qt::NoItemFlags : Qt::ItemIsDropEnabled; // drop into the shown folder
    const ProjectEntry& e = m_entries[index.row()];
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (e.exists) {
        f |= Qt::ItemIsDragEnabled;
        if (index.column() == NameColumn && !m_library->isOpenOrContainsOpen(e.path))
            f |= Qt::ItemIsEditable;
    }
    if (e.isFolder && !m_recent)
        f |= Qt::ItemIsDropEnabled;
    return f;
}

QStringList ProjectLibraryModel::mimeTypes() const
{
    return {QLatin1String(kMimeType), QStringLiteral("text/uri-list")};
}

QMimeData* ProjectLibraryModel::mimeData(const QModelIndexList& indexes) const
{
    QStringList paths;
    QList<QUrl> urls;
    for (const QModelIndex& i : indexes) {
        if (i.column() != NameColumn)
            continue;
        const QString p = m_entries.value(i.row()).path;
        if (!p.isEmpty() && !paths.contains(p)) {
            paths << p;
            urls << QUrl::fromLocalFile(p);
        }
    }
    auto* data = new QMimeData;
    data->setData(QLatin1String(kMimeType), paths.join(QLatin1Char('\n')).toUtf8());
    data->setUrls(urls); // a drop on a file manager copies the project file
    return data;
}

Qt::DropActions ProjectLibraryModel::supportedDropActions() const
{
    return Qt::MoveAction | Qt::CopyAction;
}

Qt::DropActions ProjectLibraryModel::supportedDragActions() const
{
    return Qt::MoveAction | Qt::CopyAction;
}

bool ProjectLibraryModel::canDropMimeData(
    const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent) const
{
    if (m_recent)
        return false;
    if (parent.isValid() && !m_entries.value(parent.row()).isFolder)
        return false;
    return data->hasFormat(QLatin1String(kMimeType)) || platform::hasDroppedFiles(data);
}

bool ProjectLibraryModel::dropMimeData(
    const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent)
{
    if (!canDropMimeData(data, action, row, column, parent))
        return false;
    const QString target = parent.isValid() ? m_entries.value(parent.row()).path : m_folder;
    const QStringList paths = data->hasFormat(QLatin1String(kMimeType))
        ? QString::fromUtf8(data->data(QLatin1String(kMimeType)))
              .split(QLatin1Char('\n'), Qt::SkipEmptyParts)
        : platform::droppedLocalFiles(data);
    return dropPaths(paths, target) > 0;
}

int ProjectLibraryModel::dropPaths(const QStringList& paths, const QString& folder)
{
    int handled = 0;
    QStringList errors;
    for (const QString& p : paths) {
        QString error;
        QString result;
        if (m_library->contains(p)) {
            if (QDir::cleanPath(p) == QDir::cleanPath(folder))
                continue; // dropped on itself
            result = m_library->move(p, folder, &error);
        } else {
            // A copy (single-file projects hold their media): on a worker, with Cancel.
            bool cancelled = false;
            ProjectLibrary* library = m_library;
            runWithProgress(QApplication::activeWindow(), tr("Import Project"),
                tr("Copying %1…").arg(QFileInfo(p).fileName()), [&](const TaskProgress& progress) {
                    result = library->importProject(p, folder, &error, progress, &cancelled);
                });
            if (cancelled)
                break;
        }
        if (result.isEmpty())
            errors << error;
        else
            ++handled;
    }
    if (!errors.isEmpty())
        emit operationFailed(errors.join(QLatin1Char('\n')));
    return handled;
}

// ---- ProjectSortProxy ----

ProjectSortProxy::ProjectSortProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setSortRole(ProjectLibraryModel::SortRole);
    setSortCaseSensitivity(Qt::CaseInsensitive);
    setSortLocaleAware(true);
    // Metadata arriving must not reshuffle the cards while the user looks at them, except
    // for the columns that need it (duration, resolution, frame rate re-sort on arrival).
    setDynamicSortFilter(true);
}

void ProjectSortProxy::setSearchText(const QString& text)
{
    if (text == m_search)
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_search = text;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    m_search = text;
    invalidateFilter();
#endif
}

bool ProjectSortProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (m_search.isEmpty())
        return true;
    const QModelIndex i
        = sourceModel()->index(sourceRow, ProjectLibraryModel::NameColumn, sourceParent);
    return i.data(Qt::EditRole).toString().contains(m_search, Qt::CaseInsensitive);
}

bool ProjectSortProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    const bool lf = left.data(ProjectLibraryModel::IsFolderRole).toBool();
    const bool rf = right.data(ProjectLibraryModel::IsFolderRole).toBool();
    if (lf != rf) {
        // Folders first in both orders: Qt reverses lessThan() for a descending sort.
        return sortOrder() == Qt::AscendingOrder ? lf : rf;
    }
    const QVariant a = left.data(sortRole());
    const QVariant b = right.data(sortRole());
    if (a.typeId() == QMetaType::QString || b.typeId() == QMetaType::QString) {
        const int c = QString::localeAwareCompare(a.toString().toLower(), b.toString().toLower());
        if (c != 0)
            return c < 0;
    } else if (a != b) {
        return QVariant::compare(a, b) == QPartialOrdering::Less;
    }
    // Ties (same date, both unloaded): by name, so the order is stable.
    const QString an = left.siblingAtColumn(0).data(Qt::EditRole).toString();
    const QString bn = right.siblingAtColumn(0).data(Qt::EditRole).toString();
    return QString::localeAwareCompare(an.toLower(), bn.toLower()) < 0;
}

} // namespace kk
