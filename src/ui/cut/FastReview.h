// SPDX-License-Identifier: GPL-3.0-or-later
// FastReview — the Cut page's Fast Review: plays the timeline faster, each shot at a speed that
// grows with its length (cutedits::fastReviewRate()).
#pragma once

#include <QObject>
#include <QPointer>

#include <cstdint>

namespace kk {

struct AppContext;
class PlaybackEngine;

/// Fast Review playback on the program engine (AppContext::playback).
///
/// start() plays from the playhead; whenever the playhead enters another shot (the range
/// between two edit points on any track) the rate becomes fastReviewRate(shot length), so a
/// long shot is skimmed in about as long as a short one. Any other transport command (a rate
/// the review did not set, a stop) ends the review; so does the timeline end. Audio follows
/// the engine's behaviour at non-1x rates (simplified: no special review sound).
///
/// Thread-safety: GUI thread only.
class FastReview : public QObject
{
    Q_OBJECT
public:
    /// @p ctx must outlive this object; without a program engine start() does nothing.
    explicit FastReview(AppContext& ctx, QObject* parent = nullptr);

    [[nodiscard]] bool isActive() const { return m_active; }
    /// Speed for the shot under timeline frame @p frame (exposed for tests).
    [[nodiscard]] double rateAt(int64_t frame) const;

public slots:
    /// Starts reviewing from the playhead.
    void start();
    /// Stops reviewing (and playback).
    void stop();
    /// start() or stop().
    void setActive(bool active);

signals:
    /// The review started or ended (the page keeps its button checked state in step).
    void activeChanged(bool active);

private:
    void onPosition(int64_t frame);
    void onRate(double rate);
    void finish();

    AppContext& m_ctx;
    QPointer<PlaybackEngine> m_engine;
    bool m_active = false;
    double m_rate = 0.0; ///< the rate the review set last
    int64_t m_shotStart = -1; ///< current shot [start, end)
    int64_t m_shotEnd = -1;
};

} // namespace kk
