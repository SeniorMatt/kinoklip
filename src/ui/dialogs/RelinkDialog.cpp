// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/RelinkDialog.h"

#include "core/MediaPool.h"
#include "core/Plural.h"
#include "media/MediaSource.h"
#include "platform/FileDialogs.h"
#include "ui/common/DialogSizing.h"
#include "ui/common/MediaImport.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace kk {

namespace {

constexpr int kDefaultSimilarity = 70; ///< percent

/// "42 %" of a 0..1 quality.
QString percent(double quality)
{
    return QStringLiteral("%1 %").arg(int(quality * 100.0 + 0.5));
}

} // namespace

RelinkDialog::RelinkDialog(
    const MediaPool& pool, const QStringList& onlyIds, const QString& folder, QWidget* parent)
    : QDialog(parent)
    , m_pool(pool)
    , m_onlyIds(onlyIds)
{
    setObjectName(QStringLiteral("RelinkDialog"));
    setWindowTitle(tr("Relink Media"));
    buildSources();

    auto* intro = new QLabel(tr("Choose the folder with the files. Names do not have to be "
                                "identical: the preview shows what will be relinked."));
    intro->setWordWrap(true);

    m_folder = new QLineEdit;
    m_folder->setObjectName(QStringLiteral("RelinkFolder"));
    m_folder->setPlaceholderText(tr("Folder with the files"));
    auto* browse = new QPushButton(tr("Browse…"));
    browse->setObjectName(QStringLiteral("RelinkBrowse"));
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);

    m_subfolders = new QCheckBox(tr("Search subfolders"));
    m_subfolders->setObjectName(QStringLiteral("RelinkSubfolders"));
    m_subfolders->setChecked(true);
    m_ignoreExtension = new QCheckBox(tr("Ignore the file extension"));
    m_ignoreExtension->setObjectName(QStringLiteral("RelinkIgnoreExtension"));
    m_ignoreExtension->setChecked(true);
    m_ignoreExtension->setToolTip(tr("\"clip.mov\" is found as \"clip.mp4\" or \"clip.mxf\""));
    m_ignoreCase = new QCheckBox(tr("Ignore case"));
    m_ignoreCase->setObjectName(QStringLiteral("RelinkIgnoreCase"));
    m_ignoreCase->setChecked(true);
    m_closest = new QCheckBox(tr("Closest name, at least"));
    m_closest->setObjectName(QStringLiteral("RelinkClosest"));
    m_closest->setToolTip(tr("Also take the file with the most similar name when no file has "
                             "the same name (a typo, another numbering, an added suffix)"));
    m_similarity = new QSpinBox;
    m_similarity->setObjectName(QStringLiteral("RelinkSimilarity"));
    m_similarity->setRange(40, 99);
    m_similarity->setValue(kDefaultSimilarity);
    m_similarity->setSuffix(QStringLiteral(" %"));
    m_similarity->setEnabled(false);
    auto* closestRow = new QHBoxLayout;
    closestRow->addWidget(m_closest);
    closestRow->addWidget(m_similarity);
    closestRow->addStretch(1);
    m_includeOnline = new QCheckBox(tr("Also relink media that is online"));
    m_includeOnline->setObjectName(QStringLiteral("RelinkIncludeOnline"));
    m_includeOnline->setVisible(!m_online.isEmpty());

    m_find = new QLineEdit;
    m_find->setObjectName(QStringLiteral("RelinkFind"));
    m_find->setPlaceholderText(tr("Regular expression, e.g. _v1$"));
    m_find->setToolTip(tr("Found in the old file name (without the extension) and replaced by "
                          "the text on the right; the result is the name to look for"));
    m_replace = new QLineEdit;
    m_replace->setObjectName(QStringLiteral("RelinkReplace"));
    m_replace->setPlaceholderText(tr("Replace with, e.g. _v2 (\\1 = first group)"));
    m_patternNote = new QLabel;
    m_patternNote->setObjectName(QStringLiteral("RelinkPatternNote"));
    m_patternNote->setForegroundRole(QPalette::PlaceholderText);
    m_patternNote->setWordWrap(true);
    auto* patternRow = new QHBoxLayout;
    patternRow->addWidget(m_find, 1);
    patternRow->addWidget(new QLabel(QStringLiteral("→")));
    patternRow->addWidget(m_replace, 1);

    auto* options = new QGridLayout;
    options->addWidget(m_subfolders, 0, 0);
    options->addWidget(m_ignoreExtension, 0, 1);
    options->addWidget(m_ignoreCase, 1, 0);
    options->addLayout(closestRow, 1, 1);
    options->addWidget(m_includeOnline, 2, 0, 1, 2);

    m_table = new QTableWidget(0, ColumnCount);
    m_table->setObjectName(QStringLiteral("RelinkTable"));
    m_table->setHorizontalHeaderLabels(
        {QString(), tr("Old file"), tr("New file"), tr("Match"), tr("Quality")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->horizontalHeader()->setSectionResizeMode(ApplyColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(OldColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(NewColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(MatchColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(QualityColumn, QHeaderView::ResizeToContents);
    m_table->setMinimumHeight(220);
    m_table->setToolTip(tr("Double-click a row to choose its file by hand"));

    m_summary = new QLabel;
    m_summary->setObjectName(QStringLiteral("RelinkSummary"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_apply = buttons->addButton(tr("Relink"), QDialogButtonBox::AcceptRole);
    m_apply->setObjectName(QStringLiteral("RelinkApply"));
    m_apply->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(folderRow);
    layout->addLayout(options);
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow(tr("Find and replace"), patternRow);
    layout->addLayout(form);
    layout->addWidget(m_patternNote);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_summary);
    layout->addWidget(buttons);

    connect(browse, &QPushButton::clicked, this, &RelinkDialog::browse);
    connect(m_folder, &QLineEdit::editingFinished, this, [this] { setFolder(m_folder->text()); });
    connect(m_subfolders, &QCheckBox::toggled, this, [this] { setFolder(m_folder->text()); });
    for (QCheckBox* box : {m_ignoreExtension, m_ignoreCase, m_includeOnline})
        connect(box, &QCheckBox::toggled, this, [this] { replan(); });
    connect(m_closest, &QCheckBox::toggled, this, [this](bool on) {
        m_similarity->setEnabled(on);
        replan();
    });
    connect(m_similarity, &QSpinBox::valueChanged, this, [this] { replan(); });
    connect(m_find, &QLineEdit::textChanged, this, [this] { replan(); });
    connect(m_replace, &QLineEdit::textChanged, this, [this] { replan(); });
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
        [this](int row, int) { chooseFileFor(row); });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (m_filling)
            return;
        if (item->column() == ApplyColumn && item->row() < m_rows.size()) {
            const QString& id = m_rows[item->row()].itemId;
            if (item->checkState() == Qt::Checked)
                m_unticked.remove(id);
            else
                m_unticked.insert(id);
        }
        updateSummary();
    });

    dialogsizing::fit(this, QSize(780, 560));
    if (!folder.isEmpty())
        setFolder(folder);
    else
        replan();
}

void RelinkDialog::buildSources()
{
    for (const MediaItem& m : m_pool.state().items) {
        if (m.isTimeline() || m.isSubtitle() || (!m_onlyIds.isEmpty() && !m_onlyIds.contains(m.id)))
            continue;
        relink::Source s;
        s.itemId = m.id;
        s.oldPath = m.path;
        s.hasVideo = m.info.hasVideo;
        (media::mediaExists(m.path) ? m_online : m_offline).append(s);
    }
}

relink::Options RelinkDialog::options() const
{
    relink::Options o;
    o.ignoreExtension = m_ignoreExtension->isChecked();
    o.ignoreCase = m_ignoreCase->isChecked();
    o.closestName = m_closest->isChecked();
    o.minimumSimilarity = m_similarity->value() / 100.0;
    o.find = m_find->text();
    o.replace = m_replace->text();
    return o;
}

void RelinkDialog::setFolder(const QString& folder)
{
    m_folder->setText(folder);
    m_files.clear();
    if (!folder.trimmed().isEmpty() && QFileInfo(folder).isDir()) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_files = relink::scanFolder(folder, m_subfolders->isChecked(), MediaPool::kMaxRelinkScanFiles);
        QApplication::restoreOverrideCursor();
    }
    replan();
}

void RelinkDialog::replan()
{
    const QString error = relink::patternError(m_find->text());
    m_patternNote->setText(error.isEmpty() ? QString() : tr("Not a valid expression: %1").arg(error));
    m_patternNote->setVisible(!error.isEmpty());
    QVector<relink::Source> sources = m_offline;
    if (m_includeOnline->isChecked() && !m_online.isEmpty())
        sources += m_online;
    // An invalid expression is ignored (the note says so), not an error that empties the table.
    relink::Options o = options();
    if (!error.isEmpty())
        o.find.clear();
    m_rows = relink::plan(sources, m_files, o);
    for (relink::MatchRow& row : m_rows) {
        const QString manual = m_manual.value(row.itemId);
        if (!manual.isEmpty()) {
            row.newPath = manual;
            row.kind = relink::MatchKind::Manual;
            row.quality = 1.0;
            row.ambiguous = false;
        }
    }
    fillTable();
}

void RelinkDialog::fillTable()
{
    m_filling = true;
    m_table->setRowCount(0);
    m_table->setRowCount(int(m_rows.size()));
    for (int r = 0; r < m_rows.size(); ++r) {
        const relink::MatchRow& row = m_rows[r];
        const bool matched = !row.newPath.isEmpty();
        auto* check = new QTableWidgetItem;
        check->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        check->setCheckState(matched && !m_unticked.contains(row.itemId) ? Qt::Checked : Qt::Unchecked);
        if (!matched)
            check->setFlags(Qt::ItemIsSelectable);
        m_table->setItem(r, ApplyColumn, check);
        auto cell = [&](int column, const QString& text, const QString& tip = {}) {
            auto* item = new QTableWidgetItem(text);
            item->setToolTip(tip.isEmpty() ? text : tip);
            if (!matched)
                item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
            m_table->setItem(r, column, item);
        };
        cell(OldColumn, QFileInfo(row.oldPath).fileName(), row.oldPath);
        if (matched) {
            cell(NewColumn, QFileInfo(row.newPath).fileName(), row.newPath);
            QString kind = relink::kindText(row.kind);
            if (row.ambiguous)
                kind += tr(" (several files fit)");
            cell(MatchColumn, kind);
            cell(QualityColumn, percent(row.quality));
        } else {
            cell(NewColumn, tr("(no file found)"));
            cell(MatchColumn, relink::kindText(relink::MatchKind::None));
            cell(QualityColumn, QString());
        }
    }
    m_filling = false;
    updateSummary();
}

void RelinkDialog::updateSummary()
{
    int matched = 0, ticked = 0;
    for (int r = 0; r < m_rows.size(); ++r) {
        if (m_rows[r].newPath.isEmpty())
            continue;
        ++matched;
        if (m_table->item(r, ApplyColumn) && m_table->item(r, ApplyColumn)->checkState() == Qt::Checked)
            ++ticked;
    }
    const int unmatched = int(m_rows.size()) - matched;
    QString text;
    if (m_rows.isEmpty())
        text = tr("No offline media to relink.");
    else if (m_files.isEmpty())
        text = tr("Choose a folder with media files.");
    else
        text = plural(ticked, tr("%n item will be relinked."), tr("%n items will be relinked."))
            + (unmatched > 0 ? QLatin1Char(' ')
                        + plural(unmatched, tr("%n item has no match."), tr("%n items have no match."))
                             : QString());
    m_summary->setText(text);
    m_apply->setEnabled(ticked > 0);
}

QVector<relink::MatchRow> RelinkDialog::result() const
{
    QVector<relink::MatchRow> out;
    for (int r = 0; r < m_rows.size(); ++r) {
        const QTableWidgetItem* check = m_table->item(r, ApplyColumn);
        if (!m_rows[r].newPath.isEmpty() && check && check->checkState() == Qt::Checked)
            out.append(m_rows[r]);
    }
    return out;
}

void RelinkDialog::browse()
{
    const QString folder = platform::getExistingDirectory(this,
        tr("Relink Media: Choose the Folder with the Files"), QStringLiteral("paths/lastRelinkDir"),
        m_folder->text());
    if (!folder.isEmpty())
        setFolder(folder);
}

void RelinkDialog::chooseFileFor(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    const QString path = platform::getOpenFileName(this, tr("Choose the File"),
        mediaFileDialogFilter(), QStringLiteral("paths/lastRelinkDir"), m_folder->text());
    if (!path.isEmpty())
        chooseFile(row, path);
}

void RelinkDialog::chooseFile(int row, const QString& path)
{
    if (row < 0 || row >= m_rows.size() || path.isEmpty())
        return;
    m_manual.insert(m_rows[row].itemId, QFileInfo(path).absoluteFilePath());
    replan();
}

} // namespace kk
