// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the typed preference accessors (hardware acceleration, autosave interval).
#include "core/Preferences.h"

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

using namespace cw;

class TestPreferences : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("cutwise-tests"));
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

    void cleanupTestCase() { QSettings().clear(); }
};

QTEST_GUILESS_MAIN(TestPreferences)
#include "test_preferences.moc"
