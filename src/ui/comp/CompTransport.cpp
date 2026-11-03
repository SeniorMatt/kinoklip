// SPDX-License-Identifier: GPL-3.0-or-later
// CompTimeRuler and CompTransport implementation.
#include "ui/comp/CompTransport.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/comp/CompSession.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr int kRulerHeight = 26;
constexpr double kRulerMarginPx = 8.0; ///< space left / right of frame 0 / the last frame
constexpr double kMinLabelSpacingPx = 70.0; ///< frame numbers at least this far apart
} // namespace

// ---- CompTimeRuler ----

CompTimeRuler::CompTimeRuler(CompSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("CompTimeRuler"));
    setMinimumHeight(kRulerHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(session, &CompSession::compositionChanged, this, qOverload<>(&QWidget::update));
    connect(session, &CompSession::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(session, &CompSession::clipChanged, this, qOverload<>(&QWidget::update));
}

QSize CompTimeRuler::sizeHint() const
{
    return QSize(400, kRulerHeight);
}

double CompTimeRuler::xForFrame(int64_t frame) const
{
    const int64_t count = m_session ? std::max<int64_t>(1, m_session->compFrameCount()) : 1;
    const double usable = std::max(1.0, width() - 2 * kRulerMarginPx);
    return kRulerMarginPx + usable * (count > 1 ? double(frame) / double(count - 1) : 0.0);
}

int64_t CompTimeRuler::frameAt(double x) const
{
    const int64_t count = m_session ? m_session->compFrameCount() : 0;
    if (count <= 1)
        return 0;
    const double usable = std::max(1.0, width() - 2 * kRulerMarginPx);
    const double f = std::round((x - kRulerMarginPx) / usable * double(count - 1));
    return std::clamp<int64_t>(int64_t(f), 0, count - 1);
}

QVector<int64_t> CompTimeRuler::keyFrames() const
{
    QVector<int64_t> out;
    if (!m_session)
        return out;
    const CompNode* n = m_session->composition().node(m_session->currentNode());
    if (!n)
        return out;
    for (const AnimatedValue& v : n->params.values)
        for (const Keyframe& k : v.keys)
            out.append(k.frame);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void CompTimeRuler::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.fillRect(rect(), sc.ruler);
    p.setPen(sc.line);
    p.drawLine(0, height() - 1, width(), height() - 1);
    if (!m_session || !m_session->clipId())
        return;
    const int64_t count = m_session->compFrameCount();
    // Label every n frames, n a 1-2-5 step that keeps labels kMinLabelSpacingPx apart.
    const double pxPerFrame
        = count > 1 ? (width() - 2 * kRulerMarginPx) / double(count - 1) : width();
    int64_t step = 1;
    for (int64_t m = 1;; m *= 10) {
        const int64_t candidates[] = {m, 2 * m, 5 * m};
        bool found = false;
        for (int64_t c : candidates) {
            if (c * pxPerFrame >= kMinLabelSpacingPx) {
                step = c;
                found = true;
                break;
            }
        }
        if (found || m > count)
            break;
    }
    p.setPen(sc.textDim);
    const QFontMetrics fm(font());
    for (int64_t f = 0; f < count; f += step) {
        const double x = xForFrame(f);
        p.drawLine(QPointF(x, height() - 8), QPointF(x, height() - 1));
        p.drawText(QPointF(x + 3, fm.ascent() + 2), QString::number(f));
    }
    // Keyframe ticks of the current node.
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(sc.keyframe);
    for (int64_t f : keyFrames()) {
        if (f < 0 || f >= count)
            continue;
        const QPointF c(xForFrame(f), height() - 6);
        p.drawPolygon(QPolygonF({c + QPointF(0, -4), c + QPointF(4, 0), c + QPointF(0, 4),
            c + QPointF(-4, 0)}));
    }
    // Playhead.
    const double x = xForFrame(m_session->compFrame());
    p.setPen(QPen(sc.playhead, 1.5));
    p.drawLine(QPointF(x, 0), QPointF(x, height()));
}

void CompTimeRuler::mousePressEvent(QMouseEvent* event)
{
    if (m_session && event->button() == Qt::LeftButton)
        m_session->seekCompFrame(frameAt(event->position().x()));
}

void CompTimeRuler::mouseMoveEvent(QMouseEvent* event)
{
    if (m_session && (event->buttons() & Qt::LeftButton))
        m_session->seekCompFrame(frameAt(event->position().x()));
}

// ---- CompTransport ----

CompTransport::CompTransport(AppContext& ctx, CompSession* session, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
    , m_session(session)
{
    setObjectName(QStringLiteral("CompTransport"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(metrics::kSpaceM, metrics::kSpaceXs, metrics::kSpaceM,
        metrics::kSpaceXs);
    layout->setSpacing(metrics::kSpaceS);
    auto button = [this, layout](const char* iconName, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(icon(QLatin1String(iconName)));
        b->setIconSize(QSize(metrics::kTransportIconSize, metrics::kTransportIconSize));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(b);
        return b;
    };
    m_stop = button("stop", tr("Stop"));
    m_stop->setObjectName(QStringLiteral("CompStop"));
    m_play = button("play", tr("Play the clip range"));
    m_play->setObjectName(QStringLiteral("CompPlay"));
    m_ruler = new CompTimeRuler(session, this);
    layout->addWidget(m_ruler, 1);
    m_position = new QLabel(this);
    m_position->setObjectName(QStringLiteral("CompPosition"));
    m_position->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("Frame 00000 / 00:00:00:00")));
    layout->addWidget(m_position);

    connect(m_play, &QToolButton::clicked, this, &CompTransport::play);
    connect(m_stop, &QToolButton::clicked, this, &CompTransport::stop);
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, &CompTransport::updatePosition);
    connect(session, &CompSession::clipChanged, this, &CompTransport::updatePosition);
    if (ctx.playback) {
        connect(ctx.playback, &PlaybackEngine::positionChanged, this,
            &CompTransport::onEnginePosition);
        connect(ctx.playback, &PlaybackEngine::rateChanged, this, [this](double rate) {
            if (rate == 0.0)
                m_playing = false;
            m_play->setIcon(icon(rate != 0.0 ? QStringLiteral("pause") : QStringLiteral("play")));
        });
    }
    updatePosition();
}

QString CompTransport::positionText() const
{
    return m_position->text();
}

void CompTransport::updatePosition()
{
    m_ruler->update();
    if (!m_session || !m_session->clipId()) {
        m_position->clear();
        return;
    }
    const Timeline* tl = m_ctx.project->timeline();
    m_position->setText(tr("Frame %1 / %2")
            .arg(m_session->compFrame())
            .arg(timecode::format(tl->playhead(), tl->data().fps)));
}

void CompTransport::play()
{
    PlaybackEngine* engine = m_ctx.playback;
    const ClipData* c = m_session ? m_session->clip() : nullptr;
    if (!c)
        return;
    if (engine && engine->isPlaying()) {
        stop();
        return;
    }
    Timeline* tl = m_ctx.project->timeline();
    if (tl->playhead() < c->start || tl->playhead() >= c->start + c->duration - 1)
        tl->setPlayhead(c->start);
    m_playing = true;
    if (engine)
        engine->play();
}

void CompTransport::stop()
{
    m_playing = false;
    if (m_ctx.playback)
        m_ctx.playback->pause();
}

void CompTransport::onEnginePosition(int64_t frame)
{
    if (!m_playing || !m_session)
        return;
    const ClipData* c = m_session->clip();
    if (!c)
        return;
    // The clip range is the composition's render range: loop back to its first frame.
    if (frame >= c->start + c->duration || frame < c->start)
        m_ctx.playback->seek(c->start);
}

} // namespace kk
