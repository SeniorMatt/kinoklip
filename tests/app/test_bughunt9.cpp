// SPDX-License-Identifier: GPL-3.0-or-later
// Ninth bug-hunting round (a beginner's full project, round 8 leftovers), in the real main
// window where it matters: one test per fix, named after the spec row it covers.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/DynamicZoom.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Subtitles.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/deliver/RenderSettingsPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTest>
#include <QToolBar>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

/// Everything main() creates, with @p clipCount clips of 50 frames on V1 (Solid, or Media
/// with 20 s of material when @p media).
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;

    explicit Fixture(int clipCount = 2, bool media = false)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        project.beginEdit(QStringLiteral("Add Clips"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 320;
        tl.height = 180;
        for (int i = 0; i < clipCount; ++i) {
            ClipData clip;
            clip.kind = media ? ClipKind::Media : ClipKind::Solid;
            clip.solidColor = QColor(40 + 60 * i, 90, 200);
            clip.name = QStringLiteral("Clip %1").arg(i + 1);
            clip.start = i * 50;
            clip.duration = 50;
            clip.sourceDurationUs = 20'000'000;
            clips.append(editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip));
        }
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    const ClipData* clip(int i) const
    {
        return editops::clipById(project.timeline()->data(), clips[i]);
    }
    InspectorPanel* inspector() const { return window->editPage()->inspector(); }
    ParamGroupBox* group(const char* name) const
    {
        return inspector()->findChild<ParamGroupBox*>(QLatin1String(name));
    }
    void show()
    {
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
};

} // namespace

class TestBughunt9 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt9"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }

    // Spec 28.19: with several captions selected, typing in the Captions tab changes the
    // shown caption only (each caption has its own words); its name follows only its text.
    void spec28_19_captionTextIsPerCaption()
    {
        Fixture f;
        f.show();
        TimelineController* controller = f.window->editPage()->timeline()->controller();
        f.project.timeline()->setPlayhead(10);
        const quint64 a = controller->addSubtitleAtPlayhead(0);
        f.project.timeline()->setPlayhead(200);
        const quint64 b = controller->addSubtitleAtPlayhead(0);
        QVERIFY(a && b && a != b);
        const QString textA = subtitles::captionText(*editops::clipById(f.project.timeline()->data(), a));
        f.selection.setClips({a, b}); // b is the shown (current) caption
        QCOMPARE(f.selection.currentClip(), b);
        auto* edit
            = f.inspector()->findChild<QPlainTextEdit*>(QStringLiteral("text:") + keys::Text);
        QVERIFY(edit);
        edit->setPlainText(QStringLiteral("Only me"));
        const TimelineData& tl = f.project.timeline()->data();
        QCOMPARE(subtitles::captionText(*editops::clipById(tl, b)), QStringLiteral("Only me"));
        QCOMPARE(subtitles::captionText(*editops::clipById(tl, a)), textA);
        QVERIFY(editops::clipById(tl, a)->name != QStringLiteral("Only me"));
    }

    // Spec 28.20: a cue without text is skipped on import (it would be an invisible caption)
    // and the import says how many were skipped.
    void spec28_20_emptyCuesSkippedAndCounted()
    {
        const QByteArray srt = "1\n00:00:01,000 --> 00:00:02,000\nHello\n\n"
                               "2\n00:00:03,000 --> 00:00:04,000\n<i></i>\n\n"
                               "3\n00:00:05,000 --> 00:00:06,000\n\n"
                               "4\n00:00:07,000 --> 00:00:08,000\nBye\n";
        QVector<SubtitleCue> cues;
        int empty = -1;
        QVERIFY(subtitles::parse(srt, &cues, nullptr, nullptr, &empty));
        QCOMPARE(cues.size(), 2);
        QCOMPARE(cues[0].text, QStringLiteral("Hello"));
        QCOMPARE(cues[1].text, QStringLiteral("Bye"));
        QCOMPARE(empty, 2);

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("empty_cues.srt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(srt);
        file.close();
        Project project;
        const ImportResult result = project.importMedia({path});
        QCOMPARE(result.importedIds.size(), 1);
        QVERIFY(result.warnings.isEmpty());
        QCOMPARE(result.notes.size(), 1);
        QVERIFY2(result.notes[0].contains(QStringLiteral("2 empty subtitle cues were skipped")),
            qPrintable(result.notes[0]));
        const MediaItem* item = project.mediaPool()->item(result.importedIds[0]);
        QVERIFY(item);
        QCOMPARE(item->cues.size(), 2);

        // A file of only empty cues is not imported.
        QVector<SubtitleCue> none;
        QString error;
        QVERIFY(!subtitles::parse("1\n00:00:01,000 --> 00:00:02,000\n\n", &none, &error));
        QVERIFY(!error.isEmpty());
    }

    // Spec 45.2: Export subtitle is greyed out for audio-only formats (no picture to burn into,
    // nothing to embed into) and an audio render never carries subtitles.
    void spec45_2_noSubtitleExportForAudio()
    {
        Fixture f;
        f.window->setCurrentPage(QStringLiteral("deliver"));
        RenderSettingsPanel* panel = f.window->deliverPage()->settingsPanel();
        auto* box = panel->findChild<QCheckBox*>(QStringLiteral("RenderSubtitleExport"));
        QVERIFY(box);
        ExportSettings s = panel->settings();
        s.subtitles.enabled = true;
        panel->setSettings(s);
        QVERIFY(box->isEnabled());
        QVERIFY(panel->settings().subtitles.enabled);
        for (const char* audio : {"wav", "mp3", "flac"}) {
            s = panel->settings();
            s.container = QLatin1String(audio);
            s.normalize();
            s.subtitles.enabled = true;
            panel->setSettings(s);
            QVERIFY2(!box->isEnabled(), audio);
            QVERIFY2(!panel->settings().subtitles.enabled, audio);
        }
    }
};

QTEST_MAIN(TestBughunt9)
#include "test_bughunt9.moc"
