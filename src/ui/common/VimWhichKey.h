// SPDX-License-Identifier: GPL-3.0-or-later
// VimWhichKey — the which-key popup of the Vim layer (spec section 83): after a prefix such as
// Space, g or z waits for the timeout, a list of the keys that can follow, with what each does,
// appears above the page bar. The idea is which-key.nvim's
// (https://github.com/folke/which-key.nvim, Apache-2.0); nothing is copied.
#pragma once

#include "ui/common/VimKeymap.h"

#include <QFrame>

class QGridLayout;
class QLabel;

namespace kk {

class VimWhichKey : public QFrame
{
    Q_OBJECT
public:
    explicit VimWhichKey(QWidget* parent = nullptr);

    /// Shows the list for @p prefix ("SPC s") at the bottom of the parent, above @p bottomOffset
    /// pixels (the page bar).
    void showEntries(
        const QString& prefix, const QList<VimContinuation>& entries, int bottomOffset);
    void hideEntries();

    /// The visible rows as "key  description" ("i  insert the source clip", "s  +source edits").
    [[nodiscard]] QStringList rows() const { return m_rows; }
    [[nodiscard]] QString title() const;

private:
    QLabel* m_title = nullptr;
    QWidget* m_body = nullptr;
    QGridLayout* m_grid = nullptr;
    QStringList m_rows;
};

} // namespace kk
