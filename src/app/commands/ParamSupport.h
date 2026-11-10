// SPDX-License-Identifier: GPL-3.0-or-later
// ParamSupport — reading and checking parameter values for the commands that set effect,
// transition, title and generator parameters by key, with the ranges of the descriptor (the same
// limits the Inspector enforces), and writing them through the Inspector's ParamAccess.
#pragma once

#include "app/commands/Command.h"
#include "core/EffectRegistry.h"
#include "ui/inspector/ParamAccess.h"

#include <QStringList>
#include <QVariant>
#include <QVector>

namespace kk::commands {

/// One checked "key=value" setting: the descriptor entry it belongs to and its parsed value.
struct ParamSetting
{
    const ParamDescriptor* descriptor = nullptr;
    double number = 0.0; ///< Double / Int parameters
    QVariant value; ///< the other types (bool, QString, QColor)
};

/// Checks one value @p text for parameter @p descriptor: a number inside [min, max] (an Int must
/// be whole), a boolean (true / false / yes / no / 1 / 0), one of the choices, a colour
/// (#rrggbb or a colour name), a text, an existing file. Failure codes: `bad_value`,
/// `out_of_range`, `file_not_found`.
[[nodiscard]] std::optional<CommandResult> checkParamValue(
    const ParamDescriptor& descriptor, const QString& text, ParamSetting& out);

/// Splits and checks @p settings ("key=value", the first '=' separates) against @p descriptor.
/// Failure: `bad_value` for a malformed entry, `unknown_parameter` for a key the descriptor
/// does not have (the message lists the keys), then checkParamValue()'s codes.
[[nodiscard]] std::optional<CommandResult> checkSettings(
    const EffectDescriptor& descriptor, const QStringList& settings, QVector<ParamSetting>& out);

/// Writes checked @p settings into the parameter set that @p base addresses (scope, clip and
/// effect id; the key is taken from each setting): constants through ParamAccess::setScalar()
/// at clip-local @p localFrame (a keyframed parameter gets a key there), the rest as statics.
void applySettings(ClipData& clip, const ParamRef& base,
    const QVector<ParamSetting>& settings, int64_t localFrame = 0);

/// "key=value" entries of the `key` + `value` pair and the `set` list of @p args (what effect.set,
/// transition.set, title.set and generator.add accept).
[[nodiscard]] QStringList settingsOf(const CommandArgs& args);

/// The parameters of a clip by key: where one lives and its descriptor.
struct ResolvedParam
{
    ParamRef ref;
    const ParamDescriptor* descriptor = nullptr;
    const EffectDescriptor* owner = nullptr;
};

/// Finds parameter @p key of @p clip: of effect instance @p effectId when it is not 0, else of
/// the clip's own groups (Transform, Cropping, Composite, Audio) or, for a title or generator
/// clip, of its title / generator descriptor. Failure: `unknown_effect` (no such instance on the
/// clip) or `unknown_parameter`.
[[nodiscard]] std::optional<CommandResult> resolveParam(
    const ClipData& clip, quint64 effectId, const QString& key, ResolvedParam& out);

/// The value of the parameter as JSON at clip-local @p localFrame: a number, bool or text.
[[nodiscard]] QJsonValue paramValueJson(const ClipData& clip, const ParamRef& ref,
    const ParamDescriptor& descriptor, int64_t localFrame);

/// The `set` list parameter shared by the commands.
[[nodiscard]] ParamSpec setListParam(const char* what);

} // namespace kk::commands
