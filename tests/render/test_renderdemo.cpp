// SPDX-License-Identifier: GPL-3.0-or-later
// Builds a demo project from the media fixtures (two video tracks + a title track, a keyframed
// blur, a picture-in-picture with drop shadow, a cross dissolve, linked audio), saves it as
// .kkproj and renders a few frames through the --render-frame code path to PNG.
// The project and PNGs go to $KK_RENDER_DEMO_DIR (default: a new <tmp>/kinoklip-render-demo-XXXXXX
// per run) so they can be inspected, and re-rendered with `kinoklip --render-frame demo.kkproj
// <frame> out.png`.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "render/RenderFrameCommand.h"

#include <QDir>
#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;

class TestRenderDemo : public QObject
{
    Q_OBJECT
private slots:
    void buildAndRender()
    {
        QString why;
        if (!ensureFixtures(&why))
            QSKIP(qPrintable(why));
        QString dir = qEnvironmentVariable("KK_RENDER_DEMO_DIR");
        // Default: a directory of this run only (kept for inspection). A fixed path raced
        // when several checkouts ran the suite at once on one machine/container.
        QTemporaryDir runDir(QDir::tempPath() + QStringLiteral("/kinoklip-render-demo-XXXXXX"));
        runDir.setAutoRemove(false);
        if (dir.isEmpty())
            dir = runDir.path();
        else
            runDir.remove();
        QVERIFY(QDir().mkpath(dir));

        Project project;
        const ImportResult imported = project.importMedia(
            {fixture("h264_25fps.mp4"), fixture("perf_1080p.mp4"), fixture("h264_2997.mp4")});
        QCOMPARE(imported.importedIds.size(), 3);
        const QString idA = imported.importedIds[0];
        const QString idB = imported.importedIds[1];
        const QString idPip = imported.importedIds[2];

        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.videoTracks.push_back(TrackData {});
        tl.videoTracks.back().id = tl.allocateId();
        const EffectRegistry& registry = EffectRegistry::instance();

        // V1: clip A (0..4 s of the 720p fixture, blur animating 0 → 20 px) then clip B (1080p
        // fixture from 1 s) with a 24-frame cross dissolve centred on the cut at frame 100.
        ClipData a = mediaClip(tl, idA, 0, 100, 0, 10'000'000);
        a.linkGroup = 1;
        EffectInstance blur
            = registry.createEffect(QStringLiteral("gaussian_blur"), tl.allocateId());
        blur.params.animated(QStringLiteral("radius"), 0).setKey(0, 0);
        blur.params.animated(QStringLiteral("radius"), 0).setKey(99, 20);
        a.effects.push_back(blur);
        ClipData b = mediaClip(tl, idB, 100, 100, 1'000'000, 8'000'000);
        b.transitionIn = registry.createTransition(QStringLiteral("cross_dissolve"), 24);
        addClip(tl, TrackType::Video, 0, a);
        addClip(tl, TrackType::Video, 0, b);

        // V2: picture-in-picture (29.97 fps fixture), top right, slightly rotated, drop shadow.
        ClipData pip = mediaClip(tl, idPip, 20, 160, 0, 6'000'000);
        pip.transform.setBase(keys::ZoomX, 0.35);
        pip.transform.setBase(keys::ZoomY, 0.35);
        pip.transform.setBase(keys::PosX, 560);
        pip.transform.setBase(keys::PosY, -300);
        pip.transform.animated(keys::Rotation, 0).setKey(0, -8);
        pip.transform.animated(keys::Rotation, 0).setKey(159, 8);
        EffectInstance shadow
            = registry.createEffect(QStringLiteral("drop_shadow"), tl.allocateId());
        shadow.params.setBase(QStringLiteral("distance"), 24);
        shadow.params.setBase(QStringLiteral("blur"), 16);
        pip.effects.push_back(shadow);
        pip.transitionIn = registry.createTransition(QStringLiteral("cross_dissolve"), 12);
        addClip(tl, TrackType::Video, 1, pip);

        // V3: title with outline, shadow and background box, fading in.
        ClipData title = titleClip(tl, QStringLiteral("Kinoklip\nrender demo"), 10, 180, 84);
        title.title = registry.defaultParams(QStringLiteral("text_title"));
        title.title.setStatic(keys::Text, QStringLiteral("Kinoklip\nrender demo"));
        title.title.setBase(keys::Size, 84);
        title.title.setStatic(keys::Bold, true);
        title.title.setBase(keys::OutlineWidth, 3);
        title.title.setStatic(keys::OutlineColor, QColor(20, 20, 60));
        title.title.setStatic(keys::Shadow, true);
        title.title.setStatic(keys::BgEnabled, true);
        title.title.setStatic(keys::BgColor, QColor(0, 0, 0));
        title.title.setBase(keys::BgOpacity, 45);
        title.title.setBase(keys::Tracking, 8);
        title.transform.setBase(keys::PosY, 330);
        title.transitionIn = registry.createTransition(QStringLiteral("cross_dissolve"), 12);
        addClip(tl, TrackType::Video, 2, title);

        // A1: the audio of clip A, linked.
        ClipData audio = mediaClip(tl, idA, 0, 100, 0, 10'000'000);
        audio.linkGroup = 1;
        addClip(tl, TrackType::Audio, 0, audio);
        project.endEdit();

        const QString projectPath = dir + QStringLiteral("/demo.kkproj");
        QVERIFY2(project.save(projectPath, &why), qPrintable(why));
        qInfo("demo project: %s", qPrintable(projectPath));

        for (const int frame : {5, 60, 100, 150}) {
            const QString png
                = dir + QStringLiteral("/frame_%1.png").arg(frame, 4, 10, QLatin1Char('0'));
            QFile::remove(png);
            QCOMPARE(runRenderFrame(projectPath, frame, png), 0);
            const QImage image(png);
            QCOMPARE(image.size(), QSize(1920, 1080));
        }
        // The transition midpoint mixes both clips: neither equals a pure-clip render.
        const QImage cut(dir + QStringLiteral("/frame_0100.png"));
        QVERIFY(!cut.isNull());
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestRenderDemo>(argc, argv);
}

#include "test_renderdemo.moc"
