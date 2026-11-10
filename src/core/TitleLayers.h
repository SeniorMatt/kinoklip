// SPDX-License-Identifier: GPL-3.0-or-later
// TitleLayers — the ordered layers of a Title clip (spec section 68). A title always had one
// text; it is now the first layer, kept in ClipData::title so every older project, preset,
// keyframe path and command still means that layer. More Text and Shape layers live in
// ClipData::titleLayers; ClipData::titleBase says where the first layer sits in the stack and
// whether it is shown. Layers are drawn bottom to top.
#pragma once

#include "core/TimelineData.h"

#include <QString>
#include <QVector>

namespace kk::titlelayers {

/// Layer id of the first layer (the one stored in ClipData::title).
inline constexpr quint64 kBaseId = 0;

/// True when @p clip is the plain old title: one shown text layer, nothing else. Such a title
/// renders exactly as before layers existed.
[[nodiscard]] bool isPlain(const ClipData& clip);

/// Layer ids from the bottom to the top (kBaseId for the first layer).
[[nodiscard]] QVector<quint64> order(const ClipData& clip);

[[nodiscard]] bool exists(const ClipData& clip, quint64 id);
[[nodiscard]] TitleLayerKind kind(const ClipData& clip, quint64 id);
[[nodiscard]] bool visible(const ClipData& clip, quint64 id);
/// The parameters of layer @p id (nullptr when there is no such layer).
[[nodiscard]] const ParamSet* params(const ClipData& clip, quint64 id);
[[nodiscard]] ParamSet* params(ClipData& clip, quint64 id);
/// The name shown in the layer list: the user's name, else the first line of a text, else the
/// shape's type.
[[nodiscard]] QString displayName(const ClipData& clip, quint64 id);

/// Descriptor id of the parameters of a Shape layer (EffectRegistry, hidden category).
inline constexpr char kShapeDescriptorId[] = "builtin.title_shape";

/// Adds a layer on top and returns its id. A Text layer starts from the Text title's defaults
/// with @p text; a Shape layer from the shape defaults (@p shape: the keys::ShapeType value).
quint64 addLayer(ClipData& clip, TitleLayerKind kind, const QString& shape = {});
/// Removes layer @p id; false for the first layer (hide it instead) and for unknown ids.
bool removeLayer(ClipData& clip, quint64 id);
/// Copies layer @p id directly above itself; returns the new id, 0 when there is no such layer.
quint64 duplicateLayer(ClipData& clip, quint64 id);
/// Moves layer @p id up (+1) or down (-1) by @p delta places; false when it did not move.
bool moveLayer(ClipData& clip, quint64 id, int delta);
bool setVisible(ClipData& clip, quint64 id, bool visible);
bool rename(ClipData& clip, quint64 id, const QString& name);

/// Shifts the keys of every extra layer (head trim, split); the first layer is clip.title.
void shiftKeys(ClipData& clip, int64_t deltaFrames);

} // namespace kk::titlelayers
