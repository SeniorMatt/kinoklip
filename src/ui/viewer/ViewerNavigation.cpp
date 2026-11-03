// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerPanel part: zoom and pan of the picture (spec 41.19 - 41.25). Mouse wheel zooms about
// the cursor, middle drag (or Space + left drag) pans, Shift+Z / Fit / middle double-click fit.
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/viewer/ColorPickOverlay.h"
#include "ui/viewer/GuideOverlay.h"
#include "ui/viewer/MaskOverlay.h"
#include "ui/viewer/MulticamOverlay.h"
#include "ui/viewer/TransformOverlay.h"
#include "ui/viewer/TrimTwoUpView.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QKeyEvent>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>

namespace kk {

namespace {

/// Zoom factor of one wheel notch (120 eighths of a degree); trackpads scale it by their delta.
constexpr double kWheelStep = 1.125;
/// Factors that are within this of a preset show and behave as the preset.
constexpr double kPresetTolerance = 1e-3;

double presetFactor(ViewerPanel::Zoom zoom)
{
    switch (zoom) {
    case ViewerPanel::Zoom::Percent50:
        return 0.5;
    case ViewerPanel::Zoom::Percent100:
        return 1.0;
    case ViewerPanel::Zoom::Percent200:
        return 2.0;
    case ViewerPanel::Zoom::Fit:
    case ViewerPanel::Zoom::Custom:
        break;
    }
    return 0.0;
}

} // namespace

/// Application event filter of one viewer: the overlays over the picture (transform, masks,
/// eyedropper) are child widgets that would otherwise take wheel and middle-button events
/// first, and Space must be seen wherever the focus is.
class ViewerNavigationFilter : public QObject
{
public:
    explicit ViewerNavigationFilter(ViewerPanel* panel)
        : QObject(panel)
        , m_panel(panel)
    { }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        // Cheap type test first: this filter sees every event of the application.
        switch (event->type()) {
        case QEvent::Wheel:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
        case QEvent::ShortcutOverride:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            return m_panel->navigationEvent(watched, event);
        default:
            return false;
        }
    }

private:
    ViewerPanel* m_panel;
};

ViewerPanel::Zoom ViewerPanel::zoom() const
{
    const double z = zoomFactor();
    if (z <= 0.0)
        return Zoom::Fit;
    for (const Zoom preset : {Zoom::Percent50, Zoom::Percent100, Zoom::Percent200}) {
        if (std::abs(z - presetFactor(preset)) < kPresetTolerance)
            return preset;
    }
    return Zoom::Custom;
}

void ViewerPanel::setZoom(Zoom zoom)
{
    if (zoom == Zoom::Custom) {
        updateZoomBox();
        return;
    }
    if (zoom == Zoom::Fit) {
        zoomToFit();
        return;
    }
    setZoomFactor(presetFactor(zoom));
}

double ViewerPanel::zoomFactor() const
{
    return m_surface->picture().zoom();
}

double ViewerPanel::effectiveZoom() const
{
    QWidget* w = m_surface->widget();
    return m_surface->picture().effectiveZoom(w->size(), w->devicePixelRatioF());
}

QPointF ViewerPanel::pan() const
{
    return zoomFactor() > 0.0 ? m_surface->picture().pan() : QPointF();
}

void ViewerPanel::setZoomFactor(double zoom)
{
    if (zoom <= 0.0) {
        zoomToFit();
        return;
    }
    zoom = std::clamp(zoom, ViewerPicture::kMinZoom, ViewerPicture::kMaxZoom);
    if (zoomFactor() <= 0.0) {
        applyView(zoom, {}); // from Fit: centred
        return;
    }
    // Keep the picture point at the surface centre in place (presets from the header).
    QWidget* w = m_surface->widget();
    const QPointF centre(w->width() / 2.0, w->height() / 2.0);
    zoomAt(zoom, centre);
}

void ViewerPanel::zoomAt(double zoom, QPointF anchor)
{
    zoom = std::clamp(zoom, ViewerPicture::kMinZoom, ViewerPicture::kMaxZoom);
    for (const Zoom preset : {Zoom::Percent50, Zoom::Percent100, Zoom::Percent200}) {
        if (std::abs(zoom - presetFactor(preset)) < kPresetTolerance)
            zoom = presetFactor(preset); // exact presets: unfiltered 1:1 / 2:1 drawing
    }
    QWidget* w = m_surface->widget();
    applyView(zoom,
        m_surface->picture().panForZoomAbout(zoom, anchor, w->size(), w->devicePixelRatioF()));
}

void ViewerPanel::setPan(QPointF pan)
{
    const double zoom = zoomFactor();
    if (zoom <= 0.0)
        return;
    QWidget* w = m_surface->widget();
    applyView(zoom, m_surface->picture().clampPan(pan, zoom, w->size(), w->devicePixelRatioF()));
}

void ViewerPanel::zoomToFit()
{
    applyView(0.0, {});
}

void ViewerPanel::applyView(double zoom, QPointF pan)
{
    const QRect before = m_surface->pictureRect();
    m_surface->setView(zoom, pan);
    updateZoomBox();
    if (m_surface->pictureRect() == before)
        return;
    if (m_maskOverlay)
        m_maskOverlay->refresh(); // handles follow the new picture rectangle
    if (m_transformOverlay)
        m_transformOverlay->refresh();
    if (m_colorPickOverlay)
        m_colorPickOverlay->update();
    if (m_multicamOverlay)
        m_multicamOverlay->refresh();
    if (m_guideOverlay)
        m_guideOverlay->refresh(); // guides and rulers follow the new mapping
    if (before.size() != m_surface->pictureRect().size()) {
        // The render size follows the picture size (at most the timeline resolution); wheel
        // steps are coalesced like resizes, so a fast wheel does not restart the render every
        // notch.
        if (m_coalesceOutput && m_outputSized && m_resizeSettle->interval() > 0)
            m_resizeSettle->start();
        else
            updateOutputClient();
    }
}

void ViewerPanel::updateZoomBox()
{
    const Zoom z = zoom();
    const QSignalBlocker block(m_zoomBox);
    const int custom = int(Zoom::Custom);
    if (z == Zoom::Custom) {
        const QString text = QStringLiteral("%1%").arg(int(std::lround(zoomFactor() * 100.0)));
        if (m_zoomBox->count() <= custom)
            m_zoomBox->addItem(text);
        else
            m_zoomBox->setItemText(custom, text);
        m_zoomBox->setCurrentIndex(custom);
    } else {
        m_zoomBox->setCurrentIndex(int(z));
        if (m_zoomBox->count() > custom)
            m_zoomBox->removeItem(custom);
    }
}

void ViewerPanel::setHandArmed(bool armed)
{
    if (armed == m_handArmed)
        return;
    m_handArmed = armed;
    if (armed)
        QApplication::setOverrideCursor(Qt::OpenHandCursor);
    else
        QApplication::restoreOverrideCursor();
}

bool ViewerPanel::navigationEvent(QObject* watched, QEvent* event)
{
    QWidget* surface = m_surface->widget();
    const QEvent::Type type = event->type();

    // ---- Keys: Space + drag (hand), Shift+Z with focus in the viewer ----
    if (type == QEvent::ShortcutOverride || type == QEvent::KeyPress
        || type == QEvent::KeyRelease) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool space = key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier;
        if (type == QEvent::KeyRelease) {
            if (!space || key->isAutoRepeat() || !m_handArmed)
                return false;
            setHandArmed(false);
            if (m_panDrag.button == Qt::LeftButton)
                m_panDrag = {};
            // A tap of Space without a drag still plays / stops, on release.
            if (!m_handUsed && m_ctx.actions
                && m_ctx.actions->contains(QStringLiteral("transport.play_pause")))
                m_ctx.actions->action(QStringLiteral("transport.play_pause"))->trigger();
            return true;
        }
        if (space && m_handArmed) {
            event->accept(); // autorepeats and the KeyPress after the override
            return true;
        }
        if (type != QEvent::ShortcutOverride || key->isAutoRepeat())
            return false;
        if (space) {
            // Armed only while the cursor is over the visible, zoomed picture with no button
            // down: everywhere else Space plays at once as before.
            // Never while typing: a focused text field gets its space.
            const QWidget* focus = QApplication::focusWidget();
            if (zoomFactor() <= 0.0 || !surface->isVisible() || QApplication::mouseButtons()
                || (focus && focus->testAttribute(Qt::WA_InputMethodEnabled))
                || !surface->rect().contains(surface->mapFromGlobal(QCursor::pos()))
                || (m_twoUp && m_twoUp->isVisible()))
                return false;
            m_handUsed = false;
            setHandArmed(true);
            event->accept();
            return true;
        }
        // Zoom to Fit: the timeline's key (Shift+Z) fits this viewer while focus is in it.
        auto* focus = QApplication::focusWidget();
        if (!m_ctx.actions || !focus || !isAncestorOf(focus)
            || !m_ctx.actions->contains(QStringLiteral("timeline.zoom_fit")))
            return false;
        const QKeySequence pressed(
            QKeyCombination(key->modifiers() & ~Qt::KeypadModifier, Qt::Key(key->key())));
        if (!m_ctx.actions->shortcuts(QStringLiteral("timeline.zoom_fit")).contains(pressed))
            return false;
        zoomToFit();
        event->accept(); // the timeline's own Zoom to Fit does not fire
        return true;
    }

    // ---- Mouse ----
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget)
        return false;
    if (type == QEvent::MouseMove || type == QEvent::MouseButtonRelease) {
        if (m_panDrag.button == Qt::NoButton)
            return false;
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (type == QEvent::MouseButtonRelease) {
            if (mouse->button() != m_panDrag.button)
                return false;
            m_panDrag = {};
            if (m_handArmed)
                QApplication::changeOverrideCursor(Qt::OpenHandCursor);
            return true;
        }
        const QPointF delta = mouse->globalPosition() - m_panDrag.lastGlobal;
        m_panDrag.lastGlobal = mouse->globalPosition();
        setPan(pan() + delta);
        return true;
    }
    if (widget != surface && !surface->isAncestorOf(widget))
        return false;
    // Layers that cover the picture with something else take their own input.
    if ((m_twoUp && m_twoUp->isVisible()))
        return false;
    if (type == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        const int delta = wheel->angleDelta().y();
        if (delta == 0)
            return false;
        const QPointF anchor = widget->mapTo(surface, wheel->position());
        m_coalesceOutput = true;
        zoomAt(effectiveZoom() * std::pow(kWheelStep, delta / 120.0), anchor);
        m_coalesceOutput = false;
        activate();
        return true;
    }
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (type == QEvent::MouseButtonDblClick && mouse->button() == Qt::MiddleButton) {
        zoomToFit();
        return true;
    }
    const bool middle = mouse->button() == Qt::MiddleButton;
    const bool hand = m_handArmed && mouse->button() == Qt::LeftButton;
    if ((type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick)
        && (middle || hand)) {
        activate();
        if (zoomFactor() > 0.0) {
            m_panDrag = {mouse->button(), mouse->globalPosition()};
            m_handUsed = m_handUsed || hand;
            if (m_handArmed)
                QApplication::changeOverrideCursor(Qt::ClosedHandCursor);
        }
        return true; // never a transform / mask / guide gesture
    }
    return false;
}

void ViewerPanel::setupNavigation()
{
    m_navigationFilter = new ViewerNavigationFilter(this);
    qApp->installEventFilter(m_navigationFilter);
}

} // namespace kk
