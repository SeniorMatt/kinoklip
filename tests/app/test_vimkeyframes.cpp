// SPDX-License-Identifier: GPL-3.0-or-later
// Vim keys in the Keyframes panel (spec section 83, key context "keyframes"): the keyboard cursor
// of the dope sheet and the curve editor, selecting, moving, deleting, copying and easing keys.
#include "../core/ClassicTimeline.h"
#include "app/Theme.h"
#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/VimInput.h"
#include "ui/keyframes/CurveView.h"
#include "ui/keyframes/DopeSheetView.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/keyframes/KeyframeParamTree.h"
#include "ui/keyframes/KeyframesPanel.h"
#include "ui/keyframes/VimKeyframeHost.h"

#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QUndoStack>

using namespace kk;
namespace ko = kk::keyframeops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};
const QString kPosX = QStringLiteral("transform/posX");

/// A 100-frame clip at frame 50: posX keys at clip frames 0 and 40, opacity a key at 40;
/// a panel showing it, the Vim layer on, the focus on the dope sheet.
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    quint64 video = 0;
    std::unique_ptr<KeyframesPanel> panel;
    struct NullHost final : VimHost
    {
        bool trigger(const QString&) override { return false; }
        int64_t playhead() const override { return 0; }
        void setPlayhead(int64_t) override { }
        void clearSelection() override { }
        void openCommandLine() override { }
    } nullHost;
    VimInput input {nullptr};
    VimKeyframeHost host {nullptr};

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        kk::test::useClassicTimeline(project);
        ClipData v;
        v.name = QStringLiteral("Shot");
        v.mediaId = QStringLiteral("m1");
        v.start = 50;
        v.duration = 100;
        v.transform.animated(keys::PosX, 0).setKey(0, 0.0);
        v.transform.animated(keys::PosX, 0).setKey(40, 100.0);
        v.transform.animated(keys::Opacity, 100).setKey(40, 50.0);
        project.beginEdit(QStringLiteral("setup"));
        video = editops::overwriteClip(project.timeline()->mutableData(), kV1, v);
        project.endEdit();
        project.undoStack()->clear();
        keyframecommands::clearClipboard();
        panel = std::make_unique<KeyframesPanel>(ctx);
        panel->resize(1400, 600);
        panel->show();
        QVERIFY(QTest::qWaitForWindowExposed(panel.get()));
        selection.selectClip(video);
        input.setHost(&nullHost);
        input.setContextHost(QStringLiteral("keyframes"), &host);
        input.setActive(true);
        panel->dopeSheet()->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
    }
    int steps() const { return project.undoStack()->count(); }
    const AnimatedValue& value(const QString& path) const
    {
        return *ko::findValue(*editops::clipById(project.timeline()->data(), video), path);
    }
    int rowOf(const QString& label) const
    {
        for (int i = 0; i < panel->tree()->rowCount(); ++i) {
            if (panel->tree()->entryAt(i).label == label)
                return i;
        }
        return -1;
    }
    KeyframeViewBase* view() const { return host.view(); }
    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            QWidget* focus = QApplication::focusWidget();
            if (c == QLatin1Char(' '))
                QTest::keyClick(focus, Qt::Key_Space);
            else if (c.isUpper() || c == QLatin1Char('$') || c == QLatin1Char('>')
                || c == QLatin1Char('<'))
                QTest::keyClick(focus, c.toLatin1(), Qt::ShiftModifier);
            else
                QTest::keyClick(focus, c.toLatin1());
        }
    }
    void ctrl(Qt::Key key)
    {
        QTest::keyClick(QApplication::focusWidget(), key, Qt::ControlModifier);
    }
    void enter() { QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return); }
    /// The cursor on the Position X row at its first key.
    void onPosX()
    {
        view()->moveCursorToRow(rowOf(QStringLiteral("Position X")));
        type(QStringLiteral("0"));
    }
    ParamId posXId() const { return {video, kPosX}; }
};

} // namespace

class TestVimKeyframes : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimkeyframes"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void theViewsAreTheKeyframesContext()
    {
        Fixture f;
        QCOMPARE(VimInput::contextOf(f.panel->dopeSheet()), QStringLiteral("keyframes"));
        QCOMPARE(VimInput::contextOf(f.panel->curveView()), QStringLiteral("keyframes"));
        // Focusing the view puts the cursor on the first row.
        QVERIFY(f.view()->hasCursor());
        QCOMPARE(f.view()->cursorRow(), 0);
    }

    void hlMoveOverTheKeysOfARow()
    {
        Fixture f;
        f.onPosX();
        QCOMPARE(f.view()->cursorRow(), f.rowOf(QStringLiteral("Position X")));
        QCOMPARE(f.view()->cursorFrame(), int64_t(50)); // clip frame 0
        f.type(QStringLiteral("l"));
        QCOMPARE(f.view()->cursorFrame(), int64_t(90));
        f.type(QStringLiteral("l")); // at the last key: stays
        QCOMPARE(f.view()->cursorFrame(), int64_t(90));
        f.type(QStringLiteral("h"));
        QCOMPARE(f.view()->cursorFrame(), int64_t(50));
        f.type(QStringLiteral("$"));
        QCOMPARE(f.view()->cursorFrame(), int64_t(90));
        f.type(QStringLiteral("0"));
        QCOMPARE(f.view()->cursorFrame(), int64_t(50));
        f.type(QStringLiteral("5l"));
        QCOMPARE(f.view()->cursorFrame(), int64_t(90));
    }

    void jkMoveOverTheRows()
    {
        Fixture f;
        f.type(QStringLiteral("gg"));
        QCOMPARE(f.view()->cursorRow(), 0);
        f.type(QStringLiteral("j"));
        QCOMPARE(f.view()->cursorRow(), 1);
        f.type(QStringLiteral("2j"));
        QCOMPARE(f.view()->cursorRow(), 3);
        f.type(QStringLiteral("k"));
        QCOMPARE(f.view()->cursorRow(), 2);
        f.type(QStringLiteral("G"));
        QCOMPARE(f.view()->cursorRow(), f.panel->tree()->rowCount() - 1);
        f.type(QStringLiteral("2G"));
        QCOMPARE(f.view()->cursorRow(), 1);
    }

    void vSelectsTheKeysUnderTheCursor()
    {
        Fixture f;
        f.onPosX();
        f.type(QStringLiteral("v"));
        QCOMPARE(f.selection.keyframes(), (QVector<KeyframeRef> {{f.video, kPosX, 0}}));
        f.type(QStringLiteral("l"));
        f.type(QStringLiteral("v"));
        QCOMPARE(f.selection.keyframes().size(), 2);
        f.type(QStringLiteral("v")); // again: unselect
        QCOMPARE(f.selection.keyframes().size(), 1);
        f.type(QStringLiteral("U"));
        QVERIFY(f.selection.keyframes().isEmpty());
        f.type(QStringLiteral("V"));
        QCOMPARE(f.selection.keyframes().size(), 2);
    }

    void ddDeletesTheKeyUnderTheCursorInOneStep()
    {
        Fixture f;
        f.onPosX();
        f.type(QStringLiteral("l"));
        const int before = f.steps();
        f.type(QStringLiteral("dd"));
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(!f.value(kPosX).hasKeyAt(40));
        QVERIFY(f.value(kPosX).hasKeyAt(0));
        QCOMPARE(f.view()->cursorFrame(), int64_t(50)); // the cursor stays on a key
        f.project.undo();
        QVERIFY(f.value(kPosX).hasKeyAt(40));
    }

    void ctrlAMovesKeysLaterInTheDopeSheet()
    {
        Fixture f;
        f.onPosX();
        f.type(QStringLiteral("l"));
        const int before = f.steps();
        f.type(QStringLiteral("3"));
        f.ctrl(Qt::Key_A);
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(f.value(kPosX).hasKeyAt(43));
        QVERIFY(!f.value(kPosX).hasKeyAt(40));
        QCOMPARE(f.view()->cursorFrame(), int64_t(93)); // the cursor follows
        f.type(QStringLiteral("<"));
        QVERIFY(f.value(kPosX).hasKeyAt(42));
        f.type(QStringLiteral("5<"));
        QVERIFY(f.value(kPosX).hasKeyAt(37));
        f.type(QStringLiteral(">"));
        QVERIFY(f.value(kPosX).hasKeyAt(38));
        // Keys stay inside the clip.
        f.type(QStringLiteral("999>"));
        QVERIFY(f.value(kPosX).hasKeyAt(99));
        // Selected keys move together.
        f.type(QStringLiteral("60<"));
        QVERIFY(f.value(kPosX).hasKeyAt(39));
        f.type(QStringLiteral("V"));
        f.type(QStringLiteral("2>"));
        QVERIFY(f.value(kPosX).hasKeyAt(2));
        QVERIFY(f.value(kPosX).hasKeyAt(41));
    }

    void yyAndPCopyKeys()
    {
        Fixture f;
        f.onPosX();
        f.type(QStringLiteral("yy"));
        QVERIFY(keyframecommands::hasCopied());
        f.project.timeline()->setPlayhead(120); // clip frame 70
        f.type(QStringLiteral("p"));
        QVERIFY(f.value(kPosX).hasKeyAt(70));
        QVERIFY(f.value(kPosX).hasKeyAt(0));
    }

    void oAddsAKeyAtThePlayhead()
    {
        Fixture f;
        f.onPosX();
        f.project.timeline()->setPlayhead(100); // clip frame 50
        const int before = f.steps();
        f.type(QStringLiteral("o"));
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(f.value(kPosX).hasKeyAt(50));
        QCOMPARE(f.view()->cursorFrame(), int64_t(100));
    }

    void easingKeysApplyToTheKeyUnderTheCursor()
    {
        Fixture f;
        f.onPosX();
        f.type(QStringLiteral(" ki"));
        QCOMPARE(keyInterp(f.value(kPosX).keys.first()), Interp::EaseIn);
        f.type(QStringLiteral(" kh"));
        QCOMPARE(keyInterp(f.value(kPosX).keys.first()), Interp::Hold);
        f.type(QStringLiteral(" kl"));
        QCOMPARE(keyInterp(f.value(kPosX).keys.first()), Interp::Linear);
        // The other key is untouched by these.
        QVERIFY(keyInterp(f.value(kPosX).keys.last()) != Interp::Hold);
    }

    void enterJumpsThePlayheadToTheKey()
    {
        Fixture f;
        f.onPosX();
        f.type(QStringLiteral("l"));
        f.enter();
        QCOMPARE(f.project.timeline()->playhead(), int64_t(90));
    }

    void gsSwitchesToCurvesWhereCtrlAChangesTheValue()
    {
        Fixture f;
        f.type(QStringLiteral("gs"));
        QCOMPARE(f.panel->view(), KeyframesPanel::View::Curves);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(f.panel->curveView()));
        QVERIFY(f.view()->hasCursor());
        // The curve rows are the shown curves: Position X is the first.
        f.view()->moveCursorToRow(0);
        f.type(QStringLiteral("0"));
        const double before = f.value(kPosX).keys.first().value;
        const int steps = f.steps();
        f.type(QStringLiteral("4"));
        f.ctrl(Qt::Key_A);
        QVERIFY(f.value(kPosX).keys.first().value > before);
        QCOMPARE(f.steps(), steps + 1);
        const double raised = f.value(kPosX).keys.first().value;
        f.ctrl(Qt::Key_X);
        f.ctrl(Qt::Key_X);
        QVERIFY(f.value(kPosX).keys.first().value < raised);
        f.type(QStringLiteral("gs"));
        QCOMPARE(f.panel->view(), KeyframesPanel::View::Keyframes);
    }

    void unboundContextKeysAreNotSwallowedAndTheCursorHidesWithVimOff()
    {
        Fixture f;
        QVERIFY(f.view()->hasCursor());
        f.host.setCursorVisible(false);
        QVERIFY(!f.view()->hasCursor());
    }
};

QTEST_MAIN(TestVimKeyframes)
#include "test_vimkeyframes.moc"
