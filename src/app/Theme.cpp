// SPDX-License-Identifier: GPL-3.0-or-later
// Theme implementation: Breeze style + KColorSchemeManager (KF6) or a palette fallback,
// icon theme selection and the generated, palette-derived panel style sheet.
#include "app/Theme.h"

#include "app/ColorSchemeFile.h"
#include "app/ComboPopups.h"
#include "app/InstantPopups.h"
#include "app/KinoklipStyle.h"
#include "app/MenuMetricsStyle.h"
#include "core/Preferences.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/UiFonts.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QFileInfo>
#include <QFormLayout>
#include <QIcon>
#include <QLoggingCategory>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>

#ifdef KK_HAVE_KF6
#include <KColorScheme>
#include <KColorSchemeManager>
#include <KColorSchemeModel>
#include <KIconTheme>
#include "adwaitastyle.h"

#include <KSharedConfig>
#include <kcolorscheme_version.h>
#endif

#include <algorithm>

// The qt-libadwaita resource (its KDE colour schemes) lives in a static library, so it has
// to be registered by hand once (Q_INIT_RESOURCE must be used outside any namespace).
static void initAdwaitaResources()
{
    static const bool done = [] {
        Q_INIT_RESOURCE(kdecolors);
        return true;
    }();
    Q_UNUSED(done);
}

namespace kk::theme {

namespace {

Q_LOGGING_CATEGORY(lcTheme, "kk.app.theme", QtInfoMsg)

constexpr char kFallbackStyle[] = "Fusion";
constexpr char kSystemStyle[] = "System";
constexpr char kSchemeCategory[] = "settings.color_scheme";
constexpr char kStyleCategory[] = "settings.style";
constexpr char kSystemActionSuffix[] = "system";
constexpr int kSwatchSize = 16; ///< px, colour scheme preview icon in the fallback build

/// Everything the theme remembers between calls (GUI thread only).
struct State
{
    QString styleOverride; ///< --style, this run only
    QString schemeOverride; ///< --color-scheme, this run only
    bool hasSchemeOverride = false;
    QString styleNote; ///< why the "System" style fell back (empty: none)
    QString activeStyle; ///< key of the style in use
    QString activeScheme; ///< id of the scheme in use ("" = system)
    QPalette systemPalette; ///< platform palette before the first apply (fallback "system")
    QString systemIconTheme;
    bool captured = false;
    QPointer<QActionGroup> schemeActions;
    QPointer<QActionGroup> styleActions;
    QHash<QString, QString> fallbackSchemePaths; ///< id -> .colors path (fallback build)
    QColor headerBackground; ///< KDE "Header" colour set background of the active scheme
    SchemeRoles roles; ///< KDE roles of the active scheme beyond QPalette (SchemeColors)
};

State& state()
{
    static State s;
    return s;
}

// ---- Styles ---------------------------------------------------------------------------

/// The factory key matching @p name case-insensitively, or empty.
QString styleKey(const QString& name)
{
    const QStringList keys = QStyleFactory::keys();
    for (const QString& key : keys) {
        if (key.compare(name, Qt::CaseInsensitive) == 0)
            return key;
    }
    return {};
}

bool isAdwaita(const QString& style)
{
    return style.compare(QLatin1String("Libadwaita"), Qt::CaseInsensitive) == 0;
}

bool isSystemStyle(const QString& style)
{
    return style.compare(QLatin1String(kSystemStyle), Qt::CaseInsensitive) == 0;
}

QString styleLabel(const QString& style)
{
    if (style.compare(KinoklipStyle::key(), Qt::CaseInsensitive) == 0)
        return QCoreApplication::translate("kk::theme", "Kinoklip (Resolve look)");
    if (isAdwaita(style))
        return QCoreApplication::translate("kk::theme", "Libadwaita (GNOME look)");
    if (isSystemStyle(style))
        return QCoreApplication::translate("kk::theme", "System (the desktop's style)");
    return style;
}

QString storedStyle()
{
    if (!state().styleOverride.isEmpty())
        return state().styleOverride;
    return QSettings()
        .value(QLatin1String(prefs::kWidgetStyle), QLatin1String(kDefaultStyle))
        .toString();
}

/// The Qt style the desktop uses: QT_STYLE_OVERRIDE (the user's own, from before the AppImage
/// dropped it), kdeglobals [KDE] widgetStyle, else Breeze.
QString desktopStyleName()
{
    for (const char* var : {"KINOKLIP_ORIG_QT_STYLE_OVERRIDE", "QT_STYLE_OVERRIDE"}) {
        const QString v = qEnvironmentVariable(var).trimmed();
        if (!v.isEmpty() && !isSystemStyle(v) && v.compare(KinoklipStyle::key(), Qt::CaseInsensitive) != 0)
            return v;
    }
    const QString home = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    for (const QString& dir : {home, QStringLiteral("/etc/xdg")}) {
        const QString file = dir + QStringLiteral("/kdeglobals");
        if (!QFile::exists(file))
            continue;
        const QString v
            = QSettings(file, QSettings::IniFormat).value(QStringLiteral("KDE/widgetStyle")).toString().trimmed();
        if (!v.isEmpty())
            return v;
    }
    return QStringLiteral("Breeze");
}

/// Sets the style; falls back to Breeze, then Fusion. Returns the key in use. "Kinoklip" is our
/// own style (KinoklipStyle, not a factory key); it carries the menu behaviour of
/// MenuMetricsStyle itself.
QString applyStyle(const QString& wanted)
{
    // Set before the style changes: widgets read it in their StyleChange handlers.
    qApp->setProperty("kkKinoklipStyle", wanted.compare(KinoklipStyle::key(), Qt::CaseInsensitive) == 0);
    if (wanted.compare(KinoklipStyle::key(), Qt::CaseInsensitive) == 0) {
        QApplication::setStyle(new KinoklipStyle);
        return KinoklipStyle::key();
    }
    state().styleNote.clear();
    if (isSystemStyle(wanted)) {
        const QString name = desktopStyleName();
        const QString key = isAdwaita(name) ? name : styleKey(name);
        QStyle* style = key.isEmpty() || isAdwaita(key) ? nullptr : QStyleFactory::create(key);
        if (isAdwaita(name) || style) {
            delete style;
            applyStyle(isAdwaita(name) ? name : key);
            qApp->setProperty("kkKinoklipStyle", false);
            return QString::fromLatin1(kSystemStyle);
        }
        applyStyle(QStringLiteral("Breeze"));
        state().styleNote = QCoreApplication::translate("kk::theme",
            "The desktop's style \"%1\" cannot be loaded here (the AppImage only loads styles built "
            "for its own Qt). Breeze or Fusion is used instead.").arg(name.isEmpty() ? QStringLiteral("?") : name);
        qCInfo(lcTheme) << "system widget style" << name << "not loadable, using Breeze/Fusion";
        qApp->setProperty("kkKinoklipStyle", false);
        return QString::fromLatin1(kSystemStyle);
    }
    if (isAdwaita(wanted)) { // built in (third_party/qt-libadwaita), not the factory plugin
        // Labels keep Kinoklip's interface font weight: libadwaita's bold 700 with greyscale
        // antialiasing looks heavy in Open Sans at Kinoklip's sizes. The style reads this once
        // per process; a value the user set (environment or qt-libadwaita.conf) wins.
        if (!qEnvironmentVariableIsSet("ADWAITA_BOLD_LABELS")
            && !QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                              + QStringLiteral("/qt-libadwaita.conf"),
                QSettings::IniFormat)
                    .contains(QStringLiteral("bold-labels")))
            qputenv("ADWAITA_BOLD_LABELS", "0");
        initAdwaitaResources();
        QApplication::setStyle(new MenuMetricsStyle(new AdwaitaStyle, true));
        return QStringLiteral("Libadwaita");
    }
    for (const QString& candidate :
        {wanted, QStringLiteral("Breeze"), QString::fromLatin1(kFallbackStyle)}) {
        const QString key = styleKey(candidate);
        if (key.isEmpty())
            continue;
        if (key.compare(wanted, Qt::CaseInsensitive) != 0)
            qCInfo(lcTheme) << "widget style" << wanted << "not available, using" << key;
        if (QStyle* style = QStyleFactory::create(key)) {
            QApplication::setStyle(new MenuMetricsStyle(style)); // see MenuMetricsStyle.h
            return key;
        }
    }
    return QApplication::style()->name();
}

// ---- Colour schemes -------------------------------------------------------------------

QString storedScheme()
{
    if (state().hasSchemeOverride)
        return state().schemeOverride;
    QSettings settings;
    if (!settings.contains(QLatin1String(prefs::kColorScheme))
        && settings.value(QLatin1String(prefs::kUseSystemColors), false).toBool()) {
        return QString(); // migrated "Use System Colors"
    }
    return settings.value(QLatin1String(prefs::kColorScheme), QLatin1String(kDefaultColorScheme))
        .toString();
}

/// Schemes compiled into the app (resources/color-schemes, qt-libadwaita's kde-colors): id ->
/// resource path.
[[maybe_unused]] QHash<QString, QString> bundledSchemes()
{
    QHash<QString, QString> paths;
    // Ours, then qt-libadwaita's (Libadwaita, Libadwaita Dark and their high-contrast forms),
    // so the Adwaita look has its own colours built in.
    initAdwaitaResources();
    for (const QString& dir :
        {QStringLiteral(":/color-schemes"), QStringLiteral(":/qt-libadwaita/kde-colors")}) {
        const QFileInfoList files
            = QDir(dir).entryInfoList({QStringLiteral("*.colors")}, QDir::Files);
        for (const QFileInfo& file : files)
            paths.insert(file.completeBaseName(), file.filePath());
    }
    return paths;
}

/// The roles SchemeColors needs that QPalette lacks, read from a .colors file.
[[maybe_unused]] SchemeRoles rolesFromFile(const ColorSchemeFile& file, bool withBackgrounds)
{
    const auto c = [&file](const char* set, const char* key) {
        return file.color(QLatin1String(set), QLatin1String(key));
    };
    SchemeRoles r;
    r.link = c("View", "ForegroundLink");
    r.visited = c("View", "ForegroundVisited");
    r.active = c("View", "ForegroundActive");
    r.inactive = c("View", "ForegroundInactive");
    r.positive = c("View", "ForegroundPositive");
    r.neutral = c("View", "ForegroundNeutral");
    r.negative = c("View", "ForegroundNegative");
    r.focus = c("View", "DecorationFocus");
    r.hover = c("View", "DecorationHover");
    if (withBackgrounds) {
        r.header = c("Header", "BackgroundNormal");
        r.complementary = c("Complementary", "BackgroundNormal");
        r.complementaryText = c("Complementary", "ForegroundNormal");
    }
    return r;
}

/// Small preview icon: window background with a selection-coloured bar (KDE shows similar
/// swatches in its colour scheme menu).
QIcon schemeSwatch(const QPalette& p)
{
    QPixmap pixmap(kSwatchSize, kSwatchSize);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(p.color(QPalette::WindowText), 1));
    painter.setBrush(p.color(QPalette::Window));
    painter.drawRoundedRect(QRectF(0.5, 0.5, kSwatchSize - 1, kSwatchSize - 1), 2, 2);
    painter.fillRect(QRectF(3, 9, kSwatchSize - 6, 4), p.color(QPalette::Highlight));
    return QIcon(pixmap);
}

#ifdef KK_HAVE_KF6
/// KF6: the SchemeColors roles of the scheme in @p config.
SchemeRoles rolesFromConfig(const KSharedConfigPtr& config)
{
    const KColorScheme view(QPalette::Active, KColorScheme::View, config);
    SchemeRoles r;
    r.link = view.foreground(KColorScheme::LinkText).color();
    r.visited = view.foreground(KColorScheme::VisitedText).color();
    r.active = view.foreground(KColorScheme::ActiveText).color();
    r.inactive = view.foreground(KColorScheme::InactiveText).color();
    r.positive = view.foreground(KColorScheme::PositiveText).color();
    r.neutral = view.foreground(KColorScheme::NeutralText).color();
    r.negative = view.foreground(KColorScheme::NegativeText).color();
    r.focus = view.decoration(KColorScheme::FocusColor).color();
    r.hover = view.decoration(KColorScheme::HoverColor).color();
    r.header = KColorScheme(QPalette::Active, KColorScheme::Header, config).background().color();
    const KColorScheme complementary(QPalette::Active, KColorScheme::Complementary, config);
    r.complementary = complementary.background().color();
    r.complementaryText = complementary.foreground().color();
    return r;
}

/// KF6: a compiled-in scheme that KColorSchemeManager's model does not list (KF6 before the
/// model looked into ":/org.kde.kcolorscheme/color-schemes", or a dev build without the
/// scheme on disk) is copied to the cache dir, so KConfig and the Breeze style (which read
/// KDE_COLOR_SCHEME_PATH) get a real file. Returns its path, or empty.
QString bundledSchemeOnDisk(const QString& id)
{
    const QString resource = bundledSchemes().value(id);
    if (resource.isEmpty())
        return {};
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/color-schemes");
    const QString path = dir + QLatin1Char('/') + id + QStringLiteral(".colors");
    QFile source(resource);
    QFile target(path);
    if (!source.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = source.readAll();
    if (target.open(QIODevice::ReadOnly) && target.readAll() == data)
        return path;
    target.close();
    if (!QDir().mkpath(dir) || !target.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || target.write(data) != data.size()) {
        qCWarning(lcTheme) << "cannot write colour scheme" << path;
        return {};
    }
    return path;
}

/// KF6: a compiled-in scheme the model lacks (see bundledSchemeOnDisk()).
bool isUnlistedBundledScheme(const QString& id)
{
    return !id.isEmpty() && !KColorSchemeManager::instance()->indexForSchemeId(id).isValid()
        && bundledSchemes().contains(id);
}
#endif

#ifndef KK_HAVE_KF6
/// Fallback: id -> file of every scheme we can find. Schemes in the XDG data dirs win over
/// the copies compiled into the app (resources/color-schemes), like KDE's own lookup.
const QHash<QString, QString>& fallbackSchemes()
{
    QHash<QString, QString>& paths = state().fallbackSchemePaths;
    if (!paths.isEmpty())
        return paths;
    QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
        QStringLiteral("color-schemes"), QStandardPaths::LocateDirectory);
    dirs.append(QStringLiteral(":/color-schemes"));
    for (const QString& dir : std::as_const(dirs)) {
        const QFileInfoList files
            = QDir(dir).entryInfoList({QStringLiteral("*.colors")}, QDir::Files);
        for (const QFileInfo& file : files) {
            if (!paths.contains(file.completeBaseName()))
                paths.insert(file.completeBaseName(), file.filePath());
        }
    }
    return paths;
}
#endif

/// Applies scheme @p id; an unknown id falls back to the default scheme, then to Breeze Dark,
/// then to the system scheme. Returns the id in use.
QString applyScheme(QApplication& app, const QString& id)
{
    State& s = state();
#ifdef KK_HAVE_KF6
    KColorSchemeManager* manager = KColorSchemeManager::instance();
    manager->setAutosaveChanges(false); // persisted by us (QSettings), not in kdeglobals
    const auto available = [manager](const QString& candidate) {
        return candidate.isEmpty() || manager->indexForSchemeId(candidate).isValid()
            || (isUnlistedBundledScheme(candidate) && !bundledSchemeOnDisk(candidate).isEmpty());
    };
    QString use = id;
    for (const char* fallback : {kDefaultColorScheme, kFallbackColorScheme, ""}) {
        if (available(use))
            break;
        qCWarning(lcTheme) << "colour scheme" << use << "not found, trying" << fallback;
        use = QString::fromLatin1(fallback);
    }
    if (isUnlistedBundledScheme(use)) {
        // Not in KColorSchemeManager's model: apply the cached copy ourselves, the way the
        // manager does (palette + KDE_COLOR_SCHEME_PATH for the Breeze style).
        const QString path = bundledSchemeOnDisk(use);
        const KSharedConfigPtr scheme = KSharedConfig::openConfig(path, KConfig::SimpleConfig);
        QApplication::setPalette(KColorScheme::createApplicationPalette(scheme));
        app.setProperty("KDE_COLOR_SCHEME_PATH", path);
        s.headerBackground
            = KColorScheme(QPalette::Active, KColorScheme::Header, scheme).background().color();
        s.roles = rolesFromConfig(scheme);
        return use;
    }
#if KCOLORSCHEME_VERSION >= QT_VERSION_CHECK(6, 19, 0)
    manager->activateSchemeId(use);
#else
    manager->activateScheme(manager->indexForSchemeId(use));
#endif
    // KColorSchemeManager sets the palette and KDE_COLOR_SCHEME_PATH (read by the Breeze
    // style for its hover/focus colours); the Header colour set is not part of QPalette.
    //
    // "Default (System)" (empty id): KColorSchemeManager falls back to the platform palette,
    // which in the bundle is Qt's "generic" theme (light), while a default-constructed
    // KColorScheme reads the user's kdeglobals (e.g. Breeze Dark on Plasma): dark title bars
    // over light panels in alpha.1. The system scheme is the kdeglobals one, so take palette
    // AND header from the same config; without a kdeglobals both are KDE's defaults.
    if (use.isEmpty()) {
        const KSharedConfigPtr system = KSharedConfig::openConfig(QStringLiteral("kdeglobals"));
        QApplication::setPalette(KColorScheme::createApplicationPalette(system));
        // The Breeze style and default-constructed KColorSchemes read KDE_COLOR_SCHEME_PATH;
        // point it at the same kdeglobals (cleared without one: KDE's defaults everywhere).
        const QString globals = QStandardPaths::locate(
            QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals"));
        app.setProperty(
            "KDE_COLOR_SCHEME_PATH", globals.isEmpty() ? QVariant() : QVariant(globals));
        s.headerBackground
            = KColorScheme(QPalette::Active, KColorScheme::Header, system).background().color();
        s.roles = rolesFromConfig(system);
    } else {
        const QString path
            = manager->indexForSchemeId(use).data(KColorSchemeModel::PathRole).toString();
        const KSharedConfigPtr scheme = path.isEmpty()
            ? KSharedConfigPtr()
            : KSharedConfig::openConfig(path, KConfig::SimpleConfig);
        s.headerBackground
            = KColorScheme(QPalette::Active, KColorScheme::Header, scheme).background().color();
        s.roles = rolesFromConfig(scheme);
    }
    return use;
#else
    QString use = id;
    const QHash<QString, QString>& schemes = fallbackSchemes();
    for (const char* fallback : {kDefaultColorScheme, kFallbackColorScheme, ""}) {
        if (use.isEmpty() || schemes.contains(use))
            break;
        qCWarning(lcTheme) << "colour scheme" << use << "not found, trying" << fallback;
        use = QString::fromLatin1(fallback);
    }
    ColorSchemeFile file;
    if (!use.isEmpty() && file.load(schemes.value(use))) {
        QApplication::setPalette(file.palette());
        s.headerBackground
            = file.color(QStringLiteral("Header"), QStringLiteral("BackgroundNormal"));
        s.roles = rolesFromFile(file, true);
        app.setProperty("KDE_COLOR_SCHEME_PATH", schemes.value(use));
    } else {
        use.clear();
        QApplication::setPalette(s.systemPalette);
        s.headerBackground = QColor();
        // The platform palette has no positive/neutral/negative roles: take them from the
        // Breeze scheme of the same lightness (backgrounds stay the platform's own).
        ColorSchemeFile breeze;
        const bool dark = s.systemPalette.color(QPalette::Window).lightness() < 128;
        s.roles = breeze.load(dark ? QStringLiteral(":/color-schemes/BreezeDark.colors")
                                   : QStringLiteral(":/color-schemes/BreezeLight.colors"))
            ? rolesFromFile(breeze, false)
            : SchemeRoles();
        app.setProperty("KDE_COLOR_SCHEME_PATH", QVariant());
    }
    return use;
#endif
}

/// Breeze icons in the variant that suits the palette. With KF6 the bundled Breeze set is
/// drawn by KIconEngine, which recolours it from the palette, so nothing needs switching.
void applyIconTheme()
{
#ifdef KK_HAVE_KF6
    // Only the Breeze set compiled into the app (":/icons/breeze" from libKF6BreezeIcons):
    // an older Breeze installed on the host (e.g. Breeze 5 in /usr/share/icons on Ubuntu)
    // would otherwise win and the icons would differ per system.
    QStringList paths = QIcon::themeSearchPaths();
    paths.erase(std::remove_if(paths.begin(), paths.end(),
                    [](const QString& p) { return !p.startsWith(QLatin1Char(':')); }),
        paths.end());
    if (paths != QIcon::themeSearchPaths())
        QIcon::setThemeSearchPaths(paths);
#else
    const QString wanted = isDark() ? QStringLiteral("breeze-dark") : QStringLiteral("breeze");
    // Some platform plugins (offscreen, minimal) report no system icon dirs: look the theme
    // up in the XDG data dirs ourselves and add its directory to the search path.
    const QString index = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
        QStringLiteral("icons/") + wanted + QStringLiteral("/index.theme"));
    if (!index.isEmpty()) {
        const QString iconsDir
            = QFileInfo(index).absoluteDir().absolutePath() + QStringLiteral("/..");
        const QString canonical = QDir(iconsDir).canonicalPath();
        QStringList paths = QIcon::themeSearchPaths();
        if (!paths.contains(canonical)) {
            paths.append(canonical);
            QIcon::setThemeSearchPaths(paths);
        }
        QIcon::setThemeName(wanted);
        QIcon::setFallbackThemeName(QStringLiteral("breeze"));
        return;
    }
    QIcon::setThemeName(state().systemIconTheme); // no Breeze: own SVGs fill the gaps
#endif
}

/// Applies widget style @p style and colour scheme @p scheme in the only order that keeps
/// them consistent: scheme (sets KDE_COLOR_SCHEME_PATH), then a NEW style instance (the
/// Breeze style reads KDE's Header colours for menu and tool bars from that path when it is
/// created and does not notice later in-app changes), then the scheme again (a style change
/// resets the application palette). Updates the state's active ids.
/// True for a style that brings its own palette: the optional "Libadwaita" Qt style
/// (qt-libadwaita, loaded from the user's plugin folder when installed) follows GNOME's light /
/// dark setting by itself, so the colour scheme only feeds the custom-painted roles. Its
/// ADWAITA_KEEP_APP_PALETTE=1 switch keeps our scheme instead.
bool styleOwnsPalette(const QString& style)
{
    return isAdwaita(style) && qEnvironmentVariableIntValue("ADWAITA_KEEP_APP_PALETTE") == 0;
}

void applyStyleAndScheme(QApplication& app, const QString& style, const QString& scheme,
    bool keepStyle = false)
{
    State& s = state();
    s.activeScheme = applyScheme(app, scheme);
    if (!keepStyle) {
        s.activeStyle = applyStyle(style);
        s.activeScheme = applyScheme(app, s.activeScheme);
    } // else: the style follows the palette by itself, no re-polish of every widget
    if (styleOwnsPalette(s.activeStyle))
        QApplication::setPalette(QApplication::style()->standardPalette());
}

/// True for styles that read the application palette each time they paint (Kinoklip, Fusion),
/// so a colour scheme switch does not need a new style instance. Breeze reads the scheme
/// file at creation and does.
bool styleFollowsPalette(const QString& style)
{
    return style.compare(KinoklipStyle::key(), Qt::CaseInsensitive) == 0
        || style.compare(QLatin1String("Fusion"), Qt::CaseInsensitive) == 0;
}

void refreshChrome(QApplication& app)
{
    uifonts::apply(app);
    applyIconTheme();
    if (!app.styleSheet().isEmpty())
        app.setStyleSheet(QString()); // the chrome paints itself (ChromeBar)
    // Last: custom-painted widgets repaint from the new roles (SchemeColorsNotifier).
    setSchemeRoles(state().roles);
    // A widget with an own style sheet keeps the palette its sheet derived when it was applied
    // (and "palette(...)" colours in it), so it does not follow the new scheme by itself. The
    // application style sheet used to re-polish everything and hide this; now only the few
    // widgets that have a sheet of their own are polished again.
    const QWidgetList all = QApplication::allWidgets();
    for (QWidget* w : all) {
        const QString sheet = w->styleSheet();
        if (sheet.isEmpty())
            continue;
        w->setStyleSheet(QString());
        w->setStyleSheet(sheet);
    }
}

void syncActionChecks()
{
    State& s = state();
    const auto check = [](QActionGroup* group, const QString& id) {
        if (!group)
            return;
        for (QAction* action : group->actions())
            action->setChecked(action->objectName() == id);
    };
    check(s.schemeActions, colorSchemeActionId(s.activeScheme));
    check(s.styleActions, styleActionId(s.activeStyle));
}

QApplication* application()
{
    return qobject_cast<QApplication*>(QCoreApplication::instance());
}

// ---- Settings page --------------------------------------------------------------------

/// Settings page "appearance": colour theme and widget style. Applies by triggering the
/// registered actions, so the Workspace menu and the stored values cannot disagree.
class AppearancePage final : public SettingsPage
{
public:
    AppearancePage(ActionRegistry& registry, QWidget* parent)
        : SettingsPage(parent)
        , m_registry(registry)
    {
        auto* form = new QFormLayout(this);
        m_scheme = new QComboBox;
        m_scheme->setObjectName(QStringLiteral("ColorSchemeCombo"));
        for (const ColorSchemeInfo& scheme : availableColorSchemes()) {
            const QString actionId = colorSchemeActionId(scheme.id);
            const QAction* action
                = registry.contains(actionId) ? registry.action(actionId) : nullptr;
            m_scheme->addItem(action ? action->icon() : QIcon(), scheme.name, actionId);
        }
        m_scheme->setCurrentIndex(
            std::max(0, m_scheme->findData(colorSchemeActionId(currentColorScheme()))));
        m_scheme->setToolTip(QCoreApplication::translate("kk::theme",
            "The colours of the whole window. Default (System) follows the desktop's colour "
            "scheme; the others are fixed colour schemes."));
        form->addRow(QCoreApplication::translate("kk::theme", "Color theme"), m_scheme);

        m_style = new QComboBox;
        m_style->setObjectName(QStringLiteral("WidgetStyleCombo"));
        for (const QString& style : availableStyles())
            m_style->addItem(styleLabel(style), styleActionId(style));
        m_style->setCurrentIndex(std::max(0, m_style->findData(styleActionId(currentStyle()))));
        m_style->setToolTip(QCoreApplication::translate("kk::theme",
            "How buttons, menus and other controls are drawn. Kinoklip is the flat, dark look "
            "of DaVinci Resolve, the same on every desktop; Breeze is the KDE look, Fusion a "
            "neutral look."));
        form->addRow(QCoreApplication::translate("kk::theme", "Widget style"), m_style);
        m_note = new QLabel;
        m_note->setObjectName(QStringLiteral("WidgetStyleNote"));
        m_note->setWordWrap(true);
        m_note->setText(styleNote());
        m_note->setVisible(!m_note->text().isEmpty());
        form->addRow(QString(), m_note);
    }

    void apply() override
    {
        for (const QComboBox* combo : {m_scheme, m_style}) {
            const QString id = combo->currentData().toString();
            QAction* action = m_registry.contains(id) ? m_registry.action(id) : nullptr;
            if (action && !action->isChecked())
                action->trigger();
        }
        m_note->setText(styleNote());
        m_note->setVisible(!m_note->text().isEmpty());
    }

private:
    ActionRegistry& m_registry;
    QComboBox* m_scheme = nullptr;
    QComboBox* m_style = nullptr;
    QLabel* m_note = nullptr;
};

} // namespace

bool haveKdeFrameworks()
{
#ifdef KK_HAVE_KF6
    return true;
#else
    return false;
#endif
}

void initBeforeApplication()
{
#ifdef KK_HAVE_KF6
    // Uses the Breeze icons compiled into libKF6BreezeIcons (no icon theme on disk needed)
    // through KIconEngine, which recolours them from the active palette: the Kdenlive setup
    // for Windows/macOS, and the same on every Linux distribution.
    KIconTheme::initTheme();
#endif
}

void setSessionOverrides(const QString& style, const QString& colorScheme)
{
    State& s = state();
    s.styleOverride = style;
    s.schemeOverride = colorScheme;
    // "--color-scheme system" selects the system scheme for the run
    if (colorScheme == QLatin1String(kSystemActionSuffix))
        s.schemeOverride.clear();
    s.hasSchemeOverride = !colorScheme.isEmpty();
}

void apply(QApplication& app)
{
    State& s = state();
    if (!s.captured) {
        s.systemPalette = QApplication::palette();
        s.systemIconTheme = QIcon::themeName();
        s.captured = true;
    }
    applyStyleAndScheme(app, storedStyle(), storedScheme());
    refreshChrome(app);
    // Preferences > UI Settings > Interface font: font and style sheet follow at once.
    uifonts::setChangedHandler([] {
        if (QApplication* a = application())
            refreshChrome(*a);
    });
    syncActionChecks();
    instantpopups::install(app); // menus and popups open and close at once
    combopopups::install(app); // combo lists are as wide as their widest entry
    qCInfo(lcTheme) << "style" << s.activeStyle << "colour scheme"
                    << (s.activeScheme.isEmpty() ? QStringLiteral("(system)") : s.activeScheme)
                    << "icon theme" << QIcon::themeName()
                    << (haveKdeFrameworks() ? "(KF6)" : "(fallback)");
}

QStringList availableStyles()
{
    QStringList keys = QStyleFactory::keys();
    keys.removeAll(KinoklipStyle::key());
    keys.prepend(KinoklipStyle::key());
    keys.removeIf([](const QString& k) { return isAdwaita(k); }); // an installed plugin copy
    keys.append(QStringLiteral("Libadwaita"));
    keys.removeIf([](const QString& k) { return isSystemStyle(k); });
    keys.append(QString::fromLatin1(kSystemStyle));
    // Kinoklip first, then Breeze, then Fusion, then the rest alphabetically.
    std::sort(keys.begin(), keys.end(), [](const QString& a, const QString& b) {
        const auto rank = [](const QString& k) {
            return k.compare(KinoklipStyle::key(), Qt::CaseInsensitive) == 0 ? 0
                : k.compare(QLatin1String("Breeze"), Qt::CaseInsensitive) == 0 ? 1
                : k.compare(QLatin1String("Fusion"), Qt::CaseInsensitive) == 0  ? 2
                : isSystemStyle(k)                                              ? 3
                                                                                : 4;
        };
        return rank(a) != rank(b) ? rank(a) < rank(b) : a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return keys;
}

QString styleDisplayName(const QString& key)
{
    return styleLabel(key);
}

QString styleNote()
{
    return isSystemStyle(state().activeStyle) ? state().styleNote : QString();
}

QString currentStyle()
{
    return state().activeStyle;
}

void setStyle(const QString& style)
{
    QSettings().setValue(QLatin1String(prefs::kWidgetStyle), style);
    state().styleOverride.clear();
    QApplication* app = application();
    if (!app)
        return;
    applyStyleAndScheme(*app, style, state().activeScheme);
    refreshChrome(*app);
    syncActionChecks();
}

QVector<ColorSchemeInfo> availableColorSchemes()
{
    QVector<ColorSchemeInfo> schemes;
    const QString systemName = QCoreApplication::translate("kk::theme", "Default (System)");
#ifdef KK_HAVE_KF6
    const QAbstractItemModel* model = KColorSchemeManager::instance()->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        const QString id = index.data(KColorSchemeModel::IdRole).toString();
        schemes.append(
            {id, id.isEmpty() ? systemName : index.data(KColorSchemeModel::NameRole).toString()});
    }
    const QHash<QString, QString> bundled = bundledSchemes();
    for (auto it = bundled.cbegin(); it != bundled.cend(); ++it) {
        ColorSchemeFile file;
        if (isUnlistedBundledScheme(it.key()) && file.load(it.value()))
            schemes.append({it.key(), file.name()});
    }
#else
    schemes.append({QString(), systemName});
    const QHash<QString, QString>& files = fallbackSchemes();
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        ColorSchemeFile file;
        if (file.load(it.value()))
            schemes.append({it.key(), file.name()});
    }
#endif
    // system entry first, then by display name
    std::sort(
        schemes.begin(), schemes.end(), [](const ColorSchemeInfo& a, const ColorSchemeInfo& b) {
            if (a.id.isEmpty() != b.id.isEmpty())
                return a.id.isEmpty();
            return a.name.localeAwareCompare(b.name) < 0;
        });
    return schemes;
}

QString currentColorScheme()
{
    return state().activeScheme;
}

void setColorScheme(const QString& id)
{
    QSettings().setValue(QLatin1String(prefs::kColorScheme), id);
    QSettings().remove(QLatin1String(prefs::kUseSystemColors)); // superseded
    state().hasSchemeOverride = false;
    QApplication* app = application();
    if (!app)
        return;
    applyStyleAndScheme(*app, state().activeStyle, id, styleFollowsPalette(state().activeStyle));
    refreshChrome(*app);
    syncActionChecks();
}

bool isDark()
{
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

QString colorSchemeActionId(const QString& id)
{
    return QLatin1String(kSchemeCategory) + QLatin1Char('.')
        + (id.isEmpty() ? QString::fromLatin1(kSystemActionSuffix) : id);
}

QString styleActionId(const QString& style)
{
    return QLatin1String(kStyleCategory) + QLatin1Char('.') + style.toLower();
}

void registerActions(ActionRegistry& registry)
{
    State& s = state();
    auto* schemes = new QActionGroup(&registry);
    schemes->setExclusive(true);
#ifdef KK_HAVE_KF6
    const QAbstractItemModel* model = KColorSchemeManager::instance()->model();
#endif
    for (const ColorSchemeInfo& scheme : availableColorSchemes()) {
        QAction* action = registry.add(
            colorSchemeActionId(scheme.id), scheme.name, QLatin1String(kSchemeCategory), {}, true);
#ifdef KK_HAVE_KF6
        // KColorSchemeModel provides KDE's own preview icon for each scheme.
        const QModelIndex index = KColorSchemeManager::instance()->indexForSchemeId(scheme.id);
        ColorSchemeFile file;
        if (index.isValid())
            action->setIcon(model->data(index, KColorSchemeModel::IconRole).value<QIcon>());
        else if (file.load(bundledSchemes().value(scheme.id)))
            action->setIcon(schemeSwatch(file.palette()));
#else
        ColorSchemeFile file;
        if (!scheme.id.isEmpty() && file.load(fallbackSchemes().value(scheme.id)))
            action->setIcon(schemeSwatch(file.palette()));
#endif
        schemes->addAction(action);
        const QString id = scheme.id;
        QObject::connect(action, &QAction::triggered, action, [id] { setColorScheme(id); });
    }

    auto* styles = new QActionGroup(&registry);
    styles->setExclusive(true);
    for (const QString& style : availableStyles()) {
        QAction* action
            = registry.add(styleActionId(style), styleLabel(style), QLatin1String(kStyleCategory), {}, true);
        styles->addAction(action);
        QObject::connect(action, &QAction::triggered, action, [style] { setStyle(style); });
    }
    s.schemeActions = schemes;
    s.styleActions = styles;
    syncActionChecks();
}

void registerSettingsPage(SettingsPageRegistry& pages, ActionRegistry& registry)
{
    ActionRegistry* actions = &registry;
    // Preferences > User > UI Settings.
    SettingsPageInfo page {QStringLiteral("appearance"),
        QCoreApplication::translate("kk::theme", "Appearance"),
        QStringLiteral("preferences-desktop-theme-global"), 30,
        [actions](QWidget* parent) { return new AppearancePage(*actions, parent); },
        QStringLiteral("user"), QStringLiteral("ui")};
    pages.add(std::move(page));
}

} // namespace kk::theme
