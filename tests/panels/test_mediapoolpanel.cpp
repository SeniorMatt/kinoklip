// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel: bin + search filtering, bin editing (undoable), drag payload, lazy posters,
// offline flag, grid/list modes and a 2,000-item smoothness check.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/MediaPool.h"
#include "ui/common/DragMime.h"
#include "ui/common/Resources.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QListView>
#include <QMimeData>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;

namespace {

/// @p ids without the project's timeline items (the media pool lists "Timeline 1" too).
QStringList files(const Project& project, const QStringList& ids)
{
    QStringList result;
    for (const QString& id : ids) {
        const MediaItem* item = project.mediaPool()->item(id);
        if (item && !item->isTimeline())
            result.append(id);
    }
    return result;
}

/// Adds a fake (unprobed) media item; @p path decides the offline state.
QString addItem(Project& project, const QString& name, const QString& path,
    const QString& binId = {}, bool video = true)
{
    MediaItem item;
    item.path = path;
    item.name = name;
    item.binId = binId;
    item.info.valid = true;
    item.info.hasVideo = video;
    item.info.hasAudio = true;
    item.info.width = 1920;
    item.info.height = 1080;
    item.info.frameRate = {25, 1};
    item.info.durationUs = 10'000'000;
    item.info.videoCodec = QStringLiteral("h264");
    item.info.audioCodec = QStringLiteral("aac");
    item.info.audioChannels = 2;
    item.info.audioSampleRate = 48000;
    project.beginEdit(QStringLiteral("add"));
    const QString id = project.mediaPool()->addItem(item);
    project.endEdit();
    return id;
}

/// A coloured 16:9 test poster.
QImage fakePoster(const QString& path, int height)
{
    QImage image(height * 16 / 9, height, QImage::Format_ARGB32_Premultiplied);
    const uint hash = qHash(path);
    image.fill(QColor::fromHsv(int(hash % 360), 120, 170));
    QPainter p(&image);
    p.setPen(Qt::white);
    p.drawLine(0, height, height * 16 / 9, 0);
    return image;
}

} // namespace

class TestMediaPoolPanel : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_mediapoolpanel"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void binAndSearchFiltering()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString self = QCoreApplication::applicationFilePath(); // an existing file
        const QString a = addItem(f.project, QStringLiteral("Beach.mp4"), self);
        const QString bin = panel.createBin(QStringLiteral("Interviews"), {});
        const QString sub = panel.createBin(QStringLiteral("Day 2"), bin);
        const QString b = addItem(f.project, QStringLiteral("Anna take 1"), self, bin);
        const QString c = addItem(f.project, QStringLiteral("Anna take 2"), self, sub);

        QCOMPARE(files(f.project, panel.visibleMediaIds()),
            QStringList {a}); // Master shows root items only
        panel.setCurrentBin(bin);
        QCOMPARE(files(f.project, panel.visibleMediaIds()), QStringList {b});
        panel.setSearchText(QStringLiteral("ANNA"));
        QCOMPARE(files(f.project, panel.visibleMediaIds()),
            QStringList({b, c})); // search includes sub-bins
        panel.setSearchText(QStringLiteral("take 2"));
        QCOMPARE(files(f.project, panel.visibleMediaIds()), QStringList {c});
        panel.setCurrentBin({});
        panel.setSearchText(QStringLiteral("beach"));
        QCOMPARE(files(f.project, panel.visibleMediaIds()), QStringList {a});
        panel.setSearchText({});
        QCOMPARE(files(f.project, panel.visibleMediaIds()), QStringList {a});

        QVERIFY(panel.selectMedia(c)); // jumps to its bin
        QCOMPARE(panel.currentBin(), sub);
        QCOMPARE(panel.currentMediaId(), c);
        QCOMPARE(panel.selectedMediaIds(), QStringList {c});
    }

    void binEditingIsUndoable()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString self = QCoreApplication::applicationFilePath();
        const QString item = addItem(f.project, QStringLiteral("Clip"), self);
        const int commands = f.project.undoStack()->count();

        const QString bin = panel.createBin(QStringLiteral("B-Roll"), {});
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        QVERIFY(panel.renameBin(bin, QStringLiteral("Cutaways")));
        QCOMPARE(f.project.mediaPool()->bin(bin)->name, QStringLiteral("Cutaways"));
        QCOMPARE(panel.moveItemsToBin({item}, bin), 1);
        QCOMPARE(panel.moveItemsToBin({item}, bin), 0); // already there: no undo step
        QCOMPARE(f.project.mediaPool()->item(item)->binId, bin);
        panel.setCurrentBin(bin);
        QCOMPARE(files(f.project, panel.visibleMediaIds()), QStringList {item});

        QVERIFY(panel.deleteBin(bin)); // items move to the parent
        QCOMPARE(f.project.mediaPool()->item(item)->binId, QString());
        QCOMPARE(panel.currentBin(), QString()); // current bin vanished -> Master
        QCOMPARE(files(f.project, panel.visibleMediaIds()), QStringList {item});
        QCOMPARE(f.project.undoStack()->count(), commands + 4);

        f.project.undo();
        QCOMPARE(f.project.mediaPool()->item(item)->binId, bin);
        QCOMPARE(panel.deleteItems({item}), 1);
        QVERIFY(!f.project.mediaPool()->item(item));
        f.project.undo();
        QVERIFY(f.project.mediaPool()->item(item));
    }

    void dragPayloadAndRename()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString self = QCoreApplication::applicationFilePath();
        const QString a = addItem(f.project, QStringLiteral("One"), self);
        const QString b = addItem(f.project, QStringLiteral("Two"), self);
        MediaPoolModel* model = panel.model();
        std::unique_ptr<QMimeData> data(
            model->mimeData({model->index(1, 0), model->index(2, 0), model->index(2, 2)}));
        QCOMPARE(QString::fromUtf8(data->data(QLatin1String(mime::MediaItems))),
            a + QLatin1Char('\n') + b);

        QVERIFY(model->setData(model->index(1, 0), QStringLiteral("Renamed"), Qt::EditRole));
        QCOMPARE(f.project.mediaPool()->item(a)->name, QStringLiteral("Renamed"));
        QCOMPARE(model->index(1, 0).data().toString(), QStringLiteral("Renamed"));
        QCOMPARE(model->index(1, MediaPoolModel::ResolutionColumn).data().toString(),
            QStringLiteral("1920 x 1080"));
        QCOMPARE(
            model->index(1, MediaPoolModel::FpsColumn).data().toString(), QStringLiteral("25"));
        QCOMPARE(model->index(1, MediaPoolModel::DurationColumn).data().toString(),
            QStringLiteral("00:00:10:00"));
    }

    void offlineAndOpenSignal()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString missing
            = addItem(f.project, QStringLiteral("Gone"), QStringLiteral("/nonexistent/x.mp4"));
        QVERIFY(panel.model()->index(1, 0).data(MediaPoolModel::OfflineRole).toBool());
        QSignalSpy open(&panel, &MediaPoolPanel::openInSourceViewer);
        auto* grid = panel.findChild<QListView*>();
        emit grid->doubleClicked(grid->model()->index(1, 0));
        QCOMPARE(open.count(), 1);
        QCOMPARE(open.front().front().toString(), missing);
    }

    void relinkOfflineFromFolder()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        QTemporaryDir dir;
        addItem(f.project, QStringLiteral("Gone"), QStringLiteral("/nonexistent/clip_a.mp4"));
        addItem(f.project, QStringLiteral("Other"), QStringLiteral("/nonexistent/clip_b.mp4"));
        QFile file(dir.filePath(QStringLiteral("clip_a.mp4")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QCOMPARE(panel.relinkFromFolder(dir.path()), 1);
        QVERIFY(!panel.model()->index(1, 0).data(MediaPoolModel::OfflineRole).toBool());
        QVERIFY(panel.model()->index(2, 0).data(MediaPoolModel::OfflineRole).toBool());
        f.project.undo(); // one undo step
        QVERIFY(!QFileInfo::exists(f.project.mediaPool()->state().items[1].path));
    }

    void postersAreLazyAndAsync()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        QStringList asked;
        bool ready = false;
        panel.setThumbnailProvider([&](const QString& path, int height) {
            asked.append(path);
            return ready ? fakePoster(path, height) : QImage();
        });
        const QString self = QCoreApplication::applicationFilePath();
        addItem(f.project, QStringLiteral("Video"), self);
        QVERIFY(asked.isEmpty()); // nothing requested until painted
        const QModelIndex index = panel.model()->index(1, 0);
        QVERIFY(index.data(MediaPoolModel::PosterRole).value<QPixmap>().isNull());
        QCOMPARE(asked.size(), 1);
        QVERIFY(index.data(MediaPoolModel::PosterRole).value<QPixmap>().isNull());
        QCOMPARE(asked.size(), 1); // pending: not asked again per paint
        ready = true;
        QSignalSpy changed(panel.model(), &QAbstractItemModel::dataChanged);
        panel.model()->thumbnailReady(self);
        QCOMPARE(changed.count(), 1);
        const QPixmap poster = index.data(MediaPoolModel::PosterRole).value<QPixmap>();
        QVERIFY(!poster.isNull());
        QVERIFY(poster.width() <= MediaPoolModel::kPosterSize.width());
        index.data(MediaPoolModel::PosterRole);
        QCOMPARE(asked.size(), 2); // cached afterwards
    }

    void viewModesAndScreenshots()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider(fakePoster);
        const QString self = QCoreApplication::applicationFilePath();
        panel.createBin(QStringLiteral("Interviews"), {});
        panel.createBin(QStringLiteral("B-Roll"), {});
        const char* names[] = {"Harbour sunrise.mp4", "Interview Anna.mov", "Drone pass 03.mp4",
            "Street ambience.wav", "Logo.png", "Market wide.mp4", "Close-up hands.mp4"};
        int i = 0;
        for (const char* name : names) {
            const bool audio = QString::fromLatin1(name).endsWith(QLatin1String(".wav"));
            // Item 5 points to a missing file to show the offline marker.
            addItem(f.project, QLatin1String(name),
                i == 5 ? QStringLiteral("/missing/m.mp4") : self, {}, !audio);
            ++i;
        }
        panel.resize(640, 380);
        panel.setViewMode(MediaPoolPanel::ViewMode::Grid);
        saveScreenshot(panel, QStringLiteral("mediapool_grid"));
        panel.setViewMode(MediaPoolPanel::ViewMode::List);
        QCOMPARE(QSettings().value(QStringLiteral("mediaPool/viewMode")).toString(),
            QStringLiteral("list"));
        saveScreenshot(panel, QStringLiteral("mediapool_list"));
        panel.setViewMode(MediaPoolPanel::ViewMode::Grid);
    }

    void twoThousandItemsStaySmooth()
    {
        PanelFixture f;
        MediaPoolState state;
        for (int i = 0; i < 2000; ++i) {
            MediaItem item;
            item.id = QStringLiteral("m%1").arg(i);
            item.path = QStringLiteral("/fake/clip_%1.mp4").arg(i);
            item.name = QStringLiteral("clip_%1.mp4").arg(i);
            item.info.hasVideo = true;
            item.info.width = 1920;
            item.info.height = 1080;
            item.info.frameRate = {25, 1};
            item.info.durationUs = 5'000'000;
            state.items.append(item);
        }
        f.project.mediaPool()->setState(state);
        MediaPoolPanel panel(f.ctx);
        int requests = 0;
        panel.setThumbnailProvider([&](const QString& path, int height) {
            ++requests;
            return fakePoster(path, height);
        });
        panel.resize(800, 600);
        panel.show();
        QElapsedTimer timer;
        timer.start();
        for (int frame = 0; frame < 10; ++frame)
            panel.grab();
        const qint64 tenPaints = timer.elapsed();
        qInfo("2000 items: 10 full repaints in %lld ms, %d posters requested", tenPaints, requests);
        QVERIFY(requests < 200); // only visible tiles are requested
        QVERIFY2(tenPaints < 2000, "repainting the media pool grid is too slow");

        timer.restart();
        panel.setSearchText(QStringLiteral("clip_19"));
        const qint64 filterMs = timer.elapsed();
        QCOMPARE(files(f.project, panel.visibleMediaIds()).size(), 111); // 19, 190-199, 1900-1999
        qInfo("search filter over 2000 items: %lld ms", filterMs);
        QVERIFY(filterMs < 500);
    }
};

QTEST_MAIN(TestMediaPoolPanel)
#include "test_mediapoolpanel.moc"
