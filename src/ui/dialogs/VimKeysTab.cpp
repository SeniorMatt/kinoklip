// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/VimKeysTab.h"

#include "ui/common/ActionRegistry.h"
#include "ui/common/VimKeymap.h"
#include "ui/common/VimKeys.h"
#include "ui/common/VimOptions.h"

#include <QComboBox>
#include <QCompleter>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace kk {

namespace {

enum Column {
    KeysColumn,
    ModeColumn,
    CommandColumn,
    DescColumn
};
constexpr int kContextRole = Qt::UserRole + 1;
constexpr int kModesRole = Qt::UserRole + 2;
constexpr int kKeysRole = Qt::UserRole + 3;
constexpr int kOriginRole = Qt::UserRole + 4;
constexpr int kRunRole = Qt::UserRole + 5;

QString contextTitle(const QString& context)
{
    if (context == QLatin1String("leader"))
        return QObject::tr("Leader");
    if (context == QLatin1String("global"))
        return QObject::tr("Global");
    if (context == QLatin1String("timeline"))
        return QObject::tr("Timeline");
    if (context == QLatin1String("viewer"))
        return QObject::tr("Viewer");
    if (context == QLatin1String("list"))
        return QObject::tr("Lists and trees");
    if (context == QLatin1String("panel"))
        return QObject::tr("Panels");
    return context;
}

/// The editor of one row: context, modes, keys (checked as typed), command, description.
class BindingEditor : public QDialog
{
public:
    BindingEditor(ActionRegistry& actions, VimKeymap& keymap, const VimKeysTab::Edit& start,
        bool isNew, QWidget* parent)
        : QDialog(parent)
        , m_actions(actions)
        , m_keymap(keymap)
    {
        setWindowTitle(isNew ? tr("Add Vim Key") : tr("Change Vim Key"));
        m_context = new QComboBox;
        for (const QString& c : VimKeymap::contexts())
            m_context->addItem(contextTitle(c), c);
        m_context->setCurrentIndex(std::max(0, m_context->findData(start.context)));
        m_modes = new QComboBox;
        m_modes->addItem(tr("NORMAL"), QStringLiteral("n"));
        m_modes->addItem(tr("VISUAL"), QStringLiteral("v"));
        m_modes->addItem(tr("NORMAL and VISUAL"), QStringLiteral("nv"));
        m_modes->addItem(tr("After an operator (motions and objects)"), QStringLiteral("o"));
        m_modes->addItem(tr("NORMAL, VISUAL and after an operator"), QStringLiteral("nvo"));
        m_modes->addItem(tr("VISUAL and after an operator"), QStringLiteral("vo"));
        m_modes->setCurrentIndex(std::max(0, m_modes->findData(start.modes)));
        m_keys = new QLineEdit(start.keys);
        m_keys->setObjectName(QStringLiteral("VimKeysEdit"));
        m_keys->setPlaceholderText(tr("gg, <leader>ff, <C-h>"));
        m_command = new QComboBox;
        m_command->setEditable(true);
        m_command->setObjectName(QStringLiteral("VimCommandEdit"));
        QStringList commands;
        for (const QString& id : actions.ids())
            commands.append(id);
        commands.sort();
        for (const QString& name : VimKeymap::builtinNames())
            commands.append(QStringLiteral("vim:") + name);
        m_command->addItems(commands);
        m_command->setCurrentText(start.command);
        auto* completer = new QCompleter(commands, this);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        m_command->setCompleter(completer);
        m_desc = new QLineEdit(start.description);
        m_note = new QLabel;
        m_note->setWordWrap(true);
        auto* form = new QFormLayout;
        form->addRow(tr("Where"), m_context);
        form->addRow(tr("Mode"), m_modes);
        form->addRow(tr("Keys"), m_keys);
        form->addRow(tr("Command"), m_command);
        form->addRow(tr("Description"), m_desc);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        auto* layout = new QVBoxLayout(this);
        layout->addLayout(form);
        layout->addWidget(
            new QLabel(tr("Keys as in Vim: gg, <leader>ff, <C-h>, <Space>, <CR>. "
                          "Command: a command id (edit.undo), vim:name or :ex line.")));
        layout->addWidget(m_note);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_keys, &QLineEdit::textChanged, this, [this] { validate(); });
        connect(m_command, &QComboBox::currentTextChanged, this, [this] { validate(); });
        m_ok = buttons->button(QDialogButtonBox::Ok);
        validate();
    }

    VimKeysTab::Edit edit() const
    {
        VimKeysTab::Edit e;
        e.context = m_context->currentData().toString();
        e.modes = m_modes->currentData().toString();
        e.keys = m_keys->text().trimmed();
        e.command = m_command->currentText().trimmed();
        e.description = m_desc->text().trimmed();
        return e;
    }

private:
    void validate()
    {
        QString problem;
        const QStringList tokens
            = vimkeys::parseKeys(m_keys->text().trimmed(), m_keymap.leader(), &problem);
        if (tokens.isEmpty() && problem.isEmpty() && !m_keys->text().isEmpty())
            problem = tr("Not a key");
        if (problem.isEmpty() && tokens.isEmpty())
            problem = tr("Type the keys");
        if (problem.isEmpty())
            problem = m_keymap.checkRunItem(m_command->currentText(), &m_actions);
        m_note->setText(problem);
        m_ok->setEnabled(problem.isEmpty());
    }

    ActionRegistry& m_actions;
    VimKeymap& m_keymap;
    QComboBox* m_context = nullptr;
    QComboBox* m_modes = nullptr;
    QLineEdit* m_keys = nullptr;
    QComboBox* m_command = nullptr;
    QLineEdit* m_desc = nullptr;
    QLabel* m_note = nullptr;
    QPushButton* m_ok = nullptr;
};

QString modesText(const QString& modes)
{
    QStringList parts;
    for (const QChar c : modes)
        parts.append(QString(c));
    return parts.join(QLatin1Char(' '));
}

} // namespace

VimKeysTab::VimKeysTab(ActionRegistry& actions, VimKeymap& keymap, QWidget* parent)
    : QWidget(parent)
    , m_actions(actions)
    , m_keymap(keymap)
{
    setObjectName(QStringLiteral("VimKeysTab"));
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("VimKeysSearch"));
    m_search->setPlaceholderText(tr("Search keys, commands or descriptions"));
    m_search->setClearButtonEnabled(true);
    auto* vimrc = new QPushButton(tr("Open vimrc"));
    vimrc->setToolTip(tr("A plain text file with one command per line (nmap, nunmap, set leader=), "
                         "read at start: %1")
            .arg(QDir::toNativeSeparators(VimKeymap::vimrcPath())));
    auto* top = new QHBoxLayout;
    top->addWidget(m_search, 1);
    top->addWidget(vimrc);

    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("VimKeysTree"));
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({tr("Keys"), tr("Mode"), tr("Command"), tr("Description")});
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setRootIsDecorated(true);
    m_tree->header()->setSectionResizeMode(DescColumn, QHeaderView::Stretch);
    m_tree->header()->resizeSection(KeysColumn, 130);
    m_tree->header()->resizeSection(ModeColumn, 50);
    m_tree->header()->resizeSection(CommandColumn, 200);
    m_tree->header()->setStretchLastSection(false);

    m_issues = new QListWidget;
    m_issues->setObjectName(QStringLiteral("VimKeysChecks"));
    m_issues->setMaximumHeight(90);
    m_status = new QLabel;
    m_status->setWordWrap(true);

    m_add = new QPushButton(tr("Add…"));
    m_edit = new QPushButton(tr("Change…"));
    m_remove = new QPushButton(tr("Remove"));
    m_reset = new QPushButton(tr("Reset"));
    auto* resetAllButton = new QPushButton(tr("Reset All"));
    auto* importButton = new QPushButton(tr("Import…"));
    auto* exportButton = new QPushButton(tr("Export…"));
    auto* buttons = new QHBoxLayout;
    for (QPushButton* b : {m_add, m_edit, m_remove, m_reset, resetAllButton})
        buttons->addWidget(b);
    buttons->addStretch(1);
    buttons->addWidget(importButton);
    buttons->addWidget(exportButton);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_issues);
    layout->addWidget(m_status);
    layout->addLayout(buttons);

    connect(m_search, &QLineEdit::textChanged, this, &VimKeysTab::setFilterText);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] { updateButtons(); });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item->data(0, kKeysRole).isValid())
            editCurrent(false);
    });
    connect(m_add, &QPushButton::clicked, this, [this] { editCurrent(true); });
    connect(m_edit, &QPushButton::clicked, this, [this] { editCurrent(false); });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        if (auto* item = m_tree->currentItem(); item && item->data(0, kKeysRole).isValid())
            removeBinding(item->data(0, kContextRole).toString(),
                item->data(0, kModesRole).toString(), item->data(0, kKeysRole).toString());
    });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        if (auto* item = m_tree->currentItem(); item && item->data(0, kKeysRole).isValid())
            resetBinding(item->data(0, kContextRole).toString(),
                item->data(0, kModesRole).toString(), item->data(0, kKeysRole).toString());
    });
    connect(resetAllButton, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Vim Keys"), tr("Bring back all the default Vim keys?"))
            == QMessageBox::Yes)
            resetAll();
    });
    connect(importButton, &QPushButton::clicked, this, &VimKeysTab::importDialog);
    connect(exportButton, &QPushButton::clicked, this, &VimKeysTab::exportDialog);
    connect(vimrc, &QPushButton::clicked, this, &VimKeysTab::openVimrc);
    connect(&m_keymap, &VimKeymap::changed, this, &VimKeysTab::refresh);
    refresh();
}

void VimKeysTab::refresh()
{
    const QString filter = m_search->text().trimmed();
    const QString currentKeys
        = m_tree->currentItem() ? m_tree->currentItem()->data(0, kKeysRole).toString() : QString();
    m_tree->clear();
    QFont bold = m_tree->font();
    bold.setBold(true);
    QMap<QString, QTreeWidgetItem*> groups;
    auto group = [&](const QString& key) {
        if (auto* g = groups.value(key))
            return g;
        auto* g = new QTreeWidgetItem(m_tree, {contextTitle(key)});
        g->setFont(0, bold);
        g->setFirstColumnSpanned(true);
        g->setFlags(Qt::ItemIsEnabled);
        groups.insert(key, g);
        return g;
    };
    // Fixed order: Leader, Global, Timeline, Viewer, Lists.
    for (const QString& c : {QStringLiteral("leader"), QStringLiteral("global"),
             QStringLiteral("timeline"), QStringLiteral("viewer"), QStringLiteral("list")})
        group(c);
    const QString leaderToken = m_keymap.leader();
    // One row per (context, keys); the modes of identical entries are joined.
    struct Row
    {
        const VimBinding* b;
        QString modes;
    };
    std::vector<Row> rows;
    for (const VimBinding& b : m_keymap.bindings()) {
        if (b.isGroup())
            continue;
        bool merged = false;
        for (Row& r : rows) {
            if (r.b->context == b.context && r.b->tokens == b.tokens && r.b->run == b.run
                && r.b->origin == b.origin && !r.modes.contains(b.mode)) {
                r.modes += b.mode;
                merged = true;
                break;
            }
        }
        if (!merged)
            rows.push_back({&b, QString(b.mode)});
    }
    for (const Row& r : rows) {
        const VimBinding& b = *r.b;
        const QString command = b.run.join(QLatin1String("; "));
        const QString shown = VimKeymap::describeRunItem(b.run.value(0), &m_actions);
        const QString text = b.keys + QLatin1Char(' ') + command + QLatin1Char(' ') + b.desc
            + QLatin1Char(' ') + shown;
        if (!filter.isEmpty() && !text.contains(filter, Qt::CaseInsensitive))
            continue;
        const bool isLeader = !b.tokens.isEmpty() && b.tokens.first() == leaderToken;
        QTreeWidgetItem* parent = group(isLeader ? QStringLiteral("leader") : b.context);
        auto* item = new QTreeWidgetItem(parent);
        item->setText(KeysColumn, b.keys);
        item->setText(ModeColumn, modesText(r.modes));
        item->setText(CommandColumn, command);
        item->setText(DescColumn, b.desc.isEmpty() ? shown : b.desc);
        item->setData(0, kContextRole, b.context);
        item->setData(0, kModesRole, r.modes);
        item->setData(0, kKeysRole, b.keys);
        item->setData(0, kOriginRole, int(b.origin));
        item->setData(0, kRunRole, b.run);
        if (b.origin != VimBinding::Origin::Default) {
            QFont f = item->font(0);
            f.setBold(b.origin == VimBinding::Origin::User);
            f.setItalic(b.origin == VimBinding::Origin::Vimrc);
            for (int c = 0; c < 4; ++c)
                item->setFont(c, f);
            item->setToolTip(DescColumn,
                b.origin == VimBinding::Origin::User ? tr("Your change")
                                                     : tr("From the vimrc file (read only)"));
            if (b.origin == VimBinding::Origin::Vimrc)
                item->setText(DescColumn, item->text(DescColumn) + tr("  (vimrc)"));
        }
        if (b.keys == currentKeys)
            m_tree->setCurrentItem(item);
    }
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        if (it.value()->childCount() == 0 && !filter.isEmpty())
            delete it.value();
        else
            it.value()->setExpanded(true);
    }
    // The checks.
    m_issues->clear();
    for (const VimKeymap::Issue& issue : m_keymap.issues(&m_actions)) {
        const QString severity = issue.severity == VimKeymap::Issue::Severity::Error ? tr("Error")
            : issue.severity == VimKeymap::Issue::Severity::Warning                  ? tr("Warning")
                                                                                     : tr("Info");
        m_issues->addItem(QStringLiteral("%1: %2").arg(severity, issue.text));
    }
    if (!m_keymap.vimrcMessages().isEmpty())
        for (const QString& m : m_keymap.vimrcMessages())
            m_issues->addItem(tr("Error: %1").arg(m));
    updateButtons();
}

void VimKeysTab::updateButtons()
{
    const QTreeWidgetItem* item = m_tree->currentItem();
    const bool row = item && item->data(0, kKeysRole).isValid();
    const bool vimrcRow
        = row && item->data(0, kOriginRole).toInt() == int(VimBinding::Origin::Vimrc);
    m_edit->setEnabled(row && !vimrcRow);
    m_remove->setEnabled(row && !vimrcRow);
    m_reset->setEnabled(row && !vimrcRow);
}

void VimKeysTab::setFilterText(const QString& text)
{
    if (m_search->text() != text)
        m_search->setText(text);
    refresh();
}

QStringList VimKeysTab::rowTexts() const
{
    QStringList list;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        const QTreeWidgetItem* group = m_tree->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            const QTreeWidgetItem* row = group->child(i);
            list.append(QStringLiteral("%1 | %2 | %3 | %4")
                    .arg(group->text(0), row->text(KeysColumn), row->text(ModeColumn),
                        row->text(CommandColumn)));
        }
    }
    return list;
}

QStringList VimKeysTab::issueLines() const
{
    QStringList list;
    for (int i = 0; i < m_issues->count(); ++i)
        list.append(m_issues->item(i)->text());
    return list;
}

QString VimKeysTab::statusText() const
{
    return m_status->text();
}

VimKeysTab::Result VimKeysTab::saveBinding(
    const Edit& edit, const Edit* original, bool replace, QString* message)
{
    auto say = [&](const QString& text) {
        if (message)
            *message = text;
        m_status->setText(text);
    };
    QString problem;
    const QStringList tokens = vimkeys::parseKeys(edit.keys, m_keymap.leader(), &problem);
    if (tokens.isEmpty()) {
        say(problem.isEmpty() ? tr("Type the keys") : problem);
        return Result::Invalid;
    }
    problem = m_keymap.checkRunItem(edit.command, &m_actions);
    if (!problem.isEmpty()) {
        say(problem);
        return Result::Invalid;
    }
    // Is something else on these keys in this context and mode?
    const bool sameAsOriginal = original && original->context == edit.context
        && original->modes == edit.modes
        && vimkeys::parseKeys(original->keys, m_keymap.leader()) == tokens;
    if (!sameAsOriginal) {
        for (const VimBinding& b : m_keymap.bindings()) {
            if (b.isGroup() || b.context != edit.context || b.tokens != tokens
                || !edit.modes.contains(b.mode))
                continue;
            say(tr("%1 is already bound to %2 in %3")
                    .arg(edit.keys, b.run.join(QLatin1String("; ")), contextTitle(b.context)));
            if (!replace)
                return Result::Conflict;
            break;
        }
    }
    if (original && !sameAsOriginal)
        m_keymap.unmap(original->context, original->modes, original->keys);
    if (!m_keymap.map(
            edit.context, edit.modes, edit.keys, {edit.command}, edit.description, &problem)) {
        say(problem);
        return Result::Invalid;
    }
    // Prefix check: warn when the new keys start longer ones, or are inside shorter ones.
    QString warning;
    for (const VimKeymap::Issue& issue : m_keymap.issues(&m_actions)) {
        if (issue.severity == VimKeymap::Issue::Severity::Warning && issue.keys == edit.keys
            && issue.context == edit.context)
            warning = issue.text;
    }
    for (const VimBinding& b : m_keymap.bindings()) {
        if (b.isGroup() || b.context != edit.context || b.tokens == tokens)
            continue;
        if (b.tokens.size() < tokens.size() && tokens.mid(0, b.tokens.size()) == b.tokens
            && edit.modes.contains(b.mode))
            warning = tr("%1 starts with %2, which is bound too: %2 runs after %3 ms")
                          .arg(edit.keys, b.keys)
                          .arg(m_keymap.timeoutMs());
    }
    say(warning.isEmpty() ? tr("Saved") : warning);
    return warning.isEmpty() ? Result::Saved : Result::SavedWithWarning;
}

bool VimKeysTab::removeBinding(const QString& context, const QString& modes, const QString& keys)
{
    QString error;
    // A key of the user's own is dropped; a default is unmapped.
    m_keymap.resetKeys(context, modes, keys);
    bool stillThere = false;
    const QStringList tokens = vimkeys::parseKeys(keys, m_keymap.leader());
    for (const VimBinding& b : m_keymap.bindings())
        stillThere = stillThere
            || (!b.isGroup() && b.context == context && b.tokens == tokens
                && modes.contains(b.mode));
    if (stillThere && !m_keymap.unmap(context, modes, keys, &error)) {
        m_status->setText(error);
        return false;
    }
    m_status->setText(tr("Removed %1").arg(keys));
    return true;
}

void VimKeysTab::resetBinding(const QString& context, const QString& modes, const QString& keys)
{
    m_keymap.resetKeys(context, modes, keys);
    m_status->setText(tr("%1 is back to its default").arg(keys));
}

void VimKeysTab::resetAll()
{
    m_keymap.resetAll();
    m_status->setText(tr("All Vim keys are back to their defaults"));
}

bool VimKeysTab::exportTo(const QString& path, QString* error) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(m_keymap.exportJson()) < 0
        || !file.commit()) {
        if (error)
            *error = tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool VimKeysTab::importFrom(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    return m_keymap.importJson(file.readAll(), error);
}

void VimKeysTab::editCurrent(bool addNew)
{
    Edit start;
    Edit original;
    bool haveOriginal = false;
    if (!addNew) {
        const QTreeWidgetItem* item = m_tree->currentItem();
        if (!item || !item->data(0, kKeysRole).isValid())
            return;
        start.context = item->data(0, kContextRole).toString();
        start.modes = item->data(0, kModesRole).toString();
        start.keys = item->data(0, kKeysRole).toString();
        start.command = item->data(0, kRunRole).toStringList().value(0);
        start.description = item->text(DescColumn);
        original = start;
        haveOriginal = true;
    }
    BindingEditor editor(m_actions, m_keymap, start, addNew, this);
    if (editor.exec() != QDialog::Accepted)
        return;
    QString message;
    Result result = saveBinding(editor.edit(), haveOriginal ? &original : nullptr, false, &message);
    if (result == Result::Conflict
        && QMessageBox::question(
               this, tr("Vim Keys"), message + QLatin1Char('\n') + tr("Replace it?"))
            == QMessageBox::Yes)
        saveBinding(editor.edit(), haveOriginal ? &original : nullptr, true, &message);
}

void VimKeysTab::openVimrc()
{
    const QString path = VimKeymap::vimrcPath();
    if (!QFile::exists(path)) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write("\" Kinoklip vimrc: one command per line, read at start and by :source.\n"
                    "\" Examples:\n"
                    "\" set leader=<Space>\n"
                    "\" set timeoutlen=400\n"
                    "\" nmap <leader>w :w<CR>\n"
                    "\" nunmap <leader>x\n");
        }
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        m_status->setText(
            tr("Open this file in your editor: %1").arg(QDir::toNativeSeparators(path)));
}

void VimKeysTab::importDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Vim Keys"), QString(), tr("Vim keys (*.json)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!importFrom(path, &error))
        QMessageBox::warning(this, tr("Vim Keys"), error);
    else
        m_status->setText(tr("Imported"));
}

void VimKeysTab::exportDialog()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Vim Keys"), QStringLiteral("vim-keys.json"), tr("Vim keys (*.json)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!exportTo(path, &error))
        QMessageBox::warning(this, tr("Vim Keys"), error);
}

} // namespace kk
