// SPDX-License-Identifier: GPL-3.0-or-later
// AiModelDialog — the download of the AI model of Background Removal and its management (spec
// 78): the dialog that tells what will be downloaded (name, size, licence, source) and only then
// downloads it, and the Preferences page "AI Models" that shows, downloads and removes the model
// and clears the matte cache.
#pragma once

#include "matting/ModelCatalog.h"
#include "matting/ModelDownload.h"

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;

namespace kk {

class SettingsPageRegistry;

/// Shows what the model is and where it comes from; nothing is fetched until Download is pressed.
class ModelDownloadDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit ModelDownloadDialog(const matting::ModelInfo& model, QWidget* parent = nullptr);

    /// Runs the dialog; true if the model is installed afterwards.
    static bool run(QWidget* parent);
    /// The facts shown (tests).
    [[nodiscard]] QString infoText() const;
    /// Starts the download as the Download button does (tests with a fake model).
    void startDownload();

private:
    void onFinished(bool ok, const QString& message);

    matting::ModelInfo m_model;
    QLabel* m_info = nullptr;
    QLabel* m_message = nullptr;
    QProgressBar* m_progress = nullptr;
    QPushButton* m_download = nullptr;
    QPushButton* m_cancel = nullptr;
    matting::ModelDownload* m_job = nullptr;
};

/// Adds the "AI Models" page to Preferences > System.
void registerAiModelsPage(SettingsPageRegistry& pages);

} // namespace kk
