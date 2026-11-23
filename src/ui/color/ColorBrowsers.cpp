// SPDX-License-Identifier: GPL-3.0-or-later
// ColorBrowsers — gallery, clips strip and mini timeline of the Color page.
#include "ui/color/ColorBrowsers.h"

#include "core/ColorMatch.h"
#include "core/EditOps.h"
#include "core/GalleryStillFiles.h"
#include "core/GradeGallery.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "render/GradeLut.h"
#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorPalettes.h"
#include "ui/color/ColorSession.h"
#include "ui/common/InputLutMenu.h"
#include "ui/common/SchemeColors.h"
#include "ui/dialogs/ClipAttributesDialog.h"
#include "ui/common/TimecodeFormat.h"

#include <QApplication>
#include <QDialog>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QScopedValueRollback>
#include <QFileDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <vector>

namespace kk {

namespace {

constexpr QSize kStillIcon(112, 63);
constexpr int kClipRole = Qt::UserRole;
constexpr int kTrackRole = Qt::UserRole + 1;
constexpr int kStartRole = Qt::UserRole + 2;
constexpr int kGradedRole = Qt::UserRole + 3;
constexpr int kCurrentRole = Qt::UserRole + 4;
constexpr int kNumberRole = Qt::UserRole + 5;

/// Paints a clips strip entry: number and track over the thumbnail, timecode below; the
/// clip being graded outlined, a mark on graded clips, selected clips tinted.
class ClipDelegate : public QStyledItemDelegate
{
public:
    explicit ClipDelegate(const ColorClipStrip* strip)
        : QStyledItemDelegate(const_cast<ColorClipStrip*>(strip))
    { }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        const int line = option.fontMetrics.height();
        return {ColorFrameSource::kThumbSize.width() + 8,
            ColorFrameSource::kThumbSize.height() + line + 6};
    }

    void paint(
        QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect.adjusted(2, 2, -2, -2);
        const QPalette& pal = option.palette;
        const bool selected = option.state & QStyle::State_Selected;
        const bool current = index.data(kCurrentRole).toBool();
        const int line = option.fontMetrics.height();
        p->fillRect(r,
            selected ? withAlpha(pal.color(QPalette::Highlight), 70)
                     : pal.color(QPalette::Base).darker(108));
        // Header: number and timecode on the left, track on the right.
        const QRect head(r.left() + 2, r.top(), r.width() - 4, line);
        p->setPen(pal.color(QPalette::Text));
        const QString number = index.data(kNumberRole).toString();
        p->drawText(head, Qt::AlignLeft | Qt::AlignVCenter, number);
        const int numberWidth = option.fontMetrics.horizontalAdvance(number + QStringLiteral("  "));
        p->setPen(pal.color(QPalette::PlaceholderText));
        p->drawText(head.adjusted(numberWidth, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
            index.data(kStartRole).toString());
        p->drawText(head, Qt::AlignRight | Qt::AlignVCenter, index.data(kTrackRole).toString());
        const QRect thumb(r.left() + (r.width() - ColorFrameSource::kThumbSize.width()) / 2,
            r.top() + line + 1, ColorFrameSource::kThumbSize.width(),
            ColorFrameSource::kThumbSize.height());
        p->fillRect(thumb, QColor(14, 14, 16));
        const QImage img = index.data(Qt::DecorationRole).value<QImage>();
        if (!img.isNull()) {
            const QSize s = img.size().scaled(thumb.size(), Qt::KeepAspectRatio);
            p->drawImage(QRect(thumb.center() - QPoint(s.width() / 2, s.height() / 2), s), img);
        }
        if (index.data(kGradedRole).toBool()) {
            // A small dot: this clip has a grade.
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(90, 200, 120));
            p->drawEllipse(QPointF(thumb.right() - 6, thumb.top() + 6), 3.5, 3.5);
        }
        if (selected) {
            // Selected clips (the targets of Copy / Paste Grade) are framed, so a Shift / Ctrl
            // selection can be seen; the current clip's playhead-coloured frame sits inside.
            p->setPen(QPen(pal.color(QPalette::Highlight), 2));
            p->setBrush(Qt::NoBrush);
            p->drawRect(QRectF(r).adjusted(1, 1, -1, -1));
        }
        if (current) {
            p->setPen(QPen(schemeColors().playhead, 2));
            p->setBrush(Qt::NoBrush);
            p->drawRect(QRectF(thumb).adjusted(-1, -1, 1, 1));
        }
        p->restore();
    }
};

} // namespace

// ---- ColorGallery ----

namespace {

constexpr int kSourceRole = Qt::UserRole;
constexpr int kAlbumRole = Qt::UserRole + 1;

/// A small read-only picture of a node graph: boxes at the node positions, lines along the
/// inputs, the output node outlined (Display Node Graph).
class NodeGraphPreview : public QWidget
{
public:
    explicit NodeGraphPreview(const ClipGrade& grade, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_grade(grade)
    {
        setObjectName(QStringLiteral("GalleryNodeGraph"));
        setMinimumSize(360, 200);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().color(QPalette::Base));
        if (m_grade.isEmpty()) {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(rect(), Qt::AlignCenter, tr("No nodes"));
            return;
        }
        QRectF bounds;
        for (const GradeNode& n : m_grade.nodes)
            bounds = bounds.united(QRectF(n.pos, QSizeF(1, 1)));
        const QSizeF box(46, 26);
        const QRectF area = QRectF(rect()).adjusted(
            box.width(), box.height(), -box.width(), -box.height());
        const double sx = bounds.width() > 0 ? area.width() / bounds.width() : 0;
        const double sy = bounds.height() > 0 ? area.height() / bounds.height() : 0;
        const double s = sx > 0 && sy > 0 ? std::min(sx, sy) : std::max(sx, sy);
        auto center = [&](const GradeNode& n) {
            return QPointF(area.left() + (bounds.width() > 0 ? (n.pos.x() - bounds.left()) * s : area.width() / 2),
                area.top() + (bounds.height() > 0 ? (n.pos.y() - bounds.top()) * s : area.height() / 2));
        };
        const QColor line = palette().color(QPalette::Mid);
        p.setPen(QPen(line, 1.5));
        for (const GradeNode& n : m_grade.nodes) {
            for (const quint64 in : n.inputs) {
                const GradeNode* from = m_grade.node(in);
                const QPointF to = center(n) - QPointF(box.width() / 2, 0);
                const QPointF start = from ? center(*from) + QPointF(box.width() / 2, 0)
                                           : QPointF(8, to.y());
                p.drawLine(start, to);
            }
        }
        for (int i = 0; i < m_grade.nodes.size(); ++i) {
            const GradeNode& n = m_grade.nodes[i];
            const QRectF r(center(n) - QPointF(box.width() / 2, box.height() / 2), box);
            p.setBrush(palette().color(QPalette::Button));
            p.setPen(QPen(n.id == m_grade.output ? palette().color(QPalette::Highlight)
                                                 : palette().color(QPalette::Mid),
                n.id == m_grade.output ? 2.5 : 1.0));
            p.drawRoundedRect(r, 3, 3);
            p.setPen(n.enabled ? palette().color(QPalette::ButtonText)
                               : palette().color(QPalette::PlaceholderText));
            p.drawText(r, Qt::AlignCenter, gradeNodeName(m_grade, i));
        }
    }

private:
    ClipGrade m_grade;
};

} // namespace

ColorGallery::ColorGallery(ColorSession* session, Project* project, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_project(project)
{
    setObjectName(QStringLiteral("ColorGalleryPanel"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_albums = new QTreeWidget(this);
    m_albums->setObjectName(QStringLiteral("GalleryAlbums"));
    m_albums->setHeaderHidden(true);
    m_albums->setIndentation(12);
    m_albums->setRootIsDecorated(false);
    m_albums->setMinimumWidth(70);
    m_albums->setMaximumWidth(150);
    m_albums->setContextMenuPolicy(Qt::CustomContextMenu);
    m_albums->viewport()->setAcceptDrops(true);
    m_albums->viewport()->installEventFilter(this);
    m_albums->setToolTip(tr("The albums: Stills and its albums, PowerGrade (the same in every "
                            "project) and Memories; right-click for more, drop stills on an album"));
    layout->addWidget(m_albums);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("ColorGallery"));
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(kStillIcon);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setSpacing(4);
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setToolTip(tr("Double-click a still to wipe the viewer with it, middle-click to "
                          "apply its grade; right-click for more"));
    m_list->viewport()->installEventFilter(this);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_list, 1);

    // As in Resolve a double-click shows the still as the viewer's reference wipe (again: off);
    // the middle button applies its grade.
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const quint64 id = item->data(Qt::UserRole).toULongLong();
        if (m_source == Source::Memories) {
            if (m_session)
                m_session->applyMemory(int(id));
            return;
        }
        setWipeStill(m_wipeStill == id ? 0 : id);
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = m_list->itemAt(pos);
        QMenu* menu = nullptr;
        if (!item) {
            if (m_source == Source::Memories)
                return;
            menu = buildAlbumMenu(m_source, m_albumId, this);
        } else if (m_source == Source::Memories) {
            menu = buildMemoryMenu(int(item->data(Qt::UserRole).toULongLong()), this);
        } else {
            menu = buildMenu(item->data(Qt::UserRole).toULongLong(), this);
        }
        if (!menu)
            return;
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(m_list->viewport()->mapToGlobal(pos));
    });
    connect(m_albums, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (m_refreshing)
            return;
        const auto items = m_albums->selectedItems();
        if (items.isEmpty())
            return;
        m_source = Source(items.first()->data(0, kSourceRole).toInt());
        m_albumId = items.first()->data(0, kAlbumRole).toULongLong();
        refresh();
    });
    connect(m_albums, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = m_albums->itemAt(pos);
        if (!item)
            return;
        QMenu* menu = buildAlbumMenu(
            Source(item->data(0, kSourceRole).toInt()), item->data(0, kAlbumRole).toULongLong(), this);
        if (!menu)
            return;
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(m_albums->viewport()->mapToGlobal(pos));
    });
    auto changed = [this] {
        refreshAlbums();
        refresh();
    };
    connect(project->gallery(), &GradeGallery::changed, this, changed);
    connect(project->powerGrades()->gallery(), &GradeGallery::changed, this, changed);
    connect(project, &Project::projectReset, this, changed);
    refreshAlbums();
    refresh();
}

GradeGallery* ColorGallery::gallery(Source source) const
{
    return source == Source::PowerGrade ? m_project->powerGrades()->gallery() : m_project->gallery();
}

GradeGallery* ColorGallery::galleryOf(quint64 id) const
{
    return gallery((id & kPowerGradeBit) ? Source::PowerGrade : Source::Project);
}

const GalleryStill* ColorGallery::stillFor(quint64 id) const
{
    return id ? galleryOf(id)->still(plainId(id)) : nullptr;
}

quint64 ColorGallery::currentStill() const
{
    if (m_source == Source::Memories)
        return 0;
    QListWidgetItem* item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toULongLong() : 0;
}

void ColorGallery::setCurrentStill(quint64 id)
{
    if (m_source == Source::Memories)
        return;
    // A still of another album: show that album first.
    if (const GalleryStill* s = stillFor(id)) {
        const Source src = (id & kPowerGradeBit) ? Source::PowerGrade : Source::Project;
        if (src != m_source || s->album != m_albumId)
            selectAlbum(src, s->album);
    }
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toULongLong() == id)
            m_list->setCurrentRow(i);
    }
}

void ColorGallery::setWipeStill(quint64 id)
{
    if (id && !stillFor(id))
        id = 0;
    if (id == m_wipeStill)
        return;
    m_wipeStill = id;
    refresh();
    emit wipeStillChanged(id);
}

void ColorGallery::selectAlbum(Source source, quint64 album)
{
    if (source == Source::Memories)
        album = 0;
    else if (!gallery(source)->album(album))
        album = GradeGallery::kDefaultAlbum;
    m_source = source;
    m_albumId = album;
    refreshAlbums();
    refresh();
}

quint64 ColorGallery::newAlbum(Source source, const QString& name)
{
    if (source == Source::Memories)
        return 0;
    const quint64 id = gallery(source)->addAlbum(name);
    selectAlbum(source, id);
    return id;
}

quint64 ColorGallery::copyStillTo(quint64 id, Source source, quint64 album)
{
    if (source == Source::Memories || !stillFor(id))
        return 0;
    GradeGallery* dst = gallery(source);
    const quint64 copy = galleryOf(id)->copyStillTo(*dst, plainId(id), album);
    return copy && source == Source::PowerGrade ? (copy | kPowerGradeBit) : copy;
}

bool ColorGallery::moveStillTo(quint64 id, quint64 album)
{
    return stillFor(id) && galleryOf(id)->moveStill(plainId(id), album);
}

quint64 ColorGallery::dropStill(quint64 id, Source source, quint64 album)
{
    if (source == Source::Memories || !stillFor(id))
        return 0;
    const Source from = (id & kPowerGradeBit) ? Source::PowerGrade : Source::Project;
    if (from == source)
        return moveStillTo(id, album) ? id : 0;
    return copyStillTo(id, source, album);
}

quint64 ColorGallery::grabStill()
{
    if (!m_session)
        return 0;
    const quint64 id = m_session->grabStill();
    if (!id)
        return 0;
    if (m_source != Source::PowerGrade) {
        if (m_source == Source::Project)
            m_project->gallery()->moveStill(id, m_albumId);
        setCurrentStill(id);
        return id;
    }
    // Grabbed into a PowerGrade album: the project keeps no copy.
    const quint64 copy = copyStillTo(id, Source::PowerGrade, m_albumId);
    m_project->gallery()->removeStill(id);
    setCurrentStill(copy);
    return copy;
}

int ColorGallery::applyStill(quint64 id, const QVector<quint64>& clips, bool append)
{
    const GalleryStill* s = stillFor(id);
    if (!s || !m_session)
        return 0;
    return m_session->applyGrade(
        s->grade, clips, append ? tr("Append Node Graph") : tr("Apply Grade"), append);
}

quint64 ColorGallery::importStill(const QString& path, QString* error)
{
    GalleryStill still;
    if (!gallerystills::importStill(path, &still, error))
        return 0;
    const Source into = m_source == Source::Memories ? Source::Project : m_source;
    const quint64 album = m_source == Source::Memories ? GradeGallery::kDefaultAlbum : m_albumId;
    const quint64 id = gallery(into)->addStillCopy(still, album);
    return into == Source::PowerGrade ? (id | kPowerGradeBit) : id;
}

bool ColorGallery::exportStill(quint64 id, const QString& path, QString* error)
{
    const GalleryStill* s = stillFor(id);
    if (!s) {
        if (error)
            *error = tr("No such still");
        return false;
    }
    return gallerystills::exportStill(*s, path, true, error);
}

bool ColorGallery::deleteStill(quint64 id)
{
    if (!stillFor(id))
        return false;
    if (m_wipeStill == id)
        setWipeStill(0);
    return galleryOf(id)->removeStill(plainId(id));
}

void ColorGallery::displayNodeGraph(quint64 id)
{
    const GalleryStill* s = stillFor(id);
    if (!s)
        return;
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("GalleryNodeGraphDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Node Graph of Still %1").arg(s->name));
    auto* box = new QVBoxLayout(dialog);
    box->addWidget(new NodeGraphPreview(s->grade, dialog));
    dialog->show();
}

QVector<quint64> ColorGallery::targetsOf(quint64 id) const
{
    QVector<quint64> ids;
    for (QListWidgetItem* item : m_list->selectedItems())
        ids.append(item->data(Qt::UserRole).toULongLong());
    if (!ids.contains(id))
        ids = {id};
    return ids;
}

void ColorGallery::importInto(const QStringList& paths)
{
    quint64 last = 0;
    QStringList errors;
    for (const QString& path : paths) {
        QString error;
        if (const quint64 id = importStill(path, &error))
            last = id;
        else
            errors.append(error);
    }
    if (last)
        setCurrentStill(last);
    if (!errors.isEmpty())
        QMessageBox::warning(this, tr("Import Still"), errors.join(QLatin1Char('\n')));
}

void ColorGallery::importDialog()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Import Still"), {},
        tr("Stills (*.png *.jpg *.jpeg *.tif *.tiff *.dpx *.exr *.bmp *.webp *.%1);;All files (*)")
            .arg(gallerystills::kGradeSuffix));
    if (!paths.isEmpty())
        importInto(paths);
}

void ColorGallery::exportDialog(quint64 id)
{
    const GalleryStill* s = stillFor(id);
    if (!s)
        return;
    QStringList filters;
    for (const QString& suffix : gallerystills::exportSuffixes())
        filters.append(tr("%1 picture (*.%2)").arg(suffix.toUpper(), suffix));
    filters.append(tr("Kinoklip grade file (*.%1)").arg(gallerystills::kGradeSuffix));
    QString selected = filters.first();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Still"),
        s->name + QStringLiteral(".png"), filters.join(QStringLiteral(";;")), &selected);
    if (path.isEmpty())
        return;
    QString error;
    if (!exportStill(id, path, &error))
        QMessageBox::warning(this, tr("Export Still"), error);
}

QMenu* ColorGallery::buildAlbumMenu(Source source, quint64 album, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    if (source == Source::Memories)
        return menu;
    QAction* add = menu->addAction(tr("New Album…"));
    add->setObjectName(QStringLiteral("GalleryNewAlbum"));
    connect(add, &QAction::triggered, this, [this, source] {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("New Album"), tr("Name"), QLineEdit::Normal, tr("Album"), &ok);
        if (ok && !name.trimmed().isEmpty())
            newAlbum(source, name.trimmed());
    });
    QAction* rename = menu->addAction(tr("Rename Album…"));
    rename->setObjectName(QStringLiteral("GalleryRenameAlbum"));
    connect(rename, &QAction::triggered, this, [this, source, album] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename Album"), tr("Name"),
            QLineEdit::Normal, gallery(source)->albumName(album), &ok);
        if (ok)
            gallery(source)->renameAlbum(album, name);
    });
    QAction* remove = menu->addAction(tr("Delete Album"));
    remove->setObjectName(QStringLiteral("GalleryDeleteAlbum"));
    remove->setEnabled(album != GradeGallery::kDefaultAlbum);
    connect(remove, &QAction::triggered, this, [this, source, album] {
        GradeGallery* g = gallery(source);
        if (!g->stillsIn(album).isEmpty()
            && QMessageBox::question(this, tr("Delete Album"),
                   tr("Delete the album \"%1\" and its stills?").arg(g->albumName(album)))
                != QMessageBox::Yes)
            return;
        if (m_wipeStill && stillFor(m_wipeStill) && stillFor(m_wipeStill)->album == album
            && (((m_wipeStill & kPowerGradeBit) != 0) == (source == Source::PowerGrade)))
            setWipeStill(0);
        g->removeAlbum(album);
    });
    menu->addSeparator();
    QAction* import = menu->addAction(tr("Import Still…"));
    import->setObjectName(QStringLiteral("GalleryImport"));
    connect(import, &QAction::triggered, this, &ColorGallery::importDialog);
    return menu;
}

QMenu* ColorGallery::buildMemoryMenu(int index, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    const QString letter = GradeGallery::memoryLetter(index);
    QAction* save = menu->addAction(tr("Save Memory %1").arg(letter));
    save->setObjectName(QStringLiteral("MemorySave"));
    connect(save, &QAction::triggered, this, [this, index] {
        if (m_session)
            m_session->saveMemory(index);
    });
    const bool used = m_project->gallery()->memory(index).used;
    QAction* apply = menu->addAction(tr("Apply Memory %1").arg(letter));
    apply->setObjectName(QStringLiteral("MemoryApply"));
    apply->setEnabled(used);
    connect(apply, &QAction::triggered, this, [this, index] {
        if (m_session)
            m_session->applyMemory(index);
    });
    QAction* applySel = menu->addAction(tr("Apply Memory %1 to Selected Clips").arg(letter));
    applySel->setObjectName(QStringLiteral("MemoryApplySelected"));
    applySel->setEnabled(used && m_session && !m_session->selectedClips().isEmpty());
    connect(applySel, &QAction::triggered, this, [this, index] {
        if (m_session)
            m_session->applyMemory(index, m_session->selectedClips());
    });
    menu->addSeparator();
    QAction* clear = menu->addAction(tr("Clear Memory %1").arg(letter));
    clear->setObjectName(QStringLiteral("MemoryClear"));
    clear->setEnabled(used);
    connect(clear, &QAction::triggered, this, [this, index] { m_project->gallery()->clearMemory(index); });
    return menu;
}

QMenu* ColorGallery::buildMenu(quint64 id, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    QAction* apply = menu->addAction(tr("Apply Grade"));
    apply->setObjectName(QStringLiteral("GalleryApply"));
    connect(apply, &QAction::triggered, this, [this, id] { applyStill(id); });
    QAction* applySel = menu->addAction(tr("Apply Grade to Selected Clips"));
    applySel->setObjectName(QStringLiteral("GalleryApplySelected"));
    applySel->setEnabled(m_session && !m_session->selectedClips().isEmpty());
    connect(applySel, &QAction::triggered, this, [this, id] {
        if (m_session)
            applyStill(id, m_session->selectedClips());
    });
    QAction* append = menu->addAction(tr("Append Node Graph"));
    append->setObjectName(QStringLiteral("GalleryAppend"));
    connect(append, &QAction::triggered, this, [this, id] {
        applyStill(id, m_session ? m_session->selectedClips() : QVector<quint64> {}, true);
    });
    QAction* graph = menu->addAction(tr("Display Node Graph"));
    graph->setObjectName(QStringLiteral("GalleryDisplayGraph"));
    connect(graph, &QAction::triggered, this, [this, id] { displayNodeGraph(id); });
    menu->addSeparator();
    QAction* wipe = menu->addAction(tr("Wipe with This Still"));
    wipe->setObjectName(QStringLiteral("GalleryWipe"));
    wipe->setCheckable(true);
    wipe->setChecked(m_wipeStill == id);
    connect(wipe, &QAction::toggled, this, [this, id](bool on) { setWipeStill(on ? id : 0); });
    menu->addSeparator();

    // Move to another album of the same list, copy to any album (also the other list).
    const Source from = (id & kPowerGradeBit) ? Source::PowerGrade : Source::Project;
    const GalleryStill* still = stillFor(id);
    QMenu* move = menu->addMenu(tr("Move to Album"));
    move->setObjectName(QStringLiteral("GalleryMoveMenu"));
    for (const GalleryAlbum& a : gallery(from)->albums()) {
        if (still && a.id == still->album)
            continue;
        QAction* act = move->addAction(a.name);
        act->setObjectName(QStringLiteral("GalleryMove_%1").arg(a.id));
        const quint64 albumId = a.id;
        connect(act, &QAction::triggered, this, [this, id, albumId] {
            for (const quint64 t : targetsOf(id))
                moveStillTo(t, albumId);
        });
    }
    move->setEnabled(!move->isEmpty());
    QMenu* copy = menu->addMenu(tr("Copy to Album"));
    copy->setObjectName(QStringLiteral("GalleryCopyMenu"));
    for (const Source src : {Source::Project, Source::PowerGrade}) {
        copy->addSection(src == Source::Project ? tr("Stills") : tr("PowerGrade"));
        for (const GalleryAlbum& a : gallery(src)->albums()) {
            QAction* act = copy->addAction(a.name);
            act->setObjectName(QStringLiteral("GalleryCopy_%1%2")
                                   .arg(src == Source::Project ? QLatin1String("G") : QLatin1String("P"))
                                   .arg(a.id));
            const quint64 albumId = a.id;
            connect(act, &QAction::triggered, this, [this, id, src, albumId] {
                for (const quint64 t : targetsOf(id))
                    copyStillTo(t, src, albumId);
            });
        }
    }
    menu->addSeparator();
    QAction* import = menu->addAction(tr("Import Still…"));
    import->setObjectName(QStringLiteral("GalleryImport"));
    connect(import, &QAction::triggered, this, &ColorGallery::importDialog);
    QAction* exportAction = menu->addAction(tr("Export Still…"));
    exportAction->setObjectName(QStringLiteral("GalleryExport"));
    connect(exportAction, &QAction::triggered, this, [this, id] { exportDialog(id); });
    menu->addSeparator();
    QAction* rename = menu->addAction(tr("Rename…"));
    rename->setObjectName(QStringLiteral("GalleryRename"));
    connect(rename, &QAction::triggered, this, [this, id] {
        const GalleryStill* s = stillFor(id);
        if (!s)
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename Still"), tr("Name"), QLineEdit::Normal, s->name, &ok);
        if (ok && !name.trimmed().isEmpty())
            galleryOf(id)->renameStill(plainId(id), name.trimmed());
    });
    QAction* remove = menu->addAction(tr("Delete Still"));
    remove->setObjectName(QStringLiteral("GalleryDelete"));
    connect(remove, &QAction::triggered, this, [this, id] {
        for (const quint64 t : targetsOf(id))
            deleteStill(t);
    });
    return menu;
}

bool ColorGallery::eventFilter(QObject* watched, QEvent* event)
{
    // Middle-click on a still (or a memory) applies its grade (Resolve habit).
    if (watched == m_list->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::MiddleButton) {
            if (QListWidgetItem* item = m_list->itemAt(me->position().toPoint())) {
                const quint64 id = item->data(Qt::UserRole).toULongLong();
                if (m_source == Source::Memories) {
                    if (m_session)
                        m_session->applyMemory(int(id));
                } else if (m_session) {
                    applyStill(id);
                }
                return true;
            }
        }
    }
    // Stills dragged onto an album of the list on the left.
    if (watched == m_albums->viewport()) {
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto* de = static_cast<QDragMoveEvent*>(event);
            QTreeWidgetItem* item = m_albums->itemAt(de->position().toPoint());
            if (de->source() == m_list && m_source != Source::Memories && item
                && Source(item->data(0, kSourceRole).toInt()) != Source::Memories) {
                de->setDropAction(Qt::CopyAction);
                de->accept();
            } else {
                de->ignore();
            }
            return true;
        }
        case QEvent::Drop: {
            auto* de = static_cast<QDropEvent*>(event);
            QTreeWidgetItem* item = m_albums->itemAt(de->position().toPoint());
            if (de->source() == m_list && m_source != Source::Memories && item) {
                const Source src = Source(item->data(0, kSourceRole).toInt());
                const quint64 album = item->data(0, kAlbumRole).toULongLong();
                QVector<quint64> ids;
                for (QListWidgetItem* it : m_list->selectedItems())
                    ids.append(it->data(Qt::UserRole).toULongLong());
                for (const quint64 id : std::as_const(ids))
                    dropStill(id, src, album);
                de->setDropAction(Qt::CopyAction);
                de->accept();
            } else {
                de->ignore();
            }
            return true;
        }
        default: break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ColorGallery::refreshAlbums()
{
    const QScopedValueRollback guard(m_refreshing, true);
    m_albums->clear();
    QTreeWidgetItem* selected = nullptr;
    auto make = [this, &selected](QTreeWidgetItem* parent, Source src, quint64 album,
                    const QString& name) {
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_albums);
        item->setText(0, name);
        item->setData(0, kSourceRole, int(src));
        item->setData(0, kAlbumRole, album);
        if (src == m_source && album == m_albumId)
            selected = item;
        return item;
    };
    for (const Source src : {Source::Project, Source::PowerGrade}) {
        const GradeGallery* g = gallery(src);
        QTreeWidgetItem* top = nullptr;
        for (const GalleryAlbum& a : g->albums()) {
            if (a.id == GradeGallery::kDefaultAlbum)
                top = make(nullptr, src, a.id, a.name);
        }
        for (const GalleryAlbum& a : g->albums()) {
            if (a.id != GradeGallery::kDefaultAlbum)
                make(top, src, a.id, a.name);
        }
        top->setExpanded(true);
    }
    make(nullptr, Source::Memories, 0, tr("Memories"));
    if (!selected) {
        m_source = Source::Project;
        m_albumId = GradeGallery::kDefaultAlbum;
        selected = m_albums->topLevelItem(0);
    }
    m_albums->setCurrentItem(selected);
    selected->setSelected(true);
}

void ColorGallery::refresh()
{
    const QScopedValueRollback guard(m_refreshing, true);
    const quint64 current = m_source == Source::Memories ? 0 : currentStill();
    const int currentRow = m_list->currentRow();
    m_list->clear();
    if (m_wipeStill && !stillFor(m_wipeStill)) {
        m_wipeStill = 0;
        emit wipeStillChanged(0);
    }
    if (m_source == Source::Memories) {
        const GradeGallery* g = m_project->gallery();
        for (int i = 0; i < kGalleryMemoryCount; ++i) {
            const GalleryMemory& m = g->memory(i);
            auto* item = new QListWidgetItem(GradeGallery::memoryLetter(i), m_list);
            item->setData(Qt::UserRole, i);
            QPixmap pix(kStillIcon);
            pix.fill(palette().color(QPalette::AlternateBase));
            QPainter p(&pix);
            if (m.used && !m.image.isNull()) {
                const QSize sz = m.image.size().scaled(kStillIcon, Qt::KeepAspectRatio);
                p.drawImage(QRect(QPoint((kStillIcon.width() - sz.width()) / 2,
                                    (kStillIcon.height() - sz.height()) / 2),
                                sz),
                    m.image);
            } else if (m.used) {
                p.setPen(palette().color(QPalette::Text));
                p.drawText(pix.rect(), Qt::AlignCenter, tr("%n node(s)", nullptr, int(m.grade.nodes.size())));
            }
            p.end();
            item->setIcon(QIcon(pix));
            item->setToolTip(m.used ? tr("Memory %1: %n node(s)", nullptr, int(m.grade.nodes.size()))
                                          .arg(GradeGallery::memoryLetter(i))
                                    : tr("Memory %1 is empty; right-click to save the current grade%2")
                                          .arg(GradeGallery::memoryLetter(i),
                                              i < kGalleryKeyedMemories
                                                  ? tr(" (Alt+%1)").arg(i + 1)
                                                  : QString()));
            if (!m.used)
                item->setForeground(palette().color(QPalette::PlaceholderText));
        }
        m_list->setToolTip(tr("Memories A to Z hold a grade each: Alt+1 to 8 save the current "
                              "grade into A to H, Ctrl+1 to 8 apply them; middle-click or "
                              "double-click a memory to apply it, right-click for more"));
        m_list->setCurrentRow(std::max(0, currentRow));
        return;
    }
    m_list->setToolTip(tr("Double-click a still to wipe the viewer with it, middle-click to "
                          "apply its grade; right-click for more"));
    const bool powerGrade = m_source == Source::PowerGrade;
    for (const GalleryStill& s : gallery(m_source)->stillsIn(m_albumId)) {
        const quint64 id = powerGrade ? (s.id | kPowerGradeBit) : s.id;
        QString text = s.name;
        if (id == m_wipeStill)
            text += QStringLiteral(" ") + tr("(wipe)");
        auto* item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, id);
        item->setToolTip(
            tr("Still %1: %n node(s)", nullptr, int(s.grade.nodes.size())).arg(s.name));
        if (!s.image.isNull()) {
            item->setIcon(QIcon(QPixmap::fromImage(
                s.image.scaled(kStillIcon, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
        }
    }
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toULongLong() == current)
            m_list->setCurrentRow(i);
    }
}

// ---- ColorClipStrip ----

ColorClipStrip::ColorClipStrip(
    ColorSession* session, Project* project, ColorFrameSource* frames, QWidget* parent)
    : QListWidget(parent)
    , m_session(session)
    , m_project(project)
    , m_frames(frames)
{
    setObjectName(QStringLiteral("ColorClipStrip"));
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setWrapping(false);
    setMovement(QListView::Static);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setItemDelegate(new ClipDelegate(this));
    setToolTip(tr("Click a clip to grade it; Ctrl / Shift click selects several; "
                  "middle-click copies a clip's grade to the selected clips"));
    const int line = fontMetrics().height();
    setFixedHeight(ColorFrameSource::kThumbSize.height() + line + 6 + 2 * frameWidth()
        + style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 6);
    connect(project->timeline(), &Timeline::changed, this, &ColorClipStrip::rebuild);
    connect(project, &Project::projectReset, this, &ColorClipStrip::rebuild);
    if (m_session) {
        connect(m_session, &ColorSession::clipChanged, this, &ColorClipStrip::updateCurrent);
        connect(m_session, &ColorSession::selectedClipsChanged, this, [this] {
            if (m_updating)
                return;
            m_updating = true;
            for (int i = 0; i < count(); ++i)
                item(i)->setSelected(m_session->selectedClips().contains(clipAt(i)));
            m_updating = false;
        });
    }
    if (m_frames) {
        connect(m_frames, &ColorFrameSource::thumbnailChanged, this, [this](quint64 id) {
            const int row = rowOf(id);
            if (row >= 0)
                item(row)->setData(Qt::DecorationRole, m_frames->thumbnail(id));
        });
    }
    connect(this, &QListWidget::itemSelectionChanged, this, &ColorClipStrip::syncSelection);
    rebuild();
}

int ColorClipStrip::rowOf(quint64 clipId) const
{
    return int(m_clips.indexOf(clipId));
}

quint64 ColorClipStrip::clipAt(int row) const
{
    return row >= 0 && row < m_clips.size() ? m_clips[row] : 0;
}

void ColorClipStrip::rebuild()
{
    if (!m_session)
        return;
    const QVector<quint64> clips = m_session->stripClips();
    const TimelineData& data = m_project->timeline()->data();
    m_updating = true;
    if (clips != m_clips) {
        m_clips = clips;
        clear();
        for (int i = 0; i < clips.size(); ++i)
            new QListWidgetItem(this);
    }
    for (int i = 0; i < m_clips.size(); ++i) {
        const auto loc = editops::findClip(data, m_clips[i]);
        if (!loc)
            continue;
        const ClipData& c = data.track(loc->track)->clips[size_t(loc->clipIndex)];
        QListWidgetItem* it = item(i);
        it->setData(kClipRole, c.id);
        it->setData(kNumberRole, QString::number(i + 1).rightJustified(2, QLatin1Char('0')));
        it->setData(kTrackRole, QStringLiteral("V%1").arg(loc->track.index + 1));
        it->setData(kStartRole, timecode::formatRecord(c.start, data));
        it->setData(kGradedRole, !c.grade.isEmpty());
        it->setToolTip(c.name.isEmpty() ? tr("Clip %1").arg(i + 1) : c.name);
        if (m_frames)
            it->setData(Qt::DecorationRole, m_frames->thumbnail(c.id));
        it->setSelected(m_session->selectedClips().contains(c.id));
    }
    m_updating = false;
    updateCurrent();
}

void ColorClipStrip::updateCurrent()
{
    const quint64 current = m_session ? m_session->clipId() : 0;
    for (int i = 0; i < count(); ++i)
        item(i)->setData(kCurrentRole, clipAt(i) == current);
    if (const int row = rowOf(current); row >= 0)
        scrollToItem(item(row));
}

void ColorClipStrip::syncSelection()
{
    if (m_updating || !m_session)
        return;
    QVector<quint64> ids;
    for (int i = 0; i < count(); ++i) {
        if (item(i)->isSelected())
            ids.append(clipAt(i));
    }
    m_updating = true;
    m_session->setSelectedClips(ids);
    m_updating = false;
}

void ColorClipStrip::mousePressEvent(QMouseEvent* event)
{
    QListWidgetItem* it = itemAt(event->position().toPoint());
    const quint64 clip = it ? clipAt(row(it)) : 0;
    if (event->button() == Qt::MiddleButton) {
        // Copy that clip's grade to the selected clips (or the current one).
        if (clip && m_session) {
            QVector<quint64> targets = m_session->selectedClips();
            targets.removeAll(clip);
            m_session->copyGrade(clip, targets);
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        // The right-click menu acts on the clicked clip and the selection as it is: pressing
        // must not replace the selection (Copy Grade to Selected Clips would lose its targets).
        event->accept();
        return;
    }
    QListWidget::mousePressEvent(event);
    if (event->button() == Qt::LeftButton && clip && m_session
        && !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)))
        m_session->gradeClip(clip);
}

void ColorClipStrip::contextMenuEvent(QContextMenuEvent* event)
{
    QListWidgetItem* it = itemAt(event->pos());
    if (!it || !m_session)
        return;
    const quint64 clip = clipAt(row(it));
    QMenu menu(this);
    QAction* grade = menu.addAction(tr("Grade This Clip"));
    QVector<quint64> others = m_session->selectedClips();
    others.removeAll(clip);
    QAction* copy = menu.addAction(tr("Copy Grade to Selected Clips"));
    copy->setEnabled(!others.isEmpty());
    if (others.isEmpty())
        copy->setToolTip(tr("Ctrl- or Shift-click the clips that should get this clip's grade"));
    QAction* paste = menu.addAction(tr("Paste Grade"));
    paste->setEnabled(m_session->hasCopiedGrade());
    QAction* shot = menu.addAction(tr("Shot Match to This Clip"));
    shot->setEnabled(!others.isEmpty());
    shot->setToolTip(others.isEmpty()
            ? tr("Ctrl- or Shift-click the clips that should be matched to this one")
            : tr("Match the selected clips' balance and contrast to this clip"));
    QAction* reset = menu.addAction(tr("Reset Grade"));
    menu.addSeparator();
    QMenu* lutMenu = menu.addMenu(tr("Generate 3D LUT (CUBE)"));
    lutMenu->setObjectName(QStringLiteral("GenerateLutMenu"));
    lutMenu->setEnabled(!m_project->clipGrade(m_project->activeTimelineUid(), clip).isEmpty());
    for (const int size : render::kGradeLutSizes) {
        QAction* a = lutMenu->addAction(tr("%1 Point Cube").arg(size));
        a->setData(size);
    }
    // Resolve's right-click > 3D LUT: the media's input LUT, applied before the node graph.
    const ClipData* clipData = editops::clipById(m_project->timeline()->data(), clip);
    QMenu* inputMenu = menu.addMenu(tr("3D LUT"));
    inputMenu->setObjectName(QStringLiteral("InputLutMenu"));
    const MediaItem* mediaItem = clipData && clipData->kind == ClipKind::Media
        ? m_project->mediaPool()->item(clipData->mediaId)
        : nullptr;
    if (mediaItem) {
        const QString mediaId = mediaItem->id;
        inputlut::populate(inputMenu, mediaItem->attributes.inputLut, this,
            [this, mediaId](const QString& path) {
                clipattributes::setInputLut(*m_project, {mediaId}, path);
            });
    } else {
        inputMenu->setEnabled(false);
    }
    QAction* chosen = menu.exec(event->globalPos());
    if (chosen && chosen->parent() == lutMenu) {
        generateLut(clip, chosen->data().toInt());
        return;
    }
    if (chosen == grade) {
        m_session->gradeClip(clip);
    } else if (chosen == copy) {
        m_session->copyGrade(clip, others);
    } else if (chosen == paste) {
        m_session->pasteGrade(m_session->selectedClips().isEmpty() ? QVector<quint64> {clip}
                                                                   : m_session->selectedClips());
    } else if (chosen == shot) {
        shotMatch(clip, others);
    } else if (chosen == reset) {
        m_project->setClipGrade(
            m_project->activeTimelineUid(), clip, ClipGrade {}, tr("Reset Grade"));
    }
}

int ColorClipStrip::shotMatch(quint64 target, const QVector<quint64>& sources)
{
    if (!m_frames || !m_session || !m_project)
        return 0;
    auto statsOf = [this](quint64 clip) {
        const QImage image = m_frames->thumbnail(clip).convertToFormat(QImage::Format_ARGB32);
        std::vector<float> rgb;
        rgb.reserve(size_t(image.width()) * size_t(image.height()) * 3);
        for (int y = 0; y < image.height(); ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                rgb.push_back(float(qRed(line[x])) / 255.0f);
                rgb.push_back(float(qGreen(line[x])) / 255.0f);
                rgb.push_back(float(qBlue(line[x])) / 255.0f);
            }
        }
        return colormatch::shotStats(rgb.data(), rgb.size() / 3);
    };
    const colormatch::ShotStats wanted = statsOf(target);
    if (!wanted.valid)
        return 0;
    const double strength = m_session->shotMatchStrength();
    const quint64 uid = m_project->activeTimelineUid();
    int changed = 0;
    m_project->beginEdit(tr("Shot Match"));
    for (const quint64 clip : sources) {
        if (clip == target)
            continue;
        const colormatch::ShotStats have = statsOf(clip);
        if (!have.valid)
            continue;
        ClipGrade grade = m_project->clipGrade(uid, clip);
        if (!colormatch::applyShotMatch(grade, colormatch::shotMatch(have, wanted, strength)))
            continue;
        if (m_project->setClipGrade(uid, clip, grade, tr("Shot Match")))
            ++changed;
    }
    m_project->endEdit();
    return changed;
}

void ColorClipStrip::generateLut(quint64 clipId, int size)
{
    const ClipData* c = editops::clipById(m_project->timeline()->data(), clipId);
    QString base = c ? c->name : QString();
    if (base.isEmpty())
        base = tr("grade");
    base.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    const QString dir = LutBrowser::defaultFolder();
    const QString path = QFileDialog::getSaveFileName(this, tr("Generate 3D LUT (CUBE)"),
        dir + QLatin1Char('/') + base + QStringLiteral(".cube"), tr("Cube LUT (*.cube)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!m_session->generateLut(clipId, size, path, &error))
        QMessageBox::warning(this, tr("Generate 3D LUT (CUBE)"), error);
}

// ---- ColorMiniTimeline ----

ColorMiniTimeline::ColorMiniTimeline(ColorSession* session, Project* project, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_project(project)
{
    setObjectName(QStringLiteral("ColorMiniTimeline"));
    setToolTip(tr("Click or drag to move the playhead"));
    Timeline* t = project->timeline();
    connect(t, &Timeline::changed, this, qOverload<>(&QWidget::update));
    connect(t, &Timeline::playheadChanged, this, qOverload<>(&QWidget::update));
    if (m_session)
        connect(m_session, &ColorSession::clipChanged, this, qOverload<>(&QWidget::update));
}

int64_t ColorMiniTimeline::length() const
{
    return std::max<int64_t>(1, m_project->timeline()->data().duration());
}

double ColorMiniTimeline::xOf(int64_t frame) const
{
    return 4.0 + double(frame) / double(length()) * std::max(1, width() - 8);
}

int64_t ColorMiniTimeline::frameAt(double x) const
{
    const double t = (x - 4.0) / std::max(1, width() - 8);
    return std::clamp<int64_t>(int64_t(std::floor(t * double(length()))), 0, length() - 1);
}

void ColorMiniTimeline::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const SchemeColors& sc = schemeColors();
    p.fillRect(rect(), palette().color(QPalette::Base).darker(110));
    const TimelineData& data = m_project->timeline()->data();
    const int tracks = std::max<int>(1, int(data.videoTracks.size()));
    const double laneH = std::max(4.0, (height() - 6.0) / tracks);
    const quint64 current = m_session ? m_session->clipId() : 0;
    for (int t = 0; t < int(data.videoTracks.size()); ++t) {
        // Highest track on top, like the Edit page.
        const double y = 3 + (tracks - 1 - t) * laneH;
        for (const ClipData& c : data.videoTracks[size_t(t)].clips) {
            const QRectF r(
                xOf(c.start), y + 1, std::max(1.0, xOf(c.end()) - xOf(c.start) - 1), laneH - 2);
            QColor fill = c.kind == ClipKind::Media ? sc.videoClip : sc.solidClip;
            if (!data.videoTracks[size_t(t)].enabled)
                fill = fill.darker(160);
            p.fillRect(r, fill);
            if (!c.grade.isEmpty())
                p.fillRect(QRectF(r.left(), r.bottom() - 2, r.width(), 2), QColor(90, 200, 120));
            if (c.id == current) {
                p.setPen(QPen(sc.playhead, 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
            }
        }
    }
    const double x = xOf(m_project->timeline()->playhead());
    p.setPen(QPen(sc.playhead, 1.5));
    p.drawLine(QPointF(x, 0), QPointF(x, height()));
}

void ColorMiniTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    m_project->timeline()->setPlayhead(frameAt(event->position().x()));
}

void ColorMiniTimeline::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging)
        m_project->timeline()->setPlayhead(frameAt(event->position().x()));
}

void ColorMiniTimeline::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = false;
}

} // namespace kk
