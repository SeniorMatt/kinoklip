// SPDX-License-Identifier: GPL-3.0-or-later
// Theme tests: Breeze style, KDE colour schemes, icon mapping, Settings menu actions/page.
// In the KF6 container build these run against the installed (latest) Breeze; in the Qt 6.4
// fallback build they check the palette approximation instead.
#include "app/ColorSchemeFile.h"
#include "app/InstantPopups.h"
#include "app/Theme.h"
#include "core/Preferences.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/Icons.h"
#include "ui/common/Resources.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QMenu>
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

    void defaultsAreBreezeAndKinoklip()
    {
        // The default colour scheme is Breeze Dark (since 1.6), in both build modes.
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
        for (const char* id : {"Kinoklip", "BreezeDark", "BreezeLight", "BreezeClassic"})
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
        QAction* kinoklip = registry.action(theme::colorSchemeActionId(QStringLiteral("Kinoklip")));
        QVERIFY(light && dark && kinoklip);
        QVERIFY(dark->isChecked()); // the default
        kinoklip->trigger();
        QVERIFY(kinoklip->isChecked());
        dark->trigger();
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

    /// Instant popups (Reddit feedback on 1.0.0-beta.1, spec 36.13): even when the desktop
    /// turns Qt's menu, combo and tooltip effects on (Windows does from its own settings), the
    /// applied theme turns them off, menus close without fade or blink, and a menu or a
    /// combo-box list is fully opaque right after it pops up.
    void spec36_13_popupsAppearInstantly()
    {
        for (const Qt::UIEffect effect : {Qt::UI_General, Qt::UI_AnimateMenu, Qt::UI_FadeMenu,
                 Qt::UI_AnimateCombo, Qt::UI_AnimateTooltip, Qt::UI_FadeTooltip})
            QApplication::setEffectEnabled(effect, true);
        theme::apply(*qApp);
        QVERIFY(instantpopups::effectsOff());
        QVERIFY(!QApplication::isEffectEnabled(Qt::UI_FadeMenu));
        QVERIFY(!QApplication::isEffectEnabled(Qt::UI_AnimateMenu));
        QVERIFY(!QApplication::isEffectEnabled(Qt::UI_AnimateCombo));
        QVERIFY(!QApplication::isEffectEnabled(Qt::UI_FadeTooltip));
        QVERIFY(!QApplication::isEffectEnabled(Qt::UI_AnimateTooltip));
        QCOMPARE(qApp->style()->styleHint(QStyle::SH_Menu_FadeOutOnHide), 0);
        QCOMPARE(qApp->style()->styleHint(QStyle::SH_Menu_FlashTriggeredItem), 0);

        QWidget host;
        host.resize(300, 200);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QMenu menu(&host);
        menu.addAction(QStringLiteral("Cut"));
        menu.addAction(QStringLiteral("Copy"));
        menu.popup(host.mapToGlobal(QPoint(20, 20)));
        // At once, before any event loop turn: visible and fully opaque (a fade starts at 0).
        QVERIFY(menu.isVisible());
        QCOMPARE(menu.windowOpacity(), 1.0);
        QVERIFY(menu.property("_kk_instant_popup").toBool()); // marked for the compositor
        if (QGuiApplication::platformName() == QLatin1String("xcb")) // KWin hint set (X11)
            QVERIFY(instantpopups::markWindow(&menu));
        QTest::qWait(40); // a fade or roll would be under way now
        QCOMPARE(menu.windowOpacity(), 1.0);
        QCOMPARE(menu.geometry().size(), menu.sizeHint());
        menu.hide();
        QVERIFY(!menu.isVisible());

        QComboBox combo(&host);
        combo.addItems({QStringLiteral("Fit"), QStringLiteral("Fill"), QStringLiteral("Stretch")});
        combo.show();
        combo.showPopup();
        QWidget* list = combo.view()->window();
        QVERIFY(list->isVisible());
        QCOMPARE(list->windowOpacity(), 1.0);
        QTest::qWait(40);
        QCOMPARE(list->windowOpacity(), 1.0);
        combo.hidePopup();
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
