// SPDX-License-Identifier: GPL-3.0-or-later
// The Metadata panel's full view (options menu > Show All Metadata): everything libavformat finds
// in the file, including non-standard tags such as the QuickTime mdta key
// "com.dji.camera.ColorGammaSxS", stream properties and side data; searchable and copyable.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/MediaPool.h"
#include "media/MediaMetadataDump.h"
#include "ui/common/Resources.h"
#include "ui/mediapool/MetadataPanel.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>

using namespace kk;
using namespace kk::test;

namespace {

bool runFfmpeg(const QStringList& args)
{
    QProcess p;
    p.start(QStringLiteral("ffmpeg"),
        QStringList {QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error")} + args);
    return p.waitForFinished(60000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

const media::MetadataEntry* find(const QVector<media::MetadataEntry>& rows, const QString& key)
{
    for (const media::MetadataEntry& e : rows) {
        if (e.key == key)
            return &e;
    }
    return nullptr;
}

QString addItem(Project& project, const QString& name, const QString& path)
{
    MediaItem item;
    item.path = path;
    item.name = name;
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

class TestMetadataFull : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_file; ///< a .mov with DJI-style mdta tags, audio and a 90 degree display matrix

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_metadatafull"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
        QVERIFY(m_dir.isValid());
        const QString base = m_dir.filePath(QStringLiteral("base.mov"));
        m_file = m_dir.filePath(QStringLiteral("dji.mov"));
        QVERIFY(runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("testsrc=size=64x36:rate=25:duration=1"), QStringLiteral("-f"),
            QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=duration=1"),
            QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-c:a"),
            QStringLiteral("aac"), QStringLiteral("-movflags"), QStringLiteral("use_metadata_tags"),
            QStringLiteral("-metadata"), QStringLiteral("com.dji.camera.ColorGammaSxS=D-Log"),
            QStringLiteral("-metadata"), QStringLiteral("title=Rooftop"), base}));
        QVERIFY(runFfmpeg({QStringLiteral("-display_rotation"), QStringLiteral("90"),
            QStringLiteral("-i"), base, QStringLiteral("-c"), QStringLiteral("copy"),
            QStringLiteral("-movflags"), QStringLiteral("use_metadata_tags"),
            QStringLiteral("-map_metadata"), QStringLiteral("0"), m_file}));
    }

    void init() { QSettings().clear(); }

    void readsContainerStreamsTagsAndSideData()
    {
        QString error;
        const auto rows = media::readAllMetadata(m_file, &error);
        QVERIFY2(!rows.isEmpty(), qPrintable(error));
        // The non-standard QuickTime key is listed with its value.
        const auto* gamma = find(rows, QStringLiteral("com.dji.camera.ColorGammaSxS"));
        QVERIFY(gamma);
        QCOMPARE(gamma->value, QStringLiteral("D-Log"));
        QCOMPARE(gamma->group, QStringLiteral("Container tags"));
        QCOMPARE(find(rows, QStringLiteral("title"))->value, QStringLiteral("Rooftop"));
        // Container and stream properties.
        QVERIFY(find(rows, QStringLiteral("format_name"))->value.contains(QStringLiteral("mov")));
        const auto* codec = find(rows, QStringLiteral("codec_name"));
        QVERIFY(codec);
        QCOMPARE(codec->value, QStringLiteral("mpeg4"));
        QVERIFY(codec->group.startsWith(QStringLiteral("Video stream")));
        QCOMPARE(find(rows, QStringLiteral("width"))->value, QStringLiteral("64"));
        QCOMPARE(find(rows, QStringLiteral("sample_rate"))->value, QStringLiteral("44100"));
        bool audio = false;
        for (const auto& e : rows)
            audio = audio || e.group.startsWith(QStringLiteral("Audio stream"));
        QVERIFY(audio);
        // Stream side data: the display matrix of the 90 degree rotation.
        bool rotation = false;
        for (const auto& e : rows) {
            if (e.group.endsWith(QStringLiteral("side data")) && e.value.contains(QStringLiteral("90")))
                rotation = true;
        }
        QVERIFY(rotation);
    }

    void filterAndText()
    {
        const auto rows = media::readAllMetadata(m_file);
        QVERIFY(!rows.isEmpty());
        QVERIFY(media::filterMetadata(rows, QString()) == rows);
        const auto hit = media::filterMetadata(rows, QStringLiteral("DJI d-log"));
        QCOMPARE(hit.size(), 1); // every word must match, any case, in group, key or value
        QCOMPARE(hit.front().key, QStringLiteral("com.dji.camera.ColorGammaSxS"));
        QVERIFY(media::filterMetadata(rows, QStringLiteral("no-such-thing-xyz")).isEmpty());
        const QString text = media::metadataToText(hit);
        QCOMPARE(text,
            QStringLiteral("[Container tags]\ncom.dji.camera.ColorGammaSxS : D-Log\n"));
        QVERIFY(media::metadataToText(rows).contains(QStringLiteral("[Container]\n")));
    }

    void unreadableFileGivesNothing()
    {
        QString error;
        QVERIFY(media::readAllMetadata(m_dir.filePath(QStringLiteral("missing.mov")), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QFile bad(m_dir.filePath(QStringLiteral("bad.mov")));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not a movie at all, just some text that is long enough to be probed");
        bad.close();
        QString error2;
        QVERIFY(media::readAllMetadata(bad.fileName(), &error2).isEmpty());
        QVERIFY(!error2.isEmpty());
    }

    void panelOptionsMenuTogglesTheFullView()
    {
        PanelFixture f;
        MetadataPanel panel(f.ctx);
        panel.resize(420, 600);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        const QString id = addItem(f.project, QStringLiteral("Rooftop"), m_file);
        panel.showMedia(id);
        auto* options = panel.findChild<QToolButton*>(QStringLiteral("MetadataOptions"));
        QVERIFY(options && options->menu());
        QAction* showAll = options->menu()->findChild<QAction*>(QStringLiteral("MetadataShowAll"));
        QVERIFY(showAll && showAll->isCheckable());
        QVERIFY(!panel.fullView()); // the normal fields by default
        QVERIFY(!panel.fullTree()->isVisible());
        showAll->setChecked(true);
        QVERIFY(panel.fullView());
        QVERIFY(panel.fullTree()->isVisible());
        QTRY_VERIFY(!panel.fullRows().isEmpty());
        QTRY_VERIFY(panel.fullTree()->topLevelItemCount() > 3);
        // The remembered choice.
        QVERIFY(QSettings().value(QStringLiteral("metadata/fullView")).toBool());
        // Off again: the editable fields.
        showAll->setChecked(false);
        QVERIFY(!panel.fullTree()->isVisible());
    }

    void treeSearchAndCopy()
    {
        PanelFixture f;
        MetadataPanel panel(f.ctx);
        panel.resize(420, 600);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        const QString id = addItem(f.project, QStringLiteral("Rooftop"), m_file);
        panel.setFullView(true);
        panel.showMedia(id);
        QTRY_VERIFY(!panel.fullRows().isEmpty());
        QTreeWidget* tree = panel.fullTree();
        // Find the DJI row in the tree.
        auto findRow = [&](const QString& key) -> QTreeWidgetItem* {
            for (int g = 0; g < tree->topLevelItemCount(); ++g) {
                QTreeWidgetItem* group = tree->topLevelItem(g);
                for (int r = 0; r < group->childCount(); ++r) {
                    if (group->child(r)->text(0) == key)
                        return group->child(r);
                }
            }
            return nullptr;
        };
        QTreeWidgetItem* dji = findRow(QStringLiteral("com.dji.camera.ColorGammaSxS"));
        QVERIFY(dji);
        QCOMPARE(dji->text(1), QStringLiteral("D-Log"));
        const int all = tree->topLevelItemCount();
        // Search narrows the tree to the matching rows (and their groups).
        panel.fullFilter()->setText(QStringLiteral("gamma"));
        QCOMPARE(tree->topLevelItemCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
        QCOMPARE(panel.fullText(), QStringLiteral("[Container tags]\ncom.dji.camera.ColorGammaSxS : D-Log\n"));
        panel.copyAll();
        QCOMPARE(QApplication::clipboard()->text(), panel.fullText());
        // Ctrl+C copies the selected row as "name : value".
        dji = findRow(QStringLiteral("com.dji.camera.ColorGammaSxS"));
        tree->setCurrentItem(dji);
        dji->setSelected(true);
        tree->setFocus();
        QApplication::clipboard()->clear();
        QTest::keyClick(tree, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),
            QStringLiteral("com.dji.camera.ColorGammaSxS : D-Log"));
        // Clearing the search brings everything back; Copy All gives the whole file.
        panel.fullFilter()->clear();
        QCOMPARE(tree->topLevelItemCount(), all);
        panel.copyAll();
        QVERIFY(QApplication::clipboard()->text().contains(QStringLiteral("[Container]")));
        QVERIFY(QApplication::clipboard()->text().contains(QStringLiteral("D-Log")));
    }

    void severalClipsAndOfflineFiles()
    {
        PanelFixture f;
        MetadataPanel panel(f.ctx);
        panel.resize(420, 600);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        const QString a = addItem(f.project, QStringLiteral("A"), m_file);
        const QString b = addItem(f.project, QStringLiteral("Gone"), QStringLiteral("/nonexistent/x.mov"));
        panel.setFullView(true);
        panel.showMediaItems({a, b});
        QCOMPARE(panel.fullTree()->topLevelItemCount(), 0); // one clip at a time
        panel.showMedia(b);
        QTRY_VERIFY(panel.fullTree()->topLevelItemCount() == 1); // the error is shown, not nothing
        QCOMPARE(panel.fullTree()->topLevelItem(0)->text(0), QStringLiteral("Error"));
        panel.showMedia(a);
        QTRY_VERIFY(panel.fullTree()->topLevelItemCount() > 3);
    }
};

QTEST_MAIN(TestMetadataFull)
#include "test_metadatafull.moc"
