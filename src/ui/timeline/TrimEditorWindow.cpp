// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/timeline/TrimEditorWindow.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/viewer/TrimTwoUpView.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace ops = editops;

namespace {

/// The side of the two-up showing @p clip's frame at timeline frame @p frame.
TrimPreviewSide sideOf(const ClipData* clip, int64_t frame, Rational fps)
{
    TrimPreviewSide side;
    if (!clip)
        return side;
    frame = std::clamp(frame, clip->start, clip->end() - 1);
    side.clipId = clip->id;
    side.mediaId = clip->kind == ClipKind::Media ? clip->mediaId : QString();
    side.sourceUs = std::max<int64_t>(0, clip->sourceTimeUsAt(frame, fps));
    side.timelineFrame = frame;
    side.name = clip->name;
    return side;
}

QString stepText(int frames)
{
    return frames > 0 ? QStringLiteral("+%1").arg(frames) : QString::number(frames);
}

} // namespace

TrimEditorWindow::Cut TrimEditorWindow::cutOf(
    const TimelineData& data, const EditPointSelection& edit)
{
    Cut cut {edit.tailClip, edit.headClip};
    const auto loc = ops::findClip(data, edit.tailClip ? edit.tailClip : edit.headClip);
    if (!loc)
        return {};
    const auto& clips = data.track(loc->track)->clips;
    const size_t i = size_t(loc->clipIndex);
    if (!cut.right && i + 1 < clips.size() && clips[i + 1].start == clips[i].end())
        cut.right = clips[i + 1].id;
    if (!cut.left && i > 0 && clips[i - 1].end() == clips[i].start)
        cut.left = clips[i - 1].id;
    return cut;
}

TrimPreview TrimEditorWindow::previewOf(const TimelineData& data, const EditPointSelection& edit)
{
    TrimPreview preview;
    if (edit.isEmpty())
        return preview;
    const Cut cut = cutOf(data, edit);
    if (const ClipData* left = ops::clipById(data, cut.left))
        preview.left = sideOf(left, left->end() - 1, data.fps);
    if (const ClipData* right = ops::clipById(data, cut.right))
        preview.right = sideOf(right, right->start, data.fps);
    return preview;
}

TrimEditorWindow::TrimEditorWindow(AppContext& ctx, TimelineController* controller, QWidget* parent)
    : QDialog(parent)
    , m_ctx(ctx)
    , m_controller(controller)
{
    setObjectName(QStringLiteral("TrimEditorWindow"));
    setWindowTitle(tr("Trim Editor"));
    setModal(false);
    resize(960, 560);
    auto* layout = new QVBoxLayout(this);

    // The pictures: the two-up view fills this canvas.
    auto* canvas = new QWidget;
    canvas->setObjectName(QStringLiteral("trimEditorCanvas"));
    canvas->setMinimumSize(480, 240);
    layout->addWidget(canvas, 1);
    m_view = new TrimTwoUpView(canvas, ctx.project ? ctx.project->mediaPool() : nullptr);
    if (ctx.playback && ctx.project)
        m_view->setProgramSource(ctx.playback, ctx.project->timeline());

    m_hint = new QLabel;
    m_hint->setObjectName(QStringLiteral("trimEditorHint"));
    m_hint->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_hint);

    // Outgoing | Roll | Incoming, each with the same steps.
    auto* row = new QHBoxLayout;
    const QString titles[3] = {tr("Outgoing"), tr("Roll"), tr("Incoming")};
    const char* names[3] = {"out", "roll", "in"};
    m_previous = new QPushButton(tr("Previous Edit"));
    m_previous->setObjectName(QStringLiteral("trimEditorPrevious"));
    row->addWidget(m_previous);
    for (int s = 0; s < 3; ++s) {
        auto* column = new QVBoxLayout;
        auto* title = new QLabel(titles[s]);
        title->setAlignment(Qt::AlignCenter);
        column->addWidget(title);
        auto* buttons = new QHBoxLayout;
        for (int b = 0; b < 6; ++b) {
            const int frames = kSteps[b];
            auto* button = new QPushButton(stepText(frames));
            button->setObjectName(
                QStringLiteral("trim_%1_%2").arg(QLatin1String(names[s])).arg(frames));
            button->setToolTip(tr("Trim %1 by %2 frame(s)").arg(titles[s]).arg(stepText(frames)));
            button->setAutoRepeat(false);
            connect(
                button, &QPushButton::clicked, this, [this, s, frames] { trim(Side(s), frames); });
            buttons->addWidget(button);
            m_buttons[s][b] = button;
        }
        column->addLayout(buttons);
        row->addLayout(column);
    }
    m_next = new QPushButton(tr("Next Edit"));
    m_next->setObjectName(QStringLiteral("trimEditorNext"));
    row->addWidget(m_next);
    layout->addLayout(row);
    connect(m_previous, &QPushButton::clicked, this, [this] { step(false); });
    connect(m_next, &QPushButton::clicked, this, [this] { step(true); });

    connect(controller, &TimelineController::stateChanged, this, &TrimEditorWindow::refresh);
    if (ctx.project)
        connect(ctx.project->timeline(), &Timeline::changed, this, &TrimEditorWindow::refresh);
    refresh();
}

bool TrimEditorWindow::trim(Side side, int frames)
{
    const EditPointSelection edit = m_controller->selectedEdit();
    if (edit.isEmpty() || frames == 0)
        return false;
    const Cut cut = cutOf(m_ctx.project->timeline()->data(), edit);
    EditPointSelection target;
    switch (side) {
    case Side::Outgoing:
        target = {cut.left, 0};
        break;
    case Side::Roll:
        target = {cut.left, cut.right};
        break;
    case Side::Incoming:
        target = {0, cut.right};
        break;
    }
    if (target.isEmpty() || (side == Side::Roll && (!cut.left || !cut.right)))
        return false;
    m_controller->setSelectedEdit(target);
    m_controller->nudge(frames);
    return true;
}

bool TrimEditorWindow::step(bool forward)
{
    const EditPointSelection before = m_controller->selectedEdit();
    if (before.isEmpty())
        return false;
    if (forward)
        m_controller->goToNextEdit();
    else
        m_controller->goToPreviousEdit();
    return !(m_controller->selectedEdit() == before);
}

void TrimEditorWindow::refresh()
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const EditPointSelection edit = m_controller->selectedEdit();
    const TrimPreview preview = previewOf(data, edit);
    const Cut cut = edit.isEmpty() ? Cut {} : cutOf(data, edit);
    if (edit.isEmpty() || !preview.isActive()) {
        m_view->setPreview({}, data.fps);
        m_hint->setText(tr("Select an edit point in the timeline."));
    } else {
        m_view->setPreview(preview, data.fps);
        m_hint->setText(QString());
    }
    for (int b = 0; b < 6; ++b) {
        m_buttons[int(Side::Outgoing)][b]->setEnabled(cut.left != 0);
        m_buttons[int(Side::Roll)][b]->setEnabled(cut.left != 0 && cut.right != 0);
        m_buttons[int(Side::Incoming)][b]->setEnabled(cut.right != 0);
    }
    m_previous->setEnabled(!edit.isEmpty());
    m_next->setEnabled(!edit.isEmpty());
}

void TrimEditorWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refresh();
}

} // namespace kk
