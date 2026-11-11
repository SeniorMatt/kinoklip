// SPDX-License-Identifier: GPL-3.0-or-later
// Menu layouts (docs/behavior-spec.md section 82): the Streamlined arrangement of the menu bar
// (menu and group sizes, depth, every command once), switching at run time without losing
// keys or check states, the page menu, the Preferences control, the settings import and the
// shorter context menus of the timeline clip and the media pool. Edit's Cut / Copy / Paste /
// Delete / Select All act on the node editor while it has focus.
#include "app/MainWindow.h"
#include "app/MenuReference.h"
#include "app/SettingsTransfer.h"
#include "app/Theme.h"
#include "app/pages/CompositingPage.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/MenuLayouts.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/NodeEditor.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>

using namespace kk;
using menulayout::Layout;

namespace {

constexpr int kMaxEntries = 12;
constexpr int kMaxGroup = 5;
constexpr int kMaxDepth = 2;

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    quint64 clipId = 0;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        theme::registerActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.name = QStringLiteral("Plate");
        clip.start = 10;
        clip.duration = 50;
        project.beginEdit(QStringLiteral("Add Clip"));
        clipId = editops::overwriteClip(
            project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }

    /// Lets the menus that were discarded go (they are deleted later).
    static void settle()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void use(Layout layout)
    {
        window->setMenuLayout(layout);
        settle();
    }

    QMenu* menuNamed(const QString& name) const { return window->findChild<QMenu*>(name); }
    QMenu* pageMenu() const { return menuNamed(QStringLiteral("menu:page")); }
};

/// The registry id a menu entry stands for (itself or what it mirrors).
QString idOf(const ActionRegistry& registry, QHash<const QAction*, QString>& ids, QAction* entry)
{
    if (ids.isEmpty()) {
        for (const QString& id : registry.ids())
            ids.insert(registry.action(id), id);
    }
    const QString mirrored = menulayout::mirroredId(entry);
    return mirrored.isEmpty() ? ids.value(entry) : mirrored;
}

/// Entries of a menu, separators not counted; the sizes of the groups between separators.
struct Shape
{
    int entries = 0;
    QVector<int> groups;
};

Shape shapeOf(const QMenu* menu)
{
    Shape shape;
    int group = 0;
    for (const QAction* a : menu->actions()) {
        if (a->isSeparator()) {
            if (group)
                shape.groups.append(group);
            group = 0;
            continue;
        }
        ++shape.entries;
        ++group;
    }
    if (group)
        shape.groups.append(group);
    return shape;
}

/// Problems with the sizes of @p menu and everything below it, as text (empty = fine).
/// @p depth is 0 for a menu of the menu bar.
QString limitProblems(const QMenu* menu, const QString& path, int depth)
{
    QString out;
    if (menu->property("kkDynamicMenu").toBool())
        return out; // lists files or machine settings
    const Shape shape = shapeOf(menu);
    // Colour and flag palettes are laid out in groups, not counted as commands.
    const QString name = menu->objectName();
    const bool palette
        = name.endsWith(QLatin1String("ColorMenu")) || name.endsWith(QLatin1String("FlagMenu"));
    if (shape.entries > kMaxEntries && !palette)
        out += QStringLiteral("%1 has %2 entries; ").arg(path).arg(shape.entries);
    for (const int group : shape.groups) {
        if (group > kMaxGroup)
            out += QStringLiteral("%1 has a group of %2; ").arg(path).arg(group);
    }
    for (const QAction* a : menu->actions()) {
        const QMenu* sub = a->menu();
        if (!sub)
            continue;
        const QString name = path + QStringLiteral(" > ") + a->text();
        if (depth + 1 > kMaxDepth) {
            out += QStringLiteral("%1 is too deep; ").arg(name);
            continue;
        }
        out += limitProblems(sub, name, depth + 1);
    }
    return out;
}

/// How often each registry command is in @p menu (submenus too); dynamic menus are skipped.
void countIds(const QMenu* menu, const ActionRegistry& registry,
    QHash<const QAction*, QString>& ids, QHash<QString, int>& counts)
{
    for (QAction* a : menu->actions()) {
        if (a->isSeparator())
            continue;
        if (const QMenu* sub = a->menu()) {
            if (!sub->property("kkDynamicMenu").toBool())
                countIds(sub, registry, ids, counts);
            continue;
        }
        const QString id = idOf(registry, ids, a);
        if (!id.isEmpty())
            ++counts[id];
    }
}

QHash<QString, int> idCounts(Fixture& f, bool withPageMenus)
{
    QHash<const QAction*, QString> ids;
    QHash<QString, int> counts;
    for (const QAction* top : f.window->menuBar()->actions()) {
        if (const QMenu* menu = top->menu())
            countIds(menu, f.actions, ids, counts);
    }
    if (withPageMenus) {
        for (const char* page : {"cut", "compositing", "color"}) {
            QMenu menu;
            const menulayout::Menu spec = menulayout::pageMenu(QLatin1String(page));
            menulayout::fill(&menu, spec.entries, f.actions, QStringLiteral("menu"));
            countIds(&menu, f.actions, ids, counts);
        }
    }
    return counts;
}

void collectNodeIds(const QList<menulayout::Node>& nodes, QStringList& out)
{
    for (const menulayout::Node& node : nodes) {
        if (node.kind == menulayout::Node::Kind::Action)
            out.append(node.id);
        collectNodeIds(node.children, out);
    }
}

/// Reads the popup that is open while @p open runs (QMenu::exec blocks): the shape of it and
/// everything below, the texts, and whether an entry is disabled.
struct PopupInfo
{
    bool seen = false;
    QString problems;
    QStringList texts;
    QStringList disabled;
};

PopupInfo readPopup(const std::function<void()>& open)
{
    PopupInfo info;
    QTimer::singleShot(0, [&info] {
        QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (auto* m = qobject_cast<QMenu*>(w); m && m->isVisible())
                    menu = m;
            }
        }
        if (!menu)
            return;
        info.seen = true;
        info.problems = limitProblems(menu, QStringLiteral("popup"), 0);
        for (const QAction* a : menu->actions()) {
            if (a->isSeparator())
                continue;
            const QString text
                = a->text().section(QLatin1Char('\t'), 0, 0).remove(QLatin1Char('&'));
            info.texts.append(text);
            if (!a->isEnabled())
                info.disabled.append(text);
        }
        menu->close();
    });
    open();
    return info;
}

} // namespace

class TestMenuLayout : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_menulayout"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    // 82.1: the choice is stored as "resolve" | "streamlined"; Resolve is the default.
    void choiceAndDefault()
    {
        QCOMPARE(menulayout::layout(), Layout::Resolve);
        QCOMPARE(menulayout::layoutId(Layout::Streamlined), QStringLiteral("streamlined"));
        QCOMPARE(menulayout::layoutFromId(QStringLiteral("nonsense")), Layout::Resolve);
        QSignalSpy spy(&menulayout::notifier(), &menulayout::Notifier::changed);
        menulayout::setLayout(Layout::Streamlined);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(QSettings().value(QStringLiteral("ui/menuLayout")).toString(),
            QStringLiteral("streamlined"));
        menulayout::setLayout(Layout::Streamlined); // unchanged: no signal
        QCOMPARE(spy.count(), 1);
        menulayout::setLayout(Layout::Resolve);
        QCOMPARE(spy.count(), 2);
    }

    // 82.2: the Resolve layout is today's menu bar, with Workspace and no page menu.
    void resolveLayoutIsUnchanged()
    {
        Fixture f;
        QStringList titles;
        for (const QAction* a : f.window->menuBar()->actions())
            titles.append(a->text().remove(QLatin1Char('&')));
        QVERIFY2(titles.contains(QStringLiteral("Workspace")), qPrintable(titles.join(',')));
        QVERIFY(titles.contains(QStringLiteral("Trim")));
        QVERIFY(!f.pageMenu());
        QVERIFY(f.menuNamed(QStringLiteral("menu:cut")));
    }

    // 82.3: Streamlined bar: the menus of the design, none over 12 entries, groups of at most
    // five, at most two levels of submenus.
    void streamlinedLimits()
    {
        Fixture f;
        f.use(Layout::Streamlined);
        QStringList titles;
        for (const QAction* a : f.window->menuBar()->actions())
            titles.append(a->text().remove(QLatin1Char('&')));
        QCOMPARE(titles,
            (QStringList {QCoreApplication::applicationName(), QStringLiteral("File"),
                QStringLiteral("Edit"), QStringLiteral("View"), QStringLiteral("Timeline"),
                QStringLiteral("Clip"), QStringLiteral("Trim"), QStringLiteral("Mark"),
                QStringLiteral("Playback"), QStringLiteral("Audio"), QStringLiteral("Help")}));
        QString problems;
        for (const QAction* top : f.window->menuBar()->actions())
            problems += limitProblems(top->menu(), top->text(), 0);
        QVERIFY2(problems.isEmpty(), qPrintable(problems));
        // The page menus, whichever page is shown.
        for (const char* page : {"cut", "compositing", "color"}) {
            QMenu menu;
            const menulayout::Menu spec = menulayout::pageMenu(QLatin1String(page));
            menulayout::fill(&menu, spec.entries, f.actions, QStringLiteral("menu"));
            const QString p = limitProblems(&menu, QLatin1String(page), 0);
            QVERIFY2(p.isEmpty(), qPrintable(p));
        }
    }

    // 82.4: every command of the Resolve menus is in the Streamlined menu bar (page menus
    // included) exactly once, except the documented hidden list, which is in no menu; and the
    // table names no command that does not exist.
    void everyCommandOnce()
    {
        Fixture f;
        f.use(Layout::Resolve);
        const QHash<QString, int> resolve = idCounts(f, false);
        f.use(Layout::Streamlined);
        const QHash<QString, int> streamlined = idCounts(f, true);
        QString problems;
        for (auto it = resolve.cbegin(); it != resolve.cend(); ++it) {
            const int n = streamlined.value(it.key());
            if (menulayout::isHidden(it.key())) {
                if (n != 0)
                    problems += it.key() + QStringLiteral(" is hidden but listed; ");
            } else if (n != 1) {
                problems += QStringLiteral("%1 appears %2 times; ").arg(it.key()).arg(n);
            }
        }
        for (auto it = streamlined.cbegin(); it != streamlined.cend(); ++it) {
            if (it.value() != 1)
                problems += QStringLiteral("%1 appears %2 times; ").arg(it.key()).arg(it.value());
            if (!resolve.contains(it.key()))
                problems += it.key() + QStringLiteral(" is not in the Resolve menus; ");
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems));
        // The hidden list names real commands.
        for (const menulayout::HiddenAction& hidden : menulayout::hiddenActions()) {
            const QString id = QLatin1String(hidden.id);
            if (id.endsWith(QLatin1Char('*'))) {
                bool any = false;
                for (const QString& known : f.actions.ids())
                    any = any || known.startsWith(id.chopped(1));
                QVERIFY2(any, hidden.id);
            } else {
                QVERIFY2(f.actions.contains(id), hidden.id);
            }
        }
        // Every id in the tables exists.
        QStringList named;
        for (const menulayout::Menu& m : menulayout::streamlinedMenus())
            collectNodeIds(m.entries, named);
        for (const char* page : {"cut", "compositing", "color"})
            collectNodeIds(menulayout::pageMenu(QLatin1String(page)).entries, named);
        for (const QString& id : named)
            QVERIFY2(f.actions.contains(id), qPrintable(id));
    }

    // 82.5: switching keeps keys and check states (they live in the actions), the menus show
    // them, and repeated switching leaves no menus or entries behind.
    void switchingKeepsKeysAndChecks()
    {
        Fixture f;
        f.action("timeline.snapping")->setChecked(false);
        f.action("tool.blade")->setChecked(true);
        f.action("playback.resolution_half")->setChecked(true);
        QHash<QString, QList<QKeySequence>> keys;
        QHash<QString, bool> checks;
        for (const QString& id : f.actions.ids()) {
            keys.insert(id, f.actions.shortcuts(id));
            checks.insert(id, f.action(id.toLatin1().constData())->isChecked());
        }
        f.use(Layout::Streamlined);
        const int menusStreamlined = f.window->findChildren<QMenu*>().size();
        const int actionsStreamlined = f.window->findChildren<QAction*>().size();
        // An entry under another name follows the command.
        QHash<const QAction*, QString> ids;
        QAction* snap = nullptr;
        for (QAction* a : f.window->findChild<QMenu*>(QStringLiteral("menu:timeline"))->actions()) {
            if (idOf(f.actions, ids, a) == QLatin1String("timeline.snapping"))
                snap = a;
        }
        QVERIFY(snap);
        QVERIFY(!snap->isChecked());
        QVERIFY(snap->text().startsWith(QStringLiteral("Snapping")));
        QVERIFY(snap->text().contains(f.actions.shortcutText(QStringLiteral("timeline.snapping"))));
        f.action("timeline.snapping")->setChecked(true);
        QVERIFY(snap->isChecked());
        snap->trigger(); // switches the command
        QVERIFY(!f.action("timeline.snapping")->isChecked());
        f.action("timeline.snapping")->setChecked(false);
        for (int i = 0; i < 3; ++i) {
            f.use(Layout::Resolve);
            f.use(Layout::Streamlined);
        }
        QCOMPARE(f.window->findChildren<QMenu*>().size(), menusStreamlined);
        QCOMPARE(f.window->findChildren<QAction*>().size(), actionsStreamlined);
        f.use(Layout::Resolve);
        for (const QString& id : f.actions.ids()) {
            QVERIFY2(f.actions.shortcuts(id) == keys.value(id), qPrintable(id));
            QVERIFY2(f.action(id.toLatin1().constData())->isChecked() == checks.value(id),
                qPrintable(id));
        }
        // Recent projects and presets are filled in again after a rebuild.
        QVERIFY(f.menuNamed(QStringLiteral("menu:file.recent")));
        f.use(Layout::Streamlined);
        QVERIFY(f.menuNamed(QStringLiteral("menu:file.recent")));
        QVERIFY(
            f.menuNamed(QStringLiteral("menu:file.recent"))->property("kkDynamicMenu").toBool());
        QVERIFY(f.menuNamed(QStringLiteral("menu:view.guide_presets")));
    }

    // 82.6: the menu follows the stored choice at start-up and when it changes.
    void followsThePreference()
    {
        menulayout::setLayout(Layout::Streamlined);
        Fixture f;
        QCOMPARE(f.window->menuLayout(), Layout::Streamlined);
        QVERIFY(f.menuNamed(QStringLiteral("menu:app")));
        QVERIFY(!f.menuNamed(QStringLiteral("menu:settings")));
        menulayout::setLayout(Layout::Resolve);
        Fixture::settle();
        QCOMPARE(f.window->menuLayout(), Layout::Resolve);
        QVERIFY(f.menuNamed(QStringLiteral("menu:settings")));
    }

    // 82.7: the page menu follows the page and is hidden on pages without one.
    void pageMenuFollowsThePage()
    {
        Fixture f;
        f.use(Layout::Streamlined);
        f.window->resize(1400, 800);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        struct Expect
        {
            const char* page;
            const char* title;
        };
        for (const Expect& e : {Expect {"edit", nullptr}, Expect {"cut", "Cut"},
                 Expect {"compositing", "Nodes"}, Expect {"color", "Grade"},
                 Expect {"media", nullptr}, Expect {"cut", "Cut"}, Expect {"deliver", nullptr}}) {
            f.window->setCurrentPage(QLatin1String(e.page));
            Fixture::settle();
            const QList<QAction*> bar = f.window->menuBar()->actions();
            int pageMenus = 0;
            for (const QAction* a : bar) {
                if (a->menu() && a->menu()->objectName() == QLatin1String("menu:page")) {
                    ++pageMenus;
                    QCOMPARE(a->text().remove(QLatin1Char('&')), QLatin1String(e.title));
                    // Right before Help.
                    QCOMPARE(bar.at(bar.indexOf(const_cast<QAction*>(a)) + 1)
                                 ->text()
                                 .remove(QLatin1Char('&')),
                        QStringLiteral("Help"));
                }
            }
            QCOMPARE(pageMenus, e.title ? 1 : 0);
        }
        // The Grade menu is the Color page's; its commands are the same actions as before.
        f.window->setCurrentPage(QStringLiteral("color"));
        Fixture::settle();
        QVERIFY(f.pageMenu());
        QCOMPARE(f.pageMenu()->title().remove(QLatin1Char('&')), QStringLiteral("Grade"));
        // Resolve layout: no page menu at all.
        f.use(Layout::Resolve);
        QVERIFY(!f.pageMenu());
        f.window->setCurrentPage(QStringLiteral("cut"));
        Fixture::settle();
        QVERIFY(!f.pageMenu());
    }

    // 82.8: Preferences > UI Settings > Menu layout, live; the settings file carries the key.
    void preferencesControl()
    {
        Fixture f;
        PreferencesDialog dialog(nullptr);
        QVERIFY(dialog.showSection(QStringLiteral("ui")));
        auto* box = dialog.findChild<QComboBox*>(QStringLiteral("MenuLayout"));
        QVERIFY(box);
        QCOMPARE(box->itemText(0), QStringLiteral("Resolve"));
        QCOMPARE(box->itemText(1), QStringLiteral("Streamlined"));
        QCOMPARE(box->currentIndex(), 0);
        box->setCurrentIndex(1);
        dialog.apply();
        Fixture::settle();
        QCOMPARE(f.window->menuLayout(), Layout::Streamlined);
        QVERIFY(f.menuNamed(QStringLiteral("menu:app")));
        box->setCurrentIndex(0);
        dialog.apply();
        Fixture::settle();
        QCOMPARE(f.window->menuLayout(), Layout::Resolve);
    }

    void settingsImportAcceptsTheKey()
    {
        QTemporaryDir dir;
        settingstransfer::Locations loc;
        loc.dataDir = dir.path() + QStringLiteral("/data");
        loc.backupDir = dir.path() + QStringLiteral("/backups");
        menulayout::setLayout(Layout::Streamlined);
        QByteArray doc = settingstransfer::exportDocument({}, loc);
        QVERIFY(doc.contains("ui/menuLayout"));
        settingstransfer::Parsed parsed;
        QString error;
        QVERIFY2(settingstransfer::parse(doc, &parsed, &error), qPrintable(error));
        QCOMPARE(parsed.settings.value(QStringLiteral("ui/menuLayout")).toString(),
            QStringLiteral("streamlined"));
        QVERIFY2(parsed.rejected.isEmpty(), qPrintable(parsed.rejected.join(' ')));
        // Only the two layouts are accepted.
        doc.replace("\"streamlined\"", "\"fancy\"");
        settingstransfer::Parsed other;
        QVERIFY(settingstransfer::parse(doc, &other, &error));
        QVERIFY(!other.settings.contains(QStringLiteral("ui/menuLayout")));
        QVERIFY(!other.rejected.isEmpty());
    }

    // 82.9: --menu-reference lists both layouts, whichever one the window is in.
    void menuReferenceHasBothLayouts()
    {
        Fixture f;
        f.use(Layout::Streamlined);
        const menureference::Reference ref = menureference::build(*f.window);
        QVERIFY2(ref.undescribed.isEmpty(), qPrintable(ref.undescribed.join(QStringLiteral("; "))));
        QVERIFY(ref.streamlinedEntries > 200);
        QVERIFY(ref.streamlinedMenus >= 11);
        QVERIFY(ref.markdown.contains(QStringLiteral("\n# Streamlined menu layout\n")));
        QVERIFY(ref.markdown.contains(QStringLiteral("| Split at Playhead | ")));
        QVERIFY(ref.markdown.contains(QStringLiteral("\n## Streamlined Grade (Color page)\n")));
        QVERIFY(!ref.markdown.contains(QStringLiteral("&")));
        QVERIFY(!ref.markdown.contains(QLatin1Char('\t')));
        QCOMPARE(f.window->menuLayout(), Layout::Streamlined); // left as it was
        f.use(Layout::Resolve);
        QCOMPARE(menureference::build(*f.window).markdown, ref.markdown);
    }

    // 82.10: Edit > Cut / Copy / Paste / Delete / Select All act on the nodes while the node
    // editor has focus, and on the timeline otherwise.
    void editCommandsActOnTheNodeEditor()
    {
        Fixture f;
        f.window->resize(1800, 1000);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        QCOMPARE(nodeEditorHasFocus(), false);
        f.project.timeline()->setPlayhead(20);
        f.window->setCurrentPage(QStringLiteral("compositing"));
        QCoreApplication::processEvents();
        CompositingPage* page = f.window->compositingPage();
        QVERIFY(page);
        CompSession* session = page->session();
        QVERIFY(session->composition().nodes.size() >= 2);
        f.selection.setClips({});
        session->setSelection({});
        f.window->activateWindow();
        page->nodeEditor()->setFocus();
        QTRY_VERIFY(nodeEditorHasFocus());
        f.action("edit.select_all")->trigger();
        QCOMPARE(session->selectedNodes().size(), qsizetype(session->composition().nodes.size()));
        QVERIFY(f.selection.clips().isEmpty()); // the timeline did not select its clips
        // Without the node editor's focus the timeline gets the command.
        f.window->setCurrentPage(QStringLiteral("edit"));
        QCoreApplication::processEvents();
        f.window->editPage()->timeline()->setFocus();
        QTRY_VERIFY(!nodeEditorHasFocus());
        f.action("edit.select_all")->trigger();
        QVERIFY(!f.selection.clips().isEmpty());
    }

    // 82.11: the timeline clip menu: Resolve keeps its long menu, Streamlined has at most 12
    // entries, groups of at most five, no greyed entries.
    void timelineClipMenu()
    {
        Fixture f;
        f.window->resize(1600, 900);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        f.window->setCurrentPage(QStringLiteral("edit"));
        TimelineWidget* w = f.window->editPage()->timeline()->timelineWidget();
        QVERIFY(w);
        w->setPixelsPerFrame(10.0, 120);
        w->scrollToFrame(0);
        QCoreApplication::processEvents();
        const TimelineLayout& layout = w->layout();
        const ClipData* clip = editops::clipById(f.project.timeline()->data(), f.clipId);
        QVERIFY(clip);
        const QRectF rect = layout.clipRect(layout.rowOf({TrackType::Video, 0}), *clip);
        QVERIFY(!rect.isEmpty());
        const QPoint pos = rect.center().toPoint();
        auto open = [&] {
            QContextMenuEvent event(QContextMenuEvent::Mouse, pos, w->viewport()->mapToGlobal(pos));
            QCoreApplication::sendEvent(w->viewport(), &event);
        };
        const PopupInfo resolve = readPopup(open);
        QVERIFY(resolve.seen);
        QVERIFY(resolve.texts.size() > kMaxEntries);
        menulayout::setLayout(Layout::Streamlined); // the context menus read the preference
        const PopupInfo slim = readPopup(open);
        QVERIFY(slim.seen);
        QVERIFY2(slim.problems.isEmpty(), qPrintable(slim.problems));
        QVERIFY2(slim.disabled.isEmpty(), qPrintable(slim.disabled.join(',')));
        QVERIFY2(slim.texts.contains(QStringLiteral("Split at Playhead")),
            qPrintable(slim.texts.join(',')));
        QVERIFY(slim.texts.contains(QStringLiteral("Speed and Duration")));
        QVERIFY(slim.texts.contains(QStringLiteral("More")));
        QVERIFY(slim.texts.size() < resolve.texts.size());
        // Nothing to paste: no Paste entry (hidden, not greyed).
        QVERIFY(!slim.texts.contains(QStringLiteral("Paste")));
    }

    // 82.12: the media pool item and bin menus.
    void mediaPoolMenus()
    {
        Fixture f;
        MediaItem item;
        item.path = QCoreApplication::applicationFilePath();
        item.name = QStringLiteral("clip.mov");
        item.info.valid = true;
        item.info.hasVideo = true;
        item.info.hasAudio = true;
        item.info.width = 1920;
        item.info.height = 1080;
        item.info.frameRate = Rational {25, 1};
        item.info.durationUs = 10'000'000;
        f.project.beginEdit(QStringLiteral("add"));
        const QString id = f.project.mediaPool()->addItem(item);
        f.project.endEdit();
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.resize(900, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QListView* grid = nullptr;
        for (QListView* v : panel.findChildren<QListView*>()) {
            if (v->viewMode() == QListView::IconMode)
                grid = v;
        }
        QVERIFY(grid);
        QModelIndex index;
        for (int row = 0; row < grid->model()->rowCount(); ++row) {
            const QModelIndex candidate = grid->model()->index(row, MediaPoolModel::NameColumn);
            if (candidate.data().toString() == QLatin1String("clip.mov"))
                index = candidate;
        }
        QVERIFY(index.isValid());
        grid->setCurrentIndex(index);
        grid->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect);
        QCOMPARE(panel.currentMediaId(), id);
        auto open
            = [&] { emit grid->customContextMenuRequested(grid->visualRect(index).center()); };
        const PopupInfo resolve = readPopup(open);
        QVERIFY(resolve.seen);
        QVERIFY2(resolve.texts.size() > kMaxEntries, qPrintable(resolve.texts.join(',')));
        menulayout::setLayout(Layout::Streamlined);
        const PopupInfo slim = readPopup(open);
        QVERIFY(slim.seen);
        QVERIFY2(slim.problems.isEmpty(), qPrintable(slim.problems));
        QVERIFY2(slim.disabled.isEmpty(), qPrintable(slim.disabled.join(',')));
        QVERIFY2(slim.texts.contains(QStringLiteral("Rename")), qPrintable(slim.texts.join(',')));
        QVERIFY(slim.texts.contains(QStringLiteral("Delete")));
        QVERIFY(slim.texts.contains(QStringLiteral("Media Tools")));
        QVERIFY(slim.texts.size() <= kMaxEntries);
        menulayout::setLayout(Layout::Resolve);
    }
};

QTEST_MAIN(TestMenuLayout)
#include "test_menulayout.moc"
