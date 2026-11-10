// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/MenuReference.h"

#include "app/MainWindow.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
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
            m_seen.insert(action);
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
    Row rowFor(const QAction* action, const QString& prefix) const
    {
        Row row;
        row.entry = prefix + plain(action->text());
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

void appendTable(QString& out, const Table& table, Reference& ref)
{
    out += QStringLiteral("\n## %1\n\n").arg(cell(table.title));
    out += QStringLiteral("| Entry | Shortcut (default Resolve keymap) | What it does |\n");
    out += QStringLiteral("|---|---|---|\n");
    for (const Row& row : table.rows) {
        out += QStringLiteral("| %1 | %2 | %3 |\n")
                   .arg(cell(row.entry), cell(row.shortcut), cell(row.what));
        ++ref.entries;
        if (row.what.isEmpty())
            ref.undescribed.append(table.title + QStringLiteral(" > ") + row.entry);
    }
    ++ref.menus;
}

} // namespace

Reference build(MainWindow& window)
{
    Reference ref;
    const ActionRegistry& registry = *window.context().actions;
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
        walker.walkMenu(menu, table.title, QString(), table);
        tables.append(table);
    }
    Table extra = walker.unlisted(QStringLiteral("Keyboard-only commands"));
    if (!extra.rows.isEmpty())
        tables.append(extra);

    QString body;
    for (const Table& table : tables)
        appendTable(body, table, ref);

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
        "same commands as these menus and are not listed again.\n");
    ref.markdown += body;
    ref.markdown += panels;
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
