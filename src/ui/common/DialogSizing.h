// SPDX-License-Identifier: GPL-3.0-or-later
// DialogSizing — every dialog and window opens at least big enough for its content, so no scroll
// bar shows when none is needed (spec 73). One helper for all of them instead of per-dialog
// magic numbers: the size comes from the content's size hints, and is capped to the screen.
#pragma once

#include <QByteArray>
#include <QScrollArea>
#include <QSize>

class QWidget;

namespace kk {

/// A scroll area for a page of a dialog (Project Settings, Preferences, ...). Its size hint is
/// the size of the widget inside, so a stack of such pages asks for the widest and tallest
/// page and the dialog opens big enough for all of them. It still scrolls when the dialog is
/// smaller than that (a small screen, or a dialog made smaller by hand where allowed).
/// The minimum size hint stays small so the dialog can be capped to the screen.
class PageScrollArea : public QScrollArea
{
    Q_OBJECT
public:
    explicit PageScrollArea(QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override;
};

namespace dialogsizing {

/// The largest size a window of @p window may open at: the available geometry of its screen
/// (the screen of its parent, else the primary screen) less room for the title bar and frame.
[[nodiscard]] QSize screenLimit(const QWidget* window);

/// Pretends every screen has this available size (tests, to check a small screen); an invalid
/// size switches the pretence off.
void setScreenSizeForTesting(QSize size);

/// Sizes the top-level @p window from its content: the minimum size becomes the size hint of
/// its layout (what the content needs for no scroll bar), the initial size max(that,
/// @p preferred); both are capped to screenLimit(). Call at the end of the constructor, when
/// the content is complete. Returns the initial size.
QSize fit(QWidget* window, QSize preferred = {});

/// Restores @p state (QWidget::saveGeometry) only when the restored window is not smaller than
/// its minimum size; otherwise (or when @p state is empty) the window gets @p fallback size
/// (the fit() size when invalid). Returns true when the saved geometry was used.
bool restoreGeometryIfFits(QWidget* window, const QByteArray& state, QSize fallback = {});

} // namespace dialogsizing
} // namespace kk
