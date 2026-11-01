// SPDX-License-Identifier: GPL-3.0-or-later
// RenderTestUtil — shared helpers of the render tests: timeline builders, pixel helpers, and a
// main() that gives GL tests a real OpenGL context (re-running itself under xvfb-run when ctest
// starts it on the offscreen platform, which has no OpenGL).
#pragma once

#include "../media/MediaTestUtil.h"
#include "core/ParamKeys.h"
#include "core/TimelineData.h"
#include "render/RenderTypes.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTest>

#include <cmath>
#include <cstdio>
#include <memory>

namespace kk::test {

/// Timeline with V1, V2, A1, A2 and nothing on them.
inline TimelineData emptyTimeline(int width = 320, int height = 180, Rational fps = {25, 1})
{
    return makeDefaultTimeline(QStringLiteral("test"), fps, width, height);
}

/// A Solid clip of @p color on [start, start + duration).
inline ClipData solidClip(TimelineData& tl, const QColor& color, int64_t start, int64_t duration)
{
    ClipData c;
    c.id = tl.allocateId();
    c.kind = ClipKind::Solid;
    c.solidColor = color;
    c.start = start;
    c.duration = duration;
    return c;
}

/// A Title clip showing @p text (white, default size unless given).
inline ClipData titleClip(
    TimelineData& tl, const QString& text, int64_t start, int64_t duration, double size = 72)
{
    ClipData c;
    c.id = tl.allocateId();
    c.kind = ClipKind::Title;
    c.start = start;
    c.duration = duration;
    c.title.setStatic(keys::Text, text);
    c.title.setBase(keys::Size, size);
    return c;
}

/// A Media clip of media id @p mediaId.
inline ClipData mediaClip(TimelineData& tl, const QString& mediaId, int64_t start, int64_t duration,
    int64_t sourceInUs = 0, int64_t sourceDurationUs = 0)
{
    ClipData c;
    c.id = tl.allocateId();
    c.kind = ClipKind::Media;
    c.mediaId = mediaId;
    c.start = start;
    c.duration = duration;
    c.sourceInUs = sourceInUs;
    c.sourceDurationUs = sourceDurationUs;
    return c;
}

/// Appends @p clip to track @p track (callers add clips in time order).
inline void addClip(TimelineData& tl, TrackType type, int track, ClipData clip)
{
    tl.tracks(type)[size_t(track)].clips.push_back(std::move(clip));
}

/// Snapshot pointer of @p tl.
inline std::shared_ptr<const TimelineData> snap(const TimelineData& tl)
{
    return std::make_shared<const TimelineData>(tl);
}

/// Premultiplied ARGB of pixel (x, y) as an int array {r, g, b, a}.
struct Px
{
    int r = 0, g = 0, b = 0, a = 0;
};
inline Px px(const QImage& image, int x, int y)
{
    const QRgb v = image.pixel(x, y);
    return {qRed(v), qGreen(v), qBlue(v), qAlpha(v)};
}

/// True if every channel of @p p is within @p tolerance of (r, g, b, a).
inline bool near(const Px& p, int r, int g, int b, int a, int tolerance)
{
    return std::abs(p.r - r) <= tolerance && std::abs(p.g - g) <= tolerance
        && std::abs(p.b - b) <= tolerance && std::abs(p.a - a) <= tolerance;
}

inline QByteArray describe(const Px& p)
{
    return QByteArray("rgba(") + QByteArray::number(p.r) + ',' + QByteArray::number(p.g) + ','
        + QByteArray::number(p.b) + ',' + QByteArray::number(p.a) + ')';
}

/// Sum of squared horizontal + vertical neighbour differences of the grey value (energy of the
/// high frequencies; unlike total variation it drops when step edges are blurred).
inline double highFrequencyEnergy(const QImage& image)
{
    double sum = 0;
    for (int y = 0; y + 1 < image.height(); ++y) {
        for (int x = 0; x + 1 < image.width(); ++x) {
            const int g = qGray(image.pixel(x, y));
            const int dx = g - qGray(image.pixel(x + 1, y));
            const int dy = g - qGray(image.pixel(x, y + 1));
            sum += double(dx * dx + dy * dy);
        }
    }
    return sum;
}

/// main() for GL tests. ctest runs every test with QT_QPA_PLATFORM=offscreen, where Qt has no
/// OpenGL; in that case — if xvfb-run is installed and KK_NO_XVFB is unset — the test re-runs
/// itself as `xvfb-run -a` with QT_QPA_PLATFORM=xcb and returns that exit code. Without
/// xvfb-run the GL test cases skip (the Compositor falls back to the CPU backend).
template<typename TestObject>
int glTestMain(int argc, char** argv)
{
    const QByteArray platform = qgetenv("QT_QPA_PLATFORM");
    if (platform == "offscreen" && qEnvironmentVariableIsEmpty("KK_NO_XVFB")) {
        const QString xvfb = QStandardPaths::findExecutable(QStringLiteral("xvfb-run"));
        if (!xvfb.isEmpty()) {
            QStringList args {QStringLiteral("-a"), QStringLiteral("env"),
                QStringLiteral("QT_QPA_PLATFORM=xcb"), QStringLiteral("KK_NO_XVFB=1"),
                QString::fromLocal8Bit(argv[0])};
            for (int i = 1; i < argc; ++i)
                args << QString::fromLocal8Bit(argv[i]);
            QProcess child;
            child.setProcessChannelMode(QProcess::ForwardedChannels);
            child.start(xvfb, args);
            if (child.waitForStarted() && child.waitForFinished(-1)
                && child.exitStatus() == QProcess::NormalExit) {
                return child.exitCode();
            }
            std::fprintf(stderr, "xvfb-run failed; running without OpenGL\n");
        }
    }
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    TestObject test;
    return QTest::qExec(&test, argc, argv);
}

} // namespace kk::test
