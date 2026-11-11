// SPDX-License-Identifier: GPL-3.0-or-later
// MatteService — computes and serves the mattes of Background Removal (spec 78). The renderer
// asks for the matte of a clip's source frame: a finished one is returned at once (memory, then
// the disk cache); an unfinished one is queued for the worker thread (lowest priority, so playback
// never waits) and the renderer draws the unprocessed picture (or a matte of a frame within 0.2 s)
// until it is ready. Renders for export wait. The worker decodes the source frame itself, runs the
// model and keeps the recurrent state only while frames follow each other: a seek or a cut resets
// it. Everything here is thread-safe.
#pragma once

#include "matting/MatteBackend.h"
#include "matting/MatteCache.h"
#include "matting/ModelCatalog.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace kk::matting {

using MattePtr = std::shared_ptr<const QImage>;

class MatteService : public QObject
{
    Q_OBJECT
public:
    static MatteService& instance();
    ~MatteService() override;

    /// Whether mattes can be made now and, if not, why not (shown in the Inspector).
    struct Status
    {
        bool ready = false;
        bool runtimeCompiled = false;
        bool modelInstalled = false;
        QString reason; ///< empty when ready
        QString runtime; ///< "ONNX Runtime 1.29.0"
    };
    [[nodiscard]] Status status() const;

    /// The matte of the source frame at @p sourceSeconds of @p mediaPath, or null. Not ready:
    /// it is queued, and a matte of a frame within 0.2 s is returned if there is one. With
    /// @p wait the call blocks until the matte exists (null only if the service is unavailable
    /// or the frame cannot be decoded).
    [[nodiscard]] MattePtr matte(
        const QString& mediaPath, double sourceSeconds, MatteQuality quality, bool wait);

    /// Queues the whole source range [@p fromSeconds, @p toSeconds] of a file (lowest priority
    /// after what playback asks for). Progress: backgroundProgress().
    void queueRange(
        const QString& mediaPath, double fromSeconds, double toSeconds, MatteQuality quality);
    void cancelBackground();
    struct Progress
    {
        int done = 0;
        int total = 0;
    };
    [[nodiscard]] Progress backgroundProgress() const;
    /// True while any job is queued or running.
    [[nodiscard]] bool busy() const;

    /// Frames of the range (sourceSeconds step @p fps) already in the disk cache.
    [[nodiscard]] int cachedFrames(const QString& mediaPath, MatteQuality quality) const;

    [[nodiscard]] MatteCache cache() const;
    void setCacheRoot(const QString& root);
    /// Drops the loaded model and every in-memory matte (after a model was removed, a cache
    /// cleared).
    void unload();

    /// Test hooks: a stand-in model, and a stand-in for the video decoder (returns the picture
    /// for a file at a time in ms, about @p edge pixels on the long side).
    using FrameSource = std::function<QImage(const QString& path, int64_t ms, int edge)>;
    void setBackendFactory(BackendFactory factory);
    void setFrameSource(FrameSource source);
    /// Counts of the work done (tests): model runs, and runs that started from a reset state.
    [[nodiscard]] int runs() const;
    [[nodiscard]] int resets() const;

signals:
    /// A matte was finished (queued signal: the viewer may draw the frame again).
    void matteComputed();
    void backgroundProgressChanged(int done, int total);

private:
    MatteService();
    struct Impl;
    std::unique_ptr<Impl> d;
};

/// Source time to the key used by the cache: milliseconds, rounded up so that the key lies at
/// or after the start of the frame.
[[nodiscard]] int64_t matteKeyMs(double sourceSeconds);

} // namespace kk::matting
