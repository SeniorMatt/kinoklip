// SPDX-License-Identifier: GPL-3.0-or-later
// Media pool organisation in the UI: bin list sections, bin moves + undo, per-bin view state,
// live smart bins, default smart bins, power bins across projects, clip colours and flags (pool
// views and timeline), list columns, search / colour filter, metadata editing (single and
// batch), usage commands. Writes screenshots when KK_SCREENSHOT_DIR is set.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/ClipColors.h"
#include "core/MediaPool.h"
#include "core/PowerBins.h"
#include "core/Preferences.h"
#include "core/SmartBins.h"
#include "ui/common/DragMime.h"
#include "ui/common/Resources.h"
#include "ui/mediapool/BinList.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/mediapool/MetadataPanel.h"
#include "ui/timeline/ClipColorMenu.h"
#include "ui/timeline/TimelinePainter.h"

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;

namespace {

QString addItem(Project& project, const QString& name, const QString& binId = {},
    Rational fps = {25, 1}, bool still = false)
{
    MediaItem item;
    item.path = QCoreApplication::applicationFilePath(); // an existing file
    item.name = name;
    item.binId = binId;
    item.info.valid = true;
    item.info.hasVideo = true;
    item.info.isStillImage = still;
    item.info.hasAudio = !still;
    item.info.width = 1920;
    item.info.height = 1080;
    item.info.frameRate = still ? Rational {0, 1} : fps;
    item.info.durationUs = still ? 0 : 10'000'000;
    item.info.videoCodec = QStringLiteral("h264");
    project.beginEdit(QStringLiteral("add"));
    const QString id = project.mediaPool()->addItem(item);
    project.endEdit();
    return id;
}

/// @p ids without timeline items.
QStringList files(const Project& project, const QStringList& ids)
{
    QStringList out;
    for (const QString& id : ids) {
        const MediaItem* m = project.mediaPool()->item(id);
        if (!m || !m->isTimeline())
            out.append(id);
    }
    return out;
}

QStringList topLevel(const BinList* list)
{
    QStringList out;
    for (int i = 0; i < list->topLevelItemCount(); ++i)
        out.append(list->topLevelItem(i)->text(0));
    return out;
}

QStringList childNames(const BinList* list, const QString& key)
{
    QStringList out;
    const QTreeWidgetItem* item = list->itemForKey(key);
    for (int i = 0; item && i < item->childCount(); ++i)
        out.append(item->child(i)->text(0));
    return out;
}

QImage fakePoster(const QString& path, int height)
{
    QImage image(height * 16 / 9, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor::fromHsv(int(qHash(path) % 360), 110, 160));
    return image;
}

void addClipOf(Project& project, const QString& mediaId, TrackRef track, int64_t start)
{
    ClipData c;
    c.mediaId = mediaId;
    c.name = project.mediaPool()->item(mediaId)->name;
    c.start = start;
    c.duration = 50;
    c.sourceDurationUs = 10'000'000;
    project.beginEdit(QStringLiteral("clip"));
    editops::overwriteClip(project.timeline()->mutableData(), track, c);
    project.endEdit();
}

} // namespace

class TestMediaOrganize : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_mediaorganize"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    void binListSections()
    {
        QTemporaryDir dir;
        PowerBins power(dir.filePath(QStringLiteral("p.json")));
        PanelFixture f;
        f.ctx.powerBins = &power;
        MediaPoolPanel panel(f.ctx);
        QCOMPARE(topLevel(panel.binList()),
            (QStringList {QStringLiteral("Master"), QStringLiteral("Smart Bins"),
                QStringLiteral("Power Bins")}));
        PanelFixture g; // no power bins: no section
        MediaPoolPanel plain(g.ctx);
        QCOMPARE(topLevel(plain.binList()).size(), 2);

        panel.setBinsVisible(false);
        QVERIFY(!panel.binsVisible());
        QVERIFY(!f.actions.action(QStringLiteral("view.show_bin_list"))->isChecked());
        f.actions.action(QStringLiteral("view.show_bin_list"))->trigger();
        QVERIFY(panel.binsVisible());
    }

    void binTreeDragAndUndo()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString a = panel.createBin(QStringLiteral("A"), {});
        const QString b = panel.createBin(QStringLiteral("B"), {});
        const QString c = panel.createBin(QStringLiteral("C"), {});
        const QString item = addItem(f.project, QStringLiteral("Clip"));
        const int steps = f.project.undoStack()->count();
        using P = BinList::DropPlace;
        QVERIFY(panel.moveBin(c, a, int(P::Before))); // C before A
        QCOMPARE(childNames(panel.binList(), QString()),
            (QStringList {QStringLiteral("C"), QStringLiteral("A"), QStringLiteral("B")}));
        QVERIFY(panel.moveBin(b, a, int(P::Into))); // B into A
        QCOMPARE(childNames(panel.binList(), a), QStringList {QStringLiteral("B")});
        QVERIFY(!panel.moveBin(a, b, int(P::Into))); // not into its own sub-bin
        QVERIFY(panel.moveBin(c, b, int(P::After))); // C after B, inside A
        QCOMPARE(childNames(panel.binList(), a),
            (QStringList {QStringLiteral("B"), QStringLiteral("C")}));
        QCOMPARE(panel.moveItemsToBin({item}, c), 1);
        QCOMPARE(f.project.undoStack()->count(), steps + 4);
        // The drop callbacks of the bin list do the same as the panel calls.
        panel.binList()->onBinDropped(c, QString(), P::Into);
        QCOMPARE(f.project.mediaPool()->bin(c)->parentId, QString());
        f.project.undo();
        QCOMPARE(f.project.mediaPool()->bin(c)->parentId, a);
        f.project.undo();
        QCOMPARE(f.project.mediaPool()->item(item)->binId, QString());
        f.project.undo();
        f.project.undo();
        f.project.undo();
        QCOMPARE(childNames(panel.binList(), QString()),
            (QStringList {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}));
    }

    void perBinViewState()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString a = panel.createBin(QStringLiteral("A"), {});
        const QString b = panel.createBin(QStringLiteral("B"), {});
        panel.setCurrentBin(a);
        panel.setViewMode(MediaPoolPanel::ViewMode::List);
        panel.setSort(MediaPoolModel::NameColumn, Qt::DescendingOrder);
        panel.setCurrentBin(b);
        panel.setViewMode(MediaPoolPanel::ViewMode::Grid);
        panel.setCurrentBin(a);
        QCOMPARE(panel.viewMode(), MediaPoolPanel::ViewMode::List);
        QCOMPARE(f.project.binViewState(a).sortKey, QStringLiteral("name"));
        QVERIFY(f.project.binViewState(a).descending);
        panel.setCurrentBin(b);
        QCOMPARE(panel.viewMode(), MediaPoolPanel::ViewMode::Grid);
        QVERIFY(f.project.binViewState(b).sortKey.isEmpty());
    }

    void smartBinIsLive()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString bin = panel.createBin(QStringLiteral("Deep"), {});
        const QString x = addItem(f.project, QStringLiteral("X"), bin);
        const QString y = addItem(f.project, QStringLiteral("Y"));
        const QString key = panel.createSmartBin(SmartBin {QString(), QStringLiteral("Beach"), true,
            {SmartRule {SmartField::Keywords, SmartOp::Has, QStringLiteral("beach")}}});
        QVERIFY(panel.binList()->itemForKey(key));
        panel.setCurrentBinKey(key);
        QVERIFY(panel.visibleMediaIds().isEmpty());
        MediaMetadata md;
        md.keywords = {QStringLiteral("Beach")};
        f.project.beginEdit(QStringLiteral("kw"));
        f.project.mediaPool()->setMetadata(x, md);
        f.project.endEdit();
        QCOMPARE(panel.visibleMediaIds(), QStringList {x}); // live, from a sub-bin too
        f.project.undo();
        QVERIFY(panel.visibleMediaIds().isEmpty());

        // Usage rule follows the timelines.
        SmartBin unused = *f.project.mediaPool()->smartBin(MediaPoolFilterModel::idOfKey(key));
        unused.rules = {SmartRule {SmartField::Usage, SmartOp::Is, QStringLiteral("unused")},
            SmartRule {SmartField::Type, SmartOp::Is, QStringLiteral("video")}};
        QVERIFY(panel.updateSmartBin(unused));
        QCOMPARE(panel.visibleMediaIds(), (QStringList {x, y}));
        addClipOf(f.project, y, TrackRef {TrackType::Video, 0}, 0);
        panel.model()->refreshUsage();
        QCOMPARE(panel.visibleMediaIds(), QStringList {x});
        QVERIFY(panel.deleteSmartBin(key));
        QCOMPARE(panel.currentBinKey(), QString());
    }

    void defaultSmartBinsFollowPreference()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString video = addItem(f.project, QStringLiteral("V"));
        addItem(f.project, QStringLiteral("S"), {}, {0, 1}, true);
        const QString key = QStringLiteral("smart:default.video");
        QVERIFY(!panel.binList()->itemForKey(key));
        prefs::setShowDefaultSmartBins(true);
        panel.applyPreferences();
        QVERIFY(panel.binList()->itemForKey(key));
        QVERIFY(panel.binList()->itemForKey(QStringLiteral("smart:default.stills")));
        panel.setCurrentBinKey(key);
        QCOMPARE(panel.visibleMediaIds(), QStringList {video});
        prefs::setShowDefaultSmartBins(false);
        panel.applyPreferences();
        QVERIFY(!panel.binList()->itemForKey(key));
        QCOMPARE(panel.currentBinKey(), QString());
    }

    void powerBinsAcrossProjects()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("power-bins.json"));
        QString logo;
        QString logos;
        {
            PowerBins power(file);
            PanelFixture f;
            f.ctx.powerBins = &power;
            MediaPoolPanel panel(f.ctx);
            logos = panel.createBin(QStringLiteral("Logos"), QStringLiteral("power:"));
            QVERIFY(logos.startsWith(QLatin1String("power:")));
            // A project item dragged into a power bin is copied there.
            const QString local = addItem(f.project, QStringLiteral("Logo"));
            QCOMPARE(panel.moveItemsToBin({local}, logos), 1);
            QVERIFY(f.project.mediaPool()->item(local)); // the project keeps its own
            QCOMPARE(power.pool()->items().size(), 1);
            logo = power.pool()->items().front().id;
        }
        // A different project sees the same power bins.
        PowerBins power(file);
        PanelFixture g;
        g.ctx.powerBins = &power;
        MediaPoolPanel panel(g.ctx);
        panel.setCurrentBinKey(logos);
        QCOMPARE(panel.currentBinKey(), logos);
        QCOMPARE(panel.visibleMediaIds(), QStringList {logo});
        const auto fileItems = [&g] {
            const auto& items = g.project.mediaPool()->items();
            return std::count_if(items.cbegin(), items.cend(),
                [](const MediaItem& m) { return m.kind == MediaItemKind::File; });
        };
        QCOMPARE(fileItems(), 0); // the project itself is untouched

        // Drag: the power id is offered; reading the media ids copies it into the project.
        std::unique_ptr<QMimeData> drag(
            panel.model()->mimeData({panel.model()->index(panel.model()->rowOf(logo), 0)}));
        QVERIFY(drag->hasFormat(QLatin1String(kPowerItemsMime)));
        QVERIFY(drag->hasFormat(QLatin1String(mime::MediaItems)));
        const QStringList ids = QString::fromUtf8(drag->data(QLatin1String(mime::MediaItems)))
                                    .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(ids.size(), 1);
        const MediaItem* copy = g.project.mediaPool()->item(ids.front());
        QVERIFY(copy);
        QCOMPARE(copy->path, power.pool()->item(logo)->path);
        // Asking again re-uses the copy.
        QCOMPARE(panel.ensureInProject({logo}), ids);
        // Colours on power items are saved at once (not undoable).
        const int steps = g.project.undoStack()->count();
        panel.setClipColor({logo}, QStringLiteral("navy"));
        QCOMPARE(g.project.undoStack()->count(), steps);
        QCOMPARE(PowerBins(file).pool()->item(logo)->clipColor, QStringLiteral("navy"));
    }

    void colorsAndFlagsInViewsAndTimeline()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString a = addItem(f.project, QStringLiteral("A"));
        const QString b = addItem(f.project, QStringLiteral("B"));
        const int steps = f.project.undoStack()->count();
        panel.setClipColor({a, b}, QStringLiteral("teal"));
        panel.setFlag({a}, flagIndex(QStringLiteral("red")), true);
        panel.setFlag({a}, flagIndex(QStringLiteral("blue")), true);
        QCOMPARE(f.project.undoStack()->count(), steps + 3);
        const QModelIndex ia = panel.model()->index(panel.model()->rowOf(a), 0);
        QCOMPARE(ia.data(MediaPoolModel::ClipColorRole).value<QColor>(),
            clipColorValue(QStringLiteral("teal")));
        QCOMPARE(flagIds(ia.data(MediaPoolModel::FlagsRole).toUInt()),
            (QStringList {QStringLiteral("blue"), QStringLiteral("red")}));
        QCOMPARE(ia.siblingAtColumn(MediaPoolModel::ClipColorColumn).data().toString(),
            clipColorName(QStringLiteral("teal")));
        QCOMPARE(ia.siblingAtColumn(MediaPoolModel::FlagsColumn).data().toString(),
            flagName(0) + QStringLiteral(", ") + flagName(4));
        panel.setFlag({a}, flagIndex(QStringLiteral("red")), false);
        QCOMPARE(f.project.mediaPool()->item(a)->flags, flagsFromIds({QStringLiteral("blue")}));
        panel.clearFlags({a});
        QCOMPARE(f.project.mediaPool()->item(a)->flags, quint32(0));

        // Timeline: the item colour shows on clips without their own; the clip menu sets one.
        addClipOf(f.project, a, TrackRef {TrackType::Video, 0}, 0);
        const ClipData clip = f.project.timeline()->data().videoTracks[0].clips[0];
        const MediaItem* item = f.project.mediaPool()->item(a);
        QCOMPARE(TimelinePainter::clipColor(clip, TrackType::Video, item),
            clipColorValue(QStringLiteral("teal")));
        QMenu menu;
        addClipColorMenus(menu, f.ctx, {clip.id}, &menu);
        auto* colors = menu.findChild<QMenu*>(QStringLiteral("TimelineClipColorMenu"));
        QVERIFY(colors);
        colors->actions().at(0)->trigger(); // first palette colour
        const ClipData coloured = f.project.timeline()->data().videoTracks[0].clips[0];
        QCOMPARE(clipColorIdOf(coloured.label), QLatin1String(clipColorPalette()[0].id));
        QCOMPARE(TimelinePainter::clipColor(coloured, TrackType::Video, item),
            QColor(clipColorPalette()[0].rgb));
        auto* flags = menu.findChild<QMenu*>(QStringLiteral("TimelineFlagMenu"));
        QVERIFY(flags);
        flags->actions().at(2)->trigger(); // green on the clip's source
        QCOMPARE(
            flagIds(f.project.mediaPool()->item(a)->flags), QStringList {QStringLiteral("green")});
        f.project.undo();
        QCOMPARE(f.project.mediaPool()->item(a)->flags, quint32(0));
        f.project.undo();
        QVERIFY(!f.project.timeline()->data().videoTracks[0].clips[0].label.isValid());
    }

    void columnsSortAndChooser()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString fast = addItem(f.project, QStringLiteral("Fast"), {}, {100, 1});
        const QString slow = addItem(f.project, QStringLiteral("Slow"), {}, {25, 1});
        QVERIFY(panel.isColumnVisible(MediaPoolModel::ClipColorColumn));
        QVERIFY(panel.isColumnVisible(MediaPoolModel::UsageColumn));
        QVERIFY(!panel.isColumnVisible(MediaPoolModel::KeywordsColumn));
        panel.setColumnVisible(MediaPoolModel::KeywordsColumn, true);
        QVERIFY(panel.isColumnVisible(MediaPoolModel::KeywordsColumn));
        panel.setColumnVisible(MediaPoolModel::NameColumn, false); // refused
        QVERIFY(panel.isColumnVisible(MediaPoolModel::NameColumn));
        {
            MediaPoolPanel again(f.ctx); // remembered
            QVERIFY(again.isColumnVisible(MediaPoolModel::KeywordsColumn));
        }
        // Numeric sort: 25 before 100 (text would sort "100" first).
        panel.setSort(MediaPoolModel::FpsColumn, Qt::AscendingOrder);
        QCOMPARE(files(f.project, panel.visibleMediaIds()), (QStringList {slow, fast}));
        panel.setSort(MediaPoolModel::FpsColumn, Qt::DescendingOrder);
        QCOMPARE(files(f.project, panel.visibleMediaIds()), (QStringList {fast, slow}));
        panel.setSort(-1, Qt::AscendingOrder);
        QCOMPARE(files(f.project, panel.visibleMediaIds()), (QStringList {fast, slow}));
        for (int c = 0; c < MediaPoolModel::ColumnCount; ++c) {
            QCOMPARE(MediaPoolModel::columnForKey(MediaPoolModel::columnKey(c)), c);
            QVERIFY(!panel.model()
                         ->headerData(c, Qt::Horizontal, Qt::DisplayRole)
                         .toString()
                         .isEmpty());
        }
    }

    void searchAndColorFilter()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString a = addItem(f.project, QStringLiteral("Alpha"));
        const QString b = addItem(f.project, QStringLiteral("Beta"));
        panel.setClipColor({a}, QStringLiteral("teal"));
        MediaMetadata md;
        md.keywords = {QStringLiteral("sunset")};
        md.comments = QStringLiteral("lovely light");
        md.scene = QStringLiteral("Scene 12");
        f.project.beginEdit(QStringLiteral("md"));
        f.project.mediaPool()->setMetadata(b, md);
        f.project.endEdit();
        panel.setFlag({b}, flagIndex(QStringLiteral("red")), true);
        const auto search = [&](const char* text) {
            panel.setSearchText(QLatin1String(text));
            return files(f.project, panel.visibleMediaIds());
        };
        QCOMPARE(search("teal"), QStringList {a});
        QCOMPARE(search("SUNSET"), QStringList {b});
        QCOMPARE(search("lovely"), QStringList {b});
        QCOMPARE(search("scene 12"), QStringList {b});
        QCOMPARE(search("red"), QStringList {b});
        search("");
        panel.setColorFilter(QStringLiteral("teal"));
        QCOMPARE(files(f.project, panel.visibleMediaIds()), QStringList {a});
        panel.setColorFilter(QStringLiteral("none"));
        QCOMPARE(files(f.project, panel.visibleMediaIds()), QStringList {b});
        QVERIFY(panel.selectMedia(a)); // clears the filter
        QCOMPARE(files(f.project, panel.visibleMediaIds()), (QStringList {a, b}));
    }

    void metadataEditAndBatch()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        MetadataPanel meta(f.ctx);
        meta.attach(&panel);
        const QString a = addItem(f.project, QStringLiteral("A"));
        const QString b = addItem(f.project, QStringLiteral("B"));
        QVERIFY(panel.selectMedia(a));
        QCOMPARE(meta.shownItems(), QStringList {a});
        const int steps = f.project.undoStack()->count();
        meta.applyField(MetadataPanel::Field::Scene, QStringLiteral("12"));
        meta.applyField(MetadataPanel::Field::Rating, 4);
        meta.applyField(MetadataPanel::Field::GoodTake, true);
        QCOMPARE(f.project.undoStack()->count(), steps + 3);
        QCOMPARE(f.project.mediaPool()->item(a)->metadata.scene, QStringLiteral("12"));
        QCOMPARE(f.project.mediaPool()->item(a)->metadata.rating, 4);
        QCOMPARE(qobject_cast<QLineEdit*>(meta.editor(MetadataPanel::Field::Scene))->text(),
            QStringLiteral("12"));
        // Typing + editingFinished in an editor writes the field.
        auto* take = qobject_cast<QLineEdit*>(meta.editor(MetadataPanel::Field::Take));
        take->setFocus();
        QTest::keyClicks(take, QStringLiteral("3"));
        emit take->editingFinished();
        QCOMPARE(f.project.mediaPool()->item(a)->metadata.take, QStringLiteral("3"));

        // Batch: two items, shared values shown, one step writes both.
        panel.setSelectedMediaIds({a, b});
        QCOMPARE(meta.shownItems(), (QStringList {a, b}));
        auto* scene = qobject_cast<QLineEdit*>(meta.editor(MetadataPanel::Field::Scene));
        QVERIFY(scene->text().isEmpty()); // differ
        QVERIFY(!scene->placeholderText().isEmpty());
        QCOMPARE(
            qobject_cast<QCheckBox*>(meta.editor(MetadataPanel::Field::GoodTake))->checkState(),
            Qt::PartiallyChecked);
        const int before = f.project.undoStack()->count();
        meta.applyField(MetadataPanel::Field::Keywords, QStringLiteral("beach, day 1"));
        QCOMPARE(f.project.undoStack()->count(), before + 1);
        for (const QString& id : {a, b})
            QCOMPARE(f.project.mediaPool()->item(id)->metadata.keywords,
                (QStringList {QStringLiteral("beach"), QStringLiteral("day 1")}));
        QCOMPARE(f.project.mediaPool()->item(a)->metadata.scene, QStringLiteral("12")); // kept
        QCOMPARE(qobject_cast<QLineEdit*>(meta.editor(MetadataPanel::Field::Keywords))->text(),
            QStringLiteral("beach, day 1"));
        f.project.undo();
        QVERIFY(f.project.mediaPool()->item(b)->metadata.keywords.isEmpty());
    }

    void usageCommands()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        const QString used = addItem(f.project, QStringLiteral("Used"));
        const QString unused = addItem(f.project, QStringLiteral("Unused"));
        addClipOf(f.project, used, TrackRef {TrackType::Video, 0}, 0);
        addClipOf(f.project, used, TrackRef {TrackType::Video, 0}, 100);
        panel.model()->refreshUsage();
        const QModelIndex iu = panel.model()->index(panel.model()->rowOf(used), 0);
        QCOMPARE(iu.data(MediaPoolModel::UsageRole).toInt(), 2);
        QCOMPARE(
            iu.siblingAtColumn(MediaPoolModel::UsageColumn).data().toString(), QStringLiteral("2"));
        QCOMPARE(panel.selectClipsInTimeline({used}), 2);
        QCOMPARE(f.selection.clips().size(), 2);
        QCOMPARE(panel.unusedClips(), QStringList {unused});
        const int steps = f.project.undoStack()->count();
        QCOMPARE(panel.removeUnusedClips(false), 1);
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        QVERIFY(!f.project.mediaPool()->item(unused));
        QVERIFY(f.project.mediaPool()->item(used));
        QCOMPARE(panel.removeUnusedClips(false), 0);
        f.project.undo();
        QVERIFY(f.project.mediaPool()->item(unused));
    }

    void screenshots()
    {
        // Documentation screenshots (KK_SCREENSHOT_DIR): nested bins, a smart bin, power bins,
        // coloured / flagged items in the list view, the metadata panel.
        if (qEnvironmentVariable("KK_SCREENSHOT_DIR").isEmpty())
            QSKIP("set KK_SCREENSHOT_DIR to write the screenshots");
        QTemporaryDir dir;
        PowerBins power(dir.filePath(QStringLiteral("p.json")));
        PanelFixture f;
        f.ctx.powerBins = &power;
        prefs::setShowDefaultSmartBins(true);
        auto* host = new QWidget;
        host->resize(1180, 520);
        auto* h = new QHBoxLayout(host);
        h->setContentsMargins(0, 0, 0, 0);
        auto* panel = new MediaPoolPanel(f.ctx);
        panel->setThumbnailProvider(fakePoster);
        auto* meta = new MetadataPanel(f.ctx);
        meta->attach(panel);
        h->addWidget(panel, 3);
        h->addWidget(meta, 1);
        const QString footage = panel->createBin(QStringLiteral("Footage"), {});
        const QString day1 = panel->createBin(QStringLiteral("Day 1"), footage);
        panel->createBin(QStringLiteral("Day 2"), footage);
        panel->createBin(QStringLiteral("Interviews"), day1);
        panel->createBin(QStringLiteral("Music"), {});
        panel->createBin(QStringLiteral("Graphics"), {});
        const char* names[] = {"A001_C003 Harbour", "A001_C004 Harbour wide", "A002_C001 Market",
            "A002_C007 Market crowd", "B001_C002 Interview Anna", "B001_C003 Interview Ben"};
        const char* colors[] = {"teal", "teal", "orange", "", "violet", "violet"};
        QStringList ids;
        for (int i = 0; i < 6; ++i) {
            ids.append(addItem(f.project, QLatin1String(names[i]), day1));
            if (*colors[i])
                panel->setClipColor({ids.back()}, QLatin1String(colors[i]));
        }
        panel->setFlag({ids[0], ids[4]}, flagIndex(QStringLiteral("green")), true);
        panel->setFlag({ids[4]}, flagIndex(QStringLiteral("red")), true);
        panel->setFlag({ids[2]}, flagIndex(QStringLiteral("yellow")), true);
        MediaMetadata md;
        md.scene = QStringLiteral("12");
        md.shot = QStringLiteral("A");
        md.take = QStringLiteral("3");
        md.camera = QStringLiteral("A cam");
        md.description = QStringLiteral("Anna at the window, morning light");
        md.comments = QStringLiteral("Best answer at 01:20");
        md.keywords = {QStringLiteral("interview"), QStringLiteral("anna")};
        md.goodTake = true;
        md.rating = 4;
        f.project.beginEdit(QStringLiteral("md"));
        f.project.mediaPool()->setMetadata(ids[4], md);
        f.project.endEdit();
        addClipOf(f.project, ids[0], TrackRef {TrackType::Video, 0}, 0);
        addClipOf(f.project, ids[4], TrackRef {TrackType::Video, 0}, 60);
        panel->model()->refreshUsage();
        panel->createSmartBin(SmartBin {QString(), QStringLiteral("Good takes"), true,
            {SmartRule {SmartField::Flags, SmartOp::Has, QStringLiteral("green")}}});
        panel->createSmartBin(SmartBin {QString(), QStringLiteral("Unused clips"), true,
            {SmartRule {SmartField::Usage, SmartOp::Is, QStringLiteral("unused")}}});
        const QString logos = panel->createBin(QStringLiteral("Logos"), QStringLiteral("power:"));
        panel->createBin(QStringLiteral("Music Beds"), QStringLiteral("power:"));
        MediaItem logo = *f.project.mediaPool()->item(ids[1]);
        logo.id.clear();
        logo.name = QStringLiteral("Studio logo sting");
        logo.binId = MediaPoolFilterModel::idOfKey(logos);
        logo.clipColor = QStringLiteral("navy");
        power.pool()->addItem(logo);
        panel->setBinsVisible(true);
        panel->binList()->expandAll();

        panel->setCurrentBin(day1);
        panel->setViewMode(MediaPoolPanel::ViewMode::List);
        panel->setColumnVisible(MediaPoolModel::KeywordsColumn, true);
        panel->setColumnVisible(MediaPoolModel::RatingColumn, true);
        panel->setSelectedMediaIds({ids[4]});
        saveScreenshot(*host, QStringLiteral("mediapool-list-colours-flags-metadata"));
        panel->setViewMode(MediaPoolPanel::ViewMode::Grid);
        saveScreenshot(*host, QStringLiteral("mediapool-nested-bins-grid"));
        panel->setCurrentBinKey(
            panel->binList()->topLevelItem(1)->child(4)->data(0, BinList::kKeyRole).toString());
        saveScreenshot(*host, QStringLiteral("mediapool-smart-bin"));
        panel->setCurrentBinKey(logos);
        saveScreenshot(*host, QStringLiteral("mediapool-power-bins"));
        panel->setCurrentBin(day1);
        panel->setViewMode(MediaPoolPanel::ViewMode::List);
        panel->setSelectedMediaIds({ids[4], ids[5]});
        saveScreenshot(*host, QStringLiteral("metadata-batch"));
        delete host;
        prefs::setShowDefaultSmartBins(false);
    }
};

QTEST_MAIN(TestMediaOrganize)
#include "test_mediaorganize.moc"
