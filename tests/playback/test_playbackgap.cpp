// SPDX-License-Identifier: GPL-3.0-or-later
// Gaps on the timeline play black (0.4.0 owner report: an earlier clip kept playing in a gap
// after a long session of edits): PlaybackEngine against a real Compositor with media clips cut
// from a longer file. Covers playing across a clip's end into a gap, seeking into it (paused
// and while playing), compound and retimed clips, the render cache, gaps made by lifts and many
// drag-style ripple trims, and changes inside a compound clip reaching its parent's playback.
//
// Manual variants: KK_GAP_TEXTURES=1 renders zero-copy textures like the GPU viewer (read back
// here), KK_GAP_CACHE=1 runs the ripple trims with the Smart render cache rendering in the
// background; run under xvfb-run with QT_QPA_PLATFORM=xcb for the OpenGL compositor.
#include "../media/MediaTestUtil.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "export/RenderCacheManager.h"
#include "media/MediaInfo.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <map>

using namespace kk;

namespace {

constexpr int64_t kClipEnd = 30; ///< the first clip covers [0, 30)
constexpr int64_t kGapEnd = 90; ///< gap [30, 90), then a red solid

std::shared_ptr<Compositor> g_compositor;

std::unique_ptr<PlaybackEngine> makeEngine()
{
    if (!g_compositor)
        g_compositor = std::make_shared<Compositor>();
    PlaybackEngine::Options options;
    options.compositor = g_compositor;
    options.noAudio = true;
    return std::make_unique<PlaybackEngine>(std::move(options));
}

/// The picture of @p f: its image, or its texture read back in a context of the share group
/// (what the GPU viewer draws).
QImage pictureOf(const RenderedFramePtr& f)
{
    if (!f)
        return {};
    if (!f->image.isNull() || f->texture == 0)
        return f->image;
    static QOffscreenSurface* surface = nullptr;
    static QOpenGLContext* ctx = nullptr;
    if (!ctx) {
        surface = new QOffscreenSurface;
        surface->setFormat(QSurfaceFormat::defaultFormat());
        surface->create();
        ctx = new QOpenGLContext;
        ctx->setShareContext(QOpenGLContext::globalShareContext());
        ctx->create();
    }
    ctx->makeCurrent(surface);
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(ctx);
    gl->initializeOpenGLFunctions();
    if (f->fence)
        gl->glWaitSync(static_cast<GLsync>(f->fence), 0, GL_TIMEOUT_IGNORED);
    GLuint fbo = 0;
    gl->glGenFramebuffers(1, &fbo);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->texture, 0);
    QImage img(f->size, QImage::Format_RGBA8888_Premultiplied);
    gl->glReadPixels(0, 0, img.width(), img.height(), GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glDeleteFramebuffers(1, &fbo);
    ctx->doneCurrent();
    return img.mirrored(false, true);
}

/// True when every pixel of @p f is black or transparent (nothing drawn).
bool isEmptyPicture(const RenderedFramePtr& f)
{
    const QImage picture = pictureOf(f);
    if (picture.isNull())
        return false;
    const QImage img = picture.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < img.height(); y += 4) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 4) {
            if ((line[x] & 0x00ffffff) != 0)
                return false;
        }
    }
    return true;
}

/// What a frame showed: empty, or the barcode index of the media frame (-1 = unreadable).
struct Seen
{
    bool empty = false;
    int index = -1;
};

} // namespace

class TestPlaybackGap : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<Project> m_project;
    QString m_media;
    int64_t m_mediaUs = 0;

    enum Variant { Plain, Compound, Retimed, Blended };

    /// V1: [0, 30) a media clip from source second 1 of a longer file (a compound clip holding
    /// it for Compound, at half speed for Retimed / Blended), gap [30, 90), a red solid from 90
    /// to 150; A2: the media's sound over [0, 150).
    void buildTimeline(int variant)
    {
        const bool nested = variant == Compound;
        m_project->beginEdit(QStringLiteral("gap"));
        TimelineData& tl = m_project->timeline()->mutableData();
        tl = makeDefaultTimeline(QStringLiteral("gap"), {25, 1}, 320, 180);
        ClipData media;
        media.kind = ClipKind::Media;
        media.mediaId = m_media;
        media.sourceInUs = 1'000'000;
        media.sourceDurationUs = m_mediaUs;
        media.duration = kClipEnd;
        if (variant == Retimed || variant == Blended)
            media.speed = 0.5;
        if (variant == Blended)
            media.retimeProcess = RetimeProcess::FrameBlend;
        media.id = tl.allocateId();
        const quint64 mediaClip = media.id;
        tl.videoTracks[0].clips.push_back(media);
        ClipData red;
        red.id = tl.allocateId();
        red.kind = ClipKind::Solid;
        red.solidColor = Qt::red;
        red.start = kGapEnd;
        red.duration = 60;
        tl.videoTracks[0].clips.push_back(red);
        ClipData sound = media; // its sound over the whole timeline
        sound.id = tl.allocateId();
        sound.duration = 150;
        sound.speed = 1.0;
        sound.retimeProcess = RetimeProcess::ProjectDefault;
        tl.audioTracks[1].clips.push_back(sound);
        m_project->endEdit();
        if (nested) {
            // Clip > New Compound Clip on the media clip: a Nested clip over [0, 30).
            QString error;
            QVERIFY2(m_project->createCompoundClip({mediaClip}, QStringLiteral("Introduction"),
                         {}, &error),
                qPrintable(error));
            const auto& clips = m_project->timeline()->data().videoTracks[0].clips;
            QCOMPARE(clips.front().kind, ClipKind::Nested);
            QCOMPARE(clips.front().end(), kClipEnd);
        }
    }

    std::unique_ptr<PlaybackEngine> engineFor()
    {
        auto e = makeEngine();
        e->setMediaPool(m_project->mediaPool());
        e->setTimeline(m_project->timeline());
        if (qEnvironmentVariableIsSet("KK_GAP_TEXTURES"))
            e->setOutputClient(this, QSize(320, 180), true);
        return e;
    }

    /// Plays from @p from until the position reaches @p until; @p seen gets what each
    /// presented frame showed.
    void play(PlaybackEngine& e, int64_t from, int64_t until, std::map<int64_t, Seen>& seen)
    {
        auto conn = connect(&e, &PlaybackEngine::frameReady, this, [&](const RenderedFramePtr& f) {
            if (f && f->ok && e.isPlaying())
                seen[f->frame] = {isEmptyPicture(f), test::readFrameIndex(pictureOf(f))};
        });
        m_project->timeline()->setPlayhead(from);
        QTRY_VERIFY_WITH_TIMEOUT(e.currentFrame() && e.currentFrame()->frame == from, 5000);
        e.play();
        QTRY_VERIFY_WITH_TIMEOUT(e.position() >= until, 20000);
        e.pause();
        disconnect(conn);
    }

    void checkGap(const std::map<int64_t, Seen>& seen)
    {
        int inGap = 0;
        for (const auto& [frame, s] : seen) {
            if (frame < kClipEnd || frame >= kGapEnd)
                continue;
            ++inGap;
            QVERIFY2(s.empty,
                qPrintable(QStringLiteral("frame %1 in the gap shows a picture (media frame %2)")
                        .arg(frame)
                        .arg(s.index)));
        }
        QVERIFY2(inGap >= 5, qPrintable(QString::number(inGap)));
    }

    void checkStill(PlaybackEngine& e, int64_t frame)
    {
        m_project->timeline()->setPlayhead(frame);
        QTRY_VERIFY_WITH_TIMEOUT(e.currentFrame() && e.currentFrame()->frame == frame, 5000);
        QVERIFY2(isEmptyPicture(e.currentFrame()),
            qPrintable(QStringLiteral("still at %1 shows a picture (media frame %2)")
                    .arg(frame)
                    .arg(test::readFrameIndex(pictureOf(e.currentFrame())))));
    }

private Q_SLOTS:
    void initTestCase()
    {
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
    }

    void init()
    {
        m_project = std::make_unique<Project>();
        const ImportResult imported = m_project->importMedia({test::fixture("h264_25fps.mp4")});
        QCOMPARE(imported.importedIds.size(), 1);
        m_media = imported.importedIds[0];
        m_mediaUs = m_project->mediaPool()->item(m_media)->info.durationUs;
        QVERIFY(m_mediaUs > 4'000'000); // longer than the clip: there is media past its end
    }

    void cleanup() { m_project.reset(); }

    void playAcrossClipEndIntoGap_data()
    {
        QTest::addColumn<int>("variant");
        QTest::newRow("media") << int(Plain);
        QTest::newRow("compound") << int(Compound);
        QTest::newRow("retimed") << int(Retimed);
        QTest::newRow("frame blend") << int(Blended);
    }

    void playAcrossClipEndIntoGap()
    {
        QFETCH(int, variant);
        buildTimeline(variant);
        if (QTest::currentTestFailed())
            return;
        auto e = engineFor();
        std::map<int64_t, Seen> seen;
        play(*e, 20, 70, seen);
        if (QTest::currentTestFailed())
            return;
        // The clip itself showed its media (the fixture is right).
        bool clipShown = false;
        for (const auto& [frame, s] : seen)
            clipShown = clipShown || (frame < kClipEnd && !s.empty);
        QVERIFY(clipShown);
        checkGap(seen);
        // The paused frame in the gap is black too.
        QTRY_VERIFY_WITH_TIMEOUT(
            e->currentFrame() && e->currentFrame()->frame == e->position(), 5000);
        QVERIFY(isEmptyPicture(e->currentFrame()));
    }

    void seekIntoGap_data() { playAcrossClipEndIntoGap_data(); }

    void seekIntoGap()
    {
        QFETCH(int, variant);
        buildTimeline(variant);
        if (QTest::currentTestFailed())
            return;
        auto e = engineFor();
        m_project->timeline()->setPlayhead(10); // a picture first
        QTRY_VERIFY_WITH_TIMEOUT(e->currentFrame() && e->currentFrame()->frame == 10, 5000);
        QVERIFY(!isEmptyPicture(e->currentFrame()));
        checkStill(*e, 45);
        checkStill(*e, kClipEnd); // the first frame after the clip
        checkStill(*e, kGapEnd - 1);
        // Playing from inside the gap.
        std::map<int64_t, Seen> seen;
        play(*e, 40, 80, seen);
        if (QTest::currentTestFailed())
            return;
        checkGap(seen);
        // A seek into the gap while playing.
        m_project->timeline()->setPlayhead(5);
        QTRY_VERIFY_WITH_TIMEOUT(e->currentFrame() && e->currentFrame()->frame == 5, 5000);
        e->play();
        QTest::qWait(200);
        m_project->timeline()->setPlayhead(50);
        QTRY_VERIFY_WITH_TIMEOUT(e->currentFrame() && e->currentFrame()->frame >= 50, 5000);
        QVERIFY(isEmptyPicture(e->currentFrame()));
        e->pause();
    }

    /// The gap is made by editing: a long clip is split and its middle lifted (paused, and
    /// while playing), then undone and redone; then the first part becomes a compound clip and
    /// its tab is opened and closed again.
    void gapMadeByEditing()
    {
        m_project->beginEdit(QStringLiteral("long"));
        TimelineData& tl = m_project->timeline()->mutableData();
        tl = makeDefaultTimeline(QStringLiteral("gap"), {25, 1}, 320, 180);
        ClipData media;
        media.kind = ClipKind::Media;
        media.mediaId = m_media;
        media.sourceDurationUs = m_mediaUs;
        media.duration = 150;
        media.id = tl.allocateId();
        tl.videoTracks[0].clips.push_back(media);
        m_project->endEdit();
        auto e = engineFor();
        std::map<int64_t, Seen> seen;
        play(*e, 20, 70, seen); // the decoders have been through the later gap
        QVERIFY(!seen.empty());

        auto lift = [&] {
            m_project->beginEdit(QStringLiteral("lift"));
            TimelineData& d = m_project->timeline()->mutableData();
            editops::splitAt(d, kClipEnd);
            editops::splitAt(d, kGapEnd);
            const quint64 middle = d.videoTracks[0].clips[1].id;
            QCOMPARE(editops::liftClips(d, {middle}), 1);
            m_project->endEdit();
        };
        lift();
        checkStill(*e, 45);
        seen.clear();
        play(*e, 20, 70, seen);
        checkGap(seen);

        m_project->undo();
        m_project->redo();
        seen.clear();
        play(*e, 20, 70, seen);
        checkGap(seen);

        // Lift while playing.
        m_project->undo();
        m_project->timeline()->setPlayhead(10);
        QTRY_VERIFY_WITH_TIMEOUT(e->currentFrame() && e->currentFrame()->frame == 10, 5000);
        e->play();
        QTest::qWait(150);
        lift();
        std::map<int64_t, Seen> during;
        auto conn = connect(e.get(), &PlaybackEngine::frameReady, this,
            [&](const RenderedFramePtr& f) {
                if (f && f->ok && e->isPlaying())
                    during[f->frame] = {isEmptyPicture(f), test::readFrameIndex(pictureOf(f))};
            });
        QTRY_VERIFY_WITH_TIMEOUT(e->position() >= 70, 20000);
        e->pause();
        disconnect(conn);
        checkGap(during);

        // Compound clip of the first part, open its tab, back to the parent.
        const quint64 parent = m_project->activeTimelineUid();
        const quint64 first = m_project->timeline()->data().videoTracks[0].clips[0].id;
        const quint64 inner
            = m_project->createCompoundClip({first}, QStringLiteral("Introduction"));
        QVERIFY(inner);
        QVERIFY(m_project->setActiveTimeline(inner));
        checkStill(*e, 45); // inside the compound timeline: its content ends at 30
        QVERIFY(m_project->setActiveTimeline(parent));
        seen.clear();
        play(*e, 20, 70, seen);
        checkGap(seen);
        checkStill(*e, 50);
    }

    /// A compound clip is cached by the Smart render cache (its chunks cover part of the gap).
    void cachedCompound()
    {
        buildTimeline(Compound);
        if (QTest::currentTestFailed())
            return;
        auto e = engineFor(); // also creates the shared compositor
        QTemporaryDir dir;
        RenderCacheManager cache(m_project.get(), dir.path());
        cache.setCompositor(g_compositor.get());
        MediaCacheSettings st = m_project->cacheSettings();
        st.renderCacheMode = RenderCacheMode::Smart;
        st.backgroundCache = false;
        m_project->setCacheSettings(st);
        cache.refresh();
        cache.renderAllNow();
        QVERIFY(cache.waitForIdle(60000));
        QVERIFY(cache.isCached(5));
        QVERIFY(cache.isCached(40));
        connect(&cache, &RenderCacheManager::viewChanged, e.get(), &PlaybackEngine::setRenderCache);
        e->setRenderCache(cache.view());
        checkStill(*e, 45);
        std::map<int64_t, Seen> seen;
        play(*e, 20, 70, seen);
        checkGap(seen);
    }

    /// A compound clip longer than its content (the content was shortened in its tab): the
    /// frames past the content's end are black.
    void compoundLongerThanContent()
    {
        buildTimeline(Compound);
        if (QTest::currentTestFailed())
            return;
        const quint64 parent = m_project->activeTimelineUid();
        const quint64 inner = m_project->timeline()->data().videoTracks[0].clips[0].nestedUid;
        auto e = engineFor();
        QVERIFY(m_project->setActiveTimeline(inner));
        m_project->beginEdit(QStringLiteral("trim"));
        m_project->timeline()->mutableData().videoTracks[0].clips[0].duration = 15;
        m_project->endEdit();
        QVERIFY(m_project->setActiveTimeline(parent));
        checkStill(*e, 20);
        std::map<int64_t, Seen> seen;
        play(*e, 5, 40, seen);
        for (const auto& [frame, s] : seen)
            if (frame >= 15 && frame < kClipEnd)
                QVERIFY2(s.empty, qPrintable(QString::number(frame)));
    }

    /// Owner report (0.4.0): after many ripple trims the viewer kept showing an earlier clip
    /// in a gap. Many ripple trims (head and tail, paused on the edit and while playing, some
    /// opening a gap under or after the playhead); after each one the picture at the playhead
    /// must match the timeline: black where no clip covers it.
    void manyRippleTrims()
    {
        m_project->beginEdit(QStringLiteral("clips"));
        TimelineData& tl = m_project->timeline()->mutableData();
        tl = makeDefaultTimeline(QStringLiteral("gap"), {25, 1}, 320, 180);
        for (int i = 0; i < 4; ++i) {
            ClipData c;
            c.kind = ClipKind::Media;
            c.mediaId = m_media;
            c.sourceDurationUs = m_mediaUs;
            c.sourceInUs = 500'000 + i * 1'200'000;
            c.start = i * 25;
            c.duration = 25;
            c.id = tl.allocateId();
            tl.videoTracks[0].clips.push_back(c);
        }
        ClipData red;
        red.id = tl.allocateId();
        red.kind = ClipKind::Solid;
        red.solidColor = Qt::red;
        red.start = 160;
        red.duration = 60;
        tl.videoTracks[0].clips.push_back(red);
        m_project->endEdit();
        auto e = engineFor();
        QTemporaryDir cacheDir;
        std::unique_ptr<RenderCacheManager> cache;
        if (qEnvironmentVariableIsSet("KK_GAP_CACHE")) {
            m_project->beginEdit(QStringLiteral("cache"));
            for (ClipData& c : m_project->timeline()->mutableData().videoTracks[0].clips)
                c.cacheOutput = true;
            m_project->endEdit();
            cache = std::make_unique<RenderCacheManager>(m_project.get(), cacheDir.path());
            cache->setCompositor(g_compositor.get());
            MediaCacheSettings st = m_project->cacheSettings();
            st.renderCacheMode = RenderCacheMode::Smart;
            st.backgroundCache = true;
            st.backgroundCacheDelaySec = 0;
            m_project->setCacheSettings(st);
            connect(cache.get(), &RenderCacheManager::viewChanged, e.get(),
                &PlaybackEngine::setRenderCache);
            cache->renderAllNow();
            QVERIFY(cache->waitForIdle(60000));
            e->setRenderCache(cache->view());
        }

        const auto covered = [this](int64_t frame) {
            return m_project->timeline()->data().videoTracks[0].clipAt(frame) != nullptr;
        };
        const auto checkShown = [&](int step) {
            const int64_t at = e->position();
            QTRY_VERIFY_WITH_TIMEOUT(
                e->currentFrame() && e->currentFrame()->frame == at, 5000);
            const bool empty = isEmptyPicture(e->currentFrame());
            QVERIFY2(empty != covered(at),
                qPrintable(QStringLiteral("step %1, frame %2: %3 (media frame %4)")
                        .arg(step)
                        .arg(at)
                        .arg(empty ? QStringLiteral("black over a clip")
                                   : QStringLiteral("a picture in a gap"))
                        .arg(test::readFrameIndex(pictureOf(e->currentFrame())))));
        };

        QRandomGenerator rng(4711);
        for (int step = 0; step < 60; ++step) {
            const auto& clips = m_project->timeline()->data().videoTracks[0].clips;
            const int count = int(clips.size()) - 1; // not the red solid
            if (count <= 0)
                break;
            const ClipData clip = clips[size_t(rng.bounded(count))];
            const bool head = rng.bounded(2) == 0;
            int64_t delta = rng.bounded(1, 9) * (rng.bounded(3) == 0 ? -1 : 1);
            if (delta >= clip.duration)
                delta = -delta;
            const bool playing = step % 3 == 2;
            // The playhead on the edit point (the frame that becomes a gap or not).
            const int64_t edge = head ? clip.start : clip.end() - 1;
            m_project->timeline()->setPlayhead(std::max<int64_t>(0, edge));
            QTRY_VERIFY_WITH_TIMEOUT(
                e->currentFrame() && e->currentFrame()->frame == e->position(), 5000);
            if (playing) {
                e->play();
                QTest::qWait(80);
            }
            // Like a trim drag in the timeline: the edit stays open, every mouse move restores
            // the original and re-applies the total delta, the timeline notifies each time.
            const TimelineData origin = m_project->timeline()->data();
            m_project->beginEdit(QStringLiteral("Ripple Trim"));
            const int moves = int(std::abs(delta));
            for (int m = 1; m <= moves; ++m) {
                m_project->timeline()->mutableData() = origin;
                TimelineData& d = m_project->timeline()->mutableData();
                const int64_t part = delta > 0 ? m : -m;
                if (head)
                    (void)editops::trimHead(d, clip.id, part, true);
                else
                    (void)editops::trimTail(d, clip.id, part, true);
                m_project->timeline()->notifyChanged();
                QTest::qWait(rng.bounded(1, 25));
            }
            m_project->endEdit();
            if (playing) {
                QTest::qWait(120);
                e->pause();
            }
            checkShown(step);
            if (QTest::currentTestFailed())
                return;
            // A seek into the gap after the last clip.
            const int64_t gapAt = clips[size_t(count) - 1].end() + 3;
            if (gapAt < 160) {
                m_project->timeline()->setPlayhead(gapAt);
                checkShown(step);
                if (QTest::currentTestFailed())
                    return;
            }
        }
        // Finally play through the last clips into the gap.
        const auto& clips = m_project->timeline()->data().videoTracks[0].clips;
        const int64_t lastEnd = clips[clips.size() - 2].end();
        std::map<int64_t, Seen> seen;
        play(*e, std::max<int64_t>(0, lastEnd - 10), std::min<int64_t>(159, lastEnd + 30), seen);
        int inGap = 0;
        for (const auto& [frame, s] : seen) {
            if (covered(frame))
                continue;
            ++inGap;
            QVERIFY2(s.empty, qPrintable(QStringLiteral("frame %1 in the gap shows media frame %2")
                                             .arg(frame)
                                             .arg(s.index)));
        }
        QVERIFY(inGap > 5);
    }

    /// A change inside a compound clip's timeline that leaves the parent's own clips as they
    /// are (here: Clip Attributes of the media used inside it) reaches the parent's playback.
    /// The engine took such changes for sound-only edits and kept the old pictures.
    void nestedChangeReachesParent()
    {
        buildTimeline(Compound);
        if (QTest::currentTestFailed())
            return;
        auto e = engineFor();
        m_project->timeline()->setPlayhead(10);
        QTRY_VERIFY_WITH_TIMEOUT(e->currentFrame() && e->currentFrame()->frame == 10, 5000);
        const QImage before = pictureOf(e->currentFrame());
        QVERIFY(test::readFrameIndex(before) >= 0);
        const auto parentTracks = m_project->timeline()->data().videoTracks;

        ClipAttributes upsideDown;
        upsideDown.rotation = 180;
        m_project->beginEdit(QStringLiteral("Clip Attributes"));
        QVERIFY(m_project->mediaPool()->setItemAttributes(m_media, upsideDown));
        m_project->endEdit();
        QVERIFY(m_project->timeline()->data().videoTracks == parentTracks);
        QTRY_VERIFY_WITH_TIMEOUT(e->currentFrame() && e->currentFrame()->frame == 10
                && pictureOf(e->currentFrame()) != before,
            5000);
        QCOMPARE(test::readFrameIndex(pictureOf(e->currentFrame())), -1); // upside down
    }
};

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication app(argc, argv);
    int result = 0;
    {
        TestPlaybackGap test;
        result = QTest::qExec(&test, argc, argv);
    }
    g_compositor.reset();
    return result;
}

#include "test_playbackgap.moc"
