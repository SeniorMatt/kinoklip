// SPDX-License-Identifier: GPL-3.0-or-later
// Builds the projects the user guide's screenshots are taken from (scripts/guide_screenshots.py):
//   KK_GUIDE_MEDIA=<dir with interview.mp4, broll.mp4, texture.mp4, music.wav, greenscreen.mp4,
//                   background.mp4, presenter.mp4, presenter-cold.mp4, luts/warm-look.cube>
//                   (the script generates them with ffmpeg and Python)
//   KK_GUIDE_DIR=<out dir>            writes edit.kkproj, greenscreen-raw.kkproj,
//                                     greenscreen.kkproj, greenscreen-matte.kkproj, music.kkproj,
//                                     fade, freeze, pip, vertical, blur, subtitles, keyframes,
//                                     compositing and the Color page's color, color-graded,
//                                     color-qualifier and color-lut.kkproj, and the
//                                     readme's readme-edit, readme-compositing and
//                                     readme-color.kkproj (scripts/readme_screenshots.py)
// Without KK_GUIDE_MEDIA the edit project uses stand-in solids and the media projects are
// skipped; every project is checked (invariants, save and load round trip) either way.
// Also checks the screenshot geometry sidecar (src/app/ScreenshotGeometry) the script draws from.
#include "TimelineTestUtils.h"
#include "app/ScreenshotGeometry.h"
#include "core/CompOps.h"
#include "core/Composition.h"
#include "core/Grade.h"
#include "core/Mask.h"
#include "core/NodeRegistry.h"
#include "core/RetimeOps.h"
#include "core/Subtitles.h"
#include "core/TitlePresets.h"
#include "core/VideoEffectKeys.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::test;

namespace {

/// Media clip of @p item on video track @p vTrack (plus linked audio on @p aTrack when the
/// file has sound and @p aTrack >= 0). Returns the video clip id (or the audio one).
quint64 place(
    TimelineData& d, const MediaItem& item, int64_t start, int64_t duration, int vTrack, int aTrack)
{
    ClipData c;
    c.kind = ClipKind::Media;
    c.mediaId = item.id;
    c.name = item.name;
    c.start = start;
    c.duration = duration;
    c.sourceDurationUs = item.info.durationUs;
    QVector<editops::PlacedClip> placed;
    if (vTrack >= 0 && item.info.hasVideo)
        placed.append({TrackRef {TrackType::Video, vTrack}, c});
    if (aTrack >= 0 && item.info.hasAudio)
        placed.append({TrackRef {TrackType::Audio, aTrack}, c});
    const QVector<quint64> ids = editops::overwriteClips(d, placed);
    if (ids.size() > 1)
        editops::linkClips(d, ids);
    return ids.isEmpty() ? 0 : ids.front();
}

const MediaItem* itemNamed(const Project& project, const QString& baseName)
{
    for (const MediaItem& item : project.mediaPool()->items()) {
        if (QFileInfo(item.path).completeBaseName() == baseName)
            return &item;
    }
    return nullptr;
}

/// The guide's demo timelines keep V1, V2, A1 and A2 (new timelines start with V1 / A1 only,
/// spec 25.14), so the screenshots stay as they are.
void withGuideTracks(TimelineData& d)
{
    while (d.videoTracks.size() < 2)
        editops::addTrack(d, TrackType::Video);
    while (d.audioTracks.size() < 2)
        editops::addTrack(d, TrackType::Audio);
}

} // namespace

class TestGuideDemo : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_guidedemo"); }

    void editProject()
    {
        Project project;
        QVERIFY(buildDemoTimeline(project, mediaDir()) > 10);
        project.setName(QStringLiteral("My First Video"));
        saveAndCheck(project, QStringLiteral("edit.kkproj"));
    }

    void greenScreenProjects_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("view"); // empty = no key on the clip
        QTest::newRow("raw") << QStringLiteral("greenscreen-raw.kkproj") << QString();
        QTest::newRow("keyed") << QStringLiteral("greenscreen.kkproj") << fxchoice::Final;
        QTest::newRow("matte") << QStringLiteral("greenscreen-matte.kkproj") << fxchoice::Matte;
    }

    void greenScreenProjects()
    {
        QFETCH(QString, file);
        QFETCH(QString, view);
        Project project;
        if (!importAll(project, {"background.mp4", "greenscreen.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no background.mp4 / greenscreen.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("Green Screen");
        while (d.videoTracks.size() < 2)
            editops::addTrack(d, TrackType::Video);
        const int64_t length = 10 * 24;
        place(d, *itemNamed(project, QStringLiteral("background")), 0, length, 0, 0);
        const quint64 fg
            = place(d, *itemNamed(project, QStringLiteral("greenscreen")), 0, length, 1, -1);
        if (!view.isEmpty()) {
            ClipData* c = editops::clipById(d, fg);
            QVERIFY(c);
            EffectInstance key = EffectRegistry::instance().createEffect(
                QLatin1String(fxid::ChromaKey), d.allocateId());
            key.params.setStatic(fxkeys::View, view);
            c->effects.append(key);
        }
        project.endEdit();
        project.timeline()->setPlayhead(length / 2);
        project.setName(QStringLiteral("Green Screen Test"));
        saveAndCheck(project, file);
    }

    void musicProject()
    {
        Project project;
        if (!importAll(project, {"interview.mp4", "music.wav"}))
            QSKIP("KK_GUIDE_MEDIA has no interview.mp4 / music.wav");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("With Music");
        d.audioTracks[1].name = QStringLiteral("Music");
        const int64_t fps = 24; // a new project's frame rate
        place(d, *itemNamed(project, QStringLiteral("interview")), 0, 12 * fps, 0, 0);
        const quint64 music
            = place(d, *itemNamed(project, QStringLiteral("music")), 0, 12 * fps, -1, 1);
        ClipData* m = editops::clipById(d, music);
        QVERIFY(m);
        m->fadeInFrames = 2 * fps;
        m->fadeOutFrames = 3 * fps;
        m->audio.setBase(keys::VolumeDb, -8);
        d.audioTracks[1].height = 90; // room for the level line and fade handles
        d.audioTracks[0].height = 70;
        project.endEdit();
        project.timeline()->setPlayhead(6 * fps);
        project.setName(QStringLiteral("Music Video"));
        saveAndCheck(project, QStringLiteral("music.kkproj"));
    }

    /// Fade to black: a fade in at the start of the first clip, a fade out at the end of the
    /// last one, the playhead in the fade out.
    void fadeProject()
    {
        Project project;
        if (!importAll(project, {"interview.mp4", "broll.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no interview.mp4 / broll.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("Fades");
        const int64_t fps = 24; // a new project's frame rate
        const quint64 first
            = place(d, *itemNamed(project, QStringLiteral("interview")), 0, 8 * fps, 0, 0);
        const quint64 last
            = place(d, *itemNamed(project, QStringLiteral("broll")), 8 * fps, 8 * fps, 0, 0);
        for (const quint64 part : editops::withLinkedClips(d, {first})) {
            ClipData* c = editops::clipById(d, part);
            QVERIFY(c);
            c->fadeInFrames = fps * 3 / 2;
        }
        for (const quint64 part : editops::withLinkedClips(d, {last})) {
            ClipData* c = editops::clipById(d, part);
            QVERIFY(c);
            c->fadeOutFrames = 2 * fps;
        }
        d.videoTracks[0].height = 80;
        project.endEdit();
        project.timeline()->setPlayhead(15 * fps);
        project.setName(QStringLiteral("Fade to Black"));
        saveAndCheck(project, QStringLiteral("fade.kkproj"));
    }

    /// Freeze frame: a two-second freeze inside a clip (retime controls).
    void freezeProject()
    {
        Project project;
        if (!importAll(project, {"presenter.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no presenter.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("Freeze");
        const int64_t fps = 24; // a new project's frame rate
        const quint64 id
            = place(d, *itemNamed(project, QStringLiteral("presenter")), 0, 8 * fps, 0, -1);
        QVERIFY(retimeops::insertFreeze(d, id, 3 * fps, 2 * fps));
        d.videoTracks[0].height = 80;
        project.endEdit();
        project.timeline()->setPlayhead(4 * fps);
        project.setName(QStringLiteral("Freeze Frame"));
        saveAndCheck(project, QStringLiteral("freeze.kkproj"));
    }

    /// Picture-in-picture: B-roll on V2 made smaller, cropped and moved to the top right.
    void pipProject()
    {
        Project project;
        if (!importAll(project, {"presenter.mp4", "broll.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no presenter.mp4 / broll.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("Picture in Picture");
        while (d.videoTracks.size() < 2)
            editops::addTrack(d, TrackType::Video);
        const int64_t fps = 24; // a new project's frame rate
        place(d, *itemNamed(project, QStringLiteral("presenter")), 0, 10 * fps, 0, 0);
        const quint64 small
            = place(d, *itemNamed(project, QStringLiteral("broll")), 0, 10 * fps, 1, -1);
        ClipData* c = editops::clipById(d, small);
        QVERIFY(c);
        c->transform.setBase(keys::ZoomX, 0.35);
        c->transform.setBase(keys::ZoomY, 0.35);
        c->transform.setBase(keys::PosX, 580);
        c->transform.setBase(keys::PosY, -300);
        c->transform.setBase(keys::CropL, 120);
        c->transform.setBase(keys::CropR, 120);
        project.endEdit();
        project.timeline()->setPlayhead(5 * fps);
        project.setName(QStringLiteral("Picture in Picture"));
        saveAndCheck(project, QStringLiteral("pip.kkproj"));
    }

    /// A vertical (9:16) project: the landscape presenter clip zoomed to fill the height.
    void verticalProject()
    {
        Project project;
        if (!importAll(project, {"presenter.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no presenter.mp4");
        ProjectSettings settings = project.settings();
        settings.width = 1080;
        settings.height = 1920;
        project.setSettings(settings);
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("Vertical");
        QCOMPARE(d.width, 1080);
        const int64_t fps = 24; // a new project's frame rate
        const quint64 id
            = place(d, *itemNamed(project, QStringLiteral("presenter")), 0, 10 * fps, 0, 0);
        ClipData* c = editops::clipById(d, id);
        QVERIFY(c);
        // Scaled to fit, a 16:9 picture is 1080 x 607.5 here; 3.16 fills the 1920 lines.
        c->transform.setBase(keys::ZoomX, 3.16);
        c->transform.setBase(keys::ZoomY, 3.16);
        project.endEdit();
        project.timeline()->setPlayhead(5 * fps);
        project.setName(QStringLiteral("Vertical Video"));
        saveAndCheck(project, QStringLiteral("vertical.kkproj"));
    }

    /// Hide a face: an adjustment clip on V2 with Mosaic and an ellipse mask over the head.
    void blurProject()
    {
        Project project;
        if (!importAll(project, {"presenter.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no presenter.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("Hide a Face");
        while (d.videoTracks.size() < 2)
            editops::addTrack(d, TrackType::Video);
        const int64_t fps = 24; // a new project's frame rate
        place(d, *itemNamed(project, QStringLiteral("presenter")), 0, 10 * fps, 0, 0);
        ClipData adj;
        adj.kind = ClipKind::Adjustment;
        adj.name = QStringLiteral("Adjustment Clip");
        adj.start = 0;
        adj.duration = 10 * fps;
        EffectInstance mosaic
            = EffectRegistry::instance().createEffect(QStringLiteral("mosaic"), d.allocateId());
        mosaic.params.setBase(QStringLiteral("blockSize"), 40);
        adj.effects.append(mosaic);
        MaskData mask = makeMask(MaskShape::Ellipse, d.allocateId(), QSizeF(420, 440));
        mask.params.setBase(maskkeys::PosY, -140);
        mask.params.setBase(maskkeys::FeatherOut, 20);
        adj.masks.append(mask);
        editops::overwriteClip(d, TrackRef {TrackType::Video, 1}, adj);
        project.endEdit();
        project.timeline()->setPlayhead(5 * fps);
        project.setName(QStringLiteral("Hide a Face"));
        saveAndCheck(project, QStringLiteral("blur.kkproj"));
    }

    /// Subtitles: three captions on ST1 over two clips.
    void subtitlesProject()
    {
        Project project;
        if (!importAll(project, {"presenter.mp4", "broll.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no presenter.mp4 / broll.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("With Subtitles");
        const int64_t fps = 24; // a new project's frame rate
        place(d, *itemNamed(project, QStringLiteral("presenter")), 0, 9 * fps, 0, 0);
        place(d, *itemNamed(project, QStringLiteral("broll")), 9 * fps, 5 * fps, 0, 0);
        editops::addTrack(d, TrackType::Subtitle);
        const std::pair<const char*, int64_t> lines[] = {
            {"Welcome to our little film.", 1 * fps},
            {"Today we try something new.", 5 * fps},
            {"Here is how it went.", 9 * fps},
        };
        for (const auto& [text, start] : lines)
            editops::overwriteClip(d, TrackRef {TrackType::Subtitle, 0},
                subtitles::makeCaption(QString::fromUtf8(text), start, 3 * fps));
        project.endEdit();
        project.timeline()->setPlayhead(6 * fps);
        project.setName(QStringLiteral("Subtitles"));
        saveAndCheck(project, QStringLiteral("subtitles.kkproj"));
    }

    /// Keyframes: a slow zoom in (Zoom 1.0 -> 1.4), the playhead on the last key.
    void keyframesProject()
    {
        Project project;
        if (!importAll(project, {"presenter.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no presenter.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("Slow Zoom");
        const int64_t fps = 24; // a new project's frame rate
        const int64_t length = 8 * fps;
        const quint64 id
            = place(d, *itemNamed(project, QStringLiteral("presenter")), 0, length, 0, 0);
        ClipData* c = editops::clipById(d, id);
        QVERIFY(c);
        for (const QString& key : {keys::ZoomX, keys::ZoomY}) {
            AnimatedValue& zoom = c->transform.animated(key, 1.0);
            zoom.setKey(0, 1.0);
            zoom.setKey(length - fps, 1.4);
        }
        d.videoTracks[0].height = 80;
        project.endEdit();
        project.timeline()->setPlayhead(length - fps);
        project.setName(QStringLiteral("Keyframes"));
        saveAndCheck(project, QStringLiteral("keyframes.kkproj"));
    }

    /// Compositing page: a Text node merged over the clip (MediaIn1 -> Merge1 <- Text1).
    void compositingProject()
    {
        Project project;
        if (!importAll(project, {"presenter.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no presenter.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("Compositing");
        const int64_t fps = 24; // a new project's frame rate
        const quint64 id
            = place(d, *itemNamed(project, QStringLiteral("presenter")), 0, 10 * fps, 0, 0);
        ClipData* c = editops::clipById(d, id);
        QVERIFY(c);
        Composition comp = compops::defaultComposition();
        const compops::AddResult text
            = compops::addNodeAfter(comp, nodetypes::Text, comp.mediaOutId);
        QVERIFY(text.node && text.merge);
        CompNode* t = comp.node(text.node);
        t->params.setStatic(nodekeys::Text, QStringLiteral("Our Summer Trip"));
        t->params.setBase(nodekeys::FontSize, 120);
        t->params.setStatic(nodekeys::Bold, true);
        t->params.setBase(nodekeys::OutlineWidth, 4);
        t->params.setBase(nodekeys::CenterY, 0.18);
        c->composition = comp;
        project.endEdit();
        project.timeline()->setPlayhead(5 * fps);
        project.setName(QStringLiteral("Compositing"));
        saveAndCheck(project, QStringLiteral("compositing.kkproj"));
    }

    void colorProjects_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("stage"); // 0 ungraded, 1 basic grade, 2 + qualifier / window, 3 LUT
        QTest::newRow("ungraded") << QStringLiteral("color.kkproj") << 0;
        QTest::newRow("graded") << QStringLiteral("color-graded.kkproj") << 1;
        QTest::newRow("qualifier") << QStringLiteral("color-qualifier.kkproj") << 2;
        QTest::newRow("lut") << QStringLiteral("color-lut.kkproj") << 3;
    }

    /// Color page: a dark, cold shot of the presenter twice with B-roll between them (clips
    /// 01 - 03). Stage 1 warms and brightens clip 01 (Gain, Temp); stage 2 adds node 02 (the
    /// blue shirt qualified and turned red) and node 03 (a circle window brightening the
    /// face); stage 3 adds node 02 with the demo LUT after the basic grade.
    void colorProjects()
    {
        QFETCH(QString, file);
        QFETCH(int, stage);
        Project project;
        if (!importAll(project, {"presenter-cold.mp4", "broll.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no presenter-cold.mp4 / broll.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        d.name = QStringLiteral("Interview");
        const int64_t fps = 24; // a new project's frame rate
        const MediaItem& cold = *itemNamed(project, QStringLiteral("presenter-cold"));
        const quint64 first = place(d, cold, 0, 5 * fps, 0, 0);
        place(d, *itemNamed(project, QStringLiteral("broll")), 5 * fps, 4 * fps, 0, 0);
        place(d, cold, 9 * fps, 5 * fps, 0, 0);
        project.endEdit();
        namespace gk = gradekeys;
        namespace wk = gradewindowkeys;
        ClipGrade g;
        if (stage >= 1) {
            const quint64 n1 = gradeops::initialize(g);
            ParamSet& p = g.node(n1)->params;
            p.setBase(gk::GainY, 1.3);
            p.setBase(gk::GammaY, 0.03);
            p.setBase(gk::LiftY, -0.02);
            p.setBase(gk::Temperature, 0.8);
            p.setBase(gk::Tint, 0.04);
            if (stage == 2) {
                const quint64 n2 = gradeops::addSerialNode(g, n1);
                GradeQualifier& q = g.node(n2)->qualifier;
                q.enabled = true;
                q.hueCenter = 0.62; // the blue shirt
                q.hueWidth = 0.1;
                q.hueSoft = 0.04;
                q.satLow = 0.35;
                q.satHigh = 1.0;
                q.lumLow = 0.05;
                q.lumHigh = 0.8;
                g.node(n2)->params.setBase(gk::Hue, 140);
                g.node(n2)->params.setBase(gk::Saturation, 1.2);
                const quint64 n3 = gradeops::addSerialNode(g, n2);
                const quint64 w = gradeops::addWindow(g, n3, GradeWindowShape::Circle);
                QVERIFY(w);
                GradeWindow* win = nullptr;
                for (GradeWindow& candidate : g.node(n3)->windows) {
                    if (candidate.id == w)
                        win = &candidate;
                }
                QVERIFY(win);
                win->params.setBase(wk::CenterX, 0.5);
                win->params.setBase(wk::CenterY, 0.37);
                win->params.setBase(wk::Width, 0.2);
                win->params.setBase(wk::Height, 0.33);
                win->params.setBase(wk::SoftOut, 0.05);
                g.node(n3)->params.setBase(gk::GammaY, 0.06);
            }
            if (stage == 3) {
                const QString lut
                    = QDir(mediaDir()).filePath(QStringLiteral("luts/warm-look.cube"));
                if (!QFileInfo::exists(lut))
                    QSKIP("KK_GUIDE_MEDIA has no luts/warm-look.cube");
                g.node(gradeops::addSerialNode(g, n1))->lutPath = lut;
            }
        }
        if (!g.isEmpty())
            QVERIFY(
                project.setClipGrade(project.activeTimelineUid(), first, g, QStringLiteral("G")));
        project.timeline()->setPlayhead(2 * fps);
        project.setName(QStringLiteral("Interview"));
        saveAndCheck(project, file);
    }

    /// The readme's Edit (also Media, Cut, Audio and Deliver) project: a presenter cut with a
    /// cross dissolve on V1 / A1, B-roll cutaways on V2, a lower third and an end title on V3,
    /// music with fades on A2 (scripts/readme_screenshots.py adds host.mp4 to the demo media).
    void readmeEditProject()
    {
        Project project;
        if (!importAll(
                project, {"host.mp4", "background.mp4", "broll.mp4", "texture.mp4", "music.wav"}))
            QSKIP("KK_GUIDE_MEDIA has no host.mp4 (scripts/readme_screenshots.py makes it)");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        d.name = QStringLiteral("Field Notes");
        while (d.videoTracks.size() < 3)
            editops::addTrack(d, TrackType::Video);
        withGuideTracks(d);
        d.videoTracks[0].name = QStringLiteral("Presenter");
        d.videoTracks[1].name = QStringLiteral("B-Roll");
        d.videoTracks[2].name = QStringLiteral("Titles");
        d.audioTracks[0].name = QStringLiteral("Dialogue");
        d.audioTracks[1].name = QStringLiteral("Music");
        const int64_t fps = 24; // a new project's frame rate
        // A clip from second @p srcSec of @p item, at second @p start for @p length seconds.
        auto at = [&](const QString& name, int64_t start, int64_t length, int64_t srcSec,
                      int vTrack, int aTrack) {
            const quint64 id
                = place(d, *itemNamed(project, name), start * fps, length * fps, vTrack, aTrack);
            for (const quint64 part : editops::withLinkedClips(d, {id})) {
                if (ClipData* c = editops::clipById(d, part))
                    c->sourceInUs = srcSec * 1000000;
            }
            return id;
        };
        // V1 / A1: the presenter, a wide shot of the landscape, the presenter again.
        at(QStringLiteral("host"), 0, 18, 0, 0, 0);
        const quint64 wide = at(QStringLiteral("background"), 18, 4, 2, 0, -1);
        const quint64 back = at(QStringLiteral("host"), 22, 8, 20, 0, 0);
        for (const quint64 id : {wide, back}) {
            ClipData* c = editops::clipById(d, id);
            QVERIFY(c);
            c->transitionIn = EffectRegistry::instance().createTransition(
                QStringLiteral("cross_dissolve"), fps);
        }
        // V2: B-roll cutaways over the presenter (picture only).
        at(QStringLiteral("broll"), 7, 4, 5, 1, -1);
        at(QStringLiteral("texture"), 12, 3, 8, 1, -1);
        // V3: a left lower third over the first shot and an end title.
        auto title = [&](const QString& preset, const QString& name, const QString& text,
                         int64_t start, int64_t length) {
            ClipData c;
            c.kind = ClipKind::Title;
            c.generator = preset;
            c.title = EffectRegistry::instance().defaultParams(preset);
            c.title.setStatic(keys::Text, text);
            c.name = name;
            c.start = start * fps;
            c.duration = length * fps;
            editops::overwriteClip(d, TrackRef {TrackType::Video, 2}, c);
        };
        title(titles::LeftLowerThird, QStringLiteral("Left Lower Third"),
            QStringLiteral("Jordan Lee\nPresenter, Field Notes"), 1, 5);
        title(titles::Text, QStringLiteral("Text"), QStringLiteral("Thanks for watching"), 26, 4);
        // A2: music under the whole piece, faded in and out, a little lower.
        ClipData* m = editops::clipById(d, at(QStringLiteral("music"), 0, 30, 0, -1, 1));
        QVERIFY(m);
        m->fadeInFrames = 2 * fps;
        m->fadeOutFrames = 3 * fps;
        m->audio.setBase(keys::VolumeDb, -10);
        d.videoTracks[0].height = 70;
        d.audioTracks[0].height = 70;
        d.audioTracks[1].height = 70;
        d.markers.append(Marker {18 * fps, QStringLiteral("Wide shot"), QColor(0x4c, 0x9b, 0xe8)});
        project.endEdit();
        project.timeline()->setPlayhead(int64_t(3.5 * double(fps)));
        project.setName(QStringLiteral("Field Notes"));
        saveAndCheck(project, QStringLiteral("readme-edit.kkproj"));
    }

    /// The readme's Compositing page: the presenter keyed off the green screen (MediaIn1 ->
    /// ChromaKey1) merged over the landscape (MediaIn2), a Text node on top; the left viewer
    /// shows the key, the right one the result.
    void readmeCompositingProject()
    {
        Project project;
        if (!importAll(project, {"greenscreen.mp4", "background.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no greenscreen.mp4 / background.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        withGuideTracks(d);
        d.name = QStringLiteral("Weather Report");
        const int64_t fps = 24; // a new project's frame rate
        const quint64 id
            = place(d, *itemNamed(project, QStringLiteral("greenscreen")), 0, 10 * fps, 0, -1);
        ClipData* c = editops::clipById(d, id);
        QVERIFY(c);
        constexpr double sx = 4.5; // the node editor's spacing (CompOps.cpp kStepX, kStepY)
        constexpr double sy = 1.5;
        Composition comp = compops::defaultComposition();
        const quint64 in1 = comp.nodes.front().id;
        comp.node(in1)->position = QPointF(0, -sy);
        const quint64 key = compops::addNode(
            comp, nodetypes::EffectPrefix + QLatin1String(fxid::ChromaKey), QPointF(sx, -sy));
        const quint64 in2 = compops::addNode(comp, nodetypes::MediaIn, QPointF(0, 0));
        const quint64 merge1 = compops::addNode(comp, nodetypes::Merge, QPointF(2 * sx, 0));
        const quint64 text = compops::addNode(comp, nodetypes::Text, QPointF(3 * sx, -sy));
        const quint64 merge2 = compops::addNode(comp, nodetypes::Merge, QPointF(3 * sx, 0));
        QVERIFY(key && in2 && merge1 && text && merge2);
        comp.node(in2)->mediaId = itemNamed(project, QStringLiteral("background"))->id;
        comp.node(comp.mediaOutId)->position = QPointF(4 * sx, 0);
        const struct
        {
            quint64 from, to;
            QString port;
        } links[] = {{in1, key, nodeports::Input}, {in2, merge1, nodeports::Background},
            {key, merge1, nodeports::Foreground}, {merge1, merge2, nodeports::Background},
            {text, merge2, nodeports::Foreground}, {merge2, comp.mediaOutId, nodeports::Input}};
        for (const auto& link : links)
            QVERIFY(compops::connect(comp, link.from, 0, link.to, link.port)
                == compops::ConnectError::None);
        CompNode* t = comp.node(text);
        t->params.setStatic(nodekeys::Text, QStringLiteral("Sunny all week"));
        t->params.setBase(nodekeys::FontSize, 110);
        t->params.setStatic(nodekeys::Bold, true);
        t->params.setBase(nodekeys::OutlineWidth, 3);
        t->params.setBase(nodekeys::CenterY, 0.85);
        compops::setViewerNodes(comp, key, 0);
        comp.viewCentre = QPointF(2 * sx, -sy / 2);
        comp.viewZoom = 1.8;
        c->composition = comp;
        project.endEdit();
        project.timeline()->setPlayhead(5 * fps);
        project.setName(QStringLiteral("Weather Report"));
        saveAndCheck(project, QStringLiteral("readme-compositing.kkproj"));
    }

    /// The readme's Color page: the cold presenter shot balanced and warmed (the wheels
    /// moved), the shirt qualified and a circle window on the face, between other shots.
    void readmeColorProject()
    {
        Project project;
        if (!importAll(
                project, {"presenter-cold.mp4", "broll.mp4", "background.mp4", "texture.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no presenter-cold.mp4 / broll.mp4 / background.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        d.name = QStringLiteral("Interview");
        const int64_t fps = 24; // a new project's frame rate
        const MediaItem& cold = *itemNamed(project, QStringLiteral("presenter-cold"));
        const quint64 first = place(d, cold, 0, 5 * fps, 0, 0);
        place(d, *itemNamed(project, QStringLiteral("background")), 5 * fps, 4 * fps, 0, 0);
        place(d, cold, 9 * fps, 5 * fps, 0, 0);
        place(d, *itemNamed(project, QStringLiteral("broll")), 14 * fps, 4 * fps, 0, 0);
        place(d, *itemNamed(project, QStringLiteral("texture")), 18 * fps, 4 * fps, 0, 0);
        place(d, cold, 22 * fps, 5 * fps, 0, 0);
        project.endEdit();
        namespace gk = gradekeys;
        namespace wk = gradewindowkeys;
        ClipGrade g;
        const quint64 n1 = gradeops::initialize(g);
        ParamSet& p = g.node(n1)->params;
        p.setBase(gk::LiftR, -0.01);
        p.setBase(gk::LiftB, 0.025);
        p.setBase(gk::GammaR, 0.03);
        p.setBase(gk::GammaB, -0.03);
        p.setBase(gk::GammaY, 0.03);
        p.setBase(gk::GainR, 1.22);
        p.setBase(gk::GainG, 1.08);
        p.setBase(gk::GainB, 0.98);
        p.setBase(gk::GainY, 1.2);
        p.setBase(gk::Contrast, 1.08);
        p.setBase(gk::Saturation, 1.15);
        const quint64 n2 = gradeops::addSerialNode(g, n1);
        GradeQualifier& q = g.node(n2)->qualifier;
        q.enabled = true;
        q.hueCenter = 0.62; // the blue shirt
        q.hueWidth = 0.1;
        q.hueSoft = 0.04;
        q.satLow = 0.35;
        q.satHigh = 1.0;
        q.lumLow = 0.05;
        q.lumHigh = 0.8;
        g.node(n2)->params.setBase(gk::Saturation, 1.3);
        const quint64 n3 = gradeops::addSerialNode(g, n2);
        const quint64 w = gradeops::addWindow(g, n3, GradeWindowShape::Circle);
        QVERIFY(w);
        for (GradeWindow& win : g.node(n3)->windows) {
            if (win.id != w)
                continue;
            win.params.setBase(wk::CenterX, 0.5);
            win.params.setBase(wk::CenterY, 0.37);
            win.params.setBase(wk::Width, 0.2);
            win.params.setBase(wk::Height, 0.33);
            win.params.setBase(wk::SoftOut, 0.05);
        }
        g.node(n3)->params.setBase(gk::GammaY, 0.05);
        QVERIFY(project.setClipGrade(project.activeTimelineUid(), first, g, QStringLiteral("G")));
        project.timeline()->setPlayhead(2 * fps);
        project.setName(QStringLiteral("Interview"));
        saveAndCheck(project, QStringLiteral("readme-color.kkproj"));
    }

    /// The sidecar lists named widgets, tool bar buttons by action id, clips, transitions and
    /// the playhead, in the panel's own coordinates.
    void geometrySidecar()
    {
        TimelineFixture f;
        buildDemoTimeline(f.project); // stand-in solids: add the transition the media gets
        f.project.beginEdit(QStringLiteral("Transition"));
        f.data().videoTracks[0].clips[1].transitionIn
            = EffectRegistry::instance().createTransition(QStringLiteral("cross_dissolve"), 25);
        f.project.endEdit();
        TimelinePanel panel(f.ctx);
        panel.resize(1600, 520);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        panel.timelineWidget()->zoomToFit();
        QTest::qWait(20);
        const QString path = m_tmp.filePath(QStringLiteral("geometry.json"));
        QVERIFY(
            writeScreenshotGeometry(&panel, path, {}, &f.view(), f.project.timeline()->playhead()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject doc = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(doc.value(QStringLiteral("width")).toInt(), 1600);
        auto find = [&](const QString& key, const QString& value) {
            for (const QJsonValue v : doc.value(QStringLiteral("entries")).toArray()) {
                if (v.toObject().value(key).toVariant().toString() == value)
                    return v.toObject();
            }
            return QJsonObject();
        };
        auto rectOf = [](const QJsonObject& o) {
            const QJsonArray r = o.value(QStringLiteral("rect")).toArray();
            return QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt());
        };
        const QRect inside(0, 0, 1600, 520);
        const QJsonObject blade = find(QStringLiteral("action"), QStringLiteral("tool.blade"));
        QVERIFY(!blade.isEmpty());
        QVERIFY(inside.contains(rectOf(blade)));
        const QJsonObject slider
            = find(QStringLiteral("name"), QStringLiteral("TimelineZoomSlider"));
        QVERIFY(!slider.isEmpty());
        // V1's first clip starts at frame 0, right of the track headers; the transition at the
        // head of V1's second clip is centred on the cut between the two.
        QJsonObject first, second, transition, playhead;
        for (const QJsonValue v : doc.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject o = v.toObject();
            const QString kind = o.value(QStringLiteral("kind")).toString();
            const bool v1 = o.value(QStringLiteral("track")).toString() == QLatin1String("V1");
            const int index = o.value(QStringLiteral("index")).toInt();
            if (kind == QLatin1String("clip") && v1 && index == 0)
                first = o;
            if (kind == QLatin1String("clip") && v1 && index == 1)
                second = o;
            if (kind == QLatin1String("transition") && v1 && index == 1)
                transition = o;
            if (kind == QLatin1String("playhead"))
                playhead = o;
        }
        QVERIFY(
            !first.isEmpty() && !second.isEmpty() && !transition.isEmpty() && !playhead.isEmpty());
        QCOMPARE(
            first.value(QStringLiteral("text")).toString(), f.view().videoTracks[0].clips[0].name);
        QVERIFY(rectOf(first).left() >= timelinemetrics::kHeaderWidth - 1);
        const int cut = rectOf(second).left();
        QVERIFY(rectOf(transition).left() < cut && rectOf(transition).right() > cut);
        QVERIFY(rectOf(playhead).width() == 1 && inside.contains(rectOf(playhead)));
    }

private:
    QTemporaryDir m_tmp;

    static QString mediaDir() { return qEnvironmentVariable("KK_GUIDE_MEDIA"); }

    /// Imports @p names from KK_GUIDE_MEDIA; false if one is missing.
    static bool importAll(Project& project, std::initializer_list<const char*> names)
    {
        const QString dir = mediaDir();
        if (dir.isEmpty())
            return false;
        QStringList paths;
        for (const char* name : names) {
            const QString path = QDir(dir).filePath(QLatin1String(name));
            if (!QFileInfo::exists(path))
                return false;
            paths << path;
        }
        return project.importMedia(paths).importedIds.size() == paths.size();
    }

    /// Checks @p project, saves it into KK_GUIDE_DIR (else a temporary folder) and loads it back.
    void saveAndCheck(Project& project, const QString& file)
    {
        QString why;
        QVERIFY2(editops::checkInvariants(project.timeline()->data(), &why), qPrintable(why));
        QString dir = qEnvironmentVariable("KK_GUIDE_DIR");
        if (dir.isEmpty())
            dir = m_tmp.path();
        QVERIFY(QDir().mkpath(dir));
        const QString path = QDir(dir).filePath(file);
        QVERIFY2(project.save(path, &why), qPrintable(why));
        Project loaded;
        QVERIFY2(loaded.load(path, &why), qPrintable(why));
        QCOMPARE(loaded.timeline()->data().videoTracks.size(),
            project.timeline()->data().videoTracks.size());
        QCOMPARE(loaded.timeline()->data().duration(), project.timeline()->data().duration());
    }
};

QTEST_MAIN(TestGuideDemo)
#include "test_guidedemo.moc"
