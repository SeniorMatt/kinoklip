// SPDX-License-Identifier: GPL-3.0-or-later
// ColorPickOverlay implementation: activation from SelectionModel::colorPicker(), the pick
// render (the clip's picture before the target effect) and the undoable write.
#include "ui/viewer/ColorPickOverlay.h"

#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "ui/common/AppContext.h"
#include "ui/common/SchemeColors.h"
#include "ui/viewer/ViewerSurface.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

/// Half size (frame pixels) of the area a click averages: 3 x 3.
constexpr int kClickRadius = 1;
/// Mouse travel (widget px) below which a press-release is a click, not a drag.
constexpr int kDragThreshold = 3;

} // namespace

ColorPickOverlay::ColorPickOverlay(AppContext& ctx, ViewerSurface* surface)
    : QWidget(surface->widget())
    , m_ctx(ctx)
    , m_surface(surface)
{
    setObjectName(QStringLiteral("ColorPickOverlay"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::CrossCursor);
    surface->widget()->installEventFilter(this);
    setGeometry(surface->widget()->rect());
    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &ColorPickOverlay::refresh);
    connect(timeline, &Timeline::playheadChanged, this, &ColorPickOverlay::refresh);
    if (ctx.selection)
        connect(
            ctx.selection, &SelectionModel::colorPickerChanged, this, &ColorPickOverlay::refresh);
    hide();
}

void ColorPickOverlay::refresh()
{
    bool active = false;
    if (m_ctx.selection && m_ctx.selection->colorPicker().isActive()) {
        const ColorPickTarget& t = m_ctx.selection->colorPicker();
        const TimelineData& data = m_ctx.project->timeline()->data();
        const auto loc = editops::findClip(data, t.clipId);
        if (loc && loc->track.type == TrackType::Video) {
            const ClipData& clip = data.track(loc->track)->clips[size_t(loc->clipIndex)];
            const bool hasEffect = std::any_of(clip.effects.cbegin(), clip.effects.cend(),
                [&](const EffectInstance& e) { return e.id == t.effectId; });
            active = hasEffect && clip.contains(m_ctx.project->timeline()->playhead());
        }
    }
    if (active != m_active) {
        m_active = active;
        m_dragging = false;
        setAttribute(Qt::WA_TransparentForMouseEvents, !active);
    }
    setVisible(active);
    if (active)
        raise();
    update();
}

bool ColorPickOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, event);
}

void ColorPickOverlay::paintEvent(QPaintEvent*)
{
    if (!m_active || !m_dragging)
        return;
    QPainter p(this);
    QPen pen(schemeColors().selection, 1, Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRect(m_pressPos, m_currentPos).normalized());
}

void ColorPickOverlay::mousePressEvent(QMouseEvent* event)
{
    if (!m_active || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    emit pressed();
    setFocus(Qt::MouseFocusReason);
    m_dragging = true;
    m_pressPos = m_currentPos = event->position().toPoint();
    event->accept();
}

void ColorPickOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging)
        return;
    m_currentPos = event->position().toPoint();
    update();
}

void ColorPickOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_dragging || event->button() != Qt::LeftButton)
        return;
    m_dragging = false;
    m_currentPos = event->position().toPoint();
    update();
    pick(m_pressPos, m_currentPos);
}

void ColorPickOverlay::keyPressEvent(QKeyEvent* event)
{
    if (m_active && event->key() == Qt::Key_Escape) {
        m_dragging = false;
        m_ctx.selection->setColorPicker({});
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

QImage ColorPickOverlay::renderSource(int64_t frame) const
{
    const ColorPickTarget target = m_ctx.selection->colorPicker();
    // The clip's picture as it enters the target effect: that effect and the ones after it
    // switched off in a private copy of the timeline.
    auto copy = std::make_shared<TimelineData>(*m_ctx.project->timeline()->snapshot());
    if (ClipData* clip = editops::clipById(*copy, target.clipId)) {
        bool after = false;
        for (EffectInstance& e : clip->effects) {
            after = after || e.id == target.effectId;
            if (after)
                e.enabled = false;
        }
    }
    if (m_source)
        return m_source(copy, frame);
    if (!m_ctx.playback || !m_ctx.playback->compositor())
        return {};
    RenderRequest rq;
    rq.timeline = copy;
    rq.media = m_ctx.playback->mediaPaths();
    rq.proxies = m_ctx.playback->proxies();
    rq.frame = frame;
    rq.outputs = OutputImage;
    rq.subtitles = false;
    const RenderedFramePtr result = m_ctx.playback->compositor()->renderAsync(rq).get();
    return result && result->ok ? result->image : QImage();
}

void ColorPickOverlay::pick(QPoint a, QPoint b)
{
    const ColorPickTarget target = m_ctx.selection->colorPicker();
    const TimelineData& data = m_ctx.project->timeline()->data();
    const QRect pic = m_surface->pictureRect();
    if (!target.isActive() || pic.isEmpty() || data.width <= 0 || data.height <= 0)
        return;
    const QImage image = renderSource(m_ctx.project->timeline()->playhead());
    if (image.isNull())
        return;
    // Widget → image pixels through the viewer's picture rectangle (Fit or zoom).
    const double sx = double(image.width()) / pic.width();
    const double sy = double(image.height()) / pic.height();
    auto toImage = [&](QPoint p) {
        return QPoint(
            int(std::floor((p.x() - pic.x()) * sx)), int(std::floor((p.y() - pic.y()) * sy)));
    };
    QRect area;
    if ((b - a).manhattanLength() < kDragThreshold) {
        const QPoint c = toImage(a);
        area
            = QRect(c - QPoint(kClickRadius, kClickRadius), c + QPoint(kClickRadius, kClickRadius));
    } else {
        area = QRect(toImage(a), toImage(b)).normalized();
    }
    const QColor color = averageColor(image, area);
    if (!color.isValid())
        return;

    m_ctx.project->beginEdit(tr("Pick Color"));
    if (ClipData* clip
        = editops::clipById(m_ctx.project->timeline()->mutableData(), target.clipId)) {
        for (EffectInstance& e : clip->effects) {
            if (e.id != target.effectId)
                continue;
            const EffectDescriptor* fx = EffectRegistry::instance().find(e.type);
            const ParamDescriptor* param = fx ? fx->param(target.key) : nullptr;
            if (param && param->isScalar()) {
                // A hue parameter (HSL Key): the picked colour's hue, at the playhead (a key
                // when the parameter is animated, else the constant value).
                const double hue = std::max(0.0, double(color.hslHueF())) * 360.0;
                AnimatedValue& value = e.params.animated(target.key, param->def);
                if (value.isAnimated())
                    value.setKey(clip->localFrame(m_ctx.project->timeline()->playhead()), hue);
                else
                    e.params.setBase(target.key, hue);
                continue;
            }
            e.params.setStatic(target.key, color);
            // An effect with a preset choice (Chroma Key's Screen) switches to the picked
            // colour.
            const EffectDescriptor* d = EffectRegistry::instance().find(e.type);
            const ParamDescriptor* screen = d ? d->param(fxkeys::Screen) : nullptr;
            if (screen && screen->type == ParamType::Choice
                && screen->choices.contains(fxchoice::Custom))
                e.params.setStatic(fxkeys::Screen, fxchoice::Custom);
        }
    }
    m_ctx.project->timeline()->notifyChanged();
    m_ctx.project->endEdit();
    m_ctx.selection->setColorPicker({}); // one pick per button press
}

QColor ColorPickOverlay::averageColor(const QImage& source, const QRect& area)
{
    const QImage image = source.format() == QImage::Format_ARGB32_Premultiplied
        ? source
        : source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QRect r = area.intersected(image.rect());
    double sum[3] = {0, 0, 0};
    double weight = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x) {
            const QRgb px = line[x];
            // Premultiplied sums divided by the alpha sum = the alpha-weighted mean of the
            // straight colours (transparent pixels drop out).
            sum[0] += qRed(px);
            sum[1] += qGreen(px);
            sum[2] += qBlue(px);
            weight += qAlpha(px);
        }
    }
    if (weight <= 0)
        return {};
    auto channel
        = [&](double v) { return std::clamp(int(std::lround(255.0 * v / weight)), 0, 255); };
    return QColor(channel(sum[0]), channel(sum[1]), channel(sum[2]));
}

} // namespace kk
