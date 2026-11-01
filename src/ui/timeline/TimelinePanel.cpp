// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER timeline implementation: tool bar + a simple painted, read-only track view.
// TODO(timeline): replace TimelinePreview with the real interactive TimelineWidget.
#include "ui/timeline/TimelinePanel.h"

#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/TimecodeFormat.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolBar>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr int kRulerHeight = 26;
constexpr int kHeaderWidth = 120;
constexpr int kVideoTrackHeight = 58;
constexpr int kAudioTrackHeight = 46;
constexpr double kPixelsPerFrame = 4.0; // fixed zoom in the placeholder

QColor clipColor(const ClipData& clip, TrackType type)
{
    if (clip.label.isValid())
        return clip.label;
    switch (clip.kind) {
    case ClipKind::Title:
        return QColor(0x7a, 0x5c, 0xa8);
    case ClipKind::Solid:
        return QColor(0x6a, 0x6a, 0x70);
    case ClipKind::Media:
        break;
    }
    return type == TrackType::Video ? QColor(0x3d, 0x6a, 0x9c) : QColor(0x3c, 0x7c, 0x5a);
}
} // namespace

/// Read-only drawing of the timeline: ruler, track headers, clips, playhead.
class TimelinePreview : public QWidget
{
public:
    TimelinePreview(AppContext& ctx, QWidget* parent)
        : QWidget(parent)
        , m_ctx(ctx)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMinimumHeight(kRulerHeight + 2 * kVideoTrackHeight);
        Timeline* timeline = ctx.project->timeline();
        connect(timeline, &Timeline::changed, this, qOverload<>(&QWidget::update));
        connect(timeline, &Timeline::playheadChanged, this, qOverload<>(&QWidget::update));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->position().y() <= kRulerHeight && event->position().x() >= kHeaderWidth) {
            const double frame = (event->position().x() - kHeaderWidth) / kPixelsPerFrame;
            m_ctx.project->timeline()->setPlayhead(int64_t(frame));
        }
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QPalette& pal = palette();
        const TimelineData& data = m_ctx.project->timeline()->data();
        p.fillRect(rect(), pal.color(QPalette::Base));

        // Rows: V(n)..V1 from the top, then A1..A(n).
        struct Row
        {
            const TrackData* track;
            int index;
            int height;
        };
        std::vector<Row> rows;
        for (int i = int(data.videoTracks.size()) - 1; i >= 0; --i)
            rows.push_back({&data.videoTracks[size_t(i)], i, kVideoTrackHeight});
        for (int i = 0; i < int(data.audioTracks.size()); ++i)
            rows.push_back({&data.audioTracks[size_t(i)], i, kAudioTrackHeight});

        const QColor headerBg = pal.color(QPalette::Window);
        const QColor line = pal.color(QPalette::Window).darker(140);
        int y = kRulerHeight;
        for (const Row& row : rows) {
            const QRect header(0, y, kHeaderWidth, row.height);
            p.fillRect(header, headerBg);
            p.setPen(line);
            p.drawLine(0, y + row.height - 1, width(), y + row.height - 1);
            p.setPen(pal.color(QPalette::WindowText));
            QFont bold = font();
            bold.setBold(true);
            p.setFont(bold);
            p.drawText(header.adjusted(10, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                trackDisplayName(*row.track, row.index));
            p.setFont(font());
            for (const ClipData& clip : row.track->clips)
                drawClip(p, clip, row.track->type, y, row.height);
            y += row.height;
        }
        p.setPen(line);
        p.drawLine(kHeaderWidth - 1, kRulerHeight, kHeaderWidth - 1, height());

        drawRuler(p, data);
        const int x = kHeaderWidth + int(m_ctx.project->timeline()->playhead() * kPixelsPerFrame);
        p.setPen(QPen(pal.color(QPalette::Highlight), 1.5));
        p.drawLine(x, 0, x, height());
    }

private:
    void drawClip(QPainter& p, const ClipData& clip, TrackType type, int y, int height)
    {
        const QRectF r(kHeaderWidth + clip.start * kPixelsPerFrame, y + 3,
            clip.duration * kPixelsPerFrame - 1, height - 7);
        QColor c = clipColor(clip, type);
        if (!clip.enabled)
            c = c.darker(180);
        p.setPen(c.lighter(140));
        p.setBrush(c);
        p.drawRoundedRect(r, 3, 3);
        p.setPen(Qt::white);
        p.drawText(r.adjusted(6, 2, -4, 0), Qt::AlignTop | Qt::AlignLeft, clip.name);
    }

    void drawRuler(QPainter& p, const TimelineData& data)
    {
        const QPalette& pal = palette();
        p.fillRect(QRect(0, 0, width(), kRulerHeight), pal.color(QPalette::Window).darker(115));
        const int fps = timecode::framesPerSecond(data.fps);
        p.setPen(pal.color(QPalette::PlaceholderText));
        // One labelled tick every 5 s, minor ticks every second.
        for (int64_t f = 0; kHeaderWidth + f * kPixelsPerFrame < width(); f += fps) {
            const int x = kHeaderWidth + int(f * kPixelsPerFrame);
            const bool major = (f / fps) % 5 == 0;
            p.drawLine(x, kRulerHeight - (major ? 10 : 5), x, kRulerHeight - 1);
            if (major)
                p.drawText(x + 4, 14, timecode::format(f, data.fps));
        }
    }

    AppContext& m_ctx;
};

TimelinePanel::TimelinePanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Timeline"), parent)
    , m_ctx(ctx)
{
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_toolBar = new QToolBar(body);
    m_toolBar->setIconSize(QSize(18, 18));
    m_toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    const char* const groups[][4] = {
        {"tool.select", "tool.trim", "tool.blade", nullptr},
        {"timeline.snapping", "timeline.linked_selection", nullptr, nullptr},
        {"edit.insert", "edit.overwrite", "edit.place_on_top", nullptr},
        {"timeline.zoom_out", "timeline.zoom_in", "timeline.zoom_fit", nullptr},
    };
    for (const auto& group : groups) {
        if (!m_toolBar->actions().isEmpty())
            m_toolBar->addSeparator();
        for (const char* id : group) {
            if (id && ctx.actions)
                m_toolBar->addAction(ctx.actions->action(QLatin1String(id)));
        }
    }
    layout->addWidget(m_toolBar);

    m_view = new TimelinePreview(ctx, body);
    layout->addWidget(m_view, 1);
    setContentWidget(body);

    // Title = timeline name (the program viewer is already called "Timeline").
    Timeline* timeline = ctx.project->timeline();
    auto updateTitle = [this, timeline] {
        const QString name = timeline->data().name;
        setTitle(name.isEmpty() ? tr("Timeline") : name);
    };
    connect(timeline, &Timeline::changed, this, updateTitle);
    updateTitle();
}

} // namespace kk
