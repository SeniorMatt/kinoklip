// SPDX-License-Identifier: GPL-3.0-or-later
// Effects library Live Preview (spec 47): hovering an entry shows it applied in the timeline
// viewer, leaving restores the picture exactly, the project stays untouched, the options-menu
// toggle, stale requests, and the preview scenes (which clip, which frame).
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/AudioTransitions.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/Preferences.h"
#include "core/Timeline.h"
#include "core/TitlePresets.h"
#include "core/VideoEffectKeys.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/effects/EffectsLivePreview.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>

#include <memory>

using namespace kk;
using namespace kk::test;

namespace {

const QString kInvert = QString::fromLatin1(fxid::Invert);
const QString kBlackWhite = QString::fromLatin1(fxid::BlackWhite);
const QString kDissolve = QStringLiteral("cross_dissolve");

/// The panel fixture with the program engine (no sound), a 320x180 timeline with a red solid
/// (frames 0-100) and a blue one (100-200) on V1, and the Edit page's wiring: library ->
/// EffectsLivePreview -> timeline viewer.
struct Fixture : PanelFixture
{
    std::unique_ptr<PlaybackEngine> engine;
    std::unique_ptr<ViewerPanel> viewer;
    std::unique_ptr<EffectsLibraryPanel> library;
    std::unique_ptr<EffectsLivePreview> preview;
    quint64 red = 0;
    quint64 blue = 0;

    Fixture()
    {
        PlaybackEngine::Options options;
        options.noAudio = true;
        engine = std::make_unique<PlaybackEngine>(std::move(options));
        engine->setMediaPool(project.mediaPool());
        engine->setTimeline(project.timeline());
        ctx.playback = engine.get();

        TimelineData tl = makeDefaultTimeline(QStringLiteral("Demo"), {25, 1}, 320, 180, 2, 2);
        auto solid = [&](QColor color, int64_t start) {
            ClipData c;
            c.id = tl.allocateId();
            c.kind = ClipKind::Solid;
            c.solidColor = color;
            c.start = start;
            c.duration = 100;
            tl.videoTracks[0].clips.push_back(c);
            return c.id;
        };
        red = solid(Qt::red, 0);
        blue = solid(Qt::blue, 100);
        project.timeline()->setData(tl);
        project.timeline()->setPlayhead(50);
        project.undoStack()->clear();

        viewer = std::make_unique<ViewerPanel>(ViewerPanel::Mode::Timeline, ctx);
        viewer->resize(640, 480);
        viewer->show();
        library = std::make_unique<EffectsLibraryPanel>(ctx);
        library->resize(420, 900);
        library->show();
        preview = std::make_unique<EffectsLivePreview>(ctx, viewer.get());
        QObject::connect(library.get(), &EffectsLibraryPanel::livePreviewRequested, preview.get(),
            &EffectsLivePreview::hover);
        QObject::connect(library.get(), &EffectsLibraryPanel::livePreviewEnded, preview.get(),
            &EffectsLivePreview::stop);
    }

    ~Fixture()
    {
        preview.reset();
        library.reset();
        viewer.reset();
    }

    [[nodiscard]] QImage picture() const
    {
        return viewer->surface()->widget()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    }
    [[nodiscard]] QColor centre() const
    {
        const QRect r = viewer->surface()->pictureRect();
        return picture().pixelColor(r.center());
    }
    /// Waits until the viewer shows the normal program picture (red at frame 50).
    bool waitForProgram() const
    {
        return QTest::qWaitFor([&] { return centre().red() > 200 && centre().blue() < 50; }, 5000);
    }

    /// Viewport point at @p fraction (0..1) along the library row of @p descriptorId.
    [[nodiscard]] QPoint rowPoint(const QString& descriptorId, double fraction = 0.5) const
    {
        QTreeWidget* tree = library->tree();
        QTreeWidgetItemIterator it(tree);
        for (; *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() != descriptorId)
                continue;
            tree->scrollToItem(*it);
            QCoreApplication::processEvents();
            const QRect r = tree->visualItemRect(*it);
            return QPoint(r.left() + int(fraction * (r.width() - 1)), r.center().y());
        }
        return {};
    }
};

/// Pixels that differ between two images of the same size.
int differingPixels(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return -1;
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x)
            count += a.pixel(x, y) != b.pixel(x, y) ? 1 : 0;
    }
    return count;
}

} // namespace

class TestLivePreview : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_livepreview"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // 47.1, 47.2, 47.6: hovering a video effect shows it on the clip under the playhead;
    // leaving the entry restores the picture exactly; project, dirty flag and undo stack are
    // untouched.
    void effectHoverShowsAndLeaveRestores()
    {
        Fixture f;
        QVERIFY(QTest::qWaitForWindowExposed(f.viewer.get()));
        QVERIFY(f.waitForProgram());
        const QImage before = f.picture();
        const TimelineData dataBefore = f.project.timeline()->data();
        const int undoCount = f.project.undoStack()->count();
        QVERIFY(!f.project.isDirty());

        QSignalSpy shown(f.preview.get(), &EffectsLivePreview::previewShown);
        const QPoint at = f.rowPoint(kInvert);
        QVERIFY(!at.isNull());
        QTest::mouseMove(f.library->tree()->viewport(), at); // through the event filter
        QCOMPARE(f.preview->hoveredId(), kInvert);
        QTRY_COMPARE_WITH_TIMEOUT(f.preview->shownId(), kInvert, 5000);
        QVERIFY(f.viewer->showsTemporaryFrame());
        // Inverted red: cyan.
        const QColor c = f.centre();
        QVERIFY2(c.red() < 60 && c.green() > 200 && c.blue() > 200, qPrintable(c.name()));
        QVERIFY(differingPixels(before, f.picture()) > 0);
        QCOMPARE(shown.count(), 1);

        // The pointer leaves the list: the normal picture, pixel for pixel.
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(f.library->tree()->viewport(), &leave);
        QVERIFY(f.preview->shownId().isEmpty());
        QVERIFY(!f.viewer->showsTemporaryFrame());
        QCOMPARE(differingPixels(before, f.picture()), 0);

        QVERIFY(!f.project.isDirty());
        QCOMPARE(f.project.undoStack()->count(), undoCount);
        QVERIFY(f.project.timeline()->data().videoTracks[0].clips[0].effects.isEmpty());
        QCOMPARE(f.project.timeline()->data().videoTracks.size(), dataBefore.videoTracks.size());
        QCOMPARE(f.project.timeline()->playhead(), int64_t(50));
    }

    // 47.1: the selected clip wins over the topmost one; nothing under the playhead = nothing.
    void effectTargetsTheSelectedClip()
    {
        Fixture f;
        TimelineData d = f.project.timeline()->data();
        ClipData top;
        top.id = d.allocateId();
        top.kind = ClipKind::Solid;
        top.solidColor = Qt::green;
        top.start = 40;
        top.duration = 20;
        d.videoTracks[1].clips.push_back(top);
        QCOMPARE(livepreview::targetClip(d, 50, {}), top.id);
        QCOMPARE(livepreview::targetClip(d, 50, {f.red}), f.red);
        QCOMPARE(livepreview::targetClip(d, 50, {f.blue}), top.id); // not under the playhead
        QCOMPARE(livepreview::targetClip(d, 500, {}), quint64(0));
        d.videoTracks[1].enabled = false;
        QCOMPARE(livepreview::targetClip(d, 50, {}), f.red);

        const auto scene = livepreview::buildScene(d, 50, {f.red}, kInvert, 0.5);
        QVERIFY(scene);
        QCOMPARE(scene->frame, int64_t(50));
        QCOMPARE(editops::clipById(*scene->timeline, f.red)->effects.size(), 1);
        QCOMPARE(editops::clipById(*scene->timeline, f.red)->effects[0].type, kInvert);
        QVERIFY(editops::clipById(d, f.red)->effects.isEmpty()); // the input is a copy
        QVERIFY(!livepreview::buildScene(d, 500, {}, kInvert, 0.5));
        // Nothing to show: audio entries, the Adjustment Clip, unknown ids.
        QVERIFY(!livepreview::buildScene(
            d, 50, {}, QString::fromLatin1(audiotransitions::kCrossFadePlus3dB), 0.5));
        QVERIFY(!livepreview::buildScene(d, 50, {}, generators::AdjustmentClip, 0.5));
        QVERIFY(!livepreview::buildScene(d, 50, {}, QStringLiteral("no_such_effect"), 0.5));
    }

    // 47.3, 47.5: a transition is shown at the edit nearest the playhead, scrubbed by the
    // pointer's place along the row (the middle = the transition's midpoint).
    void transitionAtTheNearestEdit()
    {
        Fixture f;
        const TimelineData& d = f.project.timeline()->data();
        // Playhead 90: the red clip's tail is nearer, the cut at 100; 1 s centred = [88, 113).
        const auto mid = livepreview::buildScene(d, 90, {}, kDissolve, 0.5);
        QVERIFY(mid);
        const ClipData* incoming = editops::clipById(*mid->timeline, f.blue);
        QVERIFY(incoming->transitionIn.has_value());
        QCOMPARE(incoming->transitionIn->type, kDissolve);
        const auto window = editops::transitionWindow(*mid->timeline, f.blue, true);
        QVERIFY(window);
        QCOMPARE(window->second - window->first, int64_t(25));
        QCOMPARE(mid->frame, window->first + 12);
        const auto start = livepreview::buildScene(d, 90, {}, kDissolve, 0.0);
        const auto end = livepreview::buildScene(d, 90, {}, kDissolve, 1.0);
        QCOMPARE(start->frame, window->first);
        QCOMPARE(end->frame, window->second - 1);
        QVERIFY(!editops::clipById(d, f.blue)->transitionIn); // the project's data unchanged

        // In the viewer: half red, half blue at the midpoint.
        QVERIFY(QTest::qWaitForWindowExposed(f.viewer.get()));
        f.project.timeline()->setPlayhead(90);
        f.library->hoverAt(f.rowPoint(kDissolve, 0.5));
        QTRY_COMPARE_WITH_TIMEOUT(f.preview->shownId(), kDissolve, 5000);
        const QColor c = f.centre();
        QVERIFY2(
            c.red() > 60 && c.red() < 210 && c.blue() > 60 && c.blue() < 210, qPrintable(c.name()));
        // Moving to the row's left end shows the transition's start: mostly red.
        f.library->hoverAt(f.rowPoint(kDissolve, 0.0));
        QTRY_VERIFY_WITH_TIMEOUT(f.centre().red() > 200, 5000);
        QVERIFY(!f.project.isDirty());
        QCOMPARE(f.project.undoStack()->count(), 0);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(90));
    }

    // 47.4: titles and generators are laid over the current frame on a new top track; the
    // pointer's place scrubs through the title clip with the playhead's picture underneath.
    void titleOverlaysTheFrame()
    {
        Fixture f;
        const TimelineData& d = f.project.timeline()->data();
        const auto scene = livepreview::buildScene(d, 50, {}, titles::Text, 0.0);
        QVERIFY(scene);
        QCOMPARE(scene->frame, int64_t(50));
        QCOMPARE(scene->timeline->videoTracks.size(), d.videoTracks.size() + 1);
        const ClipData& title = scene->timeline->videoTracks.back().clips.front();
        QCOMPARE(title.kind, ClipKind::Title);
        QCOMPARE(title.start, int64_t(50)); // position 0: the playhead at the title's start
        const auto later = livepreview::buildScene(d, 50, {}, titles::Text, 0.5);
        QVERIFY(later->timeline->videoTracks.back().clips.front().start < 50);
        QVERIFY(later->timeline->videoTracks.back().clips.front().start >= 0);

        QVERIFY(QTest::qWaitForWindowExposed(f.viewer.get()));
        QVERIFY(f.waitForProgram());
        const QImage before = f.picture();
        f.library->hoverAt(f.rowPoint(titles::Text));
        QTRY_COMPARE_WITH_TIMEOUT(f.preview->shownId(), titles::Text, 5000);
        QVERIFY(differingPixels(before, f.picture()) > 0);
        // Red still shows around the text.
        const QRect r = f.viewer->surface()->pictureRect();
        QVERIFY(f.picture().pixelColor(r.topLeft() + QPoint(4, 4)).red() > 200);
        f.library->endHover();
        QCOMPARE(differingPixels(before, f.picture()), 0);
        QVERIFY(!f.project.isDirty());
        QCOMPARE(f.project.timeline()->data().videoTracks.size(), d.videoTracks.size());
    }

    // 47.7: the options menu's Live Preview toggle turns it off, and it is remembered.
    void toggleOffShowsNothing()
    {
        Fixture f;
        QVERIFY(f.library->livePreviewAction()->isChecked()); // default on
        f.library->livePreviewAction()->setChecked(false);
        QVERIFY(!prefs::effectsLivePreview());
        QSignalSpy requested(f.library.get(), &EffectsLibraryPanel::livePreviewRequested);
        f.library->hoverAt(f.rowPoint(kInvert));
        QCOMPARE(requested.count(), 0);
        QTest::qWait(EffectsLivePreview::kDebounceMs * 3);
        QVERIFY(f.preview->shownId().isEmpty());
        QVERIFY(!f.viewer->showsTemporaryFrame());
        {
            EffectsLibraryPanel other(f.ctx);
            QVERIFY(!other.livePreviewAction()->isChecked());
        }

        // Switching it off while a preview shows ends that preview.
        f.library->livePreviewAction()->setChecked(true);
        QVERIFY(prefs::effectsLivePreview());
        QVERIFY(QTest::qWaitForWindowExposed(f.viewer.get()));
        f.library->hoverAt(f.rowPoint(kInvert));
        QTRY_COMPARE_WITH_TIMEOUT(f.preview->shownId(), kInvert, 5000);
        f.library->livePreviewAction()->setChecked(false);
        QVERIFY(!f.viewer->showsTemporaryFrame());
        f.library->livePreviewAction()->setChecked(true);
    }

    // 47.8: a new entry waits for the pointer to rest; a request made stale by another entry
    // is cancelled and its picture never shown.
    void staleRequestsAreCancelled()
    {
        Fixture f;
        QVERIFY(QTest::qWaitForWindowExposed(f.viewer.get()));
        QSignalSpy shown(f.preview.get(), &EffectsLivePreview::previewShown);
        // Sweeping over entries renders nothing until the pointer rests.
        f.preview->hover(kInvert, 0.5);
        f.preview->hover(kBlackWhite, 0.5);
        QCOMPARE(f.preview->requestCount(), 0);
        f.preview->hover(kInvert, 0.5);
        f.preview->flushDebounce();
        QCOMPARE(f.preview->requestCount(), 1);
        // Another entry before the render was picked up: cancelled.
        f.preview->hover(kBlackWhite, 0.5);
        QCOMPARE(f.preview->cancelledCount(), 1);
        f.preview->flushDebounce();
        QTRY_COMPARE_WITH_TIMEOUT(f.preview->shownId(), kBlackWhite, 5000);
        QTest::qWait(50);
        QCOMPARE(shown.count(), 1);
        QCOMPARE(shown.front().front().toString(), kBlackWhite);
        // Black & White of red: grey.
        const QColor c = f.centre();
        QVERIFY2(std::abs(c.red() - c.green()) < 10 && std::abs(c.green() - c.blue()) < 10,
            qPrintable(c.name()));
        // stop() while a render runs: nothing is shown afterwards.
        f.preview->hover(kInvert, 0.5);
        f.preview->flushDebounce();
        f.preview->stop();
        QTest::qWait(200);
        QVERIFY(!f.viewer->showsTemporaryFrame());
        QVERIFY(f.preview->shownId().isEmpty());
    }

    // 47.2: an edit, playback or a drag ends the preview; a playhead move re-renders it.
    void editsAndPlaybackEndThePreview()
    {
        Fixture f;
        QVERIFY(QTest::qWaitForWindowExposed(f.viewer.get()));
        f.preview->hover(kInvert, 0.5);
        f.preview->flushDebounce();
        QTRY_COMPARE_WITH_TIMEOUT(f.preview->shownId(), kInvert, 5000);
        // Playhead move: still previewing, at the new frame (blue inverted: yellow).
        const int before = f.preview->requestCount();
        f.project.timeline()->setPlayhead(150);
        QCOMPARE(f.preview->requestCount(), before + 1);
        QTRY_VERIFY_WITH_TIMEOUT(f.centre().red() > 200 && f.centre().blue() < 60, 5000);
        QVERIFY(f.viewer->showsTemporaryFrame());

        // An edit (here: a clip added) ends it.
        f.addClip(TrackRef {TrackType::Video, 1}, 300, 10, ClipKind::Solid);
        QVERIFY(!f.viewer->showsTemporaryFrame());
        QVERIFY(f.preview->hoveredId().isEmpty());

        // Playback ends it.
        f.preview->hover(kInvert, 0.5);
        f.preview->flushDebounce();
        QTRY_COMPARE_WITH_TIMEOUT(f.preview->shownId(), kInvert, 5000);
        f.engine->play();
        QVERIFY(!f.viewer->showsTemporaryFrame());
        f.preview->hover(kBlackWhite, 0.5); // no preview while playing
        QVERIFY(f.preview->hoveredId().isEmpty());
        f.engine->pause();
    }
};

int main(int argc, char** argv)
{
    qputenv("KK_NO_AUDIO", "1");
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    TestLivePreview test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_livepreview.moc"
