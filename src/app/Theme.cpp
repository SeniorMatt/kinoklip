// SPDX-License-Identifier: GPL-3.0-or-later
// Dark palette + generated style sheet for the panel chrome.
#include "app/Theme.h"

#include "core/Preferences.h"

#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

namespace kk::theme {

namespace {

// Neutral greys from darkest (gaps between panels) to lightest (hover), one accent.
const QColor kGap(0x141416);
const QColor kBase(0x1c1c1f);
const QColor kWindow(0x242427);
const QColor kAlternate(0x28282c);
const QColor kButton(0x313136);
const QColor kLight(0x46464c);
const QColor kText(0xd6d6da);
const QColor kTextDim(0x8a8a92);
const QColor kTextDisabled(0x66666d);
const QColor kAccent(0x4c9be8);

/// Platform style and palette from before the first apply(), so "Use System Colors" can return
/// to them (on Plasma this is Breeze with the user's colour scheme).
struct SystemLook
{
    QString styleName;
    QPalette palette;
    bool captured = false;
};

SystemLook& systemLook()
{
    static SystemLook look;
    return look;
}

QPalette darkPalette()
{
    QPalette p;
    p.setColor(QPalette::Window, kWindow);
    p.setColor(QPalette::WindowText, kText);
    p.setColor(QPalette::Base, kBase);
    p.setColor(QPalette::AlternateBase, kAlternate);
    p.setColor(QPalette::ToolTipBase, kButton);
    p.setColor(QPalette::ToolTipText, kText);
    p.setColor(QPalette::PlaceholderText, kTextDim);
    p.setColor(QPalette::Text, kText);
    p.setColor(QPalette::Button, kButton);
    p.setColor(QPalette::ButtonText, kText);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Light, kLight);
    p.setColor(QPalette::Midlight, kButton.lighter(115));
    p.setColor(QPalette::Mid, kWindow.darker(120));
    p.setColor(QPalette::Dark, kGap);
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Highlight, kAccent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, kAccent);
    p.setColor(QPalette::LinkVisited, kAccent.darker(120));
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, kTextDisabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, kButton);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, kTextDisabled);
    return p;
}

QString name(const QColor& c)
{
    return c.name(QColor::HexArgb);
}

/// Style sheet for our own chrome only (object names set by PanelFrame, MainWindow, PageBar).
/// Colours are derived from the active palette so it also fits the system theme.
QString styleSheet(const QPalette& p)
{
    const bool dark = p.color(QPalette::Window).lightness() < 128;
    const QColor window = p.color(QPalette::Window);
    const QColor gap = dark ? window.darker(170) : window.darker(125);
    const QColor titleBar = dark ? window.lighter(118) : window.darker(106);
    const QColor bar = dark ? window.darker(125) : window.darker(104);
    const QColor text = p.color(QPalette::WindowText);
    QColor dim = text;
    dim.setAlphaF(0.55f);
    const QColor accent = p.color(QPalette::Highlight);
    QColor hover = text;
    hover.setAlphaF(0.08f);
    QColor checkedBg = accent;
    checkedBg.setAlphaF(0.18f);

    QString css = QStringLiteral(R"(
QMainWindow, QMainWindow > QWidget#CentralWidget { background: %1; }
QSplitter::handle { background: %1; }
QFrame#PanelFrame { background: %2; border: none; }
QWidget#PanelTitleBar { background: %3; border-bottom: 1px solid %1; }
QLabel#PanelTitle { color: %4; font-weight: 600; }
QLabel#PlaceholderText { color: %5; }
QWidget#InterfaceBar, QWidget#PageBar { background: %6; }
QWidget#InterfaceBar { border-bottom: 1px solid %1; }
QWidget#PageBar { border-top: 1px solid %1; }
QLabel#ProjectTitle { color: %4; font-weight: 600; }
QWidget#InterfaceBar QToolButton, QWidget#PageBar QToolButton {
    color: %5; background: transparent; border: none; border-radius: 4px; padding: 3px 8px;
}
QWidget#InterfaceBar QToolButton:hover, QWidget#PageBar QToolButton:hover { background: %7; }
QWidget#InterfaceBar QToolButton:checked, QWidget#PageBar QToolButton:checked {
    color: %4; background: %8;
}
QWidget#PageBar QToolButton:disabled { color: %5; }
QWidget#ViewerCanvas { background: #000000; }
QLabel#TimecodeLabel { color: %9; font-family: monospace; font-weight: 600; }
)");
    css = css.arg(name(gap), name(window), name(titleBar), name(text), name(dim), name(bar),
        name(hover), name(checkedBg), name(accent));
    if (dark) {
        // Fusion's dark menus/tool tips need a little help to look consistent.
        css += QStringLiteral(R"(
QToolTip { color: %1; background: %2; border: 1px solid %3; padding: 3px; }
QMenu::separator { height: 1px; background: %3; margin: 4px 8px; }
)")
                   .arg(name(text), name(kButton), name(kLight));
    }
    return css;
}

} // namespace

QColor accentColor()
{
    return kAccent;
}

bool useSystemColors()
{
    return QSettings().value(QLatin1String(prefs::kUseSystemColors), false).toBool();
}

void setUseSystemColors(bool on)
{
    QSettings().setValue(QLatin1String(prefs::kUseSystemColors), on);
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance()))
        apply(*app);
}

void apply(QApplication& app)
{
    SystemLook& system = systemLook();
    if (!system.captured) {
        system.styleName = app.style()->name();
        system.palette = QApplication::palette();
        system.captured = true;
    }

    if (useSystemColors()) {
        QApplication::setStyle(QStyleFactory::create(system.styleName));
        QApplication::setPalette(system.palette);
    } else {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(darkPalette());
    }
    app.setStyleSheet(styleSheet(QApplication::palette()));
}

} // namespace kk::theme
