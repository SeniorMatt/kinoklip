// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorTitleLayers — the layer list above the Title tab's parameters (spec section 68):
// add Text / Shape, duplicate, delete, move up / down, show / hide, rename. The controls below
// the list edit the selected layer. Part of InspectorPanel.
#include "core/TitleLayers.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"

#include <QHBoxLayout>
#include <QListWidget>
#include <QMenu>
#include <QSignalBlocker>
#include <QToolButton>

#include <algorithm>

namespace kk {

namespace {

struct ShapeChoice
{
    const char* type;
    const char* label;
};

const ShapeChoice kShapes[] = {
    {"rect", QT_TRANSLATE_NOOP("kk::InspectorPanel", "Rectangle")},
    {"rounded", QT_TRANSLATE_NOOP("kk::InspectorPanel", "Rounded Rectangle")},
    {"ellipse", QT_TRANSLATE_NOOP("kk::InspectorPanel", "Ellipse")},
    {"line", QT_TRANSLATE_NOOP("kk::InspectorPanel", "Line")},
};

} // namespace

void InspectorPanel::selectTitleLayer(quint64 layerId)
{
    m_titleLayer = layerId;
    onTimelineChanged(); // a changed selection is a changed structure: rebuilds the tab
}

void InspectorPanel::addTitleLayer(TitleLayerKind kind, const QString& shape)
{
    const quint64 clipId = m_clipId;
    quint64 created = 0;
    editClip(kind == TitleLayerKind::Shape ? tr("Add Shape Layer") : tr("Add Text Layer"), clipId,
        [&](ClipData& c) { created = titlelayers::addLayer(c, kind, shape); });
    if (created != 0)
        selectTitleLayer(created);
}

void InspectorPanel::duplicateTitleLayer(quint64 layerId)
{
    quint64 created = 0;
    editClip(tr("Duplicate Layer"), m_clipId,
        [&](ClipData& c) { created = titlelayers::duplicateLayer(c, layerId); });
    if (created != 0)
        selectTitleLayer(created);
}

void InspectorPanel::removeTitleLayer(quint64 layerId)
{
    bool removed = false;
    editClip(tr("Delete Layer"), m_clipId,
        [&](ClipData& c) { removed = titlelayers::removeLayer(c, layerId); });
    if (removed)
        selectTitleLayer(titlelayers::kBaseId);
}

void InspectorPanel::moveTitleLayer(quint64 layerId, int delta)
{
    editClip(tr("Reorder Layers"), m_clipId,
        [&](ClipData& c) { titlelayers::moveLayer(c, layerId, delta); });
}

void InspectorPanel::buildTitleLayersSection(const ClipData& c)
{
    const quint64 clipId = m_clipId;
    ParamGroupBox* group = addGroup(TitleTab, QStringLiteral("title-layers"), tr("Layers"), clipId);
    group->setObjectName(QStringLiteral("title-layers"));

    // The top layer first, as in Resolve's and every layer list.
    auto* list = new QListWidget;
    list->setObjectName(QStringLiteral("title-layer-list"));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    list->setToolTip(tr("The layers of this title, top layer first. Tick to show, double-click "
                        "to rename."));
    const QVector<quint64> order = titlelayers::order(c);
    for (qsizetype i = order.size() - 1; i >= 0; --i) {
        const quint64 id = order[i];
        auto* item = new QListWidgetItem(titlelayers::displayName(c, id), list);
        item->setData(Qt::UserRole, qulonglong(id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
        item->setCheckState(titlelayers::visible(c, id) ? Qt::Checked : Qt::Unchecked);
        if (id == m_structure.titleLayer)
            list->setCurrentItem(item);
    }
    list->setFixedHeight(std::clamp(int(order.size()), 2, 6) * list->sizeHintForRow(0) + 6);
    group->addWidgetRow({}, list);

    connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (!item)
            return;
        const quint64 id = item->data(Qt::UserRole).toULongLong();
        if (id != m_titleLayer)
            QMetaObject::invokeMethod(
                this, [this, id] { selectTitleLayer(id); }, Qt::QueuedConnection);
    });
    connect(list, &QListWidget::itemChanged, this, [this, clipId](QListWidgetItem* item) {
        const quint64 id = item->data(Qt::UserRole).toULongLong();
        const bool shown = item->checkState() == Qt::Checked;
        const QString name = item->text();
        const ClipData* now = clip(clipId);
        if (!now || !titlelayers::exists(*now, id))
            return;
        if (titlelayers::visible(*now, id) != shown)
            editClip(shown ? tr("Show Layer") : tr("Hide Layer"), clipId,
                [=](ClipData& c) { titlelayers::setVisible(c, id, shown); });
        else if (titlelayers::displayName(*now, id) != name)
            editClip(
                tr("Rename Layer"), clipId, [=](ClipData& c) { titlelayers::rename(c, id, name); });
    });

    auto* bar = new QWidget;
    bar->setObjectName(QStringLiteral("title-layer-buttons"));
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(2);
    auto textButton = [&](const QString& name, const QString& text, const QString& tip) {
        auto* b = new QToolButton(bar);
        b->setObjectName(name);
        b->setAutoRaise(true);
        b->setText(text);
        b->setToolTip(tip);
        barLayout->addWidget(b);
        return b;
    };
    QToolButton* addText = textButton(
        QStringLiteral("title-layer-add-text"), tr("Text"), tr("Add a text layer on top"));
    connect(addText, &QToolButton::clicked, this, [this] { addTitleLayer(TitleLayerKind::Text); });
    QToolButton* addShape = textButton(
        QStringLiteral("title-layer-add-shape"), tr("Shape"), tr("Add a shape layer on top"));
    auto* shapeMenu = new QMenu(addShape);
    for (const ShapeChoice& s : kShapes) {
        const QString type = QString::fromLatin1(s.type);
        shapeMenu->addAction(
            tr(s.label), this, [this, type] { addTitleLayer(TitleLayerKind::Shape, type); });
    }
    addShape->setMenu(shapeMenu);
    addShape->setPopupMode(QToolButton::InstantPopup);
    const quint64 selected = m_structure.titleLayer;
    QToolButton* duplicate = textButton(
        QStringLiteral("title-layer-duplicate"), tr("Duplicate"), tr("Copy the selected layer"));
    connect(duplicate, &QToolButton::clicked, this,
        [this, selected] { duplicateTitleLayer(selected); });
    barLayout->addStretch(1);
    auto glyphButton = [&](const QString& name, InspectorGlyph glyph, const QString& tip) {
        auto* b = new QToolButton(bar);
        b->setObjectName(name);
        b->setAutoRaise(true);
        b->setIcon(inspectorGlyph(glyph));
        b->setToolTip(tip);
        barLayout->addWidget(b);
        return b;
    };
    QToolButton* up = glyphButton(
        QStringLiteral("title-layer-up"), InspectorGlyph::MoveUp, tr("Move the layer up"));
    QToolButton* down = glyphButton(
        QStringLiteral("title-layer-down"), InspectorGlyph::MoveDown, tr("Move the layer down"));
    QToolButton* remove = glyphButton(QStringLiteral("title-layer-delete"), InspectorGlyph::Remove,
        tr("Delete the layer (the first layer can only be hidden)"));
    const qsizetype at = order.indexOf(selected);
    up->setEnabled(at >= 0 && at + 1 < order.size());
    down->setEnabled(at > 0);
    remove->setEnabled(selected != titlelayers::kBaseId);
    connect(up, &QToolButton::clicked, this, [this, selected] { moveTitleLayer(selected, +1); });
    connect(down, &QToolButton::clicked, this, [this, selected] { moveTitleLayer(selected, -1); });
    connect(remove, &QToolButton::clicked, this, [this, selected] { removeTitleLayer(selected); });
    group->addWidgetRow({}, bar);
}

} // namespace kk
