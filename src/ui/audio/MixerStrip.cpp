// SPDX-License-Identifier: GPL-3.0-or-later
// MixerStrip layout and gestures.
#include "ui/audio/MixerStrip.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/audio/DynamicsWindow.h"
#include "ui/audio/EqWindow.h"
#include "ui/audio/FaderWidget.h"
#include "ui/audio/LevelMeter.h"
#include "ui/audio/MixerController.h"
#include "ui/audio/VoiceOverController.h"
#include "ui/audio/VoiceOverMenu.h"
#include "ui/common/Metrics.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TextFit.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr AutomationMode kModes[]
    = {AutomationMode::Off, AutomationMode::Read, AutomationMode::Touch, AutomationMode::Write};

QString modeLabel(AutomationMode m)
{
    switch (m) {
    case AutomationMode::Off:
        return QCoreApplication::translate("kk::MixerStrip", "Off");
    case AutomationMode::Read:
        return QCoreApplication::translate("kk::MixerStrip", "Read");
    case AutomationMode::Touch:
        return QCoreApplication::translate("kk::MixerStrip", "Touch");
    case AutomationMode::Write:
        return QCoreApplication::translate("kk::MixerStrip", "Write");
    }
    return {};
}

/// The input level of an armed track (dBFS scale from -48, red above -6, a clip mark).
class InputLevelBar final : public QWidget
{
public:
    explicit InputLevelBar(const VoiceOverController* vo, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_vo(vo)
    {
        setFixedHeight(6);
        setToolTip(QCoreApplication::translate("kk::MixerStrip", "Level of the microphone"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const SchemeColors& sc = schemeColors();
        QPainter p(this);
        p.fillRect(rect(), palette().window().color().darker(150));
        const float peak = m_vo ? m_vo->peak() : 0.0f;
        const double db = peak > 0.0f ? 20.0 * std::log10(double(peak)) : -96.0;
        const double t = std::clamp((db + 48.0) / 48.0, 0.0, 1.0);
        p.fillRect(QRect(0, 0, int(t * width()), height()),
            db > -6.0        ? sc.negative
                : db > -18.0 ? sc.neutral
                             : sc.positive);
        if (m_vo && m_vo->clipped())
            p.fillRect(QRect(width() - 4, 0, 4, height()), sc.negative);
    }

private:
    const VoiceOverController* m_vo;
};

QToolButton* toggleButton(const QString& text, const QString& tip)
{
    auto* b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setAutoRaise(false);
    b->setFocusPolicy(Qt::NoFocus);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

QString dbText(double db)
{
    if (db <= kFaderMinDb)
        return QCoreApplication::translate("kk::MixerStrip", "-inf");
    return QString::number(db, 'f', 1);
}

} // namespace

MixerStrip::MixerStrip(
    MixerController* controller, MeterModel* meters, const MixTarget& target, QWidget* parent)
    : QWidget(parent)
    , m_controller(controller)
    , m_meters(meters)
    , m_target(target)
{
    setObjectName(target.isBus() ? QStringLiteral("BusStrip") : QStringLiteral("TrackStrip"));
    setFixedWidth(kWidth);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceXs, metrics::kSpaceS, metrics::kSpaceXs, metrics::kSpaceS);
    layout->setSpacing(metrics::kSpaceXs);
    const bool bus = target.isBus();

    m_input = new QLabel;
    m_input->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_input);

    m_eq = toggleButton(tr("EQ"), tr("Equalizer: lit when on; click to edit"));
    m_dynamics
        = toggleButton(tr("Dyn"), tr("Dynamics (compressor, limiter): lit when on; click to edit"));
    // The buttons show the state; a click opens the editor (the state does not toggle).
    for (QToolButton* b : {m_eq, m_dynamics})
        layout->addWidget(b);
    connect(m_eq, &QToolButton::clicked, this, [this] {
        refresh(m_frame); // the click toggled the button: show the real state again
        openEq();
    });
    connect(m_dynamics, &QToolButton::clicked, this, [this] {
        refresh(m_frame);
        openDynamics();
    });

    m_bus = new QToolButton;
    m_bus->setPopupMode(QToolButton::InstantPopup);
    m_bus->setFocusPolicy(Qt::NoFocus);
    m_bus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (!bus) {
        m_bus->setToolTip(tr("Bus assignment"));
        auto* menu = new QMenu(m_bus);
        m_bus->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, &MixerStrip::fillBusMenu);
    } else {
        m_bus->setCheckable(true);
        m_bus->setText(tr("Output"));
        m_bus->setToolTip(
            tr("Make this bus the output (heard in playback and rendered by export)"));
        connect(m_bus, &QToolButton::clicked, this, [this] {
            m_controller->edit(tr("Set Output Bus"),
                [this](TimelineData& d) { return mixerops::setOutputBus(d, int(m_target.id)); });
            refresh(m_frame);
        });
    }
    layout->addWidget(m_bus);

    if (!bus) {
        m_pan = new PanKnob;
        m_pan->setFixedHeight(40);
        layout->addWidget(m_pan, 0, Qt::AlignHCenter);
        connect(m_pan, &PanKnob::pressed, this,
            [this] { m_controller->pressControl(m_target, MixerController::Control::Pan); });
        connect(m_pan, &PanKnob::moved, this, [this](double v) {
            m_controller->moveControl(m_target, MixerController::Control::Pan, v);
        });
        connect(m_pan, &PanKnob::released, this,
            [this] { m_controller->releaseControl(m_target, MixerController::Control::Pan); });
        connect(m_pan, &PanKnob::valueRequested, this, [this](double v) {
            m_controller->setControl(m_target, MixerController::Control::Pan, v);
        });

        m_automation = new QComboBox;
        m_automation->setToolTip(tr("Automation mode of the fader and pan"));
        m_automation->setFocusPolicy(Qt::NoFocus);
        for (AutomationMode m : kModes)
            m_automation->addItem(modeLabel(m), int(m));
        layout->addWidget(m_automation);
        connect(m_automation, &QComboBox::activated, this, [this](int index) {
            const auto mode = AutomationMode(m_automation->itemData(index).toInt());
            m_controller->edit(tr("Automation Mode"),
                [&](TimelineData& d) { return mixerops::setAutomationMode(d, m_target.id, mode); });
        });
    }

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(metrics::kSpaceXs);
    if (!bus) {
        m_arm = toggleButton(tr("R"), tr("Record arm (voice-over)"));
        m_arm->setContextMenuPolicy(Qt::CustomContextMenu);
        buttons->addWidget(m_arm);
        if (VoiceOverController* vo = m_controller->voiceOver()) {
            connect(m_arm, &QToolButton::clicked, this, [this, vo] {
                vo->toggleArm(m_target.id);
                m_arm->setChecked(vo->isArmed(m_target.id)); // a refused arm stays off
            });
            connect(m_arm, &QToolButton::customContextMenuRequested, this, [this, vo](QPoint pos) {
                QMenu menu(this);
                fillVoiceOverInputMenu(menu, vo, m_target.id);
                menu.exec(m_arm->mapToGlobal(pos));
            });
            connect(vo, &VoiceOverController::armedChanged, this, [this] { refresh(m_frame); });
            connect(vo, &VoiceOverController::inputChanged, this, [this] { refresh(m_frame); });
            m_inputLevel = new InputLevelBar(vo);
            m_inputLevel->hide();
            connect(vo, &VoiceOverController::levelChanged, m_inputLevel,
                qOverload<>(&QWidget::update));
        } else {
            m_arm->setEnabled(false);
        }
    }
    m_mute = toggleButton(tr("M"), tr("Mute"));
    buttons->addWidget(m_mute);
    if (!bus) {
        m_solo = toggleButton(tr("S"), tr("Solo"));
        buttons->addWidget(m_solo);
        connect(m_solo, &QToolButton::clicked, this,
            [this](bool on) { m_controller->setTrackSolo(m_target.id, on); });
        connect(m_mute, &QToolButton::clicked, this,
            [this](bool on) { m_controller->setTrackMuted(m_target.id, on); });
    } else {
        connect(m_mute, &QToolButton::clicked, this, [this](bool on) {
            m_controller->edit(on ? tr("Mute Bus") : tr("Unmute Bus"), [&](TimelineData& d) {
                BusData* b = mixerops::bus(d, int(m_target.id));
                if (!b || b->strip.muted == on)
                    return false;
                b->strip.muted = on;
                return true;
            });
        });
    }
    layout->addLayout(buttons);
    if (m_inputLevel)
        layout->addWidget(m_inputLevel);

    auto* faderRow = new QHBoxLayout;
    faderRow->setSpacing(metrics::kSpaceXs);
    m_fader = new FaderWidget;
    m_meter = new LevelMeter(
        meters, bus ? LevelMeter::Source::Bus : LevelMeter::Source::Track, target.id);
    faderRow->addWidget(m_fader, 1);
    faderRow->addWidget(m_meter);
    layout->addLayout(faderRow, 1);
    connect(m_fader, &FaderWidget::pressed, this,
        [this] { m_controller->pressControl(m_target, MixerController::Control::Fader); });
    connect(m_fader, &FaderWidget::moved, this, [this](double db) {
        m_controller->moveControl(m_target, MixerController::Control::Fader, db);
        m_value->setText(dbText(db));
    });
    connect(m_fader, &FaderWidget::released, this,
        [this] { m_controller->releaseControl(m_target, MixerController::Control::Fader); });
    connect(m_fader, &FaderWidget::valueRequested, this, [this](double db) {
        m_controller->setControl(m_target, MixerController::Control::Fader, db);
    });

    m_value = new QLabel;
    m_value->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_value);

    m_name = new QLabel;
    m_name->setAlignment(Qt::AlignCenter);
    m_name->setAutoFillBackground(true);
    m_name->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_name, &QLabel::customContextMenuRequested, this,
        [this](const QPoint& pos) { showNameMenu(m_name->mapToGlobal(pos)); });
    layout->addWidget(m_name);

    refresh(0);
}

void MixerStrip::refresh(int64_t frame)
{
    m_frame = frame;
    const TimelineData& d = m_controller->data();
    const SchemeColors& sc = schemeColors();
    const ChannelStrip* s = mixerops::strip(d, m_target);
    if (!s)
        return;
    QPalette pal = m_name->palette();
    const QColor plate = m_target.isBus() ? sc.focus : sc.audioTrack;
    pal.setColor(QPalette::Window, plate);
    pal.setColor(QPalette::WindowText, sc.textOn(plate));
    m_name->setPalette(pal);

    m_eq->setChecked(s->eq.enabled);
    m_dynamics->setChecked(s->dynamics.compressorEnabled || s->dynamics.limiterEnabled);
    if (m_target.isBus()) {
        const BusData* b = mixerops::bus(d, int(m_target.id));
        // The strip has a fixed width (as in Resolve): long names are elided, in full as the tip.
        textfit::setElidedText(m_input, tr("Sum"), tr("Input: the tracks routed to this bus"));
        textfit::setElidedText(m_name,
            QStringLiteral("%1\n%2").arg(busDisplayName(*b), b->mono ? tr("Mono") : tr("Stereo")),
            tr("Right-click for the channel format, rename and delete"));
        m_bus->setChecked(d.mixer.outputBus == b->id);
        m_mute->setChecked(s->muted);
    } else {
        const TrackData* t = mixerops::audioTrack(d, m_target.id);
        const QString name = mixerops::targetName(d, m_target);
        textfit::setElidedText(m_input, name, tr("Input: the track's clips"));
        textfit::setElidedText(m_name,
            QStringLiteral("%1\n%2").arg(name, t->mix.mono ? tr("Mono") : tr("Stereo")),
            tr("Right-click for the channel format"));
        QStringList buses;
        for (int id : t->mix.buses) {
            if (const BusData* b = mixerops::bus(d, id))
                buses << busDisplayName(*b);
        }
        textfit::setElidedText(m_bus,
            buses.isEmpty()         ? tr("No Bus")
                : buses.size() == 1 ? buses.front()
                                    : tr("%n Buses", nullptr, int(buses.size())),
            tr("Bus assignment: %1")
                .arg(buses.isEmpty() ? tr("none") : buses.join(QStringLiteral(", "))));
        m_mute->setChecked(t->muted);
        m_solo->setChecked(t->solo);
        if (const VoiceOverController* vo = m_controller->voiceOver()) {
            const bool armed = vo->isArmed(t->id);
            m_arm->setChecked(armed);
            m_arm->setToolTip(voiceOverArmTooltip(vo, t->id));
            if (m_inputLevel)
                m_inputLevel->setVisible(armed);
        }
        m_automation->setCurrentIndex(m_automation->findData(int(t->mix.automation)));
    }
    using C = MixerController::Control;
    if (!m_fader->isDragging()) {
        const double db = m_controller->controlValue(m_target, C::Fader, double(frame));
        m_fader->setValue(db);
        m_value->setText(dbText(db));
    }
    if (m_pan)
        m_pan->setValue(m_controller->controlValue(m_target, C::Pan, double(frame)));
}

EqWindow* MixerStrip::openEq()
{
    if (!m_eqWindow) {
        m_eqWindow = new EqWindow(m_controller, m_target, window());
        m_eqWindow->resize(620, 460);
        // Shortcuts are window-scoped: Space, J / K / L, undo work in the tool window too.
        m_eqWindow->addActions(window()->actions());
    }
    m_eqWindow->show();
    m_eqWindow->raise();
    m_eqWindow->activateWindow();
    return m_eqWindow;
}

DynamicsWindow* MixerStrip::openDynamics()
{
    if (!m_dynamicsWindow) {
        m_dynamicsWindow = new DynamicsWindow(m_controller, m_meters, m_target, window());
        m_dynamicsWindow->addActions(window()->actions()); // window-scoped shortcuts
    }
    m_dynamicsWindow->show();
    m_dynamicsWindow->raise();
    m_dynamicsWindow->activateWindow();
    return m_dynamicsWindow;
}

void MixerStrip::fillBusMenu()
{
    QMenu* menu = m_bus->menu();
    menu->clear();
    const TimelineData& d = m_controller->data();
    const TrackData* t = mixerops::audioTrack(d, m_target.id);
    if (!t)
        return;
    for (const BusData& b : d.mixer.buses) {
        QAction* a = menu->addAction(busDisplayName(b));
        a->setCheckable(true);
        a->setChecked(t->mix.buses.contains(b.id));
        const int id = b.id;
        connect(a, &QAction::triggered, this, [this, id](bool on) {
            m_controller->edit(
                on ? tr("Assign to Bus") : tr("Remove from Bus"), [&](TimelineData& data) {
                    return mixerops::setTrackRoute(data, m_target.id, id, on);
                });
        });
    }
}

void MixerStrip::showNameMenu(const QPoint& globalPos)
{
    const TimelineData& d = m_controller->data();
    QMenu menu;
    const bool bus = m_target.isBus();
    const bool mono = bus ? mixerops::bus(d, int(m_target.id))->mono
                          : mixerops::audioTrack(d, m_target.id)->mix.mono;
    QAction* monoAction = menu.addAction(tr("Mono"));
    monoAction->setCheckable(true);
    monoAction->setChecked(mono);
    QAction* stereoAction = menu.addAction(tr("Stereo"));
    stereoAction->setCheckable(true);
    stereoAction->setChecked(!mono);
    QAction* rename = nullptr;
    QAction* remove = nullptr;
    if (bus) {
        menu.addSeparator();
        rename = menu.addAction(tr("Rename Bus…"));
        remove = menu.addAction(tr("Delete Bus"));
        remove->setEnabled(m_target.id != quint64(kMainBusId));
    }
    QAction* chosen = menu.exec(globalPos);
    if (!chosen)
        return;
    if (chosen == monoAction || chosen == stereoAction) {
        const bool wantMono = chosen == monoAction;
        m_controller->edit(tr("Channel Format"), [&](TimelineData& data) {
            return bus ? mixerops::setBusMono(data, int(m_target.id), wantMono)
                       : mixerops::setTrackMono(data, m_target.id, wantMono);
        });
    } else if (chosen == rename) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename Bus"), tr("Name:"),
            QLineEdit::Normal, busDisplayName(*mixerops::bus(d, int(m_target.id))), &ok);
        if (ok)
            m_controller->edit(tr("Rename Bus"), [&](TimelineData& data) {
                return mixerops::renameBus(data, int(m_target.id), name.trimmed());
            });
    } else if (chosen == remove) {
        m_controller->edit(tr("Delete Bus"),
            [&](TimelineData& data) { return mixerops::removeBus(data, int(m_target.id)); });
    }
}

} // namespace kk
