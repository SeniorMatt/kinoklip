// SPDX-License-Identifier: GPL-3.0-or-later
// PluginRegistry — see PluginRegistry.h.
#include "plugins/PluginRegistry.h"

#include "core/EffectRegistry.h"
#include "core/NodeRegistry.h"
#include "plugins/ZipArchive.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>

namespace kk::plugins {

namespace {

constexpr qint64 kMaxShaderBytes = 256 * 1024;
constexpr qint64 kMaxPluginBytes = 32 * 1024 * 1024;
constexpr int kMaxPluginFiles = 1000;

const QStringList kEffectFolders = {QStringLiteral("blur"), QStringLiteral("color"),
    QStringLiteral("distort"), QStringLiteral("key"), QStringLiteral("light"),
    QStringLiteral("stylize"), QStringLiteral("time"), QStringLiteral("temporal"),
    QStringLiteral("texture"), QStringLiteral("revival"), QStringLiteral("sharpen"),
    QStringLiteral("refine"), QStringLiteral("transform")};
const QStringList kTransitionFolders = {QStringLiteral("dissolve"), QStringLiteral("iris"),
    QStringLiteral("motion"), QStringLiteral("shape"), QStringLiteral("wipe")};

QString joinProblems(const QVector<Problem>& problems)
{
    QStringList lines;
    for (const Problem& p : problems)
        lines << p.text();
    return lines.join(QLatin1Char('\n'));
}

/// The Effects Library folder id for a manifest "group" text.
QString folderFor(const QString& group, bool transition)
{
    if (group.isEmpty())
        return QString::fromLatin1(effectgroups::Plugins);
    const QString lower = group.trimmed().toLower();
    if ((transition ? kTransitionFolders : kEffectFolders).contains(lower))
        return lower;
    return group.trimmed(); // a folder of the plugin's own, shown by its text
}

ParamDescriptor toDescriptor(const PluginParam& p)
{
    ParamDescriptor d;
    d.key = p.key;
    d.label = p.label;
    d.tooltip = p.tooltip;
    d.unit = p.unit;
    switch (p.type) {
    case ParamKind::Number:
    case ParamKind::Integer:
        d.type = p.type == ParamKind::Number ? ParamType::Double : ParamType::Int;
        d.min = p.min;
        d.max = p.max;
        d.def = p.def;
        d.step = p.step;
        d.animatable = p.keyframable;
        break;
    case ParamKind::Boolean:
        d.type = ParamType::Bool;
        d.defStatic = p.boolDefault;
        d.animatable = false;
        break;
    case ParamKind::Color:
        d.type = ParamType::Color;
        d.defStatic = p.colorDefault;
        d.animatable = false;
        break;
    case ParamKind::Choice:
        d.type = ParamType::Choice;
        d.choices = p.choices;
        d.choiceLabels = p.choiceLabels;
        d.defStatic = p.stringDefault;
        d.animatable = false;
        break;
    case ParamKind::String:
        d.type = ParamType::String;
        d.defStatic = p.stringDefault;
        d.animatable = false;
        break;
    }
    return d;
}

/// True if the two descriptors show the same to the user and to project data.
bool sameDescriptor(const EffectDescriptor& a, const EffectDescriptor& b)
{
    if (a.id != b.id || a.name != b.name || a.category != b.category
        || a.description != b.description || a.group != b.group || a.plugin != b.plugin
        || a.params.size() != b.params.size())
        return false;
    for (qsizetype i = 0; i < a.params.size(); ++i) {
        const ParamDescriptor& p = a.params[i];
        const ParamDescriptor& q = b.params[i];
        if (p.key != q.key || p.label != q.label || p.type != q.type || p.min != q.min
            || p.max != q.max || p.def != q.def || p.step != q.step || p.unit != q.unit
            || p.choices != q.choices || p.choiceLabels != q.choiceLabels
            || p.defStatic != q.defStatic || p.animatable != q.animatable || p.tooltip != q.tooltip)
            return false;
    }
    return true;
}

/// Puts @p d in the registry; false when it was already there unchanged. An unchanged descriptor
/// (the author edited only the shader) is left alone, so the render thread, which reads descriptors
/// without a lock, never sees one change.
bool registerDescriptor(EffectRegistry& registry, const EffectDescriptor& d)
{
    if (const EffectDescriptor* old = registry.find(d.id); old && sameDescriptor(*old, d)) {
        if (old->unavailable)
            registry.markAvailable(d.id);
        return false;
    }
    registry.add(d);
    return true;
}

bool readTextFile(const QString& path, qint64 maxBytes, QString* text, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read the file");
        return false;
    }
    if (f.size() > maxBytes) {
        if (error)
            *error = QStringLiteral("the file is larger than %1 KiB").arg(maxBytes / 1024);
        return false;
    }
    *text = QString::fromUtf8(f.readAll());
    return true;
}

/// Copies @p from into the new folder @p to (files and folders, no links, no hidden entries).
bool copyFolder(const QString& from, const QString& to, QString* error)
{
    const QDir source(from);
    qint64 total = 0;
    int files = 0;
    QDirIterator it(
        from, QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    QDir().mkpath(to);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString relative = source.relativeFilePath(file);
        if (relative.startsWith(QLatin1Char('.')) || relative.contains(QStringLiteral("/.")))
            continue; // .git and other hidden folders do not belong in an installed plugin
        total += QFileInfo(file).size();
        if (++files > kMaxPluginFiles || total > kMaxPluginBytes) {
            if (error)
                *error = QStringLiteral("the plugin is larger than %1 files or %2 MiB")
                             .arg(kMaxPluginFiles)
                             .arg(kMaxPluginBytes / (1024 * 1024));
            return false;
        }
        const QString target = QDir(to).absoluteFilePath(relative);
        QDir().mkpath(QFileInfo(target).absolutePath());
        if (!QFile::copy(file, target)) {
            if (error)
                *error = QStringLiteral("cannot copy %1").arg(relative);
            return false;
        }
    }
    return true;
}

/// The folder that holds plugin.json: @p dir itself or its only sub-folder (a zip made of the
/// plugin folder).
QString findPluginRoot(const QString& dir)
{
    if (QFileInfo::exists(dir + QLatin1Char('/') + QLatin1String(kManifestFile)))
        return dir;
    const QStringList subs = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (subs.size() == 1
        && QFileInfo::exists(dir + QLatin1Char('/') + subs.first() + QLatin1Char('/')
            + QLatin1String(kManifestFile)))
        return dir + QLatin1Char('/') + subs.first();
    return {};
}

} // namespace

PluginRegistry& PluginRegistry::instance()
{
    static PluginRegistry registry;
    return registry;
}

QString PluginRegistry::userDir() const
{
    if (m_overridden && !m_userOverride.isEmpty())
        return m_userOverride;
    const QByteArray env = qgetenv("KK_PLUGIN_USER_DIR");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/plugins");
}

QString PluginRegistry::bundledDir() const
{
    if (m_overridden)
        return m_bundledOverride;
    const QByteArray env = qgetenv("KK_PLUGIN_BUNDLED_DIR");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    const QString app = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        app + QStringLiteral("/../share/kinoklip/plugins"), app + QStringLiteral("/plugins")};
#ifdef KK_PLUGIN_BUNDLED_SOURCE_DIR
    candidates << QStringLiteral(KK_PLUGIN_BUNDLED_SOURCE_DIR); // a build from the source tree
#endif
    for (const QString& candidate : std::as_const(candidates)) {
        if (QFileInfo(candidate).isDir())
            return QDir::cleanPath(candidate);
    }
    return {};
}

QString PluginRegistry::examplesDir()
{
    const QByteArray env = qgetenv("KK_PLUGIN_EXAMPLES_DIR");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    const QString app = QCoreApplication::applicationDirPath();
    QStringList candidates = {app + QStringLiteral("/../share/kinoklip/plugin-examples"),
        app + QStringLiteral("/plugin-examples")};
#ifdef KK_PLUGIN_EXAMPLES_SOURCE_DIR
    candidates << QStringLiteral(KK_PLUGIN_EXAMPLES_SOURCE_DIR); // a build from the source tree
#endif
    for (const QString& candidate : std::as_const(candidates)) {
        if (QFileInfo(candidate).isDir())
            return QDir::cleanPath(candidate);
    }
    return {};
}

void PluginRegistry::setDirectories(const QString& bundled, const QString& user)
{
    m_bundledOverride = bundled;
    m_userOverride = user;
    m_overridden = !bundled.isEmpty() || !user.isEmpty();
    m_loaded = false;
}

void PluginRegistry::load()
{
    if (m_loaded)
        return;
    reload();
}

void PluginRegistry::reload()
{
    m_loaded = true;
    m_plugins.clear();
    scanDirectory(bundledDir(), PluginSource::Bundled);
    scanDirectory(userDir(), PluginSource::User);
    // The same id twice: the first (bundled) one wins, the other says why it is not used.
    QStringList seen;
    for (LoadedPlugin& p : m_plugins) {
        if (!p.valid)
            continue;
        if (seen.contains(p.manifest.id)) {
            p.valid = false;
            p.problems.append({QString::fromLatin1(kManifestFile),
                QStringLiteral("another plugin with the id \"%1\" is installed").arg(p.manifest.id),
                0});
        }
        seen.append(p.manifest.id);
    }
    for (LoadedPlugin& p : m_plugins)
        p.enabled = storedEnabled(p);
    registerAll();
    emit changed();
}

void PluginRegistry::scanDirectory(const QString& dir, PluginSource source)
{
    if (dir.isEmpty())
        return;
    const QDir root(dir);
    for (const QFileInfo& info :
        root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        if (info.fileName().startsWith(QLatin1Char('.')))
            continue; // staging folders of an install in progress
        LoadedPlugin p;
        p.dir = info.absoluteFilePath();
        p.folderName = info.fileName();
        p.source = source;
        QFile manifestFile(p.dir + QLatin1Char('/') + QLatin1String(kManifestFile));
        if (!manifestFile.open(QIODevice::ReadOnly)) {
            if (!info.fileName().isEmpty() && source == PluginSource::User) {
                p.problems.append({QString::fromLatin1(kManifestFile),
                    QStringLiteral("this folder has no plugin.json"), 0});
                m_plugins.append(p);
            }
            continue;
        }
        const ParseResult parsed = parseManifest(manifestFile.readAll());
        p.problems = parsed.problems;
        if (parsed.ok) {
            p.manifest = parsed.manifest;
            checkFiles(p.dir, p.manifest, &p.problems);
        }
        p.valid = parsed.ok && p.problems.isEmpty();
        m_plugins.append(p);
    }
}

void PluginRegistry::checkFiles(
    const QString& dir, const PluginManifest& manifest, QVector<Problem>* problems)
{
    const auto check = [&](const QString& path, const QString& file, bool json, bool shader) {
        const QString full = dir + QLatin1Char('/') + file;
        QString text;
        QString why;
        if (!QFileInfo(full).isFile()) {
            problems->append({path + QStringLiteral(".file"),
                QStringLiteral("the file \"%1\" does not exist").arg(file), 0});
            return;
        }
        if (!readTextFile(full, shader ? kMaxShaderBytes : kMaxPluginBytes, &text, &why)) {
            problems->append({file, why, 0});
            return;
        }
        if (json) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
            if (doc.isNull() || !doc.isObject()) {
                int line = 1;
                for (int i = 0; i < err.offset && i < text.size(); ++i)
                    line += text[i] == QLatin1Char('\n') ? 1 : 0;
                problems->append({file,
                    doc.isNull() ? QStringLiteral("not valid JSON: %1").arg(err.errorString())
                                 : QStringLiteral("must be a JSON object { ... }"),
                    doc.isNull() ? line : 0});
            } else if (!doc.object().value(QLatin1String("commands")).isArray()) {
                problems->append({file,
                    QStringLiteral("a preset recipe needs a \"commands\" list: "
                                   "{\"commands\":[{\"command\":\"effect.add\",\"args\":{...}}]}"),
                    0});
            }
        }
    };
    for (int i = 0; i < manifest.effects.size(); ++i)
        check(QStringLiteral("effects[%1]").arg(i), manifest.effects[i].file, false, true);
    for (int i = 0; i < manifest.transitions.size(); ++i)
        check(QStringLiteral("transitions[%1]").arg(i), manifest.transitions[i].file, false, true);
    for (int i = 0; i < manifest.presets.size(); ++i)
        check(QStringLiteral("presets[%1]").arg(i), manifest.presets[i].file, true, false);
    for (int i = 0; i < manifest.scripts.size(); ++i)
        check(QStringLiteral("scripts[%1]").arg(i), manifest.scripts[i].file, false, false);
}

bool PluginRegistry::storedEnabled(const LoadedPlugin& plugin) const
{
    QSettings settings;
    const QString key = QStringLiteral("plugins/enabled/") + plugin.id();
    if (settings.contains(key))
        return settings.value(key).toBool();
    // Plugins with scripts start switched off: running someone's program takes a decision.
    return !(plugin.valid && plugin.manifest.hasScripts());
}

void PluginRegistry::registerAll()
{
    EffectRegistry& registry = EffectRegistry::instance();
    for (const QString& id : std::as_const(m_registeredPlugins))
        registry.markPluginUnavailable(id);
    QHash<QString, std::shared_ptr<const PluginShaderSpec>> specs;
    QCryptographicHash contentHash(QCryptographicHash::Sha1);
    for (const LoadedPlugin& p : std::as_const(m_plugins)) {
        if (!p.active())
            continue;
        const auto addSpec
            = [&](const QString& fullId, bool transition, const QString& file, const QString& entry,
                  const QVector<PluginParam>& params, double padding) {
                  QString text;
                  QString why;
                  if (!readTextFile(p.dir + QLatin1Char('/') + file, kMaxShaderBytes, &text, &why))
                      return false;
                  auto spec = std::make_shared<PluginShaderSpec>();
                  spec->fullId = fullId;
                  spec->pluginId = p.manifest.id;
                  spec->transition = transition;
                  spec->entry = entry;
                  spec->params = params;
                  spec->source = text;
                  spec->file = file;
                  spec->paddingPx = padding;
                  specs.insert(fullId, spec);
                  contentHash.addData(fullId.toUtf8());
                  contentHash.addData(text.toUtf8());
                  for (const PluginParam& param : params)
                      contentHash.addData(QStringLiteral("%1:%2:%3:%4")
                                              .arg(param.key)
                                              .arg(param.min)
                                              .arg(param.max)
                                              .arg(param.def)
                                              .toUtf8());
                  return true;
              };
        for (const PluginEffectDef& e : p.manifest.effects) {
            if (!addSpec(e.fullId, false, e.file, e.entry, e.params, e.paddingPx))
                continue;
            EffectDescriptor d;
            d.id = e.fullId;
            d.name = e.name;
            d.category = EffectCategory::VideoEffect;
            d.description = e.description.isEmpty() ? p.manifest.name : e.description;
            d.group = folderFor(e.group, false);
            d.plugin = p.manifest.id;
            for (const PluginParam& param : e.params)
                d.params.append(toDescriptor(param));
            const bool changed = registerDescriptor(registry, d);
            if (changed || !NodeRegistry::instance().find(NodeRegistry::effectNodeType(d.id)))
                NodeRegistry::registerEffectNode(*registry.find(d.id));
        }
        for (const PluginTransitionDef& t : p.manifest.transitions) {
            if (!addSpec(t.fullId, true, t.file, {}, t.params, 0.0))
                continue;
            EffectDescriptor d;
            d.id = t.fullId;
            d.name = t.name;
            d.category = EffectCategory::Transition;
            d.description = t.description.isEmpty() ? p.manifest.name : t.description;
            d.group = folderFor(t.group, true);
            d.plugin = p.manifest.id;
            for (const PluginParam& param : t.params)
                d.params.append(toDescriptor(param));
            registerDescriptor(registry, d);
        }
        if (!m_registeredPlugins.contains(p.manifest.id))
            m_registeredPlugins.append(p.manifest.id);
    }
    {
        QMutexLocker lock(&m_mutex);
        m_specs = std::move(specs);
        m_shaderErrors.clear();
        m_contentKey = contentHash.result().toHex();
    }
    ++m_generation;
}

const LoadedPlugin* PluginRegistry::find(const QString& pluginId) const
{
    for (const LoadedPlugin& p : m_plugins) {
        if (p.id() == pluginId)
            return &p;
    }
    return nullptr;
}

bool PluginRegistry::install(const QString& path, QString* error, QString* installedId)
{
    auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    const QFileInfo info(path);
    if (!info.exists())
        return fail(QStringLiteral("%1 does not exist").arg(path));
    QDir().mkpath(userDir());
    // Staging folder next to the plugins (same file system, so the final step is a rename); the
    // leading dot keeps it out of the scan.
    QTemporaryDir staging(userDir() + QStringLiteral("/.staging-XXXXXX"));
    if (!staging.isValid())
        return fail(QStringLiteral("cannot write to %1").arg(userDir()));
    QString source;
    if (info.isDir()) {
        source = info.absoluteFilePath();
    } else {
        const QString unpacked = staging.path() + QStringLiteral("/unpacked");
        QDir().mkpath(unpacked);
        QString why;
        if (!zip::extractAll(info.absoluteFilePath(), unpacked, &why))
            return fail(
                QStringLiteral("%1 is not a usable .kkplugin file: %2").arg(info.fileName(), why));
        source = findPluginRoot(unpacked);
        if (source.isEmpty())
            return fail(
                QStringLiteral("%1 has no plugin.json at its top level").arg(info.fileName()));
    }
    QFile manifestFile(source + QLatin1Char('/') + QLatin1String(kManifestFile));
    if (!manifestFile.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("%1 has no plugin.json").arg(source));
    const ParseResult parsed = parseManifest(manifestFile.readAll());
    if (!parsed.ok)
        return fail(QStringLiteral("plugin.json has problems:\n") + joinProblems(parsed.problems));
    QVector<Problem> problems;
    checkFiles(source, parsed.manifest, &problems);
    if (!problems.isEmpty())
        return fail(QStringLiteral("the plugin has problems:\n") + joinProblems(problems));
    const QString id = parsed.manifest.id;
    if (const LoadedPlugin* existing = find(id);
        existing && existing->source == PluginSource::Bundled)
        return fail(QStringLiteral("a bundled plugin with the id \"%1\" exists").arg(id));
    const QString target = userDir() + QLatin1Char('/') + id;
    if (QFileInfo(target).canonicalFilePath() == QFileInfo(source).canonicalFilePath()
        && QFileInfo(target).exists()) {
        reload(); // installed in place (a plugin being developed in the plugins folder)
        if (installedId)
            *installedId = id;
        return true;
    }
    QString why;
    const QString fresh = staging.path() + QStringLiteral("/new");
    if (!copyFolder(source, fresh, &why))
        return fail(why);
    const QString old = staging.path() + QStringLiteral("/old");
    const bool replacing = QFileInfo::exists(target);
    if (replacing && !QDir().rename(target, old))
        return fail(QStringLiteral("cannot replace %1").arg(target));
    if (!QDir().rename(fresh, target)) {
        if (replacing)
            QDir().rename(old, target); // put the old one back
        return fail(QStringLiteral("cannot install into %1").arg(target));
    }
    reload();
    if (installedId)
        *installedId = id;
    return true;
}

bool PluginRegistry::uninstall(const QString& pluginId, QString* error)
{
    const LoadedPlugin* p = find(pluginId);
    if (!p) {
        if (error)
            *error = QStringLiteral("no plugin \"%1\" is installed").arg(pluginId);
        return false;
    }
    if (p->source != PluginSource::User) {
        if (error)
            *error = QStringLiteral("\"%1\" is a bundled plugin: it can be disabled, not removed")
                         .arg(pluginId);
        return false;
    }
    const QString dir = p->dir;
    if (!QDir(dir).removeRecursively()) {
        if (error)
            *error = QStringLiteral("cannot delete %1").arg(dir);
        return false;
    }
    QSettings().remove(QStringLiteral("plugins/enabled/") + pluginId);
    reload();
    return true;
}

bool PluginRegistry::setEnabled(const QString& pluginId, bool enabled)
{
    LoadedPlugin* target = nullptr;
    for (LoadedPlugin& p : m_plugins) {
        if (p.id() == pluginId)
            target = &p;
    }
    if (!target)
        return false;
    QSettings().setValue(QStringLiteral("plugins/enabled/") + pluginId, enabled);
    if (target->enabled == enabled)
        return true;
    target->enabled = enabled;
    registerAll();
    emit changed();
    return true;
}

bool PluginRegistry::exportPackage(
    const QString& pluginId, const QString& zipPath, QString* error) const
{
    const LoadedPlugin* p = find(pluginId);
    if (!p) {
        if (error)
            *error = QStringLiteral("no plugin \"%1\" is installed").arg(pluginId);
        return false;
    }
    return zip::createFromFolder(p->dir, zipPath, error);
}

std::shared_ptr<const PluginShaderSpec> PluginRegistry::shaderSpec(const QString& fullId) const
{
    QMutexLocker lock(&m_mutex);
    return m_specs.value(fullId);
}

void PluginRegistry::reportShaderError(const QString& fullId, const QString& message)
{
    QMutexLocker lock(&m_mutex);
    if (message.isEmpty())
        m_shaderErrors.remove(fullId);
    else
        m_shaderErrors.insert(fullId, message);
}

QByteArray PluginRegistry::contentKey() const
{
    QMutexLocker lock(&m_mutex);
    return m_contentKey;
}

QString PluginRegistry::shaderError(const QString& fullId) const
{
    QMutexLocker lock(&m_mutex);
    return m_shaderErrors.value(fullId);
}

QVector<PresetEntry> PluginRegistry::presets() const
{
    QVector<PresetEntry> out;
    for (const LoadedPlugin& p : m_plugins) {
        if (!p.active())
            continue;
        for (const PluginPresetDef& d : p.manifest.presets)
            out.append({d, p.manifest.id, p.manifest.name});
    }
    return out;
}

QVector<ScriptEntry> PluginRegistry::scripts() const
{
    QVector<ScriptEntry> out;
    for (const LoadedPlugin& p : m_plugins) {
        if (!p.active())
            continue;
        for (const PluginScriptDef& d : p.manifest.scripts)
            out.append({d, p.manifest.id, p.manifest.name, p.dir});
    }
    return out;
}

const PluginPresetDef* PluginRegistry::findPreset(const QString& fullId) const
{
    for (const LoadedPlugin& p : m_plugins) {
        if (!p.active())
            continue;
        for (const PluginPresetDef& d : p.manifest.presets) {
            if (d.fullId == fullId)
                return &d;
        }
    }
    return nullptr;
}

QJsonObject PluginRegistry::presetRecipe(const QString& fullId, QString* error) const
{
    for (const LoadedPlugin& p : m_plugins) {
        if (!p.active())
            continue;
        for (const PluginPresetDef& d : p.manifest.presets) {
            if (d.fullId != fullId)
                continue;
            QFile f(p.dir + QLatin1Char('/') + d.file);
            if (!f.open(QIODevice::ReadOnly)) {
                if (error)
                    *error = QStringLiteral("cannot read %1").arg(d.file);
                return {};
            }
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (!doc.isObject()) {
                if (error)
                    *error = QStringLiteral("%1 is not a JSON object").arg(d.file);
                return {};
            }
            return doc.object();
        }
    }
    if (error)
        *error = QStringLiteral("no preset \"%1\" in an enabled plugin").arg(fullId);
    return {};
}

} // namespace kk::plugins
