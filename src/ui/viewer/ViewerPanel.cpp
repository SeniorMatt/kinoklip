// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerPanel — source / timeline viewer wired to a PlaybackEngine.
#include "ui/viewer/ViewerPanel.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "render/RenderTypes.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragAndDrop.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/common/MediaManagement.h"
#include "ui/common/Metrics.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/viewer/MaskOverlay.h"
#include "ui/viewer/MulticamOverlay.h"
#include "ui/viewer/PlaybackResolution.h"
#include "ui/viewer/ScrubBar.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDrag>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace kk {

namespace {

constexpr int kTransportIconSize = metrics::kTransportIconSize;
constexpr int kTransportBarHeight = 36;
constexpr int kDragPixmapWidth = 160; ///< width of the picture shown while dragging a range
/// While the window (or a splitter) is being resized, the engine renders for the new picture
/// size only once the size has been stable this long. Meanwhile the surface draws the last
/// frame scaled into the new picture rect. Re-rendering at every intermediate size (one
/// composite + new output textures per resize step) was what made live resizing stutter.
constexpr int kResizeSettleMs = 120;

QList<ViewerPanel*>& registry()
{
    static QList<ViewerPanel*> viewers;
    return viewers;
}
ViewerPanel* s_active = nullptr;

/// @p base mirrored horizontally (play reverse from the play icon), for every icon mode.
QIcon mirrored(const QIcon& base)
{
    QIcon out;
    const QSize size(kTransportIconSize * 2, kTransportIconSize * 2);
    for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active}) {
        out.addPixmap(base.pixmap(size, mode).transformed(QTransform().scale(-1, 1)), mode);
    }
    return out;
}

/// The play icon between in/out brackets ("play in to out").
/// @p base between square brackets ("play in to out"), drawn with paletteIcon() so base and
/// brackets follow colour scheme switches.
QIcon bracketed(const QIcon& base, const QString& cacheKey)
{
    return paletteIcon(cacheKey, [base](QPainter& p, const QSizeF& size, const QColor& color) {
        const qreal s = qMin(size.width(), size.height());
        const QRectF inner(s / 4, s / 4, s / 2, s / 2);
        base.paint(&p, inner.toAlignedRect());
        p.setPen(QPen(color, 2.5 * s / (kTransportIconSize * 2)));
        const qreal t = s / 6;
        const qreal b = s - s / 6;
        p.drawPolyline(QPolygonF({{s / 4, t}, {s / 8, t}, {s / 8, b}, {s / 4, b}}));
        p.drawPolyline(QPolygonF({{s - s / 4, t}, {s - s / 8, t}, {s - s / 8, b}, {s - s / 4, b}}));
    });
}

double zoomFactor(ViewerPanel::Zoom zoom)
{
    switch (zoom) {
    case ViewerPanel::Zoom::Percent50:
        return 0.5;
    case ViewerPanel::Zoom::Percent100:
        return 1.0;
    case ViewerPanel::Zoom::Percent200:
        return 2.0;
    case ViewerPanel::Zoom::Fit:
        break;
    }
    return 0.0;
}

QString rateText(double rate)
{
    if (rate == 0.0 || rate == 1.0) {
        return {};
    }
    return QString::number(rate, 'g', 3) + QLatin1Char('x');
}

} // namespace

QStringList ViewerPanel::sourceKeyActionIds()
{
    // Actions owned by the timeline module that should act on the clip while a source viewer
    // has focus. Their keys come from the ActionRegistry, so remapping keeps working.
    return {QStringLiteral("mark.in"), QStringLiteral("mark.out"), QStringLiteral("mark.clip"),
        QStringLiteral("mark.clear_in"), QStringLiteral("mark.clear_out"),
        QStringLiteral("mark.clear_both"), QStringLiteral("mark.go_to_in"),
        QStringLiteral("mark.go_to_out"), QStringLiteral("transport.go_to_start"),
        QStringLiteral("transport.go_to_end")};
}

ViewerPanel::ViewerPanel(Mode mode, AppContext& ctx, QWidget* parent)
    : PanelFrame(mode == Mode::Source ? tr("Source") : tr("Timeline"), parent)
    , m_ctx(ctx)
    , m_mode(mode)
{
    registry().append(this);
    m_resizeSettle = new QTimer(this);
    m_resizeSettle->setSingleShot(true);
    // KK_VIEWER_RESIZE_SETTLE_MS overrides the delay (0: every size at once, the old
    // behaviour; used by the resize benchmark, tests/app/test_resizebench.cpp).
    m_resizeSettle->setInterval(qEnvironmentVariableIsSet("KK_VIEWER_RESIZE_SETTLE_MS")
            ? qEnvironmentVariableIntValue("KK_VIEWER_RESIZE_SETTLE_MS")
            : kResizeSettleMs);
    connect(m_resizeSettle, &QTimer::timeout, this, &ViewerPanel::updateOutputClient);
    setObjectName(
        mode == Mode::Source ? QStringLiteral("SourceViewer") : QStringLiteral("TimelineViewer"));
    setFocusPolicy(Qt::ClickFocus);

    // ---- Title bar (Resolve's viewer header): zoom on the left, the clip / timeline name
    // centred, preview quality and the timecode on the right ----
    setTitleCentered(true);
    m_zoomBox = new QComboBox;
    m_zoomBox->setObjectName(QStringLiteral("ViewerZoom"));
    m_zoomBox->addItems({tr("Fit"), tr("50%"), tr("100%"), tr("200%")});
    m_zoomBox->setFocusPolicy(Qt::NoFocus);
    m_zoomBox->setToolTip(tr("Viewer zoom"));
    m_zoomBox->setMinimumWidth(metrics::kTitleComboMinWidth);
    connect(m_zoomBox, &QComboBox::currentIndexChanged, this, [this](int i) { setZoom(Zoom(i)); });
    addTitleBarLeadingWidget(m_zoomBox);

    m_qualityBox = new QComboBox;
    m_qualityBox->setObjectName(QStringLiteral("ViewerQuality"));
    m_qualityBox->setFocusPolicy(Qt::NoFocus);
    m_qualityBox->setMinimumWidth(metrics::kTitleComboMinWidth);
    // One application-wide playback resolution (Playback menu, every viewer, Preferences).
    PlaybackResolution& resolution = PlaybackResolution::instance();
    resolution.bindCombo(m_qualityBox);
    connect(
        &resolution, &PlaybackResolution::changed, this, [this](PlaybackEngine::QualityMode mode) {
            if (engine()) {
                engine()->setQualityMode(mode);
            }
        });
    // "Proxy" when the shown timeline / clip plays proxy or optimized media (Resolve shows
    // the proxy state in the viewer as well); hidden otherwise.
    m_proxyLabel = new QLabel(tr("Proxy"));
    m_proxyLabel->setObjectName(QStringLiteral("ViewerProxyIndicator"));
    m_proxyLabel->setContentsMargins(metrics::kSpaceS, 0, metrics::kSpaceS, 0);
    m_proxyLabel->setToolTip(tr("Playing proxy or optimized media (Playback > Proxy Handling). "
                                "Export uses the camera originals."));
    // The scheme's neutral (warning) colour, refreshed on a scheme switch.
    const auto styleProxyLabel = [label = m_proxyLabel] {
        label->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;")
                                 .arg(schemeColors().neutral.name(QColor::HexRgb)));
    };
    styleProxyLabel();
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, m_proxyLabel,
        styleProxyLabel);
    m_proxyLabel->setVisible(false);
    addTitleBarWidget(m_proxyLabel);
    addTitleBarWidget(m_qualityBox);

    m_timecode = new QLabel(timecode::format(0, Rational {}));
    m_timecode->setObjectName(QStringLiteral("TimecodeLabel"));
    m_timecode->setContentsMargins(metrics::kSpaceS, 0, metrics::kSpaceS, 0);
    m_timecode->setToolTip(tr("Current timecode"));
    addTitleBarWidget(m_timecode);

    // ---- Body: picture, scrub bar, transport ----
    m_body = new QWidget;
    auto* layout = new QVBoxLayout(m_body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_surface = ViewerSurface::create(m_body);
    m_surface->widget()->setFocusPolicy(Qt::ClickFocus);
    m_surface->widget()->installEventFilter(this);
    layout->addWidget(m_surface->widget(), 1);
    if (m_mode == Mode::Timeline) {
        // On-screen mask controls: an additive child layer over the picture surface.
        m_maskOverlay = new MaskOverlay(m_ctx, m_surface);
        connect(m_maskOverlay, &MaskOverlay::pressed, this, [this] { activate(); });
    }

    m_scrub = new ScrubBar(m_body);
    // While playing, scrubbing keeps playing from where the mouse leaves the playhead, like the
    // timeline ruler (PlaybackEngine parks on the frame under the mouse meanwhile).
    connect(m_scrub, &ScrubBar::scrubStarted, this, [this] { activate(); });
    connect(m_scrub, &ScrubBar::scrubbed, this, [this](int64_t frame) {
        if (engine()) {
            engine()->seek(frame);
        } else if (m_boundTimeline) {
            m_boundTimeline->setPlayhead(frame);
        }
    });
    layout->addWidget(m_scrub);

    m_transport = new QWidget(m_body);
    m_transport->setObjectName(QStringLiteral("ViewerTransport"));
    m_transport->setFixedHeight(kTransportBarHeight);
    auto* bar = new QHBoxLayout(m_transport);
    bar->setContentsMargins(metrics::kSpaceM, 0, metrics::kSpaceM, 0);
    bar->setSpacing(metrics::kSpaceXs);
    if (m_maskOverlay) {
        // Bottom-left on-screen controls menu (Off / Mask), like Resolve's viewer.
        QAction* masks
            = m_ctx.actions && m_ctx.actions->contains(QStringLiteral("view.onscreen_masks"))
            ? m_ctx.actions->action(QStringLiteral("view.onscreen_masks"))
            : nullptr;
        bar->addWidget(MaskOverlay::createModeButton(masks, m_transport));
    }
    m_marksLabel = new QLabel;
    m_marksLabel->setObjectName(QStringLiteral("ViewerMarks"));
    // Secondary text from the palette (readable in light and dark colour schemes).
    m_marksLabel->setForegroundRole(QPalette::PlaceholderText);
    bar->addWidget(m_marksLabel);
    bar->addStretch(1);

    auto act = [this](const char* id) {
        return m_ctx.actions && m_ctx.actions->contains(QLatin1String(id))
            ? m_ctx.actions->action(QLatin1String(id))
            : nullptr;
    };
    auto onEngine = [this](void (PlaybackEngine::*fn)()) {
        return [this, fn] {
            activate();
            if (PlaybackEngine* e = engine()) {
                (e->*fn)();
            }
        };
    };
    // Marks: the source player's own, or the timeline's (through its actions, owned by the
    // timeline module).
    auto mark = [this, act](const char* id, void (SourcePlayer::*fn)()) {
        return [this, act, id, fn] {
            activate();
            if (m_mode == Mode::Source) {
                if (m_player) {
                    (m_player->*fn)();
                }
            } else if (QAction* a = act(id)) {
                a->trigger();
            }
        };
    };
    // Resolve's transport row: marked duration left, transport centred, in / out marks right.
    addButton(bar, QStringLiteral("transport.go_to_start"), icon(QStringLiteral("go_start")),
        tr("Go to Start"), onEngine(&PlaybackEngine::goToStart));
    addButton(bar, QStringLiteral("transport.prev_frame"), icon(QStringLiteral("prev_frame")),
        tr("Previous Frame"), [this] {
            activate();
            if (engine()) {
                engine()->step(-1);
            }
            Q_EMIT stepRequested(-1);
        });
    addButton(bar, QStringLiteral("transport.shuttle_reverse"),
        mirrored(icon(QStringLiteral("play"))), tr("Play Reverse"), [this] {
            activate();
            if (engine()) {
                engine()->setRate(-1.0);
            }
        });
    addButton(bar, QStringLiteral("transport.shuttle_stop"), icon(QStringLiteral("stop")),
        tr("Stop"), [this] {
            activate();
            if (engine()) {
                engine()->stop();
            }
            Q_EMIT stopRequested();
        });
    m_playButton = addButton(bar, QStringLiteral("transport.play_pause"),
        icon(QStringLiteral("play")), tr("Play / Pause"), [this] {
            activate();
            if (engine()) {
                engine()->togglePlay();
            }
            Q_EMIT playPauseRequested();
        });
    addButton(bar, QStringLiteral("transport.next_frame"), icon(QStringLiteral("next_frame")),
        tr("Next Frame"), [this] {
            activate();
            if (engine()) {
                engine()->step(1);
            }
            Q_EMIT stepRequested(1);
        });
    addButton(bar, QStringLiteral("transport.go_to_end"), icon(QStringLiteral("go_end")),
        tr("Go to End"), onEngine(&PlaybackEngine::goToEnd));
    bar->addSpacing(metrics::kSpaceL);
    // Loop is one global toggle (like most NLEs): the button shows the shared action.
    if (QAction* loop = act("transport.loop")) {
        auto* button = new QToolButton;
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setIconSize(QSize(kTransportIconSize, kTransportIconSize));
        button->setDefaultAction(loop);
        bar->addWidget(button);
        connect(loop, &QAction::toggled, this, [this](bool on) {
            if (engine()) {
                engine()->setLoop(on);
            }
        });
    }
    addButton(bar, QStringLiteral("transport.play_in_out"),
        bracketed(icon(QStringLiteral("play")), QStringLiteral("viewer-play-in-out")),
        tr("Play In to Out"), onEngine(&PlaybackEngine::playInToOut));
    bar->addStretch(1);
    addButton(bar, QStringLiteral("mark.in"), icon(QStringLiteral("mark_in")), tr("Mark In"),
        mark("mark.in", &SourcePlayer::markInAtPosition));
    addButton(bar, QStringLiteral("mark.out"), icon(QStringLiteral("mark_out")), tr("Mark Out"),
        mark("mark.out", &SourcePlayer::markOutAtPosition));
    m_rateLabel = new QLabel;
    m_rateLabel->setObjectName(QStringLiteral("ViewerRate"));
    m_rateLabel->setMinimumWidth(40);
    m_rateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_rateLabel);
    layout->addWidget(m_transport);
    setContentWidget(m_body);

    // ---- Content ----
    if (m_mode == Mode::Timeline) {
        bindEngine(ctx.playback);
        if (ctx.project) {
            bindTimeline(ctx.project->timeline());
        }
    } else if (ctx.project) {
        // A removed or relinked item must not stay loaded.
        connect(ctx.project->mediaPool(), &MediaPool::changed, this, [this] {
            if (!m_sourceMediaId.isEmpty()) {
                const MediaItem* item = m_ctx.project->mediaPool()->item(m_sourceMediaId);
                if (!item
                    || (m_player
                        && (item->path != m_player->media().path
                            || !(item->attributes == m_player->media().attributes)))) {
                    setSourceMedia(item ? item->id : QString());
                }
            }
        });
    }
    setupMulticam();
    clearFrame();
    updateTitle();
    updateMarksLabel();
}

ViewerPanel::~ViewerPanel()
{
    registry().removeAll(this);
    if (s_active == this) {
        s_active = nullptr;
    }
    if (m_engine) {
        m_engine->removeOutputClient(this);
        disconnect(m_engine, nullptr, this, nullptr);
    }
    // The player (a child) would otherwise be deleted after the labels it updates.
    delete m_player;
    m_player = nullptr;
}

QToolButton* ViewerPanel::addButton(QHBoxLayout* layout, const QString& actionId,
    const QIcon& buttonIcon, const QString& tip, const std::function<void()>& handler)
{
    auto* button = new QToolButton;
    button->setObjectName(QStringLiteral("ViewerButton_") + actionId);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus); // keys stay with the viewer / application
    button->setIconSize(QSize(kTransportIconSize, kTransportIconSize));
    button->setIcon(buttonIcon);
    // Tooltip with the action's current shortcut (looked up, never hard-coded).
    QString shortcut;
    if (m_ctx.actions && m_ctx.actions->contains(actionId)) {
        const QList<QKeySequence> keys = m_ctx.actions->shortcuts(actionId);
        if (!keys.isEmpty()) {
            shortcut = keys.first().toString(QKeySequence::NativeText);
        }
    }
    button->setToolTip(shortcut.isEmpty() ? tip : QStringLiteral("%1 (%2)").arg(tip, shortcut));
    connect(button, &QToolButton::clicked, this, handler);
    layout->addWidget(button);
    return button;
}

// ---- Engine / timeline binding ----

PlaybackEngine* ViewerPanel::engine() const
{
    return m_engine.data();
}

void ViewerPanel::ensurePlayer()
{
    if (m_player || m_mode != Mode::Source) {
        return;
    }
    // Share the program engine's compositor: one render thread and GL context for the app.
    std::shared_ptr<Compositor> compositor
        = m_ctx.playback ? m_ctx.playback->compositor() : nullptr;
    m_player = new SourcePlayer(std::move(compositor), this);
    if (m_ctx.playback) {
        m_player->engine()->setBehaviour(m_ctx.playback->behaviour());
    }
    if (MediaManagement* mm = m_ctx.media) {
        // The source viewer plays proxies too (sound stays with the clip's own file).
        m_player->engine()->setSubstitutes(mm->proxies(), nullptr);
        PlaybackEngine* engine = m_player->engine();
        connect(mm, &MediaManagement::proxiesChanged, engine,
            [engine](std::shared_ptr<const ProxyMap> proxies) {
                engine->setSubstitutes(std::move(proxies), nullptr);
            });
    }
    bindEngine(m_player->engine());
    bindTimeline(m_player->timeline());
    connect(m_player, &SourcePlayer::marksChanged, this, &ViewerPanel::updateMarksLabel);
}

void ViewerPanel::bindEngine(PlaybackEngine* e)
{
    if (m_engine == e) {
        return;
    }
    if (m_engine) {
        m_engine->removeOutputClient(this);
        disconnect(m_engine, nullptr, this, nullptr);
    }
    m_engine = e;
    if (!e) {
        return;
    }
    connect(e, &PlaybackEngine::frameReady, this, [this](const RenderedFramePtr& frame) {
        if (frame) {
            m_surface->setRenderedFrame(frame);
        }
    });
    connect(e, &PlaybackEngine::positionChanged, this, &ViewerPanel::onPositionChanged);
    connect(e, &PlaybackEngine::rateChanged, this, &ViewerPanel::onRateChanged);
    connect(e, &PlaybackEngine::proxiesInUseChanged, m_proxyLabel, &QWidget::setVisible);
    m_proxyLabel->setVisible(e->proxiesInUse());
    e->setQualityMode(PlaybackResolution::instance().mode());
    if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("transport.loop"))) {
        e->setLoop(m_ctx.actions->action(QStringLiteral("transport.loop"))->isChecked());
    }
    if (RenderedFramePtr current = e->currentFrame()) {
        m_surface->setRenderedFrame(current);
    }
    onRateChanged(e->rate());
    updateOutputClient();
}

void ViewerPanel::bindTimeline(Timeline* timeline)
{
    disconnect(m_tlChanged);
    disconnect(m_tlPlayhead);
    m_boundTimeline = timeline;
    if (!timeline) {
        return;
    }
    m_tlChanged = connect(timeline, &Timeline::changed, this, &ViewerPanel::onTimelineChanged);
    if (!m_engine) {
        // No engine (tests, headless tools): at least follow the playhead.
        m_tlPlayhead
            = connect(timeline, &Timeline::playheadChanged, this, &ViewerPanel::onPositionChanged);
    }
    onTimelineChanged();
    onPositionChanged(m_engine ? m_engine->position() : timeline->playhead());
}

void ViewerPanel::onTimelineChanged()
{
    if (!m_boundTimeline) {
        return;
    }
    const TimelineData& d = m_boundTimeline->data();
    m_fps = d.fps;
    m_surface->setAspect(QSize(d.width, d.height));
    if (m_maskOverlay)
        m_maskOverlay->refresh();
    if (m_multicamOverlay && m_multicamOverlay->isVisible())
        m_multicamOverlay->refresh();
    m_scrub->setDuration(d.duration());
    m_scrub->setMarks(d.markIn, d.markOut);
    updateMarksLabel();
    updateTitle();
    onPositionChanged(m_engine ? m_engine->position() : m_boundTimeline->playhead());
    updateOutputClient();
}

void ViewerPanel::onPositionChanged(int64_t frame)
{
    // Source viewer: Clip Attributes > Timecode can give the media its own start timecode.
    int64_t start = 0;
    if (m_mode == Mode::Source && m_ctx.project && !m_sourceMediaId.isEmpty()) {
        const MediaItem* item = m_ctx.project->mediaPool()->item(m_sourceMediaId);
        if (item && item->attributes.overrideTimecode)
            start = item->attributes.startTimecodeFrames;
    }
    m_timecode->setText(timecode::format(start + frame, m_fps));
    m_scrub->setPosition(frame);
}

void ViewerPanel::onRateChanged(double rate)
{
    m_playButton->setIcon(icon(rate != 0.0 ? QStringLiteral("pause") : QStringLiteral("play")));
    m_rateLabel->setText(rateText(rate));
}

void ViewerPanel::updateOutputClient()
{
    if (!m_engine) {
        return;
    }
    m_resizeSettle->stop();
    if (!isVisible()) {
        m_engine->removeOutputClient(this);
        m_outputSized = false;
        return;
    }
    m_outputSized = true;
    const qreal dpr = m_surface->widget()->devicePixelRatioF();
    const QSize logical = m_surface->pictureRect().size();
    const QSize pixels(int(logical.width() * dpr), int(logical.height() * dpr));
    m_engine->setOutputClient(this, pixels, m_surface->canDrawTextures());
}

void ViewerPanel::updateTitle()
{
    const QString base = m_mode == Mode::Source ? tr("Source") : tr("Timeline");
    QString name;
    if (m_mode == Mode::Source) {
        name = m_player && m_player->hasMedia() ? m_player->media().name : QString();
    } else if (m_boundTimeline) {
        name = m_boundTimeline->data().name;
    }
    // Resolve's header shows just the name of what is loaded.
    setTitle(name.isEmpty() ? base : name);
}

void ViewerPanel::updateMarksLabel()
{
    if (!m_boundTimeline) {
        m_marksLabel->clear();
        return;
    }
    const TimelineData& d = m_boundTimeline->data();
    const int64_t in = d.markIn >= 0 ? d.markIn : 0;
    const int64_t out = d.markOut > 0 ? d.markOut : d.duration();
    QStringList parts;
    if (d.markIn >= 0) {
        parts << tr("In %1").arg(timecode::format(d.markIn, d.fps));
    }
    if (d.markOut > 0) {
        parts << tr("Out %1").arg(timecode::format(d.markOut - 1, d.fps));
    }
    // Duration of the marked range (or of everything), as Resolve shows it next to the marks.
    parts << tr("Dur %1").arg(timecode::format(std::max<int64_t>(0, out - in), d.fps));
    m_marksLabel->setText(parts.join(QStringLiteral("   ")));
}

// ---- Public API ----

void ViewerPanel::showFrame(const QImage& frame)
{
    m_surface->setFrame(frame);
}

void ViewerPanel::clearFrame()
{
    m_surface->clear(m_mode == Mode::Source ? tr("No clip loaded") : tr("No picture"));
}

void ViewerPanel::setPosition(int64_t frame, Rational fps)
{
    m_fps = fps;
    onPositionChanged(frame);
}

void ViewerPanel::setSourceMedia(const QString& mediaId)
{
    if (m_mode != Mode::Source) {
        return;
    }
    const MediaItem* item
        = m_ctx.project && !mediaId.isEmpty() ? m_ctx.project->mediaPool()->item(mediaId) : nullptr;
    m_sourceMediaId = item ? item->id : QString();
    m_customContent = false;
    if (item && item->kind == MediaItemKind::MulticamClip && m_multicamOverlay
        && m_multicamAllowed) {
        // A multicam clip opens as its angle grid (it wins over the timeline's clip until
        // the timeline playhead is moved again).
        m_multicamPrefersSource = true;
        m_localVideoAngle = m_localAudioAngle = 0;
        if (m_multicam)
            updateMulticam();
        else
            setMulticamMode(true);
    } else if (item) {
        ensurePlayer();
        m_player->setMedia(item);
        m_surface->setOverlayText(item->info.hasVideo ? QString() : tr("Audio only"));
        if (!item->info.hasVideo) {
            m_surface->clear(QString());
        }
    } else {
        if (m_player) {
            m_player->setMedia(nullptr);
        }
        m_surface->setOverlayText(QString());
        clearFrame();
    }
    updateTitle();
    updateMarksLabel();
    Q_EMIT sourceMediaChanged(m_sourceMediaId);
}

void ViewerPanel::setSourceContent(const MediaItem& item, TimelineData content)
{
    if (m_mode != Mode::Source || !m_ctx.project)
        return;
    m_sourceMediaId.clear();
    if (content.duration() <= 0) {
        m_customContent = false;
        if (m_player)
            m_player->setMedia(nullptr);
        clearFrame();
    } else {
        m_customContent = true;
        ensurePlayer();
        Project* project = m_ctx.project;
        m_player->setContent(item, std::move(content),
            makeMediaPathMap(project->mediaPool()->state()),
            [project](const TimelineData& d) { return project->nestedTimelinesFor(d); });
        m_surface->setOverlayText(QString());
    }
    updateTitle();
    updateMarksLabel();
}

void ViewerPanel::setMulticamAllowed(bool allowed)
{
    m_multicamAllowed = allowed;
    if (!allowed && m_multicam)
        setMulticamMode(false);
    if (m_modeBox)
        m_modeBox->setVisible(allowed);
}

QSize ViewerPanel::pictureSize() const
{
    return m_surface->pictureRect().size();
}

ViewerSurface* ViewerPanel::surface() const
{
    return m_surface;
}

std::optional<SourceRange> ViewerPanel::sourceSelection() const
{
    return m_player ? m_player->selection() : std::nullopt;
}

void ViewerPanel::setZoom(Zoom zoom)
{
    m_zoom = zoom;
    if (m_zoomBox->currentIndex() != int(zoom)) {
        m_zoomBox->setCurrentIndex(int(zoom));
    }
    m_surface->setZoom(zoomFactor(zoom));
    if (m_maskOverlay)
        m_maskOverlay->refresh(); // handles follow the new picture rectangle
    if (m_multicamOverlay)
        m_multicamOverlay->refresh();
    updateOutputClient();
}

void ViewerPanel::setChromeVisible(bool visible)
{
    setTitleBarVisible(visible);
    m_scrub->setVisible(visible);
    m_transport->setVisible(visible);
}

void ViewerPanel::activate()
{
    if (s_active != this) {
        s_active = this;
        Q_EMIT activated();
    }
}

ViewerPanel* ViewerPanel::activeViewer()
{
    return s_active;
}

QList<ViewerPanel*> ViewerPanel::allViewers()
{
    return registry();
}

// ---- Events ----

void ViewerPanel::showEvent(QShowEvent* event)
{
    PanelFrame::showEvent(event);
    updateOutputClient();
    if (m_multicam)
        updateMulticam();
}

void ViewerPanel::hideEvent(QHideEvent* event)
{
    PanelFrame::hideEvent(event);
    if (m_engine) {
        m_engine->removeOutputClient(this);
    }
}

QString ViewerPanel::matchSourceKey(const QKeyEvent* key) const
{
    if (!m_ctx.actions || key->isAutoRepeat()) {
        return {};
    }
    const QKeySequence pressed(
        QKeyCombination(key->modifiers() & ~Qt::KeypadModifier, Qt::Key(key->key())));
    for (const QString& id : sourceKeyActionIds()) {
        if (!m_ctx.actions->contains(id)) {
            continue;
        }
        for (const QKeySequence& seq : m_ctx.actions->shortcuts(id)) {
            if (seq == pressed) {
                return id;
            }
        }
    }
    return {};
}

bool ViewerPanel::handleSourceKey(const QString& id)
{
    if (!m_player || !m_player->hasMedia()) {
        return false;
    }
    PlaybackEngine* e = m_player->engine();
    if (id == QLatin1String("mark.in")) {
        m_player->markInAtPosition();
    } else if (id == QLatin1String("mark.out")) {
        m_player->markOutAtPosition();
    } else if (id == QLatin1String("mark.clip")) {
        m_player->setMarks(0, e->duration());
    } else if (id == QLatin1String("mark.clear_in")) {
        m_player->clearIn();
    } else if (id == QLatin1String("mark.clear_out")) {
        m_player->clearOut();
    } else if (id == QLatin1String("mark.clear_both")) {
        m_player->clearBoth();
    } else if (id == QLatin1String("mark.go_to_in")) {
        e->goToIn();
    } else if (id == QLatin1String("mark.go_to_out")) {
        e->goToOut();
    } else if (id == QLatin1String("transport.go_to_start")) {
        e->goToStart();
        Q_EMIT goToStartRequested();
    } else if (id == QLatin1String("transport.go_to_end")) {
        e->goToEnd();
        Q_EMIT goToEndRequested();
    } else {
        return false;
    }
    return true;
}

bool ViewerPanel::event(QEvent* event)
{
    if (m_mode == Mode::Source
        && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* key = static_cast<QKeyEvent*>(event);
        const QString id = m_player && m_player->hasMedia() ? matchSourceKey(key) : QString();
        if (!id.isEmpty()) {
            // Accepting the override suppresses the global (timeline) shortcut; the key then
            // arrives here as a KeyPress.
            if (event->type() == QEvent::KeyPress) {
                handleSourceKey(id);
            }
            event->accept();
            return true;
        }
    }
    return PanelFrame::event(event);
}

bool ViewerPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_surface->widget()) {
        return PanelFrame::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::Resize:
        // First size: at once (the first picture must come at the right size). Later sizes:
        // coalesced, see kResizeSettleMs.
        if (m_outputSized && m_resizeSettle->interval() > 0)
            m_resizeSettle->start();
        else
            updateOutputClient();
        break;
    case QEvent::MouseButtonPress: {
        activate();
        auto* mouse = static_cast<QMouseEvent*>(event);
        m_dragArmed = m_mode == Mode::Source && mouse->button() == Qt::LeftButton && m_player
            && m_player->hasMedia();
        m_dragStart = mouse->position().toPoint();
        break;
    }
    case QEvent::MouseMove: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (m_dragArmed && (mouse->buttons() & Qt::LeftButton)
            && (mouse->position().toPoint() - m_dragStart).manhattanLength()
                >= QApplication::startDragDistance()) {
            m_dragArmed = false;
            startSourceDrag();
            return true;
        }
        break;
    }
    case QEvent::MouseButtonRelease:
        m_dragArmed = false;
        break;
    default:
        break;
    }
    return PanelFrame::eventFilter(watched, event);
}

void ViewerPanel::startSourceDrag()
{
    const std::optional<SourceRange> range = sourceSelection();
    if (!range || m_customContent) {
        return; // a source tape range is not one media item
    }
    // Payload contract: ui/common/DragMime.h (mime::SourceRange).
    QJsonObject json;
    json.insert(QStringLiteral("mediaId"), range->mediaId);
    json.insert(QStringLiteral("inUs"), double(range->inUs));
    json.insert(QStringLiteral("outUs"), double(range->outUs));
    auto* data = new QMimeData;
    data->setData(
        QLatin1String(mime::SourceRange), QJsonDocument(json).toJson(QJsonDocument::Compact));
    auto* drag = new QDrag(this);
    drag->setMimeData(data);
    const QPixmap picture = m_surface->widget()->grab(
        m_surface->pictureRect().intersected(m_surface->widget()->rect()));
    if (!picture.isNull()) {
        drag->setPixmap(picture.scaledToWidth(kDragPixmapWidth, Qt::SmoothTransformation));
    }
    execCopyDrag(drag, "source viewer");
}

} // namespace kk
