// SPDX-License-Identifier: GPL-3.0-or-later
// ModelCatalog — the AI model Background Removal runs (spec 78): what it is, where its official
// file is, the SHA-256 the file must have, and where it lives on disk. The model is never
// bundled and never downloaded unless the user asks (ModelDownload.h).
#pragma once

#include <QString>
#include <QUrl>

#include <cstdint>

namespace kk::matting {

/// One downloadable model file.
struct ModelInfo
{
    QString id; ///< stable id, also part of the matte cache key
    QString name; ///< shown to the user
    QString description;
    QString fileName; ///< file name inside modelsDirectory()
    qint64 sizeBytes = 0; ///< exact size of the file
    QString sha256; ///< lower-case hex, hard-coded; the download must match it
    QUrl url; ///< official release asset of the model's project (HTTPS)
    QString licence; ///< SPDX-style licence name of code and weights
    QUrl licenceUrl;
    QUrl sourceUrl; ///< the project page
};

/// The model Background Removal uses: Robust Video Matting, MobileNetV3, float32 ONNX.
[[nodiscard]] const ModelInfo& defaultModel();

/// Folder of the downloaded models: `<AppDataLocation>/models`. Tests may redirect it.
[[nodiscard]] QString modelsDirectory();
void setModelsDirectoryForTests(const QString& dir);

/// Full path of the model file (it may not exist).
[[nodiscard]] QString modelPath(const ModelInfo& model);
/// True if the file exists with the exact size (its SHA-256 was checked when it was installed).
[[nodiscard]] bool isInstalled(const ModelInfo& model);
/// Deletes the model file (and an unfinished download). @return true if nothing is left.
bool removeModel(const ModelInfo& model);
/// Size of the model file on disk, 0 if absent.
[[nodiscard]] qint64 installedBytes(const ModelInfo& model);

/// SHA-256 of a file as lower-case hex; empty if it cannot be read.
[[nodiscard]] QString sha256OfFile(const QString& path);

/// The only URLs a model may be fetched from: HTTPS on the hosts GitHub serves release assets
/// from. Anything else (plain HTTP, other hosts) is refused, also after a redirect.
[[nodiscard]] bool isAllowedDownloadUrl(const QUrl& url);

/// Checks the finished download @p partPath against @p model (size, SHA-256) and moves it to
/// its place. On any mismatch the file is deleted and false returned with a reason.
[[nodiscard]] bool installDownloaded(const ModelInfo& model, const QString& partPath, QString* why);

/// "14.3 MB" style size for the dialogs.
[[nodiscard]] QString sizeText(qint64 bytes);

} // namespace kk::matting
