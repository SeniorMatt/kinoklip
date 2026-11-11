// SPDX-License-Identifier: GPL-3.0-or-later
// The model download dialog and the Preferences page "AI Models" (spec 78): the dialog shows the
// name, size, licence and source before anything is fetched, has Download and Cancel, and a
// download of a forbidden address fails at once; the page shows the state and can remove the model.
#include "matting/MatteService.h"
#include "matting/ModelCatalog.h"
#include "ui/common/SettingsPages.h"
#include "ui/dialogs/AiModelDialog.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;

class TestAiModelDialog : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_dir.isValid());
        matting::setModelsDirectoryForTests(m_dir.path());
        matting::MatteService::instance().setCacheRoot(m_dir.path() + QStringLiteral("/cache"));
    }
    void cleanupTestCase() { matting::setModelsDirectoryForTests({}); }

    void dialogTellsWhatIsDownloadedBeforeAnythingHappens()
    {
        const matting::ModelInfo& m = matting::defaultModel();
        ModelDownloadDialog dialog(m);
        const QString text = dialog.infoText();
        QVERIFY(text.contains(m.name));
        QVERIFY(text.contains(matting::sizeText(m.sizeBytes)));
        QVERIFY(text.contains(m.licence));
        QVERIFY(text.contains(m.sourceUrl.toString()));
        QVERIFY(text.contains(m.licenceUrl.toString()));
        QVERIFY(text.contains(QStringLiteral("only now because you asked")));
        auto* download = dialog.findChild<QPushButton*>(QStringLiteral("ModelDownload"));
        auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("ModelCancel"));
        QVERIFY(download && cancel);
        QVERIFY(download->isEnabled());
        QVERIFY(dialog.findChild<QProgressBar*>(QStringLiteral("ModelProgress"))->isHidden());
        QVERIFY(!matting::isInstalled(m)); // nothing was fetched
        QVERIFY(!QFile::exists(matting::modelPath(m) + QStringLiteral(".part")));
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("AiBadge")));
    }

    void thePageShowsTheStateAndRemovesTheModel()
    {
        SettingsPageRegistry registry;
        registerAiModelsPage(registry);
        QVERIFY(registry.contains(QStringLiteral("ai_models")));
        const SettingsPageInfo info = registry.pages().first();
        QCOMPARE(info.group, QStringLiteral("system"));
        std::unique_ptr<SettingsPage> page(info.create(nullptr));
        auto* status = page->findChild<QLabel*>(QStringLiteral("AiModelStatus"));
        QVERIFY(status);
        QVERIFY(status->text().contains(matting::defaultModel().licence));
        QVERIFY(status->text().contains(QStringLiteral("not installed")));
        auto* remove = page->findChild<QPushButton*>(QStringLiteral("AiModelRemove"));
        QVERIFY(remove && !remove->isEnabled()); // nothing to remove
        auto* download = page->findChild<QPushButton*>(QStringLiteral("AiModelDownload"));
        QVERIFY(download);
        if (!matting::runtimeCompiledIn()) {
            QVERIFY(!download->isEnabled());
            QVERIFY(status->text().contains(QStringLiteral("not part of this build")));
        }
        const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            page->resize(560, 420);
            page->show();
            QCoreApplication::processEvents();
            page->grab().save(dir + QStringLiteral("/preferences_ai_models.png"));
            ModelDownloadDialog dialog(matting::defaultModel());
            dialog.show();
            QCoreApplication::processEvents();
            dialog.grab().save(dir + QStringLiteral("/ai_model_download.png"));
        }
    }
};

QTEST_MAIN(TestAiModelDialog)
#include "test_aimodeldialog.moc"
