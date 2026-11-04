// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorPanel implementation: structure tracking, generic group building, edit protocol.
#include "ui/inspector/InspectorPanel.h"

#include "core/AudioTransitions.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Subtitles.h"
#include "core/Timeline.h"
#include "core/TitlePresets.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Metrics.h"
#include "ui/effects/EffectActions.h"
#include "ui/help/Guide.h"
#include "ui/inspector/KeyframeButton.h"
#include "ui/inspector/MultiClipEdit.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {

constexpr int kTabMargin = 0;

/// Virtual clip ids address the default style of a subtitle track (bit 63 + TrackData::id):
/// clip ids are allocated from 1 upwards and never get near it.
constexpr quint64 kTrackStyleBit = quint64(1) << 63;

quint64 trackStyleClipId(quint64 trackId)
{
    return kTrackStyleBit | trackId;
}

TrackData* subtitleTrackById(TimelineData& data, quint64 trackId)
{
    for (TrackData& t : data.subtitleTracks) {
        if (t.id == trackId)
            return &t;
    }
    return nullptr;
}
constexpr int kGroupSpacing = 1;

/// Style sheet of the inspector's own widgets, derived from the palette so it matches both
/// the built-in dark theme and system colours.
QString inspectorStyleSheet(const QPalette& p)
{
    const QColor window = p.color(QPalette::Window);
    const bool dark = window.lightness() < 128;
    const QColor header = dark ? window.lighter(112) : window.darker(104);
    const QColor body = window;
    const QColor gap = dark ? window.darker(140) : window.darker(115);
    QColor dim = p.color(QPalette::WindowText);
    dim.setAlphaF(0.65f);
    return QStringLiteral(R"(
QWidget#InspectorGroupHeader { background: %1; border-top: 1px solid %3; }
QWidget#InspectorGroupBody, QWidget#InspectorTabPage { background: %2; }
QLabel#InspectorGroupTitle { font-weight: 600; }
QLabel#InspectorParamLabel, QLabel#InspectorClipInfo { color: %4; }
QLabel#InspectorClipName { font-weight: 600; }
QWidget#InspectorHeader { background: %2; border-bottom: 1px solid %3; }
QScrollArea#InspectorScroll { background: %2; border: none; }
)")
        .arg(header.name(QColor::HexArgb), body.name(QColor::HexArgb), gap.name(QColor::HexArgb),
            dim.name(QColor::HexArgb));
}

QString tabName(int tab)
{
    switch (tab) {
    case InspectorPanel::VideoTab:
        return InspectorPanel::tr("Video");
    case InspectorPanel::AudioTab:
        return InspectorPanel::tr("Audio");
    case InspectorPanel::EffectsTab:
        return InspectorPanel::tr("Effects");
    case InspectorPanel::TitleTab:
        return InspectorPanel::tr("Title");
    default:
        return InspectorPanel::tr("Transition");
    }
}

constexpr int kTabCount = InspectorPanel::TransitionTab + 1;

} // namespace

InspectorPanel::InspectorPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Inspector"), parent)
    , m_ctx(ctx)
{
    setPanelKey(QStringLiteral("inspector")); // in-app help: panel:inspector
    setAcceptDrops(true);
    setStyleSheet(inspectorStyleSheet(QApplication::palette()));

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("InspectorHeader"));
    header->setAttribute(Qt::WA_StyledBackground);
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(metrics::kSpaceM, metrics::kSpaceS + metrics::kSpaceXs,
        metrics::kSpaceM, metrics::kSpaceS + metrics::kSpaceXs);
    headerLayout->setSpacing(1);
    m_clipName = new QLabel;
    m_clipName->setObjectName(QStringLiteral("InspectorClipName"));
    m_clipInfo = new QLabel;
    m_clipInfo->setObjectName(QStringLiteral("InspectorClipInfo"));
    headerLayout->addWidget(m_clipName);
    headerLayout->addWidget(m_clipInfo);
    layout->addWidget(header);

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    for (int tab = 0; tab < kTabCount; ++tab) {
        auto* scroll = new QScrollArea;
        scroll->setObjectName(QStringLiteral("InspectorScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_tabScroll.append(scroll);
        m_tabs->addTab(scroll, tabName(tab));
    }
    layout->addWidget(m_tabs, 1);
    setContentWidget(content);

    if (ctx.actions) {
        for (const char* id : {"clip.prev_keyframe", "clip.add_keyframe", "clip.next_keyframe"})
            addTitleBarAction(ctx.actions->action(QLatin1String(id)));
        connect(ctx.actions->action(QStringLiteral("clip.add_keyframe")), &QAction::triggered, this,
            &InspectorPanel::toggleKeyframeOnActiveParam);
        connect(ctx.actions->action(QStringLiteral("clip.prev_keyframe")), &QAction::triggered,
            this, [this] { goToKeyframe(false); });
        connect(ctx.actions->action(QStringLiteral("clip.next_keyframe")), &QAction::triggered,
            this, [this] { goToKeyframe(true); });
        // Clip > Add Mask: a mask on the clip the Inspector shows (video clips only).
        const std::pair<const char*, MaskShape> maskActions[]
            = {{"mask.add_rectangle", MaskShape::Rectangle},
                {"mask.add_ellipse", MaskShape::Ellipse}, {"mask.add_polygon", MaskShape::Polygon},
                {"mask.add_bezier", MaskShape::Bezier}, {"mask.add_freehand", MaskShape::Freehand}};
        for (const auto& [id, shape] : maskActions) {
            if (!ctx.actions->contains(QLatin1String(id)))
                continue;
            connect(ctx.actions->action(QLatin1String(id)), &QAction::triggered, this,
                [this, shape = shape] {
                    if (m_structure.onVideoTrack)
                        addMask(shape);
                });
        }
    }
    if (ctx.selection) {
        connect(ctx.selection, &SelectionModel::changed, this, [this] {
            // A selected transition shows its clip on the Transition tab (spec 13.10).
            if (const auto t = m_ctx.selection->transition()) {
                inspect(t->clipId, TransitionTab);
                return;
            }
            const quint64 current = m_ctx.selection->currentClip();
            showClip(current);
            if (current != m_lastCurrent && m_clipId != 0)
                followClickedPart(current);
            m_lastCurrent = current;
        });
    }
    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &InspectorPanel::onTimelineChanged);
    connect(timeline, &Timeline::playheadChanged, this, &InspectorPanel::refreshValues);
    connect(ctx.project, &Project::projectReset, this, [this] { showClip(0); });
    // Ctrl+S right after typing a title: the typing step closes first (else the project is
    // dirty again just after the save).
    connect(ctx.project, &Project::aboutToSave, this, [this] {
        for (ParamGroupBox* group : std::as_const(m_groups))
            for (ParamRow* row : group->rows())
                row->finishPendingEdit();
    });
    if (ctx.selection) {
        // The mask groups' "edit in the viewer" buttons follow the active mask.
        connect(ctx.selection, &SelectionModel::colorPickerChanged, this,
            &InspectorPanel::refreshValues); // the eyedropper buttons' checked state
        connect(ctx.selection, &SelectionModel::activeMaskChanged, this,
            &InspectorPanel::refreshValues);
    }

    rebuild();
}

InspectorPanel::~InspectorPanel()
{
    // Rows call back into this object (ParamEditHost) when they are destroyed with an open
    // gesture; delete them while the InspectorPanel part still exists.
    for (QScrollArea* scroll : std::as_const(m_tabScroll))
        delete scroll->takeWidget();
    for (const QPointer<QWidget>& page : std::as_const(m_retiredPages))
        delete page.data();
    while (m_openEdits > 0)
        endParamEdit();
}

// ---- Public API ----

void InspectorPanel::showClip(quint64 clipId)
{
    const Structure structure = computeStructure(clipId);
    m_clipId = structure.clipId;
    m_audioClipId = structure.audioClipId;
    if (structure != m_structure || m_groups.isEmpty()) {
        const auto hasTitleTab = [](const Structure& s) {
            return s.clipId != 0 && (s.kind == ClipKind::Subtitle || s.kind == ClipKind::Title);
        };
        const bool hadTitleTab = hasTitleTab(m_structure);
        m_structure = structure;
        rebuild();
        // A caption's properties are all on the Captions tab (Resolve's subtitle inspector),
        // and a title's text and look on the Title tab, so selecting one shows that tab;
        // leaving them for a clip that has no title goes back to the Video tab (specs 28.15,
        // 14.9).
        if (hasTitleTab(structure) && !hadTitleTab)
            m_tabs->setCurrentIndex(TitleTab);
        else if (hadTitleTab && !hasTitleTab(structure) && m_tabs->currentIndex() == TitleTab)
            m_tabs->setCurrentIndex(VideoTab);
    } else {
        refreshValues();
    }
    updateHeader();
}

void InspectorPanel::followClickedPart(quint64 clickedId)
{
    // Clicking the audio part of a pair shows the Audio tab, the video part the Video tab
    // (spec 24.2); other tabs (effects, title, transition) stay where the user put them.
    const int tab = m_tabs->currentIndex();
    if (tab != VideoTab && tab != AudioTab)
        return;
    const bool audio = clickedId == m_audioClipId && clickedId != 0;
    m_tabs->setCurrentIndex(audio ? AudioTab : VideoTab);
}

int InspectorPanel::selectedItemCount() const
{
    if (!m_ctx.selection)
        return 1;
    // Linked clips count once (a video clip and its audio are one item, spec 24.1).
    const TimelineData& data = m_ctx.project->timeline()->data();
    QSet<quint64> groups;
    int items = 0;
    for (quint64 id : m_ctx.selection->clips()) {
        const ClipData* c = editops::clipById(data, id);
        if (!c)
            continue;
        if (c->linkGroup == 0)
            ++items;
        else if (!groups.contains(c->linkGroup)) {
            groups.insert(c->linkGroup);
            ++items;
        }
    }
    return items;
}

void InspectorPanel::inspect(quint64 clipId, Tab tab)
{
    showClip(clipId);
    if (m_clipId == clipId && clipId != 0)
        m_tabs->setCurrentIndex(tab);
}

void InspectorPanel::toggleKeyframeOnActiveParam()
{
    if (!m_activeRef || (m_activeRef->clipId != m_clipId && m_activeRef->clipId != m_audioClipId))
        return;
    const ParamRef ref = *m_activeRef;
    const ParamDescriptor desc = m_activeDesc;
    const ClipData* c = clip(ref.clipId);
    if (!c || !desc.isScalar() || !desc.animatable)
        return;
    const int64_t frame = localFrame(ref.clipId);
    const AnimatedValue* anim = paramaccess::animated(*c, ref);
    editClip(anim && anim->hasKeyAt(frame) ? tr("Remove Keyframe") : tr("Add Keyframe"), ref.clipId,
        [ref, desc, frame](ClipData& cd) { paramaccess::toggleKey(cd, ref, desc, frame); });
}

void InspectorPanel::goToKeyframe(bool forward)
{
    // Search every animated row of every group in timeline frames (groups may be bound to
    // different clips: the video clip and its linked audio clip).
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    std::optional<int64_t> best;
    for (int i = 0; i < m_groups.size(); ++i) {
        const ClipData* c = clip(m_groupClip[i]);
        if (!c)
            continue;
        for (const ParamRow* row : m_groups[i]->rows()) {
            const AnimatedValue* anim = paramaccess::animated(*c, row->ref());
            if (!anim)
                continue;
            for (const Keyframe& key : anim->keys) {
                const int64_t frame = c->start + key.frame;
                if (forward ? frame <= playhead : frame >= playhead)
                    continue;
                if (!best || (forward ? frame < *best : frame > *best))
                    best = frame;
            }
        }
    }
    if (best)
        m_ctx.project->timeline()->setPlayhead(*best);
}

// ---- ParamEditHost ----

const ClipData* InspectorPanel::clip(quint64 clipId) const
{
    if (clipId & kTrackStyleBit) {
        const TimelineData& data = m_ctx.project->timeline()->data();
        for (const TrackData& t : data.subtitleTracks) {
            if (trackStyleClipId(t.id) == clipId) {
                m_trackStyleProxy.kind = ClipKind::Subtitle;
                m_trackStyleProxy.id = clipId;
                m_trackStyleProxy.duration = 1;
                m_trackStyleProxy.title = t.subtitleStyle;
                return &m_trackStyleProxy;
            }
        }
        return nullptr;
    }
    return clipId ? editops::clipById(m_ctx.project->timeline()->data(), clipId) : nullptr;
}

int64_t InspectorPanel::localFrame(quint64 clipId) const
{
    if (clipId & kTrackStyleBit)
        return 0; // track styles are not animated
    const ClipData* c = clip(clipId);
    if (!c)
        return 0;
    const int64_t local = c->localFrame(m_ctx.project->timeline()->playhead());
    return std::clamp<int64_t>(local, 0, std::max<int64_t>(0, c->duration - 1));
}

void InspectorPanel::beginParamEdit(const QString& label)
{
    ++m_openEdits;
    m_ctx.project->beginEdit(label);
}

void InspectorPanel::endParamEdit()
{
    if (m_openEdits <= 0)
        return;
    --m_openEdits;
    m_ctx.project->endEdit();
}

void InspectorPanel::modifyClip(quint64 clipId, const std::function<void(ClipData&)>& mutate)
{
    Timeline* timeline = m_ctx.project->timeline();
    TimelineData& data = timeline->mutableData();
    if (clipId & kTrackStyleBit) {
        // Track style: edit a stand-in clip and copy its parameter set back to the track.
        if (TrackData* t = subtitleTrackById(data, clipId & ~kTrackStyleBit)) {
            ClipData proxy;
            proxy.kind = ClipKind::Subtitle;
            proxy.duration = 1;
            proxy.title = t->subtitleStyle;
            mutate(proxy);
            t->subtitleStyle = proxy.title;
            timeline->notifyChanged();
        }
        return;
    }
    if (ClipData* c = editops::clipById(data, clipId)) {
        const bool ownBefore = c->kind == ClipKind::Subtitle && subtitles::hasOwnStyle(*c);
        // Multi-clip editing (spec 24.1): the change also goes to the other selected clips of
        // the same kind, inside the same open edit (one undo step).
        const QVector<quint64> peers = editPeers(clipId);
        const std::optional<ClipData> before
            = peers.isEmpty() ? std::nullopt : std::optional<ClipData>(*c);
        mutate(*c);
        if (before) {
            const ClipData after = *c; // peers live in the same track vectors: copy first
            for (quint64 id : peers)
                if (ClipData* peer = editops::clipById(data, id))
                    multiclip::propagate(*before, after, *peer);
            c = editops::clipById(data, clipId);
        }
        // A caption is named after its text (first line), as in Resolve: the Edit Index and
        // the Inspector header follow the typing (spec 28.17).
        const auto nameCaption = [](ClipData& cd) {
            if (cd.kind == ClipKind::Subtitle)
                cd.name = subtitles::captionText(cd).section(QLatin1Char('\n'), 0, 0);
        };
        nameCaption(*c);
        for (quint64 id : peers)
            if (ClipData* peer = editops::clipById(data, id))
                nameCaption(*peer);
        if (c->kind == ClipKind::Subtitle && !ownBefore && subtitles::hasOwnStyle(*c)) {
            // "Custom Style" switched on: start from the track's look (keys the caption does
            // not set yet), so nothing changes until the user edits it.
            if (const auto loc = editops::findClip(data, clipId)) {
                const ParamSet& style = data.track(loc->track)->subtitleStyle;
                for (auto it = style.values.cbegin(); it != style.values.cend(); ++it)
                    c->title.values.insert(it.key(), it.value());
                for (auto it = style.statics.cbegin(); it != style.statics.cend(); ++it)
                    c->title.statics.insert(it.key(), it.value());
            }
        }
        timeline->notifyChanged(); // live update of viewers while the gesture continues
    }
}

QVector<quint64> InspectorPanel::editPeers(quint64 clipId) const
{
    if (!m_ctx.selection || (clipId & kTrackStyleBit) || m_ctx.selection->clips().size() < 2)
        return {};
    return multiclip::peers(m_ctx.project->timeline()->data(), m_ctx.selection->clips(), clipId);
}

bool InspectorPanel::isMixed(const ParamRef& ref, const ParamDescriptor& desc) const
{
    const QVector<quint64> peers = editPeers(ref.clipId);
    if (peers.isEmpty())
        return false;
    const ClipData* shown = clip(ref.clipId);
    if (!shown)
        return false;
    const bool scalar = desc.type == ParamType::Double || desc.type == ParamType::Int;
    const double value
        = scalar ? paramaccess::scalar(*shown, ref, desc, localFrame(ref.clipId)) : 0;
    const QVariant fixed = scalar ? QVariant() : paramaccess::staticValue(*shown, ref, desc);
    for (quint64 id : peers) {
        const ClipData* peer = clip(id);
        const std::optional<ParamRef> pref
            = peer ? multiclip::peerRef(*shown, *peer, ref) : std::nullopt;
        if (!pref)
            continue;
        if (scalar) {
            // Compared at each clip's own playhead frame (the value each one shows).
            if (std::abs(paramaccess::scalar(*peer, *pref, desc, localFrame(id)) - value) > 1e-9)
                return true;
        } else if (paramaccess::staticValue(*peer, *pref, desc) != fixed) {
            return true;
        }
    }
    return false;
}

void InspectorPanel::editClip(
    const QString& label, quint64 clipId, const std::function<void(ClipData&)>& mutate)
{
    beginParamEdit(label);
    modifyClip(clipId, mutate);
    endParamEdit();
}

void InspectorPanel::seekLocalFrame(quint64 clipId, int64_t local)
{
    if (clipId & kTrackStyleBit)
        return;
    if (const ClipData* c = clip(clipId))
        m_ctx.project->timeline()->setPlayhead(c->start + local);
}

void InspectorPanel::setActiveParam(const ParamRef& ref, const ParamDescriptor& desc)
{
    m_activeRef = ref;
    m_activeDesc = desc;
}

void InspectorPanel::setColorPicking(const ParamRef& ref, bool on)
{
    if (!m_ctx.selection)
        return;
    if (on)
        m_ctx.selection->setColorPicker({ref.clipId, ref.effectId, ref.key});
    else if (isColorPicking(ref))
        m_ctx.selection->setColorPicker({});
}

bool InspectorPanel::isColorPicking(const ParamRef& ref) const
{
    if (!m_ctx.selection)
        return false;
    const ColorPickTarget& t = m_ctx.selection->colorPicker();
    return t.isActive() && t.clipId == ref.clipId && t.effectId == ref.effectId && t.key == ref.key;
}

QColor InspectorPanel::pickColor(const QColor& initial, const QString& title)
{
    if (m_colorPicker)
        return m_colorPicker(initial, title);
    return QColorDialog::getColor(initial, this, title);
}

QString InspectorPanel::pickFile(
    const QString& current, const QString& title, const QString& filter)
{
    if (m_filePicker)
        return m_filePicker(current, title, filter);
    return QFileDialog::getOpenFileName(this, title, current, filter);
}

// ---- Structure ----

InspectorPanel::Structure InspectorPanel::computeStructure(quint64 clipId) const
{
    Structure s;
    const TimelineData& data = m_ctx.project->timeline()->data();
    auto location = clipId ? editops::findClip(data, clipId) : std::nullopt;
    if (!location)
        return s;
    // A linked video + audio pair is one item (spec 24.1): the Video tab edits the video
    // member, the Audio tab the audio member, whichever of the two was clicked.
    if (location->track.type == TrackType::Audio) {
        const ClipData& a = data.track(location->track)->clips[size_t(location->clipIndex)];
        if (a.linkGroup != 0) {
            for (quint64 linked : editops::linkedClips(data, clipId)) {
                const auto l = editops::findClip(data, linked);
                if (l && l->track.type == TrackType::Video) {
                    s.audioClipId = clipId;
                    clipId = linked;
                    location = l;
                    break;
                }
            }
        }
    }
    const ClipData& c = data.track(location->track)->clips[size_t(location->clipIndex)];
    s.clipId = clipId;
    s.kind = c.kind;
    s.onVideoTrack = location->track.type == TrackType::Video;
    s.effectIds.reserve(c.effects.size());
    for (const EffectInstance& fx : c.effects)
        s.effectIds.append(fx.id);
    s.maskIds.reserve(c.masks.size());
    for (const MaskData& m : c.masks)
        s.maskIds.append(m.id);
    s.transitionIn = c.transitionIn ? c.transitionIn->type : QString();
    s.transitionOut = c.transitionOut ? c.transitionOut->type : QString();
    if (c.kind == ClipKind::Subtitle) {
        s.subtitleTrackId = data.track(location->track)->id;
        s.ownStyle = subtitles::hasOwnStyle(c);
    }
    if (location->track.type == TrackType::Audio) {
        s.audioClipId = clipId;
    } else if (s.audioClipId == 0 && c.linkGroup != 0) {
        for (quint64 linked : editops::linkedClips(data, clipId)) {
            const auto l = editops::findClip(data, linked);
            if (l && l->track.type == TrackType::Audio) {
                s.audioClipId = linked;
                break;
            }
        }
    }
    return s;
}

void InspectorPanel::onTimelineChanged()
{
    const Structure structure = computeStructure(m_clipId);
    if (structure != m_structure) {
        m_clipId = structure.clipId;
        m_audioClipId = structure.audioClipId;
        m_structure = structure;
        rebuild();
        updateHeader();
    } else {
        refreshValues();
        updateHeader();
    }
}

// ---- Building ----

void InspectorPanel::clearTabs()
{
    // deleteLater: a rebuild can be triggered from a button inside a group (remove effect),
    // which must not be destroyed inside its own signal.
    for (ParamGroupBox* group : std::as_const(m_groups))
        group->hide();
    m_groups.clear();
    m_groupClip.clear();
    for (QScrollArea* scroll : std::as_const(m_tabScroll)) {
        if (QWidget* old = scroll->takeWidget()) {
            old->hide();
            old->deleteLater();
            m_retiredPages.append(old);
        }
    }
    m_addEffect = nullptr;
    m_retiredPages.removeIf([](const QPointer<QWidget>& page) { return page.isNull(); });
}

QVBoxLayout* InspectorPanel::tabLayout(Tab tab) const
{
    return static_cast<QVBoxLayout*>(m_tabScroll[tab]->widget()->layout());
}

void InspectorPanel::addPlaceholder(Tab tab, const QString& text)
{
    QWidget* placeholder = createPlaceholderContent(text);
    placeholder->setMinimumHeight(120);
    tabLayout(tab)->addWidget(placeholder);
}

ParamGroupBox* InspectorPanel::addGroup(
    Tab tab, const QString& key, const QString& title, quint64 clipId)
{
    auto* group = new ParamGroupBox(title);
    group->setCollapsed(m_collapsed.value(key, false));
    connect(group, &ParamGroupBox::collapsedChanged, this,
        [this, key](bool collapsed) { m_collapsed.insert(key, collapsed); });
    tabLayout(tab)->addWidget(group);
    m_groups.append(group);
    m_groupClip.append(clipId);
    return group;
}

void InspectorPanel::addDescriptorGroup(
    Tab tab, const EffectDescriptor& desc, ParamScope scope, quint64 clipId)
{
    ParamGroupBox* group = addGroup(tab, desc.id, desc.name, clipId);
    attachHelp(group, desc.id, desc.name);
    for (const ParamDescriptor& p : desc.params)
        group->addParam(ParamRef {clipId, scope, 0, p.key}, p, *this);
    addSectionKeyframeButton(group, desc, scope, clipId);
    QToolButton* reset = group->addHeaderButton(InspectorGlyph::Reset, tr("Reset all"));
    connect(reset, &QToolButton::clicked, this, [this, group, clipId, name = desc.name] {
        QVector<std::pair<ParamRef, ParamDescriptor>> params;
        for (const ParamRow* row : group->rows())
            params.append({row->ref(), row->descriptor()});
        editClip(tr("Reset %1").arg(name), clipId, [params](ClipData& c) {
            for (const auto& [ref, desc] : params)
                paramaccess::reset(c, ref, desc);
        });
    });
}

void InspectorPanel::attachHelp(ParamGroupBox* group, const QString& effectId, const QString& name)
{
    if (effectId.isEmpty())
        return;
    const QString key = QStringLiteral("effect:") + effectId;
    guide::setHelpContext(group, key);
    if (guide::HelpButton* help = guide::createHelpButton(key))
        group->addHeaderWidget(help);
    if (QAction* howTo = guide::createHowToAction(key, name, group))
        group->addHeaderMenuAction(howTo);
}

void InspectorPanel::addSectionKeyframeButton(
    ParamGroupBox* group, const EffectDescriptor& desc, ParamScope scope, quint64 clipId)
{
    // Spec 15.13: one diamond in the section header keys every animatable parameter of the
    // section at once (filled when all of them have a key at the playhead).
    QVector<ParamDescriptor> params;
    for (const ParamDescriptor& p : desc.params) {
        if (p.isScalar() && p.animatable)
            params.append(p);
    }
    if (params.isEmpty() || scope == ParamScope::SolidColor)
        return;
    auto* diamond = new KeyframeButton;
    diamond->setObjectName(QStringLiteral("keyframe-section:") + desc.id);
    diamond->setToolTip(tr("Add or remove keyframes of every %1 parameter").arg(desc.name));
    group->addHeaderWidget(diamond);
    connect(diamond, &KeyframeButton::clicked, this, [this, params, scope, clipId, desc] {
        const ClipData* c = clip(clipId);
        if (!c)
            return;
        const int64_t frame = localFrame(clipId);
        bool allOnKey = true;
        for (const ParamDescriptor& p : params) {
            const AnimatedValue* v = paramaccess::animated(*c, ParamRef {clipId, scope, 0, p.key});
            allOnKey = allOnKey && v && v->hasKeyAt(frame);
        }
        editClip(allOnKey ? tr("Remove %1 Keyframes").arg(desc.name)
                          : tr("Add %1 Keyframes").arg(desc.name),
            clipId, [&](ClipData& target) {
                ParamSet* set = paramaccess::paramSet(target, ParamRef {clipId, scope, 0, {}});
                if (!set)
                    return;
                for (const ParamDescriptor& p : params) {
                    AnimatedValue& v = set->animated(p.key, p.def);
                    if (allOnKey)
                        v.removeKey(frame);
                    else if (!v.hasKeyAt(frame))
                        v.setKey(frame, v.valueAt(frame));
                }
            });
    });
    group->setHeaderRefresh([this, diamond, params, scope, clipId](const ClipData& c) {
        const int64_t playhead = m_ctx.project->timeline()->playhead();
        const int64_t frame = c.localFrame(playhead);
        bool any = false;
        bool all = true;
        for (const ParamDescriptor& p : params) {
            const AnimatedValue* v = paramaccess::animated(c, ParamRef {clipId, scope, 0, p.key});
            any = any || (v && v->isAnimated());
            all = all && v && v->hasKeyAt(frame);
        }
        using S = KeyframeButton::State;
        diamond->setState(all ? S::OnKey : (any ? S::Animated : S::NotAnimated));
        diamond->setEnabled(c.contains(playhead));
    });
}

void InspectorPanel::rebuild()
{
    ++m_rebuildCount;
    QVector<int> scrollPositions;
    for (QScrollArea* scroll : std::as_const(m_tabScroll))
        scrollPositions.append(scroll->verticalScrollBar()->value());
    clearTabs();
    for (QScrollArea* scroll : std::as_const(m_tabScroll)) {
        auto* page = new QWidget;
        page->setObjectName(QStringLiteral("InspectorTabPage"));
        page->setAttribute(Qt::WA_StyledBackground);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(kTabMargin, kTabMargin, kTabMargin, kTabMargin);
        layout->setSpacing(kGroupSpacing);
        scroll->setWidget(page);
    }

    const ClipData* c = clip(m_clipId);
    if (!c) {
        const QString hint = tr("Select a clip on the timeline to see and animate its properties.");
        for (int tab = 0; tab < kTabCount; ++tab)
            addPlaceholder(Tab(tab), hint);
    } else {
        const EffectRegistry& registry = EffectRegistry::instance();
        m_tabs->setTabText(
            TitleTab, c->kind == ClipKind::Subtitle ? tr("Captions") : tabName(TitleTab));
        if (c->kind == ClipKind::Subtitle) {
            buildCaptionsTab(*c);
        } else if (m_structure.onVideoTrack) {
            if (c->kind == ClipKind::Generator) {
                if (const EffectDescriptor* d = registry.find(c->generator))
                    addDescriptorGroup(VideoTab, *d, ParamScope::Title, m_clipId);
            }
            if (c->kind == ClipKind::Solid) {
                if (const EffectDescriptor* d = registry.find(QStringLiteral("solid_color")))
                    addDescriptorGroup(VideoTab, *d, ParamScope::SolidColor, m_clipId);
            }
            // Resolve's order: Transform, Cropping, Dynamic Zoom, Composite, Speed Change,
            // Retime and Scaling (then our Masks).
            for (const char* id : {groups::Transform, groups::Cropping}) {
                if (const EffectDescriptor* d = registry.find(QLatin1String(id)))
                    addDescriptorGroup(VideoTab, *d, ParamScope::Transform, m_clipId);
            }
            buildDynamicZoomSection(m_clipId);
            if (const EffectDescriptor* d = registry.find(QLatin1String(groups::Composite)))
                addDescriptorGroup(VideoTab, *d, ParamScope::Transform, m_clipId);
            if (c->kind == ClipKind::Media || c->kind == ClipKind::Nested)
                buildSpeedSections(*c);
            buildMasksSection(*c);
        } else {
            addPlaceholder(VideoTab, tr("Audio clips have no video properties."));
        }
        // Audio tab
        if (m_audioClipId) {
            if (const EffectDescriptor* d = registry.find(QLatin1String(groups::Audio)))
                addDescriptorGroup(AudioTab, *d, ParamScope::Audio, m_audioClipId);
        } else {
            addPlaceholder(AudioTab, tr("This clip has no audio."));
        }
        if (c->kind != ClipKind::Subtitle)
            buildEffectsTab(*c);
        // Title tab
        if (c->kind == ClipKind::Subtitle) {
            // built by buildCaptionsTab()
        } else if (c->kind == ClipKind::Title) {
            // The clip's preset (Left Lower Third, Scroll, ...) or Text: one group.
            if (const EffectDescriptor* d = titles::descriptorOf(*c))
                addDescriptorGroup(TitleTab, *d, ParamScope::Title, m_clipId);
        } else {
            addPlaceholder(TitleTab,
                tr("Not a title clip. Double-click a title in the Effects library to add one."));
        }
        if (c->kind == ClipKind::Subtitle)
            addPlaceholder(TransitionTab, tr("Subtitle clips have no transitions."));
        else
            buildTransitionTab(*c);
    }
    for (int tab = 0; tab < kTabCount; ++tab)
        tabLayout(Tab(tab))->addStretch(1);

    refreshValues();
    // Restore the scroll positions once the new pages have a size.
    for (int i = 0; i < m_tabScroll.size(); ++i)
        m_tabScroll[i]->verticalScrollBar()->setValue(scrollPositions[i]);
}

void InspectorPanel::buildCaptionsTab(const ClipData& c)
{
    // Like Resolve's subtitle inspector: the caption (text + whether it has its own style),
    // its style when it does, and the default style of its track (every caption without a
    // custom style follows it).
    const EffectRegistry& registry = EffectRegistry::instance();
    addPlaceholder(VideoTab, tr("Subtitle clips are drawn over the picture; see Captions."));
    addPlaceholder(EffectsTab, tr("Subtitle clips have no effects."));
    if (const EffectDescriptor* d = registry.find(subtitles::CaptionGroup))
        addDescriptorGroup(TitleTab, *d, ParamScope::Title, m_clipId);
    const EffectDescriptor* style = registry.find(subtitles::StyleGroup);
    if (!style)
        return;
    if (subtitles::hasOwnStyle(c)) {
        EffectDescriptor own = *style;
        own.name = tr("Caption Style");
        addDescriptorGroup(TitleTab, own, ParamScope::Title, m_clipId);
    }
    const auto loc = editops::findClip(m_ctx.project->timeline()->data(), m_clipId);
    if (!loc)
        return;
    const TrackData* track = m_ctx.project->timeline()->data().track(loc->track);
    EffectDescriptor trackStyle = *style;
    trackStyle.name = tr("Track Style (%1)").arg(trackDisplayName(*track, loc->track.index));
    addDescriptorGroup(TitleTab, trackStyle, ParamScope::Title, trackStyleClipId(track->id));
}

void InspectorPanel::buildEffectsTab(const ClipData& c)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    const EffectCategory category
        = m_structure.onVideoTrack ? EffectCategory::VideoEffect : EffectCategory::AudioEffect;

    // Toolbar: "Add Effect" menu with every effect that fits this clip's track.
    auto* bar = new QWidget;
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(8, 6, 8, 6);
    m_addEffect = new QToolButton(bar);
    m_addEffect->setText(tr("Add Effect"));
    m_addEffect->setIcon(inspectorGlyph(InspectorGlyph::Add));
    m_addEffect->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_addEffect->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(m_addEffect);
    const QVector<const EffectDescriptor*> effects = registry.byCategory(category);
    auto addEntry = [&](QMenu* into, const EffectDescriptor* d) {
        const QString id = d->id;
        const quint64 clipId = m_clipId;
        into->addAction(d->name, this,
            [this, id, clipId] { effectactions::applyEffect(*m_ctx.project, {clipId}, id); });
    };
    // One submenu per library folder (Blur, Color, Key, ...), like the Effects library.
    for (const QString& group : registry.groups(category)) {
        QMenu* sub = menu->addMenu(EffectRegistry::groupName(group));
        for (const EffectDescriptor* d : effects) {
            if (d->group == group)
                addEntry(sub, d);
        }
    }
    for (const EffectDescriptor* d : effects) {
        if (d->group.isEmpty())
            addEntry(menu, d);
    }
    m_addEffect->setMenu(menu);
    m_addEffect->setEnabled(!menu->isEmpty());
    barLayout->addWidget(m_addEffect);
    barLayout->addStretch(1);
    tabLayout(EffectsTab)->addWidget(bar);

    if (c.effects.isEmpty()) {
        addPlaceholder(EffectsTab,
            tr("No effects on this clip.\nDrag one from the Effects library or use Add Effect."));
        return;
    }
    for (int i = 0; i < c.effects.size(); ++i) {
        const EffectInstance& fx = c.effects[i];
        const EffectDescriptor* d = registry.find(fx.type);
        const QString key = QStringLiteral("fx:%1").arg(fx.id);
        ParamGroupBox* group = addGroup(EffectsTab, key, d ? d->name : fx.type, m_clipId);
        const quint64 clipId = m_clipId;
        const quint64 effectId = fx.id;

        QCheckBox* enable = group->addEnableToggle();
        attachHelp(group, fx.type, d ? d->name : fx.type);
        connect(enable, &QCheckBox::clicked, this, [this, clipId, effectId](bool on) {
            editClip(on ? tr("Enable Effect") : tr("Disable Effect"), clipId,
                [effectId, on](ClipData& cd) {
                    if (EffectInstance* e = paramaccess::effect(cd, effectId))
                        e->enabled = on;
                });
        });
        group->setHeaderRefresh([enable, effectId](const ClipData& cd) {
            const EffectInstance* e = paramaccess::effect(cd, effectId);
            const bool on = e && e->enabled;
            if (enable->isChecked() != on)
                enable->setChecked(on);
        });
        QToolButton* up = group->addHeaderButton(InspectorGlyph::MoveUp, tr("Move up"));
        QToolButton* down = group->addHeaderButton(InspectorGlyph::MoveDown, tr("Move down"));
        QToolButton* remove = group->addHeaderButton(InspectorGlyph::Remove, tr("Remove effect"));
        up->setEnabled(i > 0);
        down->setEnabled(i + 1 < c.effects.size());
        connect(up, &QToolButton::clicked, this, [this, effectId] { moveEffect(effectId, -1); });
        connect(down, &QToolButton::clicked, this, [this, effectId] { moveEffect(effectId, +1); });
        connect(remove, &QToolButton::clicked, this, [this, effectId] { removeEffect(effectId); });

        if (!d) {
            group->addWidgetRow(
                {}, createPlaceholderContent(tr("Unknown effect type “%1”.").arg(fx.type)));
            continue;
        }
        for (const ParamDescriptor& p : d->params)
            group->addParam(ParamRef {clipId, ParamScope::Effect, effectId, p.key}, p, *this);
    }
}

void InspectorPanel::moveEffect(quint64 effectId, int delta)
{
    editClip(tr("Reorder Effects"), m_clipId, [effectId, delta](ClipData& c) {
        for (int i = 0; i < c.effects.size(); ++i) {
            if (c.effects[i].id != effectId)
                continue;
            const int target = i + delta;
            if (target >= 0 && target < c.effects.size())
                c.effects.move(i, target);
            return;
        }
    });
}

void InspectorPanel::removeEffect(quint64 effectId)
{
    editClip(tr("Remove Effect"), m_clipId, [effectId](ClipData& c) {
        c.effects.removeIf([effectId](const EffectInstance& fx) { return fx.id == effectId; });
    });
}

TrackType InspectorPanel::trackTypeOfClip(quint64 clipId) const
{
    const auto loc = editops::findClip(m_ctx.project->timeline()->data(), clipId);
    return loc ? loc->track.type : TrackType::Video;
}

void InspectorPanel::buildTransitionTab(const ClipData& c)
{
    if (!c.transitionIn && !c.transitionOut) {
        addPlaceholder(TransitionTab,
            tr("No transitions on this clip.\nDrag one from the Effects library onto the "
               "clip, or add one here."));
    }
    if (c.transitionIn)
        addTransitionGroup(c, true);
    if (c.transitionOut)
        addTransitionGroup(c, false);

    // Buttons for the missing ends.
    auto* bar = new QWidget;
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(8, 6, 8, 6);
    const quint64 clipId = m_clipId;
    const QString defaultId = effectactions::defaultTransitionId(trackTypeOfClip(clipId));
    if (!c.transitionIn) {
        auto* addIn = new QPushButton(inspectorGlyph(InspectorGlyph::Add), tr("Transition In"));
        connect(addIn, &QPushButton::clicked, this, [this, clipId, defaultId] {
            effectactions::applyTransition(*m_ctx.project, clipId, defaultId, true);
        });
        layout->addWidget(addIn);
    }
    if (!c.transitionOut) {
        auto* addOut = new QPushButton(inspectorGlyph(InspectorGlyph::Add), tr("Transition Out"));
        connect(addOut, &QPushButton::clicked, this, [this, clipId, defaultId] {
            effectactions::applyTransition(*m_ctx.project, clipId, defaultId, false);
        });
        layout->addWidget(addOut);
    }
    layout->addStretch(1);
    if (!c.transitionIn || !c.transitionOut)
        tabLayout(TransitionTab)->addWidget(bar);
    else
        delete bar;
}

void InspectorPanel::addTransitionGroup(const ClipData& c, bool atHead)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    const TransitionData& t = atHead ? *c.transitionIn : *c.transitionOut;
    const ParamScope scope = atHead ? ParamScope::TransitionIn : ParamScope::TransitionOut;
    const quint64 clipId = m_clipId;
    ParamGroupBox* group = addGroup(TransitionTab,
        atHead ? QStringLiteral("transition.in") : QStringLiteral("transition.out"),
        atHead ? tr("Transition In") : tr("Transition Out"), clipId);
    if (const EffectDescriptor* d = registry.find(t.type))
        attachHelp(group, d->id, d->name);
    QToolButton* remove = group->addHeaderButton(InspectorGlyph::Remove, tr("Remove transition"));
    connect(remove, &QToolButton::clicked, this, [this, clipId, atHead] {
        effectactions::removeTransition(*m_ctx.project, clipId, atHead);
    });

    // Type: switching keeps the length (applyTransition with duration < 1).
    auto* type = new QComboBox;
    type->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    type->setMinimumContentsLength(6);
    // The transitions of the clip's track kind: video transitions on video clips, audio
    // transitions on audio clips. An audio clip still holding a video id (older projects)
    // plays the +3 dB crossfade, so that entry is shown.
    const TrackType trackType = trackTypeOfClip(clipId);
    const EffectCategory category = trackType == TrackType::Audio ? EffectCategory::AudioTransition
                                                                  : EffectCategory::Transition;
    for (const EffectDescriptor* d : registry.byCategory(category))
        type->addItem(d->name, d->id);
    int current = type->findData(t.type);
    if (current < 0 && category == EffectCategory::AudioTransition)
        current = type->findData(audiotransitions::defaultId());
    type->setCurrentIndex(current);
    connect(type, &QComboBox::activated, this, [this, type, clipId, atHead](int index) {
        effectactions::applyTransition(
            *m_ctx.project, clipId, type->itemData(index).toString(), atHead);
    });
    group->addWidgetRow(tr("Type"), type);

    // Alignment on the cut: Centre / Start / End (only meaningful between two clips; a fade
    // at a free end keeps its place).
    auto* alignment = new QComboBox;
    alignment->addItem(tr("Center on Edit"), int(TransitionAlignment::Centre));
    alignment->addItem(tr("Start on Edit"), int(TransitionAlignment::StartOnCut));
    alignment->addItem(tr("End on Edit"), int(TransitionAlignment::EndOnCut));
    alignment->setCurrentIndex(alignment->findData(int(t.alignment)));
    const int before = alignment->currentIndex();
    connect(alignment, &QComboBox::activated, this,
        [this, alignment, clipId, atHead, before](int index) {
            const auto value = TransitionAlignment(alignment->itemData(index).toInt());
            Project& project = *m_ctx.project;
            project.beginEdit(tr("Transition Alignment"));
            if (editops::setTransitionAlignment(
                    project.timeline()->mutableData(), clipId, atHead, value))
                project.endEdit();
            else {
                project.cancelEdit();
                alignment->setCurrentIndex(before); // nothing changed (e.g. a fade at a free end)
            }
        });
    group->addWidgetRow(tr("Alignment"), alignment);

    // Synthetic "Duration" parameter routed to TransitionData::duration.
    ParamDescriptor duration;
    duration.key = paramaccess::kTransitionDuration;
    duration.label = tr("Duration");
    duration.type = ParamType::Int;
    duration.min = 1;
    duration.max = double(std::max<int64_t>(1, c.duration));
    duration.def
        = double(effectactions::defaultTransitionFrames(m_ctx.project->timeline()->data().fps));
    duration.step = 1;
    duration.unit = tr("fr");
    duration.animatable = false;
    group->addParam(ParamRef {clipId, scope, 0, duration.key}, duration, *this);

    if (const EffectDescriptor* d = registry.find(t.type)) {
        for (const ParamDescriptor& p : d->params)
            group->addParam(ParamRef {clipId, scope, 0, p.key}, p, *this);
    }
}

// ---- Refreshing ----

void InspectorPanel::refreshValues()
{
    // O(rows): two clip lookups at most (video clip + linked audio clip), then each row reads
    // its value. No widgets are created and rows skip unchanged values.
    const Timeline* timeline = m_ctx.project->timeline();
    const int64_t playhead = timeline->playhead();
    quint64 cachedId = 0;
    const ClipData* cached = nullptr;
    for (int i = 0; i < m_groups.size(); ++i) {
        const quint64 id = m_groupClip[i];
        if (id != cachedId) {
            cachedId = id;
            cached = clip(id);
        }
        if (!cached)
            continue;
        const int64_t local = std::clamp<int64_t>(
            cached->localFrame(playhead), 0, std::max<int64_t>(0, cached->duration - 1));
        m_groups[i]->refresh(*cached, local, cached->contains(playhead));
    }
}

void InspectorPanel::updateHeader()
{
    const ClipData* c = clip(m_clipId);
    // Like Resolve, the inspector shows only its tabs while nothing is selected (the tab
    // pages explain what to do); the header appears with a clip.
    m_clipName->parentWidget()->setVisible(c != nullptr);
    if (!c) {
        m_clipName->setText(tr("No clip selected"));
        m_clipInfo->clear();
        m_clipInfo->hide();
        return;
    }
    m_clipName->setText(c->name.isEmpty() ? tr("Untitled clip") : c->name);
    QString info;
    switch (c->kind) {
    case ClipKind::Media:
        info = m_structure.onVideoTrack ? tr("Video clip") : tr("Audio clip");
        break;
    case ClipKind::Title:
        info = tr("Title");
        break;
    case ClipKind::Solid:
        info = tr("Solid color");
        break;
    case ClipKind::Adjustment:
        info = tr("Adjustment clip");
        break;
    case ClipKind::Generator: {
        const EffectDescriptor* d = EffectRegistry::instance().find(c->generator);
        info = d ? tr("Generator: %1").arg(d->name) : tr("Generator");
        break;
    }
    case ClipKind::Subtitle:
        info = tr("Subtitle");
        break;
    case ClipKind::Nested: {
        // Compound clip or nested timeline: transform, opacity, effects and volume act on
        // the nested result as a whole.
        const auto nested = m_ctx.project->timelineData(c->nestedUid);
        if (!nested)
            info = tr("Compound clip (offline)");
        else
            info = nested->compound ? tr("Compound clip") : tr("Timeline clip");
        break;
    }
    }
    const int selected = selectedItemCount();
    if (selected > 1) {
        // Multi-clip editing (spec 24.1): say which clips an edit reaches.
        const int reach = int(editPeers(m_clipId).size()) + 1;
        info = reach >= selected ? tr("%1 clips selected, edits apply to all").arg(selected)
            : reach > 1
            ? tr("%1 clips selected, edits apply to %2 of this kind").arg(selected).arg(reach)
            : tr("%1 clips selected, showing the last one").arg(selected);
    }
    if (m_clipInfo->text() != info)
        m_clipInfo->setText(info);
    m_clipInfo->show();
}

// ---- Drag and drop (effects library) ----

void InspectorPanel::changeEvent(QEvent* event)
{
    PanelFrame::changeEvent(event);
    // The style sheet is generated from the palette: regenerate it after a colour scheme
    // switch. Only when the text differs, because setStyleSheet() re-polishes every child.
    //
    // Built from the APPLICATION palette, never palette(): setStyleSheet() re-polishes this
    // widget, the style sheet style assigns it a derived palette, and that sends another
    // PaletteChange. With palette() as input every round produced a slightly different sheet
    // and the recursion overflowed the stack (the alpha.1 crash on a colour scheme switch).
    // The guard keeps the nested PaletteChange from re-entering even if the input changes.
    if ((event->type() == QEvent::PaletteChange
            || event->type() == QEvent::ApplicationPaletteChange)
        && !m_updatingStyleSheet) {
        const QString sheet = inspectorStyleSheet(QApplication::palette());
        if (sheet != styleSheet()) {
            m_updatingStyleSheet = true;
            setStyleSheet(sheet);
            m_updatingStyleSheet = false;
        }
    }
}

void InspectorPanel::dragEnterEvent(QDragEnterEvent* event)
{
    if (m_clipId && event->mimeData()->hasFormat(QLatin1String(mime::Effect)))
        event->acceptProposedAction();
    else
        event->ignore();
}

void InspectorPanel::dragMoveEvent(QDragMoveEvent* event)
{
    // Answer every move explicitly (Wayland: each one becomes wl_data_offer.accept).
    if (m_clipId && event->mimeData()->hasFormat(QLatin1String(mime::Effect)))
        event->acceptProposedAction();
    else
        event->ignore();
}

void InspectorPanel::dropEvent(QDropEvent* event)
{
    const QString id = QString::fromUtf8(event->mimeData()->data(QLatin1String(mime::Effect)));
    const EffectDescriptor* desc = EffectRegistry::instance().find(id);
    if (!desc || !m_clipId) {
        event->ignore();
        return;
    }
    bool done = false;
    if (desc->category == EffectCategory::Transition
        || desc->category == EffectCategory::AudioTransition) {
        const ClipData* c = clip(m_clipId);
        const int64_t playhead = m_ctx.project->timeline()->playhead();
        const bool atHead = !c || std::abs(playhead - c->start) <= std::abs(c->end() - playhead);
        done = effectactions::applyTransition(*m_ctx.project, m_clipId, id, atHead);
        if (done)
            m_tabs->setCurrentIndex(TransitionTab);
    } else {
        done = effectactions::applyEffect(*m_ctx.project, {m_clipId}, id) > 0;
        if (done)
            m_tabs->setCurrentIndex(EffectsTab);
    }
    if (done)
        event->acceptProposedAction();
}

} // namespace kk
