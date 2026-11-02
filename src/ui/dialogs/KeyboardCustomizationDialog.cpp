// SPDX-License-Identifier: GPL-3.0-or-later
// KeyboardCustomizationDialog implementation.
#include "ui/dialogs/KeyboardCustomizationDialog.h"

#include "ui/common/ActionRegistry.h"
#include "ui/common/StandardActions.h"

#include <QAction>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace kk {

namespace {

constexpr int kIdRole = Qt::UserRole;
enum Column {
    CommandColumn,
    ShortcutColumn,
    DefaultColumn
};

QString keysText(const QList<QKeySequence>& keys)
{
    QStringList parts;
    for (const QKeySequence& key : keys)
        parts.append(key.toString(QKeySequence::NativeText));
    return parts.join(QStringLiteral(", "));
}

QString stripMnemonic(QString text)
{
    // "&Open Project…" -> "Open Project…"; "&&" stays a literal ampersand.
    text.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
    text.remove(QLatin1Char('&'));
    text.replace(QStringLiteral("\x01"), QStringLiteral("&"));
    return text;
}

} // namespace

// ---- ShortcutRecorder ----

ShortcutRecorder::ShortcutRecorder(QWidget* parent)
    : QLineEdit(parent)
{
    setReadOnly(true);
    setPlaceholderText(tr("Press a key combination…"));
}

bool ShortcutRecorder::event(QEvent* event)
{
    // Tab / Shift+Tab are valid shortcuts too: take them before focus navigation does.
    if (event->type() == QEvent::KeyPress) {
        keyPressEvent(static_cast<QKeyEvent*>(event));
        return true;
    }
    // Swallow shortcut overrides so application shortcuts do not fire while recording.
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    return QLineEdit::event(event);
}

void ShortcutRecorder::keyPressEvent(QKeyEvent* event)
{
    const int key = event->key();
    switch (key) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_unknown:
        return; // wait for the real key
    default:
        break;
    }
    Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if (key == Qt::Key_Escape && modifiers == Qt::NoModifier) {
        emit cancelled();
        return;
    }
    if (key == Qt::Key_Backtab) // Shift+Tab arrives as Backtab
        modifiers |= Qt::ShiftModifier;
    const QKeySequence sequence(
        QKeyCombination(modifiers, key == Qt::Key_Backtab ? Qt::Key_Tab : Qt::Key(key)));
    setText(sequence.toString(QKeySequence::NativeText));
    emit recorded(sequence);
}

// ---- KeyboardCustomizationDialog ----

KeyboardCustomizationDialog::KeyboardCustomizationDialog(ActionRegistry& registry, QWidget* parent)
    : QDialog(parent)
    , m_registry(registry)
{
    setWindowTitle(tr("Keyboard Customization"));
    resize(760, 620);
    for (const QString& id : registry.ids())
        m_original.insert(id, registry.shortcuts(id));

    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search commands or keys"));
    m_search->setClearButtonEnabled(true);

    m_tree = new QTreeWidget;
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Command"), tr("Shortcut"), tr("Default")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->header()->setSectionResizeMode(CommandColumn, QHeaderView::Stretch);
    m_tree->header()->resizeSection(ShortcutColumn, 170);
    m_tree->header()->resizeSection(DefaultColumn, 170);
    m_tree->header()->setStretchLastSection(false);

    m_recorder = new ShortcutRecorder;
    m_recorder->setEnabled(false);
    m_record = new QPushButton(tr("Record"));
    m_clear = new QPushButton(tr("Clear"));
    m_reset = new QPushButton(tr("Reset"));
    auto* resetAll = new QPushButton(tr("Reset All"));
    auto* importButton = new QPushButton(tr("Import…"));
    auto* exportButton = new QPushButton(tr("Export…"));
    m_status = new QLabel;
    m_status->setWordWrap(true);

    auto* editRow = new QHBoxLayout;
    editRow->addWidget(new QLabel(tr("Shortcut:")));
    editRow->addWidget(m_recorder, 1);
    editRow->addWidget(m_record);
    editRow->addWidget(m_clear);
    editRow->addWidget(m_reset);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->addButton(resetAll, QDialogButtonBox::ResetRole);
    buttons->addButton(importButton, QDialogButtonBox::ActionRole);
    buttons->addButton(exportButton, QDialogButtonBox::ActionRole);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_search);
    layout->addWidget(m_tree, 1);
    layout->addLayout(editRow);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    populate();

    connect(m_search, &QLineEdit::textChanged, this, &KeyboardCustomizationDialog::setFilterText);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] {
        m_recorder->setEnabled(false);
        m_recorder->clear();
        m_status->clear();
        updateButtons();
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (!item->data(0, kIdRole).toString().isEmpty())
            startRecording();
    });
    connect(m_record, &QPushButton::clicked, this, &KeyboardCustomizationDialog::startRecording);
    connect(m_recorder, &ShortcutRecorder::recorded, this,
        &KeyboardCustomizationDialog::finishRecording);
    connect(m_recorder, &ShortcutRecorder::cancelled, this, [this] {
        m_recorder->setEnabled(false);
        m_recorder->clear();
        m_tree->setFocus();
    });
    connect(m_clear, &QPushButton::clicked, this, [this] { clearShortcuts(currentId()); });
    connect(m_reset, &QPushButton::clicked, this, &KeyboardCustomizationDialog::resetCurrent);
    connect(resetAll, &QPushButton::clicked, this, &KeyboardCustomizationDialog::resetAll);
    connect(importButton, &QPushButton::clicked, this, &KeyboardCustomizationDialog::importKeymap);
    connect(exportButton, &QPushButton::clicked, this, &KeyboardCustomizationDialog::exportKeymap);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateButtons();
}

QString KeyboardCustomizationDialog::commandName(const QString& id) const
{
    const QAction* action = m_registry.action(id);
    QString name = action ? stripMnemonic(action->text()) : id;
    // Some titles carry a %1 placeholder for the application name.
    return name.replace(QStringLiteral("%1"), QCoreApplication::applicationName());
}

void KeyboardCustomizationDialog::populate()
{
    QHash<QString, QString> titles;
    for (const MenuCategory& menu : standardMenus())
        titles.insert(menu.id, stripMnemonic(menu.title));
    QFont bold = m_tree->font();
    bold.setBold(true);
    for (const QString& category : m_registry.categories()) {
        QString title = titles.value(category, stripMnemonic(m_registry.submenuTitle(category)));
        if (title.isEmpty())
            title = category;
        auto* section = new QTreeWidgetItem(m_tree, {title});
        section->setFont(CommandColumn, bold);
        section->setFlags(Qt::ItemIsEnabled);
        section->setFirstColumnSpanned(true);
        for (const QString& id : m_registry.menuEntries(category)) {
            // separators and submenu markers (the submenu is a section of its own)
            if (id == QLatin1String(ActionRegistry::kSeparator)
                || !ActionRegistry::submenuOf(id).isEmpty())
                continue;
            auto* row = new QTreeWidgetItem(section);
            row->setData(CommandColumn, kIdRole, id);
            row->setText(CommandColumn, commandName(id));
            row->setToolTip(CommandColumn, id);
            row->setText(DefaultColumn, keysText(m_registry.defaultShortcuts(id)));
            m_rows.insert(id, row);
            refreshRow(id);
        }
        section->setExpanded(true);
    }
}

void KeyboardCustomizationDialog::refreshRow(const QString& id)
{
    QTreeWidgetItem* row = m_rows.value(id);
    if (!row)
        return;
    const QList<QKeySequence> current = m_registry.shortcuts(id);
    row->setText(ShortcutColumn, keysText(current));
    // Customised keys stand out in the accent colour.
    const bool custom = current != m_registry.defaultShortcuts(id);
    QFont font = m_tree->font();
    font.setBold(custom);
    row->setFont(ShortcutColumn, font);
    row->setForeground(ShortcutColumn,
        custom ? palette().brush(QPalette::Highlight) : palette().brush(QPalette::Text));
}

void KeyboardCustomizationDialog::refreshAllRows()
{
    for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it)
        refreshRow(it.key());
    setFilterText(m_search->text());
}

void KeyboardCustomizationDialog::updateButtons()
{
    const QString id = currentId();
    m_record->setEnabled(!id.isEmpty());
    m_clear->setEnabled(!id.isEmpty() && !m_registry.shortcuts(id).isEmpty());
    m_reset->setEnabled(
        !id.isEmpty() && m_registry.shortcuts(id) != m_registry.defaultShortcuts(id));
}

QString KeyboardCustomizationDialog::currentId() const
{
    const QTreeWidgetItem* item = m_tree->currentItem();
    return item ? item->data(CommandColumn, kIdRole).toString() : QString();
}

void KeyboardCustomizationDialog::setCurrentId(const QString& id)
{
    if (QTreeWidgetItem* row = m_rows.value(id))
        m_tree->setCurrentItem(row);
}

QStringList KeyboardCustomizationDialog::visibleIds() const
{
    QStringList ids;
    for (int s = 0; s < m_tree->topLevelItemCount(); ++s) {
        const QTreeWidgetItem* section = m_tree->topLevelItem(s);
        if (section->isHidden())
            continue;
        for (int i = 0; i < section->childCount(); ++i) {
            if (!section->child(i)->isHidden())
                ids.append(section->child(i)->data(CommandColumn, kIdRole).toString());
        }
    }
    return ids;
}

void KeyboardCustomizationDialog::setFilterText(const QString& text)
{
    if (m_search->text() != text)
        m_search->setText(text);
    const QString needle = text.trimmed();
    for (int s = 0; s < m_tree->topLevelItemCount(); ++s) {
        QTreeWidgetItem* section = m_tree->topLevelItem(s);
        int visible = 0;
        for (int i = 0; i < section->childCount(); ++i) {
            QTreeWidgetItem* row = section->child(i);
            const bool match = needle.isEmpty()
                || row->text(CommandColumn).contains(needle, Qt::CaseInsensitive)
                || row->text(ShortcutColumn).contains(needle, Qt::CaseInsensitive)
                || row->data(CommandColumn, kIdRole)
                       .toString()
                       .contains(needle, Qt::CaseInsensitive);
            row->setHidden(!match);
            visible += match ? 1 : 0;
        }
        section->setHidden(visible == 0);
    }
}

QString KeyboardCustomizationDialog::conflictFor(
    const QString& id, const QKeySequence& sequence) const
{
    return m_registry.actionForShortcut(sequence, id);
}

bool KeyboardCustomizationDialog::assignShortcut(
    const QString& id, const QKeySequence& sequence, ConflictPolicy policy)
{
    if (id.isEmpty() || sequence.isEmpty())
        return false;
    const QString other = conflictFor(id, sequence);
    if (!other.isEmpty()) {
        if (policy == ConflictPolicy::Ask) {
            const auto answer = QMessageBox::question(this, tr("Shortcut in Use"),
                tr("%1 is already assigned to “%2”.\n\nAssign it to “%3” instead?")
                    .arg(sequence.toString(QKeySequence::NativeText), commandName(other),
                        commandName(id)));
            policy = answer == QMessageBox::Yes ? ConflictPolicy::Reassign : ConflictPolicy::Cancel;
        }
        if (policy == ConflictPolicy::Cancel) {
            m_status->setText(
                tr("Kept %1 on “%2”.")
                    .arg(sequence.toString(QKeySequence::NativeText), commandName(other)));
            return false;
        }
        QList<QKeySequence> otherKeys = m_registry.shortcuts(other);
        otherKeys.removeAll(sequence);
        m_registry.setShortcuts(other, otherKeys);
        refreshRow(other);
    }
    QList<QKeySequence> keys = m_registry.shortcuts(id);
    keys.removeAll(sequence);
    if (keys.isEmpty())
        keys.append(sequence);
    else
        keys[0] = sequence; // the new key replaces the primary one, secondaries stay
    m_registry.setShortcuts(id, keys);
    refreshRow(id);
    m_status->setText(other.isEmpty()
            ? QString()
            : tr("Removed %1 from “%2”.")
                  .arg(sequence.toString(QKeySequence::NativeText), commandName(other)));
    updateButtons();
    return true;
}

void KeyboardCustomizationDialog::clearShortcuts(const QString& id)
{
    if (id.isEmpty())
        return;
    m_registry.setShortcuts(id, {});
    refreshRow(id);
    updateButtons();
}

void KeyboardCustomizationDialog::startRecording()
{
    if (currentId().isEmpty())
        return;
    m_recorder->setEnabled(true);
    m_recorder->clear();
    m_recorder->setFocus();
    m_status->setText(
        tr("Press the new shortcut for “%1” (Escape cancels).").arg(commandName(currentId())));
}

void KeyboardCustomizationDialog::finishRecording(const QKeySequence& sequence)
{
    const QString id = currentId();
    m_recorder->setEnabled(false);
    assignShortcut(id, sequence, ConflictPolicy::Ask);
    m_tree->setFocus();
}

void KeyboardCustomizationDialog::resetCurrent()
{
    const QString id = currentId();
    if (id.isEmpty())
        return;
    m_registry.resetShortcuts(id);
    refreshRow(id);
    updateButtons();
}

void KeyboardCustomizationDialog::resetAll()
{
    const auto answer = QMessageBox::question(
        this, tr("Reset All Shortcuts"), tr("Restore the default shortcut of every command?"));
    if (answer != QMessageBox::Yes)
        return;
    m_registry.resetAllShortcuts();
    refreshAllRows();
    updateButtons();
}

void KeyboardCustomizationDialog::importKeymap()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Keyboard Shortcuts"), QString(), tr("Keymap (*.json)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!m_registry.importKeymap(path, &error)) {
        QMessageBox::warning(this, tr("Import Keyboard Shortcuts"), error);
        return;
    }
    refreshAllRows();
    updateButtons();
}

void KeyboardCustomizationDialog::exportKeymap()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export Keyboard Shortcuts"),
        QStringLiteral("keymap.json"), tr("Keymap (*.json)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        path += QStringLiteral(".json");
    QString error;
    if (!m_registry.exportKeymap(path, &error))
        QMessageBox::warning(this, tr("Export Keyboard Shortcuts"), error);
}

void KeyboardCustomizationDialog::reject()
{
    // Undo every change made while the dialog was open.
    for (auto it = m_original.cbegin(); it != m_original.cend(); ++it) {
        if (m_registry.shortcuts(it.key()) != it.value())
            m_registry.setShortcuts(it.key(), it.value());
    }
    QDialog::reject();
}

} // namespace kk
