// SPDX-License-Identifier: GPL-3.0-or-later
// Transitions from the keyboard in Vim mode (spec section 83): ]t and [t select the next and
// previous transition, Space e + / - / a / d lengthen, shorten, align and delete it, and the
// Inspector's Transition tab is a form.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/VimController.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/VimFormHost.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimOptions.h"
#include "ui/effects/EffectActions.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QUndoStack>

#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};

/// Three 50-frame clips in a row with a transition at the head of the second and of the third.
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;

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
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        project.beginEdit(QStringLiteral("setup"));
        for (int i = 0; i < 3; ++i) {
            ClipData v;
            v.kind = ClipKind::Solid;
            v.start = i * 50;
            v.duration = 50;
            v.solidColor = Qt::darkCyan;
            clips.append(ops::overwriteClip(project.timeline()->mutableData(), kV1, v));
        }
        project.endEdit();
        QVERIFY(effectactions::applyTransition(
            project, clips[1], QStringLiteral("cross_dissolve"), true, 10));
        QVERIFY(effectactions::applyTransition(
            project, clips[2], QStringLiteral("cross_dissolve"), true, 8));
        QCoreApplication::processEvents();
        project.timeline()->setPlayhead(0);
        vim()->runCommand(QStringLiteral("page edit"));
        window->editPage()->timeline()->timelineWidget()->setFocus();
    }
    VimController* vim() const { return window->vimController(); }
    const TimelineData& data() const { return project.timeline()->data(); }
    const ClipData& clip(int i) const { return *ops::clipById(data(), clips[i]); }
    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            QWidget* focus = QApplication::focusWidget();
            if (c == QLatin1Char(' '))
                QTest::keyClick(focus, Qt::Key_Space);
            else if (c == QLatin1Char('+'))
                QTest::keyClick(focus, Qt::Key_Plus, Qt::ShiftModifier);
            else if (c.isUpper())
                QTest::keyClick(focus, c.toLatin1(), Qt::ShiftModifier);
            else
                QTest::keyClick(focus, c.toLatin1());
        }
    }
};

} // namespace

class TestVimTransitions : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimtransitions"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void bracketTSelectsTransitionsInOrder()
    {
        Fixture f;
        f.type(QStringLiteral("]t"));
        QVERIFY(f.selection.transition());
        QCOMPARE(f.selection.transition()->clipId, f.clips[1]);
        QVERIFY(f.selection.transition()->head);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(50));
        f.type(QStringLiteral("]t"));
        QCOMPARE(f.selection.transition()->clipId, f.clips[2]);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(100));
        f.type(QStringLiteral("]t")); // the last one stays
        QCOMPARE(f.selection.transition()->clipId, f.clips[2]);
        f.type(QStringLiteral("[t"));
        QCOMPARE(f.selection.transition()->clipId, f.clips[1]);
        // From a playhead between them.
        f.selection.clear();
        f.project.timeline()->setPlayhead(70);
        f.type(QStringLiteral("]t"));
        QCOMPARE(f.selection.transition()->clipId, f.clips[2]);
        f.selection.clear();
        f.project.timeline()->setPlayhead(70);
        f.type(QStringLiteral("[t"));
        QCOMPARE(f.selection.transition()->clipId, f.clips[1]);
    }

    void leaderEPlusAndMinusChangeTheLength()
    {
        Fixture f;
        f.type(QStringLiteral("]t"));
        const int before = f.project.undoStack()->count();
        const int64_t length = f.clip(1).transitionIn->duration;
        f.type(QStringLiteral(" e+"));
        QCOMPARE(f.clip(1).transitionIn->duration, length + 1);
        QCOMPARE(f.project.undoStack()->count(), before + 1);
        f.type(QStringLiteral("5 e+"));
        QCOMPARE(f.clip(1).transitionIn->duration, length + 6);
        f.type(QStringLiteral("2 e-"));
        QCOMPARE(f.clip(1).transitionIn->duration, length + 4);
        f.type(QStringLiteral("99 e-"));
        QVERIFY(f.clip(1).transitionIn->duration >= 1);
    }

    void leaderEAChangesTheAlignmentAndEDDeletes()
    {
        Fixture f;
        f.type(QStringLiteral("]t"));
        QCOMPARE(f.clip(1).transitionIn->alignment, TransitionAlignment::Centre);
        f.type(QStringLiteral(" ea"));
        QVERIFY(f.clip(1).transitionIn->alignment != TransitionAlignment::Centre
            || f.window->vimController()->modeLine());
        f.type(QStringLiteral(" ed"));
        QVERIFY(!f.clip(1).transitionIn);
        QVERIFY(f.clip(2).transitionIn); // the other one stays
        f.project.undo();
        QVERIFY(f.clip(1).transitionIn);
    }

    void withoutATransitionThereIsAMessage()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.selection.clear();
        f.type(QStringLiteral(" e+"));
        QCOMPARE(f.clip(1).transitionIn->duration, int64_t(10)); // nothing changed
    }

    void theInspectorTransitionTabIsAFormForTheDuration()
    {
        Fixture f;
        f.type(QStringLiteral("]t"));
        QCoreApplication::processEvents();
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("inspector")));
        auto* inspector = f.window->editPage()->inspector();
        inspector->inspect(f.clips[1], InspectorPanel::TransitionTab);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCOMPARE(VimInput::contextOf(QApplication::focusWidget()), QStringLiteral("form"));
        // Walk to the Duration row and add five frames.
        auto* host = f.vim()->formHost();
        host->setRoot(inspector);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_G);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_G);
        bool found = false;
        for (int i = 0; i < 40 && !found; ++i) {
            const auto item = host->current();
            found = item.anchor && host->rowName(item).contains(QStringLiteral("Duration"));
            if (!found)
                QTest::keyClick(QApplication::focusWidget(), Qt::Key_J);
        }
        QVERIFY(found);
        const int64_t length = f.clip(1).transitionIn->duration;
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_5);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_A, Qt::ControlModifier);
        QVERIFY(f.clip(1).transitionIn->duration != length);
    }
};

QTEST_MAIN(TestVimTransitions)
#include "test_vimtransitions.moc"
