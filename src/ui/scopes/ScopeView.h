// SPDX-License-Identifier: GPL-3.0-or-later
// ScopeView — one video scope (waveform, RGB waveform, parade, vectorscope, histogram) drawn
// from a picture; ScopesPanel — a scope with its type menu (Color page, the Edit page's Video
// Scopes window). The counts come from render/Scopes.h; this file only draws them.
#pragma once

#include "render/Scopes.h"

#include <QImage>
#include <QWidget>

class QComboBox;

namespace kk {

/// A custom-painted scope: dark background, graticule (0 - 100 % lines for the waveforms and
/// the histogram; the 75 % colour targets, the skin tone line and the 100 % circle for the
/// vectorscope) and the trace, its brightness growing with the number of pixels at each
/// spot (1 - exp(-k n), so single pixels show and full areas do not burn out).
///
/// setImage() computes the counts at once (a few milliseconds for a frame of 480 px: pass a
/// small picture, see scopes::scopeInput()). GUI thread only.
class ScopeView : public QWidget
{
    Q_OBJECT
public:
    explicit ScopeView(QWidget* parent = nullptr);

    void setType(scopes::ScopeType type);
    [[nodiscard]] scopes::ScopeType type() const { return m_type; }
    /// Shows the scope of @p image (any size; reduced with scopes::scopeInput()). A null
    /// image clears the trace.
    void setImage(const QImage& image);
    /// The trace as drawn (without the graticule), for tests: waveform columns x 256 levels
    /// (level 0 at the bottom), parade three of those side by side, vectorscope 256 x 256,
    /// histogram 256 x 128.
    [[nodiscard]] const QImage& traceImage() const { return m_trace; }

    [[nodiscard]] QSize sizeHint() const override { return {360, 240}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void rebuild();

    scopes::ScopeType m_type = scopes::ScopeType::WaveformLuma;
    QImage m_input;
    QImage m_trace;
};

/// A scope with a type menu above it, the unit the Color page shows (and the Edit page's
/// Video Scopes window, which has two). The chosen type is remembered under @p settingsKey
/// (empty = not remembered).
class ScopesPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ScopesPanel(const QString& settingsKey = {}, QWidget* parent = nullptr);

    [[nodiscard]] ScopeView* view() const { return m_view; }
    /// Shows the scopes of @p image.
    void setImage(const QImage& image) { m_view->setImage(image); }
    void setType(scopes::ScopeType type);

private:
    QString m_settingsKey;
    QComboBox* m_types = nullptr;
    ScopeView* m_view = nullptr;
};

} // namespace kk
