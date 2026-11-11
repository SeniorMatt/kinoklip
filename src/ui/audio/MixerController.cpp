// SPDX-License-Identifier: GPL-3.0-or-later
// MixerController — undo-step handling and automation recording of the mixer widgets.
#include "ui/audio/MixerController.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
/// A playhead step longer than this (frames) or backwards is a jump (loop, seek): the
/// recording does not erase the keys between the old and the new position.
constexpr int64_t kMaxRecordStepFrames = 50;
} // namespace

MixerController::MixerController(AppContext& ctx, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
{
    connect(m_ctx.project->timeline(), &Timeline::playheadChanged, this,
        &MixerController::onPlayheadChanged);
    connect(m_ctx.project->timeline(), &Timeline::changed, this, &MixerController::mixChanged);
    if (m_ctx.playback) {
        connect(
            m_ctx.playback, &PlaybackEngine::stateChanged, this, &MixerController::onPlaybackState);
    }
}

VoiceOverController* MixerController::voiceOver() const
{
    return m_ctx.voiceOver;
}

MixerController::~MixerController()
{
    if (m_gesture)
        m_ctx.project->endEdit();
    stopRecordings();
}

const TimelineData& MixerController::data() const
{
    return m_ctx.project->timeline()->data();
}

QString MixerController::automationKey(Control control)
{
    return control == Control::Pan ? mixkeys::kPan : mixkeys::kVolumeDb;
}

bool MixerController::playing() const
{
    if (m_playingProbe)
        return m_playingProbe();
    return m_ctx.playback && m_ctx.playback->isPlaying();
}

void MixerController::edit(const QString& label, const std::function<bool(TimelineData&)>& change)
{
    m_ctx.project->beginEdit(label);
    if (!change(m_ctx.project->timeline()->mutableData())) {
        m_ctx.project->cancelEdit();
        return;
    }
    m_ctx.project->endEdit();
}

void MixerController::beginGesture(const QString& label)
{
    if (m_gesture)
        endGesture();
    m_ctx.project->beginEdit(label);
    m_gesture = true;
}

void MixerController::updateGesture(const std::function<bool(TimelineData&)>& change)
{
    if (!m_gesture)
        return;
    if (change(m_ctx.project->timeline()->mutableData()))
        m_ctx.project->timeline()->notifyChanged();
}

void MixerController::endGesture()
{
    if (!m_gesture)
        return;
    m_gesture = false;
    m_ctx.project->endEdit();
}

double MixerController::controlValue(const MixTarget& target, Control control, double frame) const
{
    const TimelineData& d = data();
    if (target.isBus()) {
        const BusData* b = mixerops::bus(d, int(target.id));
        return !b ? 0.0 : control == Control::Fader ? b->strip.volumeDb : 0.0;
    }
    const TrackData* t = mixerops::audioTrack(d, target.id);
    if (!t)
        return 0.0;
    return control == Control::Fader ? mixerops::faderDbAt(*t, frame) : mixerops::panAt(*t, frame);
}

bool MixerController::applyStatic(
    TimelineData& d, const MixTarget& target, Control control, double value)
{
    ChannelStrip* s = mixerops::strip(d, target);
    if (!s)
        return false;
    if (!target.isBus()) {
        const TrackData* t = mixerops::audioTrack(d, target.id);
        if (mixerops::readsAutomation(t->mix, automationKey(control))) {
            // The curve plays: the control edits the key at the playhead.
            const int64_t frame = m_ctx.project->timeline()->playhead();
            return mixerops::writeAutomation(
                d, target.id, automationKey(control), frame, value, frame);
        }
    }
    double& field = control == Control::Pan ? s->pan : s->volumeDb;
    const double clamped = control == Control::Pan ? std::clamp(value, -1.0, 1.0)
                                                   : std::clamp(value, kFaderMinDb, kFaderMaxDb);
    if (field == clamped)
        return false;
    field = clamped;
    return true;
}

MixerController::Recording* MixerController::recording(quint64 trackId, Control control)
{
    for (Recording& r : m_recordings) {
        if (r.trackId == trackId && r.control == control)
            return &r;
    }
    return nullptr;
}

void MixerController::writeRecording(Recording& r, int64_t frame)
{
    const bool jump = frame < r.lastFrame || frame - r.lastFrame > kMaxRecordStepFrames;
    const int64_t eraseFrom = jump ? frame : r.lastFrame;
    if (mixerops::writeAutomation(m_ctx.project->timeline()->mutableData(), r.trackId,
            automationKey(r.control), frame, r.value, eraseFrom))
        m_ctx.project->timeline()->notifyChanged();
    r.lastFrame = frame;
}

void MixerController::pressControl(const MixTarget& target, Control control)
{
    const TrackData* t = target.isBus() ? nullptr : mixerops::audioTrack(data(), target.id);
    const bool records = t && playing()
        && (t->mix.automation == AutomationMode::Touch
            || t->mix.automation == AutomationMode::Write);
    if (!records) {
        beginGesture(control == Control::Pan ? tr("Pan") : tr("Fader"));
        return;
    }
    if (Recording* r = recording(target.id, control)) {
        r->touched = true; // Write pass already running for this control
        return;
    }
    if (!m_recordEditOpen) {
        m_ctx.project->beginEdit(tr("Write Automation"));
        m_recordEditOpen = true;
    }
    const int64_t frame = m_ctx.project->timeline()->playhead();
    Recording r;
    r.trackId = target.id;
    r.control = control;
    r.value = controlValue(target, control, double(frame));
    r.lastFrame = frame;
    r.touched = true;
    m_recordings.append(r);
}

void MixerController::moveControl(const MixTarget& target, Control control, double value)
{
    if (Recording* r = target.isBus() ? nullptr : recording(target.id, control)) {
        r->value = value;
        writeRecording(*r, m_ctx.project->timeline()->playhead());
        return;
    }
    updateGesture([&](TimelineData& d) { return applyStatic(d, target, control, value); });
}

void MixerController::releaseControl(const MixTarget& target, Control control)
{
    if (Recording* r = target.isBus() ? nullptr : recording(target.id, control)) {
        const TrackData* t = mixerops::audioTrack(data(), target.id);
        if (t && t->mix.automation == AutomationMode::Write) {
            r->touched = false; // a Write pass keeps writing the last value until stop
            return;
        }
        m_recordings.erase(m_recordings.begin() + (r - m_recordings.data()));
        if (m_recordings.isEmpty() && m_recordEditOpen) {
            m_recordEditOpen = false;
            m_ctx.project->endEdit();
        }
        return;
    }
    endGesture();
}

void MixerController::setControl(const MixTarget& target, Control control, double value)
{
    edit(control == Control::Pan ? tr("Pan") : tr("Fader"),
        [&](TimelineData& d) { return applyStatic(d, target, control, value); });
}

void MixerController::setTrackMuted(quint64 trackId, bool muted)
{
    edit(muted ? tr("Mute Track") : tr("Unmute Track"), [&](TimelineData& d) {
        TrackData* t = mixerops::audioTrack(d, trackId);
        if (!t || t->muted == muted)
            return false;
        t->muted = muted;
        return true;
    });
}

void MixerController::setTrackSolo(quint64 trackId, bool solo)
{
    edit(solo ? tr("Solo Track") : tr("Unsolo Track"), [&](TimelineData& d) {
        TrackData* t = mixerops::audioTrack(d, trackId);
        if (!t || t->solo == solo)
            return false;
        t->solo = solo;
        return true;
    });
}

void MixerController::onPlayheadChanged(int64_t frame)
{
    if (m_recordings.isEmpty())
        return;
    if (!playing()) {
        stopRecordings();
        return;
    }
    for (Recording& r : m_recordings)
        writeRecording(r, frame);
}

void MixerController::onPlaybackState()
{
    if (playing())
        startWritePasses();
    else
        stopRecordings();
}

void MixerController::startWritePasses()
{
    if (!m_writePasses)
        return;
    const int64_t frame = m_ctx.project->timeline()->playhead();
    for (const TrackData& t : data().audioTracks) {
        if (t.mix.automation != AutomationMode::Write)
            continue;
        for (Control c : {Control::Fader, Control::Pan}) {
            if (recording(t.id, c))
                continue;
            if (!m_recordEditOpen) {
                m_ctx.project->beginEdit(tr("Write Automation"));
                m_recordEditOpen = true;
            }
            Recording r;
            r.trackId = t.id;
            r.control = c;
            r.value = controlValue(MixTarget::track(t.id), c, double(frame));
            r.lastFrame = frame;
            m_recordings.append(r);
            writeRecording(m_recordings.back(), frame);
        }
    }
}

void MixerController::stopRecordings()
{
    m_recordings.clear();
    if (m_recordEditOpen) {
        m_recordEditOpen = false;
        m_ctx.project->endEdit();
    }
}

} // namespace kk
