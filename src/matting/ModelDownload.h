// SPDX-License-Identifier: GPL-3.0-or-later
// ModelDownload — fetches a model file (ModelCatalog.h) over HTTPS, only when the user pressed
// Download. Progress and cancel; the file is written next to its place as "<name>.part", checked
// against the hard-coded size and SHA-256 and only then renamed. No cookies, no credentials, no
// other request is ever made.
#pragma once

#include "matting/ModelCatalog.h"

#include <QObject>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

namespace kk::matting {

class ModelDownload : public QObject
{
    Q_OBJECT
public:
    explicit ModelDownload(const ModelInfo& model, QObject* parent = nullptr);
    ~ModelDownload() override;

    /// Starts the download of the model's URL.
    void start();
    /// Stops and deletes the partial file; finished(false, "cancelled") follows.
    void cancel();
    [[nodiscard]] bool isRunning() const { return m_reply != nullptr; }

signals:
    void progress(qint64 received, qint64 total);
    void finished(bool ok, const QString& message);

private:
    void onReadyRead();
    void onFinished();
    void finish(bool ok, const QString& message);

    ModelInfo m_model;
    QNetworkAccessManager* m_net = nullptr;
    QNetworkReply* m_reply = nullptr;
    QFile* m_file = nullptr;
    bool m_cancelled = false;
};

} // namespace kk::matting
