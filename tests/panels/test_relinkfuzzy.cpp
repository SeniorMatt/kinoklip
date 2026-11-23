// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzzy relink (Media Pool > Relink Selected Clips / Relink Offline Media): the planner
// (core/RelinkMatcher: exact name, other case, other extension, find / replace pattern,
// closest name, one file per item), the preview dialog (table, options, rows switched off or
// chosen by hand) and applying the plan as one undo step.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/MediaPool.h"
#include "core/RelinkMatcher.h"
#include "ui/common/Resources.h"
#include "ui/dialogs/RelinkDialog.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::test;
using kk::relink::MatchKind;

namespace {

relink::Source source(const QString& name, bool video = true)
{
    relink::Source s;
    s.itemId = name;
    s.oldPath = QStringLiteral("/old/place/") + name;
    s.hasVideo = video;
    return s;
}

QStringList files(std::initializer_list<const char*> names)
{
    QStringList out;
    for (const char* n : names)
        out.append(QStringLiteral("/new/") + QLatin1String(n));
    return out;
}

QString touch(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write("x");
    return path;
}

QString addOffline(Project& project, const QString& file)
{
    MediaItem item;
    item.path = QStringLiteral("/nonexistent/") + file;
    item.name = file;
    item.info.valid = true;
    item.info.hasVideo = true;
    item.info.width = 64;
    item.info.height = 36;
    item.info.frameRate = {25, 1};
    item.info.durationUs = 1'000'000;
    project.beginEdit(QStringLiteral("add"));
    const QString id = project.mediaPool()->addItem(item);
    project.endEdit();
    return id;
}

} // namespace

class TestRelinkFuzzy : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_relinkfuzzy"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // ---- The planner ----

    void exactBeatsOtherCase()
    {
        const auto rows = relink::plan({source(QStringLiteral("clip.mov"))},
            files({"CLIP.MOV", "clip.mov", "clip.mp4"}), {});
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].newPath, QStringLiteral("/new/clip.mov"));
        QCOMPARE(rows[0].kind, MatchKind::Exact);
        QCOMPARE(rows[0].quality, 1.0);
        QVERIFY(!rows[0].ambiguous);
    }

    void caseInsensitive()
    {
        const auto rows = relink::plan({source(QStringLiteral("clip.mov"))}, files({"CLIP.MOV"}), {});
        QCOMPARE(rows[0].kind, MatchKind::CaseInsensitive);
        QCOMPARE(rows[0].newPath, QStringLiteral("/new/CLIP.MOV"));
        relink::Options strict;
        strict.ignoreCase = false;
        strict.ignoreExtension = false;
        QCOMPARE(relink::plan({source(QStringLiteral("clip.mov"))}, files({"CLIP.MOV"}), strict)[0].kind,
            MatchKind::None);
    }

    void otherExtension()
    {
        auto rows = relink::plan({source(QStringLiteral("A001_C002.mov"))},
            files({"a001_c002.MXF", "other.mov"}), {});
        QCOMPARE(rows[0].kind, MatchKind::OtherExtension);
        QCOMPARE(rows[0].newPath, QStringLiteral("/new/a001_c002.MXF"));
        QVERIFY(rows[0].quality > 0.9 && rows[0].quality < 1.0);
        relink::Options noExt;
        noExt.ignoreExtension = false;
        QCOMPARE(relink::plan({source(QStringLiteral("A001_C002.mov"))}, files({"A001_C002.mxf"}), noExt)[0].kind,
            MatchKind::None);
        // A name with dots: only the last extension is dropped.
        rows = relink::plan({source(QStringLiteral("take.2.mov"))}, files({"take.2.mp4", "take.mp4"}), {});
        QCOMPARE(rows[0].newPath, QStringLiteral("/new/take.2.mp4"));
    }

    void audioFilesDoNotStandInForVideo()
    {
        QCOMPARE(relink::plan({source(QStringLiteral("clip.mov"), true)}, files({"clip.wav"}), {})[0].kind,
            MatchKind::None);
        QCOMPARE(relink::plan({source(QStringLiteral("clip.mov"), false)}, files({"clip.wav"}), {})[0].kind,
            MatchKind::OtherExtension);
    }

    void patternReplacesPartOfTheName()
    {
        relink::Options o;
        o.find = QStringLiteral("_v1$");
        o.replace = QStringLiteral("_v2");
        auto rows = relink::plan({source(QStringLiteral("shot_v1.mov"))}, files({"shot_v2.mov", "shot_v1b.mov"}), o);
        QCOMPARE(rows[0].kind, MatchKind::Pattern);
        QCOMPARE(rows[0].newPath, QStringLiteral("/new/shot_v2.mov"));
        QCOMPARE(rows[0].quality, 0.9);
        // Groups, both \1 and $1 spellings.
        o.find = QStringLiteral("^(\\w+)_v\\d+$");
        for (const QString& r : {QStringLiteral("\\1_final"), QStringLiteral("$1_final")}) {
            o.replace = r;
            rows = relink::plan({source(QStringLiteral("shot_v1.mov"))}, files({"shot_final.mp4"}), o);
            QCOMPARE(rows[0].newPath, QStringLiteral("/new/shot_final.mp4"));
            QCOMPARE(rows[0].kind, MatchKind::Pattern);
        }
        // A pattern that finds nothing in the name leaves the item to the other rules.
        o.find = QStringLiteral("zzz");
        o.replace = QStringLiteral("y");
        QCOMPARE(relink::plan({source(QStringLiteral("shot_v1.mov"))}, files({"shot_v2.mov"}), o)[0].kind,
            MatchKind::None);
        // An invalid expression is reported and ignored.
        QVERIFY(!relink::patternError(QStringLiteral("(unclosed")).isEmpty());
        QVERIFY(relink::patternError(QStringLiteral("_v\\d+$")).isEmpty());
        o.find = QStringLiteral("(unclosed");
        QCOMPARE(relink::plan({source(QStringLiteral("shot_v1.mov"))}, files({"shot_v1.mov"}), o)[0].kind,
            MatchKind::Exact);
    }

    void closestName()
    {
        relink::Options o;
        const QVector<relink::Source> src {source(QStringLiteral("interview_0042.mov"))};
        QCOMPARE(relink::plan(src, files({"interview_0043.mov"}), o)[0].kind, MatchKind::None); // off
        o.closestName = true;
        auto rows = relink::plan(src, files({"interview_0043.mov", "broll_9.mov"}), o);
        QCOMPARE(rows[0].kind, MatchKind::Closest);
        QCOMPARE(rows[0].newPath, QStringLiteral("/new/interview_0043.mov"));
        QVERIFY(rows[0].quality > 0.8 && rows[0].quality < 1.0);
        // The most similar of several.
        rows = relink::plan(src, files({"interview_0143.mov", "interview_0043.mov"}), o);
        QCOMPARE(rows[0].newPath, QStringLiteral("/new/interview_0043.mov"));
        // A suffix added to the name is close as well.
        rows = relink::plan({source(QStringLiteral("beach.mov"))}, files({"beach_graded.mov"}), o);
        QCOMPARE(rows[0].kind, MatchKind::Closest);
        // Too different: no match; the threshold decides.
        rows = relink::plan({source(QStringLiteral("abc.mov"))}, files({"xyz.mov"}), o);
        QCOMPARE(rows[0].kind, MatchKind::None);
        o.minimumSimilarity = 0.99;
        QCOMPARE(relink::plan(src, files({"interview_0043.mov"}), o)[0].kind, MatchKind::None);
        // Similarity itself.
        QCOMPARE(relink::nameSimilarity(QStringLiteral("Clip"), QStringLiteral("clip")), 1.0);
        QVERIFY(relink::nameSimilarity(QStringLiteral("abcd"), QStringLiteral("abce")) > 0.7);
        QVERIFY(relink::nameSimilarity(QStringLiteral("abc"), QStringLiteral("xyz")) < 0.2);
    }

    void oneFilePerItemAndTheBestMatchWins()
    {
        // Two items want the same file: the exact name takes it, the other finds nothing else.
        auto rows = relink::plan({source(QStringLiteral("a.mov")), source(QStringLiteral("A.mov"))},
            files({"a.mov"}), {});
        QCOMPARE(rows[0].kind, MatchKind::Exact);
        QCOMPARE(rows[1].kind, MatchKind::None);
        // Order of the items does not matter for who wins.
        rows = relink::plan({source(QStringLiteral("A.mov")), source(QStringLiteral("a.mov"))},
            files({"a.mov"}), {});
        QCOMPARE(rows[0].kind, MatchKind::None);
        QCOMPARE(rows[1].kind, MatchKind::Exact);
    }

    void severalFilesOfTheSameNameAreAmbiguousAndTheFirstIsTaken()
    {
        const QStringList list {QStringLiteral("/new/shallow/clip.mov"), QStringLiteral("/new/deep/er/clip.mov")};
        const auto rows = relink::plan({source(QStringLiteral("clip.mov"))}, list, {});
        QCOMPARE(rows[0].newPath, list.first());
        QVERIFY(rows[0].ambiguous);
    }

    void scanFolderListsMediaShallowFirst()
    {
        QTemporaryDir dir;
        touch(dir.filePath(QStringLiteral("b.mp4")));
        touch(dir.filePath(QStringLiteral("notes.txt")));
        touch(dir.filePath(QStringLiteral("sub/a.mov")));
        touch(dir.filePath(QStringLiteral("sub/deeper/c.wav")));
        const QStringList all = relink::scanFolder(dir.path(), true);
        QCOMPARE(all.size(), 3);
        QVERIFY(all[0].endsWith(QStringLiteral("/b.mp4")));
        QVERIFY(all[1].endsWith(QStringLiteral("/sub/a.mov")));
        QCOMPARE(relink::scanFolder(dir.path(), false).size(), 1);
        QCOMPARE(relink::scanFolder(dir.path(), true, 1).size(), 1); // the scan limit
        QCOMPARE(relink::baseName(QStringLiteral("a.b.mov")), QStringLiteral("a.b"));
        QCOMPARE(relink::baseName(QStringLiteral(".hidden")), QStringLiteral(".hidden"));
        QCOMPARE(relink::baseName(QStringLiteral("noext")), QStringLiteral("noext"));
    }

    // ---- The dialog ----

    void dialogShowsThePreviewTable()
    {
        PanelFixture f;
        QTemporaryDir dir;
        touch(dir.filePath(QStringLiteral("shot_a.mov")));
        touch(dir.filePath(QStringLiteral("SHOT_B.MOV")));
        touch(dir.filePath(QStringLiteral("shot_c.mxf")));
        touch(dir.filePath(QStringLiteral("deep/shot_d_v2.mov")));
        touch(dir.filePath(QStringLiteral("shot_f_graded.mov")));
        const QString a = addOffline(f.project, QStringLiteral("shot_a.mov"));
        const QString b = addOffline(f.project, QStringLiteral("shot_b.mov"));
        const QString c = addOffline(f.project, QStringLiteral("shot_c.mov"));
        const QString d = addOffline(f.project, QStringLiteral("shot_d_v1.mov"));
        const QString e = addOffline(f.project, QStringLiteral("shot_e.mov"));
        const QString g = addOffline(f.project, QStringLiteral("shot_f.mov"));
        RelinkDialog dialog(*f.project.mediaPool(), {}, dir.path());
        QTableWidget* table = dialog.table();
        QCOMPARE(table->rowCount(), 6);
        auto row = [&](const QString& id) {
            for (int r = 0; r < dialog.rows().size(); ++r) {
                if (dialog.rows()[r].itemId == id)
                    return r;
            }
            return -1;
        };
        auto text = [&](const QString& id, int column) {
            return table->item(row(id), column)->text();
        };
        // Old file, new file, kind and quality per row.
        QCOMPARE(text(a, RelinkDialog::OldColumn), QStringLiteral("shot_a.mov"));
        QCOMPARE(text(a, RelinkDialog::NewColumn), QStringLiteral("shot_a.mov"));
        QCOMPARE(text(a, RelinkDialog::MatchColumn), QStringLiteral("Exact name"));
        QCOMPARE(text(a, RelinkDialog::QualityColumn), QStringLiteral("100 %"));
        QCOMPARE(text(b, RelinkDialog::NewColumn), QStringLiteral("SHOT_B.MOV"));
        QCOMPARE(text(b, RelinkDialog::MatchColumn), QStringLiteral("Other case"));
        QCOMPARE(text(c, RelinkDialog::NewColumn), QStringLiteral("shot_c.mxf"));
        QCOMPARE(text(c, RelinkDialog::MatchColumn), QStringLiteral("Other extension"));
        // Unmatched rows are listed and not ticked.
        QCOMPARE(text(d, RelinkDialog::MatchColumn), QStringLiteral("No match"));
        QCOMPARE(table->item(row(d), RelinkDialog::ApplyColumn)->checkState(), Qt::Unchecked);
        QCOMPARE(text(e, RelinkDialog::MatchColumn), QStringLiteral("No match"));
        QCOMPARE(dialog.result().size(), 3);
        QVERIFY(dialog.summaryLabel()->text().contains(QStringLiteral("3 items will be relinked")));
        QVERIFY(dialog.summaryLabel()->text().contains(QStringLiteral("3 items have no match")));
        // Ignore extension off: the .mxf is no match any more.
        dialog.ignoreExtensionBox()->setChecked(false);
        QCOMPARE(text(c, RelinkDialog::MatchColumn), QStringLiteral("No match"));
        QCOMPARE(dialog.result().size(), 2);
        dialog.ignoreExtensionBox()->setChecked(true);
        // The pattern _v1 -> _v2 finds the file in the subfolder.
        dialog.findEdit()->setText(QStringLiteral("_v1$"));
        dialog.replaceEdit()->setText(QStringLiteral("_v2"));
        QCOMPARE(text(d, RelinkDialog::NewColumn), QStringLiteral("shot_d_v2.mov"));
        QCOMPARE(text(d, RelinkDialog::MatchColumn), QStringLiteral("Pattern"));
        QCOMPARE(dialog.result().size(), 4);
        // Subfolders off: that file is out of reach.
        dialog.subfoldersBox()->setChecked(false);
        QCOMPARE(text(d, RelinkDialog::MatchColumn), QStringLiteral("No match"));
        dialog.subfoldersBox()->setChecked(true);
        // An invalid expression says so and does not empty the table.
        dialog.findEdit()->setText(QStringLiteral("(oops"));
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("RelinkPatternNote"))->isVisibleTo(&dialog));
        QCOMPARE(table->rowCount(), 6);
        dialog.findEdit()->clear();
        // Closest name finds the graded file.
        QCOMPARE(text(g, RelinkDialog::MatchColumn), QStringLiteral("No match"));
        dialog.closestBox()->setChecked(true);
        QVERIFY(dialog.similarityBox()->isEnabled());
        QCOMPARE(text(g, RelinkDialog::NewColumn), QStringLiteral("shot_f_graded.mov"));
        QVERIFY(text(g, RelinkDialog::MatchColumn).startsWith(QStringLiteral("Closest name")));
        QVERIFY(text(g, RelinkDialog::QualityColumn).endsWith(QStringLiteral("%")));
        // Un-ticking a row leaves it out; choosing a file by hand puts it in.
        dialog.closestBox()->setChecked(false);
        table->item(row(a), RelinkDialog::ApplyColumn)->setCheckState(Qt::Unchecked);
        QCOMPARE(dialog.result().size(), 2);
        const QString mine = touch(dir.filePath(QStringLiteral("whatever.mov")));
        dialog.chooseFile(row(e), mine);
        QCOMPARE(text(e, RelinkDialog::MatchColumn), QStringLiteral("Chosen by hand"));
        QCOMPARE(text(e, RelinkDialog::NewColumn), QStringLiteral("whatever.mov"));
        bool hasE = false;
        for (const auto& r : dialog.result())
            hasE = hasE || (r.itemId == e && r.newPath == mine);
        QVERIFY(hasE);
        // Only the asked-for items are in scope.
        RelinkDialog some(*f.project.mediaPool(), {a, c}, dir.path());
        QCOMPARE(some.table()->rowCount(), 2);
    }

    void onlineItemsOnlyOnRequest()
    {
        PanelFixture f;
        QTemporaryDir dir;
        const QString here = touch(dir.filePath(QStringLiteral("here.mov")));
        MediaItem item;
        item.path = here;
        item.name = QStringLiteral("here.mov");
        item.info.valid = true;
        item.info.hasVideo = true;
        f.project.beginEdit(QStringLiteral("add"));
        f.project.mediaPool()->addItem(item);
        f.project.endEdit();
        touch(dir.filePath(QStringLiteral("other/here.mxf")));
        addOffline(f.project, QStringLiteral("gone.mov"));
        RelinkDialog dialog(*f.project.mediaPool(), {}, dir.path());
        QCOMPARE(dialog.table()->rowCount(), 1); // the offline one only
        QVERIFY(dialog.includeOnlineBox()->isVisibleTo(&dialog) || !dialog.includeOnlineBox()->isHidden());
        dialog.includeOnlineBox()->setChecked(true);
        QCOMPARE(dialog.table()->rowCount(), 2);
    }

    // ---- Applying ----

    void panelAppliesThePlanAsOneUndoStep()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        QTemporaryDir dir;
        const QString fileA = touch(dir.filePath(QStringLiteral("clip_a.mov")));
        const QString fileB = touch(dir.filePath(QStringLiteral("CLIP_B.MP4")));
        const QString a = addOffline(f.project, QStringLiteral("clip_a.mov"));
        const QString b = addOffline(f.project, QStringLiteral("clip_b.mov"));
        const QString c = addOffline(f.project, QStringLiteral("clip_c.mov"));
        const int undoBefore = f.project.undoStack()->count();
        RelinkDialog dialog(*f.project.mediaPool(), {}, dir.path());
        const int relinked = panel.relinkRows(dialog.result());
        QCOMPARE(relinked, 2);
        QCOMPARE(f.project.mediaPool()->item(a)->path, fileA);
        QCOMPARE(f.project.mediaPool()->item(b)->path, fileB);
        QVERIFY(f.project.mediaPool()->item(c)->path.startsWith(QStringLiteral("/nonexistent")));
        QVERIFY(!f.project.mediaPool()->isOffline(a));
        QVERIFY(f.project.mediaPool()->isOffline(c));
        QCOMPARE(f.project.undoStack()->count(), undoBefore + 1); // one step
        f.project.undo();
        QVERIFY(f.project.mediaPool()->item(a)->path.startsWith(QStringLiteral("/nonexistent")));
        QVERIFY(f.project.mediaPool()->item(b)->path.startsWith(QStringLiteral("/nonexistent")));
        QVERIFY(f.project.mediaPool()->isOffline(a));
        f.project.redo();
        QCOMPARE(f.project.mediaPool()->item(b)->path, fileB);
        QCOMPARE(panel.relinkRows({}), 0);
    }
};

QTEST_MAIN(TestRelinkFuzzy)
#include "test_relinkfuzzy.moc"
