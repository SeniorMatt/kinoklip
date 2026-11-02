// SPDX-License-Identifier: GPL-3.0-or-later
// Integration test of the editing keys in the real main window (alpha.2 feedback: "ripple
// delete / ripple cut don't work"): real key events through QTest on the focused widget after
// clicking a clip, the shifted-character fallback (Ctrl+Shift+{ for Ctrl+Shift+[), and a scan of
// every QAction / QShortcut in the window for keys bound twice in the same context.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/ShortcutFallback.h"
#include "ui/common/StandardActions.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QMap>
#include <QSettings>
#include <QShortcut>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};
constexpr TrackRef kA1 {TrackType::Audio, 0};

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
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        window = std::make_unique<MainWindow>(ctx, options);
        window->resize(1600, 900);
        window->show();
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        // Three clips on V1 with linked audio: [0,40) [40,80) [80,120).
        project.beginEdit(QStringLiteral("setup"));
        for (int i = 0; i < 3; ++i) {
            ClipData v;
            v.kind = ClipKind::Solid;
            v.start = i * 40;
            v.duration = 40;
            v.solidColor = Qt::darkCyan;
            ClipData a = v;
            a.kind = ClipKind::Solid;
            const quint64 vid = ops::overwriteClip(project.timeline()->mutableData(), kV1, v);
            const quint64 aid = ops::overwriteClip(project.timeline()->mutableData(), kA1, a);
            ops::linkClips(project.timeline()->mutableData(), {vid, aid});
        }
        project.endEdit();
        timeline()->setPixelsPerFrame(4.0, 0);
        timeline()->scrollToFrame(0);
        QCoreApplication::processEvents();
    }

    TimelineWidget* timeline() const { return window->editPage()->timeline()->timelineWidget(); }

    QString layout(TrackRef ref) const
    {
        QStringList parts;
        for (const ClipData& c : project.timeline()->data().track(ref)->clips)
            parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
        return parts.join(QLatin1Char(' '));
    }

    /// Clicks V1 at @p frame with the mouse (selects the clip there, focuses the timeline).
    void clickClip(double frame)
    {
        const TimelineLayout& L = timeline()->layout();
        const QPoint p(int(std::lround(L.frameToX(frame))), L.rowRect(L.rowOf(kV1)).center().y());
        QTest::mouseClick(timeline()->viewport(), Qt::LeftButton, {}, p);
    }

    /// A real key press on whatever widget has the focus (as the user would type it).
    static void key(Qt::Key k, Qt::KeyboardModifiers mods = {})
    {
        QWidget* target = QApplication::focusWidget();
        QVERIFY2(target, "no focus widget");
        QTest::keyClick(target, k, mods);
    }
};

} // namespace

class TestEditKeys : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_editkeys"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void clickFocusesTimeline()
    {
        Fixture f;
        f.clickClip(10);
        QCOMPARE(f.selection.clips().size(), qsizetype(2)); // linked audio too
        QVERIFY(f.timeline()->hasFocus() || f.timeline()->viewport()->hasFocus());
    }

    void backspaceLifts()
    {
        Fixture f;
        f.clickClip(50);
        f.key(Qt::Key_Backspace);
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 80:40"));
        QCOMPARE(f.layout(kA1), QStringLiteral("0:40 80:40"));
    }

    void rippleDeleteKeys_data()
    {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("mods");
        QTest::newRow("Delete") << int(Qt::Key_Delete) << int(Qt::NoModifier);
        QTest::newRow("Shift+Backspace") << int(Qt::Key_Backspace) << int(Qt::ShiftModifier);
        QTest::newRow("Ctrl+Shift+X")
            << int(Qt::Key_X) << int(Qt::ControlModifier | Qt::ShiftModifier);
    }
    void rippleDeleteKeys()
    {
        QFETCH(int, key);
        QFETCH(int, mods);
        Fixture f;
        f.clickClip(50);
        const int before = f.project.undoStack()->count();
        f.key(Qt::Key(key), Qt::KeyboardModifiers(mods));
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40"));
        QCOMPARE(f.layout(kA1), QStringLiteral("0:40 40:40"));
        QCOMPARE(f.project.undoStack()->count(), before + 1);
    }

    void trimToPlayheadKeys_data()
    {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("mods");
        QTest::addColumn<QString>("expected");
        const int cs = int(Qt::ControlModifier | Qt::ShiftModifier);
        const int s = int(Qt::ShiftModifier);
        // Playhead at 50 inside the middle clip.
        QTest::newRow("Ctrl+Shift+[") << int(Qt::Key_BracketLeft) << cs << "0:40 40:30 70:40";
        QTest::newRow("Ctrl+Shift+{") << int(Qt::Key_BraceLeft) << cs << "0:40 40:30 70:40";
        QTest::newRow("Ctrl+Shift+]") << int(Qt::Key_BracketRight) << cs << "0:40 40:10 50:40";
        QTest::newRow("Ctrl+Shift+}") << int(Qt::Key_BraceRight) << cs << "0:40 40:10 50:40";
        QTest::newRow("Shift+[") << int(Qt::Key_BracketLeft) << s << "0:40 50:30 80:40";
        QTest::newRow("Shift+{") << int(Qt::Key_BraceLeft) << s << "0:40 50:30 80:40";
        QTest::newRow("Shift+]") << int(Qt::Key_BracketRight) << s << "0:40 40:10 80:40";
        QTest::newRow("Shift+}") << int(Qt::Key_BraceRight) << s << "0:40 40:10 80:40";
    }
    void trimToPlayheadKeys()
    {
        QFETCH(int, key);
        QFETCH(int, mods);
        QFETCH(QString, expected);
        Fixture f;
        f.clickClip(30); // focus the timeline on another clip, then clear the selection
        f.selection.clear();
        f.project.timeline()->setPlayhead(50);
        f.key(Qt::Key(key), Qt::KeyboardModifiers(mods));
        QCOMPARE(f.layout(kV1), expected);
        QCOMPARE(f.layout(kA1), expected);
    }

    void keysWorkWithViewerFocused()
    {
        Fixture f;
        f.clickClip(50);
        ViewerPanel* viewer = f.window->editPage()->timelineViewer();
        QTest::mouseClick(viewer, Qt::LeftButton, {}, viewer->rect().center());
        viewer->setFocus(); // a click on the picture focuses the viewer panel
        QCoreApplication::processEvents();
        f.key(Qt::Key_Delete);
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40"));
        f.selection.clear();
        f.project.timeline()->setPlayhead(50);
        f.key(Qt::Key_BraceRight, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:10"));
    }

    void fallbackSkipsTextInput()
    {
        Fixture f;
        auto* edit = new QLineEdit(f.window.get());
        edit->show();
        edit->setFocus();
        QCoreApplication::processEvents();
        f.project.timeline()->setPlayhead(50);
        QTest::keyClick(edit, Qt::Key_BraceLeft, Qt::ShiftModifier);
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40 80:40"));
        QCOMPARE(ShortcutFallback::unshiftedKey(Qt::Key_Less), int(Qt::Key_Comma));
    }

    /// No key may reach two enabled actions / shortcuts in the same window: Qt would report an
    /// "ambiguous shortcut overload" and run neither.
    void noAmbiguousShortcutsInWindow()
    {
        Fixture f;
        struct Binding
        {
            QString who;
            Qt::ShortcutContext context;
        };
        QMap<QString, QList<Binding>> byKey;
        auto addKeys = [&](const QList<QKeySequence>& keys, const QString& who,
                           Qt::ShortcutContext context, bool enabled) {
            if (!enabled)
                return;
            for (const QKeySequence& k : keys) {
                if (!k.isEmpty())
                    byKey[k.toString(QKeySequence::PortableText)].append({who, context});
            }
        };
        // The window's own actions (the registry) plus every action / shortcut of any child.
        QSet<QAction*> seen;
        for (QAction* a : f.window->actions()) {
            seen.insert(a);
            addKeys(a->shortcuts(), a->objectName() + QLatin1Char('/') + a->text(),
                a->shortcutContext(), a->isEnabled());
        }
        for (QAction* a : f.window->findChildren<QAction*>()) {
            if (seen.contains(a) || a->associatedObjects().isEmpty())
                continue;
            seen.insert(a);
            addKeys(a->shortcuts(), a->objectName() + QLatin1Char('/') + a->text(),
                a->shortcutContext(), a->isEnabled());
        }
        for (QShortcut* s : f.window->findChildren<QShortcut*>())
            addKeys(s->keys(), QStringLiteral("QShortcut %1").arg(s->objectName()), s->context(),
                s->isEnabled());
        QStringList clashes;
        for (auto it = byKey.cbegin(); it != byKey.cend(); ++it) {
            // Widget-context bindings only collide with window ones while that widget has focus;
            // count window / application ones against each other.
            QStringList global;
            for (const Binding& b : it.value()) {
                if (b.context == Qt::WindowShortcut || b.context == Qt::ApplicationShortcut)
                    global.append(b.who);
            }
            if (global.size() > 1)
                clashes.append(it.key() + QLatin1String(": ") + global.join(QLatin1String(", ")));
        }
        QVERIFY2(clashes.isEmpty(), qPrintable(clashes.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(TestEditKeys)
#include "test_editkeys.moc"
