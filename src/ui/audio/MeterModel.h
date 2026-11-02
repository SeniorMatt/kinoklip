// SPDX-License-Identifier: GPL-3.0-or-later
// MeterModel — meter ballistics for the Audio page: turns the playback engine's MeterFrame
// readings into displayed levels with peak hold and fall-back, shared by every meter widget.
#pragma once

#include "render/MixLevels.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>

namespace kk {

class PlaybackEngine;

/// What one meter shows (dBFS per side; mono channels repeat side 0).
struct MeterReading
{
    float levelDb[2] = {-200.0f, -200.0f}; ///< falling bar
    float holdDb[2] = {-200.0f, -200.0f}; ///< peak hold mark
    float gainReductionDb = 0.0f;
    bool mono = false;
    qint64 holdAtMs[2] = {0, 0}; ///< when the hold mark was last raised (model clock)
};

/// Keeps the displayed state of every track, bus and output meter.
///
/// Ballistics (applied on the GUI thread at the engine's ~30 Hz meter rate, and by an own
/// timer while levels are falling after playback stops): instant rise, the bar falls back at
/// kFallDbPerSecond; the peak hold mark stays kHoldMs, then falls at the same rate.
/// Readings are keyed by MixTarget-like keys: track ids, bus ids, and the output.
///
/// Thread-safety: GUI thread only.
class MeterModel : public QObject
{
    Q_OBJECT
public:
    static constexpr double kFallDbPerSecond = 24.0;
    static constexpr int kHoldMs = 1500;

    explicit MeterModel(QObject* parent = nullptr);

    /// Follows @p engine's metersUpdated() (nullptr = none).
    void setEngine(PlaybackEngine* engine);
    /// Takes a reading (from the engine, or computed offline for previews and tests).
    void setFrame(const MeterFrame& frame);
    /// Clears every meter and the loudness readings.
    void clear();

    [[nodiscard]] MeterReading track(quint64 trackId) const { return m_tracks.value(trackId); }
    [[nodiscard]] MeterReading bus(int busId) const { return m_buses.value(busId); }
    [[nodiscard]] MeterReading output() const { return m_output; }
    /// The last frame's loudness values (LUFS / dBTP; -200 = none).
    [[nodiscard]] const MeterFrame& lastFrame() const { return m_frame; }

signals:
    /// Displayed values changed (repaint the meters).
    void changed();

private:
    void decay();
    void feed(MeterReading& r, const ChannelLevel& level, qint64 nowMs, double fallDb);
    void fall(MeterReading& r, qint64 nowMs, double fallDb);

    PlaybackEngine* m_engine = nullptr;
    QMetaObject::Connection m_connection;
    QHash<quint64, MeterReading> m_tracks;
    QHash<int, MeterReading> m_buses;
    MeterReading m_output;
    MeterFrame m_frame;
    QElapsedTimer m_clock;
    qint64 m_lastMs = 0;
    QTimer m_decay;
};

} // namespace kk
