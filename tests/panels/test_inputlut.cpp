// SPDX-License-Identifier: GPL-3.0-or-later
// Input LUT of a media item (Media Pool / Color page right-click > 3D LUT, ClipAttributes::
// inputLut): set through the menu, undoable, saved as an optional key (no format bump), mirrored
// into the clips, shown on the Media Pool poster. The renders (GPU / CPU parity, before the node
// graph) and the render cache key are tested in tests/render/test_graderender.cpp.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/MediaPool.h"
#include "core/Serialization.h"
#include "platform/FileDialogs.h"
#include "ui/color/ColorPalettes.h"
#include "ui/common/InputLutMenu.h"
#include "ui/common/Resources.h"
#include "ui/dialogs/ClipAttributesDialog.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace kk;
using namespace kk::test;

namespace {

/// A 2-point 1D .cube that inverts every channel.
QByteArray invertCube()
{
    return "TITLE \"invert\"\nLUT_1D_SIZE 2\n1 1 1\n0 0 0\n";
}

QString addVideoItem(Project& project, const QString& name, const QString& path)
{
    MediaItem item;
    item.path = path;
    item.name = name;
    item.info.valid = true;
    item.info.hasVideo = true;
    item.info.width = 64;
    item.info.height = 36;
    item.info.frameRate = {25, 1};
    item.info.durationUs = 10'000'000;
    project.beginEdit(QStringLiteral("add"));
    const QString id = project.mediaPool()->addItem(item);
    project.endEdit();
    return id;
}

QString writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(data);
    return path;
}

/// The checked entries of @p menu (recursively): their texts.
QStringList checkedTexts(const QMenu* menu)
{
    QStringList out;
    for (const QAction* a : menu->actions()) {
        if (a->menu())
            out += checkedTexts(a->menu());
        else if (a->isChecked())
            out.append(a->text());
    }
    return out;
}

QAction* findAction(const QMenu* menu, const QString& text)
{
    for (QAction* a : menu->actions()) {
        if (a->text() == text)
            return a;
        if (a->menu()) {
            if (QAction* inner = findAction(a->menu(), text))
                return inner;
        }
    }
    return nullptr;
}

} // namespace

class TestInputLut : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_lutA, m_lutB, m_sub;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_inputlut"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
        QVERIFY(m_dir.isValid());
        // The LUT library: one folder with two LUTs and a subfolder with one.
        const QString lib = m_dir.filePath(QStringLiteral("luts"));
        m_lutA = writeFile(lib + QStringLiteral("/Alpha.cube"), invertCube());
        m_lutB = writeFile(lib + QStringLiteral("/Beta.cube"), invertCube());
        m_sub = writeFile(lib + QStringLiteral("/Film/Gamma.cube"), invertCube());
        QVERIFY(!m_lutA.isEmpty() && !m_lutB.isEmpty() && !m_sub.isEmpty());
        QSettings().setValue(QStringLiteral("color/lutFolders"), QStringList {lib});
    }

    void cleanupTestCase() { platform::setFileDialogHandlerForTests({}); }

    void menuListsTheLibrary()
    {
        QMenu menu;
        QStringList picked;
        inputlut::populate(&menu, QString(), nullptr, [&](const QString& p) { picked.append(p); });
        // None first (checked: no LUT), then the library files, the subfolder as a submenu.
        QCOMPARE(menu.actions().first()->text(), QStringLiteral("None"));
        QVERIFY(menu.actions().first()->isChecked());
        QVERIFY(findAction(&menu, QStringLiteral("Alpha")));
        QVERIFY(findAction(&menu, QStringLiteral("Beta")));
        QVERIFY(findAction(&menu, QStringLiteral("Gamma")));
        QVERIFY(findAction(&menu, QStringLiteral("Film"))->menu()); // the subfolder
        QVERIFY(findAction(&menu, QStringLiteral("Browse…")));
        findAction(&menu, QStringLiteral("Beta"))->trigger();
        findAction(&menu, QStringLiteral("None"))->trigger();
        findAction(&menu, QStringLiteral("Gamma"))->trigger();
        QCOMPARE(picked, (QStringList {m_lutB, QString(), m_sub}));
    }

    void menuChecksTheCurrentLut()
    {
        QMenu inLibrary;
        inputlut::populate(&inLibrary, m_lutB, nullptr, [](const QString&) {});
        QCOMPARE(checkedTexts(&inLibrary), (QStringList {QStringLiteral("Beta")}));
        // A file outside the library gets its own checked entry.
        const QString other = writeFile(m_dir.filePath(QStringLiteral("elsewhere/Delta.cube")), invertCube());
        QMenu outside;
        inputlut::populate(&outside, other, nullptr, [](const QString&) {});
        QCOMPARE(checkedTexts(&outside), (QStringList {QStringLiteral("Delta")}));
    }

    void browseAsksForAFile()
    {
        const QString other = writeFile(m_dir.filePath(QStringLiteral("browse/Epsilon.cube")), invertCube());
        platform::setFileDialogHandlerForTests([&](const platform::FileDialogRequest&) {
            return QStringList {other};
        });
        QMenu menu;
        QStringList picked;
        inputlut::populate(&menu, QString(), nullptr, [&](const QString& p) { picked.append(p); });
        findAction(&menu, QStringLiteral("Browse…"))->trigger();
        QCOMPARE(picked, (QStringList {other}));
        // Cancelled: nothing picked.
        platform::setFileDialogHandlerForTests(
            [](const platform::FileDialogRequest&) { return QStringList(); });
        findAction(&menu, QStringLiteral("Browse…"))->trigger();
        QCOMPARE(picked.size(), 1);
        platform::setFileDialogHandlerForTests({});
    }

    void setUndoRedoAndClipMirror()
    {
        PanelFixture f;
        const QString id = addVideoItem(f.project, QStringLiteral("Clip"), m_lutA);
        // A clip of the item on the timeline.
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = id;
        clip.start = 0;
        clip.duration = 50;
        f.project.beginEdit(QStringLiteral("clip"));
        const quint64 clipId
            = editops::overwriteClip(f.project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        f.project.endEdit();
        QVERIFY(f.project.mediaPool()->item(id)->attributes.inputLut.isEmpty());
        QVERIFY(clipattributes::setInputLut(f.project, {id}, m_lutA));
        QCOMPARE(f.project.mediaPool()->item(id)->attributes.inputLut, m_lutA);
        QCOMPARE(f.clip(clipId)->attributes.inputLut, m_lutA); // the render snapshot carries it
        // The other attributes stay.
        QVERIFY(f.project.mediaPool()->item(id)->attributes.hasInputLut());
        // The same LUT again changes nothing (no undo step).
        QVERIFY(!clipattributes::setInputLut(f.project, {id}, m_lutA));
        // One undo step.
        f.project.undo();
        QVERIFY(f.project.mediaPool()->item(id)->attributes.inputLut.isEmpty());
        QVERIFY(f.clip(clipId)->attributes.inputLut.isEmpty());
        f.project.redo();
        QCOMPARE(f.clip(clipId)->attributes.inputLut, m_lutA);
        // None removes it.
        QVERIFY(clipattributes::setInputLut(f.project, {id}, QString()));
        QVERIFY(!f.project.mediaPool()->item(id)->attributes.hasInputLut());
    }

    void savedAsAnOptionalKeyWithoutAFormatBump()
    {
        Project project;
        const QString id = addVideoItem(project, QStringLiteral("Clip"), m_lutA);
        const QString path = m_dir.filePath(QStringLiteral("lut.kkproj"));
        // Without a LUT the project has no such key at all (an older build sees nothing new).
        QVERIFY(project.save(path));
        QFile plain(path);
        QVERIFY(plain.open(QIODevice::ReadOnly));
        const QByteArray plainBytes = plain.readAll();
        QVERIFY(!plainBytes.contains("inputLut"));
        plain.close();
        QVERIFY(clipattributes::setInputLut(project, {id}, m_lutB));
        QVERIFY(project.save(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(root.value(QLatin1String("formatVersion")).toInt(), serialization::kFormatVersion);
        f.close();
        Project loaded;
        QString error;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const MediaItem* item = nullptr;
        for (const MediaItem& m : loaded.mediaPool()->state().items) {
            if (!m.isTimeline())
                item = &m;
        }
        QVERIFY(item);
        QCOMPARE(item->attributes.inputLut, m_lutB);
        // JSON level: only the LUT differs from the default attributes.
        ClipAttributes a;
        a.inputLut = m_lutA;
        const QJsonObject o = serialization::attributesToJson(a);
        QCOMPARE(o.keys(), (QStringList {QStringLiteral("inputLut")}));
        QCOMPARE(serialization::attributesFromJson(o).inputLut, m_lutA);
        // An old project (attributes without the key) loads with no LUT.
        QVERIFY(!serialization::attributesFromJson(QJsonObject {{"rotation", 90}}).hasInputLut());
    }

    void posterShowsTheLut()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider([](const QString&, int height) {
            QImage image(height * 16 / 9, height, QImage::Format_ARGB32_Premultiplied);
            image.fill(QColor(200, 40, 40));
            return image;
        });
        const QString id = addVideoItem(f.project, QStringLiteral("Clip"), m_lutA);
        auto posterColor = [&] {
            const QModelIndex index = panel.model()->index(1, 0);
            const QPixmap poster = index.data(MediaPoolModel::PosterRole).value<QPixmap>();
            return poster.toImage().pixelColor(poster.width() / 2, poster.height() / 2);
        };
        const QColor plain = posterColor();
        QVERIFY(plain.red() > 150 && plain.green() < 80);
        QVERIFY(clipattributes::setInputLut(f.project, {id}, m_lutA)); // inverts
        const QColor lut = posterColor();
        QVERIFY2(lut.red() < 90 && lut.green() > 170, qPrintable(lut.name()));
        QVERIFY(clipattributes::setInputLut(f.project, {id}, QString()));
        QCOMPARE(posterColor().name(), plain.name());
    }

    void poolContextMenuHasTheSubmenuAndAppliesIt()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        panel.resize(600, 400);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        const QString id = addVideoItem(f.project, QStringLiteral("Clip"), m_lutA);
        QVERIFY(panel.selectMedia(id));
        QAbstractItemView* view = nullptr;
        for (QAbstractItemView* v : panel.findChildren<QAbstractItemView*>()) {
            if (v->isVisible() && v->model() == panel.model()) {
                view = v;
                break;
            }
        }
        if (!view) {
            for (QAbstractItemView* v : panel.findChildren<QAbstractItemView*>()) {
                if (v->isVisible() && v->model()) {
                    view = v;
                    break;
                }
            }
        }
        QVERIFY(view);
        bool found = false;
        QTimer::singleShot(0, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu)
                return;
            QAction* sub = nullptr;
            for (QAction* a : menu->actions()) {
                if (a->text() == QStringLiteral("3D LUT"))
                    sub = a;
            }
            found = sub && sub->menu();
            if (found)
                findAction(sub->menu(), QStringLiteral("Alpha"))->trigger();
            menu->close();
        });
        emit view->customContextMenuRequested(QPoint(10, 10));
        QVERIFY(found);
        QCOMPARE(f.project.mediaPool()->item(id)->attributes.inputLut, m_lutA);
    }
};

QTEST_MAIN(TestInputLut)
#include "test_inputlut.moc"
