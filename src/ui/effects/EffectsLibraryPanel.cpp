// SPDX-License-Identifier: GPL-3.0-or-later
// EffectsLibraryPanel implementation: category tree with painted glyphs, search, drag source.
#include "ui/effects/EffectsLibraryPanel.h"

#include "core/EffectRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/effects/EffectActions.h"

#include <QApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace kk {

namespace {

constexpr int kGlyphSize = 20;
constexpr int kDescriptorRole = Qt::UserRole; ///< descriptor id (empty for section rows)
constexpr int kCategoryRole = Qt::UserRole + 1; ///< EffectCategory of a section row
constexpr int kSidebarWidth = 150; ///< px, category sidebar
constexpr int kListWidth = 230;

/// Tree that exports mime::Effect for drags (QTreeWidget's default exports its own format).
class EffectsTree final : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {QLatin1String(mime::Effect)}; }
    /// Copy only (the drag source's actions come from here): the library never loses items,
    /// and Wayland negotiates one unambiguous action.
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction; }

    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        if (items.isEmpty())
            return nullptr;
        const QString id = items.front()->data(0, kDescriptorRole).toString();
        if (id.isEmpty())
            return nullptr;
        auto* data = new QMimeData;
        data->setData(QLatin1String(mime::Effect), id.toUtf8());
        data->setText(id);
        return data;
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && currentItem()) {
            emit itemDoubleClicked(currentItem(), 0);
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }
};

/// A small rounded tile with a category-specific mark, drawn once per category. Text marks
/// ("T", "fx") are plain letters, not emoji.
QIcon categoryGlyph(EffectCategory category)
{
    QPixmap pixmap(kGlyphSize * 2, kGlyphSize * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF tile(1.5, 3.5, kGlyphSize - 3, kGlyphSize - 7);
    QColor tint;
    switch (category) {
    case EffectCategory::Transition:
        tint = QColor(0x6f, 0x9f, 0xd8);
        break;
    case EffectCategory::Title:
        tint = QColor(0xc9, 0x8f, 0xd8);
        break;
    case EffectCategory::Generator:
        tint = QColor(0x7f, 0xc0, 0x8a);
        break;
    case EffectCategory::AudioEffect:
        tint = QColor(0x5f, 0xc4, 0xb8);
        break;
    default:
        tint = QColor(0xd8, 0xa8, 0x5f);
        break;
    }
    QColor fill = tint;
    fill.setAlphaF(0.22f);
    p.setPen(QPen(tint, 1.2));
    p.setBrush(fill);
    p.drawRoundedRect(tile, 2.5, 2.5);

    switch (category) {
    case EffectCategory::Transition: {
        // Two frames blending: the right half shaded with a diagonal edge.
        QPainterPath half;
        half.moveTo(tile.center().x() + 2, tile.top());
        half.lineTo(tile.right(), tile.top());
        half.lineTo(tile.right(), tile.bottom());
        half.lineTo(tile.center().x() - 2, tile.bottom());
        half.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(tint);
        p.drawPath(half);
        break;
    }
    case EffectCategory::Generator:
        p.setPen(Qt::NoPen);
        p.setBrush(tint);
        p.drawRoundedRect(tile.adjusted(3.5, 3, -3.5, -3), 1, 1);
        break;
    case EffectCategory::AudioEffect: {
        p.setPen(QPen(tint, 1.4, Qt::SolidLine, Qt::RoundCap));
        const qreal cy = tile.center().y();
        const qreal heights[] = {2, 4, 2.5, 3.5, 1.5};
        qreal x = tile.left() + 3.5;
        for (qreal h : heights) {
            p.drawLine(QPointF(x, cy - h), QPointF(x, cy + h));
            x += 2.5;
        }
        break;
    }
    default: {
        QFont font = QApplication::font();
        font.setPixelSize(category == EffectCategory::Title ? 10 : 8);
        font.setBold(true);
        p.setFont(font);
        p.setPen(tint.lighter(130));
        p.drawText(tile, Qt::AlignCenter,
            category == EffectCategory::Title ? QStringLiteral("T") : QStringLiteral("fx"));
        break;
    }
    }
    return QIcon(pixmap);
}

QString sectionTitle(EffectCategory category)
{
    switch (category) {
    case EffectCategory::Transition:
        return EffectsLibraryPanel::tr("Video Transitions");
    case EffectCategory::Title:
        return EffectsLibraryPanel::tr("Titles");
    case EffectCategory::Generator:
        return EffectsLibraryPanel::tr("Generators");
    case EffectCategory::VideoEffect:
        return EffectsLibraryPanel::tr("Video Effects");
    case EffectCategory::AudioEffect:
        return EffectsLibraryPanel::tr("Audio Effects");
    case EffectCategory::ClipProperties:
        break;
    }
    return {};
}

} // namespace

EffectsLibraryPanel::EffectsLibraryPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Effects"), parent)
    , m_ctx(ctx)
{
    // Resolve's layout: a category sidebar on the left, search + the effects on the right.
    m_categories = new QTreeWidget;
    m_categories->setObjectName(QStringLiteral("EffectsCategories"));
    m_categories->setHeaderHidden(true);
    m_categories->setFrameShape(QFrame::NoFrame);
    m_categories->setIndentation(metrics::kSpaceL);
    m_categories->setSelectionMode(QAbstractItemView::SingleSelection);

    auto* listSide = new QWidget;
    auto* layout = new QVBoxLayout(listSide);
    layout->setContentsMargins(metrics::kSpaceS, metrics::kSpaceS, metrics::kSpaceS, 0);
    layout->setSpacing(metrics::kSpaceS);

    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search effects"));
    m_search->setClearButtonEnabled(true);
    if (hasIcon(QStringLiteral("search")))
        m_search->addAction(icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    layout->addWidget(m_search);

    m_tree = new EffectsTree;
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setIconSize(QSize(kGlyphSize, kGlyphSize));
    m_tree->setUniformRowHeights(true);
    m_tree->setIndentation(14);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setDefaultDropAction(Qt::CopyAction);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_tree, 1);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    splitter->addWidget(m_categories);
    splitter->addWidget(listSide);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({kSidebarWidth, kListWidth});
    setContentWidget(splitter);

    populate();

    connect(m_search, &QLineEdit::textChanged, this, &EffectsLibraryPanel::setFilterText);
    connect(m_categories, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        setCategoryFilter(item ? item->data(0, Qt::UserRole).toInt() : -1);
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const QString id = item->data(0, kDescriptorRole).toString();
        if (!id.isEmpty())
            activate(id);
    });
}

void EffectsLibraryPanel::populate()
{
    const EffectRegistry& registry = EffectRegistry::instance();
    QFont sectionFont = m_tree->font();
    sectionFont.setBold(true);
    for (EffectCategory category : {EffectCategory::Transition, EffectCategory::Title,
             EffectCategory::Generator, EffectCategory::VideoEffect, EffectCategory::AudioEffect}) {
        const auto descriptors = registry.byCategory(category);
        if (descriptors.isEmpty())
            continue;
        auto* section = new QTreeWidgetItem(m_tree, {sectionTitle(category)});
        section->setFlags(Qt::ItemIsEnabled);
        section->setFont(0, sectionFont);
        const QIcon glyph = categoryGlyph(category);
        for (const EffectDescriptor* d : descriptors) {
            auto* item = new QTreeWidgetItem(section, {d->name});
            item->setIcon(0, glyph);
            item->setData(0, kDescriptorRole, d->id);
            item->setToolTip(0, d->description);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        section->setExpanded(true);
        section->setData(0, kCategoryRole, int(category));
    }

    // Sidebar: "Toolbox" (everything for the timeline picture) with its categories, then the
    // audio effects, like Resolve's effects library.
    auto* toolbox = new QTreeWidgetItem(m_categories, {tr("Toolbox")});
    toolbox->setData(0, Qt::UserRole, -1);
    for (EffectCategory category : {EffectCategory::Transition, EffectCategory::Title,
             EffectCategory::Generator, EffectCategory::VideoEffect}) {
        if (registry.byCategory(category).isEmpty())
            continue;
        auto* item = new QTreeWidgetItem(toolbox, {sectionTitle(category)});
        item->setData(0, Qt::UserRole, int(category));
    }
    toolbox->setExpanded(true);
    if (!registry.byCategory(EffectCategory::AudioEffect).isEmpty()) {
        auto* audio = new QTreeWidgetItem(m_categories, {tr("Audio FX")});
        audio->setData(0, Qt::UserRole, int(EffectCategory::AudioEffect));
    }
    m_categories->setCurrentItem(toolbox);
}

void EffectsLibraryPanel::setCategoryFilter(int category)
{
    m_category = category;
    applyFilters();
}

void EffectsLibraryPanel::setFilterText(const QString& text)
{
    if (m_search->text() != text)
        m_search->setText(text); // re-enters via textChanged
    applyFilters();
}

void EffectsLibraryPanel::applyFilters()
{
    const QString needle = m_search->text().trimmed();
    const EffectRegistry& registry = EffectRegistry::instance();
    for (int s = 0; s < m_tree->topLevelItemCount(); ++s) {
        QTreeWidgetItem* section = m_tree->topLevelItem(s);
        // A search looks through every category, like Resolve's library search.
        const bool inCategory = !needle.isEmpty() || m_category < 0
            || section->data(0, kCategoryRole).toInt() == m_category;
        int visible = 0;
        for (int i = 0; i < section->childCount(); ++i) {
            QTreeWidgetItem* item = section->child(i);
            const EffectDescriptor* d = registry.find(item->data(0, kDescriptorRole).toString());
            const bool match = needle.isEmpty()
                || item->text(0).contains(needle, Qt::CaseInsensitive)
                || (d && d->description.contains(needle, Qt::CaseInsensitive));
            item->setHidden(!match);
            visible += match ? 1 : 0;
        }
        section->setHidden(!inCategory || visible == 0);
        if (!needle.isEmpty() && visible > 0)
            section->setExpanded(true);
    }
}

QStringList EffectsLibraryPanel::visibleDescriptorIds() const
{
    QStringList ids;
    for (int s = 0; s < m_tree->topLevelItemCount(); ++s) {
        const QTreeWidgetItem* section = m_tree->topLevelItem(s);
        if (section->isHidden())
            continue;
        for (int i = 0; i < section->childCount(); ++i) {
            if (!section->child(i)->isHidden())
                ids.append(section->child(i)->data(0, kDescriptorRole).toString());
        }
    }
    return ids;
}

void EffectsLibraryPanel::activate(const QString& descriptorId)
{
    effectactions::activate(m_ctx, descriptorId);
    emit effectActivated(descriptorId);
}

} // namespace kk
