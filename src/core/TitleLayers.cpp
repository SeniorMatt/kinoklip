// SPDX-License-Identifier: GPL-3.0-or-later
// TitleLayers — see TitleLayers.h.
#include "core/TitleLayers.h"

#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"
#include "core/TitlePresets.h"

#include <QCoreApplication>

#include <algorithm>

namespace kk::titlelayers {

namespace {

qsizetype indexOfLayer(const ClipData& clip, quint64 id)
{
    for (qsizetype i = 0; i < clip.titleLayers.size(); ++i) {
        if (clip.titleLayers[i].id == id)
            return i;
    }
    return -1;
}

/// The base layer's slot, clamped into the stack (0 = below every extra layer).
int baseSlot(const ClipData& clip)
{
    return std::clamp(clip.titleBase.slot, 0, int(clip.titleLayers.size()));
}

/// Rebuilds titleLayers and titleBase from a bottom-to-top id list.
void applyOrder(ClipData& clip, const QVector<quint64>& ids)
{
    QVector<TitleLayer> sorted;
    sorted.reserve(clip.titleLayers.size());
    int slot = 0;
    for (const quint64 id : ids) {
        if (id == kBaseId) {
            slot = int(sorted.size());
            continue;
        }
        const qsizetype i = indexOfLayer(clip, id);
        if (i >= 0)
            sorted.append(clip.titleLayers[i]);
    }
    clip.titleLayers = sorted;
    clip.titleBase.slot = slot;
}

quint64 nextId(const ClipData& clip)
{
    quint64 id = 0;
    for (const TitleLayer& l : clip.titleLayers)
        id = std::max(id, l.id);
    return id + 1;
}

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::TitleLayers", text);
}

} // namespace

bool isPlain(const ClipData& clip)
{
    return clip.titleLayers.isEmpty() && clip.titleBase.visible;
}

QVector<quint64> order(const ClipData& clip)
{
    QVector<quint64> ids;
    ids.reserve(clip.titleLayers.size() + 1);
    const int slot = baseSlot(clip);
    for (int i = 0; i <= clip.titleLayers.size(); ++i) {
        if (i == slot)
            ids.append(kBaseId);
        if (i < clip.titleLayers.size())
            ids.append(clip.titleLayers[i].id);
    }
    return ids;
}

bool exists(const ClipData& clip, quint64 id)
{
    return id == kBaseId || indexOfLayer(clip, id) >= 0;
}

TitleLayerKind kind(const ClipData& clip, quint64 id)
{
    const qsizetype i = indexOfLayer(clip, id);
    return i >= 0 ? clip.titleLayers[i].kind : TitleLayerKind::Text;
}

bool visible(const ClipData& clip, quint64 id)
{
    const qsizetype i = indexOfLayer(clip, id);
    return i >= 0 ? clip.titleLayers[i].visible : clip.titleBase.visible;
}

const ParamSet* params(const ClipData& clip, quint64 id)
{
    if (id == kBaseId)
        return &clip.title;
    const qsizetype i = indexOfLayer(clip, id);
    return i >= 0 ? &clip.titleLayers[i].params : nullptr;
}

ParamSet* params(ClipData& clip, quint64 id)
{
    if (id == kBaseId)
        return &clip.title;
    const qsizetype i = indexOfLayer(clip, id);
    return i >= 0 ? &clip.titleLayers[i].params : nullptr;
}

QString displayName(const ClipData& clip, quint64 id)
{
    const qsizetype i = indexOfLayer(clip, id);
    const QString own = i >= 0 ? clip.titleLayers[i].name : clip.titleBase.name;
    if (!own.isEmpty())
        return own;
    const ParamSet& p = i >= 0 ? clip.titleLayers[i].params : clip.title;
    if (i >= 0 && clip.titleLayers[i].kind == TitleLayerKind::Shape) {
        const QString shape = p.staticValue(keys::ShapeType).toString();
        if (shape == QLatin1String("rounded"))
            return tr("Rounded Rectangle");
        if (shape == QLatin1String("ellipse"))
            return tr("Ellipse");
        if (shape == QLatin1String("line"))
            return tr("Line");
        return tr("Rectangle");
    }
    const QString text = p.staticValue(keys::Text).toString().section(QLatin1Char('\n'), 0, 0);
    return text.isEmpty() ? tr("Text") : text;
}

quint64 addLayer(ClipData& clip, TitleLayerKind kind, const QString& shape)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    TitleLayer layer;
    layer.id = nextId(clip);
    layer.kind = kind;
    if (kind == TitleLayerKind::Shape) {
        layer.params = registry.defaultParams(QLatin1String(kShapeDescriptorId));
        if (!shape.isEmpty())
            layer.params.setStatic(keys::ShapeType, shape);
    } else {
        layer.params = registry.defaultParams(titles::Text);
        layer.params.setStatic(keys::Text, tr("Text"));
    }
    // Added on top: after every layer, so a base layer below keeps its slot.
    clip.titleLayers.append(layer);
    return layer.id;
}

bool removeLayer(ClipData& clip, quint64 id)
{
    const qsizetype i = indexOfLayer(clip, id);
    if (id == kBaseId || i < 0)
        return false;
    QVector<quint64> ids = order(clip);
    ids.removeOne(id);
    clip.titleLayers.removeAt(i);
    applyOrder(clip, ids);
    return true;
}

quint64 duplicateLayer(ClipData& clip, quint64 id)
{
    TitleLayer copy;
    if (id == kBaseId) {
        copy.kind = TitleLayerKind::Text;
        copy.params = clip.title;
        copy.name = clip.titleBase.name;
        copy.visible = clip.titleBase.visible;
    } else {
        const qsizetype i = indexOfLayer(clip, id);
        if (i < 0)
            return 0;
        copy = clip.titleLayers[i];
    }
    copy.id = nextId(clip);
    QVector<quint64> ids = order(clip);
    const qsizetype at = ids.indexOf(id);
    clip.titleLayers.append(copy);
    ids.insert(at + 1, copy.id);
    applyOrder(clip, ids);
    return copy.id;
}

bool moveLayer(ClipData& clip, quint64 id, int delta)
{
    QVector<quint64> ids = order(clip);
    const qsizetype from = ids.indexOf(id);
    if (from < 0)
        return false;
    const qsizetype to = std::clamp<qsizetype>(from + delta, 0, ids.size() - 1);
    if (to == from)
        return false;
    ids.move(from, to);
    applyOrder(clip, ids);
    return true;
}

bool setVisible(ClipData& clip, quint64 id, bool visible)
{
    if (id == kBaseId) {
        clip.titleBase.visible = visible;
        return true;
    }
    const qsizetype i = indexOfLayer(clip, id);
    if (i < 0)
        return false;
    clip.titleLayers[i].visible = visible;
    return true;
}

bool rename(ClipData& clip, quint64 id, const QString& name)
{
    if (id == kBaseId) {
        clip.titleBase.name = name;
        return true;
    }
    const qsizetype i = indexOfLayer(clip, id);
    if (i < 0)
        return false;
    clip.titleLayers[i].name = name;
    return true;
}

void shiftKeys(ClipData& clip, int64_t deltaFrames)
{
    for (TitleLayer& l : clip.titleLayers)
        l.params.shiftKeys(deltaFrames);
}

} // namespace kk::titlelayers
