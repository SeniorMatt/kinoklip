// SPDX-License-Identifier: GPL-3.0-or-later
// In-app help (src/ui/help): guide index parsing and resolution, F1, "?" buttons, "How to use"
// entries and the Help > Guide dialog. The URL opener is replaced: no browser opens.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/ColorPage.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/VideoEffectKeys.h"
#include "ui/color/ColorBrowsers.h"
#include "ui/color/ColorNodeEditor.h"
#include "ui/color/ColorPalettes.h"
#include "ui/color/ColorWidgets.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/effects/EffectActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/help/Guide.h"
#include "ui/help/GuideDialog.h"
#include "ui/help/HelpController.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/scopes/ScopeView.h"
#include "ui/timeline/TimelinePanel.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTest>
#include <QTreeWidget>
#include <QUrl>

#include <memory>

using namespace kk;

namespace {

const TrackRef kV1 {TrackType::Video, 0};
const QString kBase = QStringLiteral("https://example.org/guide/");

/// A guide index like the real one, with one article per context kind.
const char kTestIndex[]
    = "{\n"
      "  'formatVersion': 1,\n"
      "  'base': 'https://example.org/guide/',\n"
      "  'start': 'readme.md',\n"
      "  'articles': [\n"
      "    {'id': 'getting-started', 'title': 'Getting started', 'file': 'getting-started.md',\n"
      "     'summary': 'First steps.', 'keywords': ['tour'], 'contexts': ['page:edit', "
      "'page:media']},\n"
      "    {'id': 'inspector-basics', 'title': 'The Inspector', 'file': 'inspector.md',\n"
      "     'summary': 'Change the properties of a clip.', 'keywords': ['parameters'],\n"
      "     'contexts': ['panel:inspector']},\n"
      "    {'id': 'change-speed', 'title': 'Change the speed of a clip', 'file': 'speed.md',\n"
      "     'summary': 'Slow motion and fast forward.', 'keywords': ['retime'],\n"
      "     'contexts': ['action:clip.change_speed', 'action:help.about']},\n"
      "    {'id': 'remove-a-green-screen', 'title': 'Remove a green screen',\n"
      "     'file': 'remove-a-green-screen.md',\n"
      "     'summary': 'Key out a green or blue background with Chroma Key.',\n"
      "     'keywords': ['green screen', 'chroma key', 'blue screen', 'keying'],\n"
      "     'contexts': ['effect:chroma_key', 'effect:hsl_key']},\n"
      "    {'id': 'inspector-again', 'title': 'Inspector (second)', 'file': 'inspector2.md',\n"
      "     'summary': 'Listed later, never wins panel:inspector.', 'contexts': "
      "['panel:inspector']},\n"
      "    {'id': 'export-for-youtube', 'title': 'Export for YouTube', 'file': "
      "'export-for-youtube.md',\n"
      "     'summary': 'Render for an upload.', 'keywords': ['render', 'deliver'],\n"
      "     'contexts': ['page:deliver']}\n"
      "  ]\n"
      "}\n";

/// JSON written with single quotes (moc does not read raw string literals).
QByteArray json(const char* text)
{
    return QByteArray(text).replace('\'', '"');
}

GuideIndex testIndex()
{
    QString error;
    GuideIndex index = GuideIndex::parse(json(kTestIndex), &error);
    Q_ASSERT(index.isValid());
    return index;
}

QUrl articleUrl(const char* file)
{
    return QUrl(kBase + QLatin1String(file));
}

/// Everything main() creates, plus a recording URL opener.
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QList<QUrl> opened;

    explicit Fixture(const GuideIndex& index = testIndex())
    {
        guide::setIndexForTesting(index);
        guide::setUrlOpenerForTesting([this](const QUrl& url) {
            opened.append(url);
            return true;
        });
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }
    ~Fixture()
    {
        window.reset();
        guide::setUrlOpenerForTesting({});
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }

    /// Shows the window active, with the pointer outside it (no hover context).
    void showActive()
    {
        QCursor::setPos(-5000, -5000);
        window->resize(1600, 1000);
        window->show();
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window.get()));
    }

    /// A clip with Chroma Key and Gaussian Blur, selected (the Inspector shows it).
    quint64 addKeyedClip()
    {
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.name = QStringLiteral("Keyed");
        clip.start = 0;
        clip.duration = 100;
        project.beginEdit(QStringLiteral("Add Clip"));
        const quint64 id = editops::overwriteClip(project.timeline()->mutableData(), kV1, clip);
        project.endEdit();
        effectactions::applyEffect(project, {id}, QLatin1String(fxid::ChromaKey));
        effectactions::applyEffect(project, {id}, QStringLiteral("gaussian_blur"));
        selection.selectClip(id);
        QCoreApplication::processEvents();
        return id;
    }
};

ParamGroupBox* groupWithContext(InspectorPanel* inspector, const QString& key)
{
    for (ParamGroupBox* g : inspector->groups()) {
        if (guide::helpContext(g) == key)
            return g;
    }
    return nullptr;
}

} // namespace

class TestInAppHelp : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_inapphelp"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void cleanup()
    {
        guide::setUrlOpenerForTesting({});
        guide::reloadBundledIndex();
    }

    // ---- GuideIndex ------------------------------------------------------------------

    void parsesTheContractFormat()
    {
        const GuideIndex index = testIndex();
        QVERIFY(index.isValid());
        QCOMPARE(index.articles().size(), 6);
        const GuideArticle* green = index.article(QStringLiteral("remove-a-green-screen"));
        QVERIFY(green);
        QCOMPARE(green->title, QStringLiteral("Remove a green screen"));
        QCOMPARE(green->keywords.size(), 4);
        QCOMPARE(green->contexts,
            QStringList({QStringLiteral("effect:chroma_key"), QStringLiteral("effect:hsl_key")}));
        QCOMPARE(index.url(*green), articleUrl("remove-a-green-screen.md"));
        QCOMPARE(index.startUrl(), articleUrl("readme.md"));
        QCOMPARE(index.articleFor(QStringLiteral("effect:hsl_key")), green);
    }

    void badIndexesFallBackToTheStartPage()
    {
        const QUrl builtInStart(
            QLatin1String(GuideIndex::kDefaultBase) + QLatin1String("readme.md"));
        for (const QByteArray& text : {QByteArray(), QByteArray("{not json"), QByteArray("[1, 2]"),
                 json("{'formatVersion': 2, 'base': 'https://example.org/', 'articles': [\n"
                      "                    {'id': 'a', 'title': 'A', 'file': 'a.md', 'contexts': "
                      "['page:edit']}]}\n"),
                 json("{'formatVersion': '1', 'articles': []}\n")}) {
            QString error;
            const GuideIndex index = GuideIndex::parse(text, &error);
            QVERIFY2(!index.isValid(), text.constData());
            QVERIFY(!error.isEmpty());
            QVERIFY(index.articles().isEmpty());
            QVERIFY(!index.resolve({QStringLiteral("page:edit")}));
            QCOMPARE(index.startUrl(), builtInStart);
        }
        // Bad entries are skipped, a bad base falls back to the built-in one.
        const GuideIndex partial = GuideIndex::parse(
            json("{'formatVersion': 1,\n"
                 "            'base': 'javascript:alert(1)', 'articles': [\n"
                 "              {'id': 'ok', 'title': 'OK', 'file': 'ok.md', 'contexts': "
                 "['page:edit', 3]},\n"
                 "              {'id': 'up', 'title': 'Up', 'file': '../secret.md'},\n"
                 "              {'id': 'abs', 'title': 'Abs', 'file': 'https://evil.example/x'},\n"
                 "              {'title': 'No id', 'file': 'x.md'},\n"
                 "              {'id': 'ok', 'title': 'Duplicate', 'file': 'dup.md'},\n"
                 "              'not an object']}\n"));
        QVERIFY(partial.isValid());
        QCOMPARE(partial.articles().size(), 1);
        QCOMPARE(partial.articles()[0].contexts, QStringList {QStringLiteral("page:edit")});
        QCOMPARE(partial.url(partial.articles()[0]),
            QUrl(QLatin1String(GuideIndex::kDefaultBase) + QLatin1String("ok.md")));
    }

    void resolutionOrder()
    {
        const GuideIndex index = testIndex();
        auto id = [&index](const QStringList& contexts) {
            const GuideArticle* a = index.resolve(contexts);
            return a ? a->id : QString();
        };
        const QString page = QStringLiteral("page:edit");
        const QString panel = QStringLiteral("panel:inspector");
        const QString effect = QStringLiteral("effect:chroma_key");
        const QString action = QStringLiteral("action:clip.change_speed");
        // effect > action > panel > page, whatever order the caller lists them in.
        QCOMPARE(id({page, panel, action, effect}), QStringLiteral("remove-a-green-screen"));
        QCOMPARE(id({page, panel, action}), QStringLiteral("change-speed"));
        QCOMPARE(id({page, panel}), QStringLiteral("inspector-basics")); // first in the index
        QCOMPARE(id({page}), QStringLiteral("getting-started"));
        // A more specific key without an article falls through to the next kind.
        QCOMPARE(id({QStringLiteral("effect:gaussian_blur"), panel, page}),
            QStringLiteral("inspector-basics"));
        QCOMPARE(id({QStringLiteral("effect:gaussian_blur"), QStringLiteral("panel:timeline"),
                     QStringLiteral("page:deliver")}),
            QStringLiteral("export-for-youtube"));
        // Nothing matching: the caller opens the start page.
        QVERIFY(id({QStringLiteral("page:color")}).isEmpty());
        QVERIFY(id({}).isEmpty());
    }

    void searchLooksAtTitleSummaryAndKeywords()
    {
        const GuideIndex index = testIndex();
        auto ids = [&index](const QString& text) {
            QStringList out;
            for (const GuideArticle* a : index.search(text))
                out.append(a->id);
            return out;
        };
        QCOMPARE(ids(QString()).size(), 6);
        QCOMPARE(
            ids(QStringLiteral("GREEN")), QStringList {QStringLiteral("remove-a-green-screen")});
        QCOMPARE(ids(QStringLiteral("keying blue")),
            QStringList {QStringLiteral("remove-a-green-screen")}); // keywords, every word
        QCOMPARE(ids(QStringLiteral("slow motion")), QStringList {QStringLiteral("change-speed")});
        QVERIFY(ids(QStringLiteral("green render")).isEmpty());
    }

    void bundledIndexIsReadable()
    {
        guide::reloadBundledIndex();
        const GuideIndex& index = guide::index();
        QVERIFY(index.isValid());
        QVERIFY(!index.articles().isEmpty());
        QVERIFY(index.startUrl().isValid());
        for (const GuideArticle& a : index.articles()) {
            QVERIFY2(a.file.endsWith(QLatin1String(".md")), qPrintable(a.id));
            QVERIFY(index.url(a).scheme() == QLatin1String("https"));
        }
    }

    /// Every context of the bundled guide index names something the app has: a registered
    /// command, an effect of the EffectRegistry, a page or a panel key (a typo would make F1
    /// silently fall back to another article).
    void bundledContextsExist()
    {
        Fixture f; // registers the commands of the main window
        guide::reloadBundledIndex();
        const QStringList pages {QStringLiteral("media"), QStringLiteral("cut"),
            QStringLiteral("edit"), QStringLiteral("compositing"), QStringLiteral("color"),
            QStringLiteral("audio"), QStringLiteral("deliver")};
        const QStringList panels {QStringLiteral("media_pool"), QStringLiteral("effects"),
            QStringLiteral("edit_index"), QStringLiteral("inspector"), QStringLiteral("keyframes"),
            QStringLiteral("mixer"), QStringLiteral("metadata"), QStringLiteral("timeline"),
            QStringLiteral("viewer"), QStringLiteral("render_settings"),
            QStringLiteral("render_queue"), QStringLiteral("color_gallery"),
            QStringLiteral("color_nodes"), QStringLiteral("color_scopes"),
            QStringLiteral("color_primaries"), QStringLiteral("color_curves"),
            QStringLiteral("color_qualifier"), QStringLiteral("color_windows"),
            QStringLiteral("color_luts"), QStringLiteral("color_clips")};
        int checked = 0;
        for (const GuideArticle& a : guide::index().articles()) {
            for (const QString& context : a.contexts) {
                const QString kind = context.section(QLatin1Char(':'), 0, 0);
                const QString id = context.section(QLatin1Char(':'), 1);
                bool known = false;
                if (kind == QLatin1String("action"))
                    known = f.actions.contains(id);
                else if (kind == QLatin1String("effect"))
                    known = EffectRegistry::instance().find(id) != nullptr;
                else if (kind == QLatin1String("page"))
                    known = pages.contains(id);
                else if (kind == QLatin1String("panel"))
                    known = panels.contains(id);
                QVERIFY2(known, qPrintable(a.id + QLatin1String(": ") + context));
                ++checked;
            }
        }
        QVERIFY(checked > 0);
    }

    // ---- F1 ---------------------------------------------------------------------------

    void f1OnAChromaKeyInTheInspectorOpensTheGreenScreenArticle()
    {
        Fixture f;
        f.showActive();
        QVERIFY(f.window->setCurrentPage(QStringLiteral("edit")));
        f.window->editPage()->setPanelVisible(PanelId::Inspector, true);
        f.addKeyedClip();
        InspectorPanel* inspector = f.window->editPage()->inspector();
        ParamGroupBox* key = groupWithContext(inspector, QStringLiteral("effect:chroma_key"));
        QVERIFY(key);
        // Show the Inspector tab holding the section and focus one of its fields.
        QTabWidget* tabs = inspector->tabs();
        for (int i = 0; i < tabs->count(); ++i) {
            if (tabs->widget(i)->isAncestorOf(key))
                tabs->setCurrentIndex(i);
        }
        QCoreApplication::processEvents();
        QWidget* field = nullptr;
        for (QWidget* w : key->findChildren<QWidget*>()) {
            if (w->focusPolicy() != Qt::NoFocus && w->isVisible()) {
                field = w;
                break;
            }
        }
        QVERIFY(field);
        field->setFocus();
        QTRY_COMPARE(QApplication::focusWidget(), field);

        QVERIFY(f.window->helpController()->currentContexts().contains(
            QStringLiteral("effect:chroma_key")));
        f.action("help.guide_context")->trigger();
        QCOMPARE(f.opened, QList<QUrl> {articleUrl("remove-a-green-screen.md")});

        // The pressed key itself: F1 is the default of help.guide_context.
        QCOMPARE(f.actions.shortcuts(QStringLiteral("help.guide_context")),
            QList<QKeySequence> {QKeySequence(Qt::Key_F1)});
        QTest::keyClick(field, Qt::Key_F1);
        QCOMPARE(f.opened.size(), 2);
        QCOMPARE(f.opened.last(), articleUrl("remove-a-green-screen.md"));

        // The Inspector's other sections fall back to its panel article.
        QCOMPARE(f.window->helpController()->urlFor(f.window->helpController()->contexts(
                     nullptr, groupWithContext(inspector, QStringLiteral("effect:gaussian_blur")))),
            articleUrl("inspector.md"));
    }

    void f1OnTheDeliverPageOpensTheExportArticle()
    {
        Fixture f;
        f.showActive();
        QVERIFY(f.window->setCurrentPage(QStringLiteral("deliver")));
        QCoreApplication::processEvents();
        f.action("help.guide_context")->trigger();
        QCOMPARE(f.opened, QList<QUrl> {articleUrl("export-for-youtube.md")});

        // A page without an article opens the start page.
        QVERIFY(f.window->setCurrentPage(QStringLiteral("audio")));
        QCoreApplication::processEvents();
        f.action("help.guide_context")->trigger();
        QCOMPARE(f.opened.last(), articleUrl("readme.md"));
    }

    /// The Color page's panels and palettes lead to the guide's three Color page articles.
    void f1OnTheColorPageOpensTheColorArticles()
    {
        guide::reloadBundledIndex();
        const GuideIndex bundled = guide::index();
        Fixture f(bundled);
        f.showActive();
        QVERIFY(f.window->setCurrentPage(QStringLiteral("color")));
        QCoreApplication::processEvents();
        ColorPage* page = f.window->colorPage();
        HelpController* help = f.window->helpController();
        auto fileFor
            = [help](QWidget* w) { return help->urlFor(help->contexts(nullptr, w)).fileName(); };
        const QString basics = QStringLiteral("colour-correct-a-clip.md");
        const QString object = QStringLiteral("change-the-colour-of-one-object.md");
        QCOMPARE(fileFor(page->primaries()), basics);
        QCOMPARE(fileFor(page->curves()), basics);
        QCOMPARE(fileFor(page->gallery()), basics);
        QCOMPARE(fileFor(page->clipStrip()), basics);
        QCOMPARE(fileFor(page->scopes()), basics);
        QCOMPARE(fileFor(page->qualifier()), object);
        QCOMPARE(fileFor(page->windows()), object);
        QCOMPARE(fileFor(page->nodeEditor()), object);
        QCOMPARE(fileFor(page->luts()), QStringLiteral("use-a-lut.md"));
        // Elsewhere on the page: the basics.
        f.action("help.guide_context")->trigger();
        QCOMPARE(f.opened.last().fileName(), basics);
    }

    void f1WithABrokenIndexOpensTheBuiltInStartPage()
    {
        Fixture f(GuideIndex::parse("{broken"));
        f.action("help.guide_context")->trigger();
        QCOMPARE(f.opened,
            QList<QUrl> {
                QUrl(QLatin1String(GuideIndex::kDefaultBase) + QLatin1String("readme.md"))});
    }

    void f1InAnOpenMenuUsesTheHighlightedCommand()
    {
        Fixture f;
        f.showActive();
        auto* help = f.window->findChild<QMenu*>(QStringLiteral("menu:help"));
        QVERIFY(help);
        help->popup(f.window->mapToGlobal(QPoint(40, 40)));
        QVERIFY(QTest::qWaitForWindowExposed(help));
        help->setActiveAction(f.action("help.about"));
        QTest::keyClick(help, Qt::Key_F1);
        QCOMPARE(f.opened, QList<QUrl> {articleUrl("speed.md")}); // lists action:help.about
        QTRY_VERIFY(!help->isVisible());
    }

    // ---- "?" buttons and "How to use" -------------------------------------------------

    void helpButtonsOnlyWhereAnArticleExists()
    {
        Fixture f;
        f.addKeyedClip();
        InspectorPanel* inspector = f.window->editPage()->inspector();
        auto* panelButton = inspector->titleBar()->findChild<guide::HelpButton*>();
        QVERIFY(panelButton);
        QCOMPARE(panelButton->articleId(), QStringLiteral("inspector-basics"));
        QCOMPARE(panelButton->size(), QSize(16, 16));
        QVERIFY(!f.window->editPage()->timeline()->titleBar()->findChild<guide::HelpButton*>());

        ParamGroupBox* key = groupWithContext(inspector, QStringLiteral("effect:chroma_key"));
        ParamGroupBox* blur = groupWithContext(inspector, QStringLiteral("effect:gaussian_blur"));
        QVERIFY(key && blur);
        auto* keyButton = key->findChild<guide::HelpButton*>();
        QVERIFY(keyButton);
        QVERIFY(!blur->findChild<guide::HelpButton*>());

        keyButton->click();
        panelButton->click();
        QCOMPARE(f.opened,
            QList<QUrl>({articleUrl("remove-a-green-screen.md"), articleUrl("inspector.md")}));

        // Without a usable index there is no "?" anywhere.
        Fixture broken(GuideIndex::parse("{broken"));
        broken.addKeyedClip();
        QVERIFY(broken.window->findChildren<guide::HelpButton*>().isEmpty());
    }

    void rightClickOffersHowToUse()
    {
        Fixture f;
        EffectsLibraryPanel library(f.ctx);
        std::unique_ptr<QMenu> menu(library.createContextMenu(QLatin1String(fxid::ChromaKey)));
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 1);
        QCOMPARE(menu->actions()[0]->text(), QStringLiteral("How to use Chroma Key…"));
        menu->actions()[0]->trigger();
        QCOMPARE(f.opened, QList<QUrl> {articleUrl("remove-a-green-screen.md")});
        QVERIFY(!library.createContextMenu(QStringLiteral("gaussian_blur")));

        // The selected entry is the library's F1 context.
        QTreeWidgetItem* chroma = nullptr;
        QTreeWidgetItemIterator it(library.tree());
        for (; *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String(fxid::ChromaKey))
                chroma = *it;
        }
        QVERIFY(chroma);
        library.tree()->setCurrentItem(chroma);
        QCOMPARE(guide::contextsAt(library.tree()),
            QStringList({QStringLiteral("effect:chroma_key"), QStringLiteral("panel:effects")}));

        // The Inspector section header's menu.
        f.addKeyedClip();
        ParamGroupBox* key = groupWithContext(
            f.window->editPage()->inspector(), QStringLiteral("effect:chroma_key"));
        QVERIFY(key);
        QStringList texts;
        for (const QAction* a : key->headerMenuActions())
            texts.append(a->text());
        QCOMPARE(texts, QStringList {QStringLiteral("How to use Chroma Key…")});
        ParamGroupBox* blur = groupWithContext(
            f.window->editPage()->inspector(), QStringLiteral("effect:gaussian_blur"));
        QVERIFY(blur && blur->headerMenuActions().isEmpty());
    }

    // ---- Help menu ----------------------------------------------------------------------

    void guideDialogFiltersAndOpens()
    {
        Fixture f;
        f.action("help.guide")->trigger();
        auto* dialog = f.window->findChild<GuideDialog*>();
        QVERIFY(dialog);
        QVERIFY(dialog->isVisible());
        const QStringList all = dialog->visibleArticleIds();
        QCOMPARE(all.size(), 7);
        QVERIFY(all.first().isEmpty()); // "Start page" on top
        QCOMPARE(dialog->list()->item(0)->text(), QStringLiteral("Start page"));

        QTest::keyClicks(dialog->searchField(), QStringLiteral("green"));
        QCOMPARE(dialog->visibleArticleIds(),
            QStringList({QString(), QStringLiteral("remove-a-green-screen")}));
        QTest::keyClick(dialog->searchField(), Qt::Key_Return);
        QCOMPARE(f.opened, QList<QUrl> {articleUrl("remove-a-green-screen.md")});
        QTRY_VERIFY(!f.window->findChild<GuideDialog*>());

        // A double-click (item activation) on the start page.
        GuideDialog* again = f.window->helpController()->showGuideDialog();
        QCOMPARE(again->visibleArticleIds().size(), 7);
        emit again->list()->itemActivated(again->list()->item(0));
        QCOMPARE(f.opened.last(), articleUrl("readme.md"));
    }

    void gettingStartedFollowsTheIndex()
    {
        {
            Fixture f;
            QAction* start = f.action("help.getting_started");
            QVERIFY(start->isVisible());
            start->trigger();
            QCOMPARE(f.opened, QList<QUrl> {articleUrl("getting-started.md")});
        }
        {
            Fixture f(GuideIndex::parse(json("{'formatVersion': 1, 'articles': []}\n")));
            QVERIFY(!f.action("help.getting_started")->isVisible());
        }
    }
};

QTEST_MAIN(TestInAppHelp)
#include "test_inapphelp.moc"
