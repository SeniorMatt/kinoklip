// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeOps implementation: path resolution, grouped listing and keyframe edits.
#include "core/KeyframeOps.h"

#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace kk::keyframeops {

namespace {

const QString kTransform = QStringLiteral("transform");
const QString kAudio = QStringLiteral("audio");
const QString kTitle = QStringLiteral("title");
const QString kEffect = QStringLiteral("effect");
const QString kRetime = QStringLiteral("retime");
const QString kMask = QStringLiteral("mask");

// Parses "<head>/<id>/<key>" (effect and mask paths). Returns false for malformed paths.
bool splitIdPath(const QString& path, qsizetype slash, quint64* id, QString* key)
{
    const qsizetype slash2 = path.indexOf(QLatin1Char('/'), slash + 1);
    if (slash2 < 0)
        return false;
    bool ok = false;
    *id = QStringView(path).mid(slash + 1, slash2 - slash - 1).toULongLong(&ok);
    if (!ok)
        return false;
    *key = path.mid(slash2 + 1);
    return true;
}

// Splits a path into the ParamSet it lives in and the key inside it. Returns nullptr for
// unknown paths. Template so the const and mutable lookups share one implementation.
template<typename Clip>
auto resolveSet(Clip& clip, const QString& path, QString* key) -> decltype(&clip.transform)
{
    const qsizetype slash = path.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return nullptr;
    const QStringView head = QStringView(path).left(slash);
    if (head == kMask) {
        quint64 id = 0;
        if (!splitIdPath(path, slash, &id, key) || *key == maskkeys::Shape)
            return nullptr; // the shape is not in a ParamSet (see findValue)
        for (auto& mask : clip.masks) {
            if (mask.id == id)
                return &mask.params;
        }
        return nullptr;
    }
    if (head == kEffect) {
        const qsizetype slash2 = path.indexOf(QLatin1Char('/'), slash + 1);
        if (slash2 < 0)
            return nullptr;
        bool ok = false;
        const quint64 id = QStringView(path).mid(slash + 1, slash2 - slash - 1).toULongLong(&ok);
        if (!ok)
            return nullptr;
        for (auto& fx : clip.effects) {
            if (fx.id == id) {
                *key = path.mid(slash2 + 1);
                return &fx.params;
            }
        }
        return nullptr;
    }
    *key = path.mid(slash + 1);
    if (head == kTransform)
        return &clip.transform;
    if (head == kAudio)
        return &clip.audio;
    if (head == kTitle)
        return &clip.title;
    if (head == kRetime)
        return &clip.retime;
    return nullptr;
}

// Default base value of a parameter created by a paste.
double defaultBase(const ClipData& clip, const QString& path, const QString& key)
{
    if (path.startsWith(kMask))
        return maskkeys::defaultValue(key);
    if (path.startsWith(kEffect)) {
        for (const EffectInstance& fx : clip.effects) {
            if (path.startsWith(effectPath(fx.id, QString()))) {
                if (const EffectDescriptor* d = EffectRegistry::instance().find(fx.type)) {
                    if (const ParamDescriptor* pd = d->param(key))
                        return pd->def;
                }
            }
        }
        return 0.0;
    }
    return keys::defaultValue(key);
}

// Appends the animated parameters of @p set in descriptor order (then the rest).
void appendGroup(QVector<AnimatedParam>& out, QSet<QString>& listed, const ParamSet& set,
    const QString& pathPrefix, const QString& groupId, const EffectDescriptor* desc,
    bool includeUndescribed)
{
    const QString groupName = desc ? desc->name : groupId;
    if (desc) {
        for (const ParamDescriptor& pd : desc->params) {
            if (!pd.isScalar())
                continue;
            const auto it = set.values.constFind(pd.key);
            if (it == set.values.cend() || !it->isAnimated() || listed.contains(pd.key))
                continue;
            listed.insert(pd.key);
            out.append({pathPrefix + pd.key, groupId, groupName, pd.label, pd.min, pd.max});
        }
    }
    if (!includeUndescribed)
        return;
    for (auto it = set.values.cbegin(); it != set.values.cend(); ++it) {
        if (!it->isAnimated() || listed.contains(it.key()))
            continue;
        listed.insert(it.key());
        out.append({pathPrefix + it.key(), groupId, groupName, it.key(), 0.0, 1.0});
    }
}

// Visits every AnimatedValue of a clip with its path.
template<typename F>
void forEachValue(const ClipData& clip, F&& fn)
{
    auto visit = [&](const ParamSet& set, const QString& prefix) {
        for (auto it = set.values.cbegin(); it != set.values.cend(); ++it)
            fn(prefix + it.key(), *it);
    };
    visit(clip.transform, kTransform + QLatin1Char('/'));
    visit(clip.title, kTitle + QLatin1Char('/'));
    visit(clip.audio, kAudio + QLatin1Char('/'));
    for (const MaskData& mask : clip.masks) {
        visit(mask.params, maskPath(mask.id, QString()));
        fn(maskPath(mask.id, maskkeys::Shape), mask.path.timing);
    }
    for (const EffectInstance& fx : clip.effects)
        visit(fx.params, effectPath(fx.id, QString()));
    visit(clip.retime, kRetime + QLatin1Char('/'));
}

// The animated shape addressed by a "mask/<id>/shape" path, or nullptr.
template<typename Clip>
auto resolveShape(Clip& clip, const QString& path) -> decltype(&clip.masks[0].path)
{
    if (!path.startsWith(kMask))
        return nullptr;
    const qsizetype slash = path.indexOf(QLatin1Char('/'));
    quint64 id = 0;
    QString key;
    if (slash != kMask.size() || !splitIdPath(path, slash, &id, &key) || key != maskkeys::Shape)
        return nullptr;
    for (auto& mask : clip.masks) {
        if (mask.id == id)
            return &mask.path;
    }
    return nullptr;
}

// Restores the AnimatedPath invariant of every shape in @p refs after their timing changed.
void normalizeShapes(TimelineData& data, const QVector<KeyframeRef>& refs)
{
    for (const KeyframeRef& r : refs) {
        if (!isShapePath(r.paramPath))
            continue;
        if (ClipData* clip = editops::clipById(data, r.clipId)) {
            if (AnimatedPath* p = resolveShape(*clip, r.paramPath))
                p->normalize();
        }
    }
}

} // namespace

// ---- Paths ----

QString memberPath(const QString& member, const QString& key)
{
    return member + QLatin1Char('/') + key;
}

QString effectPath(quint64 effectId, const QString& key)
{
    return kEffect + QLatin1Char('/') + QString::number(effectId) + QLatin1Char('/') + key;
}

QString maskPath(quint64 maskId, const QString& key)
{
    return kMask + QLatin1Char('/') + QString::number(maskId) + QLatin1Char('/') + key;
}

bool isShapePath(const QString& path)
{
    return path.startsWith(kMask) && path.endsWith(QLatin1Char('/') + maskkeys::Shape);
}

const MaskData* findMask(const ClipData& clip, quint64 maskId)
{
    for (const MaskData& m : clip.masks) {
        if (m.id == maskId)
            return &m;
    }
    return nullptr;
}

MaskData* findMask(ClipData& clip, quint64 maskId)
{
    for (MaskData& m : clip.masks) {
        if (m.id == maskId)
            return &m;
    }
    return nullptr;
}

const AnimatedValue* findValue(const ClipData& clip, const QString& path)
{
    if (const AnimatedPath* shape = resolveShape(clip, path))
        return &shape->timing;
    QString key;
    const ParamSet* set = resolveSet(clip, path, &key);
    if (!set)
        return nullptr;
    const auto it = set->values.constFind(key);
    return it == set->values.cend() ? nullptr : &*it;
}

AnimatedValue* findValue(ClipData& clip, const QString& path)
{
    if (AnimatedPath* shape = resolveShape(clip, path))
        return &shape->timing;
    QString key;
    ParamSet* set = resolveSet(clip, path, &key);
    if (!set)
        return nullptr;
    const auto it = set->values.find(key);
    return it == set->values.end() ? nullptr : &*it;
}

QVector<AnimatedParam> animatedParams(const ClipData& clip)
{
    QVector<AnimatedParam> out;
    const EffectRegistry& reg = EffectRegistry::instance();
    {
        // Transform and Cropping share ClipData::transform.
        QSet<QString> listed;
        const QString prefix = kTransform + QLatin1Char('/');
        appendGroup(out, listed, clip.transform, prefix, QString::fromLatin1(groups::Transform),
            reg.find(QString::fromLatin1(groups::Transform)), false);
        appendGroup(out, listed, clip.transform, prefix, QString::fromLatin1(groups::Cropping),
            reg.find(QString::fromLatin1(groups::Cropping)), true);
    }
    if (std::any_of(clip.masks.cbegin(), clip.masks.cend(),
            [](const MaskData& m) { return m.isAnimated(); })) {
        // One "Masks" group; the labels name the mask because several masks share keys.
        const QString groupId = QStringLiteral("builtin.masks");
        const EffectDescriptor* desc = reg.find(QStringLiteral("builtin.mask"));
        const QString groupName = desc ? desc->name : QStringLiteral("Masks");
        for (int i = 0; i < clip.masks.size(); ++i) {
            const MaskData& mask = clip.masks[i];
            if (!mask.isAnimated())
                continue;
            const QString name = maskDisplayName(mask, i);
            QVector<AnimatedParam> part;
            QSet<QString> listed;
            appendGroup(
                part, listed, mask.params, maskPath(mask.id, QString()), groupId, desc, true);
            if (mask.path.isAnimated()) {
                part.append({maskPath(mask.id, maskkeys::Shape), groupId, groupName,
                    QCoreApplication::translate("kk::KeyframeOps", "Shape"), 0.0,
                    double(std::max<qsizetype>(1, mask.path.timing.keys.size() - 1))});
            }
            for (AnimatedParam& p : part) {
                p.groupName = groupName;
                p.label = name + QStringLiteral(": ") + p.label;
                out.append(p);
            }
        }
    }
    if (clip.title.isAnimated()) {
        QSet<QString> listed;
        // Generator clips keep their parameters in ClipData::title too.
        const EffectDescriptor* titleDesc
            = clip.kind == ClipKind::Generator ? reg.find(clip.generator) : nullptr;
        for (const EffectDescriptor* d : reg.byCategory(EffectCategory::Title)) {
            if (titleDesc)
                break;
            if (d->storage == kTitle)
                titleDesc = d;
        }
        appendGroup(out, listed, clip.title, kTitle + QLatin1Char('/'), kTitle, titleDesc, true);
    }
    if (clip.audio.isAnimated()) {
        QSet<QString> listed;
        appendGroup(out, listed, clip.audio, kAudio + QLatin1Char('/'),
            QString::fromLatin1(groups::Audio), reg.find(QString::fromLatin1(groups::Audio)), true);
    }
    for (const EffectInstance& fx : clip.effects) {
        if (!fx.params.isAnimated())
            continue;
        QSet<QString> listed;
        const QString prefix = effectPath(fx.id, QString());
        appendGroup(out, listed, fx.params, prefix, prefix.chopped(1), reg.find(fx.type), true);
    }
    // The retime curve (speed points, core/Retime.h): source frame over clip time.
    if (const auto it = clip.retime.values.constFind(keys::RetimeFrame);
        it != clip.retime.values.cend() && it->isAnimated()) {
        double lo = 0.0, hi = 1.0;
        for (const Keyframe& k : it->keys) {
            lo = std::min(lo, k.value);
            hi = std::max(hi, k.value);
        }
        out.append({memberPath(kRetime, keys::RetimeFrame), kRetime,
            QCoreApplication::translate("kk::keyframeops", "Retime"),
            QCoreApplication::translate("kk::keyframeops", "Retime Frame"), lo, hi});
    }
    return out;
}

// ---- Navigation ----

QVector<int64_t> allKeyFrames(const ClipData& clip)
{
    QVector<int64_t> frames;
    forEachValue(clip, [&](const QString&, const AnimatedValue& v) {
        for (const Keyframe& k : v.keys)
            frames.append(k.frame);
    });
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

std::optional<int64_t> adjacentKeyFrame(const ClipData& clip, int64_t localFrame, bool forward)
{
    std::optional<int64_t> best;
    forEachValue(clip, [&](const QString&, const AnimatedValue& v) {
        const auto f = forward ? v.nextKeyFrame(localFrame) : v.previousKeyFrame(localFrame);
        if (f && (!best || (forward ? *f < *best : *f > *best)))
            best = f;
    });
    return best;
}

QVector<KeyframeRef> keysAt(const ClipData& clip, int64_t localFrame)
{
    QVector<KeyframeRef> refs;
    forEachValue(clip, [&](const QString& path, const AnimatedValue& v) {
        if (v.hasKeyAt(localFrame))
            refs.append({clip.id, path, localFrame});
    });
    return refs;
}

QVector<KeyframeRef> allKeys(const ClipData& clip)
{
    QVector<KeyframeRef> refs;
    forEachValue(clip, [&](const QString& path, const AnimatedValue& v) {
        for (const Keyframe& k : v.keys)
            refs.append({clip.id, path, k.frame});
    });
    return refs;
}

QVector<KeyframeRef> keysInRange(
    const ClipData& clip, int64_t fromFrame, int64_t toFrame, const QVector<QString>& paths)
{
    QVector<KeyframeRef> refs;
    forEachValue(clip, [&](const QString& path, const AnimatedValue& v) {
        if (!paths.isEmpty() && !paths.contains(path))
            return;
        for (const Keyframe& k : v.keys) {
            if (k.frame >= fromFrame && k.frame <= toFrame)
                refs.append({clip.id, path, k.frame});
        }
    });
    return refs;
}

// ---- Editing ----

int64_t clampMoveDelta(
    const TimelineData& data, const QVector<KeyframeRef>& refs, int64_t deltaFrames)
{
    int64_t lo = std::numeric_limits<int64_t>::min();
    int64_t hi = std::numeric_limits<int64_t>::max();
    for (const KeyframeRef& r : refs) {
        const ClipData* clip = editops::clipById(data, r.clipId);
        if (!clip)
            continue;
        // A key may stay outside the clip if it already is (trimmed edge), but never moves
        // further out.
        const int64_t first = std::min<int64_t>(0, r.frame);
        const int64_t last = std::max<int64_t>(clip->duration - 1, r.frame);
        lo = std::max(lo, first - r.frame);
        hi = std::min(hi, last - r.frame);
    }
    if (lo > hi)
        return 0;
    return std::clamp(deltaFrames, lo, hi);
}

QVector<KeyframeRef> moveKeys(
    TimelineData& data, const QVector<KeyframeRef>& refs, int64_t deltaFrames)
{
    if (deltaFrames == 0)
        return refs;
    // Group by (clip, path): all moving keys of one value are taken out first and put back at
    // their new frames afterwards, so a key moving onto another moving key's old frame never
    // overwrites it.
    QHash<std::pair<quint64, QString>, QVector<qsizetype>> groups;
    for (qsizetype i = 0; i < refs.size(); ++i)
        groups[{refs[i].clipId, refs[i].paramPath}].append(i);

    QVector<std::optional<KeyframeRef>> moved(refs.size());
    for (auto g = groups.cbegin(); g != groups.cend(); ++g) {
        ClipData* clip = editops::clipById(data, g.key().first);
        AnimatedValue* value = clip ? findValue(*clip, g.key().second) : nullptr;
        if (!value)
            continue;
        QVector<std::pair<qsizetype, Keyframe>> taken;
        for (qsizetype i : g.value()) {
            const qsizetype idx = value->indexOf(refs[i].frame);
            if (idx < 0)
                continue;
            taken.append({i, value->keys[idx]});
            value->keys.remove(idx);
        }
        for (auto& [i, key] : taken) {
            key.frame += deltaFrames;
            value->putKey(key);
            moved[i] = KeyframeRef {refs[i].clipId, refs[i].paramPath, key.frame};
        }
    }
    QVector<KeyframeRef> out;
    out.reserve(refs.size());
    for (const auto& m : moved) {
        if (m)
            out.append(*m);
    }
    normalizeShapes(data, out); // moved shape keys may have changed their order
    return out;
}

namespace {
// Calls fn(key) for every existing key of refs; returns how many were visited.
template<typename F>
int forEachKey(TimelineData& data, const QVector<KeyframeRef>& refs, F&& fn)
{
    int n = 0;
    for (const KeyframeRef& r : refs) {
        ClipData* clip = editops::clipById(data, r.clipId);
        AnimatedValue* value = clip ? findValue(*clip, r.paramPath) : nullptr;
        const qsizetype idx = value ? value->indexOf(r.frame) : -1;
        if (idx < 0)
            continue;
        fn(*value, idx);
        ++n;
    }
    return n;
}
} // namespace

int offsetKeyValues(TimelineData& data, const QVector<KeyframeRef>& refs, double deltaValue)
{
    int n = 0;
    for (const KeyframeRef& r : refs) {
        if (isShapePath(r.paramPath))
            continue; // shape key values are shape indices
        n += forEachKey(
            data, {r}, [&](AnimatedValue& v, qsizetype i) { v.keys[i].value += deltaValue; });
    }
    return n;
}

int deleteKeys(TimelineData& data, const QVector<KeyframeRef>& refs)
{
    int n = 0;
    for (const KeyframeRef& r : refs) {
        ClipData* clip = editops::clipById(data, r.clipId);
        AnimatedValue* value = clip ? findValue(*clip, r.paramPath) : nullptr;
        if (value && value->hasKeyAt(r.frame)) {
            value->removeKey(r.frame); // the last key's value becomes the constant value
            ++n;
        }
    }
    normalizeShapes(data, refs);
    return n;
}

int setInterp(TimelineData& data, const QVector<KeyframeRef>& refs, Interp interp)
{
    return forEachKey(
        data, refs, [&](AnimatedValue& v, qsizetype i) { applyInterp(v.keys[i], interp); });
}

int flattenHandles(TimelineData& data, const QVector<KeyframeRef>& refs)
{
    return forEachKey(data, refs, [](AnimatedValue& v, qsizetype i) {
        Keyframe& k = v.keys[i];
        for (KeyHandle* h : {&k.in, &k.out}) {
            if (h->mode == KeyHandle::Mode::Free)
                h->dv = 0.0;
            else
                h->mode = KeyHandle::Mode::Ease;
        }
        k.hold = false;
    });
}

bool setHandle(TimelineData& data, const KeyframeRef& ref, HandleSide side, double dtFrames,
    double dv, bool keepSmooth)
{
    ClipData* clip = editops::clipById(data, ref.clipId);
    AnimatedValue* value = clip ? findValue(*clip, ref.paramPath) : nullptr;
    const qsizetype idx = value ? value->indexOf(ref.frame) : -1;
    if (idx < 0)
        return false;
    dtFrames = std::max(dtFrames, 1e-3); // a zero-length handle has no direction
    // Geometry of the other handle before the change (resolved, so presets keep their look).
    const auto other = side == HandleSide::In ? value->outHandle(idx) : value->inHandle(idx);
    Keyframe& k = value->keys[idx];
    KeyHandle& h = side == HandleSide::In ? k.in : k.out;
    KeyHandle& o = side == HandleSide::In ? k.out : k.in;
    h = KeyHandle {KeyHandle::Mode::Free, dtFrames, dv};
    k.hold = false;
    if (keepSmooth && o.mode != KeyHandle::Mode::Linear) {
        // Same slope on both sides (both dv conventions are slope * dt), other length kept.
        const double slope = dv / dtFrames;
        const double len = other.first > 0 ? other.first : dtFrames;
        o = KeyHandle {KeyHandle::Mode::Free, len, slope * len};
    }
    return true;
}

KeyClipboard copyKeys(const TimelineData& data, const QVector<KeyframeRef>& refs)
{
    KeyClipboard board;
    if (refs.isEmpty())
        return board;
    const quint64 clipId = refs.front().clipId;
    const ClipData* clip = editops::clipById(data, clipId);
    if (!clip)
        return board;
    int64_t first = std::numeric_limits<int64_t>::max();
    for (const KeyframeRef& r : refs) {
        if (r.clipId == clipId)
            first = std::min(first, r.frame);
    }
    for (const KeyframeRef& r : refs) {
        const AnimatedValue* value = r.clipId == clipId ? findValue(*clip, r.paramPath) : nullptr;
        const qsizetype idx = value ? value->indexOf(r.frame) : -1;
        if (idx < 0)
            continue;
        KeyClipboard::Entry entry {r.paramPath, r.frame - first, value->keys[idx], {}};
        if (const AnimatedPath* shape = resolveShape(*clip, r.paramPath))
            entry.shape = shape->shapes.value(idx);
        board.entries.append(entry);
    }
    return board;
}

QVector<KeyframeRef> pasteKeys(
    TimelineData& data, quint64 clipId, const KeyClipboard& board, int64_t localFrame)
{
    QVector<KeyframeRef> out;
    ClipData* clip = editops::clipById(data, clipId);
    if (!clip)
        return out;
    for (const KeyClipboard::Entry& e : board.entries) {
        if (isShapePath(e.path)) {
            AnimatedPath* shape = resolveShape(*clip, e.path);
            if (!shape || (shape->pointCount() != e.shape.size() && shape->pointCount() != 0))
                continue; // another mask, or a different number of points
            const int64_t frame = localFrame + e.offset;
            shape->setKey(frame, e.shape);
            const qsizetype idx = shape->timing.indexOf(frame);
            if (idx >= 0) {
                // Keep the copied easing (setKey gave the new key the default preset).
                Keyframe& k = shape->timing.keys[idx];
                k.in = e.key.in;
                k.out = e.key.out;
                k.hold = e.key.hold;
            }
            out.append({clipId, e.path, frame});
            continue;
        }
        QString key;
        ParamSet* set = resolveSet(*clip, e.path, &key);
        if (!set || key.isEmpty())
            continue; // e.g. an effect instance this clip does not have
        AnimatedValue& value = set->animated(key, defaultBase(*clip, e.path, key));
        Keyframe k = e.key;
        k.frame = localFrame + e.offset;
        value.putKey(k);
        out.append({clipId, e.path, k.frame});
    }
    return out;
}

} // namespace kk::keyframeops
