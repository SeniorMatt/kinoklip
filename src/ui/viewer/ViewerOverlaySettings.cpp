// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerOverlaySettings — storage of the viewer overlay options and the View menu bindings.
#include "ui/viewer/ViewerOverlaySettings.h"

#include "core/ViewerGuides.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/SchemeColors.h"
#include "ui/viewer/GuideOverlay.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColorDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <functional>

namespace kk {

namespace {
// QSettings keys (per user; the guide positions themselves are in the project).
constexpr char kShowGuides[] = "viewer/overlays/showGuides";
constexpr char kLockGuides[] = "viewer/overlays/lockGuides";
constexpr char kSnapToGuides[] = "viewer/overlays/snapToGuides";
constexpr char kGuideColor[] = "viewer/overlays/guideColor"; ///< "#rrggbb", "" = default
constexpr char kShowRulers[] = "viewer/overlays/showRulers";
constexpr char kActionSafe[] = "viewer/overlays/actionSafe";
constexpr char kTitleSafe[] = "viewer/overlays/titleSafe";
constexpr char kCentreMark[] = "viewer/overlays/centreMark";
constexpr char kAspectMarker[] = "viewer/overlays/aspectMarker"; ///< double, 0 = off
constexpr char kShowGrid[] = "viewer/overlays/showGrid";
constexpr char kGridDivisions[] = "viewer/overlays/gridDivisions";

constexpr int kQuarterGrid = 4; ///< the "4 x 4" grid entry
/// Two aspect ratios count as the same menu entry when this close (stored as doubles).
constexpr double kAspectEpsilon = 1e-3;

QAction* find(ActionRegistry* actions, const char* id)
{
    return actions && actions->contains(QLatin1String(id)) ? actions->action(QLatin1String(id))
                                                           : nullptr;
}

/// Frame size of @p overlay as QSizeF (the preset fractions' reference).
QSizeF frameOf(const GuideOverlay* overlay)
{
    return QSizeF(overlay->frameSize());
}
} // namespace

ViewerOverlaySettings& ViewerOverlaySettings::instance()
{
    static ViewerOverlaySettings* settings = new ViewerOverlaySettings; // lives until exit
    return *settings;
}

ViewerOverlaySettings::ViewerOverlaySettings()
{
    load();
}

const std::vector<ViewerOverlaySettings::AspectChoice>& ViewerOverlaySettings::aspectChoices()
{
    static const std::vector<AspectChoice> choices {
        {"view.aspect_none", 0.0},
        {"view.aspect_2_39", 2.39},
        {"view.aspect_1_85", 1.85},
        {"view.aspect_4_3", 4.0 / 3.0},
        {"view.aspect_1_1", 1.0},
        {"view.aspect_4_5", 4.0 / 5.0},
        {"view.aspect_9_16", 9.0 / 16.0},
    };
    return choices;
}

void ViewerOverlaySettings::load()
{
    const QSettings s;
    ViewerOverlayOptions o;
    o.showGuides = s.value(QLatin1String(kShowGuides), o.showGuides).toBool();
    o.lockGuides = s.value(QLatin1String(kLockGuides), o.lockGuides).toBool();
    o.snapToGuides = s.value(QLatin1String(kSnapToGuides), o.snapToGuides).toBool();
    const QString color = s.value(QLatin1String(kGuideColor)).toString();
    o.guideColor = color.isEmpty() ? QColor() : QColor(color);
    o.showRulers = s.value(QLatin1String(kShowRulers), o.showRulers).toBool();
    o.actionSafe = s.value(QLatin1String(kActionSafe), o.actionSafe).toBool();
    o.titleSafe = s.value(QLatin1String(kTitleSafe), o.titleSafe).toBool();
    o.centreMark = s.value(QLatin1String(kCentreMark), o.centreMark).toBool();
    o.aspectMarker = std::max(0.0, s.value(QLatin1String(kAspectMarker), 0.0).toDouble());
    o.showGrid = s.value(QLatin1String(kShowGrid), o.showGrid).toBool();
    o.gridDivisions = std::clamp(s.value(QLatin1String(kGridDivisions), o.gridDivisions).toInt(), 2,
        guides::kMaxGridDivisions);
    if (o != m_options) {
        m_options = o;
        syncActions();
        emit changed();
    }
}

void ViewerOverlaySettings::setOptions(const ViewerOverlayOptions& options)
{
    if (options == m_options)
        return;
    m_options = options;
    m_options.gridDivisions = std::clamp(m_options.gridDivisions, 2, guides::kMaxGridDivisions);
    QSettings s;
    s.setValue(QLatin1String(kShowGuides), m_options.showGuides);
    s.setValue(QLatin1String(kLockGuides), m_options.lockGuides);
    s.setValue(QLatin1String(kSnapToGuides), m_options.snapToGuides);
    s.setValue(QLatin1String(kGuideColor),
        m_options.guideColor.isValid() ? m_options.guideColor.name() : QString());
    s.setValue(QLatin1String(kShowRulers), m_options.showRulers);
    s.setValue(QLatin1String(kActionSafe), m_options.actionSafe);
    s.setValue(QLatin1String(kTitleSafe), m_options.titleSafe);
    s.setValue(QLatin1String(kCentreMark), m_options.centreMark);
    s.setValue(QLatin1String(kAspectMarker), m_options.aspectMarker);
    s.setValue(QLatin1String(kShowGrid), m_options.showGrid);
    s.setValue(QLatin1String(kGridDivisions), m_options.gridDivisions);
    syncActions();
    emit changed();
}

QColor ViewerOverlaySettings::guideColor() const
{
    // Default: the scheme's focus decoration (cyan in Breeze), readable over most pictures
    // and distinct from the selection outline of the transform controls.
    return m_options.guideColor.isValid() ? m_options.guideColor : schemeColors().focus;
}

GuideOverlay* ViewerOverlaySettings::targetOverlay()
{
    ViewerPanel* active = ViewerPanel::activeViewer();
    if (active && active->isVisible() && active->guideOverlay())
        return active->guideOverlay();
    const QList<ViewerPanel*> viewers = ViewerPanel::allViewers();
    for (ViewerPanel* v : viewers) {
        if (v->isVisible() && v->mode() == ViewerPanel::Mode::Timeline && v->guideOverlay())
            return v->guideOverlay();
    }
    for (ViewerPanel* v : viewers) {
        if (v->guideOverlay())
            return v->guideOverlay();
    }
    return nullptr;
}

void ViewerOverlaySettings::syncActions()
{
    if (!m_actions)
        return;
    m_syncing = true;
    const ViewerOverlayOptions& o = m_options;
    const std::pair<const char*, bool> checks[] = {
        {"view.guides_show", o.showGuides},
        {"view.guides_lock", o.lockGuides},
        {"view.guides_snap", o.snapToGuides},
        {"view.show_rulers", o.showRulers},
        {"view.safe_action", o.actionSafe},
        {"view.safe_title", o.titleSafe},
        {"view.safe_centre", o.centreMark},
        {"view.grid_show", o.showGrid},
        {"view.grid_thirds", o.gridDivisions == 3},
        {"view.grid_quarters", o.gridDivisions == kQuarterGrid},
        {"view.grid_custom", o.gridDivisions != 3 && o.gridDivisions != kQuarterGrid},
    };
    for (const auto& [id, on] : checks) {
        if (QAction* a = find(m_actions, id))
            a->setChecked(on);
    }
    for (const AspectChoice& c : aspectChoices()) {
        if (QAction* a = find(m_actions, c.actionId))
            a->setChecked(std::abs(c.aspect - o.aspectMarker) < kAspectEpsilon);
    }
    if (QAction* custom = find(m_actions, "view.grid_custom")) {
        custom->setText(o.gridDivisions != 3 && o.gridDivisions != kQuarterGrid
                ? tr("&Custom Grid (%1 x %1)…").arg(o.gridDivisions)
                : tr("&Custom Grid…"));
    }
    m_syncing = false;
}

void ViewerOverlaySettings::connectActions(ActionRegistry* actions)
{
    if (!actions || m_actions == actions)
        return;
    m_actions = actions;
    // Checkable options: the action writes its field; syncActions() writes the check state.
    using Field = bool ViewerOverlayOptions::*;
    const std::pair<const char*, Field> toggles[] = {
        {"view.guides_show", &ViewerOverlayOptions::showGuides},
        {"view.guides_lock", &ViewerOverlayOptions::lockGuides},
        {"view.guides_snap", &ViewerOverlayOptions::snapToGuides},
        {"view.show_rulers", &ViewerOverlayOptions::showRulers},
        {"view.safe_action", &ViewerOverlayOptions::actionSafe},
        {"view.safe_title", &ViewerOverlayOptions::titleSafe},
        {"view.safe_centre", &ViewerOverlayOptions::centreMark},
        {"view.grid_show", &ViewerOverlayOptions::showGrid},
    };
    for (const auto& [id, field] : toggles) {
        if (QAction* a = find(actions, id)) {
            connect(a, &QAction::toggled, this, [this, field = field](bool on) {
                if (m_syncing)
                    return;
                ViewerOverlayOptions o = m_options;
                o.*field = on;
                setOptions(o);
            });
        }
    }
    // Aspect marker: exclusive choice (None or one ratio).
    auto* aspects = new QActionGroup(actions);
    for (const AspectChoice& c : aspectChoices()) {
        QAction* a = find(actions, c.actionId);
        if (!a)
            continue;
        a->setCheckable(true);
        aspects->addAction(a);
        connect(a, &QAction::triggered, this, [this, aspect = c.aspect] {
            ViewerOverlayOptions o = m_options;
            o.aspectMarker = aspect;
            setOptions(o);
        });
    }
    // Grid type: picking one shows the grid.
    auto* grids = new QActionGroup(actions);
    auto setGrid = [this](int divisions) {
        ViewerOverlayOptions o = m_options;
        o.gridDivisions = divisions;
        o.showGrid = true;
        setOptions(o);
        syncActions(); // a cancelled custom dialog restores the radio state
    };
    if (QAction* a = find(actions, "view.grid_thirds")) {
        grids->addAction(a);
        connect(a, &QAction::triggered, this, [setGrid] { setGrid(3); });
    }
    if (QAction* a = find(actions, "view.grid_quarters")) {
        grids->addAction(a);
        connect(a, &QAction::triggered, this, [setGrid] { setGrid(kQuarterGrid); });
    }
    if (QAction* a = find(actions, "view.grid_custom")) {
        grids->addAction(a);
        connect(a, &QAction::triggered, this, [this, setGrid] {
            bool ok = false;
            const int n = QInputDialog::getInt(QApplication::activeWindow(), tr("Custom Grid"),
                tr("Columns and rows:"), m_options.gridDivisions, 2, guides::kMaxGridDivisions, 1,
                &ok);
            if (ok)
                setGrid(n);
            else
                syncActions();
        });
    }

    // Guide commands act on the target viewer's overlay.
    auto onOverlay = [this, actions](const char* id, std::function<void(GuideOverlay*)> fn) {
        if (QAction* a = find(actions, id)) {
            connect(a, &QAction::triggered, this, [fn = std::move(fn)] {
                if (GuideOverlay* overlay = targetOverlay())
                    fn(overlay);
            });
        }
    };
    onOverlay("view.guide_add_horizontal", [](GuideOverlay* o) { o->addGuide(Qt::Horizontal); });
    onOverlay("view.guide_add_vertical", [](GuideOverlay* o) { o->addGuide(Qt::Vertical); });
    onOverlay("view.guides_clear", [](GuideOverlay* o) { o->clearGuides(); });
    onOverlay("view.guide_save_preset", [this](GuideOverlay* o) {
        if (o->guides().empty()) {
            QMessageBox::information(QApplication::activeWindow(), tr("Save Guide Preset"),
                tr("The viewer has no guides to save."));
            return;
        }
        bool ok = false;
        const QString name = QInputDialog::getText(QApplication::activeWindow(),
            tr("Save Guide Preset"), tr("Preset name:"), QLineEdit::Normal, QString(), &ok)
                                 .trimmed();
        if (!ok || name.isEmpty())
            return;
        QString error;
        if (!saveUserPreset(guides::makeUserPreset(name, o->guides(), frameOf(o)), &error))
            QMessageBox::warning(QApplication::activeWindow(), tr("Save Guide Preset"),
                tr("Cannot save the preset: %1").arg(error));
    });
    if (QAction* a = find(actions, "view.guide_color")) {
        connect(a, &QAction::triggered, this, [this] {
            const QColor c = QColorDialog::getColor(
                guideColor(), QApplication::activeWindow(), tr("Guide Color"));
            if (!c.isValid())
                return;
            ViewerOverlayOptions o = m_options;
            o.guideColor = c;
            setOptions(o);
        });
    }
    syncActions();
}

QString ViewerOverlaySettings::presetPath() const
{
    return m_presetPath.isEmpty() ? guides::userPresetPath() : m_presetPath;
}

bool ViewerOverlaySettings::saveUserPreset(const GuidePreset& preset, QString* error)
{
    std::vector<GuidePreset> presets = guides::loadUserPresets(presetPath());
    // Same name: replace (the user saved the guides again under an existing name).
    auto it = std::find_if(presets.begin(), presets.end(),
        [&preset](const GuidePreset& p) { return p.name == preset.name; });
    if (it != presets.end())
        *it = preset;
    else
        presets.push_back(preset);
    return guides::saveUserPresets(presets, presetPath(), error);
}

bool ViewerOverlaySettings::deleteUserPreset(const QString& name, QString* error)
{
    std::vector<GuidePreset> presets = guides::loadUserPresets(presetPath());
    std::erase_if(presets, [&name](const GuidePreset& p) { return p.name == name; });
    return guides::saveUserPresets(presets, presetPath(), error);
}

void ViewerOverlaySettings::attachPresetMenus(QMenu* load, QMenu* remove)
{
    // Rebuilt on every opening (a few entries): reflects presets saved meanwhile.
    if (load) {
        connect(load, &QMenu::aboutToShow, this, [this, load] {
            load->clear();
            bool builtIn = true;
            for (const GuidePreset& p : guides::allPresets(presetPath())) {
                if (builtIn && !p.builtIn)
                    load->addSeparator(); // the user's presets below the built-in ones
                builtIn = p.builtIn;
                QAction* a = load->addAction(p.name);
                a->setObjectName(p.id);
                connect(a, &QAction::triggered, this, [p] {
                    if (GuideOverlay* overlay = targetOverlay())
                        overlay->applyPreset(p);
                });
            }
        });
    }
    if (remove) {
        connect(remove, &QMenu::aboutToShow, this, [this, remove] {
            remove->clear();
            const std::vector<GuidePreset> user = guides::loadUserPresets(presetPath());
            if (user.empty()) {
                remove->addAction(tr("No User Presets"))->setEnabled(false);
                return;
            }
            for (const GuidePreset& p : user) {
                QAction* a = remove->addAction(p.name);
                connect(a, &QAction::triggered, this, [this, name = p.name] {
                    QString error;
                    if (!deleteUserPreset(name, &error))
                        QMessageBox::warning(QApplication::activeWindow(),
                            tr("Delete Guide Preset"),
                            tr("Cannot save the presets: %1").arg(error));
                });
            }
        });
    }
}

} // namespace kk
