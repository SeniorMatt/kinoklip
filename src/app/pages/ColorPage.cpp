// SPDX-License-Identifier: GPL-3.0-or-later
// ColorPage layout and wiring of the color.* commands.
#include "app/pages/ColorPage.h"

#include "core/EditOps.h"
#include "core/GradeGallery.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/color/ColorBrowsers.h"
#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorNodeEditor.h"
#include "ui/color/ColorPalettes.h"
#include "ui/color/ColorSession.h"
#include "ui/color/ColorViewerOverlay.h"
#include "ui/color/ColorWidgets.h"
#include "ui/color/QualifierPick.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelFrame.h"
#include "ui/common/PanelSplitter.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/common/TimelineCommandSuspension.h"
#include "ui/scopes/ScopeFeeder.h"
#include "ui/scopes/ScopeView.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSettings>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace kk {

namespace {

/// Layout version of the saved splitter states (bump when the structure changes).
constexpr int kLayoutVersion = 2;
/// Node editor thumbnails.
constexpr QSize kNodeThumb(88, 50);
/// Quiet time before node thumbnails and the highlight follow an edit.
constexpr int kPictureDelayMs = 40;

QAction* registryAction(const AppContext& ctx, const QString& id)
{
    return ctx.actions && ctx.actions->contains(id) ? ctx.actions->action(id) : nullptr;
}

QToolButton* headerButton(const QString& text, const QString& name, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setObjectName(name);
    b->setText(text);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

/// A row of checkable tab buttons over a stack (the palette selectors).
QWidget* tabRow(
    QButtonGroup* group, const QStringList& names, const QString& objectPrefix, QWidget* parent)
{
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("ColorPaletteBar"));
    auto* l = new QHBoxLayout(row);
    l->setContentsMargins(4, 2, 4, 2);
    l->setSpacing(1);
    for (int i = 0; i < names.size(); ++i) {
        auto* b = headerButton(names[i], objectPrefix + QString::number(i), row);
        group->addButton(b, i);
        l->addWidget(b);
    }
    l->addStretch(1);
    group->setExclusive(true);
    group->button(0)->setChecked(true);
    return row;
}

/// Gives @p w the tooltip @p text with the current key of command @p id (it follows Keyboard
/// Customization).
void toolTipWithKey(const AppContext& ctx, QWidget* w, const char* id, const QString& text)
{
    if (ctx.actions && ctx.actions->contains(QLatin1String(id)))
        ctx.actions->bindToolTip(w, QLatin1String(id), text);
    else
        w->setToolTip(text);
}

/// True when @p w takes typed text (the page then leaves the keys to it).
bool isTextInput(const QWidget* w)
{
    return qobject_cast<const QLineEdit*>(w) || qobject_cast<const QAbstractSpinBox*>(w)
        || qobject_cast<const QPlainTextEdit*>(w);
}

} // namespace

ColorPage::ColorPage(AppContext& ctx, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
{
    setObjectName(QStringLiteral("ColorPage"));
    m_session = new ColorSession(ctx, this);
    m_frames = new ColorFrameSource(ctx.playback, ctx.project, m_session, this);
    m_pictureTimer = new QTimer(this);
    m_pictureTimer->setSingleShot(true);
    m_pictureTimer->setInterval(kPictureDelayMs);
    connect(m_pictureTimer, &QTimer::timeout, this, &ColorPage::refreshPictures);

    // ---- Top: gallery | viewer | nodes ----
    m_gallery = new ColorGallery(m_session, ctx.project);
    m_galleryFrame = new PanelFrame(tr("Gallery"));
    m_galleryFrame->setContentWidget(m_gallery);
    if (QAction* grab = registryAction(ctx, QStringLiteral("color.grab_still")))
        m_galleryFrame->addTitleBarAction(grab);

    QWidget* viewer = buildViewer();

    m_nodes = new ColorNodeEditor(m_session, ctx.actions);
    m_nodesFrame = new PanelFrame(tr("Nodes"));
    m_nodesFrame->setContentWidget(m_nodes);
    m_top = new PanelSplitter(Qt::Horizontal);
    m_top->setObjectName(QStringLiteral("ColorTopSplitter"));
    m_top->addWidget(m_galleryFrame);
    m_top->addWidget(viewer);
    m_top->addWidget(m_nodesFrame);
    m_top->setStretchFactor(0, 0);
    m_top->setStretchFactor(1, 1);
    m_top->setStretchFactor(2, 0);
    m_top->setCollapsible(1, false);

    // ---- Middle: clips strip + mini timeline ----
    m_middle = new QWidget;
    m_middle->setObjectName(QStringLiteral("ColorMiddle"));
    auto* ml = new QVBoxLayout(m_middle);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(0);
    m_strip = new ColorClipStrip(m_session, ctx.project, m_frames);
    m_mini = new ColorMiniTimeline(m_session, ctx.project);
    m_mini->setFixedHeight(30);
    ml->addWidget(m_strip);
    ml->addWidget(m_mini);
    m_middle->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // ---- Bottom: palettes | scopes / key ----
    QWidget* palettes = buildPalettes();
    m_scopes = new ScopesPanel(QStringLiteral("colorPage/scopeType"));
    m_feeder = new ScopeFeeder(ctx.playback, this);
    connect(m_feeder, &ScopeFeeder::imageReady, m_scopes, &ScopesPanel::setImage);
    m_key = new KeyPanel(m_session, m_frames);
    m_rightStack = new QStackedWidget;
    m_rightStack->addWidget(m_scopes);
    m_rightStack->addWidget(m_key);
    auto* right = new QWidget;
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    m_rightButtons = new QButtonGroup(this);
    rl->addWidget(
        tabRow(m_rightButtons, {tr("Scopes"), tr("Key")}, QStringLiteral("ColorRightTab_"), right));
    rl->addWidget(m_rightStack, 1);
    connect(
        m_rightButtons, &QButtonGroup::idClicked, m_rightStack, &QStackedWidget::setCurrentIndex);
    m_scopesFrame = new PanelFrame(tr("Scopes"));
    m_scopesFrame->setContentWidget(right);
    m_scopesFrame->setTitleBarVisible(false); // the Scopes / Key tabs are the header

    m_bottom = new PanelSplitter(Qt::Horizontal);
    m_bottom->setObjectName(QStringLiteral("ColorBottomSplitter"));
    m_bottom->addWidget(palettes);
    m_bottom->addWidget(m_scopesFrame);
    m_bottom->setStretchFactor(0, 3);
    m_bottom->setStretchFactor(1, 1);
    m_bottom->setCollapsible(0, false);

    m_rows = new PanelSplitter(Qt::Vertical);
    m_rows->setObjectName(QStringLiteral("ColorRowsSplitter"));
    m_rows->addWidget(m_top);
    m_rows->addWidget(m_middle);
    m_rows->addWidget(m_bottom);
    m_rows->setStretchFactor(0, 5);
    m_rows->setStretchFactor(1, 0);
    m_rows->setStretchFactor(2, 4);
    m_rows->setCollapsible(0, false);
    m_rows->setCollapsible(2, false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_rows);

    // ---- Wiring ----
    if (ctx.playback) {
        connect(ctx.playback, &PlaybackEngine::frameReady, this, [this](const RenderedFramePtr& f) {
            if (isVisible() && f)
                m_surface->setRenderedFrame(f);
        });
    }
    connect(ctx.project->timeline(), &Timeline::changed, this, [this] {
        const TimelineData& d = m_ctx.project->timeline()->data();
        m_surface->setAspect(QSize(d.width, d.height));
        updateHeader();
    });
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, &ColorPage::updateTimecode);
    connect(m_session, &ColorSession::clipChanged, this, &ColorPage::updateHeader);
    connect(m_session, &ColorSession::gradeChanged, this, &ColorPage::updateHeader);
    connect(m_session, &ColorSession::bypassAllChanged, this, &ColorPage::updateHeader);
    connect(m_session, &ColorSession::gradeChanged, m_pictureTimer, qOverload<>(&QTimer::start));
    connect(
        m_session, &ColorSession::selectedNodeChanged, m_pictureTimer, qOverload<>(&QTimer::start));
    connect(m_frames, &ColorFrameSource::inputChanged, m_pictureTimer, qOverload<>(&QTimer::start));
    connect(m_targetButton, &QToolButton::toggled, this, [this](bool timeline) {
        m_session->setTarget(
            timeline ? ColorSession::Target::Timeline : ColorSession::Target::Clip);
    });
    connect(m_bypassButton, &QToolButton::toggled, m_session, &ColorSession::setBypassAll);
    connect(m_gallery, &ColorGallery::wipeStillChanged, this, [this](quint64 id) {
        if (id && !m_wipeOn) {
            setWipe(true);
            return;
        }
        if (!id && m_wipeOn) {
            setWipe(false);
            return;
        }
        updateWipe();
    });
    connect(m_ctx.project->gallery(), &GradeGallery::changed, this, &ColorPage::updateWipe);
    connect(m_overlay, &ColorViewerOverlay::pickRequested, this, &ColorPage::pickQualifier);
    connect(m_qualifier, &QualifierPanel::pickModeChanged, this,
        [this] { m_overlay->setPickMode(m_qualifier->pickMode().has_value()); });
    connect(m_qualifier, &QualifierPanel::highlightModeChanged, this, &ColorPage::refreshPictures);
    connect(m_qualifier->highlightButton(), &QToolButton::toggled, this, &ColorPage::setHighlight);
    connect(m_highlightButton, &QToolButton::toggled, this, &ColorPage::setHighlight);
    connect(m_wipeButton, &QToolButton::toggled, this, &ColorPage::setWipe);
    connectActions();
    updateHeader();
    updateTimecode();

    // No Edit page timeline here: its editing commands are off while the page is shown (so
    // Ctrl+D is Enable / Disable Node, Ctrl+C copies the grade); the transport, page keys,
    // undo and the color.* commands stay.
    new TimelineCommandSuspension(this, ctx.actions);
}

ColorPage::~ColorPage()
{
    if (m_filterInstalled)
        qApp->removeEventFilter(this);
    if (m_ctx.playback)
        m_ctx.playback->removeOutputClient(this);
}

QWidget* ColorPage::buildViewer()
{
    m_viewerFrame = new PanelFrame(QString());
    m_viewerFrame->setTitleCentered(true);
    // Leading: Clip / Timeline grade.
    auto* targets = new QWidget;
    auto* tl = new QHBoxLayout(targets);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(0);
    m_clipButton = headerButton(tr("Clip"), QStringLiteral("ColorClipTargetButton"), targets);
    m_clipButton->setToolTip(tr("Grade the clip under the playhead"));
    m_targetButton = headerButton(tr("Timeline"), QStringLiteral("ColorTargetButton"), targets);
    m_targetButton->setToolTip(tr("Grade the whole timeline (applied after the clip grades)"));
    auto* group = new QButtonGroup(targets);
    group->addButton(m_clipButton);
    group->addButton(m_targetButton);
    group->setExclusive(true);
    m_clipButton->setChecked(true);
    tl->addWidget(m_clipButton);
    tl->addWidget(m_targetButton);
    m_viewerFrame->addTitleBarLeadingWidget(targets);
    m_highlightButton
        = headerButton(tr("Highlight"), QStringLiteral("ColorHighlightButton"), nullptr);
    toolTipWithKey(m_ctx, m_highlightButton, "color.highlight", tr("Show the current node's key"));
    m_wipeButton = headerButton(tr("Wipe"), QStringLiteral("ColorWipeButton"), nullptr);
    toolTipWithKey(
        m_ctx, m_wipeButton, "color.wipe", tr("Split the viewer with the selected gallery still"));
    m_bypassButton = headerButton(tr("Bypass"), QStringLiteral("ColorBypassButton"), nullptr);
    toolTipWithKey(
        m_ctx, m_bypassButton, "color.bypass_all", tr("Bypass every grade in the viewers"));
    m_viewerFrame->addTitleBarWidget(m_highlightButton);
    m_viewerFrame->addTitleBarWidget(m_wipeButton);
    m_viewerFrame->addTitleBarWidget(m_bypassButton);

    auto* body = new QWidget;
    auto* bl = new QVBoxLayout(body);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);
    m_title = new QLabel; // kept for the header text (tests read the frame title)
    m_title->setObjectName(QStringLiteral("ColorViewerTitle"));
    m_title->hide();
    bl->addWidget(m_title);
    m_surface = ViewerSurface::create(body);
    bl->addWidget(m_surface->widget(), 1);
    const TimelineData& data = m_ctx.project->timeline()->data();
    m_surface->setAspect(QSize(data.width, data.height));
    m_overlay = new ColorViewerOverlay(m_session, m_ctx.project, m_surface);

    // Transport.
    auto* transport = new QWidget;
    transport->setObjectName(QStringLiteral("ColorTransport"));
    auto* trl = new QHBoxLayout(transport);
    trl->setContentsMargins(6, 1, 6, 1);
    trl->setSpacing(1);
    trl->addStretch(1);
    for (const char* id : {"transport.go_to_start", "transport.prev_edit", "transport.prev_frame",
             "transport.play_reverse", "transport.shuttle_stop", "transport.play_pause",
             "transport.next_frame", "transport.next_edit", "transport.go_to_end",
             "transport.loop"}) {
        if (QAction* a = registryAction(m_ctx, QLatin1String(id))) {
            auto* b = new QToolButton(transport);
            b->setDefaultAction(a);
            b->setAutoRaise(true);
            b->setObjectName(QStringLiteral("ColorTransport_") + QLatin1String(id));
            if (a->icon().isNull()) {
                b->setToolButtonStyle(Qt::ToolButtonTextOnly);
                if (id == std::string_view("transport.play_reverse"))
                    b->setText(QStringLiteral("◂"));
            }
            trl->addWidget(b);
        }
    }
    trl->addStretch(1);
    m_timecode = new QLabel(transport);
    m_timecode->setObjectName(QStringLiteral("ColorTimecode"));
    m_timecode->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    trl->addWidget(m_timecode);
    bl->addWidget(transport);
    m_viewerFrame->setContentWidget(body);
    return m_viewerFrame;
}

QWidget* ColorPage::buildPalettes()
{
    m_primaries = new PrimariesPanel(m_session);
    m_curves = new ColorCurvesPanel(m_session);
    m_qualifier = new QualifierPanel(m_session);
    toolTipWithKey(m_ctx, m_qualifier->highlightButton(), "color.highlight",
        QualifierPanel::tr("Show the node's key in the viewer"));
    m_windows = new WindowsPanel(m_session);
    m_luts = new LutBrowser(m_session);
    m_paletteStack = new QStackedWidget;
    m_paletteStack->setObjectName(QStringLiteral("ColorPaletteStack"));
    for (QWidget* w : {static_cast<QWidget*>(m_primaries), static_cast<QWidget*>(m_curves),
             static_cast<QWidget*>(m_qualifier), static_cast<QWidget*>(m_windows),
             static_cast<QWidget*>(m_luts)})
        m_paletteStack->addWidget(w);
    auto* box = new QWidget;
    box->setObjectName(QStringLiteral("ColorPalettes"));
    auto* l = new QVBoxLayout(box);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    m_paletteButtons = new QButtonGroup(this);
    QWidget* bar = tabRow(m_paletteButtons,
        {tr("Primaries"), tr("Curves"), tr("Qualifier"), tr("Windows"), tr("LUTs")},
        QStringLiteral("ColorPaletteTab_"), box);
    // The primaries reset sits at the right of the bar (like Resolve's palette reset).
    auto* reset = new QToolButton(bar);
    reset->setObjectName(QStringLiteral("ColorPaletteReset"));
    reset->setText(tr("Reset"));
    reset->setAutoRaise(true);
    reset->setToolTip(tr("Reset the shown palette's values of the current node"));
    static_cast<QHBoxLayout*>(bar->layout())->addWidget(reset);
    connect(reset, &QToolButton::clicked, this, [this] {
        switch (currentPalette()) {
        case Palette::Primaries:
            m_primaries->resetAll();
            break;
        case Palette::Curves:
            m_curves->resetCurve();
            break;
        case Palette::Qualifier:
            if (const GradeNode* n = m_session->currentNode();
                n && n->qualifier != GradeQualifier {})
                m_session->setQualifier(0, GradeQualifier {}, tr("Reset Qualifier"));
            break;
        case Palette::Windows:
            if (const GradeNode* n = m_session->currentNode(); n && !n->windows.isEmpty()) {
                ClipGrade g = m_session->grade();
                g.node(n->id)->windows.clear();
                m_session->commit(g, tr("Reset Windows"));
            }
            break;
        case Palette::Luts:
            m_session->setLut(0, {});
            break;
        }
    });
    l->addWidget(bar);
    l->addWidget(m_paletteStack, 1);
    connect(m_paletteButtons, &QButtonGroup::idClicked, this,
        [this](int id) { showPalette(Palette(id)); });
    auto* frame = new PanelFrame(QString());
    frame->setContentWidget(box);
    frame->setTitleBarVisible(false); // the palette bar is the header
    return frame;
}

void ColorPage::showPalette(Palette palette)
{
    m_paletteStack->setCurrentIndex(int(palette));
    if (QAbstractButton* b = m_paletteButtons->button(int(palette)); b && !b->isChecked())
        b->setChecked(true);
    m_overlay->setWindowControls(palette == Palette::Windows);
}

ColorPage::Palette ColorPage::currentPalette() const
{
    return Palette(m_paletteStack->currentIndex());
}

QStringList ColorPage::commandIds(const ActionRegistry& registry)
{
    QStringList ids;
    for (const QString& id : registry.menuEntries(QStringLiteral("color"))) {
        if (registry.contains(id))
            ids.append(id);
    }
    return ids;
}

void ColorPage::connectActions()
{
    auto on = [this](const char* id, auto&& fn) {
        if (QAction* a = registryAction(m_ctx, QLatin1String(id)))
            connect(a, &QAction::triggered, this, fn);
    };
    on("color.add_serial", [this] { m_session->addSerialNode(); });
    on("color.add_serial_before", [this] { m_session->addSerialNodeBefore(); });
    on("color.add_parallel", [this] { m_session->addParallelNode(); });
    on("color.add_layer", [this] { m_session->addLayerNode(); });
    on("color.delete_node", [this] { m_session->removeNode(); });
    on("color.bypass_node", [this] { m_session->toggleNodeBypass(); });
    on("color.reset_node", [this] { m_session->resetNode(); });
    on("color.reset_grade", [this] { m_session->resetGrade(); });
    on("color.keyframe", [this] { m_session->toggleKeyframe(); });
    on("color.grab_still", [this] {
        const quint64 id = m_session->grabStill();
        if (id)
            m_gallery->setCurrentStill(id);
    });
    on("color.apply_still", [this] {
        if (const quint64 id = m_gallery->currentStill())
            m_session->applyStill(id, m_session->selectedClips());
    });
    on("color.copy_grade", [this] { m_session->copyCurrentGrade(); });
    on("color.paste_grade", [this] { m_session->pasteGrade(); });
    auto checkable = [this](const char* id, auto&& setter, auto&& changed) {
        QAction* a = registryAction(m_ctx, QLatin1String(id));
        if (!a)
            return;
        a->setCheckable(true);
        connect(a, &QAction::toggled, this, setter);
        changed(a);
    };
    checkable(
        "color.bypass_all", [this](bool b) { m_session->setBypassAll(b); },
        [this](QAction* a) {
            connect(m_session, &ColorSession::bypassAllChanged, a, [a](bool b) {
                const QSignalBlocker block(a);
                a->setChecked(b);
            });
        });
    checkable(
        "color.timeline_grade",
        [this](bool b) {
            m_session->setTarget(b ? ColorSession::Target::Timeline : ColorSession::Target::Clip);
        },
        [](QAction*) {}); // updateHeader() keeps it in step with the target
    checkable("color.highlight", [this](bool b) { setHighlight(b); }, [](QAction*) {});
    checkable("color.wipe", [this](bool b) { setWipe(b); }, [](QAction*) {});
    // Disabled until the page is shown (the Color menu is in the Edit menu of every page).
    if (m_ctx.actions) {
        for (const QString& id : commandIds(*m_ctx.actions))
            m_ctx.actions->action(id)->setEnabled(false);
    }
}

void ColorPage::setHighlight(bool on)
{
    if (on == m_highlightOn)
        return;
    m_highlightOn = on;
    for (QAbstractButton* b : {static_cast<QAbstractButton*>(m_highlightButton),
             static_cast<QAbstractButton*>(m_qualifier->highlightButton())}) {
        const QSignalBlocker block(b);
        b->setChecked(on);
    }
    if (QAction* a = registryAction(m_ctx, QStringLiteral("color.highlight"))) {
        const QSignalBlocker block(a);
        a->setChecked(on);
    }
    refreshPictures();
}

void ColorPage::setWipe(bool on)
{
    if (on && !m_gallery->wipeStill()) {
        // Wipe against the selected still, else the newest one.
        quint64 id = m_gallery->currentStill();
        const auto& stills = m_ctx.project->gallery()->stills();
        if (!id && !stills.isEmpty())
            id = stills.last().id;
        if (!id)
            on = false;
        else {
            m_wipeOn = true; // before setWipeStill(): its signal must not toggle again
            m_gallery->setWipeStill(id);
        }
    }
    m_wipeOn = on;
    if (!on && m_gallery->wipeStill())
        m_gallery->setWipeStill(0);
    for (QAbstractButton* b : {static_cast<QAbstractButton*>(m_wipeButton)}) {
        const QSignalBlocker block(b);
        b->setChecked(on);
    }
    if (QAction* a = registryAction(m_ctx, QStringLiteral("color.wipe"))) {
        const QSignalBlocker block(a);
        a->setChecked(on);
    }
    updateWipe();
}

void ColorPage::updateWipe()
{
    const GalleryStill* s
        = m_wipeOn ? m_ctx.project->gallery()->still(m_gallery->wipeStill()) : nullptr;
    m_overlay->setWipeImage(s ? s->image : QImage());
}

void ColorPage::refreshPictures()
{
    m_pictureTimer->stop();
    // Node thumbnails: each node's output on the ungraded frame.
    QHash<quint64, QImage> thumbs;
    if (!m_frames->input().isNull()) {
        for (const GradeNode& n : m_session->grade().nodes)
            thumbs.insert(n.id, m_frames->nodeOutput(n.id, kNodeThumb));
        // The node 01 the editor shows for an ungraded clip: the picture as it is.
        if (m_session->grade().isEmpty() && m_session->hasGrade()) {
            thumbs.insert(1,
                m_frames->input().scaled(
                    kNodeThumb, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        }
    }
    m_nodes->setThumbnails(thumbs);
    // Highlight.
    QImage highlight;
    if (m_highlightOn) {
        highlight = m_frames->highlight(m_session->selectedNode(),
            m_qualifier->highlightBlackWhite() ? ColorFrameSource::HighlightMode::BlackWhite
                                               : ColorFrameSource::HighlightMode::Grey);
    }
    m_overlay->setHighlightImage(highlight);
}

void ColorPage::pickQualifier(const QRectF& frameRect)
{
    if (!m_session->hasGrade())
        return;
    // The pick needs a corrector: an ungraded clip's node 01 (setQualifier() creates it).
    const GradeNode* current = m_session->currentNode();
    if (!m_session->grade().isEmpty() && (!current || current->isMixer()))
        return;
    const quint64 node = current ? current->id : 0;
    const GradeQualifier base = current ? current->qualifier : GradeQualifier {};
    QVector<qualifierpick::Hsl> samples;
    const int steps = frameRect.isEmpty() ? 1 : 6;
    for (int iy = 0; iy < steps; ++iy) {
        for (int ix = 0; ix < steps; ++ix) {
            const QPointF uv = steps == 1
                ? frameRect.topLeft()
                : QPointF(frameRect.left() + frameRect.width() * (ix + 0.5) / steps,
                      frameRect.top() + frameRect.height() * (iy + 0.5) / steps);
            float rgb[3];
            if (m_frames->sampleNodeInput(node, uv, rgb))
                samples.append(qualifierpick::toHsl(rgb));
        }
    }
    if (samples.isEmpty())
        return;
    const qualifierpick::Mode mode = m_qualifier->pickMode().value_or(qualifierpick::Mode::Pick);
    const GradeQualifier q = qualifierpick::apply(base, samples, mode);
    m_session->setQualifier(0, q,
        mode == qualifierpick::Mode::Pick      ? tr("Qualifier Pick")
            : mode == qualifierpick::Mode::Add ? tr("Qualifier Add")
                                               : tr("Qualifier Remove"));
    if (currentPalette() != Palette::Qualifier)
        showPalette(Palette::Qualifier);
}

void ColorPage::updateHeader()
{
    QString text;
    if (m_session->target() == ColorSession::Target::Timeline) {
        text = tr("Timeline grade");
    } else if (const quint64 id = m_session->clipId()) {
        const ClipData* c = editops::clipById(m_ctx.project->timeline()->data(), id);
        const QVector<quint64> clips = m_session->stripClips();
        const int number = int(clips.indexOf(id)) + 1;
        QString name = c ? c->name : QString();
        if (name.isEmpty() && c && !c->mediaId.isEmpty()) {
            if (const MediaItem* m = m_ctx.project->mediaPool()->item(c->mediaId))
                name = m->name;
        }
        text = QStringLiteral("%1  %2").arg(
            QString::number(number).rightJustified(2, QLatin1Char('0')),
            name.isEmpty() ? tr("Clip") : name);
        if (m_session->grade().isEmpty())
            text += QStringLiteral(" — ") + tr("ungraded");
    } else {
        text = tr("No clip under the playhead");
    }
    if (m_session->bypassAll())
        text += QStringLiteral(" — ") + tr("grades bypassed");
    m_title->setText(text);
    m_viewerFrame->setTitle(text);
    const QSignalBlocker block(m_bypassButton);
    m_bypassButton->setChecked(m_session->bypassAll());
    {
        const QSignalBlocker b1(m_targetButton);
        const QSignalBlocker b2(m_clipButton);
        const bool timeline = m_session->target() == ColorSession::Target::Timeline;
        m_targetButton->setChecked(timeline);
        m_clipButton->setChecked(!timeline);
    }
    if (QAction* a = registryAction(m_ctx, QStringLiteral("color.timeline_grade"))) {
        const QSignalBlocker block(a);
        a->setChecked(m_session->target() == ColorSession::Target::Timeline);
    }
}

void ColorPage::updateTimecode()
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    m_timecode->setText(timecode::format(m_ctx.project->timeline()->playhead(), data.fps));
}

QAction* ColorPage::actionForKey(const QKeyEvent* event) const
{
    if (!m_ctx.actions)
        return nullptr;
    const QKeySequence seq(event->keyCombination());
    for (const QString& id : commandIds(*m_ctx.actions)) {
        QAction* a = m_ctx.actions->action(id);
        if (!a->isEnabled())
            continue;
        for (const QKeySequence& k : a->shortcuts()) {
            if (k == seq)
                return a;
        }
    }
    return nullptr;
}

bool ColorPage::eventFilter(QObject* watched, QEvent* event)
{
    // The page claims the keys of its commands while focus is on it (scope widget:color_page):
    // ShortcutOverride keeps the window shortcuts from taking them, the key press triggers.
    // A text field keeps its own keys (Ctrl+C copies text there, Backspace deletes).
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto* w = qobject_cast<QWidget*>(watched);
        if (w && isVisible() && (w == this || isAncestorOf(w)) && !isTextInput(w)) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (QAction* a = actionForKey(key)) {
                if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
                    a->trigger();
                event->accept();
                return true;
            }
        }
    }
    return Page::eventFilter(watched, event);
}

void ColorPage::updateOutputClient()
{
    if (!m_ctx.playback)
        return;
    if (!isVisible()) {
        m_ctx.playback->removeOutputClient(this);
        return;
    }
    const qreal dpr = m_surface->widget()->devicePixelRatioF();
    const QSize logical = m_surface->pictureRect().size();
    m_ctx.playback->setOutputClient(this,
        QSize(int(logical.width() * dpr), int(logical.height() * dpr)),
        m_surface->canDrawTextures());
}

void ColorPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    if (event->spontaneous())
        return;
    if (m_ctx.actions) {
        for (const QString& id : commandIds(*m_ctx.actions))
            m_ctx.actions->action(id)->setEnabled(true);
    }
    if (!m_filterInstalled) {
        qApp->installEventFilter(this);
        m_filterInstalled = true;
    }
    if (m_defaultSizes) {
        // First show: Resolve-like proportions (gallery and nodes narrow, palettes wide).
        m_defaultSizes = false;
        const int w = std::max(800, width());
        const int side = std::clamp(w / 7, 170, 280);
        const int nodes = std::clamp(w / 4, 260, 460);
        m_top->setSizes({side, w - side - nodes, nodes});
        m_bottom->setSizes({w * 2 / 3, w - w * 2 / 3});
        const int h = std::max(500, height());
        const int middle = m_middle->sizeHint().height();
        const int bottom = std::clamp((h - middle) * 2 / 5, 230, 380);
        m_rows->setSizes({h - middle - bottom, middle, bottom});
    }
    m_session->openClip(0);
    // The page's keys act while focus is inside it (Alt+S right after Shift+6, as in
    // Resolve): when the page that had the focus went away, the node editor takes it.
    QTimer::singleShot(0, this, [this] {
        const QWidget* focus = QApplication::focusWidget();
        const bool elsewhereInWindow
            = focus && focus->window() == window() && focus != this && !isAncestorOf(focus);
        if (isVisible() && (!focus || !focus->isVisible() || elsewhereInWindow))
            m_nodes->setFocus(Qt::OtherFocusReason);
    });
    updateOutputClient();
    if (m_ctx.playback && m_ctx.playback->currentFrame())
        m_surface->setRenderedFrame(m_ctx.playback->currentFrame());
    m_feeder->setActive(true);
    m_frames->setActive(true);
    updateHeader();
    updateTimecode();
}

void ColorPage::hideEvent(QHideEvent* event)
{
    Page::hideEvent(event);
    if (event->spontaneous())
        return; // minimised: keep the state
    if (m_ctx.actions) {
        for (const QString& id : commandIds(*m_ctx.actions))
            m_ctx.actions->action(id)->setEnabled(false);
    }
    if (m_filterInstalled) {
        qApp->removeEventFilter(this);
        m_filterInstalled = false;
    }
    m_feeder->setActive(false);
    m_frames->setActive(false);
    if (m_ctx.playback)
        m_ctx.playback->removeOutputClient(this);
}

void ColorPage::resizeEvent(QResizeEvent* event)
{
    Page::resizeEvent(event);
    updateOutputClient();
}

// ---- Panels ----

QWidget* ColorPage::panelWidget(PanelId id) const
{
    switch (id) {
    case PanelId::Gallery:
        return m_galleryFrame;
    case PanelId::Nodes:
        return m_nodesFrame;
    case PanelId::Clips:
        return m_strip;
    case PanelId::ColorTimeline:
        return m_mini;
    case PanelId::Scopes:
        return m_scopesFrame;
    default:
        return nullptr;
    }
}

bool ColorPage::hasPanel(PanelId id) const
{
    return panelWidget(id) != nullptr;
}

void ColorPage::setPanelVisible(PanelId id, bool visible)
{
    QWidget* w = panelWidget(id);
    if (!w || w->isHidden() == !visible)
        return;
    w->setVisible(visible);
    if (id == PanelId::Clips || id == PanelId::ColorTimeline)
        m_middle->setVisible(!m_strip->isHidden() || !m_mini->isHidden());
    emit panelVisibilityChanged(id, visible);
}

bool ColorPage::isPanelVisible(PanelId id) const
{
    QWidget* w = panelWidget(id);
    return w && !w->isHidden();
}

void ColorPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("layoutVersion"), kLayoutVersion);
    settings.setValue(QStringLiteral("rows"), m_rows->saveState());
    settings.setValue(QStringLiteral("top"), m_top->saveState());
    settings.setValue(QStringLiteral("bottom"), m_bottom->saveState());
    settings.setValue(QStringLiteral("palette"), int(currentPalette()));
    settings.setValue(QStringLiteral("bars"), m_primaries->barsMode());
    settings.setValue(QStringLiteral("rightTab"), m_rightStack->currentIndex());
    for (const PanelId id :
        {PanelId::Gallery, PanelId::Nodes, PanelId::Clips, PanelId::ColorTimeline, PanelId::Scopes})
        settings.setValue(QStringLiteral("visible/") + panelIdToString(id), isPanelVisible(id));
}

void ColorPage::restoreLayout(const QSettings& settings)
{
    if (settings.value(QStringLiteral("layoutVersion"), 0).toInt() != kLayoutVersion)
        return;
    m_defaultSizes = false;
    m_rows->restoreState(settings.value(QStringLiteral("rows")).toByteArray());
    m_top->restoreState(settings.value(QStringLiteral("top")).toByteArray());
    m_bottom->restoreState(settings.value(QStringLiteral("bottom")).toByteArray());
    showPalette(Palette(std::clamp(settings.value(QStringLiteral("palette"), 0).toInt(), 0, 4)));
    m_primaries->setBarsMode(settings.value(QStringLiteral("bars"), false).toBool());
    const int right = std::clamp(settings.value(QStringLiteral("rightTab"), 0).toInt(), 0, 1);
    m_rightStack->setCurrentIndex(right);
    m_rightButtons->button(right)->setChecked(true);
    for (const PanelId id : {PanelId::Gallery, PanelId::Nodes, PanelId::Clips,
             PanelId::ColorTimeline, PanelId::Scopes}) {
        const QString key = QStringLiteral("visible/") + panelIdToString(id);
        if (settings.contains(key))
            setPanelVisible(id, settings.value(key).toBool());
    }
}

} // namespace kk
