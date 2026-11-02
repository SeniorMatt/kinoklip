// SPDX-License-Identifier: GPL-3.0-or-later
// ExportPresets — preset loading and user preset storage.
#include "export/ExportPresets.h"

#include "export/ExportLog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

// Global scope: Q_INIT_RESOURCE declares an extern function (see ui/common/Resources.cpp).
static void initExportPresetResources()
{
    Q_INIT_RESOURCE(exportpresets);
}

namespace kk {

namespace {

constexpr int kFormatVersion = 1; ///< "version" of presets.json / export_presets.json
const QLatin1String kUserPrefix("user:");

/// Settings without the job-specific parts (output file and range).
ExportSettings stripJobFields(ExportSettings s)
{
    s.outputPath.clear();
    s.range = RangeMode::Entire;
    s.rangeIn = 0;
    s.rangeOut = 0;
    return s;
}

QList<ExportPreset> parse(const QByteArray& data, bool builtin, const QString& source)
{
    QList<ExportPreset> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (doc.isNull()) {
        qCWarning(lcExport).noquote() << "cannot parse" << source << ":" << err.errorString();
        return out;
    }
    const QJsonArray presets = doc.object().value(QLatin1String("presets")).toArray();
    for (const QJsonValue& value : presets) {
        const QJsonObject o = value.toObject();
        ExportPreset p;
        p.id = o.value(QLatin1String("id")).toString();
        p.name = o.value(QLatin1String("name")).toString();
        p.category = o.value(QLatin1String("category"))
                         .toString(builtin ? QString() : QStringLiteral("User"));
        p.description = o.value(QLatin1String("description")).toString();
        p.builtin = builtin;
        p.settings = ExportSettings::fromJson(
            o.value(QLatin1String("settings")).toObject(), ExportSettings {});
        p.settings.normalize();
        p.settings = stripJobFields(p.settings);
        if (p.id.isEmpty() || p.name.isEmpty() || !p.settings.containerInfo()) {
            qCWarning(lcExport).noquote() << "skipping invalid preset" << p.id << "in" << source;
            continue;
        }
        if (builtin) {
            // Built-in names are UI strings: translatable through the "kk::ExportPresets" context.
            p.name = QCoreApplication::translate("kk::ExportPresets", p.name.toUtf8().constData());
        }
        out.push_back(p);
    }
    return out;
}

} // namespace

const QList<ExportPreset>& ExportPresets::builtinPresets()
{
    static const QList<ExportPreset> presets = [] {
        initExportPresetResources();
        QFile file(QStringLiteral(":/export/presets.json"));
        if (!file.open(QIODevice::ReadOnly)) {
            qCWarning(lcExport) << "built-in presets resource missing";
            return QList<ExportPreset> {};
        }
        return parse(file.readAll(), true, file.fileName());
    }();
    return presets;
}

ExportPresets::ExportPresets(const QString& userFile)
    : m_userFile(userFile)
{
    if (m_userFile.isEmpty()) {
        m_userFile = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/export_presets.json");
    }
    loadUser();
}

void ExportPresets::loadUser()
{
    m_presets = builtinPresets();
    QFile file(m_userFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return; // no user presets yet
    }
    QList<ExportPreset> user = parse(file.readAll(), false, m_userFile);
    std::sort(user.begin(), user.end(), [](const ExportPreset& a, const ExportPreset& b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    for (ExportPreset& p : user) {
        p.id = kUserPrefix + p.name;
        p.builtin = false;
        p.category = QStringLiteral("User");
    }
    m_presets += user;
}

const ExportPreset* ExportPresets::find(const QString& id) const
{
    for (const ExportPreset& p : m_presets) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

bool ExportPresets::apply(const QString& id, ExportSettings& base) const
{
    const ExportPreset* p = find(id);
    if (!p) {
        return false;
    }
    ExportSettings s = p->settings;
    s.outputPath = base.outputPath;
    s.range = base.range;
    s.rangeIn = base.rangeIn;
    s.rangeOut = base.rangeOut;
    s.normalize(); // fixes the output extension for the preset's container
    base = s;
    return true;
}

QString ExportPresets::saveUserPreset(
    const QString& name, const ExportSettings& settings, QString* error)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        if (error) {
            *error = QCoreApplication::translate("kk::ExportPresets", "The preset needs a name.");
        }
        return {};
    }
    const QString id = kUserPrefix + trimmed;
    const QList<ExportPreset> before = m_presets;
    m_presets.erase(std::remove_if(m_presets.begin(), m_presets.end(),
                        [&](const ExportPreset& p) { return p.id == id; }),
        m_presets.end());
    ExportPreset p;
    p.id = id;
    p.name = trimmed;
    p.category = QStringLiteral("User");
    p.builtin = false;
    p.settings = stripJobFields(settings);
    m_presets.push_back(p);
    if (!writeUser(error)) {
        m_presets = before;
        return {};
    }
    loadUser(); // re-sort
    return id;
}

bool ExportPresets::removeUserPreset(const QString& id, QString* error)
{
    const ExportPreset* p = find(id);
    if (!p || p->builtin) {
        if (error) {
            *error = QCoreApplication::translate(
                "kk::ExportPresets", "Only user presets can be deleted.");
        }
        return false;
    }
    const QList<ExportPreset> before = m_presets;
    m_presets.erase(std::remove_if(m_presets.begin(), m_presets.end(),
                        [&](const ExportPreset& x) { return x.id == id; }),
        m_presets.end());
    if (!writeUser(error)) {
        m_presets = before;
        return false;
    }
    return true;
}

bool ExportPresets::writeUser(QString* error) const
{
    QJsonArray list;
    for (const ExportPreset& p : m_presets) {
        if (p.builtin) {
            continue;
        }
        QJsonObject o;
        o[QStringLiteral("id")] = p.id;
        o[QStringLiteral("name")] = p.name;
        o[QStringLiteral("settings")] = p.settings.toJson();
        list.push_back(o);
    }
    QJsonObject root;
    root[QStringLiteral("version")] = kFormatVersion;
    root[QStringLiteral("presets")] = list;
    QDir().mkpath(QFileInfo(m_userFile).absolutePath());
    QSaveFile file(m_userFile);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson()) < 0
        || !file.commit()) {
        if (error) {
            *error = QCoreApplication::translate("kk::ExportPresets", "Cannot write %1.")
                         .arg(m_userFile);
        }
        return false;
    }
    return true;
}

} // namespace kk
