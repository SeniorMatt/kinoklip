// SPDX-License-Identifier: GPL-3.0-or-later
// AudioPluginRegistry — see AudioPluginRegistry.h.
#include "plugins/AudioPluginRegistry.h"
#include "core/HelperProcess.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QProcess>
#include <QRunnable>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcAudioPlugins, "kk.audioplug.registry")

namespace kk::plugins {

namespace {

constexpr int kCacheVersion = 1;

bool isLv2(const QString& format)
{
    return format == QLatin1String(audioplug::kFormatLv2);
}

QString tagOf(const QString& format)
{
    return QString::fromLatin1(isLv2(format) ? kLv2PluginTag : kClapPluginTag);
}

/// Size and newest date of the files of a bundle: a changed plugin has another stamp.
void stampOf(const QString& path, qint64* size, qint64* mtime)
{
    *size = 0;
    *mtime = 0;
    const QFileInfo fi(path);
    if (fi.isFile()) {
        *size = fi.size();
        *mtime = fi.lastModified().toSecsSinceEpoch();
        return;
    }
    int seen = 0;
    const std::function<void(const QDir&, int)> walk = [&](const QDir& d, int depth) {
        for (const QFileInfo& e : d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (++seen > 5000)
                return;
            if (e.isDir()) {
                if (depth < 2)
                    walk(QDir(e.absoluteFilePath()), depth + 1);
            } else {
                *size += e.size();
                *mtime = std::max<qint64>(*mtime, e.lastModified().toSecsSinceEpoch());
            }
        }
    };
    walk(QDir(path), 0);
}

bool sameDescriptor(const EffectDescriptor& a, const EffectDescriptor& b)
{
    if (a.id != b.id || a.name != b.name || a.description != b.description || a.group != b.group
        || a.params.size() != b.params.size())
        return false;
    for (qsizetype i = 0; i < a.params.size(); ++i) {
        const ParamDescriptor& p = a.params[i];
        const ParamDescriptor& q = b.params[i];
        if (p.key != q.key || p.label != q.label || p.type != q.type || p.min != q.min || p.max != q.max
            || p.def != q.def || p.step != q.step || p.unit != q.unit || p.choices != q.choices
            || p.defStatic != q.defStatic || p.animatable != q.animatable)
            return false;
    }
    return true;
}

} // namespace

struct AudioPluginRegistry::Job
{
    QString format;
    QString path;
    audioplug::BundleInfo result;
    QString failure;
};

AudioPluginRegistry::AudioPluginRegistry()
{
    m_pool.setMaxThreadCount(3);
    m_pool.setExpiryTimeout(2000);
}

AudioPluginRegistry& AudioPluginRegistry::instance()
{
    static AudioPluginRegistry registry;
    return registry;
}

void AudioPluginRegistry::setProbeThreads(int n)
{
    m_pool.setMaxThreadCount(std::clamp(n, 1, 4));
}

QString AudioPluginRegistry::userDir(const QString& format)
{
    const QByteArray env = qgetenv(isLv2(format) ? "KK_LV2_USER_DIR" : "KK_CLAP_USER_DIR");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QLatin1Char('/')
        + (isLv2(format) ? QStringLiteral("lv2") : QStringLiteral("clap"));
}

bool AudioPluginRegistry::isPluginType(const QString& type)
{
    return type.startsWith(QLatin1String(kLv2IdPrefix)) || type.startsWith(QLatin1String(kClapIdPrefix));
}

QStringList AudioPluginRegistry::searchDirs(const QString& format) const
{
    const bool lv2 = isLv2(format);
    if (m_overridden[lv2 ? 0 : 1])
        return lv2 ? m_dirsLv2 : m_dirsClap;
    QStringList dirs;
    const QString name = lv2 ? QStringLiteral("lv2") : QStringLiteral("clap");
    const QByteArray bundledEnv = qgetenv(lv2 ? "KK_LV2_BUNDLED_DIR" : "KK_CLAP_BUNDLED_DIR");
    if (!bundledEnv.isEmpty()) {
        dirs << QString::fromLocal8Bit(bundledEnv);
    } else {
        const QString app = QCoreApplication::applicationDirPath();
        for (const QString& candidate :
            {app + QStringLiteral("/../share/kinoklip/") + name, app + QLatin1Char('/') + name}) {
            if (QFileInfo(candidate).isDir()) {
                dirs << QDir::cleanPath(candidate);
                break;
            }
        }
    }
    dirs << userDir(format);
    for (const QString& p : QString::fromLocal8Bit(qgetenv(lv2 ? "LV2_PATH" : "CLAP_PATH"))
                                .split(QLatin1Char(':'), Qt::SkipEmptyParts))
        dirs << p;
    dirs << QDir::homePath() + QLatin1Char('/') + (lv2 ? QStringLiteral(".lv2") : QStringLiteral(".clap"));
    dirs << QStringLiteral("/usr/local/lib/") + name << QStringLiteral("/usr/lib/") + name;
    if (lv2)
        dirs << QStringLiteral("/usr/lib64/lv2");
    dirs.removeDuplicates();
    return dirs;
}

void AudioPluginRegistry::setSearchDirs(const QString& format, const QStringList& dirs)
{
    (isLv2(format) ? m_dirsLv2 : m_dirsClap) = dirs;
    m_overridden[isLv2(format) ? 0 : 1] = true;
}

bool AudioPluginRegistry::enabled(const QString& format) const
{
    return QSettings().value(QStringLiteral("plugins/%1/enabled").arg(format), true).toBool();
}

void AudioPluginRegistry::setEnabled(const QString& format, bool on)
{
    QSettings().setValue(QStringLiteral("plugins/%1/enabled").arg(format), on);
    registerAll();
    emit changed();
}

QString AudioPluginRegistry::cacheFilePath() const
{
    if (!m_cacheFile.isEmpty())
        return m_cacheFile;
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/audio-plugin-scan.json");
}

QString AudioPluginRegistry::markerDir() const
{
    return QFileInfo(cacheFilePath()).absolutePath() + QStringLiteral("/audio-plugin-loading");
}

void AudioPluginRegistry::markLoading(const QString& bundlePath)
{
    QDir().mkpath(markerDir());
    QFile f(markerDir() + QLatin1Char('/')
        + QString::fromLatin1(QCryptographicHash::hash(bundlePath.toUtf8(), QCryptographicHash::Sha1).toHex()));
    if (f.open(QIODevice::WriteOnly))
        f.write(bundlePath.toUtf8());
}

void AudioPluginRegistry::markLoaded(const QString& bundlePath)
{
    QFile::remove(markerDir() + QLatin1Char('/')
        + QString::fromLatin1(QCryptographicHash::hash(bundlePath.toUtf8(), QCryptographicHash::Sha1).toHex()));
}

void AudioPluginRegistry::reportError(const QString& fullId, const QString& message)
{
    QMutexLocker lock(&m_mutex);
    if (message.isEmpty())
        m_errors.remove(fullId);
    else
        m_errors.insert(fullId, message);
}

QString AudioPluginRegistry::error(const QString& fullId) const
{
    QMutexLocker lock(&m_mutex);
    return m_errors.value(fullId);
}

// ---- parameters ----

QStringList AudioPluginRegistry::choiceLabels(const audioplug::ParamInfo& p)
{
    QStringList out;
    for (const QString& label : p.enumLabels) {
        QString v = label;
        for (int n = 2; out.contains(v); ++n)
            v = QStringLiteral("%1 (%2)").arg(label).arg(n);
        out.append(v);
    }
    return out;
}

QVector<ParamDescriptor> AudioPluginRegistry::descriptorParams(const audioplug::PluginInfo& info)
{
    QVector<ParamDescriptor> out;
    for (const audioplug::ParamInfo& p : info.params) {
        if (p.hidden || p.readOnly)
            continue;
        ParamDescriptor d;
        d.key = p.key;
        d.label = p.group.isEmpty() ? p.label : p.group + QStringLiteral(" > ") + p.label;
        d.animatable = false; // the processors read one value per block; track automation uses the key
        if (p.enumeration && !p.enumLabels.isEmpty()) {
            d.type = ParamType::Choice;
            d.choices = choiceLabels(p);
            d.choiceLabels = p.enumLabels;
            int idx = 0;
            for (int k = 0; k < p.enumValues.size(); ++k) {
                if (std::fabs(p.enumValues[k] - p.def) < 1e-6)
                    idx = k;
            }
            d.defStatic = d.choices.value(idx);
        } else if (p.toggle) {
            d.type = ParamType::Bool;
            d.defStatic = p.def >= 0.5;
        } else {
            d.type = p.integer ? ParamType::Int : ParamType::Double;
            d.min = p.min;
            d.max = p.max;
            d.def = std::clamp(p.def, p.min, p.max);
            d.unit = p.unit;
            const double range = p.max - p.min;
            d.step = p.integer ? 1.0 : std::pow(10.0, std::floor(std::log10(std::max(range, 1e-9) / 200.0)));
            if (!(d.step > 0.0) || !std::isfinite(d.step))
                d.step = 0.01;
        }
        out.append(d);
    }
    return out;
}

double AudioPluginRegistry::valueFor(const audioplug::ParamInfo& p, const ParamSet& set)
{
    if (p.enumeration && !p.enumLabels.isEmpty()) {
        const auto it = set.statics.constFind(p.key);
        if (it != set.statics.constEnd()) {
            const QString v = it->toString();
            // no allocation in the usual case (the audio thread calls this for every block)
            for (qsizetype k = 0; k < p.enumLabels.size() && k < p.enumValues.size(); ++k) {
                if (p.enumLabels[k] == v)
                    return p.enumValues[k];
            }
            const qsizetype idx = choiceLabels(p).indexOf(v); // a label that was made unique
            if (idx >= 0 && idx < p.enumValues.size())
                return p.enumValues[idx];
        }
        return p.def;
    }
    if (p.toggle) {
        const auto it = set.statics.constFind(p.key);
        return it != set.statics.constEnd() ? (it->toBool() ? 1.0 : 0.0) : (p.def >= 0.5 ? 1.0 : 0.0);
    }
    double v = set.value(p.key, 0, p.def);
    if (!std::isfinite(v))
        v = p.def;
    v = std::clamp(v, p.min, p.max);
    return p.integer ? std::round(v) : v;
}

// ---- probing and the cache ----

audioplug::BundleInfo AudioPluginRegistry::probe(const QString& format, const QString& path, QString* failure) const
{
    audioplug::BundleInfo none;
    none.path = path;
    none.format = format;
    QString program = m_probeProgram;
    if (program.isEmpty())
        program = QString::fromLocal8Bit(qgetenv("KK_AUDIO_PROBE"));
    if (program.isEmpty())
        program = QCoreApplication::applicationDirPath() + QStringLiteral("/kinoklip-audio-probe");
    if (!QFileInfo(program).isExecutable()) {
        *failure = QStringLiteral("the probe program %1 is missing").arg(program);
        return none;
    }
    QTemporaryFile result;
    result.setFileTemplate(QDir::tempPath() + QStringLiteral("/kk-audio-probe-XXXXXX.json"));
    if (!result.open()) {
        *failure = QStringLiteral("cannot make a temporary file");
        return none;
    }
    result.close();
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    kk::startHelperProcess(process, program, {format, path, result.fileName()});
    if (!process.waitForStarted(10000)) {
        *failure = QStringLiteral("the probe program could not be started");
        return none;
    }
    if (!process.waitForFinished(m_probeTimeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        *failure = QStringLiteral("the plugin did not finish loading within %1 seconds (it hangs)")
                       .arg(m_probeTimeoutMs / 1000.0, 0, 'g', 3);
        return none;
    }
    if (process.exitStatus() != QProcess::NormalExit) {
        *failure = QStringLiteral("the plugin crashed while loading (the probe process was killed by a signal)");
        return none;
    }
    if (process.exitCode() != 0) {
        *failure = QStringLiteral("the probe failed (exit code %1)").arg(process.exitCode());
        return none;
    }
    QFile f(result.fileName());
    if (!f.open(QIODevice::ReadOnly)) {
        *failure = QStringLiteral("the probe wrote no result");
        return none;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *failure = QStringLiteral("the probe's result is unreadable");
        return none;
    }
    audioplug::BundleInfo info = audioplug::BundleInfo::fromJson(doc.object());
    info.path = path;
    info.format = format;
    if (!info.error.isEmpty())
        *failure = info.error;
    return info;
}

void AudioPluginRegistry::loadCache()
{
    QFile f(cacheFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root[QLatin1String("version")].toInt() != kCacheVersion)
        return;
    for (const QJsonValue& v : root[QLatin1String("bundles")].toArray()) {
        const QJsonObject o = v.toObject();
        AudioBundleEntry b;
        b.path = o[QLatin1String("path")].toString();
        b.format = o[QLatin1String("format")].toString();
        b.stampSize = o[QLatin1String("size")].toVariant().toLongLong();
        b.stampMtime = o[QLatin1String("mtime")].toVariant().toLongLong();
        b.bad = o[QLatin1String("bad")].toBool();
        b.reason = o[QLatin1String("reason")].toString();
        b.probed = QDateTime::fromString(o[QLatin1String("probed")].toString(), Qt::ISODate);
        b.info = audioplug::BundleInfo::fromJson(o[QLatin1String("info")].toObject());
        if (!b.path.isEmpty())
            m_bundles.append(b);
    }
}

void AudioPluginRegistry::saveCache() const
{
    if (m_bundles.isEmpty() && !QFileInfo::exists(cacheFilePath()))
        return;
    QJsonArray arr;
    for (const AudioBundleEntry& b : m_bundles) {
        if (b.probing)
            continue;
        QJsonObject o;
        o[QLatin1String("path")] = b.path;
        o[QLatin1String("format")] = b.format;
        o[QLatin1String("size")] = double(b.stampSize);
        o[QLatin1String("mtime")] = double(b.stampMtime);
        o[QLatin1String("bad")] = b.bad;
        o[QLatin1String("reason")] = b.reason;
        o[QLatin1String("probed")] = b.probed.toString(Qt::ISODate);
        o[QLatin1String("info")] = b.info.toJson();
        arr.append(o);
    }
    QJsonObject root;
    root[QLatin1String("version")] = kCacheVersion;
    root[QLatin1String("bundles")] = arr;
    QDir().mkpath(QFileInfo(cacheFilePath()).absolutePath());
    QFile f(cacheFilePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void AudioPluginRegistry::applyCrashMarkers()
{
    // A bundle whose plugin was loading when the editor died is a bad plugin now.
    const QDir markers(markerDir());
    for (const QString& name : markers.entryList(QDir::Files)) {
        QFile f(markers.filePath(name));
        if (f.open(QIODevice::ReadOnly)) {
            const QString path = QString::fromUtf8(f.readAll());
            for (AudioBundleEntry& b : m_bundles) {
                if (b.path == path) {
                    b.bad = true;
                    b.reason = QStringLiteral("Kinoklip crashed the last time this plugin was loaded");
                }
            }
        }
        f.close();
        QFile::remove(markers.filePath(name));
    }
}

void AudioPluginRegistry::resetForTesting()
{
    waitForScan();
    m_loaded = false;
    m_bundles.clear();
}

void AudioPluginRegistry::load()
{
    if (m_loaded)
        return;
    m_loaded = true;
    loadCache();
    applyCrashMarkers();
    startScan(false, {}, false);
}

void AudioPluginRegistry::loadAsync()
{
    if (m_loaded)
        return;
    m_loaded = true;
    loadCache();
    applyCrashMarkers();
    startScan(false, {}, true);
}

void AudioPluginRegistry::reload(bool retryBad)
{
    m_loaded = true;
    startScan(retryBad, {}, false);
}

void AudioPluginRegistry::reloadAsync(bool retryBad)
{
    m_loaded = true;
    startScan(retryBad, {}, true);
}

void AudioPluginRegistry::retry(const QString& path)
{
    markLoaded(path);
    startScan(false, path, false);
}

void AudioPluginRegistry::retryAsync(const QString& path)
{
    markLoaded(path);
    startScan(false, path, true);
}

void AudioPluginRegistry::waitForScan()
{
    if (!m_scanning)
        return;
    QEventLoop loop;
    connect(this, &AudioPluginRegistry::scanFinished, &loop, &QEventLoop::quit);
    if (m_scanning)
        loop.exec();
}

AudioPluginRegistry::Jobs AudioPluginRegistry::discover(bool retryBad, const QString& onlyBundle)
{
    auto jobs = std::make_shared<QVector<Job>>();
    QVector<AudioBundleEntry> found;
    QStringList seen;
    for (const QString& format : {QString::fromLatin1(audioplug::kFormatLv2), QString::fromLatin1(audioplug::kFormatClap)}) {
        const bool lv2 = isLv2(format);
        const QString suffix = lv2 ? QStringLiteral(".lv2") : QStringLiteral(".clap");
        const QByteArray bundledEnv = qgetenv(lv2 ? "KK_LV2_BUNDLED_DIR" : "KK_CLAP_BUNDLED_DIR");
        for (const QString& dir : searchDirs(format)) {
            const QString source = [&] {
                if (dir == userDir(format))
                    return QStringLiteral("user");
                if (dir.contains(QLatin1String("share/kinoklip/")) || dir == QString::fromLocal8Bit(bundledEnv))
                    return QStringLiteral("bundled");
                if (dir.startsWith(QLatin1String("/usr/")))
                    return QStringLiteral("system");
                return QStringLiteral("other");
            }();
            QStringList paths;
            const QDir d(dir);
            const auto isBundle = [&](const QFileInfo& e) {
                return e.fileName().endsWith(suffix) && (lv2 ? e.isDir() : e.isFile());
            };
            for (const QFileInfo& e : d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                if (isBundle(e)) {
                    paths << e.absoluteFilePath();
                } else if (e.isDir()) { // one level of folders (a pack unpacked into a folder of its own)
                    for (const QFileInfo& e2 : QDir(e.absoluteFilePath())
                                                   .entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
                        if (isBundle(e2))
                            paths << e2.absoluteFilePath();
                }
            }
            for (const QString& path : paths) {
                const QString canonical = QFileInfo(path).canonicalFilePath();
                if (seen.contains(canonical))
                    continue;
                seen << canonical;
                AudioBundleEntry entry;
                entry.path = path;
                entry.format = format;
                entry.source = source;
                stampOf(path, &entry.stampSize, &entry.stampMtime);
                const AudioBundleEntry* cached = nullptr;
                for (const AudioBundleEntry& c : std::as_const(m_bundles)) {
                    if (c.path == path && c.format == format) {
                        cached = &c;
                        break;
                    }
                }
                const bool same = cached && cached->stampSize == entry.stampSize && cached->stampMtime == entry.stampMtime;
                if (same && path != onlyBundle && !(retryBad && cached->bad)) {
                    entry = *cached;
                    entry.source = source;
                } else {
                    entry.probing = true;
                    entry.info.path = path;
                    entry.info.format = format;
                    Job job;
                    job.format = format;
                    job.path = path;
                    jobs->append(job);
                }
                found.append(entry);
            }
        }
    }
    m_bundles = found;
    return jobs;
}

void AudioPluginRegistry::startScan(bool retryBad, const QString& onlyBundle, bool async)
{
    if (m_scanning) {
        m_rescanPending = true;
        m_rescanRetry = m_rescanRetry || retryBad;
        m_rescanOnly = onlyBundle;
        if (!async)
            waitForScan();
        return;
    }
    m_scanning = true;
    const Jobs jobs = discover(retryBad, onlyBundle);
    registerAll(); // the cached plugins are there at once
    emit changed();
    if (jobs->isEmpty()) {
        finishScan(jobs);
        return;
    }
    auto remaining = std::make_shared<std::atomic<int>>(int(jobs->size()));
    for (qsizetype i = 0; i < jobs->size(); ++i) {
        m_pool.start(QRunnable::create([this, jobs, i, remaining, async] {
            Job& job = (*jobs)[i];
            job.result = probe(job.format, job.path, &job.failure);
            if (--*remaining == 0 && async)
                QMetaObject::invokeMethod(this, [this, jobs] { finishScan(jobs); }, Qt::QueuedConnection);
        }));
    }
    if (!async) {
        m_pool.waitForDone();
        finishScan(jobs);
    }
}

void AudioPluginRegistry::finishScan(const Jobs& jobs)
{
    for (const Job& job : *jobs) {
        for (AudioBundleEntry& b : m_bundles) {
            if (b.path != job.path || b.format != job.format)
                continue;
            b.probing = false;
            b.info = job.result;
            b.probed = QDateTime::currentDateTime();
            b.bad = !job.failure.isEmpty();
            b.reason = job.failure;
            if (b.bad)
                qCWarning(lcAudioPlugins).noquote()
                    << b.format << "bundle" << b.path << "is a bad plugin:" << job.failure;
        }
    }
    registerAll();
    saveCache();
    m_scanning = false;
    emit changed();
    emit scanFinished();
    if (m_rescanPending) {
        m_rescanPending = false;
        const bool retryBad = m_rescanRetry;
        m_rescanRetry = false;
        const QString only = m_rescanOnly;
        m_rescanOnly.clear();
        startScan(retryBad, only, true);
    }
}

QVector<AudioBundleEntry> AudioPluginRegistry::bundles() const
{
    return m_bundles;
}

const AudioBundleEntry* AudioPluginRegistry::bundle(const QString& path) const
{
    for (const AudioBundleEntry& b : m_bundles) {
        if (b.path == path)
            return &b;
    }
    return nullptr;
}

QVector<AudioPluginEntry> AudioPluginRegistry::allPlugins() const
{
    QMutexLocker lock(&m_mutex);
    return m_all;
}

std::shared_ptr<const AudioPluginEntry> AudioPluginRegistry::plugin(const QString& fullId) const
{
    QMutexLocker lock(&m_mutex);
    return m_plugins.value(fullId);
}

QByteArray AudioPluginRegistry::contentKey() const
{
    QMutexLocker lock(&m_mutex);
    return m_contentKey;
}

void AudioPluginRegistry::registerAll()
{
    EffectRegistry& registry = EffectRegistry::instance();
    registry.markPluginUnavailable(QString::fromLatin1(kLv2PluginTag));
    registry.markPluginUnavailable(QString::fromLatin1(kClapPluginTag));
    QHash<QString, std::shared_ptr<const AudioPluginEntry>> plugins;
    QVector<AudioPluginEntry> all;
    QCryptographicHash hash(QCryptographicHash::Sha1);
    for (const AudioBundleEntry& b : std::as_const(m_bundles)) {
        if (b.bad || b.probing)
            continue;
        const bool on = enabled(b.format);
        for (const audioplug::PluginInfo& info : b.info.plugins) {
            AudioPluginEntry entry;
            entry.fullId = QString::fromLatin1(isLv2(b.format) ? kLv2IdPrefix : kClapIdPrefix) + info.id;
            entry.info = info;
            entry.bundlePath = b.path;
            entry.source = b.source;
            if (!info.error.isEmpty()) {
                all.append(entry);
                continue;
            }
            if (plugins.contains(entry.fullId)) {
                entry.info.error = QStringLiteral("another bundle with the same plugin comes first");
                all.append(entry);
                continue;
            }
            all.append(entry);
            if (!on)
                continue;
            plugins.insert(entry.fullId, std::make_shared<const AudioPluginEntry>(entry));
            hash.addData(QStringLiteral("%1|%2|%3|%4\n").arg(entry.fullId, b.path).arg(b.stampSize).arg(info.version).toUtf8());
            hash.addData(QByteArray::number(b.stampMtime));
            EffectDescriptor d;
            d.id = entry.fullId;
            d.name = info.name;
            d.category = EffectCategory::AudioEffect;
            d.description = !info.description.isEmpty() ? info.description
                : !info.vendor.isEmpty()                ? QStringLiteral("%1 (%2)").arg(info.category, info.vendor).trimmed()
                                                        : info.category;
            d.group = QString::fromLatin1(isLv2(b.format) ? effectgroups::Lv2 : effectgroups::Clap);
            d.plugin = tagOf(b.format);
            d.params = descriptorParams(info);
            const EffectDescriptor* old = registry.find(d.id);
            if (old && sameDescriptor(*old, d)) {
                if (old->unavailable)
                    registry.markAvailable(d.id);
            } else {
                registry.add(d);
            }
        }
    }
    {
        QMutexLocker lock(&m_mutex);
        m_plugins = std::move(plugins);
        m_all = std::move(all);
        m_errors.clear();
        m_contentKey = hash.result().toHex();
    }
    ++m_generation;
}

} // namespace kk::plugins
