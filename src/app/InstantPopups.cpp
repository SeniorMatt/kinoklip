// SPDX-License-Identifier: GPL-3.0-or-later
// InstantPopups implementation: Qt effects off, an application filter that marks popup
// windows for the compositor (X11 property / DWM attribute, resolved at run time).
#include "app/InstantPopups.h"

#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QLibrary>
#include <QPointer>
#include <QWidget>
#include <QWindow>

#if defined(QT_FEATURE_xcb) && QT_FEATURE_xcb == 1 && __has_include(<xcb/xcb.h>)
#include <cstdlib>
#include <cstring>
#include <xcb/xcb.h>
#define KK_INSTANT_POPUPS_XCB 1
#endif

namespace kk::instantpopups {

namespace {

constexpr Qt::UIEffect kPopupEffects[] = {Qt::UI_AnimateMenu, Qt::UI_FadeMenu, Qt::UI_AnimateCombo,
    Qt::UI_AnimateTooltip, Qt::UI_FadeTooltip};

/// Dynamic property: the window was already marked (each popup is marked once).
constexpr char kMarked[] = "_kk_instant_popup";

bool isPopupWindow(const QWidget* w)
{
    if (!w || !w->isWindow())
        return false;
    const Qt::WindowType type = w->windowType();
    return type == Qt::Popup || type == Qt::ToolTip;
}

/// Marks popup windows when they are shown (QMenu, combo-box lists, completer lists, the
/// node library, tooltips): the native window exists by then and is not mapped yet.
class PopupFilter : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Show && watched->isWidgetType()) {
            auto* w = static_cast<QWidget*>(watched);
            if (isPopupWindow(w) && !w->property(kMarked).toBool()) {
                w->setProperty(kMarked, true);
                markWindow(w);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

QPointer<PopupFilter> g_filter;

#ifdef KK_INSTANT_POPUPS_XCB
/// libxcb resolved at run time (already loaded by Qt's xcb plugin), so the app does not link
/// it and runs where only Wayland is present.
struct Xcb
{
    using InternAtom
        = xcb_intern_atom_cookie_t (*)(xcb_connection_t*, uint8_t, uint16_t, const char*);
    using InternAtomReply = xcb_intern_atom_reply_t* (*)(xcb_connection_t*,
        xcb_intern_atom_cookie_t, xcb_generic_error_t**);
    using ChangeProperty = xcb_void_cookie_t (*)(xcb_connection_t*, uint8_t, xcb_window_t,
        xcb_atom_t, xcb_atom_t, uint8_t, uint32_t, const void*);

    InternAtom internAtom = nullptr;
    InternAtomReply internAtomReply = nullptr;
    ChangeProperty changeProperty = nullptr;
    xcb_atom_t skipCloseAnimation = XCB_ATOM_NONE;
    bool tried = false;
};

Xcb& xcb()
{
    static Xcb x;
    return x;
}

bool markX11(QWidget* popup)
{
    auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11 || !x11->connection())
        return false;
    xcb_connection_t* conn = x11->connection();
    Xcb& x = xcb();
    if (!x.tried) {
        x.tried = true;
        QLibrary lib(QStringLiteral("xcb"), 1);
        if (lib.load()) {
            x.internAtom = reinterpret_cast<Xcb::InternAtom>(lib.resolve("xcb_intern_atom"));
            x.internAtomReply
                = reinterpret_cast<Xcb::InternAtomReply>(lib.resolve("xcb_intern_atom_reply"));
            x.changeProperty
                = reinterpret_cast<Xcb::ChangeProperty>(lib.resolve("xcb_change_property"));
        }
        if (x.internAtom && x.internAtomReply && x.changeProperty) {
            static constexpr char name[] = "_KDE_NET_WM_SKIP_CLOSE_ANIMATION";
            const xcb_intern_atom_cookie_t cookie
                = x.internAtom(conn, 0, uint16_t(std::strlen(name)), name);
            if (xcb_intern_atom_reply_t* reply = x.internAtomReply(conn, cookie, nullptr)) {
                x.skipCloseAnimation = reply->atom;
                std::free(reply);
            }
        }
    }
    if (x.skipCloseAnimation == XCB_ATOM_NONE || !popup->windowHandle())
        return false;
    const uint32_t on = 1;
    x.changeProperty(conn, XCB_PROP_MODE_REPLACE, xcb_window_t(popup->windowHandle()->winId()),
        x.skipCloseAnimation, XCB_ATOM_CARDINAL, 32, 1, &on);
    return true;
}
#endif

#ifdef Q_OS_WIN
/// dwmapi resolved at run time (no extra link library). DWMWA_TRANSITIONS_FORCEDISABLED = 3.
bool markWindows(QWidget* popup)
{
    using DwmSetWindowAttribute
        = long(__stdcall*)(void*, unsigned long, const void*, unsigned long);
    static const auto set = reinterpret_cast<DwmSetWindowAttribute>(
        QLibrary::resolve(QStringLiteral("dwmapi"), "DwmSetWindowAttribute"));
    if (!set || !popup->windowHandle())
        return false;
    const int disabled = 1; // BOOL TRUE
    return set(reinterpret_cast<void*>(popup->windowHandle()->winId()), 3, &disabled,
               sizeof(disabled))
        >= 0;
}
#endif

} // namespace

void install(QApplication& app)
{
    for (const Qt::UIEffect effect : kPopupEffects)
        QApplication::setEffectEnabled(effect, false);
    if (!g_filter) {
        g_filter = new PopupFilter(&app);
        app.installEventFilter(g_filter);
    }
}

bool effectsOff()
{
    for (const Qt::UIEffect effect : kPopupEffects) {
        if (QApplication::isEffectEnabled(effect))
            return false;
    }
    return true;
}

bool markWindow(QWidget* popup)
{
    if (!popup)
        return false;
    const QString platform = QGuiApplication::platformName();
#ifdef KK_INSTANT_POPUPS_XCB
    if (platform == QLatin1String("xcb"))
        return markX11(popup);
#endif
#ifdef Q_OS_WIN
    if (platform == QLatin1String("windows"))
        return markWindows(popup);
#endif
    Q_UNUSED(platform);
    return false;
}

} // namespace kk::instantpopups
