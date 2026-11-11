// SPDX-License-Identifier: GPL-3.0-or-later
// VoiceOverController — record from the microphone onto an audio track while the timeline plays
// (spec 75): arm a track, count in, record, write the take as a WAV file, import it and place it.
#pragma once

#include "core/TimelineData.h"
#include "playback/AudioInput.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace kk {

namespace media {
class WavWriter;
}

struct AppContext;

/// The voice-over tool.
///
/// * **Arm**: one audio track at a time (arming another moves the arm). An armed track opens
///   its input device and meters it (peak, with a clip indicator), nothing is recorded and
///   nothing is played back from the microphone (no monitoring: no feedback).
/// * **Record**: startRecording() counts in (Preferences, 0 = none), then plays the timeline from
///   the playhead (when the application has a playback engine) and writes the input to a WAV file.
///   stopRecording() (also: playback stopping, Space, Stop, the end of the timeline) ends it.
/// * **Take**: the file "<track> take N.wav" is imported into the media pool (bin "Voice Over")
///   and laid over the armed track at the frame recording started at, overwriting what was
///   there, shifted by the input latency (the file keeps the whole recording; the clip skips
///   the latency at its head). Importing and placing are ONE undo step.
///
/// The input is behind AudioInput, created by an AudioInputFactory; tests give it a generated
/// signal. The state is per session (nothing is saved in the project).
///
/// Thread-safety: GUI thread only.
class VoiceOverController : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,
        CountIn,
        Recording
    };
    Q_ENUM(State)

    /// What a finished take made.
    struct Take
    {
        QString path; ///< the WAV file (inside the project file for single-file projects)
        QString mediaId; ///< its media pool item ("" if it was not imported)
        quint64 clipId = 0; ///< the clip placed on the track (0 = none)
        QString trackName;
        int number = 0; ///< the N of "<track> take N"
        int64_t startFrame = 0; ///< playhead frame recording started at
        int64_t clipStart = 0; ///< frame the clip starts at (= startFrame)
        int64_t clipDuration = 0; ///< frames
        int64_t sourceInUs = 0; ///< latency skipped at the head of the file
        int64_t latencyUs = 0; ///< total latency compensated
        int64_t sampleFrames = 0; ///< samples per channel in the file
        int sampleRate = 0;
        int channels = 0;
    };

    /// Registers itself as AppContext::voiceOver. @p factory creates the input (tests pass
    /// their own).
    explicit VoiceOverController(AppContext& ctx, AudioInputFactory factory = qtAudioInputFactory(),
        QObject* parent = nullptr);
    ~VoiceOverController() override;

    /// Replaces the device list (default: systemAudioInputs()) and the "why no recording is
    /// possible" check (default: systemAudioInputProblem()); for tests.
    void setDeviceSource(
        std::function<QList<AudioInputDevice>()> lister, std::function<QString()> problemCheck);

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] bool isRecording() const { return m_state == State::Recording; }

    // ---- Arming ----

    /// Arms audio track @p trackId of the active timeline. False (and lastMessage()) when no
    /// microphone is available, the track is locked or unknown, or recording is under way.
    bool arm(quint64 trackId);
    void disarm();
    [[nodiscard]] quint64 armedTrackId() const { return m_armedTrack; }
    [[nodiscard]] bool isArmed(quint64 trackId) const { return trackId && trackId == m_armedTrack; }
    /// Arms @p trackId, or disarms it when it is the armed track (the button's behaviour).
    bool toggleArm(quint64 trackId);

    /// Why a track cannot be armed now (no microphone, permission denied, a failure of the
    /// device); empty when arming works. The text of the arm button's tooltip.
    [[nodiscard]] QString problem() const;

    // ---- Input ----

    /// The inputs offered in the menu, the system default first.
    [[nodiscard]] QList<AudioInputDevice> inputs() const;
    /// The input id used for @p trackId: its own choice, else Preferences' default input, else
    /// "" (system default).
    [[nodiscard]] QByteArray inputFor(quint64 trackId) const;
    /// True if @p trackId has its own input choice (not the default).
    [[nodiscard]] bool hasOwnInput(quint64 trackId) const { return m_inputs.contains(trackId); }
    /// Chooses the input of @p trackId (empty = the default again); reopens it when armed.
    void setInputFor(quint64 trackId, const QByteArray& deviceId);
    /// Name of the input @p trackId records from ("Default (Name)" style text for the menu).
    [[nodiscard]] QString inputName(quint64 trackId) const;

    // ---- Level ----

    /// Peak of the armed input, 0..1 linear, falling back after the sound stops.
    [[nodiscard]] float peak() const { return m_peak; }
    /// True for 2 s after a sample reached full scale.
    [[nodiscard]] bool clipped() const;
    void clearClip() { m_clipTimer.invalidate(); }

    // ---- Recording ----

    /// Starts the count-in (or the recording at once when it is 0). False with lastMessage()
    /// when nothing is armed, the playhead is at the end, the track is locked, there is no input...
    bool startRecording();
    /// Ends the count-in (nothing is made) or the recording (the take is written, imported and
    /// placed). No-op when idle.
    void stopRecording();
    /// Cancels a count-in under way (nothing is made). True if there was one.
    bool cancelCountIn();
    /// startRecording() when idle, stopRecording() otherwise (the Record action).
    void toggleRecording();
    /// Seconds left in the count-in (0 outside it).
    [[nodiscard]] int countInRemaining() const { return m_countRemaining; }

    [[nodiscard]] const Take& lastTake() const { return m_lastTake; }
    /// The last explanation shown to the user (a refusal or a failure); empty after a success.
    [[nodiscard]] QString lastMessage() const { return m_lastMessage; }

    /// Folder the next take is written to (before it is stored in a single-file project).
    [[nodiscard]] QString takeFolder() const;
    /// File name of the next take of @p trackId ("A1 take 3.wav").
    [[nodiscard]] QString nextTakeFileName(quint64 trackId) const;

Q_SIGNALS:
    void armedChanged();
    void inputChanged();
    void stateChanged(VoiceOverController::State state);
    /// The count-in shows @p remaining seconds (3, 2, 1; 0 when recording starts).
    void countInTick(int remaining);
    /// The level of the armed input changed (about 30 times a second while sound arrives).
    void levelChanged();
    void takeFinished(const VoiceOverController::Take& take);
    /// Something the user must be told; @p error: it went wrong (a refusal or failure).
    void message(const QString& text, bool error);

private:
    void openInput();
    void closeInput();
    void onSamples(const std::vector<float>& block);
    void onInputFailed(const QString& text);
    void tickCountIn();
    void beginTake();
    void finishTake();
    void abortTake(const QString& text);
    void setState(State state);
    void refuse(const QString& text);
    void decay();
    [[nodiscard]] std::optional<TrackRef> armedRef() const;
    [[nodiscard]] int nextTakeNumber(const QString& base) const;
    [[nodiscard]] QString takeBaseName(quint64 trackId) const;
    void checkArmedTrack();

    AppContext& m_ctx;
    AudioInputFactory m_factory;
    std::function<QList<AudioInputDevice>()> m_lister;
    std::function<QString()> m_problemCheck;

    State m_state = State::Idle;
    quint64 m_armedTrack = 0;
    quint64 m_armedTimeline = 0;
    QHash<quint64, QByteArray> m_inputs;
    std::unique_ptr<AudioInput> m_input;
    QString m_deviceProblem; ///< the input could not be opened / broke

    float m_peak = 0.0f;
    float m_blockPeak = 0.0f;
    QElapsedTimer m_clipTimer;
    QTimer m_decayTimer;
    QElapsedTimer m_levelEmit;

    QTimer m_countTimer;
    int m_countRemaining = 0;

    std::unique_ptr<media::WavWriter> m_writer;
    QString m_takePath;
    int m_takeNumber = 0;
    int64_t m_startFrame = 0;
    bool m_finishing = false;
    QMetaObject::Connection m_rateConnection;

    Take m_lastTake;
    QString m_lastMessage;
};

} // namespace kk
