// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the last Vim features (spec section 83): macros (q, @, @@, @:), `?` and the search
// direction, f F t T with ; and , (clips found by the first letter of their name), * and #,
// the numbered registers' dot-repeat, and a walk over the whole default key table (every key
// resolves in every context, every command is implemented).
#include "app/VimEx.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/VimGrammar.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimKeymap.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

#include <cstring>

using namespace kk;

namespace {

/// One track (V1): Alpha [0,40) Beta [50,90) Bravo [100,140) Delta [150,190) beta2 [200,240).
class NameHost final : public VimHost
{
public:
    int64_t frame = 0;
    QStringList log;
    QStringList ops;
    QHash<quint64, QString> names {{1, QStringLiteral("Alpha")}, {2, QStringLiteral("Beta")},
        {3, QStringLiteral("Bravo")}, {4, QStringLiteral("Delta")}, {5, QStringLiteral("beta2")}};

    bool trigger(const QString& id) override
    {
        log.append(id);
        return true;
    }
    int64_t playhead() const override { return frame; }
    void setPlayhead(int64_t f) override { frame = f; }
    void clearSelection() override { }
    void openCommandLine() override { log.append(QStringLiteral("colon")); }
    void openSearch() override { log.append(QStringLiteral("search")); }
    void runEx(const QString& line) override { log.append(QStringLiteral("ex ") + line); }
    QVector<VimClipSpan> clipSpans(int t) const override
    {
        if (t != 0)
            return {};
        return {{1, 0, 40}, {2, 50, 90}, {3, 100, 140}, {4, 150, 190}, {5, 200, 240}};
    }
    QString clipName(quint64 id) const override { return names.value(id); }
    int64_t timelineEnd() const override { return 240; }
    bool operate(const VimOperation& o) override
    {
        ops.append(QStringLiteral("%1%2 %3-%4")
                       .arg(QLatin1String(o.op == VimOp::Delete ? "d" : o.op == VimOp::Yank ? "y" : "c"),
                           o.reg.isNull() ? QString() : QStringLiteral("[%1]").arg(o.reg))
                       .arg(o.range.start)
                       .arg(o.range.end));
        return true;
    }
    bool pasteRegister(QChar reg, int count, bool insert) override
    {
        log.append(QStringLiteral("paste[%1] %2 %3")
                       .arg(reg.isNull() ? QStringLiteral("-") : QString(reg))
                       .arg(count)
                       .arg(insert ? QStringLiteral("insert") : QStringLiteral("over")));
        return true;
    }
    void showMessage(const QString& text) override { log.append(QStringLiteral("msg ") + text); }
    bool searchStep(bool forward, int count) override
    {
        log.append(QStringLiteral("step %1 %2").arg(forward ? 1 : 0).arg(count));
        return true;
    }
    bool searchFor(const QString& pattern, bool forward) override
    {
        log.append(QStringLiteral("find %1 %2").arg(pattern).arg(forward ? 1 : 0));
        return pattern != QLatin1String("Nothing");
    }
};

struct Rig
{
    VimMachine m;
    NameHost host;
    Rig() { m.setHost(&host); }
    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            const QString t(c);
            QVERIFY2(m.handleKey(c.unicode(), {}, t), qPrintable(keys));
        }
    }
    void esc() { QVERIFY(m.handleKey(Qt::Key_Escape, {}, QString())); }
    QString go(const QString& keys, int64_t from = 0)
    {
        host.frame = from;
        type(keys);
        return QString::number(host.frame);
    }
    QString op(const QString& keys, int64_t from = 0)
    {
        host.frame = from;
        host.ops.clear();
        type(keys);
        return host.ops.join(QLatin1Char('|'));
    }
};

QString sampleOf(const QString& token)
{
    if (!vimkeys::isClassToken(token))
        return token;
    return QString(token.at(1)); // the first character of the class
}

QStringList chainOf(const QString& context)
{
    if (context == QLatin1String("viewer"))
        return {QStringLiteral("global"), QStringLiteral("timeline"), QStringLiteral("viewer")};
    if (context == QLatin1String("timeline"))
        return {QStringLiteral("global"), QStringLiteral("timeline")};
    if (context == QLatin1String("global"))
        return {QStringLiteral("global")};
    return {QStringLiteral("global"), context};
}

} // namespace

class TestVimMacros : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimmacros"));
        initResources();
    }
    void init() { QSettings().clear(); }

    // ---- f F t T ; , ----

    void findMovesToClipsByFirstLetter()
    {
        Rig r;
        QCOMPARE(r.go(QStringLiteral("fb")), QStringLiteral("50")); // Beta (any case)
        QCOMPARE(r.go(QStringLiteral("fB")), QStringLiteral("50"));
        QCOMPARE(r.go(QStringLiteral("2fb")), QStringLiteral("100")); // Bravo
        QCOMPARE(r.go(QStringLiteral("3fb")), QStringLiteral("200")); // beta2
        QCOMPARE(r.go(QStringLiteral("4fb")), QStringLiteral("0")); // no fourth: stays
        QCOMPARE(r.go(QStringLiteral("fz")), QStringLiteral("0")); // no such clip
        QCOMPARE(r.go(QStringLiteral("fa"), 0), QStringLiteral("0")); // Alpha is not after 0
        QCOMPARE(r.go(QStringLiteral("fd")), QStringLiteral("150"));
        QCOMPARE(r.go(QStringLiteral("Fb"), 200), QStringLiteral("100"));
        QCOMPARE(r.go(QStringLiteral("2Fb"), 200), QStringLiteral("50"));
        QCOMPARE(r.go(QStringLiteral("Fa"), 200), QStringLiteral("0"));
        QCOMPARE(r.go(QStringLiteral("tb")), QStringLiteral("49")); // the frame before Beta
        QCOMPARE(r.go(QStringLiteral("tb"), 49), QStringLiteral("49")); // already there
        QCOMPARE(r.go(QStringLiteral("Tb"), 200), QStringLiteral("140")); // just after Bravo
        QCOMPARE(r.go(QStringLiteral("Tb"), 140), QStringLiteral("140"));
    }

    void semicolonAndCommaRepeat()
    {
        Rig r;
        QCOMPARE(r.go(QStringLiteral("fb")), QStringLiteral("50"));
        r.type(QStringLiteral(";"));
        QCOMPARE(QString::number(r.host.frame), QStringLiteral("100"));
        r.type(QStringLiteral(";"));
        QCOMPARE(QString::number(r.host.frame), QStringLiteral("200"));
        r.type(QStringLiteral(","));
        QCOMPARE(QString::number(r.host.frame), QStringLiteral("100")); // the other way
        r.type(QStringLiteral("2;"));
        QCOMPARE(QString::number(r.host.frame), QStringLiteral("100")); // only one more clip
        // t does not stay where it is when repeated
        QCOMPARE(r.go(QStringLiteral("tb")), QStringLiteral("49"));
        r.type(QStringLiteral(";"));
        QCOMPARE(QString::number(r.host.frame), QStringLiteral("99"));
        r.type(QStringLiteral(","));
        QCOMPARE(QString::number(r.host.frame), QStringLiteral("90")); // T: after Beta
        // without a find first, nothing happens
        Rig fresh;
        QCOMPARE(fresh.go(QStringLiteral(";"), 7), QStringLiteral("7"));
    }

    void findWorksWithOperators()
    {
        Rig r;
        QCOMPARE(r.op(QStringLiteral("dfb")), QStringLiteral("d 0-50")); // up to Beta
        QCOMPARE(r.op(QStringLiteral("dtb")), QStringLiteral("d 0-50"));
        QCOMPARE(r.op(QStringLiteral("2yfb")), QStringLiteral("y 0-100"));
        QCOMPARE(r.op(QStringLiteral("dFb"), 200), QStringLiteral("d 100-200"));
        QCOMPARE(r.op(QStringLiteral("dfz")), QString()); // nothing found: no operation
        QVERIFY(!r.m.operatorPending());
        // d; after a find
        r.go(QStringLiteral("fb"));
        QCOMPARE(r.op(QStringLiteral("d;"), 50), QStringLiteral("d 50-100"));
        // a find followed by Esc leaves nothing pending
        r.type(QStringLiteral("f"));
        r.esc();
        QVERIFY(r.m.pending().isEmpty());
    }

    // ---- Macros ----

    void recordAndPlay()
    {
        Rig r;
        r.host.frame = 0;
        r.type(QStringLiteral("qa"));
        QCOMPARE(r.m.recordingRegister(), QLatin1Char('a'));
        QVERIFY(r.m.statusText().contains(QStringLiteral("recording @a")));
        r.type(QStringLiteral("w"));
        r.type(QStringLiteral("q"));
        QVERIFY(r.m.recordingRegister().isNull());
        QCOMPARE(r.m.macro(QLatin1Char('a')), (QStringList {QStringLiteral("w")}));
        QCOMPARE(r.host.frame, int64_t(50)); // recording runs the keys too
        r.type(QStringLiteral("@a"));
        QCOMPARE(r.host.frame, int64_t(100));
        r.type(QStringLiteral("2@a"));
        QCOMPARE(r.host.frame, int64_t(200));
        r.host.frame = 0;
        r.type(QStringLiteral("@a@@"));
        QCOMPARE(r.host.frame, int64_t(100)); // @@ is the last macro again
    }

    void countsAndOperatorsInMacros()
    {
        Rig r;
        r.host.ops.clear();
        r.type(QStringLiteral("qb2dwq"));
        QCOMPARE(r.m.macro(QLatin1Char('b')),
            (QStringList {QStringLiteral("2"), QStringLiteral("d"), QStringLiteral("w")}));
        QCOMPARE(r.host.ops.size(), 1);
        r.host.ops.clear();
        r.host.frame = 0;
        r.type(QStringLiteral("@b"));
        QCOMPARE(r.host.ops.size(), 1);
        QCOMPARE(r.host.ops.first(), QStringLiteral("d 0-100")); // 2w from 0 is frame 100
        // `.` repeats the last change that ran inside the macro
        r.host.ops.clear();
        r.host.frame = 0;
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.ops.size(), 1);
    }

    void capitalLetterAppends()
    {
        Rig r;
        r.type(QStringLiteral("qawq"));
        r.type(QStringLiteral("qAbq"));
        QCOMPARE(r.m.macro(QLatin1Char('a')),
            (QStringList {QStringLiteral("w"), QStringLiteral("b")}));
        r.type(QStringLiteral("qaq")); // a lower-case letter starts over
        QVERIFY(r.m.macro(QLatin1Char('a')).isEmpty());
    }

    void macroStopsAtTheFirstFailure()
    {
        Rig r;
        r.type(QStringLiteral("qafbq")); // to the next clip starting with b
        QCOMPARE(r.host.frame, int64_t(50));
        r.host.frame = 0;
        r.type(QStringLiteral("100@a")); // 100 times, but only three b clips follow
        QCOMPARE(r.host.frame, int64_t(200));
        QVERIFY(r.m.pending().isEmpty());
        QVERIFY(!r.m.playingMacro());
    }

    void recursiveMacroEndsWhereItFails()
    {
        Rig r;
        r.type(QStringLiteral("qaq")); // clear a
        r.type(QStringLiteral("qafb@aq"));
        r.host.frame = 0;
        r.type(QStringLiteral("@a"));
        QCOMPARE(r.host.frame, int64_t(200));
        QVERIFY(!r.m.playingMacro());
        // a macro that never fails stops at the depth limit instead of hanging
        r.type(QStringLiteral("qcq"));
        r.type(QStringLiteral("qc0@cq"));
        r.host.frame = 5;
        r.type(QStringLiteral("@c"));
        QVERIFY(!r.m.playingMacro());
    }

    void emptyMacroSaysSo()
    {
        Rig r;
        r.type(QStringLiteral("@z"));
        QVERIFY(r.host.log.last().contains(QStringLiteral("empty")));
        Rig fresh;
        fresh.type(QStringLiteral("@@"));
        QVERIFY(fresh.host.log.last().contains(QStringLiteral("No macro")));
    }

    void commandLinesAreRecorded()
    {
        Rig r;
        r.type(QStringLiteral("qa:"));
        QCOMPARE(r.host.log.last(), QStringLiteral("colon"));
        r.m.lineSubmitted(QLatin1Char(':'), QStringLiteral("marker add"));
        r.type(QStringLiteral("q"));
        r.host.log.clear();
        r.type(QStringLiteral("@a"));
        // the line runs; the `:` key does not open the line again
        QCOMPARE(r.host.log, (QStringList {QStringLiteral("ex marker add")}));
        r.host.log.clear();
        r.type(QStringLiteral("2@:"));
        QCOMPARE(r.host.log,
            (QStringList {QStringLiteral("ex marker add"), QStringLiteral("ex marker add")}));
    }

    void searchLinesAreRecorded()
    {
        Rig r;
        r.type(QStringLiteral("qa/"));
        r.m.lineSubmitted(QLatin1Char('/'), QStringLiteral("Delta"));
        r.type(QStringLiteral("q"));
        r.host.log.clear();
        r.type(QStringLiteral("@a"));
        QCOMPARE(r.host.log, (QStringList {QStringLiteral("find Delta 1")}));
        // a search that finds nothing ends the macro
        Rig n;
        n.type(QStringLiteral("qb/"));
        n.m.lineSubmitted(QLatin1Char('/'), QStringLiteral("Nothing"));
        n.type(QStringLiteral("w"));
        n.type(QStringLiteral("q"));
        n.host.frame = 0;
        n.type(QStringLiteral("@b"));
        QCOMPARE(n.host.frame, int64_t(0)); // the w after the failed search did not run
    }

    void recordingSurvivesEscAndVisual()
    {
        Rig r;
        r.type(QStringLiteral("qv"));
        r.type(QStringLiteral("vl"));
        r.esc();
        r.type(QStringLiteral("q"));
        QCOMPARE(r.m.macro(QLatin1Char('v')),
            (QStringList {QStringLiteral("v"), QStringLiteral("l"), QStringLiteral("<Esc>")}));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Normal);
        r.type(QStringLiteral("@v"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Normal);
    }

    void qColonStillOpensHistory()
    {
        Rig r;
        bool opened = false;
        r.m.setCommandHandler([&](const QString& name, int, bool, const QString&) {
            opened = opened || name == QLatin1String("picker_history");
            return true;
        });
        r.type(QStringLiteral("q:"));
        QVERIFY(opened);
        QVERIFY(r.m.recordingRegister().isNull());
    }

    // ---- Search ----

    void questionMarkSearchesBackwards()
    {
        Rig r;
        QVERIFY(r.m.searchForward());
        r.type(QStringLiteral("?"));
        QCOMPARE(r.host.log.last(), QStringLiteral("search"));
        QVERIFY(!r.m.searchForward());
        r.m.lineSubmitted(QLatin1Char('?'), QStringLiteral("x"));
        r.type(QStringLiteral("n"));
        QCOMPARE(r.host.log.last(), QStringLiteral("step 0 1")); // n follows ?
        r.type(QStringLiteral("N"));
        QCOMPARE(r.host.log.last(), QStringLiteral("step 1 1"));
        r.type(QStringLiteral("3n"));
        QCOMPARE(r.host.log.last(), QStringLiteral("step 0 3"));
        r.type(QStringLiteral("/"));
        QVERIFY(r.m.searchForward());
        r.type(QStringLiteral("n"));
        QCOMPARE(r.host.log.last(), QStringLiteral("step 1 1"));
    }

    void starAndHashSearchTheNameUnderTheCursor()
    {
        Rig r;
        r.host.frame = 55; // inside Beta
        r.type(QStringLiteral("*"));
        QCOMPARE(r.host.log.last(), QStringLiteral("find Beta 1"));
        r.type(QStringLiteral("#"));
        QCOMPARE(r.host.log.last(), QStringLiteral("find Beta 0"));
        QVERIFY(!r.m.searchForward());
        r.type(QStringLiteral("n"));
        QCOMPARE(r.host.log.last(), QStringLiteral("step 0 1"));
        r.host.frame = 45; // in the gap: no name
        r.type(QStringLiteral("*"));
        QVERIFY(r.host.log.last().startsWith(QStringLiteral("msg")));
    }

    // ---- Registers ----

    void numberedRegisterStepsWithDot()
    {
        Rig r;
        r.type(QStringLiteral("\"1p"));
        QCOMPARE(r.host.log.last(), QStringLiteral("paste[1] 1 insert"));
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.log.last(), QStringLiteral("paste[2] 1 insert"));
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.log.last(), QStringLiteral("paste[3] 1 insert"));
        r.type(QStringLiteral("\"ap"));
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.log.last(), QStringLiteral("paste[a] 1 insert")); // letters stay
        r.type(QStringLiteral("\"-p\"\"p"));
        QVERIFY(r.host.log.last().startsWith(QStringLiteral("paste[-]")));
    }

    // ---- The key table ----

    void everyKeyResolvesInItsContexts()
    {
        VimKeymap map;
        QString error;
        QVERIFY2(map.loadDefaults(&error), qPrintable(error));
        int checked = 0;
        for (const QString& context : VimKeymap::contexts()) {
            const QStringList chain = chainOf(context);
            for (const VimBinding& b : map.bindings()) {
                if (!chain.contains(b.context) || b.isGroup())
                    continue;
                QStringList typed;
                for (const QString& t : b.tokens)
                    typed.append(sampleOf(t));
                const VimTrie& trie = map.trie(context, b.mode);
                const VimMatch match = trie.match(typed);
                const bool found = match.kind == VimMatch::Kind::Exact
                    || match.kind == VimMatch::Kind::ExactAndPartial;
                QVERIFY2(found,
                    qPrintable(QStringLiteral("%1 %2 %3 does not resolve")
                                   .arg(context, QString(b.mode), b.keys)));
                // a key of the same name in a more specific context may take it over, never
                // a different key
                QCOMPARE(match.binding->tokens.size(), b.tokens.size());
                ++checked;
            }
        }
        QVERIFY(checked > 800);
    }

    void everyGroupHasMembers()
    {
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        for (const QString& context : VimKeymap::contexts()) {
            for (const QChar mode : {QLatin1Char('n'), QLatin1Char('v'), QLatin1Char('o')}) {
                const VimTrie& trie = map.trie(context, mode);
                for (const VimBinding& b : map.bindings()) {
                    if (!b.isGroup() || b.mode != mode || !chainOf(context).contains(b.context))
                        continue;
                    QVERIFY2(!trie.continuations(b.tokens).isEmpty(),
                        qPrintable(QStringLiteral("empty group %1 %2 %3")
                                       .arg(context, QString(mode), b.keys)));
                }
            }
        }
    }

    void everyCommandIsImplementedSomewhere()
    {
        // Each vim: command of the table must appear as a string in the code that runs it
        // (the machine, the grammar, a host, the window); VimKeymap.cpp only lists the names.
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        QSet<QString> used;
        for (const VimBinding& b : map.bindings()) {
            for (const QString& item : b.run) {
                const QString n = VimKeymap::normaliseRunItem(item);
                if (n.startsWith(QLatin1String("vim:")))
                    used.insert(n.mid(4));
                else if (n.startsWith(QLatin1String("ex:"))) {
                    const vimex::Parsed p = vimex::parse(n.mid(3));
                    QVERIFY2(!p.name.isEmpty() || n.mid(3).contains(QLatin1Char(' ')),
                        qPrintable(n));
                }
            }
        }
        QString code;
        QDir root(QStringLiteral(KK_TEST_DATA_DIR));
        root.cd(QStringLiteral("../../src"));
        QVERIFY(root.exists());
        QDirIterator it(root.absolutePath(), {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
            QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (path.endsWith(QLatin1String("VimKeymap.cpp")))
                continue;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly) && QFileInfo(path).fileName().contains(QLatin1String("Vim")))
                code += QString::fromUtf8(f.readAll());
        }
        QVERIFY(code.size() > 100000);
        QStringList missing;
        for (const QString& name : std::as_const(used)) {
            // The hosts of panels get the name without their prefix (`form_down` -> "down").
            QString bare = name;
            for (const char* prefix : {"form_", "node_", "kf_", "mix_", "picker_", "focus_"}) {
                if (name.startsWith(QLatin1String(prefix)))
                    bare = name.mid(int(strlen(prefix)));
            }
            // and a family such as `key_*` / `interp_*` is matched by its start
            const QString family = bare.left(bare.indexOf(QLatin1Char('_')) + 1);
            if (!code.contains(QLatin1Char('"') + name + QLatin1Char('"'))
                && !code.contains(QLatin1Char('"') + bare + QLatin1Char('"'))
                && !(family.size() > 1 && code.contains(QLatin1Char('"') + family + QLatin1Char('"'))))
                missing.append(name);
        }
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QLatin1Char(' '))));
    }

    void whichKeyListsEveryContext()
    {
        // In every context, every group of keys (g, z, Space, Space w, Ctrl+W ...) shows its
        // continuations once the timeout runs out, and the list holds what the table says.
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        int groups = 0;
        for (const QString& context : VimKeymap::contexts()) {
            for (const VimBinding& g : map.bindings()) {
                if (!g.isGroup() || g.mode != QLatin1Char('n')
                    || !chainOf(context).contains(g.context))
                    continue;
                VimMachine m;
                NameHost host;
                m.setHost(&host);
                m.setKeymap(&map);
                m.setContextProvider([&] { return context; });
                m.setTimeoutMs(100000);
                for (const QString& t : g.tokens) {
                    bool ok = false;
                    if (t == QLatin1String("<Space>"))
                        ok = m.handleKey(Qt::Key_Space, {}, QStringLiteral(" "));
                    else if (t.size() == 5 && t.startsWith(QLatin1String("<C-")))
                        ok = m.handleKey(Qt::Key_A + (t.at(3).unicode() - 'a'), Qt::ControlModifier,
                            QString());
                    else if (t.size() == 1)
                        ok = m.handleKey(t.at(0).unicode(), {}, t);
                    QVERIFY2(ok, qPrintable(context + " " + g.keys));
                }
                m.timeoutNow();
                QVERIFY2(m.whichKeyShown(), qPrintable(context + " " + g.keys));
                QVERIFY2(!m.whichKeyEntries().isEmpty(), qPrintable(context + " " + g.keys));
                for (const VimContinuation& c : m.whichKeyEntries())
                    QVERIFY2(!c.desc.isEmpty(), qPrintable(context + " " + g.keys + " " + c.token));
                m.cancelPending();
                QVERIFY(!m.whichKeyShown());
                ++groups;
            }
        }
        QVERIFY(groups > 40);
    }

    void noDefaultIsShadowedByAShorterOne()
    {
        // The checks of the Vim keys tab report no error for the defaults.
        ActionRegistry actions;
        registerStandardActions(actions);
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        for (const VimKeymap::Issue& issue : map.issues(&actions))
            QVERIFY2(issue.severity != VimKeymap::Issue::Severity::Error,
                qPrintable(issue.keys + ": " + issue.text));
    }
};

QTEST_MAIN(TestVimMacros)
#include "test_vimmacros.moc"
