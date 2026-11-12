// SPDX-License-Identifier: GPL-3.0-or-later
#include "matting/MatteService.h"

#include "core/Preferences.h"
#include "media/MediaInfo.h"
#include "media/VideoDecoder.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace kk::matting {

namespace {

constexpr int64_t kToleranceMs = 3; ///< a key this close to a stored one is the same frame
constexpr int64_t kStaleMs = 200; ///< a matte this old may stand in for a missing one
constexpr int kLookAhead = 6; ///< frames computed ahead of what the renderer asked for
constexpr size_t kMemoryEntries = 96;
constexpr int64_t kSequenceMs = 120; ///< frames closer than this continue the recurrent state

struct Job
{
    QString path;
    int64_t ms = 0;
    MatteQuality quality = MatteQuality::Preview;
    MatteModel model = MatteModel::Video;
};

struct Range
{
    QString path;
    int64_t fromMs = 0, toMs = 0;
    MatteQuality quality = MatteQuality::Preview;
    MatteModel model = MatteModel::Video;
    int64_t nextMs = 0;
    bool started = false;
};

} // namespace

int64_t matteKeyMs(double sourceSeconds)
{
    return int64_t(std::ceil(sourceSeconds * 1000.0 - 1e-6));
}

struct MatteService::Impl
{
    struct Folder
    {
        QString key;
        std::map<int64_t, MattePtr> mem;
        std::set<int64_t> disk;
        bool diskLoaded = false;
        std::set<int64_t> failed;
    };

    mutable std::mutex mutex;
    std::condition_variable wake; ///< the worker waits here
    std::condition_variable done; ///< waiting renders wait here
    std::unique_ptr<QThread> thread;
    bool stop = false;

    std::deque<Job> urgent;
    std::optional<Job> hint;
    std::deque<Range> ranges;
    int rangeDone = 0, rangeTotal = 0;
    bool working = false;

    using FolderPtr = std::shared_ptr<Folder>; ///< shared: a clear() must not free one in use
    std::map<QString, FolderPtr> folders; ///< folder key -> state
    std::map<QString, QString> folderOfFile; ///< path|quality -> folder key
    std::deque<std::pair<QString, int64_t>> order; ///< memory entries, oldest first

    BackendFactory factory;
    using FrameSourceFn = MatteService::FrameSource;
    FrameSourceFn frameSource;
    QString cacheRoot;
    QString loadError;
    MatteModel loadErrorModel = MatteModel::Video; ///< the model loadError is about
    std::atomic<int> runCount {0}, resetCount {0};
    std::atomic<bool> resetBackend {false}; ///< the worker drops its session before the next job

    // Worker-only state.
    std::unique_ptr<MatteBackend> backend;
    MatteModel backendModel = MatteModel::Video;
    media::VideoDecoder decoder;
    QString decoderPath;
    double decoderFps = 25.0;
    QString lastFolder;
    int64_t lastMs = -1000000;

    MatteCache makeCache() const
    {
        const QString root = cacheRoot.isEmpty()
            ? prefs::effectiveCacheDirectory() + QStringLiteral("/MatteCache")
            : cacheRoot;
        return MatteCache(root);
    }

    FolderPtr folderFor(const QString& path, MatteQuality q, MatteModel model)
    {
        const QString id = path + QLatin1Char('|') + QString::number(int(q)) + QLatin1Char('|')
            + QString::number(int(model));
        auto it = folderOfFile.find(id);
        if (it == folderOfFile.end()) {
            const QFileInfo info(path);
            const QString key = MatteCache::folderKey(path, info.size(),
                info.lastModified().toMSecsSinceEpoch(), modelInfo(model).id, int(q));
            it = folderOfFile.emplace(id, key).first;
        }
        FolderPtr& slot = folders[it->second];
        if (!slot) {
            slot = std::make_shared<Folder>();
            slot->key = it->second;
        }
        return slot;
    }

    // Finds the stored matte nearest to ms within the tolerance (memory, then disk).
    MattePtr find(Folder& f, int64_t ms, int64_t tolerance, std::unique_lock<std::mutex>& lock)
    {
        auto it = f.mem.lower_bound(ms - tolerance);
        MattePtr best;
        int64_t bestDist = tolerance + 1;
        for (; it != f.mem.end() && it->first <= ms + tolerance; ++it) {
            const int64_t d = std::llabs(it->first - ms);
            if (d < bestDist) {
                bestDist = d;
                best = it->second;
            }
        }
        if (best)
            return best;
        if (tolerance > kToleranceMs)
            return {};
        const MatteCache cache = makeCache();
        if (!f.diskLoaded) {
            lock.unlock();
            const QList<int64_t> times = cache.times(f.key);
            lock.lock();
            for (int64_t t : times)
                f.disk.insert(t);
            f.diskLoaded = true;
        }
        auto dit = f.disk.lower_bound(ms - tolerance);
        if (dit == f.disk.end() || *dit > ms + tolerance)
            return {};
        const int64_t at = *dit;
        const QString key = f.key;
        lock.unlock();
        const QImage image = cache.load(key, at);
        lock.lock();
        if (image.isNull()) {
            f.disk.erase(at);
            return {};
        }
        auto ptr = std::make_shared<const QImage>(image);
        remember(f, at, ptr);
        return ptr;
    }

    void remember(Folder& f, int64_t ms, const MattePtr& ptr)
    {
        f.mem[ms] = ptr;
        order.emplace_back(f.key, ms);
        while (order.size() > kMemoryEntries) {
            const auto [key, t] = order.front();
            order.pop_front();
            auto it = folders.find(key);
            if (it != folders.end())
                it->second->mem.erase(t);
        }
    }

    void push(std::deque<Job>& q, const Job& j)
    {
        q.erase(std::remove_if(q.begin(), q.end(),
                    [&](const Job& o) {
                        return o.path == j.path && o.ms == j.ms && o.quality == j.quality
                            && o.model == j.model;
                    }),
            q.end());
        q.push_back(j);
        while (q.size() > 24)
            q.pop_front();
    }

    void ensureThread()
    {
        if (thread)
            return;
        thread.reset(QThread::create([this] { loop(); }));
        thread->setObjectName(QStringLiteral("MatteWorker"));
        thread->start(QThread::LowestPriority);
    }

    // ---- worker ----

    bool isCached(const Job& j, std::unique_lock<std::mutex>& lock)
    {
        const FolderPtr keep = folderFor(j.path, j.quality, j.model);
        Folder& f = *keep;
        return find(f, j.ms, kToleranceMs, lock) != nullptr || f.failed.count(j.ms);
    }

    void loop()
    {
#ifdef __linux__
        // Qt's thread priority does nothing for normal Linux threads: lower this thread's nice
        // value itself (the model's own worker threads inherit it), so playback and the UI win
        // whenever the processor is busy.
        setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 15);
#endif
        std::unique_lock<std::mutex> lock(mutex);
        while (!stop) {
            std::optional<Job> job;
            if (!urgent.empty()) {
                job = urgent.back();
                urgent.pop_back();
            } else if (hint) {
                job = nextAhead(lock);
                if (!job)
                    hint.reset();
            }
            if (!job && !ranges.empty())
                job = nextRangeJob(lock);
            if (!job) {
                // The lock was released while looking ahead (disk lookups): a render may have
                // queued a job meanwhile and its wake-up went unheard.
                if (!urgent.empty())
                    continue;
                working = false;
                done.notify_all();
                wake.wait(lock);
                continue;
            }
            working = true;
            if (isCached(*job, lock)) {
                continue;
            }
            lock.unlock();
            compute(*job, lock);
            if (!lock.owns_lock())
                lock.lock();
            done.notify_all();
        }
        working = false;
        done.notify_all();
    }

    std::optional<Job> nextAhead(std::unique_lock<std::mutex>& lock)
    {
        const Job base = *hint;
        if (isStillImagePath(base.path))
            return std::nullopt; // one picture: nothing follows it
        const int64_t step
            = std::max<int64_t>(1, int64_t(std::llround(1000.0 / decoderFpsFor(base.path))));
        for (int i = 1; i <= kLookAhead; ++i) {
            Job j = base;
            j.ms = base.ms + i * step;
            if (!isCached(j, lock))
                return j;
        }
        return std::nullopt;
    }

    std::optional<Job> nextRangeJob(std::unique_lock<std::mutex>& lock)
    {
        while (!ranges.empty()) {
            Range& r = ranges.front();
            if (!r.started) {
                r.nextMs = r.fromMs;
                r.started = true;
            }
            if (r.nextMs > r.toMs) {
                ranges.pop_front();
                continue;
            }
            const int64_t step
                = std::max<int64_t>(1, int64_t(std::llround(1000.0 / decoderFpsFor(r.path))));
            Job j {r.path, r.nextMs, r.quality, r.model};
            r.nextMs += step;
            if (isCached(j, lock)) {
                ++rangeDone;
                continue;
            }
            rangeAccounting = true;
            return j;
        }
        return std::nullopt;
    }
    bool rangeAccounting = false;

    // Frame rate of a file, probed without the lock held for long (cheap after the first time).
    double decoderFpsFor(const QString& path)
    {
        auto it = fpsOf.find(path);
        if (it != fpsOf.end())
            return it->second;
        return 25.0;
    }
    std::map<QString, double> fpsOf;

    QImage defaultFrame(const QString& path, int64_t ms, int edge)
    {
        if (path != decoderPath) {
            decoder.close();
            decoderPath.clear();
            QString error;
            if (!decoder.open(path, &error))
                return {};
            decoderPath = path;
            const auto fr = decoder.info().frameRate;
            decoderFps = fr.isValid() && fr.toDouble() > 1.0 ? fr.toDouble() : 25.0;
            std::lock_guard<std::mutex> guard(mutex);
            fpsOf[path] = decoderFps;
        }
        return decoder.frameAt(ms * 1000, QSize(edge, edge));
    }

    void compute(const Job& job, std::unique_lock<std::mutex>& lock)
    {
        const int edge = inputEdge(job.quality, job.model);
        bool ok = false;
        MattePtr result;
        QString folderKey;
        BackendFactory makeBackend;
        FrameSourceFn source;
        {
            std::lock_guard<std::mutex> guard(mutex);
            makeBackend = factory;
            source = frameSource;
        }
        if (resetBackend.exchange(false)) {
            backend.reset();
            lastFolder.clear();
        }
        QImage pic = source ? source(job.path, job.ms, edge) : defaultFrame(job.path, job.ms, edge);
        if (!pic.isNull()) {
            // Sizes the model takes: multiples of 4, long side at most `edge`.
            QSize size = pic.size();
            if (std::max(size.width(), size.height()) > edge)
                size.scale(edge, edge, Qt::KeepAspectRatio);
            size = QSize(
                std::max(4, (size.width() + 2) / 4 * 4), std::max(4, (size.height() + 2) / 4 * 4));
            if (size != pic.size())
                pic = pic.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            pic = pic.convertToFormat(QImage::Format_RGB888);

            if (backend && backendModel != job.model) {
                backend.reset(); // the other model: load its session
                lastFolder.clear();
            }
            if (!backend) {
                QString why;
                const QString path = modelPath(modelInfo(job.model));
                backend
                    = makeBackend ? makeBackend(path, &why) : createBackend(job.model, path, &why);
                backendModel = job.model;
                std::lock_guard<std::mutex> guard(mutex);
                loadError = backend ? QString() : why;
                loadErrorModel = job.model;
            }
            if (backend) {
                const QString id = job.path + QLatin1Char('|') + QString::number(int(job.quality))
                    + QLatin1Char('|') + QString::number(int(job.model));
                const bool sequence
                    = lastFolder == id && job.ms > lastMs && job.ms - lastMs <= kSequenceMs;
                if (!sequence)
                    ++resetCount;
                ++runCount;
                QImage matte = backend->process(pic, job.quality, !sequence);
                if (!matte.isNull()) {
                    lastFolder = id;
                    lastMs = job.ms;
                    result = std::make_shared<const QImage>(matte);
                    ok = true;
                } else {
                    backend.reset(); // start the next frame from a clean session
                    lastFolder.clear();
                }
            }
        }
        lock.lock();
        const MatteCache cache = makeCache();
        const FolderPtr keep = folderFor(job.path, job.quality, job.model);
        Folder& f = *keep;
        folderKey = f.key;
        if (ok) {
            remember(f, job.ms, result);
            f.disk.insert(job.ms);
        } else {
            f.failed.insert(job.ms);
        }
        if (rangeAccounting) {
            ++rangeDone;
            rangeAccounting = false;
        }
        const int total = rangeTotal, doneCount = rangeDone;
        lock.unlock();
        if (ok)
            cache.store(folderKey, job.ms, *result);
        emit owner->matteComputed();
        if (total > 0)
            emit owner->backgroundProgressChanged(doneCount, total);
    }

    MatteService* owner = nullptr;
};

MatteService& MatteService::instance()
{
    static MatteService* service = [] {
        auto* s = new MatteService;
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, s, [s] {
            std::unique_lock<std::mutex> lock(s->d->mutex);
            s->d->stop = true;
            s->d->wake.notify_all();
            lock.unlock();
            if (s->d->thread)
                s->d->thread->wait();
        });
        return s;
    }();
    return *service;
}

MatteService::MatteService()
    : d(std::make_unique<Impl>())
{
    d->owner = this;
}

MatteService::~MatteService()
{
    {
        std::lock_guard<std::mutex> guard(d->mutex);
        d->stop = true;
        d->wake.notify_all();
        d->done.notify_all();
    }
    if (d->thread)
        d->thread->wait();
}

MatteService::Status MatteService::status(MatteModel model) const
{
    Status s;
    s.runtimeCompiled = runtimeCompiledIn();
    s.runtime = runtimeVersion();
    s.modelInstalled = isInstalled(modelInfo(model));
    std::lock_guard<std::mutex> guard(d->mutex);
    if (d->factory) {
        s.ready = true;
        return s;
    }
    if (!s.runtimeCompiled)
        s.reason = tr("This build of Kinoklip has no AI runtime.");
    else if (!s.modelInstalled)
        s.reason
            = tr("The AI model is not installed. Download it from the Inspector or Preferences.");
    else if (!d->loadError.isEmpty() && d->loadErrorModel == model)
        s.reason = d->loadError;
    else
        s.ready = true;
    return s;
}

MattePtr MatteService::matte(const QString& mediaPath, double sourceSeconds, MatteQuality quality,
    bool wait, MatteModel model)
{
    if (mediaPath.isEmpty() || !status(model).ready)
        return {};
    // A still picture has one matte, whatever source time the clip asks for.
    const int64_t ms = isStillImagePath(mediaPath) ? 0 : matteKeyMs(sourceSeconds);
    std::unique_lock<std::mutex> lock(d->mutex);
    const Impl::FolderPtr keep = d->folderFor(mediaPath, quality, model);
    Impl::Folder& f = *keep;
    if (MattePtr hit = d->find(f, ms, kToleranceMs, lock)) {
        d->hint = Job {mediaPath, ms, quality, model};
        d->ensureThread();
        d->wake.notify_all();
        return hit;
    }
    if (f.failed.count(ms) && !wait)
        return {};
    f.failed.erase(ms); // a retry: the file may be readable now
    d->push(d->urgent, Job {mediaPath, ms, quality, model});
    d->hint = Job {mediaPath, ms, quality, model};
    d->ensureThread();
    d->wake.notify_all();
    if (!wait)
        return d->find(f, ms, kStaleMs, lock);
    while (!d->stop) {
        const Impl::FolderPtr again = d->folderFor(mediaPath, quality, model);
        Impl::Folder& g = *again;
        if (MattePtr hit = d->find(g, ms, kToleranceMs, lock))
            return hit;
        if (g.failed.count(ms))
            return {};
        if (!d->working && d->urgent.empty()) {
            // The queue was emptied under us (unload()): ask again.
            d->push(d->urgent, Job {mediaPath, ms, quality, model});
            d->wake.notify_all();
        }
        d->done.wait_for(lock, std::chrono::milliseconds(100));
    }
    return {};
}

void MatteService::queueRange(const QString& mediaPath, double fromSeconds, double toSeconds,
    MatteQuality quality, MatteModel model)
{
    if (mediaPath.isEmpty() || toSeconds < fromSeconds || !status(model).ready)
        return;
    if (isStillImagePath(mediaPath))
        fromSeconds = toSeconds = 0.0;
    std::lock_guard<std::mutex> guard(d->mutex);
    if (d->ranges.empty() && !d->working)
        d->rangeDone = d->rangeTotal = 0;
    Range r;
    r.path = mediaPath;
    r.fromMs = matteKeyMs(fromSeconds);
    r.toMs = matteKeyMs(toSeconds);
    r.quality = quality;
    r.model = model;
    const double fps = d->decoderFpsFor(mediaPath);
    d->rangeTotal += int((r.toMs - r.fromMs) * fps / 1000.0) + 1;
    d->ranges.push_back(r);
    d->ensureThread();
    d->wake.notify_all();
}

void MatteService::cancelBackground()
{
    std::lock_guard<std::mutex> guard(d->mutex);
    d->ranges.clear();
    d->rangeDone = d->rangeTotal = 0;
}

MatteService::Progress MatteService::backgroundProgress() const
{
    std::lock_guard<std::mutex> guard(d->mutex);
    Progress p;
    p.done = d->rangeDone;
    p.total = d->rangeTotal;
    if (d->ranges.empty() && !d->working) {
        p = Progress {};
    }
    return p;
}

bool MatteService::busy() const
{
    std::lock_guard<std::mutex> guard(d->mutex);
    return d->working || !d->urgent.empty() || !d->ranges.empty();
}

int MatteService::cachedFrames(
    const QString& mediaPath, MatteQuality quality, MatteModel model) const
{
    std::lock_guard<std::mutex> guard(d->mutex);
    const Impl::FolderPtr f = d->folderFor(mediaPath, quality, model);
    return int(d->makeCache().times(f->key).size());
}

MatteCache MatteService::cache() const
{
    std::lock_guard<std::mutex> guard(d->mutex);
    return d->makeCache();
}

void MatteService::setCacheRoot(const QString& root)
{
    std::lock_guard<std::mutex> guard(d->mutex);
    d->cacheRoot = root;
    d->folders.clear();
    d->folderOfFile.clear();
    d->order.clear();
}

void MatteService::unload()
{
    std::unique_lock<std::mutex> lock(d->mutex);
    d->ranges.clear();
    d->urgent.clear();
    d->hint.reset();
    d->rangeDone = d->rangeTotal = 0;
    d->folders.clear();
    d->folderOfFile.clear();
    d->order.clear();
    d->loadError.clear();
    d->resetBackend = true; // the worker drops its session before its next job
}

void MatteService::setBackendFactory(BackendFactory factory)
{
    std::lock_guard<std::mutex> guard(d->mutex);
    d->factory = std::move(factory);
    d->resetBackend = true;
}

void MatteService::setFrameSource(FrameSource source)
{
    std::lock_guard<std::mutex> guard(d->mutex);
    d->frameSource = std::move(source);
}

int MatteService::runs() const
{
    return d->runCount;
}

int MatteService::resets() const
{
    return d->resetCount;
}

} // namespace kk::matting
