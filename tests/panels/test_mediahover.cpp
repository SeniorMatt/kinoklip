// SPDX-License-Identifier: GPL-3.0-or-later
// Media Pool Live Media Preview (spec 66): the pointer's place over a thumbnail maps to the
// clip's length, the source viewer shows that frame and gets its own picture back on leaving,
// the options-menu switch, a playing viewer is left alone, and fast moves ask only for the
// newest frame.
#include "../media/MediaTestUtil.h"
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "media/MediaCache.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/Resources.h"
#include "ui/mediapool/MediaHoverPreview.h"
#include "ui/mediapool/MediaPoolDelegate.h"
#include "ui/mediapool/MediaPoolModel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QListView>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int64_t kClipUs = 10'000'000; // h264_25fps.mp4

/// A media pool, a source viewer and the preview between them, with a video and a still.
struct Hover : PanelFixture
{
    QString video;
    QString still;
    std::unique_ptr<MediaPoolPanel> pool;
    std::unique_ptr<ViewerPanel> viewer;
    MediaHoverPreview* preview = nullptr;

    Hover()
    {
        const auto result = project.importMedia(
            {kk::test::fixture("h264_25fps.mp4"), kk::test::fixture("still.jpg")});
        video = result.importedIds.value(0);
        still = result.importedIds.value(1);
        pool = std::make_unique<MediaPoolPanel>(ctx);
        pool->resize(700, 400);
        pool->show();
        viewer = std::make_unique<ViewerPanel>(ViewerPanel::Mode::Source, ctx);
        viewer->resize(640, 480);
        viewer->show();
        preview = pool->showLivePreviewIn(viewer.get());
        QApplication::processEvents();
    }
    ~Hover()
    {
        pool.reset();
        viewer.reset();
    }

    [[nodiscard]] QListView* grid() const
    {
        return pool->findChild<QListView*>(QStringLiteral("MediaPoolGrid"));
    }
    /// Viewport point at @p fraction along the poster of item @p id.
    [[nodiscard]] QPoint posterPoint(const QString& id, double fraction) const
    {
        QListView* view = grid();
        for (int row = 0; row < view->model()->rowCount(); ++row) {
            const QModelIndex index = view->model()->index(row, 0);
            if (index.data(MediaPoolModel::IdRole).toString() != id)
                continue;
            const QRect area = MediaPoolDelegate::posterRect(view->visualRect(index),
                static_cast<MediaPoolDelegate*>(view->itemDelegate())->posterSize());
            return {area.left() + int(fraction * (area.width() - 1)), area.center().y()};
        }
        return {};
    }
    [[nodiscard]] QImage picture() const
    {
        return viewer->surface()->widget()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    }
};

} // namespace

class TestMediaHover : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_mediahover"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init()
    {
        QSettings().clear();
        media::MediaCache::instance().clearMemoryCache();
    }

    void pointerPlaceMapsAlongThePoster()
    {
        QString why;
        if (!kk::test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Hover f;
        QVERIFY(f.grid());
        QSignalSpy requested(f.pool.get(), &MediaPoolPanel::livePreviewRequested);
        QSignalSpy ended(f.pool.get(), &MediaPoolPanel::livePreviewEnded);
        f.pool->hoverAt(f.grid(), f.posterPoint(f.video, 0.0));
        f.pool->hoverAt(f.grid(), f.posterPoint(f.video, 0.5));
        f.pool->hoverAt(f.grid(), f.posterPoint(f.video, 1.0));
        QCOMPARE(requested.size(), 3);
        QCOMPARE(requested[0][1].toLongLong(), qint64(kClipUs));
        QVERIFY(qAbs(requested[0][2].toDouble() - 0.0) < 0.01);
        QVERIFY(qAbs(requested[1][2].toDouble() - 0.5) < 0.02);
        QVERIFY(qAbs(requested[2][2].toDouble() - 1.0) < 0.01);
        QCOMPARE(ended.size(), 0);
        // Below the poster (the name line) is not the thumbnail.
        QPoint name = f.posterPoint(f.video, 0.5);
        name.setY(f.grid()->visualRect(f.grid()->indexAt(name)).bottom() - 3);
        f.pool->hoverAt(f.grid(), name);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(requested.size(), 3);
        // A still image previews with duration 0.
        f.pool->hoverAt(f.grid(), f.posterPoint(f.still, 0.3));
        QCOMPARE(requested.size(), 4);
        QCOMPARE(requested[3][1].toLongLong(), qint64(0));
    }

    void viewerShowsTheFrameAndLeavingRestoresIt()
    {
        QString why;
        if (!kk::test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Hover f;
        f.viewer->setSourceMedia(f.video);
        PlaybackEngine* engine = f.viewer->engine();
        QVERIFY(engine);
        engine->seek(10);
        QTRY_VERIFY(
            f.picture().pixelColor(f.viewer->surface()->pictureRect().center()).lightness() > 20);
        QTest::qWait(200);
        const QImage before = f.picture();

        f.pool->hoverAt(f.grid(), f.posterPoint(f.still, 0.5));
        QTRY_VERIFY(f.preview->showing());
        QVERIFY(f.viewer->showsTemporaryFrame());
        QVERIFY(f.picture() != before);
        QCOMPARE(f.viewer->sourceMedia(), f.video); // the clip stays loaded

        f.pool->endHover();
        QVERIFY(!f.preview->showing());
        QVERIFY(!f.viewer->showsTemporaryFrame());
        QCOMPARE(f.viewer->sourceMedia(), f.video);
        QCOMPARE(engine->position(), int64_t(10));
        QTRY_COMPARE(f.picture(), before);
    }

    void positionMapsToTheClipTime()
    {
        QString why;
        if (!kk::test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Hover f;
        f.preview->hover(kk::test::fixture("h264_25fps.mp4"), kClipUs, 0.5);
        QCOMPARE(f.preview->wantedTimeUs(), int64_t(5'000'000));
        QCOMPARE(f.preview->requestedTimes().value(0), int64_t(5'000'000));
        f.preview->hover(kk::test::fixture("h264_25fps.mp4"), kClipUs, 2.0); // clamped
        QVERIFY(f.preview->wantedTimeUs() < kClipUs);
        QVERIFY(f.preview->wantedTimeUs() >= kClipUs - 40'000);
        f.preview->hover(kk::test::fixture("h264_25fps.mp4"), kClipUs, -1.0);
        QCOMPARE(f.preview->wantedTimeUs(), int64_t(0));
    }

    void optionOffDoesNothing()
    {
        QString why;
        if (!kk::test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Hover f;
        QAction* option = f.pool->liveMediaPreviewAction();
        QVERIFY(option);
        QCOMPARE(option->text(), QStringLiteral("Live Media Preview"));
        QVERIFY(option->isCheckable());
        QVERIFY(option->isChecked()); // on by default
        QSignalSpy requested(f.pool.get(), &MediaPoolPanel::livePreviewRequested);
        QSignalSpy ended(f.pool.get(), &MediaPoolPanel::livePreviewEnded);
        f.pool->hoverAt(f.grid(), f.posterPoint(f.video, 0.5));
        QCOMPARE(requested.size(), 1);
        option->setChecked(false); // switching off ends a preview in progress
        QCOMPARE(ended.size(), 1);
        QVERIFY(!prefs::mediaLivePreview());
        f.pool->hoverAt(f.grid(), f.posterPoint(f.video, 0.5));
        QCOMPARE(requested.size(), 1);
        QVERIFY(!f.preview->showing());
        // Remembered: a new panel starts with the option off, then on again.
        MediaPoolPanel again(f.ctx);
        QVERIFY(!again.liveMediaPreviewEnabled());
        option->setChecked(true);
        QVERIFY(prefs::mediaLivePreview());
    }

    void aPlayingViewerIgnoresTheHover()
    {
        QString why;
        if (!kk::test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Hover f;
        f.viewer->setSourceMedia(f.video);
        PlaybackEngine* engine = f.viewer->engine();
        QVERIFY(engine);
        engine->play();
        QVERIFY(engine->isPlaying());
        f.pool->hoverAt(f.grid(), f.posterPoint(f.still, 0.5));
        QVERIFY(f.preview->requestedTimes().isEmpty());
        QVERIFY(!f.preview->showing());
        QVERIFY(!f.viewer->showsTemporaryFrame());
        // Playback starting during a preview ends it.
        engine->pause();
        f.pool->hoverAt(f.grid(), f.posterPoint(f.still, 0.5));
        QTRY_VERIFY(f.preview->showing());
        engine->play();
        QTRY_VERIFY(!f.preview->showing());
        QVERIFY(!f.viewer->showsTemporaryFrame());
        engine->pause();
    }

    void fastMovesAskOnlyForTheNewestFrame()
    {
        QString why;
        if (!kk::test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Hover f;
        const QString path = kk::test::fixture("h264_25fps.mp4");
        for (int i = 0; i <= 40; ++i)
            f.preview->hover(path, kClipUs, i / 40.0);
        // One decode in flight, the rest only moved the wish.
        QCOMPARE(f.preview->requestedTimes().size(), 1);
        const int64_t last = f.preview->wantedTimeUs();
        QVERIFY(last > 9'000'000);
        QTRY_VERIFY(f.preview->showing());
        QCOMPARE(f.preview->requestedTimes().size(), 2);
        QCOMPARE(f.preview->requestedTimes().last(), last);
    }
};

QTEST_MAIN(TestMediaHover)
#include "test_mediahover.moc"
