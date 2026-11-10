// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorStabilize — the Video tab's Stabilization section (spec section 58). Part of
// InspectorPanel. The controls only edit ClipData::stabilization (core/Stabilization.h); the
// Stabilize button runs tracking::StabilizeRunner on a worker thread with a progress bar and
// Cancel, and the finished measurement is stored in one undo step.
#include "compositing/tracking/StabilizeJob.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ValueField.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace kk {

namespace {

namespace st = stabilize;

constexpr int kSliderSteps = 1000;

/// One slider row: a drag field and a slider over [min, max], both editing through the callbacks.
struct SliderRow
{
    QWidget* widget = nullptr;
    ValueField* field = nullptr;
    QSlider* slider = nullptr;

    void show(double value)
    {
        if (!field->isScrubbing() && std::abs(field->value() - value) > 1e-9)
            field->setValue(value);
        const int pos = int(std::lround(
            (value - field->minimum()) / (field->maximum() - field->minimum()) * kSliderSteps));
        if (!slider->isSliderDown() && slider->value() != pos) {
            const QSignalBlocker block(slider);
            slider->setValue(pos);
        }
    }
};

} // namespace

void InspectorPanel::onStabilizeFinished(quint64 clipId)
{
    const tracking::StabilizeOutcome& result = m_stabilizer->outcome();
    if (result.cancelled) {
        m_stabilizeMessage = tr("Stabilization cancelled. The clip is unchanged.");
    } else if (!result.data) {
        m_stabilizeMessage = tr("Stabilize failed: %1.").arg(result.error);
    } else if (!clip(clipId)) {
        m_stabilizeMessage = tr("The clip was removed; the result was dropped.");
    } else {
        const auto data = result.data;
        editClip(tr("Stabilize"), clipId, [data](ClipData& c) {
            tracking::applyStabilization(c, c.stabilization.settings, data);
        });
        m_stabilizeMessage = result.lost > 0
            ? tr("Stabilized. %1 of %2 frame pairs could not be matched and were left as they are.")
                  .arg(result.lost)
                  .arg(result.pairs)
            : tr("Stabilized.");
    }
    m_stabilizeMessageClip = clipId;
    refreshValues();
}

void InspectorPanel::buildStabilizationSection(const ClipData& c)
{
    const quint64 clipId = m_clipId;
    ParamGroupBox* group
        = addGroup(VideoTab, QStringLiteral("stabilization"), tr("Stabilization"), clipId);
    group->setObjectName(QStringLiteral("StabilizationGroup"));
    attachHelp(group, QLatin1String(st::kDescriptorId), tr("Stabilization"));
    QCheckBox* enable = group->addEnableToggle();
    enable->setObjectName(QStringLiteral("StabilizationEnable"));
    connect(enable, &QCheckBox::clicked, this, [this, clipId](bool on) {
        editClip(on ? tr("Enable Stabilization") : tr("Disable Stabilization"), clipId,
            [on](ClipData& cd) { cd.stabilization.settings.enabled = on; });
    });

    auto* mode = new QComboBox;
    mode->setObjectName(QStringLiteral("StabilizationMode"));
    mode->addItem(tr("Perspective"), int(st::Mode::Perspective));
    mode->addItem(tr("Similarity"), int(st::Mode::Similarity));
    mode->addItem(tr("Translation"), int(st::Mode::Translation));
    mode->setToolTip(tr("What kind of camera movement is corrected: Perspective is the full "
                        "movement, Similarity is pan, tilt, zoom and rotation, Translation is "
                        "pan and tilt only. Press Stabilize again after changing it."));
    group->addWidgetRow(tr("Mode"), mode);
    connect(mode, &QComboBox::activated, this, [this, clipId, mode](int index) {
        const auto value = st::Mode(mode->itemData(index).toInt());
        editClip(tr("Stabilization Mode"), clipId,
            [value](ClipData& cd) { cd.stabilization.settings.mode = value; });
    });

    auto* lock = new QCheckBox(tr("Camera Lock"));
    lock->setObjectName(QStringLiteral("StabilizationCameraLock"));
    lock->setToolTip(tr("Remove all camera movement, as if the camera stood on a tripod"));
    group->addWidgetRow(QString(), lock);
    connect(lock, &QCheckBox::clicked, this, [this, clipId](bool on) {
        editClip(tr("Camera Lock"), clipId,
            [on](ClipData& cd) { cd.stabilization.settings.cameraLock = on; });
    });
    auto* zoom = new QCheckBox(tr("Zoom"));
    zoom->setObjectName(QStringLiteral("StabilizationZoom"));
    zoom->setToolTip(tr("Zoom in just enough to hide the empty edges the correction leaves"));
    group->addWidgetRow(QString(), zoom);
    connect(zoom, &QCheckBox::clicked, this, [this, clipId](bool on) {
        editClip(tr("Stabilization Zoom"), clipId,
            [on](ClipData& cd) { cd.stabilization.settings.zoom = on; });
    });

    // Cropping Ratio, Smooth and Strength: a drag field with a slider; a drag is one undo step.
    auto addSlider
        = [&](const QString& label, const QString& name, double min, double max, double step,
              const QString& tip, std::function<void(st::Settings&, double)> set) {
              SliderRow row;
              row.widget = new QWidget;
              auto* layout = new QHBoxLayout(row.widget);
              layout->setContentsMargins(0, 0, 0, 0);
              row.slider = new QSlider(Qt::Horizontal);
              row.slider->setRange(0, kSliderSteps);
              row.slider->setFocusPolicy(Qt::NoFocus);
              row.field = new ValueField;
              row.field->setRange(min, max);
              row.field->setStep(step);
              row.field->setDecimals(2);
              row.widget->setObjectName(name);
              row.field->setObjectName(name + QStringLiteral("Field"));
              row.slider->setObjectName(name + QStringLiteral("Slider"));
              row.widget->setToolTip(tip);
              layout->addWidget(row.slider, 1);
              layout->addWidget(row.field);
              group->addWidgetRow(label, row.widget);
              auto apply = [this, clipId, set](double v) {
                  modifyClip(clipId, [set, v](ClipData& cd) { set(cd.stabilization.settings, v); });
              };
              connect(row.field, &ValueField::scrubStarted, this,
                  [this, label] { beginParamEdit(tr("Stabilization %1").arg(label)); });
              connect(row.field, &ValueField::valueScrubbed, this, apply);
              connect(row.field, &ValueField::scrubFinished, this, [this] { endParamEdit(); });
              connect(row.field, &ValueField::valueCommitted, this, [this, label, apply](double v) {
                  beginParamEdit(tr("Stabilization %1").arg(label));
                  apply(v);
                  endParamEdit();
              });
              connect(row.slider, &QSlider::sliderPressed, this,
                  [this, label] { beginParamEdit(tr("Stabilization %1").arg(label)); });
              connect(row.slider, &QSlider::sliderReleased, this, [this] { endParamEdit(); });
              connect(row.slider, &QSlider::valueChanged, this, [row, min, max, apply](int pos) {
                  if (!row.slider->isSliderDown())
                      return;
                  const double v = min + (max - min) * pos / kSliderSteps;
                  row.field->setValue(v);
                  apply(v);
              });
              return row;
          };
    const SliderRow cropping = addSlider(tr("Cropping Ratio"),
        QStringLiteral("StabilizationCropping"), st::kMinCroppingRatio, st::kMaxCroppingRatio,
        0.005,
        tr("How far Zoom may crop the picture: the picture is enlarged at most (1 + ratio) times"),
        [](st::Settings& s, double v) { s.croppingRatio = v; });
    const SliderRow smooth = addSlider(tr("Smooth"), QStringLiteral("StabilizationSmooth"),
        st::kMinSmooth, st::kMaxSmooth, 0.005,
        tr("How smooth the new camera path is; 0 follows the camera, 1 is the steadiest"),
        [](st::Settings& s, double v) { s.smooth = v; });
    const SliderRow strength = addSlider(tr("Strength"), QStringLiteral("StabilizationStrength"),
        st::kMinStrength, st::kMaxStrength, 0.005,
        tr("How much of the correction is applied: 0 is off, 1 is full, below 0 shakes the "
           "picture the opposite way"),
        [](st::Settings& s, double v) { s.strength = v; });

    // Stabilize, Cancel, progress and status.
    auto* buttons = new QWidget;
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    auto* stabilizeButton = new QPushButton(tr("Stabilize"));
    stabilizeButton->setObjectName(QStringLiteral("StabilizationStabilize"));
    stabilizeButton->setToolTip(tr("Measure the camera movement of the clip. Press it again "
                                   "after changing the Mode."));
    auto* cancelButton = new QPushButton(tr("Cancel"));
    cancelButton->setObjectName(QStringLiteral("StabilizationCancel"));
    buttonLayout->addWidget(stabilizeButton);
    buttonLayout->addWidget(cancelButton);
    buttonLayout->addStretch(1);
    group->addWidgetRow(QString(), buttons);
    auto* progress = new QProgressBar;
    progress->setObjectName(QStringLiteral("StabilizationProgress"));
    progress->setTextVisible(true);
    group->addWidgetRow(QString(), progress);
    auto* status = new QLabel;
    status->setObjectName(QStringLiteral("StabilizationStatus"));
    status->setWordWrap(true);
    group->addWidgetRow(QString(), status);

    connect(stabilizeButton, &QPushButton::clicked, this, [this, clipId] {
        const ClipData* shown = clip(clipId);
        if (!shown || m_stabilizer->isRunning())
            return;
        const MediaItem* item = m_ctx.project->mediaPool()->item(shown->mediaId);
        QString why;
        std::optional<tracking::StabilizeJob> job;
        if (item)
            job = tracking::planStabilizeJob(*shown, *item, m_ctx.project->timeline()->data().fps,
                shown->stabilization.settings.mode, &why);
        else
            why = tr("the media of the clip is missing");
        if (!job) {
            m_stabilizeMessage = tr("Cannot stabilize: %1.").arg(why);
            m_stabilizeMessageClip = clipId;
            refreshValues();
            return;
        }
        m_stabilizeMessage.clear();
        m_stabilizer->start(clipId, std::move(*job));
        refreshValues();
    });
    connect(cancelButton, &QPushButton::clicked, this, [this] { m_stabilizer->cancel(); });
    connect(m_stabilizer, &tracking::StabilizeRunner::progress, group, [this, progress](int, int) {
        progress->setMaximum(std::max(1, m_stabilizer->total()));
        progress->setValue(m_stabilizer->done());
    });

    QToolButton* reset = group->addHeaderButton(InspectorGlyph::Reset, tr("Reset all"));
    connect(reset, &QToolButton::clicked, this, [this, clipId] {
        editClip(tr("Reset Stabilization"), clipId,
            [](ClipData& cd) { cd.stabilization = stabilize::ClipStabilization {}; });
    });

    const bool canStabilize = c.kind == ClipKind::Media;
    group->setHeaderRefresh(
        [this, clipId, enable, mode, lock, zoom, cropping, smooth, strength, stabilizeButton,
            cancelButton, progress, status, canStabilize](const ClipData& cd) {
            const st::Settings& s = cd.stabilization.settings;
            if (enable->isChecked() != s.enabled)
                enable->setChecked(s.enabled);
            const int index = mode->findData(int(s.mode));
            if (index >= 0 && mode->currentIndex() != index) {
                const QSignalBlocker block(mode);
                mode->setCurrentIndex(index);
            }
            if (lock->isChecked() != s.cameraLock)
                lock->setChecked(s.cameraLock);
            if (zoom->isChecked() != s.zoom)
                zoom->setChecked(s.zoom);
            SliderRow rows[] = {cropping, smooth, strength};
            const double values[] = {s.croppingRatio, s.smooth, s.strength};
            for (int i = 0; i < 3; ++i)
                rows[i].show(values[i]);
            // Camera Lock removes all movement: Smooth and Cropping Ratio have nothing to do.
            cropping.widget->setEnabled(!s.cameraLock);
            smooth.widget->setEnabled(!s.cameraLock);

            const bool running = m_stabilizer->isRunning();
            const bool mine = running && m_stabilizer->clipId() == clipId;
            stabilizeButton->setEnabled(canStabilize && !running);
            cancelButton->setVisible(mine);
            progress->setVisible(mine);
            if (mine) {
                progress->setMaximum(std::max(1, m_stabilizer->total()));
                progress->setValue(m_stabilizer->done());
            }
            QString text;
            if (mine) {
                text = tr("Analysing the clip...");
            } else if (running) {
                text = tr("Another clip is being analysed.");
            } else if (st::needsAnalysis(cd.stabilization)) {
                text = tr("The Mode changed since the analysis. Press Stabilize again.");
            } else if (m_stabilizeMessageClip == clipId && !m_stabilizeMessage.isEmpty()) {
                text = m_stabilizeMessage;
            } else if (!canStabilize) {
                text = tr("Only clips of video files can be stabilized.");
            } else if (!cd.stabilization.data) {
                text = tr("Not analysed yet. Press Stabilize.");
            } else {
                text = tr("Analysed: source frames %1 to %2.")
                           .arg(cd.stabilization.data->firstFrame())
                           .arg(cd.stabilization.data->lastFrame());
            }
            if (status->text() != text)
                status->setText(text);
        });
}

} // namespace kk
