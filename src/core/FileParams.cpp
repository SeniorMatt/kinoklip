// SPDX-License-Identifier: GPL-3.0-or-later
// FileParams implementation: walks every clip effect's File parameters (by descriptor).
#include "core/FileParams.h"

#include "core/EffectRegistry.h"

#include <QFileInfo>
#include <QVariantMap>

namespace kk::fileparams {

namespace {

/// Calls @p fn(value) for every File parameter static of every effect in @p timeline.
template<typename Timeline, typename Fn>
void forEachFileParam(Timeline& timeline, Fn&& fn)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    for (auto* tracks : {&timeline.videoTracks, &timeline.audioTracks}) {
        for (auto& track : *tracks) {
            for (auto& clip : track.clips) {
                for (auto& effect : clip.effects) {
                    const EffectDescriptor* d = registry.find(effect.type);
                    if (!d)
                        continue;
                    for (const ParamDescriptor& p : d->params) {
                        if (p.type != ParamType::File)
                            continue;
                        auto it = effect.params.statics.find(p.key);
                        if (it != effect.params.statics.end())
                            fn(*it);
                    }
                }
            }
        }
    }
}

} // namespace

bool hasFileParams(const TimelineData& timeline)
{
    bool any = false;
    forEachFileParam(timeline, [&any](const QVariant& v) {
        any = any || v.typeId() == QMetaType::QVariantMap || !v.toString().isEmpty();
    });
    return any;
}

void encodeForSave(TimelineData& timeline, const QDir& projectDir)
{
    forEachFileParam(timeline, [&projectDir](QVariant& v) {
        if (v.typeId() == QMetaType::QVariantMap)
            return; // already encoded
        const QString path = v.toString();
        if (path.isEmpty())
            return;
        const QString absolute
            = QFileInfo(path).isAbsolute() ? path : projectDir.absoluteFilePath(path);
        QVariantMap stored;
        stored.insert(QLatin1String(kFileKey), absolute);
        stored.insert(QLatin1String(kRelativeKey), projectDir.relativeFilePath(absolute));
        v = stored;
    });
}

void decodeAfterLoad(TimelineData& timeline, const QDir& projectDir)
{
    forEachFileParam(timeline, [&projectDir](QVariant& v) {
        QString absolute, relative;
        if (v.typeId() == QMetaType::QVariantMap) {
            const QVariantMap stored = v.toMap();
            absolute = stored.value(QLatin1String(kFileKey)).toString();
            relative = stored.value(QLatin1String(kRelativeKey)).toString();
        } else {
            absolute = v.toString();
            if (absolute.isEmpty())
                return;
            if (!QFileInfo(absolute).isAbsolute()) {
                relative = absolute;
                absolute = projectDir.absoluteFilePath(relative);
            }
        }
        if (!absolute.isEmpty() && QFileInfo::exists(absolute)) {
            v = absolute;
            return;
        }
        if (!relative.isEmpty()) {
            const QString candidate = QDir::cleanPath(projectDir.absoluteFilePath(relative));
            if (QFileInfo::exists(candidate)) {
                v = candidate;
                return;
            }
        }
        v = absolute.isEmpty() ? QDir::cleanPath(projectDir.absoluteFilePath(relative)) : absolute;
    });
}

} // namespace kk::fileparams
