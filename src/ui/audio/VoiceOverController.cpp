// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/audio/VoiceOverController.h"

#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "media/WavWriter.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaStore.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr int kDecayMs = 33;
constexpr int kClipHoldMs = 2000;
constexpr const char* kBinName = "Voice Over";

QString safeFileName(QString name)
{
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
    name.replace(bad, QStringLiteral("_"));
    name = name.trimmed();
    return name.isEmpty() ? QStringLiteral("Audio") : name;
}

} // namespace

VoiceOverController::VoiceOverController(
    AppContext& ctx, AudioInputFactory factory, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_factory(std::move(factory))
    , m_lister(&systemAudioInputs)
    , m_problemCheck(&systemAudioInputProblem)
{
    ctx.voiceOver = this;
    m_decayTimer.setInterval(kDecayMs);
    connect(&m_decayTimer, &QTimer::timeout, this, &VoiceOverController::decay);
    m_countTimer.setInterval(1000);
    m_countTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_countTimer, &QTimer::timeout, this, &VoiceOverController::tickCountIn);
    if (ctx.project) {
        connect(ctx.project, &Project::activeTimelineChanged, this, [this] { checkArmedTrack(); });
        connect(ctx.project->timeline(), &Timeline::changed, this, [this] { checkArmedTrack(); });
    }
}

VoiceOverController::~VoiceOverController()
{
    if (m_state != State::Idle) {
        m_countTimer.stop();
        if (m_writer) {
            m_writer->close();
            QFile::remove(m_takePath);
        }
    }
    if (m_ctx.voiceOver == this)
        m_ctx.voiceOver = nullptr;
}

void VoiceOverController::setDeviceSource(
    std::function<QList<AudioInputDevice>()> lister, std::function<QString()> problemCheck)
{
    m_lister = std::move(lister);
    m_problemCheck = std::move(problemCheck);
}

// ---- Arming ----

std::optional<TrackRef> VoiceOverController::armedRef() const
{
    if (!m_armedTrack || !m_ctx.project)
        return std::nullopt;
    if (m_ctx.project->activeTimelineUid() != m_armedTimeline)
        return std::nullopt;
    const TimelineData& data = m_ctx.project->timeline()->data();
    for (size_t i = 0; i < data.audioTracks.size(); ++i) {
        if (data.audioTracks[i].id == m_armedTrack)
            return TrackRef {TrackType::Audio, int(i)};
    }
    return std::nullopt;
}

void VoiceOverController::refuse(const QString& text)
{
    m_lastMessage = text;
    emit message(text, true);
}

QString VoiceOverController::problem() const
{
    if (!m_deviceProblem.isEmpty())
        return m_deviceProblem;
    return m_problemCheck ? m_problemCheck() : QString();
}

bool VoiceOverController::arm(quint64 trackId)
{
    if (m_state != State::Idle) {
        refuse(tr("Stop the recording before arming another track."));
        return false;
    }
    if (trackId == m_armedTrack && m_input)
        return true;
    const TimelineData& data = m_ctx.project->timeline()->data();
    const TrackData* track = nullptr;
    for (const TrackData& t : data.audioTracks) {
        if (t.id == trackId)
            track = &t;
    }
    if (!track) {
        refuse(tr("Only audio tracks can be armed."));
        return false;
    }
    if (track->locked) {
        refuse(tr("The track is locked. Unlock it to record onto it."));
        return false;
    }
    m_deviceProblem.clear();
    const QString why = problem();
    if (!why.isEmpty()) {
        refuse(why);
        return false;
    }
    const quint64 previous = m_armedTrack;
    m_armedTrack = trackId;
    m_armedTimeline = m_ctx.project->activeTimelineUid();
    openInput();
    if (!m_input) {
        m_armedTrack = 0;
        m_armedTimeline = 0;
        if (previous)
            emit armedChanged();
        refuse(m_deviceProblem.isEmpty() ? tr("The microphone could not be opened.")
                                         : m_deviceProblem);
        return false;
    }
    m_lastMessage.clear();
    emit armedChanged();
    return true;
}

void VoiceOverController::disarm()
{
    if (m_state != State::Idle)
        stopRecording();
    if (!m_armedTrack)
        return;
    closeInput();
    m_armedTrack = 0;
    m_armedTimeline = 0;
    emit armedChanged();
}

bool VoiceOverController::toggleArm(quint64 trackId)
{
    if (isArmed(trackId)) {
        disarm();
        return true;
    }
    return arm(trackId);
}

void VoiceOverController::checkArmedTrack()
{
    if (!m_armedTrack || m_state != State::Idle)
        return;
    const auto ref = armedRef();
    const TrackData* track = ref ? m_ctx.project->timeline()->data().track(*ref) : nullptr;
    if (!track || track->locked)
        disarm();
}

// ---- Input ----

QList<AudioInputDevice> VoiceOverController::inputs() const
{
    return m_lister ? m_lister() : QList<AudioInputDevice>();
}

QByteArray VoiceOverController::inputFor(quint64 trackId) const
{
    const auto it = m_inputs.constFind(trackId);
    if (it != m_inputs.constEnd())
        return *it;
    return prefs::audioInputDevice().toUtf8();
}

void VoiceOverController::setInputFor(quint64 trackId, const QByteArray& deviceId)
{
    if (deviceId.isEmpty())
        m_inputs.remove(trackId);
    else
        m_inputs.insert(trackId, deviceId);
    emit inputChanged();
    if (isArmed(trackId) && m_state == State::Idle) {
        openInput();
        if (!m_input) {
            disarm();
            refuse(m_deviceProblem);
        }
    }
}

QString VoiceOverController::inputName(quint64 trackId) const
{
    const QByteArray id = inputFor(trackId);
    const QList<AudioInputDevice> devices = inputs();
    if (!id.isEmpty()) {
        for (const AudioInputDevice& d : devices) {
            if (d.id == id)
                return d.description;
        }
    }
    if (!devices.isEmpty())
        return devices.first().description;
    return tr("No input");
}

void VoiceOverController::openInput()
{
    closeInput();
    m_deviceProblem.clear();
    QString error;
    m_input = m_factory ? m_factory(inputFor(m_armedTrack), &error) : nullptr;
    if (!m_input) {
        m_deviceProblem = error.isEmpty() ? tr("No microphone was found.") : error;
        return;
    }
    connect(m_input.get(), &AudioInput::samples, this, &VoiceOverController::onSamples);
    connect(m_input.get(), &AudioInput::failed, this, &VoiceOverController::onInputFailed);
    if (!m_input->start()) {
        m_deviceProblem = m_input->error().isEmpty() ? tr("The microphone could not be opened.")
                                                     : m_input->error();
        m_input.reset();
        return;
    }
    m_decayTimer.start();
}

void VoiceOverController::closeInput()
{
    m_decayTimer.stop();
    if (m_input) {
        m_input->disconnect(this);
        m_input->stop();
        m_input.release()->deleteLater(); // may be called from the input's own signal
    }
    m_peak = 0.0f;
    m_clipTimer.invalidate();
    emit levelChanged();
}

void VoiceOverController::onInputFailed(const QString& text)
{
    m_deviceProblem = text;
    if (m_state == State::Recording) {
        // Keep what was recorded so far: the take is finished as if Stop was pressed.
        emit message(text, true);
        stopRecording();
    } else if (m_state == State::CountIn) {
        stopRecording();
        refuse(text);
    }
    if (m_armedTrack && m_state == State::Idle) {
        closeInput();
        m_armedTrack = 0;
        m_armedTimeline = 0;
        emit armedChanged();
        refuse(text);
    }
}

// ---- Level ----

bool VoiceOverController::clipped() const
{
    return m_clipTimer.isValid() && m_clipTimer.elapsed() < kClipHoldMs;
}

void VoiceOverController::decay()
{
    const float before = m_peak;
    m_peak = std::max(m_blockPeak, m_peak * 0.82f);
    m_blockPeak = 0.0f;
    if (m_peak < 0.002f)
        m_peak = 0.0f;
    if (m_peak != before)
        emit levelChanged();
}

void VoiceOverController::onSamples(const std::vector<float>& block)
{
    if (block.empty())
        return;
    float peak = 0.0f;
    for (const float s : block)
        peak = std::max(peak, std::fabs(s));
    m_blockPeak = std::max(m_blockPeak, peak);
    m_peak = std::max(m_peak, peak);
    if (peak >= 0.99f)
        m_clipTimer.start();
    if (m_state == State::Recording && m_writer && m_input) {
        const int channels = std::max(1, m_input->channels());
        const int64_t frames = int64_t(block.size()) / channels;
        if (!m_writer->write(block.data(), frames)) {
            abortTake(tr("The recording could not be written: %1").arg(m_writer->error()));
            return;
        }
    }
    if (!m_levelEmit.isValid() || m_levelEmit.elapsed() >= kDecayMs) {
        m_levelEmit.start();
        emit levelChanged();
    }
}

// ---- Recording ----

void VoiceOverController::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

bool VoiceOverController::startRecording()
{
    if (m_state != State::Idle) {
        refuse(tr("A recording is already under way."));
        return false;
    }
    if (!m_armedTrack) {
        refuse(tr("Arm an audio track first: click the R button of the track."));
        return false;
    }
    const auto ref = armedRef();
    const TrackData* track = ref ? m_ctx.project->timeline()->data().track(*ref) : nullptr;
    if (!track) {
        disarm();
        refuse(tr("The armed track is gone. Arm a track again."));
        return false;
    }
    if (track->locked) {
        refuse(tr("The track is locked. Unlock it to record onto it."));
        return false;
    }
    if (!m_input) {
        openInput();
        if (!m_input) {
            refuse(m_deviceProblem);
            return false;
        }
    }
    if (m_ctx.playback) {
        if (m_ctx.playback->isPlaying()) {
            refuse(tr("Stop playback before recording."));
            return false;
        }
        const int64_t end = m_ctx.playback->duration();
        if (end <= 0 || m_ctx.project->timeline()->playhead() >= end) {
            refuse(tr("There is nothing to play from the playhead on. Move the playhead before "
                      "the end of the timeline: recording stops where playback stops."));
            return false;
        }
    }
    m_lastMessage.clear();
    m_countRemaining = prefs::voiceOverCountInSeconds();
    if (m_countRemaining <= 0) {
        m_countRemaining = 0;
        beginTake();
        return m_state == State::Recording;
    }
    setState(State::CountIn);
    emit countInTick(m_countRemaining);
    m_countTimer.start();
    return true;
}

void VoiceOverController::tickCountIn()
{
    if (m_state != State::CountIn) {
        m_countTimer.stop();
        return;
    }
    if (--m_countRemaining > 0) {
        emit countInTick(m_countRemaining);
        return;
    }
    m_countTimer.stop();
    m_countRemaining = 0;
    beginTake();
}

QString VoiceOverController::takeFolder() const
{
    if (m_ctx.mediaStore && m_ctx.mediaStore->storesMedia()) {
        // The take is copied into the project file when it is finished; it is written aside.
        return QStandardPaths::writableLocation(QStandardPaths::TempLocation)
            + QStringLiteral("/kinoklip-voice-over");
    }
    const QString project = m_ctx.project ? m_ctx.project->filePath() : QString();
    if (!project.isEmpty())
        return QFileInfo(project).absolutePath() + QStringLiteral("/Voice Over");
    // Never saved: the application's data folder (Save As moves nothing; relink if needed).
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/Voice Over");
}

QString VoiceOverController::takeBaseName(quint64 trackId) const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    for (size_t i = 0; i < data.audioTracks.size(); ++i) {
        if (data.audioTracks[i].id == trackId)
            return safeFileName(trackDisplayName(data.audioTracks[i], int(i)));
    }
    return QStringLiteral("Audio");
}

int VoiceOverController::nextTakeNumber(const QString& base) const
{
    const QRegularExpression re(
        QStringLiteral("^%1 take (\\d+)\\.wav$").arg(QRegularExpression::escape(base)),
        QRegularExpression::CaseInsensitiveOption);
    int highest = 0;
    auto consider = [&](const QString& fileName) {
        const auto m = re.match(fileName);
        if (m.hasMatch())
            highest = std::max(highest, m.captured(1).toInt());
    };
    const QDir dir(takeFolder());
    for (const QString& f : dir.entryList(QDir::Files))
        consider(f);
    if (m_ctx.project) {
        for (const MediaItem& item : m_ctx.project->mediaPool()->items()) {
            consider(item.name);
            consider(QFileInfo(item.path).fileName());
        }
    }
    return highest + 1;
}

QString VoiceOverController::nextTakeFileName(quint64 trackId) const
{
    const QString base = takeBaseName(trackId);
    return QStringLiteral("%1 take %2.wav").arg(base).arg(nextTakeNumber(base));
}

void VoiceOverController::beginTake()
{
    const QString base = takeBaseName(m_armedTrack);
    m_takeNumber = nextTakeNumber(base);
    const QString folder = takeFolder();
    if (!QDir().mkpath(folder)) {
        setState(State::Idle);
        refuse(tr("The folder for the take could not be created: %1").arg(folder));
        return;
    }
    m_takePath
        = QDir(folder).filePath(QStringLiteral("%1 take %2.wav").arg(base).arg(m_takeNumber));
    m_writer = std::make_unique<media::WavWriter>();
    if (!m_writer->open(m_takePath, m_input->sampleRate(), std::max(1, m_input->channels()),
            prefs::voiceOverBits())) {
        const QString why = m_writer->error();
        m_writer.reset();
        setState(State::Idle);
        refuse(tr("The take could not be created: %1").arg(why));
        return;
    }
    m_startFrame = m_ctx.project->timeline()->playhead();
    m_finishing = false;
    setState(State::Recording);
    emit countInTick(0);
    if (m_ctx.playback) {
        m_rateConnection
            = connect(m_ctx.playback, &PlaybackEngine::rateChanged, this, [this](double rate) {
                  if (rate == 0.0 && m_state == State::Recording && !m_finishing)
                      stopRecording();
              });
        m_ctx.playback->play();
    }
}

bool VoiceOverController::cancelCountIn()
{
    if (m_state != State::CountIn)
        return false;
    stopRecording();
    return true;
}

void VoiceOverController::toggleRecording()
{
    if (m_state == State::Idle)
        startRecording();
    else
        stopRecording();
}

void VoiceOverController::stopRecording()
{
    if (m_state == State::Idle || m_finishing)
        return;
    if (m_state == State::CountIn) {
        m_countTimer.stop();
        m_countRemaining = 0;
        setState(State::Idle);
        emit countInTick(0);
        return;
    }
    m_finishing = true;
    QObject::disconnect(m_rateConnection);
    if (m_ctx.playback && m_ctx.playback->isPlaying())
        m_ctx.playback->stop();
    finishTake();
    m_finishing = false;
}

void VoiceOverController::abortTake(const QString& text)
{
    m_finishing = true;
    QObject::disconnect(m_rateConnection);
    if (m_ctx.playback && m_ctx.playback->isPlaying())
        m_ctx.playback->stop();
    if (m_writer) {
        m_writer->close();
        m_writer.reset();
    }
    QFile::remove(m_takePath);
    setState(State::Idle);
    m_finishing = false;
    refuse(text);
}

void VoiceOverController::finishTake()
{
    const int rate = m_input ? m_input->sampleRate() : 48000;
    const int channels = m_input ? std::max(1, m_input->channels()) : 1;
    const int64_t latencyUs
        = (m_input ? m_input->latencyUs() : 0) + int64_t(prefs::voiceOverLatencyMs()) * 1000;
    const bool closed = m_writer && m_writer->close();
    const int64_t sampleFrames = m_writer ? m_writer->framesWritten() : 0;
    const QString writeError = m_writer ? m_writer->error() : QString();
    m_writer.reset();
    setState(State::Idle);

    if (!closed || sampleFrames < 1) {
        QFile::remove(m_takePath);
        const QString text = closed ? tr("Nothing was recorded: the microphone delivered no sound.")
                                    : tr("The take could not be saved: %1").arg(writeError);
        refuse(text);
        return;
    }

    Take take;
    take.path = m_takePath;
    take.number = m_takeNumber;
    take.startFrame = m_startFrame;
    take.sampleFrames = sampleFrames;
    take.sampleRate = rate;
    take.channels = channels;
    take.latencyUs = latencyUs;
    take.trackName = takeBaseName(m_armedTrack);

    // A single-file project keeps its media inside the file: copy the take in first.
    QStringList paths {m_takePath};
    if (m_ctx.mediaStore && m_ctx.mediaStore->storesMedia()) {
        if (!m_ctx.mediaStore->storeMedia(paths, QApplication::activeWindow())) {
            m_lastTake = take;
            refuse(tr("The take was recorded but not added to the project. It is saved as %1")
                       .arg(QDir::toNativeSeparators(m_takePath)));
            return;
        }
        take.path = paths.first();
        if (take.path != m_takePath)
            QFile::remove(m_takePath);
    }

    Project* project = m_ctx.project;
    MediaPool* pool = project->mediaPool();
    const auto ref = armedRef();
    project->beginEdit(tr("Voice Over"));
    QString binId;
    for (const Bin& b : pool->bins()) {
        if (b.name == QLatin1String(kBinName) && b.parentId.isEmpty()) {
            binId = b.id;
            break;
        }
    }
    if (binId.isEmpty())
        binId = pool->addBin(QString::fromLatin1(kBinName));
    const ImportResult imported = project->importMedia({take.path}, binId);
    if (imported.importedIds.isEmpty()) {
        project->cancelEdit();
        m_lastTake = take;
        refuse(tr("The take could not be added to the media pool: %1")
                   .arg(imported.warnings.join(QLatin1Char(' '))));
        return;
    }
    take.mediaId = imported.importedIds.first();
    const MediaItem* item = pool->item(take.mediaId);
    const TimelineData& data = project->timeline()->data();
    if (item && ref && data.track(*ref) && !data.track(*ref)->locked) {
        const Rational fps = data.fps;
        const int64_t mediaUs
            = item->info.durationUs > 0 ? item->info.durationUs : sampleFrames * 1000000 / rate;
        // The sound reaches the file `latency` after it happened: skip that much at the head
        // so the voice sits where it was spoken against the picture (sub-frame exact).
        const int64_t skipUs = std::clamp<int64_t>(latencyUs, 0, std::max<int64_t>(0, mediaUs - 1));
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = take.mediaId;
        clip.name = QFileInfo(take.path).completeBaseName();
        clip.start = take.startFrame;
        clip.sourceInUs = skipUs;
        clip.sourceDurationUs = item->info.durationUs;
        clip.duration = std::max<int64_t>(1, usToFrames(mediaUs - skipUs, fps));
        TimelineData& edit = project->timeline()->mutableData();
        take.clipId = editops::overwriteClip(edit, *ref, clip);
        take.clipStart = clip.start;
        take.clipDuration = clip.duration;
        take.sourceInUs = skipUs;
    }
    project->endEdit();
    m_lastTake = take;
    if (!take.clipId)
        emit message(
            tr("The take was added to the media pool; its track is gone or locked."), true);
    else
        m_lastMessage.clear();
    emit takeFinished(take);
}

} // namespace kk
