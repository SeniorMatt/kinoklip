// SPDX-License-Identifier: GPL-3.0-or-later
#include "matting/ModelDownload.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace kk::matting {

ModelDownload::ModelDownload(const ModelInfo& model, QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_net(new QNetworkAccessManager(this))
{ }

ModelDownload::~ModelDownload()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    if (m_file) {
        m_file->close();
        m_file->remove();
    }
}

void ModelDownload::start()
{
    if (m_reply)
        return;
    m_cancelled = false;
    if (!isAllowedDownloadUrl(m_model.url)) {
        finish(false, tr("The download address is not allowed: %1").arg(m_model.url.toString()));
        return;
    }
    const QString target = modelPath(m_model);
    QDir().mkpath(QFileInfo(target).absolutePath());
    m_file = new QFile(target + QStringLiteral(".part"));
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString why = m_file->errorString();
        delete m_file;
        m_file = nullptr;
        finish(false, tr("Cannot write the model file: %1").arg(why));
        return;
    }
    QNetworkRequest request(m_model.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy); // HTTPS stays HTTPS
    request.setAttribute(
        QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("kinoklip"));
    m_reply = m_net->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &ModelDownload::onReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        emit progress(got, total > 0 ? total : m_model.sizeBytes);
    });
    connect(m_reply, &QNetworkReply::redirected, this, [this](const QUrl& to) {
        if (!isAllowedDownloadUrl(to) && m_reply) {
            m_reply->abort(); // finished() reports it
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, &ModelDownload::onFinished);
}

void ModelDownload::cancel()
{
    if (!m_reply)
        return;
    m_cancelled = true;
    m_reply->abort();
}

void ModelDownload::onReadyRead()
{
    if (!m_reply || !m_file)
        return;
    const QByteArray chunk = m_reply->readAll();
    // Never write more than the model's size: a wrong file ends early.
    if (m_file->size() + chunk.size() > m_model.sizeBytes + 1024) {
        m_reply->abort();
        return;
    }
    m_file->write(chunk);
}

void ModelDownload::onFinished()
{
    QNetworkReply* reply = m_reply;
    if (!reply)
        return;
    onReadyRead();
    m_reply = nullptr;
    reply->deleteLater();
    const QString part = m_file ? m_file->fileName() : QString();
    if (m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
    if (m_cancelled) {
        QFile::remove(part);
        finish(false, tr("Download cancelled."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        QFile::remove(part);
        finish(false, tr("The download failed: %1").arg(reply->errorString()));
        return;
    }
    QString why;
    if (!installDownloaded(m_model, part, &why)) {
        finish(false, why);
        return;
    }
    finish(true, tr("The model is installed."));
}

void ModelDownload::finish(bool ok, const QString& message)
{
    emit finished(ok, message);
}

} // namespace kk::matting
