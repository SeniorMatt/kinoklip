// SPDX-License-Identifier: GPL-3.0-or-later
// Dialog sizes (spec 73): every dialog opens big enough for its content, so no scroll bar shows
// on its first page at the default size, and it fits a 1366x768 screen. Project Settings and
// Preferences are checked on every page. Saved geometry smaller than the minimum is not used.
#include "PanelTestUtils.h"
#include "app/BugReport.h"
#include "app/BugReportDialog.h"
#include "app/LogExportDialog.h"
#include "app/SettingsTransfer.h"
#include "app/SettingsTransferDialog.h"
#include "app/Theme.h"
#include "core/SmartBins.h"
#include "projects/ProjectLibrary.h"
#include "ui/common/DialogSizing.h"
#include "ui/common/Resources.h"
#include "ui/dialogs/ClipAttributesDialog.h"
#include "ui/dialogs/HistoryWindow.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/dialogs/NormalizeAudioDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/ProjectSettingsDialog.h"
#include "ui/help/CopyLinkDialog.h"
#include "ui/help/GuideDialog.h"
#include "ui/mediapool/SmartBinDialog.h"
#include "ui/projects/NewProjectDialog.h"
#include "ui/projects/ProjectManagerWindow.h"
#include "ui/timeline/ChangeSpeedDialog.h"

#include <QAbstractItemView>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace kk;
using namespace kk::test;

namespace {

const QSize kScreen(1366, 768);

/// The scroll bars that must not be needed: every scroll area except the views whose content
/// can really be long (lists, trees, text previews).
bool mayScroll(const QAbstractScrollArea* area)
{
    if (qobject_cast<const QPlainTextEdit*>(area))
        return true;
    if (qobject_cast<const QAbstractItemView*>(area))
        return !area->objectName().endsWith(
            QLatin1String("Sections")); // section lists never scroll
    return false;
}

/// A message naming the first visible scroll bar that is shown without need, else empty.
QString scrollBarProblem(QWidget* dialog)
{
    QCoreApplication::processEvents();
    for (auto* area : dialog->findChildren<QAbstractScrollArea*>()) {
        if (!area->isVisible() || mayScroll(area))
            continue;
        if (area->horizontalScrollBar()->isVisible())
            return QStringLiteral("horizontal scroll bar in %1 %2")
                .arg(QLatin1String(area->metaObject()->className()), area->objectName());
        if (area->verticalScrollBar()->isVisible())
            return QStringLiteral("vertical scroll bar in %1 %2")
                .arg(QLatin1String(area->metaObject()->className()), area->objectName());
    }
    return {};
}

/// Shows @p dialog and checks it fits the screen and the first page needs no scroll bar.
void verifyOpens(QDialog& dialog, const char* name)
{
    dialog.show();
    QVERIFY2(QTest::qWaitForWindowExposed(&dialog), name);
    QCoreApplication::processEvents();
    const QSize limit = dialogsizing::screenLimit(&dialog);
    QVERIFY2(dialog.width() <= limit.width() && dialog.height() <= limit.height(),
        qPrintable(QStringLiteral("%1 is %2x%3")
                       .arg(QLatin1String(name))
                       .arg(dialog.width())
                       .arg(dialog.height())));
    QVERIFY2(dialog.width() <= kScreen.width() && dialog.height() <= kScreen.height(), name);
    QVERIFY2(
        dialog.minimumWidth() <= limit.width() && dialog.minimumHeight() <= limit.height(), name);
    QVERIFY2(
        dialog.width() >= dialog.minimumWidth() && dialog.height() >= dialog.minimumHeight(), name);
    const QString problem = scrollBarProblem(&dialog);
    QVERIFY2(problem.isEmpty(), qPrintable(QLatin1String(name) + QStringLiteral(": ") + problem));
    qInfo("%s opens at %dx%d (minimum %dx%d)", name, dialog.width(), dialog.height(),
        dialog.minimumWidth(), dialog.minimumHeight());
}

} // namespace

class TestDialogSizing : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_dialogsizing"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
        dialogsizing::setScreenSizeForTesting(kScreen);
    }
    void cleanupTestCase() { dialogsizing::setScreenSizeForTesting({}); }

    void projectSettingsEveryPage()
    {
        PanelFixture f;
        ProjectSettingsDialog dialog(&f.project);
        verifyOpens(dialog, "ProjectSettings");
        auto* sections = dialog.findChild<QListWidget*>(QStringLiteral("ProjectSettingsSections"));
        QVERIFY(sections);
        for (int row = 0; row < sections->count(); ++row) {
            sections->setCurrentRow(row);
            const QString problem = scrollBarProblem(&dialog);
            QVERIFY2(problem.isEmpty(),
                qPrintable(sections->item(row)->text() + QStringLiteral(": ") + problem));
        }
    }
    void projectSettingsIsNotTiny()
    {
        PanelFixture f;
        ProjectSettingsDialog dialog(&f.project);
        // Whatever size a window manager remembers: not below what the content needs.
        QVERIFY(dialog.minimumWidth() >= 600 && dialog.minimumHeight() >= 300);
        dialog.resize(390, 200);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(
            dialog.width() >= dialog.minimumWidth() && dialog.height() >= dialog.minimumHeight());
    }
    void preferencesEverySection()
    {
        PreferencesDialog dialog(nullptr);
        verifyOpens(dialog, "Preferences");
        auto* sections = dialog.findChild<QListWidget*>(QStringLiteral("PreferencesSections"));
        QVERIFY(sections);
        QStringList failed;
        for (int row = 0; row < sections->count(); ++row) {
            if (sections->item(row)->isHidden())
                continue;
            sections->setCurrentRow(row);
            const QString problem = scrollBarProblem(&dialog);
            if (!problem.isEmpty())
                failed << sections->item(row)->text() + QStringLiteral(": ") + problem;
        }
        QVERIFY2(failed.isEmpty(), qPrintable(failed.join(QLatin1Char('\n'))));
    }
    void keyboardCustomization()
    {
        PanelFixture f;
        KeyboardCustomizationDialog dialog(f.actions);
        verifyOpens(dialog, "KeyboardCustomization");
    }
    void historyWindow()
    {
        PanelFixture f;
        HistoryWindow dialog(&f.project);
        verifyOpens(dialog, "History");
    }
    void guideDialog()
    {
        GuideDialog dialog;
        verifyOpens(dialog, "Guide");
    }
    void smartBinDialog()
    {
        SmartBin bin;
        SmartBinDialog dialog(bin);
        verifyOpens(dialog, "SmartBin");
    }
    void newProjectDialog()
    {
        NewProjectDialog dialog(QStringLiteral("My project"));
        verifyOpens(dialog, "NewProject");
    }
    void bugReportDialog()
    {
        BugReportDialog dialog(bugreport::collect());
        verifyOpens(dialog, "BugReport");
    }
    void logExportDialog()
    {
        LogExportDialog dialog(QStringLiteral("line one\nline two\n"));
        verifyOpens(dialog, "LogExport");
    }
    void settingsTransferDialogs()
    {
        std::unique_ptr<SettingsTransferDialog> exportDialog(
            SettingsTransferDialog::forExport(QStringLiteral("/tmp/kinoklip-settings.kks")));
        verifyOpens(*exportDialog, "ExportSettings");
        settingstransfer::Parsed parsed;
        parsed.appVersion = QStringLiteral("1.3");
        parsed.summary = {QStringLiteral("Editing preferences: 14 settings")};
        std::unique_ptr<SettingsTransferDialog> importDialog(
            SettingsTransferDialog::forImport(QStringLiteral("/tmp/kinoklip-settings.kks"), parsed,
                settingstransfer::standardLocations()));
        verifyOpens(*importDialog, "ImportSettings");
    }
    void copyLinkDialog()
    {
        guide::CopyLinkDialog dialog(QUrl(QStringLiteral("https://example.org/guide/some-article")));
        verifyOpens(dialog, "CopyLink");
    }
    void clipAttributesDialog()
    {
        MediaItem item;
        item.name = QStringLiteral("clip.mp4");
        item.path = QStringLiteral("/tmp/clip.mp4");
        ClipAttributesDialog dialog(item, 1);
        verifyOpens(dialog, "ClipAttributes");
    }
    void normalizeAudioDialog()
    {
        PanelFixture f;
        NormalizeAudioDialog dialog(&f.project, {});
        verifyOpens(dialog, "NormalizeAudio");
    }
    void changeSpeedDialog()
    {
        ClipData clip;
        clip.duration = 100;
        ChangeSpeedDialog dialog(clip, Rational {25, 1}, Rational {25, 1}, nullptr);
        verifyOpens(dialog, "ChangeSpeed");
    }
    void projectManagerWindow()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ProjectLibrary library(root.path(), root.path() + QStringLiteral("/cache"));
        ProjectManagerWindow dialog(&library);
        verifyOpens(dialog, "ProjectManager");
    }
    void savedGeometryBelowMinimumIsIgnored()
    {
        QWidget small;
        small.resize(200, 150);
        const QByteArray tiny = small.saveGeometry();
        QWidget window;
        window.setMinimumSize(600, 400);
        window.resize(800, 500);
        QVERIFY(!dialogsizing::restoreGeometryIfFits(&window, tiny, QSize(800, 500)));
        QCOMPARE(window.size(), QSize(800, 500));
        QWidget ok;
        ok.resize(700, 450);
        const QByteArray fine = ok.saveGeometry();
        QVERIFY(dialogsizing::restoreGeometryIfFits(&window, fine, QSize(800, 500)));
        QCOMPARE(window.size(), QSize(700, 450));
    }
    void sizeIsCappedToTheScreen()
    {
        QWidget window;
        window.setMinimumSize(100, 100);
        QSize initial = dialogsizing::fit(&window, QSize(5000, 5000));
        const QSize limit = dialogsizing::screenLimit(&window);
        QCOMPARE(initial, limit);
        QVERIFY(window.minimumWidth() <= limit.width());
    }
};

QTEST_MAIN(TestDialogSizing)
#include "test_dialogsizing.moc"
