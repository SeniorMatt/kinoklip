// SPDX-License-Identifier: GPL-3.0-or-later
// SmartBinDialog — create / edit a smart bin: name, match all / any, a list of rules.
#pragma once

#include "core/MediaItem.h"

#include <QDialog>
#include <QVector>

class QComboBox;
class QLineEdit;
class QVBoxLayout;

namespace kk {

/// The smart bin editor (Resolve layout: name, "Match all / any of the following", one row per
/// rule: field, operator, value, remove button; "Add Rule"). The value editor follows the
/// field: text, choice list, date or number of days.
/// Thread-safety: GUI thread only.
class SmartBinDialog final : public QDialog
{
    Q_OBJECT
public:
    /// Edits a copy of @p bin (an empty bin gets one "Clip Name contains" rule).
    explicit SmartBinDialog(const SmartBin& bin, QWidget* parent = nullptr);

    /// The edited smart bin (same id as the one given to the constructor).
    [[nodiscard]] SmartBin smartBin() const;

    /// Adds a rule row (tests and the "Add Rule" button).
    void addRule(const SmartRule& rule);
    /// Number of rule rows.
    [[nodiscard]] int ruleCount() const { return int(m_rows.size()); }

private:
    struct RuleRow;
    void removeRow(RuleRow* row);
    void updateOk();

    QString m_id;
    QLineEdit* m_name = nullptr;
    QComboBox* m_match = nullptr;
    QVBoxLayout* m_rulesLayout = nullptr;
    QVector<RuleRow*> m_rows;
    QWidget* m_okButton = nullptr;
};

} // namespace kk
