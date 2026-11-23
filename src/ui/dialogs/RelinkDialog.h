// SPDX-License-Identifier: GPL-3.0-or-later
// RelinkDialog — Media Pool > Relink Selected Clips / Relink Offline Media: matches the media
// to the files of a folder that are not exactly the same name (other case, other extension, a
// find / replace pattern on the old name, the closest name) and shows the plan as a preview
// table - old file, new file, kind and quality of the match, unmatched rows - before anything
// changes. The rows can be switched off or given another file by hand.
#pragma once

#include "core/RelinkMatcher.h"

#include <QDialog>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace kk {

class MediaPool;

/// Modal dialog. Thread-safety: GUI thread only.
class RelinkDialog : public QDialog
{
    Q_OBJECT
public:
    /// Columns of the preview table.
    enum Column { ApplyColumn, OldColumn, NewColumn, MatchColumn, QualityColumn, ColumnCount };

    /// @p onlyIds: the items to relink (empty = every offline item); @p folder: the folder to
    /// search first (empty = none yet).
    RelinkDialog(const MediaPool& pool, const QStringList& onlyIds, const QString& folder,
        QWidget* parent = nullptr);

    /// The rows that will be relinked (ticked, with a new file), in table order.
    [[nodiscard]] QVector<relink::MatchRow> result() const;
    /// Every row of the plan (also unmatched ones).
    [[nodiscard]] const QVector<relink::MatchRow>& rows() const { return m_rows; }
    /// The options as set in the dialog.
    [[nodiscard]] relink::Options options() const;

    /// Searches @p folder (the folder field) and rebuilds the plan.
    void setFolder(const QString& folder);
    /// Rebuilds the plan from the current options (after any option changed).
    void replan();

    // ---- Controls (tests, screenshots) ----
    [[nodiscard]] QTableWidget* table() const { return m_table; }
    [[nodiscard]] QLineEdit* folderEdit() const { return m_folder; }
    [[nodiscard]] QCheckBox* subfoldersBox() const { return m_subfolders; }
    [[nodiscard]] QCheckBox* ignoreCaseBox() const { return m_ignoreCase; }
    [[nodiscard]] QCheckBox* ignoreExtensionBox() const { return m_ignoreExtension; }
    [[nodiscard]] QCheckBox* closestBox() const { return m_closest; }
    [[nodiscard]] QSpinBox* similarityBox() const { return m_similarity; }
    [[nodiscard]] QLineEdit* findEdit() const { return m_find; }
    [[nodiscard]] QLineEdit* replaceEdit() const { return m_replace; }
    [[nodiscard]] QCheckBox* includeOnlineBox() const { return m_includeOnline; }
    [[nodiscard]] QLabel* summaryLabel() const { return m_summary; }
    [[nodiscard]] QPushButton* applyButton() const { return m_apply; }
    /// Gives row @p row the file @p path by hand (what a double-click on its new file does).
    void chooseFile(int row, const QString& path);

private:
    void buildSources();
    void fillTable();
    void updateSummary();
    void browse();
    void chooseFileFor(int row);

    const MediaPool& m_pool;
    QStringList m_onlyIds;
    QVector<relink::Source> m_offline; ///< the items in scope that are offline
    QVector<relink::Source> m_online; ///< the items in scope that are online
    QStringList m_files; ///< the media files found in the folder
    QVector<relink::MatchRow> m_rows;
    QHash<QString, QString> m_manual; ///< item id -> file chosen by hand
    QSet<QString> m_unticked; ///< items the user switched off
    bool m_filling = false;

    QLineEdit* m_folder = nullptr;
    QCheckBox* m_subfolders = nullptr;
    QCheckBox* m_ignoreCase = nullptr;
    QCheckBox* m_ignoreExtension = nullptr;
    QCheckBox* m_closest = nullptr;
    QSpinBox* m_similarity = nullptr;
    QLineEdit* m_find = nullptr;
    QLineEdit* m_replace = nullptr;
    QLabel* m_patternNote = nullptr;
    QCheckBox* m_includeOnline = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_summary = nullptr;
    QPushButton* m_apply = nullptr;
};

} // namespace kk
