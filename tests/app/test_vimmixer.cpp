// SPDX-License-Identifier: GPL-3.0-or-later
// Vim keys in the Mixer (spec section 83, key context "mixer"): choosing a strip with h and l,
// the fader with j k Ctrl+A Ctrl+X and 0, pan, mute, solo and the EQ window.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/VimController.h"
#include "app/pages/AudioPage.h"
#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/audio/EqWindow.h"
#include "ui/audio/FaderWidget.h"
#include "ui/audio/MixerPanel.h"
#include "ui/audio/MixerStrip.h"
#include "ui/audio/VimMixerHost.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimOptions.h"

#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        actions.action(QLatin1String(vimopts::kActionId))->setChecked(true);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
        window->resize(1800, 1000);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        actions.action("view.page_audio")->trigger();
        QCoreApplication::processEvents();
        actions.action("timeline.add_audio_track")->trigger(); // A2
        QCoreApplication::processEvents();
        QVERIFY(window->vimController()->focusPanelKey(QStringLiteral("mixer")));
    }
    const TimelineData& data() const { return project.timeline()->data(); }
    int steps() const { return project.undoStack()->count(); }
    MixerPanel* mixer() const { return window->audioPage()->mixer(); }
    VimMixerHost* host() const { return window->vimController()->mixerHost(); }
    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            QWidget* focus = QApplication::focusWidget();
            if (c == QLatin1Char('>') || c == QLatin1Char('<'))
                QTest::keyClick(focus, c.toLatin1(), Qt::ShiftModifier);
            else
                QTest::keyClick(focus, c.toLatin1());
        }
    }
    void ctrl(Qt::Key key)
    {
        QTest::keyClick(QApplication::focusWidget(), key, Qt::ControlModifier);
    }
};

} // namespace

class TestVimMixer : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimmixer"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void theMixerIsAContext()
    {
        Fixture f;
        QCOMPARE(VimInput::contextOf(QApplication::focusWidget()), QStringLiteral("mixer"));
        QCOMPARE(f.host()->index(), 0);
        QVERIFY(f.host()->current());
    }

    void hlChooseTheStrip()
    {
        Fixture f;
        QVERIFY(f.mixer()->strips().size() >= 2);
        f.type(QStringLiteral("l"));
        QCOMPARE(f.host()->index(), 1);
        QCOMPARE(f.host()->current(), f.mixer()->strips()[1]);
        f.type(QStringLiteral("h"));
        QCOMPARE(f.host()->index(), 0);
        f.type(QStringLiteral("G"));
        QCOMPARE(f.host()->index(), int(f.mixer()->strips().size()) - 1);
        f.type(QStringLiteral("gg"));
        QCOMPARE(f.host()->index(), 0);
        f.type(QStringLiteral("2G"));
        QCOMPARE(f.host()->index(), 1);
    }

    void jkAndCtrlAMoveTheFaderByHalfADecibelInOneStepEach()
    {
        Fixture f;
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, 0.0);
        const int before = f.steps();
        f.type(QStringLiteral("j"));
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, -0.5);
        QCOMPARE(f.steps(), before + 1);
        f.type(QStringLiteral("6j"));
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, -3.5);
        QCOMPARE(f.steps(), before + 2);
        f.type(QStringLiteral("k"));
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, -3.0);
        f.ctrl(Qt::Key_A);
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, -2.5);
        f.type(QStringLiteral("2"));
        f.ctrl(Qt::Key_X);
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, -3.5);
        // 0: back to 0 dB; the fader widget follows.
        f.type(QStringLiteral("0"));
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, 0.0);
        QCOMPARE(f.mixer()->strips()[0]->fader()->value(), 0.0);
        // The ceiling is +12 dB.
        f.type(QStringLiteral("100k"));
        QCOMPARE(f.data().audioTracks[1 - 1].mix.strip.volumeDb, 12.0);
        // The keys act on the strip the cursor is on.
        f.type(QStringLiteral("l"));
        f.type(QStringLiteral("2j"));
        QCOMPARE(f.data().audioTracks[1].mix.strip.volumeDb, -1.0);
    }

    void panMuteAndSolo()
    {
        Fixture f;
        f.type(QStringLiteral("4>"));
        QVERIFY(qAbs(f.data().audioTracks[0].mix.strip.pan - 0.2) < 1e-6);
        f.type(QStringLiteral("<"));
        QVERIFY(qAbs(f.data().audioTracks[0].mix.strip.pan - 0.15) < 1e-6);
        f.type(QStringLiteral("m"));
        QVERIFY(f.data().audioTracks[0].muted);
        QVERIFY(f.mixer()->strips()[0]->muteButton()->isChecked());
        f.type(QStringLiteral("m"));
        QVERIFY(!f.data().audioTracks[0].muted);
        f.type(QStringLiteral("s"));
        QVERIFY(f.data().audioTracks[0].solo);
        f.type(QStringLiteral("s"));
        QVERIFY(!f.data().audioTracks[0].solo);
    }

    void enterOpensTheEq()
    {
        Fixture f;
        f.type(QStringLiteral("l"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QVERIFY(f.mixer()->strips()[1]->findChild<EqWindow*>() || QApplication::activeWindow());
        bool open = false;
        for (QWidget* w : QApplication::topLevelWidgets())
            open = open || (qobject_cast<EqWindow*>(w) && w->isVisible());
        QVERIFY(open);
    }
};

QTEST_MAIN(TestVimMixer)
#include "test_vimmixer.moc"
