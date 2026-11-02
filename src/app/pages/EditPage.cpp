// SPDX-License-Identifier: GPL-3.0-or-later
// EditPage layout and panel visibility handling.
#include "app/pages/EditPage.h"

#include "playback/PlaybackEngine.h"
#include "playback/SourcePlayer.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelFrame.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/mediapool/MetadataPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {

// Default sizes for a 1920x1080 window; QSplitter scales them to the real size.
constexpr int kLeftColumnWidth = 380;
constexpr int kViewerWidth = 560;
constexpr int kRightColumnWidth = 420;
constexpr int kTopRowHeight = 520;
constexpr int kBottomRowHeight = 440;
constexpr int kMixerWidth = 260;

QSplitter* makeSplitter(Qt::Orientation orientation)
{
    auto* splitter = new QSplitter(orientation);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(3);
    return splitter;
}

} // namespace

EditPage::EditPage(AppContext& ctx, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
{
    m_mediaPool = new MediaPoolPanel(ctx);
    m_effects = new EffectsLibraryPanel(ctx);
    m_editIndex = createPlaceholderPanel(
        tr("Edit Index"), tr("A list of all edits, markers and clips will appear here."));
    m_sourceViewer = new ViewerPanel(ViewerPanel::Mode::Source, ctx);
    m_timelineViewer = new ViewerPanel(ViewerPanel::Mode::Timeline, ctx);
    m_inspector = new InspectorPanel(ctx);
    m_metadata = new MetadataPanel(ctx);
    m_timeline = new TimelinePanel(ctx);
    m_mixer = createPlaceholderPanel(tr("Mixer"), tr("Track levels and meters will appear here."));

    m_leftColumn = makeSplitter(Qt::Vertical);
    m_leftColumn->addWidget(m_mediaPool);
    m_leftColumn->addWidget(m_effects);
    m_leftColumn->addWidget(m_editIndex);

    m_rightColumn = makeSplitter(Qt::Vertical);
    m_rightColumn->addWidget(m_inspector);
    m_rightColumn->addWidget(m_metadata);

    m_topRow = makeSplitter(Qt::Horizontal);
    m_topRow->addWidget(m_leftColumn);
    m_topRow->addWidget(m_sourceViewer);
    m_topRow->addWidget(m_timelineViewer);
    m_topRow->addWidget(m_rightColumn);
    m_topRow->setStretchFactor(1, 1);
    m_topRow->setStretchFactor(2, 1);
    m_topRow->setSizes({kLeftColumnWidth, kViewerWidth, kViewerWidth, kRightColumnWidth});

    m_bottomRow = makeSplitter(Qt::Horizontal);
    m_bottomRow->addWidget(m_timeline);
    m_bottomRow->addWidget(m_mixer);
    m_bottomRow->setStretchFactor(0, 1);
    m_bottomRow->setSizes({1920 - kMixerWidth, kMixerWidth});

    m_main = makeSplitter(Qt::Vertical);
    m_main->addWidget(m_topRow);
    m_main->addWidget(m_bottomRow);
    m_main->setStretchFactor(1, 1);
    m_main->setSizes({kTopRowHeight, kBottomRowHeight});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_main);

    m_panels = {
        {PanelId::MediaPool, m_mediaPool},
        {PanelId::Effects, m_effects},
        {PanelId::EditIndex, m_editIndex},
        {PanelId::Mixer, m_mixer},
        {PanelId::Metadata, m_metadata},
        {PanelId::Inspector, m_inspector},
    };
    // Default visibility (first start): media pool + inspector, like PLAN §4.
    for (PanelId id : {PanelId::Effects, PanelId::EditIndex, PanelId::Mixer, PanelId::Metadata})
        m_panels.value(id)->hide();
    updateColumns();

    connect(
        m_mediaPool, &MediaPoolPanel::currentMediaChanged, m_metadata, &MetadataPanel::showMedia);
    // Double-click in the media pool: load the clip into the source viewer and make it the
    // target of the transport keys (Space, J/K/L, I/O act on it).
    connect(m_mediaPool, &MediaPoolPanel::openInSourceViewer, this, [this](const QString& id) {
        if (isSingleViewer()) {
            setSingleViewer(false); // a loaded clip needs its viewer
        }
        m_sourceViewer->setSourceMedia(id);
        m_sourceViewer->activate();
        m_sourceViewer->setFocus();
    });
    // F9 / F10 / F12 take the source viewer's clip and marked range.
    m_timeline->controller()->setSourceProvider([this]() -> std::optional<SourceSelection> {
        const std::optional<SourceRange> range = m_sourceViewer->sourceSelection();
        if (!range) {
            return std::nullopt;
        }
        return SourceSelection {range->mediaId, range->inUs, range->outUs};
    });
    // F (match frame): the source of the clip under the playhead, parked on the same frame,
    // with the part the timeline uses marked.
    m_timeline->controller()->setMatchFrameHandler(
        [this](const QString& mediaId, int64_t sourceUs, int64_t inUs, int64_t outUs) {
            if (isSingleViewer())
                setSingleViewer(false);
            m_sourceViewer->setSourceMedia(mediaId);
            if (SourcePlayer* player = m_sourceViewer->sourcePlayer()) {
                const Rational fps = player->fps();
                player->setMarks(usToFrames(inUs, fps), usToFrames(outUs, fps));
                player->engine()->seek(usToFrames(sourceUs, fps));
            }
            m_sourceViewer->activate();
        });
    // Q: switch the transport target between the source and the timeline viewer.
    if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("view.toggle_source_timeline"))) {
        connect(m_ctx.actions->action(QStringLiteral("view.toggle_source_timeline")),
            &QAction::triggered, this, [this] {
                if (!isVisible())
                    return;
                ViewerPanel* next
                    = ViewerPanel::activeViewer() == m_sourceViewer || isSingleViewer()
                    ? m_timelineViewer
                    : m_sourceViewer;
                next->activate();
                next->setFocus();
            });
    }
    // Focus in the timeline makes the program viewer the transport target (ViewerTransport).
    m_timeline->setProperty("kkProgramFocus", true);
}

void EditPage::setPanelVisible(PanelId id, bool visible)
{
    QWidget* panel = m_panels.value(id);
    if (!panel || panel->isHidden() == !visible)
        return;
    panel->setVisible(visible);
    updateColumns();
    emit panelVisibilityChanged(id, visible);
}

bool EditPage::isPanelVisible(PanelId id) const
{
    const QWidget* panel = m_panels.value(id);
    return panel && !panel->isHidden();
}

void EditPage::updateColumns()
{
    // A column without visible panels would leave an empty strip: hide the column itself.
    auto anyVisible = [](std::initializer_list<QWidget*> panels) {
        return std::any_of(panels.begin(), panels.end(), [](QWidget* w) { return !w->isHidden(); });
    };
    m_leftColumn->setVisible(anyVisible({m_mediaPool, m_effects, m_editIndex}));
    m_rightColumn->setVisible(anyVisible({m_inspector, m_metadata}));
}

void EditPage::setSingleViewer(bool single)
{
    m_sourceViewer->setVisible(!single);
}

bool EditPage::isSingleViewer() const
{
    return m_sourceViewer->isHidden();
}

void EditPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("main"), m_main->saveState());
    settings.setValue(QStringLiteral("topRow"), m_topRow->saveState());
    settings.setValue(QStringLiteral("bottomRow"), m_bottomRow->saveState());
    settings.setValue(QStringLiteral("leftColumn"), m_leftColumn->saveState());
    settings.setValue(QStringLiteral("rightColumn"), m_rightColumn->saveState());
    settings.setValue(QStringLiteral("singleViewer"), isSingleViewer());
    for (PanelId id : kAllPanels)
        settings.setValue(QStringLiteral("visible/") + panelIdToString(id), isPanelVisible(id));
}

void EditPage::restoreLayout(const QSettings& settings)
{
    m_main->restoreState(settings.value(QStringLiteral("main")).toByteArray());
    m_topRow->restoreState(settings.value(QStringLiteral("topRow")).toByteArray());
    m_bottomRow->restoreState(settings.value(QStringLiteral("bottomRow")).toByteArray());
    m_leftColumn->restoreState(settings.value(QStringLiteral("leftColumn")).toByteArray());
    m_rightColumn->restoreState(settings.value(QStringLiteral("rightColumn")).toByteArray());
    // Splitter states may carry visibility too; the explicit flags below win.
    for (PanelId id : kAllPanels) {
        const QString key = QStringLiteral("visible/") + panelIdToString(id);
        if (settings.contains(key))
            setPanelVisible(id, settings.value(key).toBool());
    }
    updateColumns();
    setSingleViewer(settings.value(QStringLiteral("singleViewer"), false).toBool());
}

} // namespace kk
