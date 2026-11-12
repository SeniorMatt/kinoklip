// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/AiModelDialog.h"

#include "matting/MatteService.h"
#include "ui/common/DialogSizing.h"
#include "ui/common/SettingsPages.h"
#include "ui/help/Guide.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace kk {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::AiModelDialog", text);
}

QString link(const QUrl& url)
{
    return QStringLiteral("<a href=\"%1\">%1</a>").arg(url.toString().toHtmlEscaped());
}

QString infoHtml(const matting::ModelInfo& m)
{
    return QStringLiteral("<p><b>%1</b></p><p>%2</p><table cellspacing=\"4\">"
                          "<tr><td>%3</td><td>%4</td></tr>"
                          "<tr><td>%5</td><td>%6 (%7)</td></tr>"
                          "<tr><td>%8</td><td>%9</td></tr></table>"
                          "%11<p>%10</p>")
        .arg(m.name.toHtmlEscaped(), m.description.toHtmlEscaped(), tr("Download size:"),
            matting::sizeText(m.sizeBytes), tr("Licence:"), m.licence.toHtmlEscaped(),
            link(m.licenceUrl), tr("Source:"), link(m.sourceUrl),
            tr("The file is downloaded once, only now because you asked, from the project's "
               "official "
               "release page over HTTPS, and checked against a checksum. It is used on this "
               "computer "
               "only: nothing is uploaded and no account is needed. You can remove it again in "
               "Preferences."),
            m.memoryNote.isEmpty() ? QString()
                                   : QStringLiteral("<p>%1</p>").arg(m.memoryNote.toHtmlEscaped()));
}

} // namespace

ModelDownloadDialog::ModelDownloadDialog(const matting::ModelInfo& model, QWidget* parent)
    : QDialog(parent)
    , m_model(model)
{
    setObjectName(QStringLiteral("ModelDownloadDialog"));
    setWindowTitle(tr("Download AI Model"));
    auto* layout = new QVBoxLayout(this);
    auto* badge = new QLabel(tr("AI tool"));
    badge->setObjectName(QStringLiteral("AiBadge"));
    layout->addWidget(badge);
    m_info = new QLabel(infoHtml(model));
    m_info->setObjectName(QStringLiteral("ModelInfo"));
    m_info->setWordWrap(true);
    m_info->setTextFormat(Qt::RichText);
    m_info->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_info->setOpenExternalLinks(false);
    connect(m_info, &QLabel::linkActivated, this,
        [this](const QString& url) { guide::openUrl(QUrl(url), this); });
    m_info->setMinimumWidth(460);
    layout->addWidget(m_info);
    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("ModelProgress"));
    m_progress->setRange(0, 1000);
    m_progress->hide();
    layout->addWidget(m_progress);
    m_message = new QLabel;
    m_message->setObjectName(QStringLiteral("ModelMessage"));
    m_message->setWordWrap(true);
    layout->addWidget(m_message);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_download = new QPushButton(tr("Download"));
    m_download->setObjectName(QStringLiteral("ModelDownload"));
    m_download->setDefault(true);
    m_cancel = new QPushButton(tr("Cancel"));
    m_cancel->setObjectName(QStringLiteral("ModelCancel"));
    buttons->addWidget(m_download);
    buttons->addWidget(m_cancel);
    layout->addLayout(buttons);
    connect(m_download, &QPushButton::clicked, this, &ModelDownloadDialog::startDownload);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_job && m_job->isRunning())
            m_job->cancel();
        else
            reject();
    });
    dialogsizing::fit(this);
}

QString ModelDownloadDialog::infoText() const
{
    return m_info->text();
}

void ModelDownloadDialog::startDownload()
{
    if (m_job && m_job->isRunning())
        return;
    delete m_job;
    m_job = new matting::ModelDownload(m_model, this);
    connect(m_job, &matting::ModelDownload::progress, this, [this](qint64 got, qint64 total) {
        m_progress->setValue(total > 0 ? int(got * 1000 / total) : 0);
        m_message->setText(
            tr("Downloading... %1 of %2").arg(matting::sizeText(got), matting::sizeText(total)));
    });
    connect(m_job, &matting::ModelDownload::finished, this, &ModelDownloadDialog::onFinished);
    m_progress->setValue(0);
    m_progress->show();
    m_download->setEnabled(false);
    m_message->setText(tr("Starting the download..."));
    m_job->start();
}

void ModelDownloadDialog::onFinished(bool ok, const QString& message)
{
    m_progress->hide();
    m_download->setEnabled(!ok);
    m_message->setText(message);
    if (ok) {
        matting::MatteService::instance().unload();
        accept();
    }
}

bool ModelDownloadDialog::run(QWidget* parent)
{
    return run(parent, matting::defaultModel());
}

bool ModelDownloadDialog::run(QWidget* parent, const matting::ModelInfo& model)
{
    ModelDownloadDialog dialog(model, parent);
    dialog.exec();
    return matting::isInstalled(model);
}

// ---- Preferences > System > AI Models ----

namespace {

class AiModelsPage final : public SettingsPage
{
public:
    explicit AiModelsPage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* layout = new QVBoxLayout(this);
        m_status = new QLabel;
        m_status->setObjectName(QStringLiteral("AiModelStatus"));
        m_status->setWordWrap(true);
        m_status->setTextFormat(Qt::RichText);
        m_status->setTextInteractionFlags(Qt::TextBrowserInteraction);
        connect(m_status, &QLabel::linkActivated, this,
            [this](const QString& url) { guide::openUrl(QUrl(url), this); });
        layout->addWidget(m_status);
        auto makeRow = [this, layout](const matting::ModelInfo& model, const QString& suffix,
                           QPushButton*& download, QPushButton*& remove) {
            auto* row = new QHBoxLayout;
            download = new QPushButton(tr("Download %1...").arg(model.name));
            download->setObjectName(QStringLiteral("Ai%1ModelDownload").arg(suffix));
            remove = new QPushButton(tr("Remove %1").arg(model.name));
            remove->setObjectName(QStringLiteral("Ai%1ModelRemove").arg(suffix));
            row->addWidget(download);
            row->addWidget(remove);
            row->addStretch(1);
            layout->addLayout(row);
            connect(download, &QPushButton::clicked, this, [this, &model] {
                ModelDownloadDialog::run(this, model);
                refresh();
            });
            connect(remove, &QPushButton::clicked, this, [this, &model] {
                if (QMessageBox::question(this, tr("Remove Model"),
                        tr("Remove the downloaded model (%1)? Background Removal with it stops "
                           "working until it is downloaded again.")
                            .arg(matting::sizeText(matting::installedBytes(model))))
                    != QMessageBox::Yes)
                    return;
                matting::MatteService::instance().unload();
                matting::removeModel(model);
                refresh();
            });
        };
        makeRow(matting::defaultModel(), QString(), m_download, m_remove);
        makeRow(matting::imageModel(), QStringLiteral("Image"), m_downloadImage, m_removeImage);
        m_clear = new QPushButton(tr("Clear Matte Cache"));
        m_clear->setObjectName(QStringLiteral("AiMatteClear"));
        layout->addWidget(m_clear);
        m_cache = new QLabel;
        m_cache->setObjectName(QStringLiteral("AiMatteCache"));
        m_cache->setWordWrap(true);
        layout->addWidget(m_cache);
        layout->addStretch(1);
        connect(m_clear, &QPushButton::clicked, this, [this] {
            matting::MatteService::instance().unload();
            matting::MatteService::instance().cache().clear();
            refresh();
        });
        refresh();
    }

    void apply() override { }

private:
    void refresh()
    {
        const auto status = matting::MatteService::instance().status();
        QString text = QStringLiteral("<p><b>%1</b> (%2)</p><p>%3</p>")
                           .arg(tr("AI tool: Background Removal"), tr("optional"),
                               tr("Cuts a subject out of the footage with a model that runs on "
                                  "this computer. The models are not part of Kinoklip: each is "
                                  "downloaded only when you ask. The people model is for video; "
                                  "the any-subject model is for still pictures."));
        for (const matting::ModelInfo* m : {&matting::defaultModel(), &matting::imageModel()}) {
            text += QStringLiteral("<p>%1 %2<br>%3 %4, %5 %6<br>%7 %8<br>%9 %10</p>")
                        .arg(tr("Model:"), m->name.toHtmlEscaped(), tr("Size:"),
                            matting::sizeText(m->sizeBytes), tr("Licence:"),
                            m->licence.toHtmlEscaped(), tr("Source:"), link(m->sourceUrl),
                            tr("Model file:"),
                            matting::isInstalled(*m)
                                ? tr("installed (%1)")
                                      .arg(matting::sizeText(matting::installedBytes(*m)))
                                : tr("not installed"));
            if (!m->memoryNote.isEmpty())
                text += QStringLiteral("<p>%1</p>").arg(m->memoryNote.toHtmlEscaped());
        }
        text += QStringLiteral("<p>%1 %2</p>")
                    .arg(tr("Runtime:"),
                        status.runtimeCompiled ? status.runtime : tr("not part of this build"));
        if (!status.runtimeCompiled)
            text += QStringLiteral("<p>%1 %2</p>")
                        .arg(tr("Unavailable:"), status.reason.toHtmlEscaped());
        m_status->setText(text);
        m_download->setEnabled(
            !matting::isInstalled(matting::defaultModel()) && status.runtimeCompiled);
        m_remove->setEnabled(matting::installedBytes(matting::defaultModel()) > 0);
        m_downloadImage->setEnabled(
            !matting::isInstalled(matting::imageModel()) && status.runtimeCompiled);
        m_removeImage->setEnabled(matting::installedBytes(matting::imageModel()) > 0);
        const qint64 bytes = matting::MatteService::instance().cache().sizeBytes();
        m_cache->setText(tr("Matte cache: %1 (kept so a clip is cut out only once)")
                .arg(matting::sizeText(bytes)));
        m_clear->setEnabled(bytes > 0);
    }

    QLabel* m_status = nullptr;
    QLabel* m_cache = nullptr;
    QPushButton* m_download = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_downloadImage = nullptr;
    QPushButton* m_removeImage = nullptr;
    QPushButton* m_clear = nullptr;
};

} // namespace

void registerAiModelsPage(SettingsPageRegistry& pages)
{
    SettingsPageInfo page {QStringLiteral("ai_models"), tr("AI Models"),
        QStringLiteral("applications-science"), 60,
        [](QWidget* parent) { return new AiModelsPage(parent); }, QStringLiteral("system"),
        QString()};
    pages.add(std::move(page));
}

} // namespace kk
