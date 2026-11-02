// SPDX-License-Identifier: GPL-3.0-or-later
// MeterModel — peak hold and fall-back ballistics.
#include "ui/audio/MeterModel.h"

#include "playback/PlaybackEngine.h"
#include "ui/audio/AudioScales.h"

#include <algorithm>

namespace kk {

namespace {
constexpr int kDecayIntervalMs = 33; ///< fall-back animation after playback stopped
constexpr float kSilentDb = -200.0f;
} // namespace

MeterModel::MeterModel(QObject* parent)
    : QObject(parent)
{
    m_clock.start();
    m_decay.setInterval(kDecayIntervalMs);
    connect(&m_decay, &QTimer::timeout, this, &MeterModel::decay);
}

void MeterModel::setEngine(PlaybackEngine* engine)
{
    disconnect(m_connection);
    m_engine = engine;
    if (m_engine) {
        m_connection = connect(m_engine, &PlaybackEngine::metersUpdated, this,
            [this] { setFrame(m_engine->meters()); });
    }
}

void MeterModel::fall(MeterReading& r, qint64 nowMs, double fallDb)
{
    for (int s = 0; s < 2; ++s) {
        r.levelDb[s] = float(std::max(double(kSilentDb), r.levelDb[s] - fallDb));
        if (nowMs - r.holdAtMs[s] > kHoldMs)
            r.holdDb[s] = float(std::max(double(kSilentDb), r.holdDb[s] - fallDb));
    }
}

void MeterModel::feed(MeterReading& r, const ChannelLevel& level, qint64 nowMs, double fallDb)
{
    fall(r, nowMs, fallDb);
    r.mono = level.mono;
    r.gainReductionDb = level.gainReductionDb;
    for (int s = 0; s < 2; ++s) {
        const float db = float(audioscale::toDb(level.peak[level.mono ? 0 : s]));
        r.levelDb[s] = std::max(r.levelDb[s], db);
        if (db >= r.holdDb[s]) {
            r.holdDb[s] = db;
            r.holdAtMs[s] = nowMs;
        }
    }
}

void MeterModel::setFrame(const MeterFrame& frame)
{
    const qint64 now = m_clock.elapsed();
    const double fallDb = kFallDbPerSecond * double(now - m_lastMs) / 1000.0;
    m_lastMs = now;
    m_frame = frame;
    // Channels missing from the frame (deleted, or none while stopped) fall back.
    for (MeterReading& r : m_tracks)
        fall(r, now, fallDb);
    for (MeterReading& r : m_buses)
        fall(r, now, fallDb);
    for (int i = 0; i < frame.levels.tracks.count; ++i) {
        const ChannelLevel& l = frame.levels.tracks.items[size_t(i)];
        feed(m_tracks[l.id], l, now, 0.0);
    }
    for (int i = 0; i < frame.levels.buses.count; ++i) {
        const ChannelLevel& l = frame.levels.buses.items[size_t(i)];
        feed(m_buses[int(l.id)], l, now, 0.0);
    }
    ChannelLevel out;
    out.peak[0] = frame.levels.output[0];
    out.peak[1] = frame.levels.output[1];
    feed(m_output, out, now, fallDb);
    if (!m_decay.isActive())
        m_decay.start();
    emit changed();
}

void MeterModel::decay()
{
    const qint64 now = m_clock.elapsed();
    if (now - m_lastMs < kDecayIntervalMs)
        return; // readings are still coming in
    const double fallDb = kFallDbPerSecond * double(now - m_lastMs) / 1000.0;
    m_lastMs = now;
    bool any = false;
    auto step = [&](MeterReading& r) {
        fall(r, now, fallDb);
        any |= r.levelDb[0] > kSilentDb || r.levelDb[1] > kSilentDb || r.holdDb[0] > kSilentDb
            || r.holdDb[1] > kSilentDb;
    };
    for (MeterReading& r : m_tracks)
        step(r);
    for (MeterReading& r : m_buses)
        step(r);
    step(m_output);
    if (!any)
        m_decay.stop();
    emit changed();
}

void MeterModel::clear()
{
    m_tracks.clear();
    m_buses.clear();
    m_output = MeterReading {};
    m_frame = MeterFrame {};
    m_decay.stop();
    emit changed();
}

} // namespace kk
