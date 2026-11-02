// SPDX-License-Identifier: GPL-3.0-or-later
// ColorSchemeFile implementation: a minimal INI reader plus KColorScheme's palette mapping.
#include "app/ColorSchemeFile.h"

#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace kk::theme {

namespace {

/// How far disabled text moves towards the background (KDE's default ContrastAmount 0.65
/// in [ColorEffects:Disabled] makes disabled text roughly this faint).
constexpr double kDisabledTextMix = 0.55;

QColor mix(const QColor& a, const QColor& b, double amount)
{
    const auto lerp = [amount](int x, int y) { return int(x + (y - x) * amount + 0.5); };
    return QColor(lerp(a.red(), b.red()), lerp(a.green(), b.green()), lerp(a.blue(), b.blue()));
}

/// "61,174,233" -> colour (invalid for anything else).
QColor parseRgb(const QString& value)
{
    const QStringList parts = value.split(QLatin1Char(','));
    if (parts.size() < 3)
        return {};
    bool ok[3] = {};
    const int r = parts[0].trimmed().toInt(&ok[0]);
    const int g = parts[1].trimmed().toInt(&ok[1]);
    const int b = parts[2].trimmed().toInt(&ok[2]);
    return ok[0] && ok[1] && ok[2] ? QColor(r, g, b) : QColor();
}

} // namespace

bool ColorSchemeFile::load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    m_id = QFileInfo(path).completeBaseName();
    m_name.clear();
    m_colors.clear();

    // Groups look like "[Colors:Window]" or "[Colors:Header][Inactive]"; only the plain
    // "[Colors:<set>]" groups and [General] matter for an application palette.
    QString set; // current colour set, empty outside a plain Colors group
    bool general = false;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('['))) {
            general = line == QLatin1String("[General]");
            set.clear();
            if (line.startsWith(QLatin1String("[Colors:")) && line.count(QLatin1Char('[')) == 1)
                set = line.mid(8, line.size() - 9);
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        if (general && key == QLatin1String("Name")) // untranslated name only (no "Name[de]")
            m_name = value;
        else if (!set.isEmpty())
            m_colors.insert(set + QLatin1Char('/') + key, parseRgb(value));
    }
    return color(QStringLiteral("Window"), QStringLiteral("BackgroundNormal")).isValid();
}

QColor ColorSchemeFile::color(const QString& set, const QString& key) const
{
    return m_colors.value(set + QLatin1Char('/') + key);
}

QPalette ColorSchemeFile::palette() const
{
    // Same role mapping as KColorScheme::createApplicationPalette(); the 3D shades are
    // approximated from the button colour (KColorScheme uses KColorUtils::shade with the
    // user's contrast setting, which we do not have without KF6).
    const auto c = [this](const char* set, const char* key, const QColor& fallback = {}) {
        const QColor value = color(QLatin1String(set), QLatin1String(key));
        return value.isValid() ? value : fallback;
    };
    const QColor window = c("Window", "BackgroundNormal");
    const QColor windowText = c("Window", "ForegroundNormal", Qt::black);
    const QColor base = c("View", "BackgroundNormal", window);
    const QColor text = c("View", "ForegroundNormal", windowText);
    const QColor button = c("Button", "BackgroundNormal", window);
    const QColor buttonText = c("Button", "ForegroundNormal", windowText);
    const QColor selection = c("Selection", "BackgroundNormal", QColor(61, 174, 233));
    const QColor selectionText = c("Selection", "ForegroundNormal", Qt::white);
    const bool dark = window.lightness() < 128;

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, windowText);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, c("View", "BackgroundAlternate", base));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, buttonText);
    p.setColor(QPalette::Highlight, selection);
    p.setColor(QPalette::HighlightedText, selectionText);
    p.setColor(QPalette::ToolTipBase, c("Tooltip", "BackgroundNormal", window));
    p.setColor(QPalette::ToolTipText, c("Tooltip", "ForegroundNormal", windowText));
    p.setColor(QPalette::Link, c("View", "ForegroundLink", selection));
    p.setColor(QPalette::LinkVisited, c("View", "ForegroundVisited", selection));
    p.setColor(QPalette::PlaceholderText, c("View", "ForegroundInactive", mix(text, base, 0.4)));
    p.setColor(QPalette::BrightText, c("Button", "ForegroundNegative", Qt::red));
    p.setColor(QPalette::Light, dark ? button.lighter(160) : button.lighter(110));
    p.setColor(QPalette::Midlight, dark ? button.lighter(130) : button.lighter(105));
    p.setColor(QPalette::Mid, dark ? button.darker(130) : button.darker(130));
    p.setColor(QPalette::Dark, dark ? button.darker(170) : button.darker(160));
    p.setColor(QPalette::Shadow, dark ? QColor(0, 0, 0) : button.darker(300));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Accent, selection);
#endif

    // Disabled: text faded towards its background, selection greyed out.
    p.setColor(QPalette::Disabled, QPalette::WindowText, mix(windowText, window, kDisabledTextMix));
    p.setColor(QPalette::Disabled, QPalette::Text, mix(text, base, kDisabledTextMix));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, mix(buttonText, button, kDisabledTextMix));
    p.setColor(QPalette::Disabled, QPalette::Highlight, mix(selection, window, 0.6));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, mix(selectionText, window, 0.4));
    return p;
}

} // namespace kk::theme
