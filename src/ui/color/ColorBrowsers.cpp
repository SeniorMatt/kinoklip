// SPDX-License-Identifier: GPL-3.0-or-later
// ColorBrowsers — gallery, clips strip and mini timeline of the Color page.
#include "ui/color/ColorBrowsers.h"

#include "core/EditOps.h"
#include "core/GradeGallery.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorSession.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"

#include <QApplication>
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

ColorGallery::ColorGallery(ColorSession* session, Project* project, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_project(project)
{
    setObjectName(QStringLiteral("ColorGalleryPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("ColorGallery"));
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(kStillIcon);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setSpacing(4);
    m_list->setUniformItemSizes(true);
    m_list->setToolTip(tr("Double-click (or middle-click) a still to apply its grade; "
                          "right-click for more"));
    m_list->viewport()->installEventFilter(this);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_list, 1);

    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (m_session)
            m_session->applyStill(item->data(Qt::UserRole).toULongLong());
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = m_list->itemAt(pos);
        if (!item)
            return;
        QMenu* menu = buildMenu(item->data(Qt::UserRole).toULongLong(), this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(m_list->viewport()->mapToGlobal(pos));
    });
    connect(project->gallery(), &GradeGallery::changed, this, &ColorGallery::refresh);
    refresh();
}

quint64 ColorGallery::currentStill() const
{
    QListWidgetItem* item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toULongLong() : 0;
}

void ColorGallery::setCurrentStill(quint64 id)
{
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toULongLong() == id)
            m_list->setCurrentRow(i);
    }
}

void ColorGallery::setWipeStill(quint64 id)
{
    if (id && !m_project->gallery()->still(id))
        id = 0;
    if (id == m_wipeStill)
        return;
    m_wipeStill = id;
    refresh();
    emit wipeStillChanged(id);
}

QMenu* ColorGallery::buildMenu(quint64 id, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    QAction* apply = menu->addAction(tr("Apply Grade"));
    apply->setObjectName(QStringLiteral("GalleryApply"));
    connect(apply, &QAction::triggered, this, [this, id] {
        if (m_session)
            m_session->applyStill(id);
    });
    QAction* applySel = menu->addAction(tr("Apply Grade to Selected Clips"));
    applySel->setObjectName(QStringLiteral("GalleryApplySelected"));
    applySel->setEnabled(m_session && !m_session->selectedClips().isEmpty());
    connect(applySel, &QAction::triggered, this, [this, id] {
        if (m_session)
            m_session->applyStill(id, m_session->selectedClips());
    });
    menu->addSeparator();
    QAction* wipe = menu->addAction(tr("Wipe with This Still"));
    wipe->setObjectName(QStringLiteral("GalleryWipe"));
    wipe->setCheckable(true);
    wipe->setChecked(m_wipeStill == id);
    connect(wipe, &QAction::toggled, this, [this, id](bool on) { setWipeStill(on ? id : 0); });
    menu->addSeparator();
    QAction* rename = menu->addAction(tr("Rename…"));
    connect(rename, &QAction::triggered, this, [this, id] {
        const GalleryStill* s = m_project->gallery()->still(id);
        if (!s)
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename Still"), tr("Name"), QLineEdit::Normal, s->name, &ok);
        if (ok && !name.trimmed().isEmpty())
            m_project->gallery()->renameStill(id, name.trimmed());
    });
    QAction* remove = menu->addAction(tr("Delete Still"));
    remove->setObjectName(QStringLiteral("GalleryDelete"));
    connect(remove, &QAction::triggered, this, [this, id] {
        if (m_wipeStill == id)
            setWipeStill(0);
        m_project->gallery()->removeStill(id);
    });
    return menu;
}

bool ColorGallery::eventFilter(QObject* watched, QEvent* event)
{
    // Middle-click on a still applies its grade (Resolve habit).
    if (watched == m_list->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::MiddleButton) {
            if (QListWidgetItem* item = m_list->itemAt(me->position().toPoint())) {
                if (m_session)
                    m_session->applyStill(item->data(Qt::UserRole).toULongLong());
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ColorGallery::refresh()
{
    const quint64 current = currentStill();
    m_list->clear();
    const QVector<GalleryStill>& stills = m_project->gallery()->stills();
    if (m_wipeStill && !m_project->gallery()->still(m_wipeStill)) {
        m_wipeStill = 0;
        emit wipeStillChanged(0);
    }
    for (const GalleryStill& s : stills) {
        QString text = s.name;
        if (s.id == m_wipeStill)
            text += QStringLiteral(" ") + tr("(wipe)");
        auto* item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, s.id);
        item->setToolTip(
            tr("Still %1: %n node(s)", nullptr, int(s.grade.nodes.size())).arg(s.name));
        if (!s.image.isNull()) {
            item->setIcon(QIcon(QPixmap::fromImage(
                s.image.scaled(kStillIcon, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
        }
    }
    setCurrentStill(current);
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
        it->setData(kStartRole, timecode::format(c.start, data.fps));
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
    QAction* copy = menu.addAction(tr("Copy Grade to Selected Clips"));
    copy->setEnabled(!m_session->selectedClips().isEmpty());
    QAction* paste = menu.addAction(tr("Paste Grade"));
    paste->setEnabled(m_session->hasCopiedGrade());
    QAction* reset = menu.addAction(tr("Reset Grade"));
    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == grade) {
        m_session->gradeClip(clip);
    } else if (chosen == copy) {
        QVector<quint64> targets = m_session->selectedClips();
        targets.removeAll(clip);
        m_session->copyGrade(clip, targets);
    } else if (chosen == paste) {
        m_session->pasteGrade(m_session->selectedClips().isEmpty() ? QVector<quint64> {clip}
                                                                   : m_session->selectedClips());
    } else if (chosen == reset) {
        m_project->setClipGrade(
            m_project->activeTimelineUid(), clip, ClipGrade {}, tr("Reset Grade"));
    }
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
