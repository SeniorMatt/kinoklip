// SPDX-License-Identifier: GPL-3.0-or-later
// GuideDialog — Help > Guide…: search the guide's articles and open one in the browser.
#pragma once

#include <QDialog>

class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace kk {

/// A search field over the articles' titles, summaries and keywords, and the list of articles
/// with "Start page" always on top. Enter in the field (or in the list) or a double-click opens
/// the current entry through guide::openArticle() and closes the dialog.
class GuideDialog : public QDialog
{
    Q_OBJECT
public:
    explicit GuideDialog(QWidget* parent = nullptr);

    /// Filters the list (the search field's text).
    void setFilterText(const QString& text);
    /// Ids of the listed articles in order; the start page is "" (tests).
    [[nodiscard]] QStringList visibleArticleIds() const;
    /// Opens the current entry (or the first article listed, else the start page).
    void openCurrent();

    [[nodiscard]] QLineEdit* searchField() const { return m_search; }
    [[nodiscard]] QListWidget* list() const { return m_list; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void populate();
    void open(QListWidgetItem* item);

    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
};

} // namespace kk
