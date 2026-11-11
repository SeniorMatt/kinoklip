// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/MenuReference.h"

#include "app/MainWindow.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/MenuLayouts.h"
#include "ui/common/Page.h"
#include "ui/common/PanelInfo.h"

#include <QAction>
#include <QActionGroup>
#include <QFile>
#include <QHash>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

namespace kk::menureference {

namespace {

/// The pages in the page bar's order.
const QStringList kPageIds {QStringLiteral("media"), QStringLiteral("cut"), QStringLiteral("edit"),
    QStringLiteral("compositing"), QStringLiteral("color"), QStringLiteral("audio"),
    QStringLiteral("deliver")};

struct Row
{
    QString entry;
    QString shortcut;
    QString what;
};

struct Table
{
    QString title;
    QVector<Row> rows;
};

QString plain(QString text)
{
    // "&File" -> "File"; "&&" is a literal ampersand. Ellipses stay: they are part of the name.
    text.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
    text.remove(QLatin1Char('&'));
    text.replace(QLatin1Char('\x01'), QLatin1Char('&'));
    // "Label<Tab>Ctrl+S": the key a Streamlined entry shows is not part of its name.
    if (const qsizetype tab = text.indexOf(QLatin1Char('\t')); tab >= 0)
        text.truncate(tab);
    return text.trimmed();
}

QString cell(QString text)
{
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    return text.simplified();
}

/// The words for an entry: statusTip, else a tool tip somebody wrote (not the generated
/// "Text  (Key)" one).
QString describe(const QAction* action)
{
    if (!action->statusTip().isEmpty())
        return action->statusTip();
    const QString tip = action->toolTip();
    if (!tip.isEmpty() && tip != action->property("kkAutoToolTip").toString()
        && plain(tip) != plain(action->text()))
        return tip;
    return {};
}

class Walker
{
public:
    explicit Walker(const ActionRegistry& registry)
        : m_registry(registry)
    {
        for (const QString& id : registry.ids())
            m_ids.insert(registry.action(id), id);
    }

    void walkMenu(const QMenu* menu, const QString& tableTitle, const QString& prefix, Table& table)
    {
        for (const QAction* action : menu->actions()) {
            if (action->isSeparator())
                continue;
            if (const QMenu* sub = action->menu()) {
                const QString name = prefix + plain(action->text());
                if (sub->property("kkDynamicMenu").toBool()) {
                    for (const QAction* item : sub->actions())
                        m_seen.insert(item);
                    table.rows.append({name, QString(), describe(action)});
                    continue;
                }
                walkMenu(sub, tableTitle, name + QStringLiteral(" > "), table);
                continue;
            }
            m_seen.insert(source(action));
            table.rows.append(rowFor(action, prefix));
        }
    }

    /// Registry actions that no menu showed, in registration order.
    Table unlisted(const QString& title) const
    {
        Table table;
        table.title = title;
        for (const QString& id : m_registry.ids()) {
            const QAction* action = m_registry.action(id);
            if (!m_seen.contains(action))
                table.rows.append(rowFor(action, QString()));
        }
        return table;
    }

private:
    /// The registry command a menu entry stands for (the entry itself unless it is a
    /// Streamlined entry under another name).
    const QAction* source(const QAction* entry) const
    {
        const QString id = menulayout::mirroredId(entry);
        return id.isEmpty() ? entry : m_registry.action(id);
    }

    Row rowFor(const QAction* shown, const QString& prefix) const
    {
        const QAction* action = source(shown);
        Row row;
        row.entry = prefix + plain(shown->text());
        if (action->isCheckable())
            row.entry += (action->actionGroup() && action->actionGroup()->isExclusive())
                ? QStringLiteral(" (choice)")
                : QStringLiteral(" (toggle)");
        if (action->property("kkPlanned").toBool())
            row.entry += QStringLiteral(" (planned)");
        const QString id = m_ids.value(action);
        QStringList keys;
        if (!id.isEmpty()) {
            for (const QKeySequence& key : m_registry.defaultShortcuts(id))
                keys.append(key.toString(QKeySequence::PortableText));
        }
        row.shortcut = keys.join(QStringLiteral(", "));
        row.what = describe(action);
        return row;
    }

    const ActionRegistry& m_registry;
    QHash<const QAction*, QString> m_ids;
    QSet<const QAction*> m_seen;
};

void appendTable(
    QString& out, const Table& table, QStringList& undescribed, int& entries, int& menus)
{
    out += QStringLiteral("\n## %1\n\n").arg(cell(table.title));
    out += QStringLiteral("| Entry | Shortcut (default Resolve keymap) | What it does |\n");
    out += QStringLiteral("|---|---|---|\n");
    for (const Row& row : table.rows) {
        out += QStringLiteral("| %1 | %2 | %3 |\n")
                   .arg(cell(row.entry), cell(row.shortcut), cell(row.what));
        ++entries;
        if (row.what.isEmpty())
            undescribed.append(table.title + QStringLiteral(" > ") + row.entry);
    }
    ++menus;
}

/// The menu bar of the window as tables, then the commands no menu shows.
QVector<Table> menuTables(
    MainWindow& window, const ActionRegistry& registry, const QString& leftover, bool streamlined)
{
    Walker walker(registry);
    QVector<Table> tables;
    for (const QAction* top : window.menuBar()->actions()) {
        const QMenu* menu = top->menu();
        if (!menu)
            continue;
        Table table;
        table.title = plain(top->property("kkTitle").toString().isEmpty()
                ? top->text()
                : top->property("kkTitle").toString());
        if (streamlined)
            table.title = QStringLiteral("Streamlined ") + table.title;
        walker.walkMenu(menu, table.title, QString(), table);
        tables.append(table);
    }
    if (streamlined) {
        // The page menus are on their pages only: written here whichever page is shown.
        for (const char* page : {"cut", "compositing", "color"}) {
            const menulayout::Menu spec = menulayout::pageMenu(QLatin1String(page));
            QMenu menu;
            menulayout::fill(
                &menu, spec.entries, *window.context().actions, QStringLiteral("menu"));
            Table table;
            QString pageName = QLatin1String(page);
            pageName[0] = pageName[0].toUpper();
            table.title = QStringLiteral("Streamlined ") + plain(spec.title) + QStringLiteral(" (")
                + pageName + QStringLiteral(" page)");
            walker.walkMenu(&menu, table.title, QString(), table);
            tables.append(table);
        }
    }
    Table extra = walker.unlisted(leftover);
    if (!extra.rows.isEmpty())
        tables.append(extra);
    return tables;
}

} // namespace

Reference build(MainWindow& window)
{
    Reference ref;
    const ActionRegistry& registry = *window.context().actions;

    const menulayout::Layout original = window.menuLayout();
    window.setMenuLayout(menulayout::Layout::Resolve);
    const QVector<Table> resolveTables
        = menuTables(window, registry, QStringLiteral("Keyboard-only commands"), false);
    window.setMenuLayout(menulayout::Layout::Streamlined);
    const QVector<Table> streamlinedTables = menuTables(
        window, registry, QStringLiteral("Streamlined: commands without a menu entry"), true);
    window.setMenuLayout(original);

    QString body;
    for (const Table& table : resolveTables)
        appendTable(body, table, ref.undescribed, ref.entries, ref.menus);
    QString streamlinedBody;
    for (const Table& table : streamlinedTables)
        appendTable(
            streamlinedBody, table, ref.undescribed, ref.streamlinedEntries, ref.streamlinedMenus);

    // Panels, page by page.
    QString panels = QStringLiteral("\n## Panels\n");
    for (const QString& id : kPageIds) {
        const Page* page = window.page(id);
        if (!page)
            continue;
        QString title = id;
        title[0] = title[0].toUpper();
        panels += QStringLiteral("\n### %1 page\n\n| Panel | What it is |\n|---|---|\n").arg(title);
        // Sorted by name, the same panel shown twice on a page (the Cut page's two source
        // viewers) once: the order of the widgets in the tree says nothing to a reader.
        QVector<QPair<QString, QString>> found;
        for (const QWidget* widget : page->findChildren<QWidget*>()) {
            const QString what = panelInfoDescription(widget);
            if (what.isEmpty())
                continue;
            const QPair<QString, QString> panel {panelInfoName(widget), what};
            if (!found.contains(panel))
                found.append(panel);
        }
        std::sort(found.begin(), found.end(), [](const auto& l, const auto& r) {
            const int byName = l.first.compare(r.first, Qt::CaseInsensitive);
            return byName != 0 ? byName < 0 : l.second < r.second;
        });
        for (const auto& panel : std::as_const(found)) {
            panels += QStringLiteral("| %1 | %2 |\n").arg(cell(panel.first), cell(panel.second));
            ++ref.panels;
        }
    }
    // Panels that exist but say nothing about themselves.
    for (const QString& id : kPageIds) {
        const Page* page = window.page(id);
        if (!page)
            continue;
        for (const QWidget* widget : page->findChildren<QWidget*>()) {
            if (!panelInfoName(widget).isEmpty() && panelInfoDescription(widget).isEmpty())
                ref.undescribed.append(id + QStringLiteral(" > ") + panelInfoName(widget));
        }
    }

    ref.markdown = QStringLiteral("%1 menu entries in %2 menus; %3 panels\n")
                       .arg(ref.entries)
                       .arg(ref.menus)
                       .arg(ref.panels);
    ref.markdown += QStringLiteral(
        "\n# Menu and panel reference\n\n"
        "Written by Kinoklip itself (`kinoklip --menu-reference <file>`) from the tables its menus "
        "are built from and from the description every panel carries, so it cannot go stale: the "
        "test `check_menu_reference` fails when this file differs from what the program writes. "
        "Do not edit it by hand; run `scripts/menu-reference.sh` after adding or renaming a "
        "menu entry or a panel.\n\n"
        "Shortcuts are the defaults of the Resolve keymap (Windows); another keyboard profile or "
        "your own changes (Keyboard Customization) do not show here. Entries are listed in menu "
        "bar order; a submenu entry is written as `Submenu > Entry`. \"(toggle)\" entries switch "
        "on and off, \"(choice)\" entries are one of a group. Menus that list files (Open "
        "Recent, guide presets) are one row. Commands without a menu entry are in the last menu "
        "table. The context menus of the timeline, the media pool and other panels offer the "
        "same commands as these menus and are not listed again. The Resolve layout is listed "
        "first; "
        "the Streamlined layout follows the panels.\n");
    ref.markdown += body;
    ref.markdown += panels;
    ref.markdown += QStringLiteral(
        "\n# Streamlined menu layout\n\n"
        "Preferences > UI Settings > Menu layout offers the menus above (Resolve, the default) or "
        "this arrangement (Streamlined): %1 commands in %2 tables (menus, page menus and the last "
        "table), no menu longer than 12 entries, grouped by task, with plain names. The commands, "
        "their keys and descriptions are the "
        "same in both. The page menu (Cut, Nodes or Grade) is shown on its page only; the tables "
        "below list all three. Commands that are on no menu are in the last table.\n")
                        .arg(ref.streamlinedEntries)
                        .arg(ref.streamlinedMenus);
    ref.markdown += streamlinedBody;
    return ref;
}

bool writeFile(MainWindow& window, const QString& path, QString* error)
{
    const Reference ref = build(window);
    if (!ref.undescribed.isEmpty()) {
        if (error)
            *error = QStringLiteral("no description for: %1")
                         .arg(ref.undescribed.join(QStringLiteral("; ")));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(ref.markdown.toUtf8()) < 0
        || !file.commit()) {
        if (error)
            *error = QStringLiteral("cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

} // namespace kk::menureference
