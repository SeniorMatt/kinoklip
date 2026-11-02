// SPDX-License-Identifier: GPL-3.0-or-later
// Theme tests: Breeze style, KDE colour schemes, icon mapping, Settings menu actions/page.
// In the KF6 container build these run against the installed (latest) Breeze; in the Qt 6.4
// fallback build they check the palette approximation instead.
#include "app/ColorSchemeFile.h"
#include "app/Theme.h"
#include "core/Preferences.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/Icons.h"
#include "ui/common/Resources.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QTest>

#include <memory>

using namespace kk;

class TestTheme : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_theme"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void defaultsAreBreeze()
    {
        QCOMPARE(theme::currentColorScheme(), QStringLiteral("BreezeDark"));
        QVERIFY(theme::isDark());
        if (theme::haveKdeFrameworks()) {
            // The container build ships the real Breeze style: it must be found and used.
            QVERIFY2(QStyleFactory::keys().contains(QStringLiteral("Breeze")),
                qPrintable(QStyleFactory::keys().join(QLatin1Char(','))));
            QCOMPARE(theme::currentStyle(), QStringLiteral("Breeze"));
            // (QApplication::style() is the style-sheet proxy around it, so check the plugin.)
            std::unique_ptr<QStyle> breeze(QStyleFactory::create(QStringLiteral("Breeze")));
            QVERIFY(breeze);
            QCOMPARE(QString::fromLatin1(breeze->metaObject()->className()),
                QStringLiteral("Breeze::Style"));
            // KColorSchemeManager installed Breeze Dark's window colour.
            QCOMPARE(QApplication::palette().color(QPalette::Window), QColor(32, 35, 38));
        } else {
            QVERIFY(!theme::currentStyle().isEmpty()); // Breeze if installed, else Fusion
        }
    }

    void schemesListed()
    {
        QStringList ids;
        for (const theme::ColorSchemeInfo& s : theme::availableColorSchemes())
            ids.append(s.id);
        QVERIFY(ids.size() >= 4);
        QVERIFY(ids.first().isEmpty()); // system entry first
        for (const char* id : {"BreezeDark", "BreezeLight", "BreezeClassic"})
            QVERIFY2(ids.contains(QLatin1String(id)), id);
    }

    void colorSchemeFileMatchesKde()
    {
        theme::ColorSchemeFile dark;
        QVERIFY(dark.load(QStringLiteral(":/color-schemes/BreezeDark.colors")));
        QCOMPARE(dark.id(), QStringLiteral("BreezeDark"));
        QCOMPARE(dark.name(), QStringLiteral("Breeze Dark"));
        const QPalette p = dark.palette();
        QCOMPARE(p.color(QPalette::Window), QColor(32, 35, 38));
        QCOMPARE(p.color(QPalette::Highlight), QColor(61, 174, 233));
        theme::ColorSchemeFile light;
        QVERIFY(light.load(QStringLiteral(":/color-schemes/BreezeLight.colors")));
        QCOMPARE(light.palette().color(QPalette::Window), QColor(239, 240, 241));
        QVERIFY(!light.load(QStringLiteral(":/does-not-exist.colors")));
    }

    void switchingThroughActions()
    {
        ActionRegistry registry;
        registerStandardActions(registry);
        theme::registerActions(registry);
        // Settings menu is data: two submenu markers, filled by the theme.
        const QStringList entries = registry.menuEntries(QStringLiteral("settings"));
        QVERIFY(entries.contains(QStringLiteral(">settings.color_scheme")));
        QVERIFY(entries.contains(QStringLiteral(">settings.style")));
        QVERIFY(registry.isSubmenu(QStringLiteral("settings.color_scheme")));

        QAction* light = registry.action(theme::colorSchemeActionId(QStringLiteral("BreezeLight")));
        QAction* dark = registry.action(theme::colorSchemeActionId(QStringLiteral("BreezeDark")));
        QVERIFY(light && dark);
        QVERIFY(dark->isChecked());
        light->trigger();
        QVERIFY(!theme::isDark());
        QVERIFY(light->isChecked());
        QVERIFY(!dark->isChecked());
        QCOMPARE(QSettings().value(QLatin1String(prefs::kColorScheme)).toString(),
            QStringLiteral("BreezeLight"));
        QCOMPARE(QApplication::palette().color(QPalette::Window), QColor(239, 240, 241));
        // The panel style sheet follows the palette.
        QVERIFY(qApp->styleSheet().contains(QStringLiteral("PanelFrame")));
        QVERIFY(qApp->styleSheet().contains(QColor(239, 240, 241).name(QColor::HexArgb)));

        // Style switch keeps the colour scheme.
        QAction* fusion = registry.action(theme::styleActionId(QStringLiteral("Fusion")));
        QVERIFY(fusion);
        fusion->trigger();
        QCOMPARE(theme::currentStyle(), QStringLiteral("Fusion"));
        QCOMPARE(QApplication::palette().color(QPalette::Window), QColor(239, 240, 241));
        QCOMPARE(QSettings().value(QLatin1String(prefs::kWidgetStyle)).toString(),
            QStringLiteral("Fusion"));

        // Settings page applies through the same actions.
        SettingsPageRegistry pages;
        registerStandardSettingsPages(pages);
        theme::registerSettingsPage(pages, registry);
        QVERIFY(pages.contains(QStringLiteral("appearance")));
        PreferencesDialog dialog(&pages);
        auto* schemeCombo = dialog.findChild<QComboBox*>(QStringLiteral("ColorSchemeCombo"));
        auto* styleCombo = dialog.findChild<QComboBox*>(QStringLiteral("WidgetStyleCombo"));
        QVERIFY(schemeCombo && styleCombo);
        QCOMPARE(schemeCombo->currentData().toString(), light->objectName());
        schemeCombo->setCurrentIndex(schemeCombo->findData(dark->objectName()));
        const QString wantedStyle = theme::styleActionId(theme::availableStyles().first());
        styleCombo->setCurrentIndex(styleCombo->findData(wantedStyle));
        dialog.apply();
        QVERIFY(dark->isChecked());
        QVERIFY(theme::isDark());
        QCOMPARE(theme::styleActionId(theme::currentStyle()), wantedStyle);
    }

    void breezeIconsMapped()
    {
        if (!theme::haveKdeFrameworks()
            && !QIcon::hasThemeIcon(QStringLiteral("media-playback-start")))
            QSKIP("no Breeze icon theme in this build/environment");
        // Every mapped name exists in the Breeze icon set we ship (catches renames in new
        // Breeze releases), and icon() then returns the theme icon, not our SVG.
        for (const QString& name : iconNames()) {
            const QString themeName = themeIconName(name);
            QVERIFY2(QIcon::hasThemeIcon(themeName),
                qPrintable(name + QStringLiteral(" -> ") + themeName));
            QCOMPARE(icon(name).name(), themeName);
        }
        // Editor glyphs without a Breeze equivalent keep our tinted SVG.
        QVERIFY(themeIconName(QStringLiteral("blade")).isEmpty());
        QVERIFY(!icon(QStringLiteral("blade")).isNull());
    }
};

int main(int argc, char** argv)
{
    theme::initBeforeApplication(); // as in main(): must run before QApplication
    QApplication app(argc, argv);
    TestTheme test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_theme.moc"
