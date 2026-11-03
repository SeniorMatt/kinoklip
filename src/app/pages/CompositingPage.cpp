// SPDX-License-Identifier: GPL-3.0-or-later
// CompositingPage layout and wiring of the comp.* commands.
#include "app/pages/CompositingPage.h"

#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelSplitter.h"
#include "ui/comp/CompAdapter.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/CompTransport.h"
#include "ui/comp/CompViewer.h"
#include "ui/comp/NodeEditor.h"
#include "ui/comp/NodeLibrary.h"

#include <QAction>
#include <QHBoxLayout>
#include <QSettings>
#include <QShowEvent>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

namespace kk {

namespace {

// Default sizes for a 1920x1080 window (splitters scale them).
constexpr int kLibraryWidth = 260;
constexpr int kCentreWidth = 1260;
constexpr int kInspectorWidth = 400;
constexpr int kViewersHeight = 470;
constexpr int kNodeAreaHeight = 420;
/// Layout version of the saved splitter states (bump when the structure changes).
constexpr int kLayoutVersion = 1;

QAction* registryAction(const AppContext& ctx, const QString& id)
{
    return ctx.actions && ctx.actions->contains(id) ? ctx.actions->action(id) : nullptr;
}

} // namespace

CompositingPage::CompositingPage(AppContext& ctx, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
{
    m_session = new CompSession(ctx, this);

    m_left = new CompViewer(0, ctx, m_session);
    m_right = new CompViewer(1, ctx, m_session);
    m_viewers = new PanelSplitter(Qt::Horizontal);
    m_viewers->setObjectName(QStringLiteral("CompViewersSplitter"));
    m_viewers->addWidget(m_left);
    m_viewers->addWidget(m_right);
    m_transport = new CompTransport(ctx, m_session);
    auto* viewerArea = new QWidget;
    auto* va = new QVBoxLayout(viewerArea);
    va->setContentsMargins(0, 0, 0, 0);
    va->setSpacing(0);
    va->addWidget(m_viewers, 1);
    va->addWidget(m_transport);

    m_editor = new NodeEditor(m_session, ctx.actions);
    m_toolBar = buildToolBar();
    m_nodeArea = new QWidget;
    m_nodeArea->setObjectName(QStringLiteral("CompNodeArea"));
    auto* na = new QVBoxLayout(m_nodeArea);
    na->setContentsMargins(0, 0, 0, 0);
    na->setSpacing(0);
    na->addWidget(m_toolBar);
    na->addWidget(m_editor, 1);

    m_centre = new PanelSplitter(Qt::Vertical);
    m_centre->setObjectName(QStringLiteral("CompCentreSplitter"));
    m_centre->addWidget(viewerArea);
    m_centre->addWidget(m_nodeArea);
    m_centre->setStretchFactor(1, 1);
    m_centre->setSizes({kViewersHeight, kNodeAreaHeight});

    m_library = new NodeLibraryPanel(m_session);
    m_inspector = new CompInspector(ctx, m_session);
    m_columns = new PanelSplitter(Qt::Horizontal);
    m_columns->setObjectName(QStringLiteral("CompColumnsSplitter"));
    m_columns->addWidget(m_library);
    m_columns->addWidget(m_centre);
    m_columns->addWidget(m_inspector);
    m_columns->setStretchFactor(1, 1);
    m_columns->setSizes({kLibraryWidth, kCentreWidth, kInspectorWidth});
    m_library->hide(); // toggle (interface bar "Effects"), off by default

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_columns);

    m_selectNode = new SelectNodePopup(m_session, this);
    connect(m_editor, &NodeEditor::viewChanged, m_session, &CompSession::setViewState);
    connectActions();
}

CompositingPage::~CompositingPage() = default;

QWidget* CompositingPage::buildToolBar()
{
    auto* bar = new QWidget;
    bar->setObjectName(QStringLiteral("CompNodeToolBar"));
    auto* l = new QHBoxLayout(bar);
    l->setContentsMargins(metrics::kSpaceM, metrics::kSpaceXs, metrics::kSpaceM, metrics::kSpaceXs);
    l->setSpacing(metrics::kSpaceXs);
    for (const QString& type : compadapter::toolbarTypes()) {
        const compadapter::NodeTypeInfo* info = compadapter::nodeType(type);
        auto* b = new QToolButton(bar);
        b->setObjectName(QStringLiteral("CompTool:") + type);
        b->setText(info->name);
        b->setToolTip(tr("Add %1 after the selected node").arg(info->name));
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QToolButton::clicked, this, [this, type] {
            m_session->addNode(type);
            m_editor->setFocus();
        });
        l->addWidget(b);
    }
    l->addStretch(1);
    return bar;
}

QToolButton* CompositingPage::toolButton(const QString& type) const
{
    return m_toolBar->findChild<QToolButton*>(QStringLiteral("CompTool:") + type);
}

void CompositingPage::connectActions()
{
    auto on = [this](const char* id, auto handler) {
        if (QAction* a = registryAction(m_ctx, QLatin1String(id)))
            connect(a, &QAction::triggered, this, [this, handler] {
                if (isVisible())
                    handler();
            });
    };
    CompSession* s = m_session;
    on("comp.select_node", [this] { showSelectNode(); });
    on("comp.add_note", [this, s] { s->addNote(m_editor->pastePosition()); });
    on("comp.cut", [s] { s->cutSelection(); });
    on("comp.copy", [s] { s->copySelection(); });
    on("comp.paste", [this, s] { s->paste(m_editor->pastePosition()); });
    on("comp.duplicate", [s] { s->duplicateSelection(); });
    on("comp.delete", [s] { s->deleteSelection(); });
    on("comp.select_all", [s] { s->selectAll(); });
    on("comp.rename", [this, s] {
        if (s->currentNode())
            m_editor->startRename(s->currentNode());
    });
    on("comp.pass_through", [s] { s->togglePassThrough(); });
    on("comp.lock", [s] { s->toggleLock(); });
    on("comp.swap_inputs", [s] { s->swapInputs(); });
    on("comp.view_left", [s] { s->setViewerNode(0, s->currentNode()); });
    on("comp.view_right", [s] { s->setViewerNode(1, s->currentNode()); });
    on("comp.fit", [this] { m_editor->fitToView(); });
    if (QAction* snap = registryAction(m_ctx, QStringLiteral("comp.snap_grid"))) {
        snap->setChecked(m_session->snapToGrid());
        connect(snap, &QAction::toggled, m_session, &CompSession::setSnapToGrid);
    }
    // The checkable commands show the current node's state.
    auto syncChecks = [this] {
        const CompNode* n = m_session->composition().node(m_session->currentNode());
        if (QAction* a = registryAction(m_ctx, QStringLiteral("comp.pass_through")))
            a->setChecked(n && !n->enabled);
        if (QAction* a = registryAction(m_ctx, QStringLiteral("comp.lock")))
            a->setChecked(n && n->locked);
    };
    connect(m_session, &CompSession::selectionChanged, this, syncChecks);
    connect(m_session, &CompSession::compositionChanged, this, syncChecks);

    // Clip > Reset Composition (Edit page timeline menu too): the selected clips.
    if (QAction* reset = registryAction(m_ctx, QStringLiteral("clip.reset_composition"))) {
        connect(reset, &QAction::triggered, this, [this] {
            QVector<quint64> clips = m_ctx.selection ? m_ctx.selection->clips() : QVector<quint64> {};
            if (isVisible() && m_session->clipId())
                clips = {m_session->clipId()};
            compadapter::resetCompositions(*m_ctx.project, clips);
        });
    }
}

void CompositingPage::showSelectNode()
{
    if (!m_session->clipId())
        return;
    const QPoint local = m_editor->mapFromGlobal(QCursor::pos());
    const QPoint at = m_editor->rect().contains(local) ? QCursor::pos()
                                                       : m_editor->mapToGlobal(m_editor->rect().center());
    m_selectNode->popup(at);
}

void CompositingPage::openClip(quint64 clipId)
{
    m_session->openClip(clipId ? clipId : m_session->clipForPage());
}

void CompositingPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    if (!event->spontaneous())
        openClip();
}

// ---- Panels and layout ----

bool CompositingPage::hasPanel(PanelId id) const
{
    return id == PanelId::Effects || id == PanelId::Nodes || id == PanelId::Inspector;
}

void CompositingPage::setPanelVisible(PanelId id, bool visible)
{
    QWidget* panel = id == PanelId::Effects ? static_cast<QWidget*>(m_library)
        : id == PanelId::Nodes              ? m_nodeArea
        : id == PanelId::Inspector          ? static_cast<QWidget*>(m_inspector)
                                            : nullptr;
    if (!panel || panel->isHidden() == !visible)
        return;
    panel->setVisible(visible);
    emit panelVisibilityChanged(id, visible);
}

bool CompositingPage::isPanelVisible(PanelId id) const
{
    switch (id) {
    case PanelId::Effects:
        return !m_library->isHidden();
    case PanelId::Nodes:
        return !m_nodeArea->isHidden();
    case PanelId::Inspector:
        return !m_inspector->isHidden();
    default:
        return false;
    }
}

void CompositingPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("layoutVersion"), kLayoutVersion);
    settings.setValue(QStringLiteral("columns"), m_columns->saveState());
    settings.setValue(QStringLiteral("centre"), m_centre->saveState());
    settings.setValue(QStringLiteral("viewers"), m_viewers->saveState());
    for (PanelId id : {PanelId::Effects, PanelId::Nodes, PanelId::Inspector})
        settings.setValue(QStringLiteral("visible/") + panelIdToString(id), isPanelVisible(id));
}

void CompositingPage::restoreLayout(const QSettings& settings)
{
    if (settings.value(QStringLiteral("layoutVersion"), 0).toInt() == kLayoutVersion) {
        m_columns->restoreState(settings.value(QStringLiteral("columns")).toByteArray());
        m_centre->restoreState(settings.value(QStringLiteral("centre")).toByteArray());
        m_viewers->restoreState(settings.value(QStringLiteral("viewers")).toByteArray());
    }
    // Splitter states carry visibility too; the explicit flags win.
    for (PanelId id : {PanelId::Effects, PanelId::Nodes, PanelId::Inspector}) {
        const QString key = QStringLiteral("visible/") + panelIdToString(id);
        if (settings.contains(key))
            setPanelVisible(id, settings.value(key).toBool());
    }
}

} // namespace kk
