// SPDX-License-Identifier: GPL-3.0-or-later
// GuideOverlay — drawing of the viewer guides, safe areas and grid, and the guide gestures.
#include "ui/viewer/GuideOverlay.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/SchemeColors.h"
#include "ui/viewer/ViewerOverlaySettings.h"
#include "ui/viewer/ViewerSurface.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr char kSourceGuidesKey[] = "viewer/sourceGuides"; ///< JSON array (guides::toJson)
constexpr int kHaloAlpha = 150; ///< dark line under the guides (readable over any picture)
constexpr int kSafeAlpha = 200; ///< safe area outlines
constexpr int kGridAlpha = 110; ///< grid lines (quieter than the guides)
constexpr int kDimAlpha = 120; ///< outside the aspect marker
constexpr int kRemoveAlpha = 110; ///< a guide that would be removed if released now
constexpr double kCentreMarkHalf = 12.0; ///< half size of the centre cross (widget px)
constexpr int kLabelMargin = 6; ///< distance of the drag position label from the pointer

QList<GuideOverlay*>& instances()
{
    static QList<GuideOverlay*> list;
    return list;
}
bool g_suppressed = false;

double along(Qt::Orientation o, QPointF framePoint)
{
    // A horizontal guide sits at a y position, a vertical one at an x position.
    return o == Qt::Horizontal ? framePoint.y() : framePoint.x();
}
double extentOf(Qt::Orientation o, QSize frame)
{
    return o == Qt::Horizontal ? frame.height() : frame.width();
}
} // namespace

GuideOverlay::GuideOverlay(AppContext& ctx, ViewerSurface* surface, bool timeline)
    : QWidget(surface->widget())
    , m_ctx(ctx)
    , m_surface(surface)
    , m_timeline(timeline)
{
    setObjectName(QStringLiteral("GuideOverlay"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents); // input comes through eventFilter()
    setFocusPolicy(Qt::ClickFocus); // Esc during a drag
    instances().append(this);
    QWidget* host = surface->widget();
    host->setMouseTracking(true); // hover moves without a button reach eventFilter()
    host->installEventFilter(this);
    for (QObject* child : host->children())
        watch(child);
    if (m_timeline && ctx.project) {
        connect(ctx.project->timeline(), &Timeline::changed, this, &GuideOverlay::refresh);
    } else {
        m_sourceGuides = guides::fromJson(QJsonDocument::fromJson(
            QSettings().value(QLatin1String(kSourceGuidesKey)).toByteArray())
                                              .array());
    }
    connect(&ViewerOverlaySettings::instance(), &ViewerOverlaySettings::changed, this,
        &GuideOverlay::refresh);
    setGeometry(host->rect());
    lower(); // below the transform / mask controls: they draw on top of the guides
    refresh();
}

GuideOverlay::~GuideOverlay()
{
    instances().removeAll(this);
}

void GuideOverlay::watch(QObject* child)
{
    // The surface's other overlays take the mouse over the picture; filtering their events
    // lets a guide win over them within kGrabPx without changing their code.
    if (child != this && child->isWidgetType())
        child->installEventFilter(this);
}

void GuideOverlay::setSuppressed(bool suppressed)
{
    g_suppressed = suppressed;
    for (GuideOverlay* o : instances())
        o->refresh();
}

bool GuideOverlay::isSuppressed()
{
    return g_suppressed;
}

std::vector<ViewerGuide> GuideOverlay::storedGuides() const
{
    if (m_timeline)
        return m_ctx.project ? m_ctx.project->timeline()->data().guides
                             : std::vector<ViewerGuide> {};
    return m_sourceGuides;
}

std::vector<ViewerGuide> GuideOverlay::guides() const
{
    std::vector<ViewerGuide> list = storedGuides();
    if (m_drag) {
        if (m_drag->index >= 0 && m_drag->index < int(list.size()))
            list[size_t(m_drag->index)] = m_drag->guide;
        else if (m_drag->index < 0)
            list.push_back(m_drag->guide);
    }
    return list;
}

void GuideOverlay::storeSourceGuides(const std::vector<ViewerGuide>& list)
{
    m_sourceGuides = list;
    QSettings().setValue(QLatin1String(kSourceGuidesKey),
        QString::fromUtf8(QJsonDocument(guides::toJson(list)).toJson(QJsonDocument::Compact)));
}

void GuideOverlay::setGuides(const std::vector<ViewerGuide>& list, const QString& undoLabel)
{
    if (list == storedGuides())
        return;
    if (m_timeline) {
        if (!m_ctx.project)
            return;
        // Document change: one undo step, marks the project modified, saved per timeline.
        m_ctx.project->beginEdit(undoLabel);
        m_ctx.project->timeline()->mutableData().guides = list;
        m_ctx.project->endEdit(); // Timeline::changed -> refresh()
        return;
    }
    storeSourceGuides(list);
    refresh();
}

QSize GuideOverlay::frameSize() const
{
    if (m_timeline && m_ctx.project) {
        const TimelineData& d = m_ctx.project->timeline()->data();
        return QSize(d.width, d.height);
    }
    return m_frameSize.isEmpty() ? QSize(1920, 1080) : m_frameSize;
}

void GuideOverlay::setFrameSize(QSize size)
{
    if (size == m_frameSize)
        return;
    m_frameSize = size;
    refresh();
}

QTransform GuideOverlay::frameToWidget() const
{
    const QRect pic = m_surface->pictureRect();
    const QSize frame = frameSize();
    if (pic.isEmpty() || frame.isEmpty())
        return {};
    return QTransform::fromScale(
               double(pic.width()) / frame.width(), double(pic.height()) / frame.height())
        * QTransform::fromTranslate(pic.x(), pic.y());
}

bool GuideOverlay::mouseEditable() const
{
    const ViewerOverlayOptions& o = ViewerOverlaySettings::instance().options();
    return o.showGuides && !o.lockGuides && !g_suppressed;
}

int GuideOverlay::guideAt(QPointF pos, bool editableOnly) const
{
    const ViewerOverlayOptions& o = ViewerOverlaySettings::instance().options();
    if (!o.showGuides || g_suppressed || (editableOnly && o.lockGuides))
        return -1;
    const QTransform map = frameToWidget();
    if (map.m11() <= 0 || map.m22() <= 0)
        return -1;
    // Only over the picture (guides are drawn there): outside it the surface keeps its own
    // gestures (source drag) and the controls theirs.
    if (!map.mapRect(QRectF(QPointF(0, 0), QSizeF(frameSize())))
             .adjusted(-kGrabPx, -kGrabPx, kGrabPx, kGrabPx)
             .contains(pos))
        return -1;
    const QPointF f = map.inverted().map(pos);
    // Tolerance in frame px per axis: kGrabPx widget px. The axes scale equally (the
    // picture keeps its aspect), so one tolerance serves both.
    return guides::nearestGuide(storedGuides(), f.x(), f.y(), kGrabPx / map.m11());
}

void GuideOverlay::addGuide(Qt::Orientation orientation, std::optional<double> position)
{
    ViewerOverlayOptions o = ViewerOverlaySettings::instance().options();
    if (!o.showGuides) {
        o.showGuides = true; // a guide the user just added must be visible
        ViewerOverlaySettings::instance().setOptions(o);
    }
    std::vector<ViewerGuide> list = storedGuides();
    list.push_back({orientation, position.value_or(extentOf(orientation, frameSize()) / 2)});
    setGuides(list, tr("Add Guide"));
}

void GuideOverlay::removeGuide(int index)
{
    std::vector<ViewerGuide> list = storedGuides();
    if (index < 0 || index >= int(list.size()))
        return;
    list.erase(list.begin() + index);
    setGuides(list, tr("Remove Guide"));
}

void GuideOverlay::clearGuides()
{
    setGuides({}, tr("Clear Guides"));
}

void GuideOverlay::applyPreset(const GuidePreset& preset)
{
    ViewerOverlayOptions o = ViewerOverlaySettings::instance().options();
    if (!o.showGuides) {
        o.showGuides = true;
        ViewerOverlaySettings::instance().setOptions(o);
    }
    setGuides(guides::presetGuides(preset, QSizeF(frameSize())), tr("Load Guide Preset"));
}

void GuideOverlay::editGuidePosition(int index)
{
    const std::vector<ViewerGuide> list = storedGuides();
    if (index < 0 || index >= int(list.size()))
        return;
    const ViewerGuide g = list[size_t(index)];
    const double extent = extentOf(g.orientation, frameSize());
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("GuidePositionDialog"));
    dialog.setWindowTitle(
        g.orientation == Qt::Horizontal ? tr("Horizontal Guide") : tr("Vertical Guide"));
    auto* form = new QFormLayout(&dialog);
    auto* value = new QDoubleSpinBox(&dialog);
    value->setObjectName(QStringLiteral("GuidePosition"));
    auto* unit = new QComboBox(&dialog);
    unit->setObjectName(QStringLiteral("GuideUnit"));
    unit->addItem(tr("px"));
    unit->addItem(tr("%"));
    // Switching the unit converts the value in the box, so the guide does not jump.
    auto applyUnit = [value, extent](int u, double px) {
        const QSignalBlocker block(value);
        if (u == 0) {
            value->setDecimals(1);
            value->setRange(0, extent);
            value->setValue(px);
        } else {
            value->setDecimals(2);
            value->setRange(0, 100);
            value->setValue(extent > 0 ? px * 100 / extent : 0);
        }
    };
    applyUnit(0, g.position);
    int current = 0;
    connect(unit, &QComboBox::currentIndexChanged, &dialog,
        [value, extent, applyUnit, &current](int u) {
            const double px = current == 0 ? value->value() : value->value() * extent / 100;
            current = u;
            applyUnit(u, px);
        });
    auto* row = new QHBoxLayout;
    row->addWidget(value, 1);
    row->addWidget(unit);
    form->addRow(
        g.orientation == Qt::Horizontal ? tr("Distance from top:") : tr("Distance from left:"),
        row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    std::vector<ViewerGuide> changed = storedGuides();
    if (index >= int(changed.size()))
        return; // the timeline changed meanwhile (undo from another window)
    changed[size_t(index)].position
        = unit->currentIndex() == 0 ? value->value() : value->value() * extent / 100;
    setGuides(changed, tr("Set Guide Position"));
}

// ---- Drags ----

void GuideOverlay::beginNewGuide(Qt::Orientation orientation, QPointF pos)
{
    if (!mouseEditable())
        return;
    m_drag = Drag {-1, {orientation, 0.0}, true};
    updateDrag(pos);
    setFocus();
}

void GuideOverlay::beginMove(int index, QPointF pos)
{
    const std::vector<ViewerGuide> list = storedGuides();
    if (!mouseEditable() || index < 0 || index >= int(list.size()))
        return;
    m_drag = Drag {index, list[size_t(index)], false};
    updateDrag(pos);
    setFocus();
}

void GuideOverlay::updateDrag(QPointF pos)
{
    if (!m_drag)
        return;
    const QTransform map = frameToWidget();
    if (map.m11() <= 0)
        return;
    const Qt::Orientation o = m_drag->guide.orientation;
    // Whole frame px while dragging (the dialog takes fractions): what the rulers count.
    const double p = std::round(along(o, map.inverted().map(pos)));
    m_drag->guide.position = p;
    // Released outside the surface (on a ruler, outside the viewer) or with the guide off the
    // picture, the guide goes away.
    m_drag->remove = !QRectF(rect()).contains(pos) || p < 0 || p > extentOf(o, frameSize());
    update();
    emit changed();
}

void GuideOverlay::dragTo(QPointF pos)
{
    updateDrag(pos);
}

void GuideOverlay::finishDrag(QPointF pos, bool commit)
{
    if (!m_drag)
        return;
    if (commit)
        updateDrag(pos);
    const Drag drag = *m_drag;
    m_drag.reset();
    std::vector<ViewerGuide> list = storedGuides();
    if (commit) {
        if (drag.index < 0) {
            if (!drag.remove) {
                list.push_back(drag.guide);
                setGuides(list, tr("Add Guide"));
            }
        } else if (drag.index < int(list.size())) {
            if (drag.remove) {
                list.erase(list.begin() + drag.index);
                setGuides(list, tr("Remove Guide"));
            } else {
                list[size_t(drag.index)] = drag.guide;
                setGuides(list, tr("Move Guide"));
            }
        }
    }
    update();
    emit changed();
}

std::vector<double> GuideOverlay::snapTargets(Qt::Orientation orientation) const
{
    const ViewerOverlayOptions& o = ViewerOverlaySettings::instance().options();
    if (!o.showGuides || !o.snapToGuides || g_suppressed)
        return {};
    return guides::positions(storedGuides(), orientation);
}

void GuideOverlay::refresh()
{
    if (parentWidget() && geometry() != parentWidget()->rect())
        setGeometry(parentWidget()->rect());
    setVisible(!g_suppressed);
    update();
    emit changed();
}

// ---- Events ----

void GuideOverlay::keyPressEvent(QKeyEvent* event)
{
    if (m_drag && event->key() == Qt::Key_Escape) {
        finishDrag({}, false);
        return;
    }
    QWidget::keyPressEvent(event);
}

bool GuideOverlay::eventFilter(QObject* watched, QEvent* event)
{
    QWidget* host = parentWidget();
    if (watched == host) {
        switch (event->type()) {
        case QEvent::Resize:
            refresh();
            break;
        case QEvent::ChildAdded:
            watch(static_cast<QChildEvent*>(event)->child());
            break;
        default:
            break;
        }
    }
    if (auto* w = qobject_cast<QWidget*>(watched); w && (w == host || w->parentWidget() == host))
        return filterMouse(w, event) || QWidget::eventFilter(watched, event);
    return QWidget::eventFilter(watched, event);
}

QPointF GuideOverlay::toHost(QWidget* widget, QPointF pos) const
{
    // mapTo() needs a strict ancestor: the host surface maps to itself.
    return widget == parentWidget() ? pos : widget->mapTo(parentWidget(), pos);
}

bool GuideOverlay::filterMouse(QWidget* widget, QEvent* event)
{
    // Layers that cover the picture with something else (two-up trim view, edit tiles while
    // dragging media) or own every click while active (the eyedropper): guides under them
    // are not grabbed.
    static const QStringList kExclusiveLayers {QStringLiteral("ColorPickOverlay"),
        QStringLiteral("TrimTwoUpView"), QStringLiteral("EditOverlay")};
    if (widget != parentWidget() && kExclusiveLayers.contains(widget->objectName()))
        return false;
    const QEvent::Type type = event->type();
    if (type == QEvent::ContextMenu) {
        auto* ctx = static_cast<QContextMenuEvent*>(event);
        const int index = guideAt(toHost(widget, QPointF(ctx->pos())), false);
        if (index < 0)
            return false;
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("GuideContextMenu"));
        QAction* edit = menu.addAction(tr("Edit Guide Position…"));
        QAction* remove = menu.addAction(tr("Remove Guide"));
        const bool editable = mouseEditable();
        edit->setEnabled(editable);
        remove->setEnabled(editable);
        QAction* chosen = menu.exec(ctx->globalPos());
        if (chosen == edit)
            editGuidePosition(index);
        else if (chosen == remove)
            removeGuide(index);
        return true;
    }
    if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress
        && type != QEvent::MouseButtonRelease && type != QEvent::MouseButtonDblClick)
        return false;
    auto* mouse = static_cast<QMouseEvent*>(event);
    const QPointF pos = toHost(widget, mouse->position());
    switch (type) {
    case QEvent::MouseMove:
        if (m_drag) {
            updateDrag(pos);
            return true;
        }
        if (mouse->buttons() == Qt::NoButton) {
            const int index = guideAt(pos);
            if (index >= 0) {
                const bool horizontal = storedGuides()[size_t(index)].orientation == Qt::Horizontal;
                widget->setCursor(horizontal ? Qt::SplitVCursor : Qt::SplitHCursor);
                m_cursorWidget = widget;
                return true; // the controls below must not replace the cursor
            }
            if (m_cursorWidget) {
                m_cursorWidget->unsetCursor();
                m_cursorWidget = nullptr;
            }
        }
        return false;
    case QEvent::MouseButtonPress: {
        if (mouse->button() != Qt::LeftButton)
            return false;
        const int index = guideAt(pos);
        if (index < 0)
            return false;
        emit pressed();
        beginMove(index, pos);
        return true;
    }
    case QEvent::MouseButtonRelease:
        if (m_drag && mouse->button() == Qt::LeftButton) {
            finishDrag(pos);
            return true;
        }
        return false;
    case QEvent::MouseButtonDblClick: {
        if (mouse->button() != Qt::LeftButton)
            return false;
        const int index = guideAt(pos);
        if (index < 0)
            return false;
        editGuidePosition(index);
        return true;
    }
    default:
        return false;
    }
}

// ---- Painting ----

void GuideOverlay::paintEvent(QPaintEvent*)
{
    const QTransform map = frameToWidget();
    const QSizeF frame(frameSize());
    if (map.m11() <= 0 || frame.isEmpty() || g_suppressed)
        return;
    QPainter p(this);
    const QRectF picture = map.mapRect(QRectF(QPointF(0, 0), frame));
    p.setClipRect(picture.intersected(QRectF(rect())));
    paintSafeAreas(p, map, frame);
    paintGrid(p, map, frame);
    paintGuides(p, map, frame);
}

void GuideOverlay::paintSafeAreas(QPainter& p, const QTransform& map, QSizeF frame) const
{
    const ViewerOverlayOptions& o = ViewerOverlaySettings::instance().options();
    const SchemeColors& sc = schemeColors();
    if (o.aspectMarker > 0) {
        // Dim what falls outside the chosen aspect, then outline it.
        const QRectF marker = map.mapRect(guides::aspectMarker(frame, o.aspectMarker));
        QPainterPath outside;
        outside.addRect(map.mapRect(QRectF(QPointF(0, 0), frame)));
        QPainterPath inside;
        inside.addRect(marker);
        p.fillPath(outside.subtracted(inside), withAlpha(sc.shadow, kDimAlpha));
        p.setPen(QPen(withAlpha(sc.overlayText, kSafeAlpha), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(marker.adjusted(0.5, 0.5, -0.5, -0.5));
    }
    p.setBrush(Qt::NoBrush);
    if (o.actionSafe) {
        p.setPen(QPen(withAlpha(sc.overlayText, kSafeAlpha), 1));
        p.drawRect(map.mapRect(guides::safeArea(frame, guides::kActionSafe)));
    }
    if (o.titleSafe) {
        p.setPen(QPen(withAlpha(sc.overlayText, kSafeAlpha), 1, Qt::DashLine));
        p.drawRect(map.mapRect(guides::safeArea(frame, guides::kTitleSafe)));
    }
    if (o.centreMark) {
        const QPointF c = map.map(QPointF(frame.width() / 2, frame.height() / 2));
        p.setPen(QPen(withAlpha(sc.overlayText, kSafeAlpha), 1));
        p.drawLine(c - QPointF(kCentreMarkHalf, 0), c + QPointF(kCentreMarkHalf, 0));
        p.drawLine(c - QPointF(0, kCentreMarkHalf), c + QPointF(0, kCentreMarkHalf));
    }
}

void GuideOverlay::paintGrid(QPainter& p, const QTransform& map, QSizeF frame) const
{
    const ViewerOverlayOptions& o = ViewerOverlaySettings::instance().options();
    if (!o.showGrid)
        return;
    std::vector<double> xs, ys;
    guides::gridLines(frame, o.gridDivisions, xs, ys);
    p.setPen(QPen(withAlpha(schemeColors().overlayText, kGridAlpha), 1));
    for (const double x : xs)
        p.drawLine(map.map(QPointF(x, 0)), map.map(QPointF(x, frame.height())));
    for (const double y : ys)
        p.drawLine(map.map(QPointF(0, y)), map.map(QPointF(frame.width(), y)));
}

void GuideOverlay::paintGuides(QPainter& p, const QTransform& map, QSizeF frame) const
{
    const ViewerOverlayOptions& o = ViewerOverlaySettings::instance().options();
    if (!o.showGuides)
        return;
    const QColor color = ViewerOverlaySettings::instance().guideColor();
    const QPen halo(withAlpha(schemeColors().shadow, kHaloAlpha), 3);
    const std::vector<ViewerGuide> list = guides();
    const int dragged = !m_drag ? -2 : m_drag->index >= 0 ? m_drag->index : int(list.size()) - 1;
    for (int i = 0; i < int(list.size()); ++i) {
        const ViewerGuide& g = list[size_t(i)];
        const bool removing = i == dragged && m_drag->remove;
        // Snap to whole device pixels: crisp 1 px lines at any zoom.
        const QLineF line = g.orientation == Qt::Horizontal
            ? QLineF(map.map(QPointF(0, g.position)), map.map(QPointF(frame.width(), g.position)))
            : QLineF(map.map(QPointF(g.position, 0)), map.map(QPointF(g.position, frame.height())));
        const QLineF crisp(std::floor(line.x1()) + 0.5, std::floor(line.y1()) + 0.5,
            std::floor(line.x2()) + 0.5, std::floor(line.y2()) + 0.5);
        p.setPen(halo);
        p.drawLine(crisp);
        p.setPen(removing ? QPen(withAlpha(color, kRemoveAlpha), 1, Qt::DashLine) : QPen(color, 1));
        p.drawLine(crisp);
        if (i == dragged) {
            // Position read-out next to the guide while dragging (frame px, as the rulers).
            const QString text = removing ? tr("Remove") : QString::number(g.position, 'f', 0);
            const QFontMetrics fm(font());
            const QPointF at = g.orientation == Qt::Horizontal
                ? QPointF(crisp.x1() + kLabelMargin, crisp.y1() - kLabelMargin)
                : QPointF(crisp.x1() + kLabelMargin, crisp.y1() + kLabelMargin + fm.ascent());
            p.setPen(halo);
            p.drawText(at + QPointF(1, 1), text);
            p.setPen(color);
            p.drawText(at, text);
        }
    }
}

} // namespace kk
