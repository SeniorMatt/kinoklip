// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the Vim key table (spec section 83): key strings and tokens, the trie with exact /
// partial / ambiguous matches, the default table (every command exists, nothing doubled, the
// Ctrl chords it defines are the documented ones), the user's changes and their persistence,
// import and export, the vimrc file and :nmap, and the checks of the Vim keys tab.
#include "ui/common/ActionRegistry.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimKeymap.h"
#include "ui/common/VimKeys.h"
#include "ui/common/VimOptions.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::vimkeys;

namespace {

QStringList tokens(const QString& spec, const QString& leader = QStringLiteral("<Space>"))
{
    return parseKeys(spec, leader);
}

VimBinding makeBinding(const QString& keys, const QString& run = QStringLiteral("action:edit.undo"))
{
    VimBinding b;
    b.context = QStringLiteral("global");
    b.keys = keys;
    b.tokens = parseKeys(keys);
    b.run = {run};
    return b;
}

KeyInfo key(int qtKey, Qt::KeyboardModifiers mods, const QString& text, quint32 scan = 0)
{
    KeyInfo k;
    k.key = qtKey;
    k.modifiers = mods;
    k.text = text;
    k.nativeScanCode = scan;
    return k;
}

} // namespace

class TestVimKeymap : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimkeymap"));
        QSettings().clear();
        initResources();
    }
    void init() { QSettings().clear(); }

    // ---- Key strings ----

    void keyStringsParseToTokens()
    {
        QCOMPARE(tokens(QStringLiteral("gg")), (QStringList {"g", "g"}));
        QCOMPARE(tokens(QStringLiteral("<leader>ff")), (QStringList {"<Space>", "f", "f"}));
        QCOMPARE(tokens(QStringLiteral("<leader>ff"), QStringLiteral(",")),
            (QStringList {",", "f", "f"}));
        QCOMPARE(tokens(QStringLiteral("<C-h>")), QStringList {"<C-h>"});
        QCOMPARE(tokens(QStringLiteral("<C-H>")), QStringList {"<C-h>"}); // letters are lower case
        QCOMPARE(tokens(QStringLiteral("<c-w>h")), (QStringList {"<C-w>", "h"}));
        QCOMPARE(tokens(QStringLiteral("<A-x>")), QStringList {"<A-x>"});
        QCOMPARE(tokens(QStringLiteral("<C-S-z>")), QStringList {"<C-S-z>"});
        QCOMPARE(tokens(QStringLiteral("<CR><Esc><Tab><BS><Del>")),
            (QStringList {"<CR>", "<Esc>", "<Tab>", "<BS>", "<Del>"}));
        QCOMPARE(tokens(QStringLiteral("<Enter>")), QStringList {"<CR>"});
        QCOMPARE(tokens(QStringLiteral("<F5><Left><PageUp>")),
            (QStringList {"<F5>", "<Left>", "<PageUp>"}));
        QCOMPARE(tokens(QStringLiteral("<Space>")), QStringList {"<Space>"});
        QCOMPARE(tokens(QStringLiteral("G$0{}")), (QStringList {"G", "$", "0", "{", "}"}));
        // < and > as keys: literal when they do not name a key.
        QCOMPARE(tokens(QStringLiteral("<")), QStringList {"<"});
        QCOMPARE(tokens(QStringLiteral(">")), QStringList {">"});
        QCOMPARE(tokens(QStringLiteral("<<")), (QStringList {"<", "<"}));
        QCOMPARE(tokens(QStringLiteral("<lt>")), QStringList {"<"});
        QCOMPARE(tokens(QStringLiteral("<gt>")), QStringList {">"});
        QCOMPARE(tokens(QStringLiteral("<foo>")), (QStringList {"<", "f", "o", "o", ">"}));
        // Character classes.
        QCOMPARE(tokens(QStringLiteral("m{a-z}")), (QStringList {"m", "{a-z}"}));
        QCOMPARE(tokens(QStringLiteral("'{a-zA-Z}")), (QStringList {"'", "{a-zA-Z}"}));
        // Errors.
        QString error;
        QVERIFY(parseKeys(QStringLiteral("a b"), QStringLiteral("<Space>"), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(parseKeys(QString(), QStringLiteral("<Space>"), &error).isEmpty());
        QCOMPARE(
            joinKeys(tokens(QStringLiteral("<leader>f<C-h>"))), QStringLiteral("<Space>f<C-h>"));
        QCOMPARE(joinKeys(QStringList {"<"}), QStringLiteral("<lt>"));
    }

    void classTokens()
    {
        QVERIFY(classMatches(QStringLiteral("{a-z}"), QStringLiteral("q")));
        QVERIFY(!classMatches(QStringLiteral("{a-z}"), QStringLiteral("Q")));
        QVERIFY(classMatches(QStringLiteral("{a-zA-Z}"), QStringLiteral("Q")));
        QVERIFY(classMatches(QStringLiteral("{0-9}"), QStringLiteral("5")));
        QVERIFY(!classMatches(QStringLiteral("{a-z}"), QStringLiteral("ab")));
    }

    // ---- Events to tokens ----

    void typedKeysBecomeTokens()
    {
        KeyTranslator t;
        QCOMPARE(t.token(key('l', {}, QStringLiteral("l"))), QStringLiteral("l"));
        QCOMPARE(
            t.token(key(Qt::Key_G, Qt::ShiftModifier, QStringLiteral("G"))), QStringLiteral("G"));
        QCOMPARE(t.token(key(Qt::Key_BraceLeft, Qt::ShiftModifier, QStringLiteral("{"))),
            QStringLiteral("{"));
        QCOMPARE(t.token(key(Qt::Key_Space, {}, QStringLiteral(" "))), QStringLiteral("<Space>"));
        QCOMPARE(t.token(key(Qt::Key_Escape, {}, QString())), QStringLiteral("<Esc>"));
        QCOMPARE(t.token(key(Qt::Key_Return, {}, QStringLiteral("\r"))), QStringLiteral("<CR>"));
        QCOMPARE(t.token(key(Qt::Key_Enter, {}, QStringLiteral("\r"))), QStringLiteral("<CR>"));
        QCOMPARE(t.token(key(Qt::Key_Left, {}, QString())), QStringLiteral("<Left>"));
        QCOMPARE(t.token(key(Qt::Key_F5, {}, QString())), QStringLiteral("<F5>"));
        QCOMPARE(
            t.token(key(Qt::Key_Backtab, Qt::ShiftModifier, QString())), QStringLiteral("<S-Tab>"));
        QCOMPARE(t.token(key(Qt::Key_R, Qt::ControlModifier, QStringLiteral("\x12"))),
            QStringLiteral("<C-r>"));
        QCOMPARE(t.token(key(
                     Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("\x1a"))),
            QStringLiteral("<C-S-z>"));
        QCOMPARE(t.token(key(Qt::Key_BracketLeft, Qt::ControlModifier, QStringLiteral("\x1b"))),
            QStringLiteral("<C-[>"));
        QCOMPARE(
            t.token(key(Qt::Key_X, Qt::AltModifier, QStringLiteral("x"))), QStringLiteral("<A-x>"));
        // Keys the layer has no name for.
        QVERIFY(t.token(key(Qt::Key_Shift, Qt::ShiftModifier, QString())).isEmpty());
        QVERIFY(t.token(key(Qt::Key_Control, Qt::ControlModifier, QString())).isEmpty());
        QVERIFY(t.token(key(Qt::Key_A, Qt::MetaModifier, QStringLiteral("a"))).isEmpty());
    }

    void evdevTable()
    {
        QCOMPARE(usCharForEvdev(16, false), QStringLiteral("q"));
        QCOMPARE(usCharForEvdev(16, true), QStringLiteral("Q"));
        QCOMPARE(usCharForEvdev(30, false), QStringLiteral("a"));
        QCOMPARE(usCharForEvdev(50, false), QStringLiteral("m"));
        QCOMPARE(usCharForEvdev(26, true), QStringLiteral("{"));
        QCOMPARE(usCharForEvdev(27, true), QStringLiteral("}"));
        QCOMPARE(usCharForEvdev(5, true), QStringLiteral("$"));
        QCOMPARE(usCharForEvdev(11, false), QStringLiteral("0"));
        QCOMPARE(usCharForEvdev(39, true), QStringLiteral(":"));
        QCOMPARE(usCharForEvdev(40, false), QStringLiteral("'"));
        QCOMPARE(usCharForEvdev(41, false), QStringLiteral("`"));
        QCOMPARE(usCharForEvdev(53, true), QStringLiteral("?"));
        QVERIFY(usCharForEvdev(1, false).isEmpty()); // Esc
        QVERIFY(usCharForEvdev(57, false).isEmpty()); // space
        QCOMPARE(evdevFromNative(0), -1);
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        QCOMPARE(evdevFromNative(24), 16); // xkb code of Q
#endif
    }

    // ---- The trie ----

    void trieMatches()
    {
        const VimBinding g = makeBinding(QStringLiteral("g"), QStringLiteral("action:a.one"));
        const VimBinding gg = makeBinding(QStringLiteral("gg"), QStringLiteral("action:a.two"));
        const VimBinding gx = makeBinding(QStringLiteral("gx"), QStringLiteral("action:a.three"));
        const VimBinding h = makeBinding(QStringLiteral("h"), QStringLiteral("action:a.four"));
        const VimBinding mark
            = makeBinding(QStringLiteral("m{a-z}"), QStringLiteral("vim:set_mark"));
        VimTrie t;
        for (const VimBinding* b : {&g, &gg, &gx, &h, &mark})
            t.insert(b);
        QCOMPARE(t.match({"h"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(t.match({"g"}).kind, VimMatch::Kind::ExactAndPartial); // g is also the start of gg
        QCOMPARE(t.match({"g"}).binding, &g);
        QCOMPARE(t.match({"g", "g"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(t.match({"g", "g"}).binding, &gg);
        QCOMPARE(t.match({"g", "z"}).kind, VimMatch::Kind::None);
        QCOMPARE(t.match({"z"}).kind, VimMatch::Kind::None);
        QCOMPARE(t.match({}).kind, VimMatch::Kind::None);
        QCOMPARE(t.match({"m"}).kind, VimMatch::Kind::Partial);
        const VimMatch m = t.match({"m", "q"});
        QCOMPARE(m.kind, VimMatch::Kind::Exact);
        QCOMPARE(m.arg, QStringLiteral("q"));
        QCOMPARE(t.match({"m", "Q"}).kind, VimMatch::Kind::None);
        QVERIFY(t.startsWith(QStringLiteral("m")));
        QVERIFY(!t.startsWith(QStringLiteral("x")));
        const auto next = t.continuations({"g"});
        QCOMPARE(next.size(), qsizetype(2));
        QCOMPARE(next[0].token, QStringLiteral("g"));
        QCOMPARE(next[1].token, QStringLiteral("x"));
        QVERIFY(t.continuations({"z"}).isEmpty());
    }

    void laterInsertReplaces()
    {
        const VimBinding a = makeBinding(QStringLiteral("x"), QStringLiteral("action:a.one"));
        const VimBinding b = makeBinding(QStringLiteral("x"), QStringLiteral("action:a.two"));
        VimTrie t;
        t.insert(&a);
        t.insert(&b);
        QCOMPARE(t.match({"x"}).binding, &b);
    }

    void groupsNameAPrefixWithoutRunningAnything()
    {
        VimBinding group = makeBinding(QStringLiteral("<Space>s"));
        group.type = QStringLiteral("group");
        group.desc = QStringLiteral("source edits");
        const VimBinding leaf
            = makeBinding(QStringLiteral("<Space>si"), QStringLiteral("action:edit.insert"));
        VimTrie t;
        t.insert(&group);
        t.insert(&leaf);
        QCOMPARE(t.match({"<Space>", "s"}).kind, VimMatch::Kind::Partial);
        const auto c = t.continuations({"<Space>"});
        QCOMPARE(c.size(), qsizetype(1));
        QCOMPARE(c[0].desc, QStringLiteral("source edits"));
        QVERIFY(c[0].more);
        QVERIFY(!c[0].binding);
    }

    // ---- The default table ----

    void defaultTableIsSound()
    {
        ActionRegistry actions;
        registerStandardActions(actions);
        VimKeymap map;
        QString error;
        QVERIFY2(map.loadDefaults(&error), qPrintable(error));
        QVERIFY(map.bindings().size() > 60);
        for (const VimKeymap::Issue& issue : map.issues(&actions))
            QVERIFY2(issue.severity != VimKeymap::Issue::Severity::Error, qPrintable(issue.text));
        for (const VimBinding& b : map.bindings()) {
            if (b.isGroup()) {
                QVERIFY2(!b.desc.isEmpty(), qPrintable(b.keys));
                continue;
            }
            QVERIFY2(!b.desc.isEmpty(), qPrintable(b.keys));
            QVERIFY2(!b.run.isEmpty(), qPrintable(b.keys));
            QVERIFY2(VimKeymap::contexts().contains(b.context), qPrintable(b.context));
            for (const QString& item : b.run) {
                QVERIFY2(
                    map.checkRunItem(item, &actions).isEmpty(), qPrintable(b.keys + " " + item));
                if (item.startsWith(QLatin1String("vim:")))
                    QVERIFY2(VimMachine::hasBuiltin(item.mid(4)), qPrintable(item));
            }
        }
    }

    void everyBuiltinIsImplemented()
    {
        for (const QString& name : VimKeymap::builtinNames())
            QVERIFY2(VimMachine::hasBuiltin(name), qPrintable(name));
    }

    void ctrlChordsAreTheDocumentedOnes()
    {
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        QSet<QString> chords;
        for (const VimBinding& b : map.bindings()) {
            for (const QString& t : b.tokens) {
                if (t.startsWith(QLatin1String("<C-")) || t.startsWith(QLatin1String("<A-")))
                    chords.insert(t);
            }
        }
        // Resolve's Ctrl keys stay the profile's, except these (design section 8): Redo, the
        // block selection of the timeline, the focus moves between panels and the window
        // prefix (83.46), and half a page in lists (<C-d> <C-u>, only in the list context), and
        // stepping a value in forms (<C-a> <C-x>).
        QCOMPARE(chords,
            (QSet<QString> {QStringLiteral("<C-r>"), QStringLiteral("<C-v>"),
                QStringLiteral("<C-h>"), QStringLiteral("<C-j>"), QStringLiteral("<C-k>"),
                QStringLiteral("<C-l>"), QStringLiteral("<C-w>"), QStringLiteral("<C-d>"),
                QStringLiteral("<C-u>"), QStringLiteral("<C-a>"), QStringLiteral("<C-x>")}));
        // <C-d> and <C-u> are only in the lists; the timeline keeps Change Duration and Duplicate.
        // <C-a> and <C-x> step values in forms (the timeline keeps Select All and Cut).
        for (const VimBinding& b : map.bindings()) {
            if (b.tokens.contains(QStringLiteral("<C-a>"))
                || b.tokens.contains(QStringLiteral("<C-x>")))
                QVERIFY2(b.context == QLatin1String("form"), qPrintable(b.context));
        }
        for (const VimBinding& b : map.bindings()) {
            if (b.tokens.contains(QStringLiteral("<C-d>"))
                || b.tokens.contains(QStringLiteral("<C-u>")))
                QCOMPARE(b.context, QStringLiteral("list"));
        }
    }

    void todaysKeysAreInTheTable()
    {
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        auto first = [&](const QString& ctx, QChar mode, const QStringList& keys) {
            return map.trie(ctx, mode).match(keys);
        };
        for (const char* k : {"h", "l", "H", "L", "w", "b", "0", "$", "G", "n", "N", "j", "k", "x",
                 "X", "p", "P", "u", "s", "M", ">", "<", "v", "i", "a", ":", "/"}) {
            QVERIFY2(first(QStringLiteral("timeline"), 'n', {QString::fromLatin1(k)}).kind
                    != VimMatch::Kind::None,
                k);
        }
        // d and y are operators; dd, yy and dw come from the grammar, not from the table
        QCOMPARE(first(QStringLiteral("timeline"), 'n', {"d"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(first(QStringLiteral("timeline"), 'n', {"d", "d"}).kind, VimMatch::Kind::None);
        QCOMPARE(first(QStringLiteral("timeline"), 'o', {"w"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(first(QStringLiteral("timeline"), 'o', {"i", "w"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(first(QStringLiteral("timeline"), 'o', {"d"}).kind, VimMatch::Kind::None);
        QCOMPARE(first(QStringLiteral("timeline"), 'n', {"g", "g"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(first(QStringLiteral("timeline"), 'n', {"z", "f"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(first(QStringLiteral("timeline"), 'n', {"m", "a"}).kind, VimMatch::Kind::Exact);
        // VISUAL: d x X y v and the motions.
        for (const char* k : {"d", "x", "X", "y", "v", "h", "l", "w"})
            QVERIFY2(first(QStringLiteral("timeline"), 'v', {QString::fromLatin1(k)}).kind
                    != VimMatch::Kind::None,
                k);
        // The leader tree.
        QCOMPARE(first(QStringLiteral("timeline"), 'n', {"<Space>", "s", "i"}).kind,
            VimMatch::Kind::Exact);
        QCOMPARE(
            first(QStringLiteral("timeline"), 'n', {"<Space>", "<Space>"}).binding->run.value(0),
            QStringLiteral("action:transport.play_pause"));
        // Viewer inherits the timeline's keys and adds its own.
        QCOMPARE(first(QStringLiteral("viewer"), 'n', {"h"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(first(QStringLiteral("viewer"), 'n', {"z", "f"}).binding->run.value(0),
            QStringLiteral("action:view.viewer_zoom_fit"));
        QCOMPARE(first(QStringLiteral("timeline"), 'n', {"z", "f"}).binding->run.value(0),
            QStringLiteral("action:timeline.zoom_fit"));
        // The source edits are on the leader now, not on i o a.
        QVERIFY(first(QStringLiteral("timeline"), 'n', {"g", "i"}).kind == VimMatch::Kind::None);
    }

    // ---- The user's changes ----

    void userMapsOverrideAndPersist()
    {
        {
            VimKeymap map;
            QVERIFY(map.loadDefaults());
            QString error;
            // A new key.
            QVERIFY(map.map(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("Q"),
                {QStringLiteral("edit.undo")}, QStringLiteral("my undo"), &error));
            QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({"Q"}).binding->run.value(0),
                QStringLiteral("action:edit.undo"));
            // Change a default: x now splits.
            QVERIFY(map.map(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("x"),
                {QStringLiteral("edit.split")}, QString(), &error));
            QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({"x"}).binding->run.value(0),
                QStringLiteral("action:edit.split"));
            QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({"x"}).binding->origin,
                VimBinding::Origin::User);
            // Remove a default.
            QVERIFY(
                map.unmap(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("p")));
            QCOMPARE(
                map.trie(QStringLiteral("timeline"), 'n').match({"p"}).kind, VimMatch::Kind::None);
            QCOMPARE(map.userOps().size(), 3);
        }
        // A restart: the changes come back from the settings.
        VimKeymap again;
        again.reload();
        QCOMPARE(
            again.trie(QStringLiteral("timeline"), 'n').match({"Q"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(again.trie(QStringLiteral("timeline"), 'n').match({"x"}).binding->run.value(0),
            QStringLiteral("action:edit.split"));
        QCOMPARE(
            again.trie(QStringLiteral("timeline"), 'n').match({"p"}).kind, VimMatch::Kind::None);
        // Reset one key: the default comes back; the others stay.
        again.resetKeys(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("p"));
        QCOMPARE(again.trie(QStringLiteral("timeline"), 'n').match({"p"}).binding->run.value(0),
            QStringLiteral("vim:cmd_paste"));
        again.resetKeys(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("x"));
        QCOMPARE(again.trie(QStringLiteral("timeline"), 'n').match({"x"}).binding->run.value(0),
            QStringLiteral("vim:cmd_delete"));
        QCOMPARE(again.userOps().size(), 1);
        again.resetAll();
        QVERIFY(again.userOps().isEmpty());
        QVERIFY(!QSettings().contains(QStringLiteral("vim/maps")));
        QCOMPARE(
            again.trie(QStringLiteral("timeline"), 'n').match({"Q"}).kind, VimMatch::Kind::None);
    }

    void userMapsAreCheckedAndAGlobalOneWinsEverywhere()
    {
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        QString error;
        QVERIFY(!map.map(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("a b"),
            {QStringLiteral("edit.undo")}, {}, &error));
        QVERIFY(!map.map(QStringLiteral("nowhere"), QStringLiteral("n"), QStringLiteral("Q"),
            {QStringLiteral("edit.undo")}, {}, &error));
        QVERIFY(!map.map(QStringLiteral("timeline"), QStringLiteral("x"), QStringLiteral("Q"),
            {QStringLiteral("edit.undo")}, {}, &error));
        QVERIFY(!map.map(
            QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("Q"), {}, {}, &error));
        QVERIFY(!map.map(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("Q"),
            {QStringLiteral("vim:nonsense")}, {}, &error));
        QVERIFY(map.userOps().isEmpty());
        // A mapping of the context "global" made by the user beats a default of the timeline.
        QVERIFY(map.map(QStringLiteral("global"), QStringLiteral("n"), QStringLiteral("h"),
            {QStringLiteral("edit.undo")}, {}, &error));
        QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({"h"}).binding->run.value(0),
            QStringLiteral("action:edit.undo"));
        QCOMPARE(map.trie(QStringLiteral("viewer"), 'n').match({"h"}).binding->run.value(0),
            QStringLiteral("action:edit.undo"));
    }

    void leaderCanChange()
    {
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({"<Space>", "s", "i"}).kind,
            VimMatch::Kind::Exact);
        map.setLeader(QStringLiteral(","));
        QCOMPARE(map.leader(), QStringLiteral(","));
        QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({",", "s", "i"}).kind,
            VimMatch::Kind::Exact);
        QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({"<Space>", "s", "i"}).kind,
            VimMatch::Kind::None);
        // It survives a restart.
        VimKeymap again;
        again.reload();
        QCOMPARE(again.leader(), QStringLiteral(","));
        QCOMPARE(again.trie(QStringLiteral("timeline"), 'n').match({",", "s", "i"}).kind,
            VimMatch::Kind::Exact);
    }

    void exportAndImport()
    {
        VimKeymap a;
        QVERIFY(a.loadDefaults());
        QVERIFY(a.map(QStringLiteral("timeline"), QStringLiteral("nv"), QStringLiteral("Q"),
            {QStringLiteral("edit.undo")}, QStringLiteral("undo"), nullptr));
        QVERIFY(a.unmap(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("p")));
        const QByteArray json = a.exportJson();
        QVERIFY(QJsonDocument::fromJson(json).object().contains(QStringLiteral("kinoklipVimKeys")));
        QSettings().clear();
        VimKeymap b;
        QVERIFY(b.loadDefaults());
        QString error;
        QVERIFY2(b.importJson(json, &error), qPrintable(error));
        QCOMPARE(b.userOps().size(), 2);
        QCOMPARE(b.trie(QStringLiteral("timeline"), 'v').match({"Q"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(b.trie(QStringLiteral("timeline"), 'n').match({"p"}).kind, VimMatch::Kind::None);
        // Stored, so it survives a restart.
        VimKeymap c;
        c.reload();
        QCOMPARE(c.userOps().size(), 2);
        // Bad files change nothing.
        QVERIFY(!c.importJson("not json", &error));
        QVERIFY(!c.importJson(
            R"({"kinoklipVimKeys":1,"maps":[{"op":"map","context":"timeline","modes":"n","keys":"Q","run":["vim:nonsense"]}]})",
            &error));
        QVERIFY(!c.importJson(R"({"something":"else"})", &error));
        QCOMPARE(c.userOps().size(), 2);
    }

    // ---- :nmap and the vimrc ----

    void exCommands()
    {
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        bool ok = false;
        QCOMPARE(map.executeLine(QStringLiteral("nmap Q edit.undo"), &ok), QString());
        QVERIFY(ok);
        QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({"Q"}).binding->run.value(0),
            QStringLiteral("action:edit.undo"));
        QCOMPARE(map.trie(QStringLiteral("viewer"), 'n').match({"Q"}).kind,
            VimMatch::Kind::Exact); // every context
        QVERIFY(
            map.executeLine(QStringLiteral(":nnoremap <silent> <leader>w :w<CR>"), &ok).isEmpty());
        QVERIFY(ok);
        QCOMPARE(
            map.trie(QStringLiteral("timeline"), 'n').match({"<Space>", "w"}).binding->run.value(0),
            QStringLiteral("ex:w"));
        QVERIFY(map.executeLine(QStringLiteral("vmap Z vim:visual_char"), &ok).isEmpty());
        QCOMPARE(
            map.trie(QStringLiteral("timeline"), 'v').match({"Z"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({"Z"}).kind, VimMatch::Kind::None);
        // :nunmap removes a default of any context.
        QVERIFY(map.executeLine(QStringLiteral("nunmap p"), &ok).isEmpty());
        QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({"p"}).kind, VimMatch::Kind::None);
        // Errors say what is wrong.
        QVERIFY(!map.executeLine(QStringLiteral("nmap Q"), &ok).isEmpty());
        QVERIFY(!ok);
        QVERIFY(!map.executeLine(QStringLiteral("nmap a b edit.undo"), &ok).isEmpty());
        QVERIFY(!ok);
        QVERIFY(!map.executeLine(QStringLiteral("frobnicate x y"), &ok).isEmpty());
        QVERIFY(!ok);
        QVERIFY(!map.executeLine(QStringLiteral("set bogus=1"), &ok).isEmpty());
        QVERIFY(!ok);
        QVERIFY(!map.executeLine(QStringLiteral("set timeoutlen=5"), &ok).isEmpty());
        QVERIFY(!ok);
        // :set options.
        QVERIFY(map.executeLine(QStringLiteral("set timeoutlen=250"), &ok).isEmpty());
        QCOMPARE(map.timeoutMs(), 250);
        QVERIFY(map.executeLine(QStringLiteral("set leader=,"), &ok).isEmpty());
        QCOMPARE(map.leader(), QStringLiteral(","));
        QVERIFY(map.executeLine(QStringLiteral("set nophysicalkeys"), &ok).isEmpty());
        QVERIFY(!vimopts::physicalKeys());
        // :map lists what the user made.
        const QStringList lines = map.listMappings();
        QVERIFY(lines.join(QLatin1Char('\n')).contains(QStringLiteral("edit.undo")));
        // Comments and empty lines do nothing.
        QVERIFY(map.executeLine(QStringLiteral("\" comment"), &ok).isEmpty());
        QVERIFY(ok);
        QVERIFY(map.executeLine(QString(), &ok).isEmpty());
        QVERIFY(ok);
    }

    void vimrcFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("vimrc"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("\" my keys\n"
                "nmap <leader>q :q<CR>\n"
                "\n"
                "set leader=,\n"
                "set timeoutlen=300\n"
                "nunmap N\n"
                "nmap Q edit.undo\n"
                "nmap\n"
                "nmap Y vim:nonsense\n");
        f.close();
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        QVERIFY(map.sourceVimrc(path));
        // The leader and timeout apply to the file's own mappings, whatever the line order.
        QCOMPARE(map.leader(), QStringLiteral(","));
        QCOMPARE(map.timeoutMs(), 300);
        QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({",", "q"}).binding->run.value(0),
            QStringLiteral("ex:q"));
        QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({",", "q"}).binding->origin,
            VimBinding::Origin::Vimrc);
        QCOMPARE(map.trie(QStringLiteral("timeline"), 'n').match({"N"}).kind, VimMatch::Kind::None);
        QCOMPARE(
            map.trie(QStringLiteral("timeline"), 'n').match({"Q"}).kind, VimMatch::Kind::Exact);
        // Bad lines are reported with their number; the good ones still apply.
        const QStringList messages = map.vimrcMessages();
        QCOMPARE(messages.size(), 2);
        QVERIFY(messages[0].contains(QStringLiteral("8")));
        QVERIFY(messages[1].contains(QStringLiteral("9")));
        // Nothing of it went to the settings, and a missing file is no error.
        QVERIFY(!QSettings().contains(QStringLiteral("vim/maps")));
        QVERIFY(!QSettings().contains(QStringLiteral("vim/leader")));
        QVERIFY(!map.sourceVimrc(dir.filePath(QStringLiteral("none"))));
        // The vimrc cannot switch Vim mode on or off.
        QFile g(dir.filePath(QStringLiteral("vimrc2")));
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write("set vim\n");
        g.close();
        QVERIFY(map.sourceVimrc(dir.filePath(QStringLiteral("vimrc2"))));
        QVERIFY(!vimopts::enabled());
        QCOMPARE(map.vimrcMessages().size(), 1);
    }

    // ---- Checks (the Vim tab shows them) ----

    void checksFindPrefixesAndUnknownCommands()
    {
        ActionRegistry actions;
        registerStandardActions(actions);
        VimKeymap map;
        QVERIFY(map.loadDefaults());
        auto count = [&](VimKeymap::Issue::Severity s) {
            int n = 0;
            for (const auto& i : map.issues(&actions))
                n += i.severity == s;
            return n;
        };
        const int warningsBefore = count(VimKeymap::Issue::Severity::Warning);
        // gx as a binding makes g... no: map g itself, which is the start of gg.
        QVERIFY(map.map(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("g"),
            {QStringLiteral("edit.undo")}, {}, nullptr));
        QCOMPARE(count(VimKeymap::Issue::Severity::Warning), warningsBefore + 1);
        bool found = false;
        for (const auto& i : map.issues(&actions))
            found = found
                || (i.severity == VimKeymap::Issue::Severity::Warning
                    && i.keys == QStringLiteral("g"));
        QVERIFY(found);
        // An action id that does not exist is an error (an old export, a removed command).
        QString error;
        QVERIFY(map.importJson(
            R"({"kinoklipVimKeys":1,"maps":[{"op":"map","context":"timeline","modes":"n","keys":"Q","run":["action:gone.away"]}]})",
            &error));
        QVERIFY(count(VimKeymap::Issue::Severity::Error) >= 1);
        // The profile's keys the layer takes in NORMAL are listed as information.
        ActionRegistry withKeys;
        withKeys.loadDefaultKeymap();
        registerStandardActions(withKeys);
        bool info = false;
        for (const auto& i : map.issues(&withKeys))
            info = info || (i.severity == VimKeymap::Issue::Severity::Info);
        QVERIFY(info);
    }

    void runItemsAreNormalised()
    {
        QCOMPARE(VimKeymap::normaliseRunItem(QStringLiteral("edit.undo")),
            QStringLiteral("action:edit.undo"));
        QCOMPARE(VimKeymap::normaliseRunItem(QStringLiteral("action:edit.undo")),
            QStringLiteral("action:edit.undo"));
        QCOMPARE(VimKeymap::normaliseRunItem(QStringLiteral(":w<CR>")), QStringLiteral("ex:w"));
        QCOMPARE(VimKeymap::normaliseRunItem(QStringLiteral(":marker add --frame 3")),
            QStringLiteral("ex:marker add --frame 3"));
        QCOMPARE(VimKeymap::normaliseRunItem(QStringLiteral("vim:visual_char")),
            QStringLiteral("vim:visual_char"));
    }

    void ofRegistryIsOnePerApplication()
    {
        ActionRegistry actions;
        registerStandardActions(actions);
        VimKeymap* a = VimKeymap::of(actions);
        QVERIFY(a);
        QCOMPARE(VimKeymap::of(actions), a);
        QCOMPARE(a->parent(), &actions);
        QVERIFY(!a->bindings().empty());
    }
};

QTEST_MAIN(TestVimKeymap)
#include "test_vimkeymap.moc"
