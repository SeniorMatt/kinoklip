// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the typed preference accessors (hardware acceleration, autosave interval).
#include "core/Preferences.h"

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

using namespace kk;

class TestPreferences : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_preferences"));
        QSettings().clear();
    }

    void hwAccel()
    {
        QCOMPARE(prefs::hwAccel(), prefs::HwAccel::Auto);
        prefs::setHwAccel(prefs::HwAccel::Vaapi);
        QCOMPARE(prefs::hwAccel(), prefs::HwAccel::Vaapi);
        QCOMPARE(
            QSettings().value(QLatin1String(prefs::kHwAccel)).toString(), QStringLiteral("vaapi"));
        for (const QString& id : prefs::hwAccelIds())
            QCOMPARE(prefs::hwAccelToString(prefs::hwAccelFromString(id)), id);
        QCOMPARE(prefs::hwAccelFromString(QStringLiteral("bogus")), prefs::HwAccel::Auto);
    }

    void autosave()
    {
        QCOMPARE(prefs::autosaveMinutes(), 2);
        prefs::setAutosaveMinutes(-3);
        QCOMPARE(prefs::autosaveMinutes(), 0);
        prefs::setAutosaveMinutes(5);
        QCOMPARE(prefs::autosaveMinutes(), 5);
    }

    /// Playback resolution: default Full; a stored "auto" from alpha.1/alpha.2 (the old
    /// default) becomes "full" exactly once, a later explicit "auto" is kept.
    void previewQualityMigration()
    {
        QSettings().clear();
        QCOMPARE(prefs::previewQuality(), QStringLiteral("full"));
        QSettings().clear();
        QSettings().setValue(QLatin1String(prefs::kPreviewQuality), QStringLiteral("auto"));
        QCOMPARE(prefs::previewQuality(), QStringLiteral("full"));
        QCOMPARE(QSettings().value(QLatin1String(prefs::kPreviewQuality)).toString(),
            QStringLiteral("full"));
        prefs::setPreviewQuality(QStringLiteral("auto")); // chosen again by the user
        QCOMPARE(prefs::previewQuality(), QStringLiteral("auto"));
        QCOMPARE(prefs::previewQuality(), QStringLiteral("auto"));
        // Other old values are kept.
        QSettings().clear();
        QSettings().setValue(QLatin1String(prefs::kPreviewQuality), QStringLiteral("half"));
        QCOMPARE(prefs::previewQuality(), QStringLiteral("half"));
    }

    void cleanupTestCase() { QSettings().clear(); }
};

QTEST_GUILESS_MAIN(TestPreferences)
#include "test_preferences.moc"
