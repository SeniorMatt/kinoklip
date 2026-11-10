// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeItemModel implementation: items from the selection, rows from the descriptors.
#include "ui/keyframes/KeyframeItemModel.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/TitleLayers.h"
#include "core/TitlePresets.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace kk {

namespace ko = keyframeops;

QVector<quint64> KeyframeItems::allClips() const
{
    QVector<quint64> out;
    for (const QVector<quint64>& item : items)
        out += item;
    return out;
}

QString KeyframeItems::firstItemName(const TimelineData& data) const
{
    if (items.isEmpty() || items.front().isEmpty())
        return {};
    const ClipData* c = editops::clipById(data, items.front().front());
    return c ? c->name : QString();
}

namespace keyframeitems {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::KeyframeItems", text);
}

/// Appends a group (header + parameter rows) for descriptor @p desc stored in @p scope.
/// Skips the group when it has no listed parameter.
void appendGroup(QVector<KeyframeEntry>& out, int& colorIndex, const ClipData& clip,
    const EffectDescriptor& desc, ParamScope scope, quint64 effectId, const QString& itemKey,
    int depth, bool showAll)
{
    const QString groupKey
        = QStringLiteral("group:%1/%2/%3").arg(clip.id).arg(desc.id).arg(effectId);
    KeyframeEntry group;
    group.kind = KeyRow::Kind::Group;
    group.label = desc.name;
    group.key = groupKey;
    group.parentKey = itemKey;
    group.depth = depth;
    QVector<KeyframeEntry> params;
    for (const ParamDescriptor& pd : desc.params) {
        if (!pd.isScalar() || !pd.animatable)
            continue;
        const ParamRef ref {clip.id, scope, effectId, pd.key};
        const AnimatedValue* v = paramaccess::animated(clip, ref);
        if (!showAll && !(v && v->isAnimated()))
            continue;
        QString path;
        switch (scope) {
        case ParamScope::Transform:
            path = ko::memberPath(QStringLiteral("transform"), pd.key);
            break;
        case ParamScope::Audio:
            path = ko::memberPath(QStringLiteral("audio"), pd.key);
            break;
        case ParamScope::Title:
            path = ko::memberPath(QStringLiteral("title"), pd.key);
            break;
        case ParamScope::TitleLayer:
            path = ko::layerPath(effectId, pd.key);
            break;
        case ParamScope::Effect:
            path = ko::effectPath(effectId, pd.key);
            break;
        case ParamScope::Mask:
            path = ko::maskPath(effectId, pd.key);
            break;
        default:
            continue;
        }
        KeyframeEntry e;
        e.kind = KeyRow::Kind::Param;
        e.label = pd.label;
        e.key = QStringLiteral("param:%1/%2").arg(clip.id).arg(path);
        e.parentKey = groupKey;
        e.depth = depth + 1;
        e.params = {{clip.id, path}};
        e.ref = ref;
        e.desc = pd;
        e.colorIndex = colorIndex++;
        group.params.append(e.params.front());
        params.append(e);
    }
    if (params.isEmpty())
        return;
    out.append(group);
    out += params;
}

/// The "Masks" group: every mask's animatable parameters labelled "<mask>: <parameter>", and
/// a "<mask>: Shape" row for point masks (their shape keys).
void appendMasksGroup(QVector<KeyframeEntry>& out, int& colorIndex, const ClipData& clip,
    const QString& itemKey, int depth, bool showAll)
{
    const EffectDescriptor* desc
        = EffectRegistry::instance().find(QLatin1String(kMaskDescriptorId));
    if (!desc || clip.masks.isEmpty())
        return;
    const QString groupKey = QStringLiteral("group:%1/%2/0").arg(clip.id).arg(desc->id);
    KeyframeEntry group;
    group.kind = KeyRow::Kind::Group;
    group.label = desc->name;
    group.key = groupKey;
    group.parentKey = itemKey;
    group.depth = depth;
    QVector<KeyframeEntry> params;
    for (int i = 0; i < clip.masks.size(); ++i) {
        const MaskData& mask = clip.masks[i];
        const QString name = maskDisplayName(mask, i);
        auto add = [&](const ParamDescriptor& pd) {
            const ParamRef ref {clip.id, ParamScope::Mask, mask.id, pd.key};
            const AnimatedValue* v = paramaccess::animated(clip, ref);
            if (!showAll && !(v && v->isAnimated()))
                return;
            const QString path = ko::maskPath(mask.id, pd.key);
            KeyframeEntry e;
            e.kind = KeyRow::Kind::Param;
            e.label = name + QStringLiteral(": ") + pd.label;
            e.key = QStringLiteral("param:%1/%2").arg(clip.id).arg(path);
            e.parentKey = groupKey;
            e.depth = depth + 1;
            e.params = {{clip.id, path}};
            e.ref = ref;
            e.desc = pd;
            e.colorIndex = colorIndex++;
            group.params.append(e.params.front());
            params.append(e);
        };
        for (const ParamDescriptor& pd : desc->params) {
            if (!pd.isScalar() || !pd.animatable)
                continue;
            const bool size = pd.key == maskkeys::Width || pd.key == maskkeys::Height;
            if (size && mask.isPathShape())
                continue;
            add(pd);
        }
        if (mask.isPathShape()) {
            ParamDescriptor shape;
            shape.key = maskkeys::Shape;
            shape.label = tr(QT_TRANSLATE_NOOP("kk::KeyframeItems", "Shape"));
            shape.min = 0;
            shape.max = double(std::max<qsizetype>(1, mask.path.timing.keys.size() - 1));
            add(shape);
        }
    }
    if (params.isEmpty())
        return;
    out.append(group);
    out += params;
}

} // namespace

KeyframeItems build(const TimelineData& data, const QVector<quint64>& selectedClips, bool showAll)
{
    KeyframeItems model;
    // Items: a selected clip and everything linked to it, once (spec 24.1).
    QSet<quint64> seen;
    for (quint64 id : selectedClips) {
        if (seen.contains(id) || !editops::findClip(data, id))
            continue;
        QVector<quint64> item = editops::linkedClips(data, id);
        // Video members first: the item is named after the picture, like the Inspector.
        std::stable_sort(item.begin(), item.end(), [&](quint64 a, quint64 b) {
            const auto la = editops::findClip(data, a);
            const auto lb = editops::findClip(data, b);
            const bool va = la && la->track.type == TrackType::Video;
            const bool vb = lb && lb->track.type == TrackType::Video;
            return va && !vb;
        });
        for (quint64 m : item)
            seen.insert(m);
        model.items.append(item);
        if (model.items.size() >= kMaxItems)
            break;
    }
    if (model.items.isEmpty())
        return model;

    const EffectRegistry& reg = EffectRegistry::instance();
    const bool multi = model.items.size() > 1;
    model.entries.append(
        {KeyRow::Kind::Master, tr(QT_TRANSLATE_NOOP("kk::KeyframeItems", "Keyframes")),
            QStringLiteral("master"), QString(), 0, {}, {}, {}, -1});
    int colorIndex = 0;
    for (const QVector<quint64>& item : std::as_const(model.items)) {
        QString itemKey;
        int depth = 0;
        qsizetype itemRow = -1;
        if (multi) {
            const ClipData* first = editops::clipById(data, item.front());
            itemKey = QStringLiteral("item:%1").arg(item.front());
            itemRow = model.entries.size();
            model.entries.append({KeyRow::Kind::Item, first ? first->name : QString(), itemKey,
                QString(), 0, {}, {}, {}, -1});
            depth = 1;
        }
        const qsizetype firstParam = model.entries.size();
        for (quint64 id : item) {
            const auto loc = editops::findClip(data, id);
            const ClipData* clip = editops::clipById(data, id);
            if (!loc || !clip)
                continue;
            const bool video = loc->track.type == TrackType::Video;
            auto group = [&](const char* descId, ParamScope scope) {
                if (const EffectDescriptor* d = reg.find(QString::fromLatin1(descId)))
                    appendGroup(
                        model.entries, colorIndex, *clip, *d, scope, 0, itemKey, depth, showAll);
            };
            if (video) {
                group(groups::Transform, ParamScope::Transform);
                group(groups::Cropping, ParamScope::Transform);
                group(groups::Composite, ParamScope::Transform);
                appendMasksGroup(model.entries, colorIndex, *clip, itemKey, depth, showAll);
                if (clip->kind == ClipKind::Generator) {
                    if (const EffectDescriptor* d = reg.find(clip->generator))
                        appendGroup(model.entries, colorIndex, *clip, *d, ParamScope::Title, 0,
                            itemKey, depth, showAll);
                }
                if (clip->kind == ClipKind::Title) {
                    if (const EffectDescriptor* d = titles::descriptorOf(*clip))
                        appendGroup(model.entries, colorIndex, *clip, *d, ParamScope::Title, 0,
                            itemKey, depth, showAll);
                    // The other layers of the title, each a group named after the layer.
                    for (const TitleLayer& layer : clip->titleLayers) {
                        const EffectDescriptor* ld = layer.kind == TitleLayerKind::Shape
                            ? reg.find(QLatin1String(titlelayers::kShapeDescriptorId))
                            : titles::descriptorOf(*clip);
                        if (!ld)
                            continue;
                        EffectDescriptor named = *ld;
                        named.name = titlelayers::displayName(*clip, layer.id);
                        appendGroup(model.entries, colorIndex, *clip, named, ParamScope::TitleLayer,
                            layer.id, itemKey, depth, showAll);
                    }
                }
            } else if (loc->track.type == TrackType::Audio) {
                group(groups::Audio, ParamScope::Audio); // captions have no animated groups
            }
            for (const EffectInstance& fx : clip->effects) {
                if (const EffectDescriptor* d = reg.find(fx.type))
                    appendGroup(model.entries, colorIndex, *clip, *d, ParamScope::Effect, fx.id,
                        itemKey, depth, showAll);
            }
        }
        // Summary rows collect their parameters.
        for (qsizetype i = firstParam; i < model.entries.size(); ++i) {
            if (model.entries[i].kind != KeyRow::Kind::Param)
                continue;
            model.entries[0].params += model.entries[i].params;
            if (itemRow >= 0)
                model.entries[itemRow].params += model.entries[i].params;
        }
    }
    return model;
}

QVector<int> visibleRows(const QVector<KeyframeEntry>& entries, const QSet<QString>& collapsed)
{
    QVector<int> out;
    out.reserve(entries.size());
    // Entries come in tree order, so a parent precedes its children: remember which keys are
    // hidden or collapsed and test each entry's parent only (O(n)).
    QSet<QString> closed;
    for (int i = 0; i < entries.size(); ++i) {
        const KeyframeEntry& e = entries[i];
        const bool hidden = !e.parentKey.isEmpty() && closed.contains(e.parentKey);
        if (hidden || collapsed.contains(e.key))
            closed.insert(e.key);
        if (!hidden)
            out.append(i);
    }
    return out;
}

} // namespace keyframeitems

} // namespace kk
