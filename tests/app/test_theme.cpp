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
#include "ui/common/StyleTokens.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QTest>
#include <QVBoxLayout>

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
            // The default style is our own Kinoklip style (spec 144); Breeze stays selectable.
            QCOMPARE(theme::currentStyle(), QStringLiteral("Kinoklip"));
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
        // No application style sheet: it made every switch re-polish every widget.
        QVERIFY(qApp->styleSheet().isEmpty());

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

    void systemStyleFollowsTheDesktopAndFallsBackWithANote()
    {
        QVERIFY(theme::availableStyles().contains(QStringLiteral("System")));
        QCOMPARE(theme::styleDisplayName(QStringLiteral("System")), QStringLiteral("System (the desktop's style)"));
        // a style the plugin folder does not have: Breeze/Fusion stands in, with a note
        qputenv("QT_STYLE_OVERRIDE", "NoSuchStyleXyz");
        theme::setStyle(QStringLiteral("System"));
        QCOMPARE(theme::currentStyle(), QStringLiteral("System"));
        QVERIFY(!theme::styleNote().isEmpty());
        QVERIFY(theme::styleNote().contains(QStringLiteral("NoSuchStyleXyz")));
        QVERIFY(!QApplication::style()->name().isEmpty());
        // one that exists is used without a note
        qputenv("QT_STYLE_OVERRIDE", "Fusion");
        theme::setStyle(QStringLiteral("System"));
        QVERIFY(theme::styleNote().isEmpty());
        QCOMPARE(QApplication::style()->name().toLower(), QStringLiteral("fusion"));
        qunsetenv("QT_STYLE_OVERRIDE");
        theme::setStyle(QStringLiteral("Kinoklip"));
    }

    void spec144_4_defaultStyleIsKinoklipAndBreezeComesBackExactly()
    {
        QCOMPARE(QString::fromLatin1(theme::kDefaultStyle), QStringLiteral("Kinoklip"));
        QCOMPARE(theme::availableStyles().first(), QStringLiteral("Kinoklip"));
        ActionRegistry registry;
        theme::registerActions(registry);
        const bool haveBreeze = QStyleFactory::keys().contains(QStringLiteral("Breeze"));
        const QString other = haveBreeze ? QStringLiteral("Breeze") : QStringLiteral("Fusion");
        registry.action(theme::styleActionId(other))->trigger();
        const auto picture = [] {
            QWidget w;
            auto* lay = new QVBoxLayout(&w);
            lay->addWidget(new QPushButton(QStringLiteral("Button")));
            lay->addWidget(new QSlider(Qt::Horizontal));
            lay->addWidget(new QComboBox);
            w.resize(200, 120);
            return w.grab().toImage();
        };
        const QImage before = picture();
        const int extentBefore = QApplication::style()->pixelMetric(QStyle::PM_ScrollBarExtent);
        QCOMPARE(theme::currentStyle(), other);
        registry.action(theme::styleActionId(QStringLiteral("Kinoklip")))->trigger();
        QCOMPARE(theme::currentStyle(), QStringLiteral("Kinoklip"));
        QCOMPARE(
            QApplication::style()->pixelMetric(QStyle::PM_ScrollBarExtent), kk::style::kScrollBar);
        QVERIFY(picture() != before);
        QCOMPARE(QSettings().value(QLatin1String(prefs::kWidgetStyle)).toString(),
            QStringLiteral("Kinoklip"));
        registry.action(theme::styleActionId(other))->trigger();
        QCOMPARE(theme::currentStyle(), other);
        QCOMPARE(QApplication::style()->pixelMetric(QStyle::PM_ScrollBarExtent), extentBefore);
        QVERIFY(picture() == before); // the same pixels as before the detour through Kinoklip
        // The Preferences list shows the Kinoklip entry first, with its label.
        SettingsPageRegistry pages;
        registerStandardSettingsPages(pages);
        theme::registerSettingsPage(pages, registry);
        PreferencesDialog dialog(&pages);
        auto* styleCombo = dialog.findChild<QComboBox*>(QStringLiteral("WidgetStyleCombo"));
        QVERIFY(styleCombo);
        QCOMPARE(
            styleCombo->itemData(0).toString(), theme::styleActionId(QStringLiteral("Kinoklip")));
        QVERIFY(styleCombo->itemText(0).contains(QStringLiteral("Kinoklip")));
        QVERIFY(!styleCombo->toolTip().isEmpty());
    }

    /// A combo box list and a menu are no taller than their items plus a frame: few entries
    /// never get a big empty dropdown, in every widget style.
    void popupsAreNoTallerThanTheirItems_data()
    {
        QTest::addColumn<QString>("style");
        for (const QString& style : theme::availableStyles())
            QTest::newRow(qPrintable(style)) << style;
    }
    void popupsAreNoTallerThanTheirItems()
    {
        QFETCH(QString, style);
        theme::setStyle(style);
        // Frame, margins and scroll arrows of any style; Libadwaita's popover adds its drop
        // shadow margin around the list.
        const int kFrame = style.compare(QLatin1String("Libadwaita"), Qt::CaseInsensitive) == 0 ? 40 : 24;
        QWidget host;
        host.resize(300, 200);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        for (const int count : {1, 3, 8}) {
            QComboBox combo(&host);
            for (int i = 0; i < count; ++i)
                combo.addItem(QStringLiteral("Entry %1").arg(i));
            combo.show();
            combo.showPopup();
            QWidget* list = combo.view()->window();
            QVERIFY(list->isVisible());
            const int items = count * combo.view()->sizeHintForRow(0);
            QVERIFY2(list->height() <= items + kFrame,
                qPrintable(QStringLiteral("%1: %2 items need %3 px, popup is %4 px")
                               .arg(style)
                               .arg(count)
                               .arg(items)
                               .arg(list->height())));
            combo.hidePopup();
        }
        for (const int count : {1, 3, 8}) {
            QMenu menu(&host);
            int items = 0;
            for (int i = 0; i < count; ++i) {
                QAction* a = menu.addAction(QStringLiteral("Entry %1").arg(i));
                Q_UNUSED(a);
            }
            menu.popup(host.mapToGlobal(QPoint(20, 20)));
            for (QAction* a : menu.actions())
                items += menu.actionGeometry(a).height();
            QVERIFY2(menu.height() <= items + kFrame,
                qPrintable(QStringLiteral("%1: menu of %2 items needs %3 px, is %4 px")
                               .arg(style)
                               .arg(count)
                               .arg(items)
                               .arg(menu.height())));
            menu.hide();
        }
        theme::setStyle(QString::fromLatin1(theme::kDefaultStyle));
    }

    /// A narrow combo box opens a list wide enough for its longest entry, in every style:
    /// entries are never cut off by the list (combopopups).
    void comboListsFitTheirEntries_data() { popupsAreNoTallerThanTheirItems_data(); }
    void comboListsFitTheirEntries()
    {
        QFETCH(QString, style);
        theme::setStyle(style);
        QWidget host;
        host.resize(600, 200);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QComboBox combo(&host);
        combo.addItems({QStringLiteral("Main mix"), QStringLiteral("A rather long bus name"),
            QStringLiteral("Music")});
        combo.setFixedWidth(60);
        combo.move(40, 20);
        combo.show();
        combo.showPopup();
        QWidget* list = combo.view()->window();
        QVERIFY(list->isVisible());
        const int longest = combo.view()->sizeHintForColumn(0);
        QVERIFY2(list->width() >= longest,
            qPrintable(QStringLiteral("%1: list is %2 px wide, an entry needs %3 px")
                           .arg(style)
                           .arg(list->width())
                           .arg(longest)));
        QVERIFY(list->width() > combo.width());
        combo.hidePopup();
        theme::setStyle(QString::fromLatin1(theme::kDefaultStyle));
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
