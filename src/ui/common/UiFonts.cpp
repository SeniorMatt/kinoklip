// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/UiFonts.h"

#include "core/Preferences.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QFontDatabase>
#include <QPointer>
#include <QWidget>
#include <QSettings>

#include <algorithm>

namespace kk::uifonts {

namespace {

// Sizes in points at 96 DPI (1 pt = 4/3 px), fitted to the native 1920x1017 screenshot of Resolve 17
// (100 % scaling) by ink width and height; all of Resolve's text is regular weight (readme.md).
constexpr double kBodyPt = 9.0; ///< 12 px: menus, toolbar labels, inspector
constexpr double kPanelPt = 9.75; ///< 13 px: panel titles, tree items, section headers, viewer numbers
constexpr double kHeadingPt = 10.5; ///< 14 px
constexpr double kTitlePt = 12.0; ///< 16 px: the project name
constexpr double kSmallPt = 8.25; ///< 11 px: item labels, mixer labels, captions, ruler (10 px in Resolve)
constexpr double kTinyPt = 7.5; ///< 10 px: ruler numbers (Role::Ruler), scales
constexpr double kTimecodePt = 15.75; ///< 21 px: the big timeline timecode

QString& sessionOverride()
{
    static QString id;
    return id;
}

QFont resolveFont(double pt, QFont::Weight weight)
{
    QFont f{QString::fromLatin1(kFamily)};
    f.setPointSizeF(pt);
    f.setWeight(weight);
    // Hinted Medium has the ink density of Resolve's text (within 3 %); Regular is 14 % lighter.
    return f;
}


QFont fontScaled(Role role, Choice which, const QFont& base, double scale)
{
    QFont f = base;
    double pt = 0;
    bool bold = false;
    if (which == Choice::System) {
        const double b = base.pointSizeF() > 0 ? base.pointSizeF() : 9.0;
        switch (role) {
        case Role::Body:
        case Role::SectionHeader:
            pt = b;
            break;
        case Role::Emphasis:
            pt = b;
            bold = true;
            break;
        case Role::PanelTitle:
            pt = b;
            f.setWeight(QFont::DemiBold);
            break;
        case Role::Heading:
            pt = b + 2;
            f.setWeight(QFont::DemiBold);
            break;
        case Role::Title:
            pt = b * 1.8;
            f.setWeight(QFont::DemiBold);
            break;
        case Role::Ruler:
        case Role::Small:
            pt = std::max(6.0, b - 1.5);
            break;
        case Role::SmallEmphasis:
            pt = std::max(6.0, b - 1.5);
            bold = true;
            break;
        case Role::Tiny:
            pt = std::max(6.0, b * 0.8);
            break;
        case Role::Timecode:
            pt = b + 2;
            bold = true;
            break;
        case Role::ViewerTimecode:
            pt = b;
            f.setFamily(QStringLiteral("monospace"));
            f.setWeight(QFont::DemiBold);
            break;
        case Role::Mono:
            return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        }
    } else {
        QFont::Weight w = QFont::Medium; // Resolve's text is heavier than Regular (readme)
        switch (role) {
        case Role::Body: pt = kBodyPt; break;
        case Role::SectionHeader: pt = kPanelPt; break;
        case Role::Emphasis: pt = kBodyPt; w = QFont::Bold; break;
        case Role::PanelTitle: pt = kPanelPt; break;
        case Role::Heading: pt = kHeadingPt; w = QFont::DemiBold; break;
        case Role::Title: pt = kTitlePt; break;
        case Role::Small: pt = kSmallPt; break;
        case Role::Ruler: pt = kTinyPt; break;
        case Role::SmallEmphasis: pt = kSmallPt; w = QFont::Bold; break;
        case Role::Tiny: pt = kTinyPt; break;
        case Role::Timecode: pt = kTimecodePt; break;
        case Role::ViewerTimecode: pt = kPanelPt; break;
        case Role::Mono: return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        }
        f = resolveFont(pt, w);
    }
    if (bold)
        f.setBold(true);
    if (scale != 1.0)
        pt *= scale;
    f.setPointSizeF(std::max(4.0, pt));
    return f;
}

} // namespace

QString choiceId(Choice choice)
{
    return QLatin1String(choice == Choice::System ? kSystemId : kResolveId);
}

Choice choiceFromId(const QString& id)
{
    return id == QLatin1String(kSystemId) ? Choice::System : Choice::Resolve;
}

Choice choice()
{
    if (!sessionOverride().isEmpty())
        return choiceFromId(sessionOverride());
    return choiceFromId(
        QSettings().value(QLatin1String(prefs::kInterfaceFont), QLatin1String(kResolveId)).toString());
}

namespace {
std::function<void()>& changedHandler()
{
    static std::function<void()> handler;
    return handler;
}
} // namespace

void setChangedHandler(std::function<void()> handler)
{
    changedHandler() = std::move(handler);
}

void setChoice(Choice wanted)
{
    const Choice before = choice();
    QSettings().setValue(QLatin1String(prefs::kInterfaceFont), choiceId(wanted));
    sessionOverride().clear();
    if (before != wanted && changedHandler())
        changedHandler()();
}

void setSessionOverride(const QString& id)
{
    const QString lower = id.toLower();
    sessionOverride() = (lower == QLatin1String(kResolveId) || lower == QLatin1String(kSystemId))
        ? lower
        : QString();
}

void loadBundledFonts()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    for (const char* file : {"Regular", "Italic", "Medium", "SemiBold", "Bold"})
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/OpenSans-%1.ttf").arg(QLatin1String(file)));
}

QFont parseKdeFont(const QString& value)
{
    // "Noto Sans,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1" (QFont::toString format, as KDE writes it)
    QFont f;
    if (value.trimmed().isEmpty() || !f.fromString(value.trimmed()) || f.family().isEmpty())
        return {};
    return f;
}

QFont parseGnomeFont(const QString& value)
{
    // gsettings prints 'Cantarell 11' or 'Inter Variable Semi-Bold 11' (Pango description)
    QString v = value.trimmed();
    if (v.size() >= 2 && v.startsWith(QLatin1Char('\'')) && v.endsWith(QLatin1Char('\'')))
        v = v.mid(1, v.size() - 2).trimmed();
    static const QRegularExpression re(QStringLiteral("^(.*?)\\s+([0-9]+(?:\\.[0-9]+)?)$"));
    const auto m = re.match(v);
    if (!m.hasMatch())
        return {};
    QString family = m.captured(1).trimmed();
    QFont::Weight weight = QFont::Normal;
    bool italic = false;
    static const QRegularExpression style(
        QStringLiteral("\\s+(Semi-Bold|Bold|Light|Medium|Italic|Regular|Oblique)$"),
        QRegularExpression::CaseInsensitiveOption);
    for (auto sm = style.match(family); sm.hasMatch(); sm = style.match(family)) {
        const QString w = sm.captured(1).toLower();
        if (w == QLatin1String("semi-bold")) weight = QFont::DemiBold;
        else if (w == QLatin1String("bold")) weight = QFont::Bold;
        else if (w == QLatin1String("light")) weight = QFont::Light;
        else if (w == QLatin1String("medium")) weight = QFont::Medium;
        else if (w == QLatin1String("italic") || w == QLatin1String("oblique")) italic = true;
        family.truncate(sm.capturedStart(0));
    }
    if (family.isEmpty())
        return {};
    QFont f(family);
    f.setPointSizeF(m.captured(2).toDouble());
    f.setWeight(weight);
    f.setItalic(italic);
    return f;
}

QFont desktopFont()
{
    // KDE: kdeglobals [General] font= (what plasma-integration would hand to Qt)
    const bool kde = qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains(QLatin1String("KDE"), Qt::CaseInsensitive);
    const QString home = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    for (const QString& dir : {home, QStringLiteral("/etc/xdg")}) {
        const QString file = QDir(dir).filePath(QStringLiteral("kdeglobals"));
        if (!QFile::exists(file))
            continue;
        const QFont f = parseKdeFont(
            QSettings(file, QSettings::IniFormat).value(QStringLiteral("General/font")).toString());
        if (!f.family().isEmpty())
            return f;
    }
    if (!kde) { // GNOME and the other gsettings desktops
        QProcess p;
        p.start(QStringLiteral("gsettings"),
            {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
                QStringLiteral("font-name")});
        if (p.waitForFinished(700) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) {
            const QFont f = parseGnomeFont(QString::fromUtf8(p.readAllStandardOutput()));
            if (!f.family().isEmpty())
                return f;
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont);
}

QFont systemFont()
{
    // The AppImage runs Qt's "generic" platform theme (apprun.sh), which knows no desktop font:
    // read the desktop's own setting then. With a real platform theme Qt's font is the truth.
    static const QFont captured
        = qEnvironmentVariable("QT_QPA_PLATFORMTHEME") == QLatin1String("generic") ? desktopFont()
                                                                                   : QApplication::font();
    return captured;
}

QFont font(Role role, Choice which, const QFont& base)
{
    return fontScaled(role, which, base, 1.0);
}

QFont font(Role role)
{
    return font(role, choice(), systemFont());
}

QFont font(Role role, const QFont& base, double scale)
{
    return fontScaled(role, choice(), base, scale);
}

QFont italic(QFont f, bool on)
{
    f.setItalic(on);
    return f;
}

QFont withPixelSize(Role role, const QFont& base, int px)
{
    QFont f = font(role, base);
    f.setPixelSize(std::max(1, px));
    return f;
}

QString css(Role role)
{
    const QFont f = font(role, systemFont());
    if (choice() == Choice::System) {
        switch (role) {
        case Role::Emphasis:
        case Role::SmallEmphasis:
            return QStringLiteral("font-weight: bold;");
        case Role::ViewerTimecode:
            return QStringLiteral("font-family: monospace; font-weight: 600;");
        case Role::Body:
        case Role::SectionHeader:
        case Role::Small:
        case Role::Ruler:
        case Role::Tiny:
        case Role::Mono:
            return {};
        default:
            return QStringLiteral("font-weight: 600;");
        }
    }
    QString out = QStringLiteral("font-weight: %1; font-size: %2pt;")
                      .arg(f.weight() >= QFont::Bold ? 700 : f.weight() >= QFont::DemiBold ? 600 : f.weight() >= QFont::Medium ? 500 : 400)
                      .arg(f.pointSizeF());
    return out;
}

namespace {
struct Bound
{
    QPointer<QWidget> widget;
    Role role;
};
QList<Bound>& bound()
{
    static QList<Bound> list;
    return list;
}
void restyle(const Bound& b)
{
    if (!b.widget)
        return;
    const QFont f = font(b.role, systemFont());
    if (b.widget->font() != f)
        b.widget->setFont(f);
}
} // namespace

void bindLabel(QWidget* label, Role role)
{
    bound().removeIf([](const Bound& b) { return b.widget.isNull(); });
    bound().append({label, role});
    restyle(bound().last());
}

void apply(QApplication& app)
{
    loadBundledFonts();
    const QFont system = systemFont(); // before the first change
    const QFont body = font(Role::Body, choice(), system);
    if (app.font() != body)
        app.setFont(body);
    for (const Bound& b : std::as_const(bound()))
        restyle(b);
}

QString styleSheetRules()
{
    // The project name in the top bar is the one place Resolve uses its Title size.
    return QStringLiteral("QLabel#PanelTitle { %1 }\nQLabel#TimecodeLabel { %2 }\n"
                          "QLabel#ProjectTitle { %3 }\n")
               .arg(css(Role::PanelTitle), css(Role::ViewerTimecode), css(Role::Title));
}

} // namespace kk::uifonts
