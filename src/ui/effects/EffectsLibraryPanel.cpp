// SPDX-License-Identifier: GPL-3.0-or-later
// EffectsLibraryPanel implementation: category tree with painted glyphs, search, drag source.
#include "ui/effects/EffectsLibraryPanel.h"

#include "core/AudioTransitions.h"
#include "core/EffectRegistry.h"
#include "core/Preferences.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelSplitter.h"
#include "ui/common/SchemeColors.h"
#include "ui/effects/EffectActions.h"
#include "ui/help/Guide.h"

#include <QApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace kk {

namespace {

constexpr int kGlyphSize = 20;
constexpr int kDescriptorRole = Qt::UserRole; ///< descriptor id (empty for section rows)
constexpr int kCategoryRole = Qt::UserRole + 1; ///< EffectCategory of a section row
constexpr int kFolderRole = Qt::UserRole + 2; ///< EffectDescriptor::group of a folder row
constexpr int kSidebarWidth = 150; ///< px, category sidebar
constexpr int kListWidth = 230;
constexpr int kScrubLineWidth = 2; ///< px, Live Preview scrub line
constexpr int kScrubFillAlpha = 48; ///< tint of the part of the row already passed

/// Tree that exports mime::Effect for drags (QTreeWidget's default exports its own format).
class EffectsTree final : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;

    /// Called before a drag starts (ends the live preview).
    std::function<void()> dragStarting;

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        if (dragStarting)
            dragStarting();
        QTreeWidget::startDrag(supportedActions);
    }

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

public:
    /// Live Preview scrub line: drawn over @p item at @p position (0..1) of its row.
    void setScrub(QTreeWidgetItem* item, double position)
    {
        const QPersistentModelIndex index(indexFromItem(item));
        if (index == m_scrubIndex && position == m_scrubPosition)
            return;
        updateScrubRow();
        m_scrubIndex = index;
        m_scrubPosition = position;
        updateScrubRow();
    }
    void clearScrub()
    {
        if (!m_scrubIndex.isValid())
            return;
        updateScrubRow();
        m_scrubIndex = QPersistentModelIndex();
    }
    /// Viewport x of the scrub line, -1 = none or scrolled out of view.
    [[nodiscard]] int scrubX() const
    {
        if (!m_scrubIndex.isValid())
            return -1;
        const QRect row = visualRect(m_scrubIndex);
        if (!row.isValid() || !row.intersects(viewport()->rect()))
            return -1;
        return row.left() + int(std::lround(m_scrubPosition * (row.width() - 1)));
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QTreeWidget::paintEvent(event);
        const int x = scrubX();
        if (x < 0)
            return;
        // Resolve-style position line: the passed part tinted, a playhead-coloured line at
        // the pointer, spanning the row. Computed from the row's current rectangle, so it
        // follows scrolling and resizing.
        const QRect row = visualRect(m_scrubIndex);
        const SchemeColors& colors = schemeColors();
        QPainter p(viewport());
        p.fillRect(QRect(row.left(), row.top(), x - row.left(), row.height()),
            withAlpha(colors.playhead, kScrubFillAlpha));
        p.fillRect(QRect(x - kScrubLineWidth / 2, row.top(), kScrubLineWidth, row.height()),
            colors.playhead);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && currentItem()) {
            emit itemDoubleClicked(currentItem(), 0);
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }

private:
    void updateScrubRow()
    {
        if (m_scrubIndex.isValid())
            viewport()->update(visualRect(m_scrubIndex));
    }

    QPersistentModelIndex m_scrubIndex; ///< row of the scrub line (invalid = none)
    double m_scrubPosition = 0.0;
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
    // Tints from the scheme, the same roles as the clips these effects make on the timeline.
    const SchemeColors& sc = schemeColors();
    QColor tint;
    switch (category) {
    case EffectCategory::Transition:
        tint = sc.transition;
        break;
    case EffectCategory::AudioTransition:
        tint = mixColors(sc.transition, sc.positive, 0.5);
        break;
    case EffectCategory::Title:
        tint = sc.visited;
        break;
    case EffectCategory::Generator:
        tint = mixColors(sc.visited, sc.neutral, 0.55);
        break;
    case EffectCategory::Effects:
        tint = mixColors(sc.link, sc.textDim, 0.4);
        break;
    case EffectCategory::AudioEffect:
        tint = sc.positive;
        break;
    default:
        tint = sc.link;
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
    case EffectCategory::Effects: {
        // Adjustment clip: stacked layers, the top one outlined (it acts on those below).
        p.setPen(Qt::NoPen);
        p.setBrush(tint);
        p.drawRect(QRectF(tile.left() + 3, tile.bottom() - 4.5, tile.width() - 6, 2));
        p.drawRect(QRectF(tile.left() + 3, tile.bottom() - 8, tile.width() - 6, 2));
        p.setPen(QPen(tint.lighter(140), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(tile.left() + 3, tile.top() + 2.5, tile.width() - 6, 3));
        break;
    }
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

/// Audio transition glyph: the category tile with the transition's two gain curves (incoming
/// rising in full tint, outgoing falling lighter), so the six shapes tell apart at a glance.
QIcon audioTransitionGlyph(const QString& descriptorId)
{
    QPixmap pixmap(kGlyphSize * 2, kGlyphSize * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF tile(1.5, 3.5, kGlyphSize - 3, kGlyphSize - 7);
    const SchemeColors& sc = schemeColors();
    const QColor tint = mixColors(sc.transition, sc.positive, 0.5);
    QColor fill = tint;
    fill.setAlphaF(0.22f);
    p.setPen(QPen(tint, 1.2));
    p.setBrush(fill);
    p.drawRoundedRect(tile, 2.5, 2.5);

    const QRectF plot = tile.adjusted(2.5, 2, -2.5, -2);
    const audiotransitions::Curve curve = audiotransitions::curveFor(descriptorId);
    QPainterPath in, out;
    constexpr int kSteps = 16;
    for (int i = 0; i <= kSteps; ++i) {
        const double t = double(i) / kSteps;
        const qreal x = plot.left() + t * plot.width();
        const QPointF pi(x, plot.bottom() - audiotransitions::gainIn(curve, t) * plot.height());
        const QPointF po(x, plot.bottom() - audiotransitions::gainOut(curve, t) * plot.height());
        if (i == 0) {
            in.moveTo(pi);
            out.moveTo(po);
        } else {
            in.lineTo(pi);
            out.lineTo(po);
        }
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(tint.lighter(150), 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(out);
    p.setPen(QPen(tint.lighter(115), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(in);
    return QIcon(pixmap);
}

/// Library icon of descriptor @p d: the audio transitions draw their curve, everything else
/// its category glyph (@p categoryIcon, drawn once per section).
QIcon effectGlyph(const EffectDescriptor& d, const QIcon& categoryIcon)
{
    return d.category == EffectCategory::AudioTransition ? audioTransitionGlyph(d.id)
                                                         : categoryIcon;
}

/// Calls @p fn for every effect row (rows with a descriptor id) below @p parent, folders
/// included.
template<typename Fn>
void forEachEffect(QTreeWidgetItem* parent, Fn&& fn)
{
    for (int i = 0; i < parent->childCount(); ++i) {
        QTreeWidgetItem* item = parent->child(i);
        if (item->data(0, kDescriptorRole).toString().isEmpty())
            forEachEffect(item, fn);
        else
            fn(item);
    }
}

QString sectionTitle(EffectCategory category)
{
    switch (category) {
    case EffectCategory::Transition:
        return EffectsLibraryPanel::tr("Video Transitions");
    case EffectCategory::AudioTransition:
        return EffectsLibraryPanel::tr("Audio Transitions");
    case EffectCategory::Title:
        return EffectsLibraryPanel::tr("Titles");
    case EffectCategory::Generator:
        return EffectsLibraryPanel::tr("Generators");
    case EffectCategory::VideoEffect:
        return EffectsLibraryPanel::tr("Video Effects");
    case EffectCategory::AudioEffect:
        return EffectsLibraryPanel::tr("Audio Effects");
    case EffectCategory::Effects:
        return EffectsLibraryPanel::tr("Effects");
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
    setPanelKey(QStringLiteral("effects")); // in-app help: panel:effects
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

    auto* tree = new EffectsTree;
    tree->dragStarting = [this] { endHover(); }; // a drag ends the live preview
    m_tree = tree;
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
    // Live Preview follows the pointer over the entries (hoverAt()).
    m_tree->setMouseTracking(true);
    m_tree->viewport()->setMouseTracking(true);
    m_tree->viewport()->installEventFilter(this);
    layout->addWidget(m_tree, 1);

    // Options menu ("…" at the right of the title bar), like the panels' option menus in
    // Resolve: Live Preview, on by default and remembered.
    auto* options = new QToolButton;
    options->setObjectName(QStringLiteral("EffectsOptions"));
    options->setText(QStringLiteral("\u2026"));
    options->setToolTip(tr("Effects library options"));
    options->setAutoRaise(true);
    options->setPopupMode(QToolButton::InstantPopup);
    auto* optionsMenu = new QMenu(options);
    m_livePreviewAction = optionsMenu->addAction(tr("Live Preview"));
    m_livePreviewAction->setObjectName(QStringLiteral("EffectsLivePreview"));
    m_livePreviewAction->setCheckable(true);
    m_livePreviewAction->setChecked(prefs::effectsLivePreview());
    m_livePreviewAction->setToolTip(
        tr("Show the entry under the pointer applied to the frame in the timeline viewer"));
    connect(m_livePreviewAction, &QAction::toggled, this, [this](bool on) {
        prefs::setEffectsLivePreview(on);
        if (!on)
            endHover();
    });
    options->setMenu(optionsMenu);
    addTitleBarWidget(options);

    auto* splitter = new PanelSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    splitter->addWidget(m_categories);
    splitter->addWidget(listSide);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({kSidebarWidth, kListWidth});
    setContentWidget(splitter);

    populate();
    // The category glyphs are pixmaps in the scheme's colours: redraw them on a switch.
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, this, [this] {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* section = m_tree->topLevelItem(i);
            const QIcon glyph
                = categoryGlyph(EffectCategory(section->data(0, kCategoryRole).toInt()));
            forEachEffect(section, [&](QTreeWidgetItem* item) {
                const EffectDescriptor* d
                    = EffectRegistry::instance().find(item->data(0, kDescriptorRole).toString());
                item->setIcon(0, d ? effectGlyph(*d, glyph) : glyph);
            });
        }
    });

    connect(m_search, &QLineEdit::textChanged, this, &EffectsLibraryPanel::setFilterText);
    connect(m_categories, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        setCategoryFilter(item ? item->data(0, Qt::UserRole).toInt() : -1);
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const QString id = item->data(0, kDescriptorRole).toString();
        if (!id.isEmpty())
            activate(id);
    });
    // In-app help: F1 over the list opens the selected entry's article (effect:<id>), the
    // context menu offers "How to use <name>…" when the guide has one.
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        const QString id = item ? item->data(0, kDescriptorRole).toString() : QString();
        guide::setHelpContext(m_tree, id.isEmpty() ? QString() : QStringLiteral("effect:") + id);
    });
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = m_tree->itemAt(pos);
        if (!item)
            return;
        m_tree->setCurrentItem(item);
        QMenu* menu = createContextMenu(item->data(0, kDescriptorRole).toString());
        if (!menu)
            return;
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(m_tree->viewport()->mapToGlobal(pos));
    });
}

QMenu* EffectsLibraryPanel::createContextMenu(const QString& descriptorId)
{
    const EffectDescriptor* d = EffectRegistry::instance().find(descriptorId);
    if (!d)
        return nullptr;
    auto* menu = new QMenu(this);
    if (QAction* howTo = guide::createHowToAction(QStringLiteral("effect:") + d->id, d->name, menu))
        menu->addAction(howTo);
    if (menu->isEmpty()) {
        delete menu;
        return nullptr;
    }
    return menu;
}

void EffectsLibraryPanel::populate()
{
    const EffectRegistry& registry = EffectRegistry::instance();
    QFont sectionFont = m_tree->font();
    sectionFont.setBold(true);
    for (EffectCategory category : {EffectCategory::Transition, EffectCategory::AudioTransition,
             EffectCategory::Title, EffectCategory::Generator, EffectCategory::Effects,
             EffectCategory::VideoEffect, EffectCategory::AudioEffect}) {
        const auto descriptors = registry.byCategory(category);
        if (descriptors.isEmpty())
            continue;
        auto* section = new QTreeWidgetItem(m_tree, {sectionTitle(category)});
        section->setFlags(Qt::ItemIsEnabled);
        section->setFont(0, sectionFont);
        const QIcon glyph = categoryGlyph(category);
        auto addEffect = [&](QTreeWidgetItem* parent, const EffectDescriptor* d) {
            auto* item = new QTreeWidgetItem(parent, {d->name});
            item->setIcon(0, effectGlyph(*d, glyph));
            item->setData(0, kDescriptorRole, d->id);
            item->setToolTip(0, d->description);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        };
        // Video effects sit in folders (Blur, Color, Key, ...) in alphabetical order, like an
        // editor's effects library; effects without a folder follow directly.
        for (const QString& group : registry.groups(category)) {
            auto* folder = new QTreeWidgetItem(section, {EffectRegistry::groupName(group)});
            folder->setFlags(Qt::ItemIsEnabled);
            folder->setData(0, kFolderRole, group);
            for (const EffectDescriptor* d : descriptors) {
                if (d->group == group)
                    addEffect(folder, d);
            }
            folder->setExpanded(true);
        }
        for (const EffectDescriptor* d : descriptors) {
            if (d->group.isEmpty())
                addEffect(section, d);
        }
        section->setExpanded(true);
        section->setData(0, kCategoryRole, int(category));
    }

    // Sidebar: "Toolbox" (the transitions, titles, generators and effects) with its
    // categories, then the audio effects, like Resolve's effects library.
    auto* toolbox = new QTreeWidgetItem(m_categories, {tr("Toolbox")});
    toolbox->setData(0, Qt::UserRole, -1);
    for (EffectCategory category :
        {EffectCategory::Transition, EffectCategory::AudioTransition, EffectCategory::Title,
            EffectCategory::Generator, EffectCategory::Effects, EffectCategory::VideoEffect}) {
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

bool EffectsLibraryPanel::livePreviewEnabled() const
{
    return m_livePreviewAction->isChecked();
}

void EffectsLibraryPanel::hoverAt(QPoint viewportPos)
{
    QTreeWidgetItem* item = m_tree->itemAt(viewportPos);
    const QString id = item ? item->data(0, kDescriptorRole).toString() : QString();
    if (id.isEmpty() || !livePreviewEnabled() || !isVisible()) {
        endHover();
        return;
    }
    // The pointer's place along the row scrubs transitions and titles (Resolve).
    const QRect row = m_tree->visualItemRect(item);
    const double position = row.width() > 1
        ? std::clamp(double(viewportPos.x() - row.left()) / (row.width() - 1), 0.0, 1.0)
        : 0.5;
    m_hoveredId = id;
    // Transitions and titles scrub: a line shows where in them the pointer is (Resolve).
    auto* tree = static_cast<EffectsTree*>(m_tree);
    const EffectDescriptor* desc = EffectRegistry::instance().find(id);
    if (desc
        && (desc->category == EffectCategory::Transition
            || desc->category == EffectCategory::Title))
        tree->setScrub(item, position);
    else
        tree->clearScrub();
    emit livePreviewRequested(id, position);
}

int EffectsLibraryPanel::scrubLineX() const
{
    return static_cast<const EffectsTree*>(m_tree)->scrubX();
}

void EffectsLibraryPanel::endHover()
{
    static_cast<EffectsTree*>(m_tree)->clearScrub();
    if (m_hoveredId.isEmpty())
        return;
    m_hoveredId.clear();
    emit livePreviewEnded();
}

bool EffectsLibraryPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_tree->viewport()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            const auto* move = static_cast<QMouseEvent*>(event);
            if (move->buttons() == Qt::NoButton)
                hoverAt(move->position().toPoint());
            break;
        }
        case QEvent::Leave:
        case QEvent::Wheel: // the entry under the pointer changes; the next move picks it
            endHover();
            break;
        default:
            break;
        }
    }
    return PanelFrame::eventFilter(watched, event);
}

void EffectsLibraryPanel::hideEvent(QHideEvent* event)
{
    endHover();
    PanelFrame::hideEvent(event);
}

void EffectsLibraryPanel::setCategoryFilter(int category)
{
    endHover();
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
    endHover(); // the rows move
    const QString needle = m_search->text().trimmed();
    const EffectRegistry& registry = EffectRegistry::instance();
    for (int s = 0; s < m_tree->topLevelItemCount(); ++s) {
        QTreeWidgetItem* section = m_tree->topLevelItem(s);
        // A search looks through every category, like Resolve's library search.
        const bool inCategory = !needle.isEmpty() || m_category < 0
            || section->data(0, kCategoryRole).toInt() == m_category;
        int visible = 0;
        forEachEffect(section, [&](QTreeWidgetItem* item) {
            const EffectDescriptor* d = registry.find(item->data(0, kDescriptorRole).toString());
            const bool match = needle.isEmpty()
                || item->text(0).contains(needle, Qt::CaseInsensitive)
                || (d && d->description.contains(needle, Qt::CaseInsensitive));
            item->setHidden(!match);
            visible += match ? 1 : 0;
        });
        // Folders: hidden without a matching effect, opened while searching.
        for (int i = 0; i < section->childCount(); ++i) {
            QTreeWidgetItem* folder = section->child(i);
            if (folder->data(0, kFolderRole).toString().isEmpty())
                continue;
            int inFolder = 0;
            forEachEffect(
                folder, [&](QTreeWidgetItem* item) { inFolder += item->isHidden() ? 0 : 1; });
            folder->setHidden(inFolder == 0);
            if (!needle.isEmpty() && inFolder > 0)
                folder->setExpanded(true);
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
        forEachEffect(const_cast<QTreeWidgetItem*>(section), [&](QTreeWidgetItem* item) {
            if (!item->isHidden() && !(item->parent() && item->parent()->isHidden()))
                ids.append(item->data(0, kDescriptorRole).toString());
        });
    }
    return ids;
}

void EffectsLibraryPanel::activate(const QString& descriptorId)
{
    const EffectDescriptor* desc = EffectRegistry::instance().find(descriptorId);
    const bool transition = desc
        && (desc->category == EffectCategory::Transition
            || desc->category == EffectCategory::AudioTransition);
    if (!(transition && m_transitionHandler && m_transitionHandler(descriptorId)))
        effectactions::activate(m_ctx, descriptorId);
    emit effectActivated(descriptorId);
}

} // namespace kk
