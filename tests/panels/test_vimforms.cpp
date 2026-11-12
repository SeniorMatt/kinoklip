// SPDX-License-Identifier: GPL-3.0-or-later
// Vim keys in forms (spec section 83, key context "form"): the row cursor of the Inspector and of
// any panel of controls, stepping values with <C-a> / <C-x>, reset, copy and paste of a value,
// keyframes, choices, check boxes, title text (INSERT), folds, and the unbound keys that must
// reach the widget.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"
#include "ui/common/Resources.h"
#include "ui/common/VimFormHost.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimRowOps.h"
#include "ui/effects/EffectActions.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"
#include "ui/inspector/ValueField.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTest>
#include <QUndoStack>
#include <QVBoxLayout>

using namespace kk;
using namespace kk::test;

namespace {

const TrackRef kV1 {TrackType::Video, 0};

/// A window with a panel of rows, the Vim layer on and a form host; keys go to the focus.
struct Keys
{
    struct NullHost final : VimHost
    {
        bool trigger(const QString& id) override
        {
            triggered.append(id);
            return false;
        }
        int64_t playhead() const override { return 0; }
        void setPlayhead(int64_t) override { }
        void clearSelection() override { }
        void openCommandLine() override { }
        QStringList triggered;
    } nullHost;
    VimInput input {nullptr};
    VimFormHost form {nullptr};
    QStringList messages;

    Keys()
    {
        input.setHost(&nullHost);
        input.setContextHost(QStringLiteral("form"), &form);
        form.onMessage = [this](const QString& text) { messages.append(text); };
        input.setActive(true);
    }
    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            QWidget* focus = QApplication::focusWidget();
            if (c == QLatin1Char(' '))
                QTest::keyClick(focus, Qt::Key_Space);
            else if (c.isUpper())
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
    void esc() { QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape); }

    /// The ParamRow under the cursor, or nullptr.
    ParamRow* row() const { return dynamic_cast<ParamRow*>(form.current().ops); }
    /// Moves the cursor (with j from the top) to the parameter @p key.
    bool goTo(const QString& key)
    {
        QCoreApplication::processEvents(); // new pages get their layout
        QCoreApplication::processEvents();
        type(QStringLiteral("gg"));
        for (int i = 0; i < 200; ++i) {
            if (row() && row()->ref().key == key)
                return true;
            type(QStringLiteral("j"));
        }
        return false;
    }
};

/// An Inspector on a clip, shown, with the focus on the panel.
struct Inspector
{
    PanelFixture f;
    quint64 id = 0;
    InspectorPanel panel {f.ctx};
    Keys keys;
    Inspector(ClipKind kind = ClipKind::Media)
    {
        id = f.addClip(kV1, 0, 100, kind);
        panel.resize(460, 900);
        panel.show();
        f.selection.selectClip(id);
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QCoreApplication::processEvents();
        panel.setFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(), &panel);
        keys.form.setRoot(&panel);
    }
    static const ParamDescriptor& desc(const QString& key)
    {
        for (const QLatin1String g : {QLatin1String(groups::Transform),
                 QLatin1String(groups::Composite), QLatin1String(groups::Cropping)}) {
            if (const ParamDescriptor* d = EffectRegistry::instance().find(g)->param(key))
                return *d;
        }
        static const ParamDescriptor none;
        return none;
    }
    static ParamRef ref(quint64 clip, const QString& key)
    {
        return {clip, ParamScope::Transform, 0, key};
    }
    double valueOf(quint64 clip, const QString& key, int64_t frame = 0) const
    {
        return paramaccess::scalar(*f.clip(clip), ref(clip, key), desc(key), frame);
    }
    double value(const QString& key) const { return valueOf(id, key); }
    QVariant statik(quint64 clip, const QString& key) const
    {
        return paramaccess::staticValue(*f.clip(clip), ref(clip, key), desc(key));
    }
    QVariant statik(const QString& key) const { return statik(id, key); }
    const AnimatedValue* anim(const QString& key) const
    {
        return paramaccess::animated(*f.clip(id), ref(id, key));
    }
};

} // namespace

class TestVimForms : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
    }

    void panelIsAForm()
    {
        Inspector in;
        QCOMPARE(VimInput::contextOf(&in.panel), QStringLiteral("form"));
        QVERIFY(!in.keys.form.lines().isEmpty());
        // Rows of the Video tab: sections and parameters, top to bottom.
        QVERIFY(in.keys.form.lines().first().first().ops);
        QCOMPARE(in.keys.form.rowName(in.keys.form.lines().first().first()),
            QStringLiteral("Transform"));
    }

    void jkMoveTheRowCursor()
    {
        Inspector in;
        in.keys.type(QStringLiteral("gg"));
        const int first = in.keys.form.cursorLine();
        QCOMPARE(first, 0);
        in.keys.type(QStringLiteral("j"));
        QCOMPARE(in.keys.form.cursorLine(), 1);
        in.keys.type(QStringLiteral("3j"));
        QCOMPARE(in.keys.form.cursorLine(), 4);
        in.keys.type(QStringLiteral("k"));
        QCOMPARE(in.keys.form.cursorLine(), 3);
        in.keys.type(QStringLiteral("G"));
        QCOMPARE(in.keys.form.cursorLine(), int(in.keys.form.lines().size()) - 1);
        in.keys.type(QStringLiteral("2G"));
        QCOMPARE(in.keys.form.cursorLine(), 1);
        // The focus stays on the panel: j and k never typed into a field.
        QCOMPARE(QApplication::focusWidget(), &in.panel);
    }

    void ctrlAAndCtrlXStepValues()
    {
        Inspector in;
        QVERIFY(in.keys.goTo(keys::Opacity));
        QCOMPARE(in.value(keys::Opacity), 100.0); // the default
        in.keys.ctrl(Qt::Key_X);
        const double step = in.keys.row()->descriptor().step;
        QCOMPARE(in.value(keys::Opacity), 100.0 - step);
        in.keys.type(QStringLiteral("5"));
        in.keys.ctrl(Qt::Key_X);
        QCOMPARE(in.value(keys::Opacity), 100.0 - 6 * step);
        in.keys.ctrl(Qt::Key_A);
        in.keys.type(QStringLiteral("g"));
        in.keys.ctrl(Qt::Key_A);
        QCOMPARE(in.value(keys::Opacity), 100.0); // clamped at the maximum
        in.keys.type(QStringLiteral("zh"));
        QVERIFY(in.value(keys::Opacity) < 100.0);
        QVERIFY(in.value(keys::Opacity) > 100.0 - step);
    }

    void aStepIsOneUndoStep()
    {
        Inspector in;
        QVERIFY(in.keys.goTo(keys::Opacity));
        const int before = in.f.project.undoStack()->count();
        in.keys.type(QStringLiteral("7"));
        in.keys.ctrl(Qt::Key_X);
        QCOMPARE(in.f.project.undoStack()->count(), before + 1);
        in.f.project.undo();
        QCOMPARE(in.value(keys::Opacity), 100.0);
    }

    void positionAndRotationStepByTheirOwnSteps()
    {
        Inspector in;
        QVERIFY(in.keys.goTo(keys::PosX));
        const double posStep = in.keys.row()->descriptor().step;
        in.keys.type(QStringLiteral("10"));
        in.keys.ctrl(Qt::Key_A);
        QCOMPARE(in.value(keys::PosX), 10 * posStep);
        QVERIFY(in.keys.goTo(keys::Rotation));
        const double rotStep = in.keys.row()->descriptor().step;
        in.keys.ctrl(Qt::Key_A);
        in.keys.ctrl(Qt::Key_A);
        QCOMPARE(in.value(keys::Rotation), 2 * rotStep);
        QVERIFY(in.keys.goTo(keys::CropL));
        in.keys.type(QStringLiteral("3"));
        in.keys.ctrl(Qt::Key_A);
        QVERIFY(in.value(keys::CropL) > 0);
    }

    void hAndLStepASingleValueRow()
    {
        Inspector in;
        QVERIFY(in.keys.goTo(keys::Rotation));
        in.keys.type(QStringLiteral("l"));
        QCOMPARE(in.value(keys::Rotation), in.keys.row()->descriptor().step);
        in.keys.type(QStringLiteral("h"));
        QCOMPARE(in.value(keys::Rotation), 0.0);
    }

    void ddResetsAndDotRepeats()
    {
        Inspector in;
        QVERIFY(in.keys.goTo(keys::Rotation));
        in.keys.type(QStringLiteral("4"));
        in.keys.ctrl(Qt::Key_A);
        const double step = in.keys.row()->descriptor().step;
        QCOMPARE(in.value(keys::Rotation), 4 * step);
        in.keys.type(QStringLiteral("."));
        QCOMPARE(in.value(keys::Rotation), 8 * step);
        in.keys.type(QStringLiteral("dd"));
        QCOMPARE(in.value(keys::Rotation), 0.0);
    }

    void yyAndPCopyAValueToAnotherClip()
    {
        Inspector in;
        QVERIFY(in.keys.goTo(keys::Opacity));
        in.keys.type(QStringLiteral("20"));
        in.keys.ctrl(Qt::Key_X);
        const double v = in.value(keys::Opacity);
        in.keys.type(QStringLiteral("yy"));
        const quint64 other = in.f.addClip(kV1, 200, 100);
        in.f.selection.selectClip(other);
        QCoreApplication::processEvents();
        in.panel.setFocus(Qt::OtherFocusReason);
        QVERIFY(in.keys.goTo(keys::Opacity));
        QCOMPARE(in.valueOf(other, keys::Opacity), 100.0);
        in.keys.type(QStringLiteral("p"));
        QCOMPARE(in.valueOf(other, keys::Opacity), v);
        // A value of another type is not pasted into a number.
        QVERIFY(in.keys.goTo(keys::FlipH));
        in.keys.type(QStringLiteral("p"));
        QVERIFY(!in.statik(other, keys::FlipH).toBool());
    }

    void leaderKkTogglesAKeyframeOnTheRow()
    {
        Inspector in;
        QVERIFY(in.keys.goTo(keys::PosX));
        in.f.project.timeline()->setPlayhead(10);
        in.keys.type(QStringLiteral(" kk"));
        const AnimatedValue* a = in.anim(keys::PosX);
        QVERIFY(a && a->hasKeyAt(10));
        in.f.project.timeline()->setPlayhead(30);
        in.keys.type(QStringLiteral(" kk"));
        QVERIFY(in.anim(keys::PosX)->hasKeyAt(30));
        // ]k and [k move the playhead between the keyframes of this parameter.
        in.keys.type(QStringLiteral("[k"));
        QCOMPARE(in.f.project.timeline()->playhead(), int64_t(10));
        in.keys.type(QStringLiteral("]k"));
        QCOMPARE(in.f.project.timeline()->playhead(), int64_t(30));
        // Interpolation of the key under the playhead.
        in.keys.type(QStringLiteral(" kl"));
        QCOMPARE(paramaccess::keyInterp(*in.f.clip(in.id), in.keys.row()->ref(), 30).value(),
            Interp::Linear);
        in.keys.type(QStringLiteral(" kb"));
        QCOMPARE(paramaccess::keyInterp(*in.f.clip(in.id), in.keys.row()->ref(), 30).value(),
            Interp::EaseInOut);
        in.keys.type(QStringLiteral(" kh"));
        QCOMPARE(paramaccess::keyInterp(*in.f.clip(in.id), in.keys.row()->ref(), 30).value(),
            Interp::Hold);
        // Delete the key.
        in.keys.type(QStringLiteral(" kd"));
        QVERIFY(!in.anim(keys::PosX)->hasKeyAt(30));
        // A value stepped on a keyframed parameter sets a key at the playhead.
        in.f.project.timeline()->setPlayhead(10);
        in.keys.ctrl(Qt::Key_A);
        QVERIFY(in.anim(keys::PosX)->hasKeyAt(10));
    }

    void enterEditsANumberByTyping()
    {
        Inspector in;
        QVERIFY(in.keys.goTo(keys::Opacity));
        in.keys.type(QStringLiteral("i"));
        auto* field = in.panel.findChild<ValueField*>(QStringLiteral("value:") + keys::Opacity);
        QVERIFY(field);
        auto* editor = field->findChild<QLineEdit*>();
        QVERIFY(editor && editor->hasFocus());
        QCOMPARE(in.keys.input.displayMode(QApplication::focusWidget()), VimMachine::Mode::Insert);
        editor->selectAll();
        QTest::keyClicks(editor, QStringLiteral("42"));
        in.keys.enter();
        QCOMPARE(in.value(keys::Opacity), 42.0);
        // Back in NORMAL: the keys move the cursor again.
        QCOMPARE(in.keys.input.displayMode(QApplication::focusWidget()), VimMachine::Mode::Normal);
        in.keys.type(QStringLiteral("k"));
        QVERIFY(in.keys.row() == nullptr || in.keys.row()->ref().key != keys::Opacity);
    }

    void escLeavesATypedValue()
    {
        Inspector in;
        QVERIFY(in.keys.goTo(keys::Opacity));
        in.keys.enter();
        auto* field = in.panel.findChild<ValueField*>(QStringLiteral("value:") + keys::Opacity);
        QTest::keyClicks(field->findChild<QLineEdit*>(), QStringLiteral("7"));
        in.keys.esc();
        QCOMPARE(in.value(keys::Opacity), 100.0);
        QCOMPARE(in.keys.input.displayMode(QApplication::focusWidget()), VimMachine::Mode::Normal);
    }

    void checkboxAndChoiceRows()
    {
        Inspector in;
        QVERIFY(in.keys.goTo(keys::FlipH));
        QVERIFY(!in.statik(keys::FlipH).toBool());
        in.keys.enter();
        QVERIFY(in.statik(keys::FlipH).toBool());
        in.keys.type(QStringLiteral("x"));
        QVERIFY(!in.statik(keys::FlipH).toBool());
        in.keys.ctrl(Qt::Key_A);
        QVERIFY(in.statik(keys::FlipH).toBool());
        // A choice cycles with <C-a> and wraps.
        QVERIFY(in.keys.goTo(keys::CompositeMode));
        const auto& choices = in.keys.row()->descriptor().choices;
        QVERIFY(choices.size() > 2);
        in.keys.ctrl(Qt::Key_A);
        QCOMPARE(in.statik(keys::CompositeMode).toString(), choices[1]);
        in.keys.type(QStringLiteral("2"));
        in.keys.ctrl(Qt::Key_X);
        QCOMPARE(in.statik(keys::CompositeMode).toString(), choices[choices.size() - 1]);
    }

    void zcAndZoFoldASection()
    {
        Inspector in;
        in.keys.type(QStringLiteral("gg"));
        const int rows = int(in.keys.form.lines().size());
        // The cursor is on a section header: close it.
        QVERIFY(in.keys.form.current().ops);
        in.keys.type(QStringLiteral("zc"));
        QTRY_VERIFY(int(in.keys.form.lines().size()) < rows);
        in.keys.type(QStringLiteral("zo"));
        QTRY_COMPARE(int(in.keys.form.lines().size()), rows);
        in.keys.type(QStringLiteral("zM"));
        QTRY_VERIFY(int(in.keys.form.lines().size()) < rows);
        in.keys.type(QStringLiteral("zR"));
        QTRY_COMPARE(int(in.keys.form.lines().size()), rows);
        // zc on a parameter closes the section it is in and the cursor goes to its header.
        QVERIFY(in.keys.goTo(keys::PosX));
        in.keys.type(QStringLiteral("zc"));
        QVERIFY(in.keys.row() == nullptr);
        QVERIFY(in.keys.form.current().ops);
    }

    void bracketBracketChangesTheTab()
    {
        Inspector in;
        QCOMPARE(in.panel.tabs()->currentIndex(), 0);
        in.keys.type(QStringLiteral("]]"));
        QCOMPARE(in.panel.tabs()->currentIndex(), 1);
        in.keys.type(QStringLiteral("[["));
        QCOMPARE(in.panel.tabs()->currentIndex(), 0);
        in.keys.type(QStringLiteral("[["));
        QCOMPARE(in.panel.tabs()->currentIndex(), in.panel.tabs()->count() - 1);
    }

    void titleTextIsTypedInInsert()
    {
        PanelFixture f;
        const quint64 id
            = effectactions::addGeneratorClip(f.project, QStringLiteral("text_title"), 0);
        QVERIFY(id);
        InspectorPanel panel(f.ctx);
        panel.resize(460, 900);
        panel.show();
        f.selection.selectClip(id);
        panel.tabs()->setCurrentIndex(InspectorPanel::TitleTab);
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QCoreApplication::processEvents();
        panel.setFocus(Qt::OtherFocusReason);
        Keys keys;
        keys.form.setRoot(&panel);
        bool found = false;
        keys.type(QStringLiteral("gg"));
        for (int i = 0; i < 100 && !found; ++i) {
            found = keys.row() && keys.row()->ref().key == keys::Text;
            if (!found)
                keys.type(QStringLiteral("j"));
        }
        QVERIFY(found);
        keys.type(QStringLiteral("i"));
        auto* text = panel.findChild<QPlainTextEdit*>();
        QVERIFY(text && text->hasFocus());
        QCOMPARE(keys.input.displayMode(QApplication::focusWidget()), VimMachine::Mode::Insert);
        // Keys are text now, j included.
        QTest::keyClicks(text, QStringLiteral("jk Hi"));
        QTest::keyClick(text, Qt::Key_Return);
        QTest::keyClicks(text, QStringLiteral("there"));
        keys.esc();
        QTRY_COMPARE(QApplication::focusWidget(), &panel);
        QCOMPARE(f.clip(id)->title.staticValue(keys::Text).toString().right(11),
            QStringLiteral("jk Hi\nthere"));
        QCOMPARE(keys.input.displayMode(QApplication::focusWidget()), VimMachine::Mode::Normal);
    }

    void colourRowOpensThePicker()
    {
        PanelFixture f;
        const quint64 id
            = effectactions::addGeneratorClip(f.project, QStringLiteral("solid_color"), 0);
        QVERIFY(id);
        InspectorPanel panel(f.ctx);
        panel.resize(460, 900);
        int asked = 0;
        panel.setColorPicker([&asked](const QColor&, const QString&) {
            ++asked;
            return QColor(Qt::red);
        });
        panel.show();
        f.selection.selectClip(id);
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QCoreApplication::processEvents();
        panel.setFocus(Qt::OtherFocusReason);
        Keys keys;
        keys.form.setRoot(&panel);
        keys.type(QStringLiteral("gg"));
        for (int i = 0;
             i < 60 && !(keys.row() && keys.row()->descriptor().type == ParamType::Color); ++i)
            keys.type(QStringLiteral("j"));
        QVERIFY(keys.row() && keys.row()->descriptor().type == ParamType::Color);
        keys.enter();
        QCOMPARE(asked, 1);
        QCOMPARE(f.clip(id)->solidColor, QColor(Qt::red));
    }

    void unboundKeysReachTheWidget()
    {
        Inspector in;
        const VimMachine& m = in.keys.input.machine();
        // A printable key nobody bound is not swallowed in a form (it was, in step 5)...
        QVERIFY(!m.wouldConsume(Qt::Key_Q, Qt::ShiftModifier, QStringLiteral("Q")));
        QVERIFY(!m.wouldConsume(Qt::Key_W, Qt::NoModifier, QStringLiteral("w")));
        // ... while the bound ones and the counts are the layer's.
        QVERIFY(m.wouldConsume(Qt::Key_J, Qt::NoModifier, QStringLiteral("j")));
        QVERIFY(m.wouldConsume(Qt::Key_3, Qt::NoModifier, QStringLiteral("3")));
    }

    void plainControlsAreRowsToo()
    {
        QWidget panel;
        markVimForm(&panel);
        auto* layout = new QVBoxLayout(&panel);
        auto* box = new QCheckBox(QStringLiteral("Loop"));
        auto* combo = new QComboBox;
        combo->addItems({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        auto* spin = new QSpinBox;
        spin->setRange(0, 50);
        spin->setSingleStep(5);
        auto* dspin = new QDoubleSpinBox;
        dspin->setSingleStep(0.5);
        auto* line = new QLineEdit;
        auto* button = new QPushButton(QStringLiteral("Go"));
        for (QWidget* w : QList<QWidget*> {box, combo, spin, dspin, line, button})
            layout->addWidget(w);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        panel.setFocus(Qt::OtherFocusReason);
        Keys keys;
        keys.form.setRoot(&panel);
        QSignalSpy clicked(button, &QPushButton::clicked);
        keys.type(QStringLiteral("gg"));
        QCOMPARE(keys.form.current().anchor.data(), static_cast<QWidget*>(box));
        keys.enter();
        QVERIFY(box->isChecked());
        keys.type(QStringLiteral("x"));
        QVERIFY(!box->isChecked());
        keys.type(QStringLiteral("j"));
        keys.ctrl(Qt::Key_A);
        QCOMPARE(combo->currentIndex(), 1);
        keys.type(QStringLiteral("2"));
        keys.ctrl(Qt::Key_A);
        QCOMPARE(combo->currentIndex(), 0); // wrapped
        keys.type(QStringLiteral("j3"));
        keys.ctrl(Qt::Key_A);
        QCOMPARE(spin->value(), 15);
        keys.type(QStringLiteral("yyj"));
        keys.type(QStringLiteral("2"));
        keys.ctrl(Qt::Key_A);
        QCOMPARE(dspin->value(), 1.0);
        keys.type(QStringLiteral("jiabc"));
        QVERIFY(line->hasFocus());
        QCOMPARE(line->text(), QStringLiteral("abc"));
        keys.esc();
        QTRY_COMPARE(QApplication::focusWidget(), &panel);
        keys.type(QStringLiteral("j"));
        keys.enter();
        QCOMPARE(clicked.count(), 1);
        // Counts multiply with G / gg.
        keys.type(QStringLiteral("gg"));
        QCOMPARE(keys.form.cursorLine(), 0);
    }

    void aClickMovesTheCursor()
    {
        Inspector in;
        auto* field = in.panel.findChild<ValueField*>(QStringLiteral("value:") + keys::Rotation);
        QVERIFY(field);
        field->setFocus(Qt::MouseFocusReason);
        in.keys.type(QStringLiteral("l")); // acts on the clicked row
        QVERIFY(in.keys.row() && in.keys.row()->ref().key == keys::Rotation);
        QVERIFY(in.value(keys::Rotation) != 0.0);
    }
};

QTEST_MAIN(TestVimForms)
#include "test_vimforms.moc"
