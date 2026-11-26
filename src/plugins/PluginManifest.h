// SPDX-License-Identifier: GPL-3.0-or-later
// PluginManifest — the plugin.json of a Kinoklip plugin: the typed form, the parser and the
// validation messages (docs/plugins/writing-plugins.md is the reference of every field).
#pragma once

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace kk::plugins {

/// The plugin API version this build implements (manifest field "api"). A plugin that asks for
/// a higher one is refused with a clear message; lower ones keep working.
inline constexpr int kApiVersion = 1;

/// Name of the manifest file inside a plugin folder.
inline constexpr char kManifestFile[] = "plugin.json";
/// File name suffix of a packed plugin (a zip file of the plugin folder).
inline constexpr char kPackageSuffix[] = ".kkplugin";
/// Prefix of the descriptor id of everything a plugin adds: "plugin:<plugin id>/<item id>".
inline constexpr char kIdPrefix[] = "plugin:";

enum class ParamKind {
    Number,
    Integer,
    Boolean,
    Color,
    Choice,
    String
};

/// One parameter of an effect or transition. Number and Integer are keyframable (unless
/// `keyframable` is false); the other kinds are static values.
struct PluginParam
{
    QString key; ///< identifier, also the shader uniform name `u_<key>`
    QString label;
    QString tooltip;
    QString unit;
    ParamKind type = ParamKind::Number;
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;
    double step = 0.01;
    bool keyframable = true;
    bool boolDefault = false;
    QColor colorDefault = Qt::white;
    QStringList choices; ///< stored values; the shader gets the index
    QStringList choiceLabels; ///< parallel to choices
    QString stringDefault; ///< String default, Choice default value
};

struct PluginEffectDef
{
    QString id; ///< item id inside the plugin
    QString name;
    QString description;
    QString group; ///< Effects Library folder: a built-in folder id or any text
    QString file; ///< GLSL file, relative to the plugin folder
    QString entry; ///< "main" (vec4 effect(vec2 uv)) or "color" (vec3 apply(vec3 c))
    QString portedFrom; ///< id of the built-in effect this one is a port of (templates)
    double paddingPx = 0.0; ///< transparent margin the effect needs around the picture
    QVector<PluginParam> params;
    /// "plugin:<plugin id>/<item id>"; filled by the parser.
    QString fullId;
};

struct PluginTransitionDef
{
    QString id;
    QString name;
    QString description;
    QString group;
    QString file;
    QString portedFrom;
    QVector<PluginParam> params;
    QString fullId;
};

/// Where a preset (command recipe) applies.
enum class PresetTarget {
    Clip, ///< once per selected clip ($clip, $start, $end, $duration)
    Playhead, ///< once at the playhead ($playhead)
    EditPoint ///< once at an edit point: the cut between two clips ($left, $right, $edit)
};

struct PluginPresetDef
{
    QString id;
    QString name;
    QString description;
    QString group;
    QString file; ///< JSON recipe, relative to the plugin folder
    PresetTarget target = PresetTarget::Clip;
    QString fullId; ///< "plugin:<plugin id>/<item id>"
};

struct PluginScriptDef
{
    QString id;
    QString name;
    QString description;
    QString file;
    QString interpreter; ///< "python3"
    QStringList permissions;
    QString fullId;
};

/// One problem found in a manifest or a plugin file.
struct Problem
{
    QString where; ///< path inside the manifest ("effects[0].params[1].min") or the file name
    QString message;
    int line = 0; ///< line in `where` (a file) when known, else 0
    bool warning = false; ///< advice (it may still work) rather than an error
    [[nodiscard]] QString text() const;
};

struct PluginManifest
{
    QString id;
    QString name;
    QString version;
    QString author;
    QString license;
    QString description;
    QString homepage;
    int api = kApiVersion;
    /// What the plugin may do beyond its pictures; scripts need explicit permissions.
    QStringList permissions;
    QVector<PluginEffectDef> effects;
    QVector<PluginTransitionDef> transitions;
    QVector<PluginPresetDef> presets;
    QVector<PluginScriptDef> scripts;
    QJsonObject raw; ///< the manifest as read

    /// "effect", "transition", "preset", "script" for the kinds present.
    [[nodiscard]] QStringList kinds() const;
    /// Every file the manifest names (shaders, recipes, scripts), relative paths.
    [[nodiscard]] QStringList files() const;
    /// True if the plugin has anything that runs code on the computer (scripts).
    [[nodiscard]] bool hasScripts() const { return !scripts.isEmpty(); }
};

struct ParseResult
{
    bool ok = false;
    PluginManifest manifest;
    QVector<Problem> problems;
};

/// Parses and validates the text of a plugin.json. `ok` is false when anything is wrong;
/// `problems` then lists every problem found (not just the first), each with its path.
/// JSON syntax errors carry the line.
[[nodiscard]] ParseResult parseManifest(const QByteArray& json);

/// True for a plugin id: lower case letters, digits, '.', '-', '_' (2 to 64 characters, starts
/// with a letter or digit), e.g. "com.example.tint-pack".
[[nodiscard]] bool isValidPluginId(const QString& id);
/// True for an item id (effect, transition, preset, script): lower case letters, digits and '_'.
[[nodiscard]] bool isValidItemId(const QString& id);
/// True for a parameter key: starts with a letter, then letters and digits (a GLSL-safe name).
[[nodiscard]] bool isValidParamKey(const QString& key);
/// "plugin:<plugin id>/<item id>".
[[nodiscard]] QString fullId(const QString& pluginId, const QString& itemId);
/// Splits a full id; false when @p id is not a plugin id.
[[nodiscard]] bool splitFullId(const QString& id, QString* pluginId, QString* itemId);
/// Relative path that stays inside the plugin folder (no "..", no absolute path, no backslash).
[[nodiscard]] bool isSafeRelativePath(const QString& path);

/// The permissions a script may declare.
[[nodiscard]] QStringList knownPermissions();
/// A one-line human description of permission @p permission (English; the dialog translates).
[[nodiscard]] QString permissionText(const QString& permission);

} // namespace kk::plugins
