// SPDX-License-Identifier: GPL-3.0-or-later
// MixerController — turns mixer widget gestures into Project edits on the active timeline:
// one undo step per gesture, fader / pan automation recording (touch / write) while playing.
#pragma once

#include "core/MixerOps.h"

#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace kk {

struct AppContext;
class PlaybackEngine;

/// Editing front end of the Audio page's mixer widgets (and of the Edit page's Mixer panel).
///
/// * edit(): one change = one undo step (buttons, menus, spin boxes);
/// * beginGesture() / updateGesture() / endGesture(): a drag (fader, pan knob, EQ handle,
///   dynamics slider) is one undo step: the edit opens on press, every move mutates the
///   timeline live (the mixer hears it at once) and the release closes it;
/// * pressControl() / moveControl() / releaseControl() for the fader and pan of tracks
///   implement the automation modes: while the transport plays, a track in Touch mode records
///   the control's value at the playhead from press to release (keys passed over are
///   replaced; holding the control still keeps writing its value as the playhead moves); in
///   Write mode recording runs from play to stop whether the control is touched or not (one
///   undo step per pass). Stopped, or in Off / Read mode, the control edits the static value
///   - except in Read / Touch / Write with an existing curve, where it sets the curve's key
///   at the playhead (so what plays is what the control shows).
///
/// Thread-safety: GUI thread only.
class MixerController : public QObject
{
    Q_OBJECT
public:
    /// Which automatable control of a strip.
    enum class Control {
        Fader,
        Pan
    };

    explicit MixerController(AppContext& ctx, QObject* parent = nullptr);
    ~MixerController() override;

    /// The active timeline's data.
    [[nodiscard]] const TimelineData& data() const;
    /// Applies @p change as one undo step named @p label; nothing is pushed if it returns
    /// false or changes nothing.
    void edit(const QString& label, const std::function<bool(TimelineData&)>& change);

    /// Starts a drag gesture named @p label (one undo step until endGesture()).
    void beginGesture(const QString& label);
    /// Applies @p change live inside the open gesture (no-op without one).
    void updateGesture(const std::function<bool(TimelineData&)>& change);
    /// Closes the gesture (pushes the undo step if anything changed).
    void endGesture();
    /// True while a gesture is open.
    [[nodiscard]] bool inGesture() const { return m_gesture; }

    /// Value of @p control of @p target that the strip shows at timeline frame @p frame
    /// (automation curve, else the static value; buses: the static fader, pan 0).
    [[nodiscard]] double controlValue(const MixTarget& target, Control control, double frame) const;
    /// Press / move / release of the fader or pan control of @p target (see the class doc).
    void pressControl(const MixTarget& target, Control control);
    void moveControl(const MixTarget& target, Control control, double value);
    void releaseControl(const MixTarget& target, Control control);
    /// One-shot value change (double-click reset, typed value): one undo step.
    void setControl(const MixTarget& target, Control control, double value);

    /// Track header state shared with the timeline headers (one undo step each).
    void setTrackMuted(quint64 trackId, bool muted);
    void setTrackSolo(quint64 trackId, bool solo);

    /// Replaces the transport probe (default: AppContext::playback->isPlaying()); tests and
    /// other transports use it, and call transportStateChanged() when it changes.
    void setPlayingProbe(std::function<bool()> probe) { m_playingProbe = std::move(probe); }
    /// The transport started or stopped: Write passes start, recordings end.
    void transportStateChanged() { onPlaybackState(); }

    /// Write mode passes (recording from play to stop) start only while enabled: one
    /// controller per application should run them (the Audio page's, while it is shown).
    void setWritePassesEnabled(bool enabled) { m_writePasses = enabled; }

    /// Automation key of @p control ("volumeDb" / "pan").
    [[nodiscard]] static QString automationKey(Control control);

signals:
    /// The active timeline changed (any edit, undo, tab switch): widgets re-read the mix.
    void mixChanged();

private:
    struct Recording
    {
        quint64 trackId = 0;
        Control control = Control::Fader;
        double value = 0.0;
        int64_t lastFrame = 0;
        bool touched = false; ///< Write mode: the user holds the control
    };
    void onPlayheadChanged(int64_t frame);
    void onPlaybackState();
    void startWritePasses();
    void stopRecordings();
    void writeRecording(Recording& r, int64_t frame);
    [[nodiscard]] bool playing() const;
    [[nodiscard]] Recording* recording(quint64 trackId, Control control);
    /// Applies a stopped / static control change to @p d (no edit handling).
    bool applyStatic(TimelineData& d, const MixTarget& target, Control control, double value);

    AppContext& m_ctx;
    bool m_gesture = false;
    QVector<Recording> m_recordings; ///< active automation recordings
    bool m_recordEditOpen = false; ///< a Project edit is open for the recordings
    bool m_writePasses = true;
    std::function<bool()> m_playingProbe;
};

} // namespace kk
