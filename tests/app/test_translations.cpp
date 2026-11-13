// SPDX-License-Identifier: GPL-3.0-or-later
// Pseudo-locale test for the interface translations (spec 100): a QTranslator that wraps every
// source string in "[!! ... !!]" is installed, the main window, every page, the menus and the
// main dialogs are built, and every visible text (labels, buttons, actions, group boxes, tabs,
// combo items, tree and list items, header labels, tool tips, window titles) is collected. A
// text that is not wrapped but looks like English was never passed through tr(): it would stay
// English in every language. Intentional exceptions (brand names, codec names, units) are in
// tests/app/translation_allowlist.txt, one per line: an exact text, or "re:<regular expression>".
// Also checks the language list, --lang / KK_LANG resolution and that the shipped .qm files load.
#include "app/MainWindow.h"
#include "app/SettingsTransferDialog.h"
#include "app/Theme.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Translations.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Page.h"
#include "ui/common/Resources.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/ProjectSettingsDialog.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTest>
#include <QTranslator>
#include <QXmlStreamReader>
#include <QTreeWidget>

#include <memory>

using namespace kk;

namespace {

/// Wraps every translated string: [!! Source !!].
class PseudoTranslator : public QTranslator
{
public:
    explicit PseudoTranslator(QObject* parent = nullptr)
        : QTranslator(parent)
    {
    }
    bool isEmpty() const override { return false; }
    /// Every text the application asked a translation for (mutable: translate() is const).
    mutable QSet<QString> asked;
    QString translate(const char* context, const char* sourceText, const char* disambiguation,
        int n) const override
    {
        Q_UNUSED(context);
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (!sourceText || !*sourceText)
            return {};
        asked.insert(QString::fromUtf8(sourceText));
        return QStringLiteral("[!! ") + QString::fromUtf8(sourceText) + QStringLiteral(" !!]");
    }
};

struct Allowlist
{
    QSet<QString> exact;
    QList<QRegularExpression> patterns;

    void load(const QString& path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return;
        for (const QByteArray& raw : f.readAll().split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            if (line.startsWith(QLatin1String("re:")))
                patterns.append(QRegularExpression(QLatin1Char('^') + line.mid(3) + QLatin1Char('$')));
            else
                exact.insert(line);
        }
    }
    bool allows(const QString& text) const
    {
        if (exact.contains(text))
            return true;
        for (const QRegularExpression& re : patterns)
            if (re.match(text).hasMatch())
                return true;
        return false;
    }
};

/// Collects the visible strings of everything under @p roots.
class Collector
{
public:
    QMap<QString, QString> found; ///< text -> where (first widget seen)

    void add(const QString& rawText, const QString& where)
    {
        QString text = rawText;
        text.remove(QLatin1Char('&'));
        text = text.trimmed();
        if (text.isEmpty() || found.contains(text))
            return;
        found.insert(text, where);
    }

    void action(QAction* a, const QString& where, QSet<QMenu*>& seen)
    {
        if (a->isSeparator())
            return;
        add(a->text(), where + QStringLiteral(" action"));
        add(a->toolTip() == a->text() ? QString() : a->toolTip(), where + QStringLiteral(" action tip"));
        if (QMenu* m = a->menu(); m && !seen.contains(m)) {
            seen.insert(m);
            menu(m, where, seen);
        }
    }
    void menu(QMenu* m, const QString& where, QSet<QMenu*>& seen)
    {
        add(m->title(), where + QStringLiteral(" menu"));
        for (QAction* a : m->actions())
            action(a, where + QLatin1Char('/') + m->title(), seen);
    }

    void tree(QTreeWidgetItem* item, const QString& where)
    {
        for (int c = 0; c < item->columnCount(); ++c)
            add(item->text(c), where + QStringLiteral(" tree item"));
        add(item->toolTip(0), where + QStringLiteral(" tree tip"));
        for (int i = 0; i < item->childCount(); ++i)
            tree(item->child(i), where);
    }

    void widgets(const QList<QWidget*>& all)
    {
        QSet<QMenu*> seenMenus;
        // Lists of things that are data, not wording: Vim key sequences, command ids (their
        // tool tips), hardware names, the LUT folder.
        static const QSet<QString> dataWidgets {QStringLiteral("VimKeysTree"),
            QStringLiteral("KeyboardCommands"), QStringLiteral("HwInfo"), QStringLiteral("LutTree")};
        for (QWidget* w : all) {
            if (dataWidgets.contains(w->objectName()))
                continue;
            const QString where = QString::fromLatin1(w->metaObject()->className())
                + QLatin1Char('#') + w->objectName();
            if (w->isWindow())
                add(w->windowTitle(), where + QStringLiteral(" window title"));
            add(w->toolTip(), where + QStringLiteral(" tooltip"));
            if (auto* l = qobject_cast<QLabel*>(w)) {
                if (l->textFormat() != Qt::PlainText || !l->text().contains(QLatin1Char('<')))
                    add(l->text(), where);
                else
                    add(l->text(), where);
            } else if (auto* b = qobject_cast<QAbstractButton*>(w)) {
                add(b->text(), where);
            } else if (auto* g = qobject_cast<QGroupBox*>(w)) {
                add(g->title(), where);
            } else if (auto* t = qobject_cast<QTabBar*>(w)) {
                for (int i = 0; i < t->count(); ++i)
                    add(t->tabText(i), where + QStringLiteral(" tab"));
            } else if (auto* c = qobject_cast<QComboBox*>(w)) {
                for (int i = 0; i < c->count(); ++i)
                    add(c->itemText(i), where + QStringLiteral(" item"));
            } else if (auto* tw = qobject_cast<QTreeWidget*>(w)) {
                if (QTreeWidgetItem* h = tw->headerItem())
                    for (int i = 0; i < h->columnCount(); ++i)
                        add(h->text(i), where + QStringLiteral(" header"));
                for (int i = 0; i < tw->topLevelItemCount(); ++i)
                    tree(tw->topLevelItem(i), where);
            } else if (auto* lw = qobject_cast<QListWidget*>(w)) {
                for (int i = 0; i < lw->count(); ++i)
                    add(lw->item(i)->text(), where + QStringLiteral(" list item"));
            } else if (auto* mb = qobject_cast<QMenuBar*>(w)) {
                for (QAction* a : mb->actions())
                    action(a, where, seenMenus);
            } else if (auto* mn = qobject_cast<QMenu*>(w)) {
                if (!seenMenus.contains(mn)) {
                    seenMenus.insert(mn);
                    menu(mn, where, seenMenus);
                }
            }
            if (auto* view = qobject_cast<QAbstractItemView*>(w); view && view->model()) {
                QAbstractItemModel* m = view->model();
                for (int c = 0; c < m->columnCount(); ++c)
                    add(m->headerData(c, Qt::Horizontal).toString(), where + QStringLiteral(" model header"));
            }
            for (QAction* a : w->actions())
                action(a, where, seenMenus);
        }
    }
};

/// True if @p text is an English phrase: a run of three or more ASCII letters, not wrapped.
bool looksUntranslated(const QString& text)
{
    if (text.contains(QLatin1String("[!!")))
        return false;
    static const QRegularExpression word(QStringLiteral("[A-Za-z]{3,}"));
    return word.match(text).hasMatch();
}

} // namespace

class TestTranslations : public QObject
{
    Q_OBJECT

    std::unique_ptr<PseudoTranslator> m_pseudo;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_translations"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void languageList()
    {
        QStringList codes;
        for (const i18n::Language& l : i18n::languages()) {
            codes << l.code;
            QVERIFY(!l.name.isEmpty());
        }
        QCOMPARE(codes, (QStringList {QStringLiteral("en"), QStringLiteral("de"),
                            QStringLiteral("es"), QStringLiteral("ru"), QStringLiteral("uk")}));
    }

    void resolveLanguageOrder()
    {
        qunsetenv("KK_LANG");
        QSettings().setValue(QLatin1String(i18n::kLanguageKey), QStringLiteral("de"));
        QCOMPARE(i18n::resolveLanguage(), QStringLiteral("de"));
        qputenv("KK_LANG", "ru");
        QCOMPARE(i18n::resolveLanguage(), QStringLiteral("ru"));
        QCOMPARE(i18n::resolveLanguage(QStringLiteral("uk")), QStringLiteral("uk")); // --lang first
        QCOMPARE(i18n::resolveLanguage(QStringLiteral("es_ES")), QStringLiteral("es"));
        qputenv("KK_LANG", "xx");
        QSettings().setValue(QLatin1String(i18n::kLanguageKey), QStringLiteral("system"));
        QVERIFY(!i18n::resolveLanguage().isEmpty()); // unknown: follows the desktop, else English
        qunsetenv("KK_LANG");
        QSettings().remove(QLatin1String(i18n::kLanguageKey));
    }

#ifdef KK_HAVE_TRANSLATIONS
    void shippedFilesLoad()
    {
        for (const char* code : {"ru", "de", "uk", "es"}) {
            QTranslator t;
            QVERIFY2(t.load(QStringLiteral(":/translations/kinoklip_%1.qm")
                                .arg(QLatin1String(code))),
                code);
            QVERIFY2(!t.isEmpty(), code);
        }
    }

    /// Every language really changes a menu text of the application.
    void shippedTranslationsApply()
    {
        for (const char* code : {"ru", "de", "uk", "es"}) {
            i18n::installLanguage(QLatin1String(code));
            const QString text = QCoreApplication::translate("kk::CommandLine", "Non-linear video editor");
            QVERIFY2(text != QLatin1String("Non-linear video editor"), code);
            // A text whose widget has no Q_OBJECT (lupdate's context differs from the run
            // time one): found through the fallback context.
            QVERIFY2(QCoreApplication::translate("QWidget", "Interface scale")
                    != QLatin1String("Interface scale"),
                code);
        }
        i18n::installLanguage(QStringLiteral("en"));
        QCOMPARE(QCoreApplication::translate("kk::CommandLine", "Non-linear video editor"),
            QStringLiteral("Non-linear video editor"));
    }
#endif

    void everyVisibleTextIsTranslatable()
    {
        m_pseudo = std::make_unique<PseudoTranslator>();
        QCoreApplication::installTranslator(m_pseudo.get());

        Project project;
        SelectionModel selection;
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        theme::registerActions(actions);
        SettingsPageRegistry settingsPages;
        registerStandardSettingsPages(settingsPages, &actions);
        theme::registerSettingsPage(settingsPages, actions);
        registerSettingsTransferPage(settingsPages);
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.settingsPages = &settingsPages;
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        MainWindow window(ctx, options);
        window.resize(1920, 1080);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        Collector collector;
        for (const QString& id : {QStringLiteral("media"), QStringLiteral("cut"),
                 QStringLiteral("edit"), QStringLiteral("compositing"), QStringLiteral("color"),
                 QStringLiteral("audio"), QStringLiteral("deliver")}) {
            if (!window.setCurrentPage(id))
                continue;
            QTest::qWait(150);
            collector.widgets(window.findChildren<QWidget*>());
        }
        // The command list as the Keyboard dialog shows it.
        for (const QString& id : actions.ids())
            if (QAction* a = actions.action(id))
                collector.add(a->text(), QStringLiteral("command ") + id);

        // The dialogs: every section of Preferences and Project Settings.
        auto sections = [&](QDialog* dialog) {
            dialog->resize(1100, 800);
            dialog->show();
            for (QListWidget* list : dialog->findChildren<QListWidget*>()) {
                for (int row = 0; row < list->count(); ++row) {
                    list->setCurrentRow(row);
                    QTest::qWait(30);
                    collector.widgets(dialog->findChildren<QWidget*>());
                }
            }
            collector.widgets(dialog->findChildren<QWidget*>());
            collector.widgets({dialog});
        };
        {
            PreferencesDialog prefs(&settingsPages, &window);
            sections(&prefs);
            for (QTabBar* tabs : prefs.findChildren<QTabBar*>()) {
                for (int i = 0; i < tabs->count(); ++i) {
                    tabs->setCurrentIndex(i);
                    sections(&prefs);
                }
            }
        }
        {
            ProjectSettingsDialog settings(&project, &window);
            sections(&settings);
        }
        {
            KeyboardCustomizationDialog keyboard(actions, &window);
            sections(&keyboard);
        }

        Allowlist allow;
        allow.load(QFileInfo(QStringLiteral(__FILE__)).absolutePath() + QStringLiteral("/translation_allowlist.txt"));
        QStringList bad;
        for (auto it = collector.found.cbegin(); it != collector.found.cend(); ++it) {
            if (looksUntranslated(it.key()) && !allow.allows(it.key()))
                bad << QStringLiteral("%1   [%2]").arg(it.key(), it.value());
        }
        // Every text the code asked a translation for must be in the .ts files (lupdate did not
        // see it otherwise, e.g. a helper function with an unusual name): then it can be
        // translated at all.
        QSet<QString> inTs;
        {
            QFile ts(QFileInfo(QStringLiteral(__FILE__)).absolutePath()
                + QStringLiteral("/../../resources/translations/kinoklip_de.ts"));
            QVERIFY(ts.open(QIODevice::ReadOnly));
            QXmlStreamReader xml(&ts);
            while (!xml.atEnd()) {
                if (xml.readNext() == QXmlStreamReader::StartElement
                    && xml.name() == QLatin1String("source"))
                    inTs.insert(xml.readElementText());
            }
        }
        QStringList notExtracted;
        for (const QString& text : std::as_const(m_pseudo->asked))
            if (!inTs.contains(text) && !allow.allows(text))
                notExtracted << text;
        notExtracted.sort();
        QCoreApplication::removeTranslator(m_pseudo.get());
        if (!notExtracted.isEmpty() && !qEnvironmentVariable("KK_TRANSLATION_REPORT").isEmpty()) {
            QFile f(qEnvironmentVariable("KK_TRANSLATION_REPORT") + QStringLiteral(".missing"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(notExtracted.join(QLatin1Char('\n')).toUtf8());
        }
        QVERIFY2(notExtracted.isEmpty(),
            qPrintable(QStringLiteral("%1 texts are translated at run time but lupdate did not "
                                      "find them (wrapper function without tr()/QT_TRANSLATE_NOOP "
                                      "visible to lupdate):\n%2")
                           .arg(notExtracted.size())
                           .arg(notExtracted.join(QLatin1Char('\n')))));
        if (!bad.isEmpty()) {
            const QString report = bad.join(QLatin1Char('\n'));
            const QString out = qEnvironmentVariable("KK_TRANSLATION_REPORT");
            if (!out.isEmpty()) {
                QFile f(out);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    f.write(report.toUtf8());
            }
            QFAIL(qPrintable(QStringLiteral("%1 visible texts bypass tr() (add them to tr(), or to "
                                            "tests/app/translation_allowlist.txt):\n%2")
                                 .arg(bad.size())
                                 .arg(report)));
        }
    }
};

QTEST_MAIN(TestTranslations)
#include "test_translations.moc"
