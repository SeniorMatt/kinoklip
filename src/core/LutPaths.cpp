// SPDX-License-Identifier: GPL-3.0-or-later
// LutPaths implementation.
#include "core/LutPaths.h"

#include <QFileInfo>
#include <QJsonArray>

namespace kk::lutpaths {

namespace {

constexpr const char* kKeys[] = {"lut", "inputLut"};

QJsonValue encode(const QJsonValue& value, const QDir& dir);
QJsonValue decode(const QJsonValue& value, const QDir& dir);

QJsonObject encodeObject(const QJsonObject& in, const QDir& dir)
{
    QJsonObject out;
    for (auto it = in.begin(); it != in.end(); ++it)
        out.insert(it.key(), encode(it.value(), dir));
    for (const char* key : kKeys) {
        const QString path = in.value(QLatin1String(key)).toString();
        if (!path.isEmpty() && QDir::isAbsolutePath(path))
            out.insert(QLatin1String(key) + QLatin1String("Relative"), dir.relativeFilePath(path));
    }
    return out;
}

QJsonObject decodeObject(const QJsonObject& in, const QDir& dir)
{
    QJsonObject out;
    for (auto it = in.begin(); it != in.end(); ++it)
        out.insert(it.key(), decode(it.value(), dir));
    for (const char* key : kKeys) {
        const QString k = QLatin1String(key);
        const QString stored = in.value(k).toString();
        if (!in.value(k).isString() || stored.isEmpty())
            continue;
        const QString relative = in.value(k + QLatin1String("Relative")).toString();
        QString path = stored;
        if (!QDir::isAbsolutePath(path)) {
            path = dir.absoluteFilePath(path);
        } else if (!QFileInfo::exists(path) && !relative.isEmpty()) {
            const QString candidate = dir.absoluteFilePath(relative);
            if (QFileInfo::exists(candidate))
                path = QDir::cleanPath(candidate);
        }
        out.insert(k, path);
    }
    return out;
}

QJsonValue encode(const QJsonValue& value, const QDir& dir)
{
    if (value.isObject())
        return encodeObject(value.toObject(), dir);
    if (value.isArray()) {
        QJsonArray out;
        for (const QJsonValue& v : value.toArray())
            out.append(encode(v, dir));
        return out;
    }
    return value;
}

QJsonValue decode(const QJsonValue& value, const QDir& dir)
{
    if (value.isObject())
        return decodeObject(value.toObject(), dir);
    if (value.isArray()) {
        QJsonArray out;
        for (const QJsonValue& v : value.toArray())
            out.append(decode(v, dir));
        return out;
    }
    return value;
}

} // namespace

QJsonObject addRelative(const QJsonObject& root, const QDir& projectDir)
{
    return encodeObject(root, projectDir);
}

QJsonObject resolve(const QJsonObject& root, const QDir& projectDir)
{
    return decodeObject(root, projectDir);
}

} // namespace kk::lutpaths
