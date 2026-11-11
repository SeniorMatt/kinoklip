// SPDX-License-Identifier: GPL-3.0-or-later
// DialogSizing implementation.
#include "ui/common/DialogSizing.h"

#include <QDataStream>
#include <QGuiApplication>
#include <QLayout>
#include <QRect>
#include <QScreen>
#include <QWidget>

#include <algorithm>

namespace kk {

PageScrollArea::PageScrollArea(QWidget* parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
}

QSize PageScrollArea::sizeHint() const
{
    if (!widget())
        return QScrollArea::sizeHint();
    return widget()->sizeHint().expandedTo(widget()->minimumSizeHint())
        + QSize(2 * frameWidth(), 2 * frameWidth());
}

namespace dialogsizing {

namespace {
constexpr int kFrameAllowance = 60; ///< title bar, window frame, panels at the screen edge
QSize g_testScreen;
} // namespace

void setScreenSizeForTesting(QSize size)
{
    g_testScreen = size;
}

QSize screenLimit(const QWidget* window)
{
    if (g_testScreen.isValid())
        return QSize(
            g_testScreen.width() - kFrameAllowance, g_testScreen.height() - kFrameAllowance);
    const QScreen* screen = nullptr;
    if (window) {
        if (const QWidget* parent = window->parentWidget())
            screen = parent->screen();
        if (!screen)
            screen = window->screen();
    }
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QSize(1024, 700);
    const QSize available = screen->availableGeometry().size();
    return QSize(std::max(320, available.width() - kFrameAllowance),
        std::max(240, available.height() - kFrameAllowance));
}

QSize fit(QWidget* window, QSize preferred)
{
    const QSize limit = screenLimit(window);
    QSize need = window->sizeHint();
    if (QLayout* layout = window->layout())
        need = need.expandedTo(layout->totalSizeHint());
    need = need.expandedTo(window->minimumSizeHint()).expandedTo(window->minimumSize());
    const QSize minimum = need.boundedTo(limit);
    window->setMinimumSize(minimum);
    const QSize initial = need.expandedTo(preferred).boundedTo(limit);
    window->resize(initial);
    return initial;
}

namespace {
/// The size of the normal (not maximized) window stored in a QWidget::saveGeometry() blob; an
/// invalid size when the blob is not in the known format.
QSize savedNormalSize(const QByteArray& state)
{
    QDataStream in(state);
    quint32 magic = 0;
    quint16 major = 0;
    quint16 minor = 0;
    QRect frame;
    QRect normal;
    in >> magic >> major >> minor;
    if (in.status() != QDataStream::Ok || magic != 0x1D9D0CB || major < 1)
        return {};
    in >> frame >> normal;
    return in.status() == QDataStream::Ok ? normal.size() : QSize();
}
} // namespace

bool restoreGeometryIfFits(QWidget* window, const QByteArray& state, QSize fallback)
{
    const QSize minimum = window->minimumSize();
    const QSize before = window->size();
    const QSize saved = savedNormalSize(state);
    const bool tooSmall
        = saved.isValid() && (saved.width() < minimum.width() || saved.height() < minimum.height());
    if (!state.isEmpty() && !tooSmall && window->restoreGeometry(state))
        return true;
    window->resize(fallback.isValid() ? fallback : before);
    return false;
}

} // namespace dialogsizing
} // namespace kk
