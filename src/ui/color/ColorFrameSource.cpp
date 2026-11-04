// SPDX-License-Identifier: GPL-3.0-or-later
// ColorFrameSource — the input picture, node pictures, highlight and clip thumbnails.
#include "ui/color/ColorFrameSource.h"

#include "core/EditOps.h"
#include "core/GradeMath.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/GradeEval.h"
#include "render/RenderTypes.h"
#include "ui/color/ColorSession.h"

#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <memory>

namespace kk {

namespace {
/// Quiet time after a playhead or timeline change before the input is rendered again.
constexpr int kInputDelayMs = 60;
/// Quiet time before thumbnails are re-rendered (a wheel drag commits many times).
constexpr int kThumbDelayMs = 250;
} // namespace

ColorFrameSource::ColorFrameSource(
    PlaybackEngine* engine, Project* project, ColorSession* session, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_project(project)
    , m_session(session)
{
    m_inputTimer = new QTimer(this);
    m_inputTimer->setSingleShot(true);
    m_inputTimer->setInterval(kInputDelayMs);
    connect(m_inputTimer, &QTimer::timeout, this, &ColorFrameSource::renderInputNow);
    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setSingleShot(true);
    m_thumbTimer->setInterval(kThumbDelayMs);
    connect(m_thumbTimer, &QTimer::timeout, this, &ColorFrameSource::renderThumbnailsNow);
    m_watcher = new QFutureWatcher<RenderedFramePtr>(this);
    connect(m_watcher, &QFutureWatcher<RenderedFramePtr>::finished, this,
        &ColorFrameSource::onRendered);
    Timeline* timeline = project->timeline();
    connect(timeline, &Timeline::playheadChanged, this, &ColorFrameSource::refreshInput);
    connect(timeline, &Timeline::changed, this, [this] {
        refreshInput();
        refreshThumbnails();
    });
    connect(project, &Project::projectReset, this, [this] {
        m_thumbs.clear();
        refreshInput();
        refreshThumbnails();
    });
    // The timeline grade starts from another picture than a clip grade.
    if (session)
        connect(session, &ColorSession::clipChanged, this, &ColorFrameSource::refreshInput);
}

ColorFrameSource::~ColorFrameSource()
{
    m_queue.clear();
    if (m_busy)
        m_watcher->waitForFinished();
}

void ColorFrameSource::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active) {
        refreshInput();
        refreshThumbnails();
    }
}

void ColorFrameSource::setInput(const QImage& image)
{
    m_input = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    emit inputChanged();
}

void ColorFrameSource::refreshInput()
{
    if (m_active)
        m_inputTimer->start();
}

void ColorFrameSource::refreshThumbnails()
{
    if (m_active)
        m_thumbTimer->start();
}

PlaybackEngine::ImageGrades ColorFrameSource::inputGrades() const
{
    return m_session && m_session->target() == ColorSession::Target::Timeline
        ? PlaybackEngine::ImageGrades::ClipsOnly
        : PlaybackEngine::ImageGrades::None;
}

void ColorFrameSource::renderInputNow()
{
    if (!m_engine || !m_engine->compositor() || !m_active)
        return;
    if (m_engine->isPlaying()) {
        m_inputTimer->start(kInputDelayMs * 4); // try again once playback stops
        return;
    }
    const TimelineData& data = m_project->timeline()->data();
    if (data.width <= 0 || data.height <= 0)
        return;
    Job job;
    job.frame = m_project->timeline()->playhead();
    job.size = QSize(
        kInputWidth, std::max(1, int(std::lround(double(kInputWidth) * data.height / data.width))));
    job.grades = inputGrades();
    job.input = true;
    job.done = [this](const QImage& image) { setInput(image); };
    // A newer input request replaces a queued one.
    m_queue.erase(
        std::remove_if(m_queue.begin(), m_queue.end(), [](const Job& j) { return j.input; }),
        m_queue.end());
    m_queue.prepend(std::move(job));
    runNext();
}

QByteArray ColorFrameSource::signature(quint64 clipId) const
{
    const TimelineData& data = m_project->timeline()->data();
    const ClipData* c = editops::clipById(data, clipId);
    if (!c)
        return {};
    QJsonObject o = serialization::clipToJson(*c);
    o.insert(QStringLiteral("timelineGrade"), serialization::gradeToJson(data.grade));
    o.insert(QStringLiteral("w"), data.width);
    o.insert(QStringLiteral("h"), data.height);
    return QCryptographicHash::hash(
        QJsonDocument(o).toJson(QJsonDocument::Compact), QCryptographicHash::Md5);
}

void ColorFrameSource::renderThumbnailsNow()
{
    if (!m_session || !m_active)
        return;
    const QVector<quint64> clips = m_session->stripClips();
    // Forget removed clips.
    for (auto it = m_thumbs.begin(); it != m_thumbs.end();) {
        if (!clips.contains(it.key()))
            it = m_thumbs.erase(it);
        else
            ++it;
    }
    if (!m_engine || !m_engine->compositor())
        return;
    const TimelineData& data = m_project->timeline()->data();
    // The current clip first: it is the one being graded.
    QVector<quint64> order = clips;
    if (const quint64 cur = m_session->clipId(); order.removeOne(cur))
        order.prepend(cur);
    for (const quint64 id : std::as_const(order)) {
        const QByteArray sig = signature(id);
        if (m_thumbs.value(id).signature == sig)
            continue;
        const ClipData* c = editops::clipById(data, id);
        if (!c)
            continue;
        // Placeholder signature so a second refresh does not queue it again.
        m_thumbs[id].signature = sig;
        Job job;
        job.frame = c->start + std::max<int64_t>(0, c->duration / 2);
        job.size = kThumbSize;
        job.grades = PlaybackEngine::ImageGrades::All; // the strip ignores Shift+D
        job.done = [this, id, sig](const QImage& image) {
            Thumb& t = m_thumbs[id];
            if (t.signature != sig)
                return; // changed again meanwhile; a newer job follows
            t.image = image;
            emit thumbnailChanged(id);
        };
        enqueue(std::move(job));
    }
}

void ColorFrameSource::enqueue(Job job)
{
    m_queue.append(std::move(job));
    runNext();
}

void ColorFrameSource::runNext()
{
    if (m_busy || m_queue.isEmpty() || !m_engine || !m_engine->compositor())
        return;
    m_running = m_queue.takeFirst();
    m_busy = true;
    auto future = std::make_shared<std::future<RenderedFramePtr>>(
        m_engine->renderImage(m_running.frame, m_running.size, m_running.grades));
    m_watcher->setFuture(QtConcurrent::run([future] { return future->get(); }));
}

void ColorFrameSource::onRendered()
{
    m_busy = false;
    const RenderedFramePtr frame = m_watcher->result();
    Job job = std::move(m_running);
    m_running = Job {};
    if (frame && frame->ok && !frame->image.isNull() && job.done)
        job.done(frame->image);
    runNext();
}

ClipGrade ColorFrameSource::cutAt(quint64 nodeId, bool includeNode) const
{
    ClipGrade g = m_session ? m_session->grade() : ClipGrade {};
    const GradeNode* n = g.node(nodeId);
    if (!n) {
        g.output = 0;
        return g;
    }
    if (includeNode)
        g.output = nodeId;
    else
        g.output = n->inputs.isEmpty() ? 0 : n->inputs.first();
    return g;
}

QImage ColorFrameSource::nodeInput(quint64 nodeId, QSize size) const
{
    if (m_input.isNull())
        return {};
    QImage image = size.isEmpty() || size == m_input.size()
        ? m_input
        : m_input.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const ClipGrade g = cutAt(nodeId, false);
    if (g.output != 0 && m_session)
        render::applyGrade(g, m_session->gradeFrame(), image);
    return image;
}

QImage ColorFrameSource::nodeOutput(quint64 nodeId, QSize size) const
{
    if (m_input.isNull())
        return {};
    QImage image = m_input.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const ClipGrade g = cutAt(nodeId, true);
    if (g.output != 0 && m_session)
        render::applyGrade(g, m_session->gradeFrame(), image);
    return image;
}

QImage ColorFrameSource::highlight(quint64 nodeId, HighlightMode mode) const
{
    if (m_input.isNull() || !m_session)
        return {};
    const GradeNode* n = m_session->grade().node(nodeId);
    if (!n || n->isMixer())
        return {};
    const int64_t frame = m_session->gradeFrame();
    const QImage in = nodeInput(nodeId).convertToFormat(QImage::Format_ARGB32);
    QImage graded = m_input;
    {
        ClipGrade g = cutAt(nodeId, true);
        if (g.output != 0)
            render::applyGrade(g, frame, graded);
        graded = graded.convertToFormat(QImage::Format_ARGB32);
    }
    grademath::PictureGeometry geo;
    geo.width = in.width();
    geo.height = in.height();
    const std::vector<grademath::WindowValues> windows = grademath::evaluateWindows(*n, frame, geo);
    const float mix = float(std::clamp(n->value(gradekeys::Mix, frame), 0.0, 1.0));
    QImage out(in.size(), QImage::Format_ARGB32);
    for (int y = 0; y < in.height(); ++y) {
        const auto* src = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        const auto* gr = reinterpret_cast<const QRgb*>(graded.constScanLine(y));
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < in.width(); ++x) {
            const float rgb[3]
                = {qRed(src[x]) / 255.0f, qGreen(src[x]) / 255.0f, qBlue(src[x]) / 255.0f};
            float key = mix * grademath::qualifierKey(n->qualifier, rgb);
            if (!windows.empty())
                key *= grademath::windowsMatte(windows, x + 0.5f, y + 0.5f);
            key = std::clamp(key, 0.0f, 1.0f);
            if (mode == HighlightMode::BlackWhite) {
                const int v = int(std::lround(key * 255.0f));
                dst[x] = qRgb(v, v, v);
            } else {
                constexpr float grey = 0.42f * 255.0f;
                auto blend = [&](int c) { return int(std::lround(grey + (c - grey) * key)); };
                dst[x] = qRgb(blend(qRed(gr[x])), blend(qGreen(gr[x])), blend(qBlue(gr[x])));
            }
        }
    }
    return out;
}

bool ColorFrameSource::sampleNodeInput(quint64 nodeId, QPointF uv, float rgb[3]) const
{
    if (m_input.isNull())
        return false;
    const QImage in = nodeInput(nodeId).convertToFormat(QImage::Format_ARGB32);
    const int cx = std::clamp(int(uv.x() * in.width()), 0, in.width() - 1);
    const int cy = std::clamp(int(uv.y() * in.height()), 0, in.height() - 1);
    double sum[3] = {0, 0, 0};
    int count = 0;
    for (int y = std::max(0, cy - 1); y <= std::min(in.height() - 1, cy + 1); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        for (int x = std::max(0, cx - 1); x <= std::min(in.width() - 1, cx + 1); ++x) {
            sum[0] += qRed(row[x]);
            sum[1] += qGreen(row[x]);
            sum[2] += qBlue(row[x]);
            ++count;
        }
    }
    for (int i = 0; i < 3; ++i)
        rgb[i] = float(sum[i] / count / 255.0);
    return true;
}

} // namespace kk
