// SPDX-License-Identifier: GPL-3.0-or-later
// SmartBinDialog implementation.
#include "ui/mediapool/SmartBinDialog.h"

#include "core/SmartBins.h"
#include "ui/common/Icons.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr int kMaxDays = 36500; ///< "in the last N days" upper bound (100 years)
constexpr int kDefaultDays = 7;
constexpr int kMinWidth = 640; ///< px: field + operator + value fit side by side
constexpr int kEditorText = 0; ///< pages of the value editor stack
constexpr int kEditorChoice = 1;
constexpr int kEditorDate = 2;
constexpr int kEditorDays = 3;
} // namespace

/// One rule row. Owned by the dialog (deleted with its widget).
struct SmartBinDialog::RuleRow
{
    QWidget* widget = nullptr;
    QComboBox* field = nullptr;
    QComboBox* op = nullptr;
    QStackedWidget* editors = nullptr;
    QLineEdit* text = nullptr;
    QComboBox* choice = nullptr;
    QDateEdit* date = nullptr;
    QSpinBox* days = nullptr;

    [[nodiscard]] SmartField currentField() const
    {
        return SmartField(field->currentData().toInt());
    }
    [[nodiscard]] SmartOp currentOp() const { return SmartOp(op->currentData().toInt()); }

    /// Fills the operators and the value editor for the chosen field.
    void fieldChanged()
    {
        const SmartField f = currentField();
        const QSignalBlocker block(op);
        op->clear();
        for (const SmartOp o : smartbins::operatorsFor(f))
            op->addItem(smartbins::operatorLabel(o), int(o));
        choice->clear();
        for (const auto& [id, label] : smartbins::choicesFor(f))
            choice->addItem(label, id);
        opChanged();
    }

    void opChanged()
    {
        switch (smartbins::valueKind(currentField())) {
        case smartbins::ValueKind::Choice:
        case smartbins::ValueKind::Flags:
            editors->setCurrentIndex(kEditorChoice);
            break;
        case smartbins::ValueKind::Date:
            editors->setCurrentIndex(currentOp() == SmartOp::InLast ? kEditorDays : kEditorDate);
            break;
        case smartbins::ValueKind::Resolution:
            text->setPlaceholderText(QStringLiteral("1920x1080"));
            editors->setCurrentIndex(kEditorText);
            break;
        default:
            text->setPlaceholderText({});
            editors->setCurrentIndex(kEditorText);
            break;
        }
    }

    [[nodiscard]] QString value() const
    {
        switch (editors->currentIndex()) {
        case kEditorChoice:
            return choice->currentData().toString();
        case kEditorDate:
            return date->date().toString(Qt::ISODate);
        case kEditorDays:
            return QString::number(days->value());
        default:
            return text->text().trimmed();
        }
    }

    void setRule(const SmartRule& rule)
    {
        field->setCurrentIndex(field->findData(int(rule.field)));
        fieldChanged();
        op->setCurrentIndex(std::max(0, op->findData(int(rule.op))));
        opChanged();
        switch (editors->currentIndex()) {
        case kEditorChoice:
            choice->setCurrentIndex(std::max(0, choice->findData(rule.value)));
            break;
        case kEditorDate:
            if (const QDate d = QDate::fromString(rule.value, Qt::ISODate); d.isValid())
                date->setDate(d);
            break;
        case kEditorDays:
            days->setValue(rule.value.isEmpty() ? kDefaultDays : rule.value.toInt());
            break;
        default:
            text->setText(rule.value);
            break;
        }
    }
};

SmartBinDialog::SmartBinDialog(const SmartBin& bin, QWidget* parent)
    : QDialog(parent)
    , m_id(bin.id)
{
    setWindowTitle(bin.id.isEmpty() ? tr("New Smart Bin") : tr("Edit Smart Bin"));
    setMinimumWidth(kMinWidth);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    m_name = new QLineEdit(bin.name.isEmpty() ? tr("Smart Bin") : bin.name);
    m_name->setObjectName(QStringLiteral("SmartBinName"));
    form->addRow(tr("Name"), m_name);
    m_match = new QComboBox;
    m_match->setObjectName(QStringLiteral("SmartBinMatch"));
    m_match->addItem(tr("all of the following rules"), true);
    m_match->addItem(tr("any of the following rules"), false);
    m_match->setCurrentIndex(bin.matchAll ? 0 : 1);
    form->addRow(tr("Match"), m_match);
    layout->addLayout(form);

    auto* rulesHost = new QWidget;
    m_rulesLayout = new QVBoxLayout(rulesHost);
    m_rulesLayout->setContentsMargins(0, 0, 0, 0);
    m_rulesLayout->addStretch(1);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(rulesHost);
    layout->addWidget(scroll, 1);

    auto* add = new QPushButton(icon(QStringLiteral("add")), tr("Add Rule"));
    add->setObjectName(QStringLiteral("SmartBinAddRule"));
    connect(add, &QPushButton::clicked, this, [this] { addRule({}); });
    auto* addRow = new QHBoxLayout;
    addRow->addWidget(add);
    addRow->addStretch(1);
    layout->addLayout(addRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(m_name, &QLineEdit::textChanged, this, &SmartBinDialog::updateOk);

    if (bin.rules.isEmpty())
        addRule({});
    for (const SmartRule& rule : bin.rules)
        addRule(rule);
    updateOk();
}

void SmartBinDialog::addRule(const SmartRule& rule)
{
    auto* row = new RuleRow;
    row->widget = new QWidget;
    auto* h = new QHBoxLayout(row->widget);
    h->setContentsMargins(0, 0, 0, 0);
    row->field = new QComboBox;
    for (const SmartField f : smartbins::fields())
        row->field->addItem(smartbins::fieldLabel(f), int(f));
    row->op = new QComboBox;
    row->editors = new QStackedWidget;
    row->text = new QLineEdit;
    row->choice = new QComboBox;
    row->date = new QDateEdit(QDate::currentDate());
    row->date->setCalendarPopup(true);
    row->date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    row->days = new QSpinBox;
    row->days->setRange(0, kMaxDays);
    row->days->setValue(kDefaultDays);
    row->days->setSuffix(tr(" days"));
    row->editors->insertWidget(kEditorText, row->text);
    row->editors->insertWidget(kEditorChoice, row->choice);
    row->editors->insertWidget(kEditorDate, row->date);
    row->editors->insertWidget(kEditorDays, row->days);
    auto* remove = new QToolButton;
    remove->setIcon(icon(QStringLiteral("remove")));
    remove->setToolTip(tr("Remove this rule"));
    remove->setAutoRaise(true);
    h->addWidget(row->field);
    h->addWidget(row->op);
    h->addWidget(row->editors, 1);
    h->addWidget(remove);
    connect(row->field, &QComboBox::currentIndexChanged, this, [row] { row->fieldChanged(); });
    connect(row->op, &QComboBox::currentIndexChanged, this, [row] { row->opChanged(); });
    connect(remove, &QToolButton::clicked, this, [this, row] { removeRow(row); });
    m_rulesLayout->insertWidget(m_rulesLayout->count() - 1, row->widget);
    m_rows.append(row);
    row->setRule(rule);
    connect(row->widget, &QObject::destroyed, this, [row] { delete row; });
    updateOk();
}

void SmartBinDialog::removeRow(RuleRow* row)
{
    m_rows.removeAll(row);
    row->widget->deleteLater(); // deletes the RuleRow too
    updateOk();
}

void SmartBinDialog::updateOk()
{
    if (m_okButton)
        m_okButton->setEnabled(!m_name->text().trimmed().isEmpty() && !m_rows.isEmpty());
}

SmartBin SmartBinDialog::smartBin() const
{
    SmartBin bin;
    bin.id = m_id;
    bin.name = m_name->text().trimmed();
    bin.matchAll = m_match->currentData().toBool();
    for (const RuleRow* row : m_rows)
        bin.rules.append(SmartRule {row->currentField(), row->currentOp(), row->value()});
    return bin;
}

} // namespace kk
