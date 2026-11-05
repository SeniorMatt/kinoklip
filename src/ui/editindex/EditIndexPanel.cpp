// SPDX-License-Identifier: GPL-3.0-or-later
// EditIndexPanel — mode switch, filters, table and the jump / select behaviour.
#include "ui/editindex/EditIndexPanel.h"

#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/editindex/EditIndexFilter.h"
#include "ui/timeline/TimelineDialogs.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

namespace kk {

namespace {

/// Side of the colour swatches in the colour filter menu (px).
constexpr int kSwatchSize = 12;

/// Settings key of the hidden columns of @p mode (prefs::kEditIndexHiddenColumns).
QString hiddenColumnsKey(EditIndexModel::Mode mode)
{
    return QLatin1String(prefs::kEditIndexHiddenColumns)
        + (mode == EditIndexModel::Mode::Edits ? QLatin1String("/edits")
                                               : QLatin1String("/markers"));
}

QIcon swatch(const QColor& color)
{
    QPixmap pm(kSwatchSize, kSwatchSize);
    pm.fill(color); // user data (marker colour), not scheme chrome
    return QIcon(pm);
}

} // namespace

EditIndexPanel::EditIndexPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Edit Index"), parent)
    , m_ctx(ctx)
{
    setPanelKey(QStringLiteral("edit_index")); // in-app help: panel:edit_index
    setObjectName(QStringLiteral("EditIndexPanel"));
    m_model = new EditIndexModel(ctx.project, this);
    m_filter = new EditIndexFilter(this);
    m_filter->setSourceModel(m_model);

    m_modeBox = new QComboBox;
    m_modeBox->setObjectName(QStringLiteral("EditIndexMode"));
    m_modeBox->addItem(tr("Edits"), int(EditIndexModel::Mode::Edits));
    m_modeBox->addItem(tr("Markers"), int(EditIndexModel::Mode::Markers));
    addTitleBarWidget(m_modeBox);

    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* filters = new QHBoxLayout;
    filters->setContentsMargins(
        metrics::kSpaceS, metrics::kSpaceS, metrics::kSpaceS, metrics::kSpaceS);
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("EditIndexSearch"));
    m_search->setPlaceholderText(tr("Search"));
    m_search->setClearButtonEnabled(true);
    filters->addWidget(m_search, 1);
    setSearchField(m_search); // spec 49.5
    m_colorFilter = new QComboBox;
    m_colorFilter->setObjectName(QStringLiteral("EditIndexColorFilter"));
    m_colorFilter->addItem(tr("All Colors"), QColor());
    for (const auto& [name, color] : timelinedialogs::markerColors())
        m_colorFilter->addItem(swatch(color), name, color);
    filters->addWidget(m_colorFilter);
    layout->addLayout(filters);

    m_table = new QTableView;
    m_table->setObjectName(QStringLiteral("EditIndexTable"));
    m_table->setModel(m_filter);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(0, Qt::AscendingOrder);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(
        m_table->fontMetrics().height() + 2 * metrics::kSpaceS);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setHighlightSections(false);
    // Right-click on the header: the column chooser (Resolve's Edit Index column options).
    m_table->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table->horizontalHeader(), &QWidget::customContextMenuRequested, this,
        [this](const QPoint& pos) {
            QMenu* menu = createColumnMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->popup(m_table->horizontalHeader()->mapToGlobal(pos));
        });
    m_table->installEventFilter(this);
    layout->addWidget(m_table, 1);
    setContentWidget(body);

    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(0);
    connect(m_rebuildTimer, &QTimer::timeout, this, &EditIndexPanel::rebuildNow);

    connect(m_modeBox, &QComboBox::currentIndexChanged, this,
        [this] { setMode(EditIndexModel::Mode(m_modeBox->currentData().toInt())); });
    connect(m_search, &QLineEdit::textChanged, m_filter, &EditIndexFilter::setSearchText);
    connect(m_colorFilter, &QComboBox::currentIndexChanged, this,
        [this] { m_filter->setMarkerColor(m_colorFilter->currentData().value<QColor>()); });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
        [this](const QModelIndex& current) {
            if (!m_syncing && current.isValid())
                activateRow(current.row());
        });
    // A click on the row that is already current jumps again (the playhead may have moved).
    connect(m_table, &QTableView::clicked, this, [this](const QModelIndex& index) {
        if (!m_syncing && index.isValid())
            activateRow(index.row());
    });

    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &EditIndexPanel::scheduleRebuild);
    connect(ctx.project->mediaPool(), &MediaPool::changed, this, &EditIndexPanel::scheduleRebuild);
    connect(ctx.project, &Project::projectReset, this, &EditIndexPanel::scheduleRebuild);
    if (ctx.selection)
        connect(ctx.selection, &SelectionModel::changed, this, &EditIndexPanel::syncFromSelection);
    m_colorFilter->setVisible(false);
    rebuildNow();
    applyHiddenColumns();
}

QMenu* EditIndexPanel::createColumnMenu(QWidget* parent)
{
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("EditIndexColumnMenu"));
    const QHeaderView* header = m_table->horizontalHeader();
    for (int column = 1; column < m_model->columnCount(); ++column) {
        QAction* a = menu->addAction(
            m_model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
        a->setCheckable(true);
        a->setChecked(!header->isSectionHidden(column));
        a->setData(column);
        connect(
            a, &QAction::toggled, this, [this, column](bool on) { setColumnVisible(column, on); });
    }
    return menu;
}

void EditIndexPanel::setColumnVisible(int column, bool visible)
{
    if (column <= 0 || column >= m_model->columnCount())
        return; // "#" always stays: rows must keep a handle
    m_table->setColumnHidden(column, !visible);
    QStringList hidden;
    for (int c = 1; c < m_model->columnCount(); ++c) {
        if (m_table->isColumnHidden(c))
            hidden << EditIndexModel::columnKey(mode(), c);
    }
    QSettings().setValue(hiddenColumnsKey(mode()), hidden);
}

void EditIndexPanel::applyHiddenColumns()
{
    const QStringList hidden = QSettings().value(hiddenColumnsKey(mode())).toStringList();
    for (int c = 0; c < m_model->columnCount(); ++c)
        m_table->setColumnHidden(c, c > 0 && hidden.contains(EditIndexModel::columnKey(mode(), c)));
}

bool EditIndexPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_table
        && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)
        && mode() == EditIndexModel::Mode::Markers && m_table->currentIndex().isValid()) {
        // (A cell editor gets its own key events, so editing text is not affected.)
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace)
            && key->modifiers() == Qt::NoModifier) {
            if (event->type() == QEvent::KeyPress) {
                const int row = m_filter->mapToSource(m_table->currentIndex()).row();
                m_model->deleteMarker(row);
            }
            event->accept(); // ShortcutOverride: the key goes to us, not to a shortcut
            return true;
        }
    }
    return PanelFrame::eventFilter(watched, event);
}

void EditIndexPanel::setMode(EditIndexModel::Mode mode)
{
    const int index = m_modeBox->findData(int(mode));
    if (m_modeBox->currentIndex() != index) {
        m_modeBox->setCurrentIndex(index); // comes back here through the signal
        return;
    }
    m_syncing = true;
    m_model->setMode(mode);
    m_syncing = false;
    m_colorFilter->setVisible(mode == EditIndexModel::Mode::Markers);
    applyHiddenColumns(); // the other mode has its own columns and choice
    m_table->resizeColumnsToContents();
    syncFromSelection();
}

void EditIndexPanel::scheduleRebuild()
{
    m_dirty = true;
    if (isVisible())
        m_rebuildTimer->start();
}

void EditIndexPanel::showEvent(QShowEvent* event)
{
    PanelFrame::showEvent(event);
    if (m_dirty)
        rebuildNow();
}

void EditIndexPanel::rebuildNow()
{
    m_rebuildTimer->stop();
    m_dirty = false;
    // Keep the current row across the reset (by its clip / marker key).
    const QModelIndex current = m_table->currentIndex();
    const quint64 clipId = current.data(EditIndexModel::ClipIdRole).toULongLong();
    const qint64 stored = current.data(EditIndexModel::StoredFrameRole).toLongLong();
    const bool hadCurrent = current.isValid();
    const bool firstFill = m_model->rowCount() == 0;
    m_syncing = true;
    m_model->rebuild();
    if (hadCurrent)
        selectKey(clipId, stored);
    m_syncing = false;
    if (firstFill)
        m_table->resizeColumnsToContents();
}

void EditIndexPanel::activateRow(int row)
{
    const QModelIndex index = m_filter->index(row, 0);
    if (!index.isValid())
        return;
    const quint64 clipId = index.data(EditIndexModel::ClipIdRole).toULongLong();
    const int64_t frame = index.data(EditIndexModel::FrameRole).toLongLong();
    m_syncing = true; // our own selection change must not move the table
    if (clipId && m_ctx.selection)
        m_ctx.selection->selectClip(clipId);
    m_ctx.project->timeline()->setPlayhead(frame);
    m_syncing = false;
}

void EditIndexPanel::syncFromSelection()
{
    if (m_syncing || !m_ctx.selection || mode() != EditIndexModel::Mode::Edits)
        return;
    const quint64 clipId = m_ctx.selection->currentClip();
    m_syncing = true;
    if (!clipId || !selectKey(clipId, 0))
        m_table->selectionModel()->clearSelection();
    m_syncing = false;
}

bool EditIndexPanel::selectKey(quint64 clipId, qint64 storedFrame)
{
    const bool markers = mode() == EditIndexModel::Mode::Markers;
    for (int row = 0; row < m_filter->rowCount(); ++row) {
        const QModelIndex index = m_filter->index(row, 0);
        if (index.data(EditIndexModel::ClipIdRole).toULongLong() != clipId)
            continue;
        if (markers && index.data(EditIndexModel::StoredFrameRole).toLongLong() != storedFrame)
            continue;
        m_table->selectionModel()->setCurrentIndex(
            index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_table->scrollTo(index);
        return true;
    }
    return false;
}

} // namespace kk
