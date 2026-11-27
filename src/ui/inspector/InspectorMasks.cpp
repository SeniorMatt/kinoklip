// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorMasks — the Masks section of the Inspector's Video tab: add buttons per shape type,
// one group per mask (enable, on-screen edit, reorder, delete, keyframe diamonds, the
// "builtin.mask" descriptor rows and the shape keyframe row). Part of InspectorPanel.
#include "compositing/tracking/TrackFollow.h"
#include "core/EditOps.h"
#include "core/EffectTrack.h"
#include "core/MatchMove.h"
#include "core/VideoEffectKeys.h"
#include "core/KeyframeOps.h"
#include "core/Plural.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/ClipGeometry.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/KeyframeButton.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace kk {

namespace {

/// Default size of a new rectangle / ellipse: half of the clip's picture (centred).
constexpr double kDefaultMaskFraction = 0.5;

struct ShapeButton
{
    MaskShape shape;
    InspectorGlyph glyph;
    const char* tip;
};

const ShapeButton kShapeButtons[] = {
    {MaskShape::Rectangle, InspectorGlyph::MaskRectangle,
        QT_TRANSLATE_NOOP("kk::InspectorPanel", "Add a rectangle mask")},
    {MaskShape::Ellipse, InspectorGlyph::MaskEllipse,
        QT_TRANSLATE_NOOP("kk::InspectorPanel", "Add an ellipse mask")},
    {MaskShape::Polygon, InspectorGlyph::MaskPolygon,
        QT_TRANSLATE_NOOP("kk::InspectorPanel",
            "Add a polygon mask: click points in the viewer, click the first point to close")},
    {MaskShape::Bezier, InspectorGlyph::MaskBezier,
        QT_TRANSLATE_NOOP("kk::InspectorPanel",
            "Add a bezier mask: click for corners, drag for smooth points, click the first point "
            "to close")},
    {MaskShape::Freehand, InspectorGlyph::MaskFreehand,
        QT_TRANSLATE_NOOP("kk::InspectorPanel", "Add a freehand mask: draw it in the viewer")},
};

/// The descriptor parameters shown for @p shape (width / height only for parametric shapes).
bool showsParam(const ParamDescriptor& p, MaskShape shape)
{
    const bool size = p.key == maskkeys::Width || p.key == maskkeys::Height;
    return !(size && (shape != MaskShape::Rectangle && shape != MaskShape::Ellipse));
}

} // namespace

void InspectorPanel::buildMasksSection(const ClipData& c)
{
    const EffectDescriptor* desc
        = EffectRegistry::instance().find(QLatin1String(kMaskDescriptorId));
    if (!desc)
        return;
    const quint64 clipId = m_clipId;

    // Section header group with one add button per shape type.
    ParamGroupBox* section = addGroup(VideoTab, QStringLiteral("masks"), desc->name, clipId);
    section->setObjectName(QStringLiteral("mask-section"));
    auto* bar = new QWidget;
    bar->setObjectName(QStringLiteral("InspectorMaskButtons"));
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(2);
    for (const ShapeButton& b : kShapeButtons) {
        auto* button = new QToolButton(bar);
        button->setObjectName(QStringLiteral("add-mask:") + maskShapeToString(b.shape));
        button->setAutoRaise(true);
        button->setIcon(inspectorGlyph(b.glyph));
        button->setIconSize(QSize(18, 18));
        button->setToolTip(tr(b.tip));
        const MaskShape shape = b.shape;
        connect(button, &QToolButton::clicked, this, [this, shape] { addMask(shape); });
        barLayout->addWidget(button);
    }
    barLayout->addStretch(1);
    section->addWidgetRow(tr("Add"), bar);
    if (c.masks.isEmpty()) {
        auto* hint = new QLabel(tr("Pick a shape, then shape it on the timeline viewer."));
        hint->setObjectName(QStringLiteral("InspectorParamLabel"));
        hint->setWordWrap(true);
        section->addWidgetRow({}, hint);
    }

    for (int i = 0; i < c.masks.size(); ++i) {
        const MaskData& mask = c.masks[i];
        const quint64 maskId = mask.id;
        ParamGroupBox* group = addGroup(
            VideoTab, QStringLiteral("mask:%1").arg(maskId), maskDisplayName(mask, i), clipId);
        group->setObjectName(QStringLiteral("mask-group:%1").arg(maskId));

        QCheckBox* enable = group->addEnableToggle();
        enable->setObjectName(QStringLiteral("mask-enable:%1").arg(maskId));
        connect(enable, &QCheckBox::clicked, this, [this, clipId, maskId](bool on) {
            editClip(
                on ? tr("Enable Mask") : tr("Disable Mask"), clipId, [maskId, on](ClipData& cd) {
                    if (MaskData* m = paramaccess::mask(cd, maskId))
                        m->enabled = on;
                });
        });
        QToolButton* edit
            = group->addHeaderButton(InspectorGlyph::EditOnScreen, tr("Edit in the viewer"));
        edit->setObjectName(QStringLiteral("mask-edit:%1").arg(maskId));
        edit->setCheckable(true);
        connect(edit, &QToolButton::clicked, this, [this, maskId](bool on) {
            if (m_ctx.selection)
                m_ctx.selection->setActiveMask(on ? maskId : 0);
        });
        QToolButton* up = group->addHeaderButton(InspectorGlyph::MoveUp, tr("Move up"));
        QToolButton* down = group->addHeaderButton(InspectorGlyph::MoveDown, tr("Move down"));
        QToolButton* remove = group->addHeaderButton(InspectorGlyph::Remove, tr("Delete mask"));
        remove->setObjectName(QStringLiteral("mask-remove:%1").arg(maskId));
        up->setEnabled(i > 0);
        down->setEnabled(i + 1 < c.masks.size());
        connect(up, &QToolButton::clicked, this, [this, maskId] { moveMask(maskId, -1); });
        connect(down, &QToolButton::clicked, this, [this, maskId] { moveMask(maskId, +1); });
        connect(remove, &QToolButton::clicked, this, [this, maskId] { removeMask(maskId); });

        // Section diamond: keys every animatable parameter of the mask (and its shape).
        QVector<ParamDescriptor> keyed;
        for (const ParamDescriptor& p : desc->params) {
            if (p.isScalar() && p.animatable && showsParam(p, mask.shape))
                keyed.append(p);
        }
        ParamDescriptor shapeDesc;
        shapeDesc.key = maskkeys::Shape;
        shapeDesc.label = tr("Shape");
        if (mask.isPathShape())
            keyed.append(shapeDesc);
        auto* diamond = new KeyframeButton;
        diamond->setObjectName(QStringLiteral("keyframe-section:mask:%1").arg(maskId));
        diamond->setToolTip(tr("Add or remove keyframes of every parameter of this mask"));
        group->addHeaderWidget(diamond);
        connect(diamond, &KeyframeButton::clicked, this, [this, keyed, clipId, maskId] {
            const ClipData* cd = clip(clipId);
            if (!cd)
                return;
            const int64_t frame = localFrame(clipId);
            bool allOnKey = true;
            for (const ParamDescriptor& p : keyed) {
                const AnimatedValue* v = paramaccess::animated(
                    *cd, ParamRef {clipId, ParamScope::Mask, maskId, p.key});
                allOnKey = allOnKey && v && v->hasKeyAt(frame);
            }
            editClip(allOnKey ? tr("Remove Mask Keyframes") : tr("Add Mask Keyframes"), clipId,
                [&](ClipData& target) {
                    for (const ParamDescriptor& p : keyed) {
                        const ParamRef ref {clipId, ParamScope::Mask, maskId, p.key};
                        const AnimatedValue* v = paramaccess::animated(target, ref);
                        const bool has = v && v->hasKeyAt(frame);
                        if (has == allOnKey)
                            paramaccess::toggleKey(target, ref, p, frame);
                    }
                });
        });

        // Shape row: point count / state and the shape keyframe diamond.
        QLabel* shapeInfo = nullptr;
        KeyframeButton* shapeKey = nullptr;
        if (mask.isPathShape()) {
            auto* row = new QWidget;
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            shapeInfo = new QLabel(row);
            shapeInfo->setObjectName(QStringLiteral("mask-shape-info:%1").arg(maskId));
            shapeKey = new KeyframeButton(row);
            shapeKey->setObjectName(QStringLiteral("keyframe:mask-shape:%1").arg(maskId));
            shapeKey->setToolTip(tr("Add or remove a shape keyframe at the playhead"));
            rowLayout->addWidget(shapeInfo, 1);
            rowLayout->addWidget(shapeKey);
            group->addWidgetRow(tr("Shape"), row);
            connect(shapeKey, &KeyframeButton::clicked, this, [this, clipId, maskId, shapeDesc] {
                const int64_t frame = localFrame(clipId);
                const ClipData* cd = clip(clipId);
                const MaskData* m = cd ? paramaccess::mask(*cd, maskId) : nullptr;
                if (!m)
                    return;
                const bool has = m->path.timing.hasKeyAt(frame);
                editClip(has ? tr("Remove Shape Keyframe") : tr("Add Shape Keyframe"), clipId,
                    [&](ClipData& target) {
                        paramaccess::toggleKey(target,
                            ParamRef {clipId, ParamScope::Mask, maskId, maskkeys::Shape}, shapeDesc,
                            frame);
                    });
            });
        }

        // The Mask Tracker: the mask follows the track of one of the clip's Match Move effects
        // (made with that effect's Track buttons) with its position, size and angle, or its points.
        {
            auto* picker = new QComboBox;
            picker->setObjectName(QStringLiteral("mask-track:%1").arg(maskId));
            picker->setToolTip(tr("A Match Move effect of this clip that has been tracked"));
            int number = 0;
            for (const EffectInstance& fx : c.effects) {
                if (fx.type != QLatin1String(fxid::MatchMove))
                    continue;
                ++number;
                if (matchmove::isActive(fx))
                    picker->addItem(tr("Match Move %1").arg(number), qulonglong(fx.id));
            }
            if (picker->count() > 0) {
                auto* row = new QWidget;
                auto* rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->addWidget(picker, 1);
                const auto tick = [&](const QString& name, const QString& text, const QString& tip,
                                      bool on) {
                    auto* box = new QCheckBox(text, row);
                    box->setObjectName(name.arg(maskId));
                    box->setToolTip(tip);
                    box->setChecked(on);
                    rowLayout->addWidget(box);
                    return box;
                };
                QCheckBox* pos = tick(QStringLiteral("mask-track-position:%1"), tr("Position"),
                    tr("Follow the position of the track"), true);
                QCheckBox* size = tick(QStringLiteral("mask-track-size:%1"), tr("Size"),
                    tr("Follow the scale of the track (a region track)"), false);
                QCheckBox* angle = tick(QStringLiteral("mask-track-angle:%1"), tr("Angle"),
                    tr("Follow the rotation of the track (a region track)"), false);
                QCheckBox* points = nullptr;
                if (mask.isPathShape())
                    points = tick(QStringLiteral("mask-track-points:%1"), tr("Points"),
                        tr("Move the points of the shape (shape keyframes) and leave the mask's "
                           "own position, size and angle alone"),
                        false);
                auto* go = new QToolButton(row);
                go->setObjectName(QStringLiteral("mask-track-apply:%1").arg(maskId));
                go->setText(tr("Track Mask"));
                go->setToolTip(tr("Copy the track into keyframes of this mask, keeping its place "
                                  "at the playhead and following the track's movement"));
                rowLayout->addWidget(go);
                group->addWidgetRow(tr("Follow Track"), row);
                connect(go, &QToolButton::clicked, this,
                    [this, clipId, maskId, picker, pos, size, angle, points] {
                        const ClipData* cd = clip(clipId);
                        if (!cd || picker->currentData().isNull())
                            return;
                        const quint64 effectId = picker->currentData().toULongLong();
                        tracking::FollowOptions o;
                        o.position = pos->isChecked();
                        o.size = size->isChecked();
                        o.angle = angle->isChecked();
                        o.points = points && points->isChecked();
                        const int64_t frame = localFrame(clipId);
                        const TimelineData& tl = m_ctx.project->timeline()->data();
                        const QSizeF frameSize(tl.width, tl.height);
                        const Rational fps = tl.fps;
                        editClip(tr("Track Mask"), clipId, [=](ClipData& target) {
                            MaskData* m = paramaccess::mask(target, maskId);
                            const EffectInstance* fx = nullptr;
                            for (const EffectInstance& e : target.effects) {
                                if (e.id == effectId)
                                    fx = &e;
                            }
                            if (m && fx)
                                tracking::followMatchMove(
                                    *m, target, *fx, frame, frameSize, fps, o);
                        });
                    });
            }
        }

        // Follow Subject: the mask follows what is under it, no tracker to set up first.
        QToolButton* followGo = nullptr;
        QToolButton* followStop = nullptr;
        QLabel* followStatus = nullptr;
        {
            auto* row = new QWidget;
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            followGo = new QToolButton(row);
            followGo->setObjectName(QStringLiteral("mask-follow-subject:%1").arg(maskId));
            followGo->setText(tr("Track"));
            followGo->setToolTip(tr("Make this mask follow the subject under it: tracks from the "
                                    "playhead to the end of the clip and back to its start, then "
                                    "keys the mask's position, scale and rotation"));
            followStop = new QToolButton(row);
            followStop->setObjectName(QStringLiteral("mask-follow-stop:%1").arg(maskId));
            followStop->setText(tr("Cancel"));
            followStatus = new QLabel(row);
            followStatus->setObjectName(QStringLiteral("mask-follow-status:%1").arg(maskId));
            followStatus->setWordWrap(true);
            rowLayout->addWidget(followGo);
            rowLayout->addWidget(followStop);
            rowLayout->addWidget(followStatus, 1);
            group->addWidgetRow(tr("Follow Subject"), row);
            connect(followGo, &QToolButton::clicked, this,
                [this, clipId, maskId] { startMaskTrack(clipId, maskId); });
            connect(followStop, &QToolButton::clicked, this, [this] { m_effectTracker->stop(); });
        }

        group->setHeaderRefresh([this, enable, edit, diamond, shapeInfo, shapeKey, keyed, clipId,
                                    maskId, followGo, followStop, followStatus,
                                    index = i](const ClipData& cd) {
            const MaskData* m = paramaccess::mask(cd, maskId);
            if (!m)
                return;
            {
                const bool running = m_effectTracker->isRunning();
                const bool mine = running && m_maskTrack && m_maskTrack->maskId == maskId;
                followGo->setEnabled(!running);
                followStop->setVisible(mine);
                QString text;
                if (mine)
                    text = tr("Tracking... %1 frames").arg(
                        m_maskTrack->samples.size() + m_effectTracker->samples().size());
                else if (m_maskTrackMessageMask == maskId && !m_maskTrackMessage.isEmpty())
                    text = m_maskTrackMessage;
                if (followStatus->text() != text)
                    followStatus->setText(text);
            }
            if (enable->isChecked() != m->enabled)
                enable->setChecked(m->enabled);
            const bool active = m_ctx.selection && m_ctx.selection->activeMask() == maskId;
            if (edit->isChecked() != active)
                edit->setChecked(active);
            const int64_t playhead = m_ctx.project->timeline()->playhead();
            const int64_t frame = cd.localFrame(playhead);
            bool any = false;
            bool all = true;
            for (const ParamDescriptor& p : keyed) {
                const AnimatedValue* v
                    = paramaccess::animated(cd, ParamRef {clipId, ParamScope::Mask, maskId, p.key});
                any = any || (v && v->isAnimated());
                all = all && v && v->hasKeyAt(frame);
            }
            using S = KeyframeButton::State;
            diamond->setState(all ? S::OnKey : (any ? S::Animated : S::NotAnimated));
            diamond->setEnabled(cd.contains(playhead));
            if (shapeInfo) {
                const qsizetype n = m->path.pointCount();
                const QString text = !m->closed ? plural(n, tr("%n point, drawing in the viewer"),
                                                      tr("%n points, drawing in the viewer"))
                                                : plural(n, tr("%n point"), tr("%n points"));
                if (shapeInfo->text() != text)
                    shapeInfo->setText(text);
                const AnimatedValue& t = m->path.timing;
                shapeKey->setState(!t.isAnimated() ? S::NotAnimated
                        : t.hasKeyAt(frame)        ? S::OnKey
                                                   : S::Animated);
                shapeKey->setEnabled(cd.contains(playhead));
            }
            Q_UNUSED(index);
        });

        for (const ParamDescriptor& p : desc->params) {
            if (showsParam(p, mask.shape))
                group->addParam(ParamRef {clipId, ParamScope::Mask, maskId, p.key}, p, *this);
        }
    }
}

void InspectorPanel::addMask(MaskShape shape)
{
    const ClipData* c = clip(m_clipId);
    if (!c)
        return;
    const TimelineData& data = m_ctx.project->timeline()->data();
    const QSizeF content = clipContentSize(m_ctx.project, *c, QSizeF(data.width, data.height));
    quint64 newId = 0;
    editClip(tr("Add %1 Mask").arg(maskShapeName(shape)), m_clipId, [&](ClipData& target) {
        // Ids come from the timeline's allocator (unique across clips: keyframe paths).
        newId = m_ctx.project->timeline()->mutableData().allocateId();
        target.masks.append(makeMask(shape, newId, content * kDefaultMaskFraction));
    });
    // The new mask is the one being edited: the viewer shows its handles (point shapes are
    // drawn there).
    if (newId && m_ctx.selection)
        m_ctx.selection->setActiveMask(newId);
}

void InspectorPanel::moveMask(quint64 maskId, int delta)
{
    editClip(tr("Reorder Masks"), m_clipId, [maskId, delta](ClipData& c) {
        for (int i = 0; i < c.masks.size(); ++i) {
            if (c.masks[i].id != maskId)
                continue;
            const int target = i + delta;
            if (target >= 0 && target < c.masks.size())
                c.masks.move(i, target);
            return;
        }
    });
}

void InspectorPanel::removeMask(quint64 maskId)
{
    editClip(tr("Delete Mask"), m_clipId, [maskId](ClipData& c) {
        c.masks.removeIf([maskId](const MaskData& m) { return m.id == maskId; });
    });
    if (m_ctx.selection && m_ctx.selection->activeMask() == maskId)
        m_ctx.selection->setActiveMask(0);
}

} // namespace kk
