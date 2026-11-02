// SPDX-License-Identifier: GPL-3.0-or-later
// Theme implementation: Breeze style + KColorSchemeManager (KF6) or a palette fallback,
// icon theme selection and the generated, palette-derived panel style sheet.
#include "app/Theme.h"

#include "app/ColorSchemeFile.h"
#include "app/MenuMetricsStyle.h"
#include "core/Preferences.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/SettingsPages.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDir>
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
#include <kcolorscheme_version.h>
#endif

#include <algorithm>

namespace kk::theme {

namespace {

Q_LOGGING_CATEGORY(lcTheme, "kk.app.theme")

constexpr char kFallbackStyle[] = "Fusion";
constexpr char kSchemeCategory[] = "settings.color_scheme";
constexpr char kStyleCategory[] = "settings.style";
constexpr char kSystemActionSuffix[] = "system";
constexpr int kSwatchSize = 16; ///< px, colour scheme preview icon in the fallback build
/// Weight of the text colour in separators/borders. Breeze draws frames and separators with
/// KColorUtils::mix(background, text, 0.2); using the same keeps our chrome lines identical.
constexpr double kSeparatorMix = 0.2;

/// Everything the theme remembers between calls (GUI thread only).
struct State
{
    QString styleOverride; ///< --style, this run only
    QString schemeOverride; ///< --color-scheme, this run only
    bool hasSchemeOverride = false;
    QString activeStyle; ///< key of the style in use
    QString activeScheme; ///< id of the scheme in use ("" = system)
    QPalette systemPalette; ///< platform palette before the first apply (fallback "system")
    QString systemIconTheme;
    bool captured = false;
    QPointer<QActionGroup> schemeActions;
    QPointer<QActionGroup> styleActions;
    QHash<QString, QString> fallbackSchemePaths; ///< id -> .colors path (fallback build)
    QColor headerBackground; ///< KDE "Header" colour set background of the active scheme
};

State& state()
{
    static State s;
    return s;
}

QColor mix(const QColor& a, const QColor& b, double amount)
{
    const auto lerp = [amount](int x, int y) { return int(x + (y - x) * amount + 0.5); };
    return QColor(lerp(a.red(), b.red()), lerp(a.green(), b.green()), lerp(a.blue(), b.blue()));
}

QString css(const QColor& c)
{
    return c.name(QColor::HexArgb);
}

// ---- Style sheet ----------------------------------------------------------------------
// Only our own chrome (object names from PanelFrame, InterfaceBar, PageBar) gets rules, and
// only backgrounds, borders and font weights: buttons, tabs, menus, scroll bars, focus rings
// and selections are all drawn by the widget style (Breeze), never by QSS.
QString styleSheet(const QPalette& p)
{
    const QColor window = p.color(QPalette::Window);
    const QColor text = p.color(QPalette::WindowText);
    const QColor separator = mix(window, text, kSeparatorMix);
    QColor header = state().headerBackground;
    if (!header.isValid())
        header = isDark() ? window.lighter(115) : window.darker(103);

    QString sheet = QStringLiteral(R"(
QSplitter::handle { background: %1; }
QFrame#PanelFrame { background: %2; border: none; }
QWidget#PanelTitleBar { background: %3; border-bottom: 1px solid %1; }
QLabel#PanelTitle { font-weight: 600; }
QLabel#PlaceholderText { color: %4; }
QWidget#InterfaceBar, QWidget#PageBar { background: %3; }
QWidget#InterfaceBar { border-bottom: 1px solid %1; }
QWidget#PageBar { border-top: 1px solid %1; }
QLabel#ProjectTitle { font-weight: 600; }
QLabel#TimecodeLabel { font-family: monospace; font-weight: 600; }
)");
    return sheet.arg(
        css(separator), css(window), css(header), css(p.color(QPalette::PlaceholderText)));
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

QString storedStyle()
{
    if (!state().styleOverride.isEmpty())
        return state().styleOverride;
    return QSettings()
        .value(QLatin1String(prefs::kWidgetStyle), QLatin1String(kDefaultStyle))
        .toString();
}

/// Sets the style; falls back to Breeze, then Fusion. Returns the key in use.
QString applyStyle(const QString& wanted)
{
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
#endif

/// Applies scheme @p id (unknown ids fall back to the default scheme). Returns the id in use.
QString applyScheme(QApplication& app, const QString& id)
{
    State& s = state();
#ifdef KK_HAVE_KF6
    KColorSchemeManager* manager = KColorSchemeManager::instance();
    manager->setAutosaveChanges(false); // persisted by us (QSettings), not in kdeglobals
    QString use = id;
    if (!use.isEmpty() && !manager->indexForSchemeId(use).isValid()) {
        qCWarning(lcTheme) << "colour scheme" << use << "not found, using" << kDefaultColorScheme;
        use = QLatin1String(kDefaultColorScheme);
        if (!manager->indexForSchemeId(use).isValid())
            use.clear();
    }
#if KCOLORSCHEME_VERSION >= QT_VERSION_CHECK(6, 19, 0)
    manager->activateSchemeId(use);
#else
    manager->activateScheme(manager->indexForSchemeId(use));
#endif
    // KColorSchemeManager sets the palette and KDE_COLOR_SCHEME_PATH (read by the Breeze
    // style for its hover/focus colours); the Header colour set is not part of QPalette.
    s.headerBackground = KColorScheme(QPalette::Active, KColorScheme::Header).background().color();
    Q_UNUSED(app);
    return use;
#else
    QString use = id;
    const QHash<QString, QString>& schemes = fallbackSchemes();
    if (!use.isEmpty() && !schemes.contains(use)) {
        qCWarning(lcTheme) << "colour scheme" << use << "not found, using" << kDefaultColorScheme;
        use = schemes.contains(QLatin1String(kDefaultColorScheme))
            ? QString::fromLatin1(kDefaultColorScheme)
            : QString();
    }
    ColorSchemeFile file;
    if (!use.isEmpty() && file.load(schemes.value(use))) {
        QApplication::setPalette(file.palette());
        s.headerBackground
            = file.color(QStringLiteral("Header"), QStringLiteral("BackgroundNormal"));
        app.setProperty("KDE_COLOR_SCHEME_PATH", schemes.value(use));
    } else {
        use.clear();
        QApplication::setPalette(s.systemPalette);
        s.headerBackground = QColor();
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

void refreshChrome(QApplication& app)
{
    applyIconTheme();
    app.setStyleSheet(styleSheet(QApplication::palette()));
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
/// registered actions, so the Settings menu and the stored values cannot disagree.
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
        form->addRow(QCoreApplication::translate("kk::theme", "Color theme"), m_scheme);

        m_style = new QComboBox;
        m_style->setObjectName(QStringLiteral("WidgetStyleCombo"));
        for (const QString& style : availableStyles())
            m_style->addItem(style, styleActionId(style));
        m_style->setCurrentIndex(std::max(0, m_style->findData(styleActionId(currentStyle()))));
        form->addRow(QCoreApplication::translate("kk::theme", "Widget style"), m_style);
    }

    void apply() override
    {
        for (const QComboBox* combo : {m_scheme, m_style}) {
            const QString id = combo->currentData().toString();
            QAction* action = m_registry.contains(id) ? m_registry.action(id) : nullptr;
            if (action && !action->isChecked())
                action->trigger();
        }
    }

private:
    ActionRegistry& m_registry;
    QComboBox* m_scheme = nullptr;
    QComboBox* m_style = nullptr;
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
    s.activeStyle = applyStyle(storedStyle());
    s.activeScheme = applyScheme(app, storedScheme());
    refreshChrome(app);
    syncActionChecks();
    qCInfo(lcTheme) << "style" << s.activeStyle << "colour scheme"
                    << (s.activeScheme.isEmpty() ? QStringLiteral("(system)") : s.activeScheme)
                    << "icon theme" << QIcon::themeName()
                    << (haveKdeFrameworks() ? "(KF6)" : "(fallback)");
}

QStringList availableStyles()
{
    QStringList keys = QStyleFactory::keys();
    // Breeze first, then Fusion, then the rest alphabetically.
    std::sort(keys.begin(), keys.end(), [](const QString& a, const QString& b) {
        const auto rank = [](const QString& k) {
            return k.compare(QLatin1String("Breeze"), Qt::CaseInsensitive) == 0 ? 0
                : k.compare(QLatin1String("Fusion"), Qt::CaseInsensitive) == 0  ? 1
                                                                                : 2;
        };
        return rank(a) != rank(b) ? rank(a) < rank(b) : a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return keys;
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
    state().activeStyle = applyStyle(style);
    // A style change resets the palette to the style's standard one: re-apply the scheme.
    state().activeScheme = applyScheme(*app, state().activeScheme);
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
    state().activeScheme = applyScheme(*app, id);
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
        if (index.isValid())
            action->setIcon(model->data(index, KColorSchemeModel::IconRole).value<QIcon>());
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
            = registry.add(styleActionId(style), style, QLatin1String(kStyleCategory), {}, true);
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
    pages.add({QStringLiteral("appearance"), QCoreApplication::translate("kk::theme", "Appearance"),
        QStringLiteral("preferences-desktop-theme-global"), 30,
        [actions](QWidget* parent) { return new AppearancePage(*actions, parent); }});
}

} // namespace kk::theme
