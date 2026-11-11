// SPDX-License-Identifier: GPL-3.0-or-later
#include "matting/ModelCatalog.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace kk::matting {

namespace {
QString g_testDirectory;
}

const ModelInfo& defaultModel()
{
    static const ModelInfo model = [] {
        ModelInfo m;
        m.id = QStringLiteral("rvm-mobilenetv3-fp32");
        m.name = QStringLiteral("Robust Video Matting (MobileNetV3)");
        m.description = QCoreApplication::translate("kk::matting",
            "A small AI model that cuts a person out of a video and keeps the edges steady from "
            "frame to frame. It runs on your computer; nothing is sent anywhere.");
        m.fileName = QStringLiteral("rvm_mobilenetv3_fp32.onnx");
        m.sizeBytes = 14975696;
        m.sha256
            = QStringLiteral("88d4531297118f595bf2fd60f6f566aec2e559393802d1f436c380f0cbbd2828");
        m.url = QUrl(QStringLiteral("https://github.com/PeterL1n/RobustVideoMatting/releases/"
                                    "download/v1.0.0/rvm_mobilenetv3_fp32.onnx"));
        m.licence = QStringLiteral("GPL-3.0");
        m.licenceUrl = QUrl(
            QStringLiteral("https://github.com/PeterL1n/RobustVideoMatting/blob/master/LICENSE"));
        m.sourceUrl = QUrl(QStringLiteral("https://github.com/PeterL1n/RobustVideoMatting"));
        return m;
    }();
    return model;
}

QString modelsDirectory()
{
    if (!g_testDirectory.isEmpty())
        return g_testDirectory;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/models");
}

void setModelsDirectoryForTests(const QString& dir)
{
    g_testDirectory = dir;
}

QString modelPath(const ModelInfo& model)
{
    return modelsDirectory() + QLatin1Char('/') + model.fileName;
}

bool isInstalled(const ModelInfo& model)
{
    const QFileInfo info(modelPath(model));
    return info.isFile() && info.size() == model.sizeBytes;
}

qint64 installedBytes(const ModelInfo& model)
{
    const QFileInfo info(modelPath(model));
    return info.isFile() ? info.size() : 0;
}

bool removeModel(const ModelInfo& model)
{
    const QString path = modelPath(model);
    QFile::remove(path + QStringLiteral(".part"));
    if (QFileInfo::exists(path))
        QFile::remove(path);
    return !QFileInfo::exists(path);
}

QString sha256OfFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool isAllowedDownloadUrl(const QUrl& url)
{
    if (url.scheme() != QLatin1String("https") || !url.userName().isEmpty() || url.port() > 0)
        return false;
    const QString host = url.host().toLower();
    return host == QLatin1String("github.com")
        || host == QLatin1String("objects.githubusercontent.com")
        || host == QLatin1String("release-assets.githubusercontent.com");
}

bool installDownloaded(const ModelInfo& model, const QString& partPath, QString* why)
{
    auto fail = [&](const QString& text) {
        QFile::remove(partPath);
        if (why)
            *why = text;
        return false;
    };
    const QFileInfo info(partPath);
    if (!info.isFile())
        return fail(QCoreApplication::translate("kk::matting", "The downloaded file is missing."));
    if (info.size() != model.sizeBytes)
        return fail(QCoreApplication::translate(
            "kk::matting", "The downloaded file has the wrong size (%1 bytes, expected %2).")
                        .arg(info.size())
                        .arg(model.sizeBytes));
    if (sha256OfFile(partPath) != model.sha256)
        return fail(QCoreApplication::translate("kk::matting",
            "The downloaded file does not match the checksum of the model. It was deleted."));
    const QString target = modelPath(model);
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::remove(target);
    if (!QFile::rename(partPath, target))
        return fail(
            QCoreApplication::translate("kk::matting", "The model could not be saved to %1.")
                .arg(target));
    return true;
}

QString sizeText(qint64 bytes)
{
    return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 1)
        + QCoreApplication::translate("kk::matting", " MB");
}

} // namespace kk::matting
