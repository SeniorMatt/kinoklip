// SPDX-License-Identifier: GPL-3.0-or-later
// Shared fixtures of the behaviour parity tests (test_parity, test_parity_edit): a controller
// fixture, a widget fixture at 10 px per frame, fake media and compact layout strings.
#pragma once

#include "TimelineTestUtils.h"
#include "core/MediaPool.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QTest>
#include <QUndoStack>

#include <cmath>

namespace kk::test::parity {

using namespace kk::timelinemetrics;
namespace ops = kk::editops;

inline constexpr TrackRef kV1 {TrackType::Video, 0};
inline constexpr TrackRef kV2 {TrackType::Video, 1};
inline constexpr TrackRef kA1 {TrackType::Audio, 0};

inline QString layout(const TimelineData& d, TrackRef ref)
{
    QStringList parts;
    for (const ClipData& c : d.track(ref)->clips)
        parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
    return parts.join(QLatin1Char(' '));
}

/// A fake 10 s A/V media item (no file needed).
inline QString addMedia(Project& project, bool video = true, bool audio = true)
{
    MediaItem item;
    item.path = QStringLiteral("/nonexistent/clip.mp4");
    item.name = QStringLiteral("clip.mp4");
    item.info.valid = true;
    item.info.hasVideo = video;
    item.info.hasAudio = audio;
    item.info.durationUs = 10'000'000;
    item.info.width = 1920;
    item.info.height = 1080;
    return project.mediaPool()->addItem(item);
}

/// A media clip with a 200-frame source starting @p srcFrame frames in (25 fps).
inline ClipData mediaClip(int64_t start, int64_t duration, int64_t srcFrame = 50)
{
    ClipData c = solidClip(start, duration);
    c.kind = ClipKind::Media;
    c.sourceInUs = srcFrame * 40000;
    c.sourceDurationUs = 200 * 40000;
    return c;
}

/// Controller only (commands).
struct Ctl
{
    TimelineFixture f;
    TimelineController c {f.ctx};
    Ctl() { c.connectActions(); }
    QAction* action(const char* id) { return f.actions.action(QLatin1String(id)); }
    Timeline* tl() { return f.project.timeline(); }
    int undoCount() const { return f.project.undoStack()->count(); }
    quint64 put(TrackRef ref, ClipData clip)
    {
        f.project.beginEdit(QStringLiteral("setup"));
        const quint64 id = ops::overwriteClip(f.data(), ref, clip);
        f.project.endEdit();
        return id;
    }
    void link(QVector<quint64> ids)
    {
        f.project.beginEdit(QStringLiteral("setup"));
        ops::linkClips(f.data(), ids);
        f.project.endEdit();
    }
    void setMarks(int64_t in, int64_t out)
    {
        f.project.beginEdit(QStringLiteral("setup"));
        f.data().markIn = in;
        f.data().markOut = out;
        f.project.endEdit();
    }
};

/// Controller + shown widget at 10 px per frame.
struct View : Ctl
{
    TimelineWidget w {f.ctx, &c};
    View()
    {
        w.setThumbnailProvider({});
        w.setWaveformProvider({});
        w.resize(1200, 500);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setPixelsPerFrame(10.0, kHeaderWidth);
        w.scrollToFrame(0);
    }
    QWidget* vp() { return w.viewport(); }
    QPoint at(double frame, TrackRef ref)
    {
        const TimelineLayout& L = w.layout();
        return QPoint(int(std::lround(L.frameToX(frame))), L.rowRect(L.rowOf(ref)).center().y());
    }
    QRectF clipRect(quint64 id)
    {
        const auto loc = ops::findClip(f.view(), id);
        const ClipData& c = f.view().track(loc->track)->clips[size_t(loc->clipIndex)];
        return w.layout().clipRect(w.layout().rowOf(loc->track), c);
    }
    void drag(QPoint from, QPoint to, Qt::KeyboardModifiers mods = {})
    {
        QTest::mousePress(vp(), Qt::LeftButton, mods, from);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(vp(), from + (to - from) * i / 4);
        QTest::mouseRelease(vp(), Qt::LeftButton, mods, to);
    }
    void click(QPoint p, Qt::KeyboardModifiers mods = {})
    {
        QTest::mouseClick(vp(), Qt::LeftButton, mods, p);
    }
};
} // namespace kk::test::parity
