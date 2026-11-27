// SPDX-License-Identifier: GPL-3.0-or-later
// OfxRegistry — see OfxRegistry.h.
#include "plugins/OfxRegistry.h"
#include "core/HelperProcess.h"

#include "core/Animation.h"
#include "core/NodeRegistry.h"

#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QEventLoop>
#include <QProcess>
#include <QRunnable>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <algorithm>
#include <cfloat>
#include <cmath>

Q_LOGGING_CATEGORY(lcOfxRegistry, "kk.ofx.registry")

namespace kk::plugins {

namespace {

constexpr int kCacheVersion = 1;
constexpr double kHugeRange = 1.0e6;
constexpr double kFallbackRange = 1.0e4;

/// Choice values made unique (two options may carry the same text).
QStringList uniqueChoices(const QStringList& labels)
{
    QStringList out;
    for (const QString& label : labels) {
        QString v = label;
        for (int n = 2; out.contains(v); ++n)
            v = QStringLiteral("%1 (%2)").arg(label).arg(n);
        out.append(v);
    }
    return out;
}

bool finiteRange(double v)
{
    return std::isfinite(v) && std::fabs(v) < kHugeRange;
}

/// The range a parameter is edited in: the plugin's hard range, else its display range, else
/// a wide default.
void rangeOf(const ofx::ParamInfo& p, int dim, double* lo, double* hi)
{
    const auto at = [dim](const QVector<double>& v, double fallback) {
        return dim < v.size() ? v[dim] : fallback;
    };
    *lo = at(p.min, -kFallbackRange);
    *hi = at(p.max, kFallbackRange);
    if (!finiteRange(*lo))
        *lo = finiteRange(at(p.displayMin, 0)) && at(p.displayMin, -DBL_MAX) > -kHugeRange
            ? at(p.displayMin, -kFallbackRange)
            : -kFallbackRange;
    if (!finiteRange(*hi))
        *hi = finiteRange(at(p.displayMax, 0)) && at(p.displayMax, DBL_MAX) < kHugeRange
            ? at(p.displayMax, kFallbackRange)
            : kFallbackRange;
    if (*hi < *lo)
        std::swap(*lo, *hi);
}

bool isSpatial(const ofx::ParamInfo& p)
{
    return p.doubleType == QLatin1String("X") || p.doubleType == QLatin1String("Y")
        || p.doubleType == QLatin1String("XY");
}

QColor colorFrom(const QVariant& v, const QColor& def)
{
    QColor c = v.value<QColor>();
    if (!c.isValid())
        c = QColor::fromString(v.toString());
    return c.isValid() ? c : def;
}

QColor defaultColor(const ofx::ParamInfo& p)
{
    const auto at = [&](int k, double fallback) { return k < p.def.size() ? p.def[k] : fallback; };
    return QColor::fromRgbF(float(std::clamp(at(0, 0.0), 0.0, 1.0)),
        float(std::clamp(at(1, 0.0), 0.0, 1.0)), float(std::clamp(at(2, 0.0), 0.0, 1.0)),
        float(std::clamp(at(3, 1.0), 0.0, 1.0)));
}

bool sameDescriptor(const EffectDescriptor& a, const EffectDescriptor& b)
{
    if (a.id != b.id || a.name != b.name || a.description != b.description || a.group != b.group
        || a.params.size() != b.params.size())
        return false;
    for (qsizetype i = 0; i < a.params.size(); ++i) {
        const ParamDescriptor& p = a.params[i];
        const ParamDescriptor& q = b.params[i];
        if (p.key != q.key || p.label != q.label || p.type != q.type || p.min != q.min
            || p.max != q.max || p.def != q.def || p.step != q.step || p.unit != q.unit
            || p.choices != q.choices || p.defStatic != q.defStatic || p.animatable != q.animatable
            || p.hidden != q.hidden)
            return false;
    }
    return true;
}

} // namespace

ofx::BundleInfo OfxPluginEntry::bundleInfo() const
{
    ofx::BundleInfo b;
    b.bundlePath = bundlePath;
    b.binary = binary;
    return b;
}

struct OfxRegistry::Job
{
    QString path;
    ofx::BundleInfo result;
    QString failure;
};

OfxRegistry::OfxRegistry()
{
    m_pool.setMaxThreadCount(3);
    m_pool.setExpiryTimeout(2000);
}

void OfxRegistry::setProbeThreads(int n)
{
    m_pool.setMaxThreadCount(std::clamp(n, 1, 4));
}

OfxRegistry& OfxRegistry::instance()
{
    static OfxRegistry registry;
    return registry;
}

QString OfxRegistry::userDir()
{
    const QByteArray env = qgetenv("KK_OFX_USER_DIR");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ofx");
}

QStringList OfxRegistry::searchDirs() const
{
    if (m_overridden)
        return m_dirsOverride;
    QStringList dirs;
    // bundled with Kinoklip (share/kinoklip/ofx next to the program)
    const QByteArray bundledEnv = qgetenv("KK_OFX_BUNDLED_DIR");
    if (!bundledEnv.isEmpty()) {
        dirs << QString::fromLocal8Bit(bundledEnv);
    } else {
        const QString app = QCoreApplication::applicationDirPath();
        for (const QString& candidate :
            {app + QStringLiteral("/../share/kinoklip/ofx"), app + QStringLiteral("/ofx")}) {
            if (QFileInfo(candidate).isDir()) {
                dirs << QDir::cleanPath(candidate);
                break;
            }
        }
    }
    dirs << userDir();
    for (const QString& p : QString::fromLocal8Bit(qgetenv("OFX_PLUGIN_PATH"))
                                .split(QLatin1Char(':'), Qt::SkipEmptyParts))
        dirs << p;
    dirs << QDir::homePath() + QStringLiteral("/.OFX/Plugins") << QStringLiteral("/usr/OFX/Plugins");
    dirs.removeDuplicates();
    return dirs;
}

void OfxRegistry::setSearchDirs(const QStringList& dirs)
{
    m_dirsOverride = dirs;
    m_overridden = true;
}

bool OfxRegistry::enabled() const
{
    return QSettings().value(QStringLiteral("plugins/ofx/enabled"), true).toBool();
}

void OfxRegistry::setEnabled(bool on)
{
    QSettings().setValue(QStringLiteral("plugins/ofx/enabled"), on);
    registerAll();
    emit changed();
}

QString OfxRegistry::cacheFilePath() const
{
    if (!m_cacheFile.isEmpty())
        return m_cacheFile;
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/ofx-scan.json");
}

QString OfxRegistry::markerDir() const
{
    return QFileInfo(cacheFilePath()).absolutePath() + QStringLiteral("/ofx-loading");
}

void OfxRegistry::markLoading(const QString& bundlePath)
{
    QDir().mkpath(markerDir());
    QFile f(markerDir() + QLatin1Char('/')
        + QString::fromLatin1(QCryptographicHash::hash(bundlePath.toUtf8(), QCryptographicHash::Sha1).toHex()));
    if (f.open(QIODevice::WriteOnly))
        f.write(bundlePath.toUtf8());
}

void OfxRegistry::markLoaded(const QString& bundlePath)
{
    QFile::remove(markerDir() + QLatin1Char('/')
        + QString::fromLatin1(QCryptographicHash::hash(bundlePath.toUtf8(), QCryptographicHash::Sha1).toHex()));
}

void OfxRegistry::reportRenderError(const QString& fullId, const QString& message)
{
    QMutexLocker lock(&m_mutex);
    if (message.isEmpty())
        m_errors.remove(fullId);
    else
        m_errors.insert(fullId, message);
}

QString OfxRegistry::renderError(const QString& fullId) const
{
    QMutexLocker lock(&m_mutex);
    return m_errors.value(fullId);
}

// ---- parameters ----

void OfxRegistry::mapParams(
    const ofx::ContextInfo& context, QVector<ParamDescriptor>* descriptors, QVector<OfxParamMap>* maps)
{
    for (const ofx::ParamInfo& p : context.params) {
        if (!p.isValueParam() || p.type == QLatin1String("StrChoice"))
            continue;
        OfxParamMap map;
        map.info = p;
        const auto base = [&](ParamDescriptor* d, const QString& key, const QString& label) {
            // keys are plain words: a '.' would read as "node.parameter" in the Compositing
            // page's expressions
            d->key = key;
            for (QChar& c : d->key) {
                if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
                    c = QLatin1Char('_');
            }
            d->label = label;
            d->tooltip = p.hint;
            d->hidden = p.secret;
        };
        const QString t = p.type;
        if (t == QLatin1String("Double") || t == QLatin1String("Double2D")
            || t == QLatin1String("Double3D") || t == QLatin1String("Integer")
            || t == QLatin1String("Integer2D") || t == QLatin1String("Integer3D")) {
            const bool isInt = t.startsWith(QLatin1String("Integer"));
            map.spatial = isSpatial(p);
            for (int k = 0; k < p.dims; ++k) {
                ParamDescriptor d;
                const QString suffix = p.dims == 1 ? QString()
                                                   : QLatin1Char('_') + QLatin1String(k == 0 ? "x" : k == 1 ? "y" : "z");
                const QString dimLabel = k < p.dimLabels.size() && !p.dimLabels[k].isEmpty()
                    ? p.dimLabels[k]
                    : (p.dims == 1 ? QString() : QString(QLatin1Char("XYZ"[k])));
                base(&d, p.name + suffix,
                    dimLabel.isEmpty() ? p.label : p.label + QLatin1Char(' ') + dimLabel);
                d.type = isInt ? ParamType::Int : ParamType::Double;
                double lo, hi;
                rangeOf(p, k, &lo, &hi);
                double def = k < p.def.size() ? p.def[k] : 0.0;
                if (map.spatial) {
                    // stored as a fraction of the clip size (the default is a fraction too,
                    // or pixels of a 1920 x 1080 clip)
                    if (p.coordinates == QLatin1String("Canonical") && def != 0.0)
                        def /= (k == 1 || p.doubleType == QLatin1String("Y")) ? 1080.0 : 1920.0;
                    lo = std::min(lo, -2.0);
                    hi = std::max(hi, 3.0);
                    if (!(lo >= -kHugeRange))
                        lo = -2.0;
                    lo = std::max(lo, -10.0);
                    hi = std::min(hi, 10.0);
                    d.unit = QString();
                    d.step = 0.01;
                } else {
                    d.step = p.increment > 0.0 ? p.increment : (isInt ? 1.0 : 0.01);
                    if (p.doubleType == QLatin1String("Angle"))
                        d.unit = QStringLiteral("°");
                }
                d.min = lo;
                d.max = hi;
                d.def = std::clamp(def, lo, hi);
                d.animatable = p.animates;
                map.keys.append(d.key);
                if (descriptors)
                    descriptors->append(d);
            }
        } else if (t == QLatin1String("Boolean")) {
            ParamDescriptor d;
            base(&d, p.name, p.label);
            d.type = ParamType::Bool;
            d.defStatic = !p.def.isEmpty() && p.def[0] != 0.0;
            d.animatable = false;
            map.keys.append(d.key);
            if (descriptors)
                descriptors->append(d);
        } else if (t == QLatin1String("Choice")) {
            ParamDescriptor d;
            base(&d, p.name, p.label);
            d.type = ParamType::Choice;
            d.choices = uniqueChoices(p.choiceLabels);
            d.choiceLabels = p.choiceLabels;
            const int idx = p.def.isEmpty() ? 0 : int(p.def[0]);
            d.defStatic = idx >= 0 && idx < d.choices.size() ? d.choices[idx]
                                                              : (d.choices.isEmpty() ? QString() : d.choices[0]);
            d.animatable = false;
            map.keys.append(d.key);
            if (descriptors)
                descriptors->append(d);
        } else if (t == QLatin1String("RGB") || t == QLatin1String("RGBA")) {
            ParamDescriptor d;
            base(&d, p.name, p.label);
            d.type = ParamType::Color;
            d.defStatic = defaultColor(p);
            d.animatable = false;
            map.color = true;
            map.keys.append(d.key);
            if (descriptors)
                descriptors->append(d);
        } else if (t == QLatin1String("String")) {
            ParamDescriptor d;
            base(&d, p.name, p.label);
            const bool file = p.stringMode == QLatin1String("FilePath")
                || p.stringMode == QLatin1String("DirectoryPath");
            d.type = file ? ParamType::File : ParamType::String;
            if (file)
                d.fileFilter = QStringLiteral("All files (*)");
            d.defStatic = p.defString;
            d.animatable = false;
            map.keys.append(d.key);
            if (descriptors)
                descriptors->append(d);
        } else {
            continue;
        }
        if (maps)
            maps->append(map);
    }
}

ofx::ParamValue OfxRegistry::valueFor(
    const OfxParamMap& map, const ParamSet& set, double frame, double widthPx, double heightPx)
{
    const ofx::ParamInfo& p = map.info;
    ofx::ParamValue out;
    const QString t = p.type;
    if (map.color) {
        const QColor c = colorFrom(set.statics.value(map.keys.value(0)), defaultColor(p));
        out.v = {c.redF(), c.greenF(), c.blueF()};
        if (p.dims == 4)
            out.v.append(c.alphaF());
        return out;
    }
    if (t == QLatin1String("Boolean")) {
        out.v = {set.statics.value(map.keys.value(0), !p.def.isEmpty() && p.def[0] != 0.0).toBool() ? 1.0 : 0.0};
        return out;
    }
    if (t == QLatin1String("Choice")) {
        const QStringList choices = uniqueChoices(p.choiceLabels);
        const QString v = set.statics.value(map.keys.value(0)).toString();
        int idx = int(choices.indexOf(v));
        if (idx < 0)
            idx = p.def.isEmpty() ? 0 : int(p.def[0]);
        out.v = {double(idx)};
        return out;
    }
    if (t == QLatin1String("String")) {
        out.s = set.statics.value(map.keys.value(0), p.defString).toString();
        return out;
    }
    for (int k = 0; k < p.dims; ++k) {
        double lo, hi;
        rangeOf(p, k, &lo, &hi);
        double def = k < p.def.size() ? p.def[k] : 0.0;
        if (map.spatial && p.coordinates == QLatin1String("Canonical") && def != 0.0)
            def /= (k == 1 || p.doubleType == QLatin1String("Y")) ? 1080.0 : 1920.0;
        double v = set.value(map.keys.value(k), qint64(std::llround(frame)), def);
        if (frame != std::round(frame))
            v = set.valueAtTime(map.keys.value(k), frame, def);
        v = std::clamp(v, lo, hi);
        if (map.spatial)
            v *= (k == 1 || p.doubleType == QLatin1String("Y")) ? heightPx : widthPx;
        if (t.startsWith(QLatin1String("Integer")))
            v = std::round(v);
        out.v.append(v);
    }
    return out;
}

// ---- probing and the cache ----

ofx::BundleInfo OfxRegistry::probe(const QString& bundlePath, QString* failure) const
{
    ofx::BundleInfo none;
    none.bundlePath = bundlePath;
    QString program = m_probeProgram;
    if (program.isEmpty())
        program = QString::fromLocal8Bit(qgetenv("KK_OFX_PROBE"));
    if (program.isEmpty())
        program = QCoreApplication::applicationDirPath() + QStringLiteral("/kinoklip-ofx-probe");
    if (!QFileInfo(program).isExecutable()) {
        *failure = QStringLiteral("the probe program %1 is missing").arg(program);
        return none;
    }
    QTemporaryFile result;
    result.setFileTemplate(QDir::tempPath() + QStringLiteral("/kk-ofx-probe-XXXXXX.json"));
    if (!result.open()) {
        *failure = QStringLiteral("cannot make a temporary file");
        return none;
    }
    result.close();
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    // The plugin must not find the editor's settings or display: only its own files.
    kk::startHelperProcess(process, program, {bundlePath, result.fileName()});
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
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        *failure = QStringLiteral("the probe's result is unreadable");
        return none;
    }
    ofx::BundleInfo info = ofx::BundleInfo::fromJson(doc.object());
    info.bundlePath = bundlePath;
    if (!info.error.isEmpty())
        *failure = info.error;
    return info;
}

void OfxRegistry::loadCache()
{
    QFile f(cacheFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root[QLatin1String("version")].toInt() != kCacheVersion)
        return;
    for (const QJsonValue& v : root[QLatin1String("bundles")].toArray()) {
        const QJsonObject o = v.toObject();
        OfxBundleEntry b;
        b.path = o[QLatin1String("path")].toString();
        b.binary = o[QLatin1String("binary")].toString();
        b.binarySize = o[QLatin1String("size")].toVariant().toLongLong();
        b.binaryMtime = o[QLatin1String("mtime")].toVariant().toLongLong();
        b.bad = o[QLatin1String("bad")].toBool();
        b.reason = o[QLatin1String("reason")].toString();
        b.probed = QDateTime::fromString(o[QLatin1String("probed")].toString(), Qt::ISODate);
        b.info = ofx::BundleInfo::fromJson(o[QLatin1String("info")].toObject());
        if (!b.path.isEmpty())
            m_bundles.append(b);
    }
}

void OfxRegistry::saveCache() const
{
    if (m_bundles.isEmpty() && !QFileInfo::exists(cacheFilePath()))
        return; // nothing found, nothing to remember (and no cache folder made for it)
    QJsonArray arr;
    for (const OfxBundleEntry& b : m_bundles) {
        if (b.probing)
            continue;
        QJsonObject o;
        o[QLatin1String("path")] = b.path;
        o[QLatin1String("binary")] = b.binary;
        o[QLatin1String("size")] = double(b.binarySize);
        o[QLatin1String("mtime")] = double(b.binaryMtime);
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

void OfxRegistry::load()
{
    if (m_loaded)
        return;
    m_loaded = true;
    loadCache();
    applyCrashMarkers();
    scan(false, {}, false);
}

void OfxRegistry::applyCrashMarkers()
{
    // A bundle whose plugin was loading when the editor died is a bad plugin now.
    const QDir markers(markerDir());
    for (const QString& name : markers.entryList(QDir::Files)) {
        QFile f(markers.filePath(name));
        if (f.open(QIODevice::ReadOnly)) {
            const QString path = QString::fromUtf8(f.readAll());
            for (OfxBundleEntry& b : m_bundles) {
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

void OfxRegistry::loadAsync()
{
    if (m_loaded)
        return;
    m_loaded = true;
    loadCache();
    applyCrashMarkers();
    scan(false, {}, true);
}

void OfxRegistry::reload(bool retryBad)
{
    m_loaded = true;
    scan(retryBad, {}, false);
}

void OfxRegistry::reloadAsync(bool retryBad)
{
    m_loaded = true;
    scan(retryBad, {}, true);
}

void OfxRegistry::retry(const QString& path)
{
    markLoaded(path);
    scan(false, path, false);
}

void OfxRegistry::retryAsync(const QString& path)
{
    markLoaded(path);
    scan(false, path, true);
}

void OfxRegistry::waitForScan()
{
    if (!m_scanning)
        return;
    QEventLoop loop;
    connect(this, &OfxRegistry::scanFinished, &loop, &QEventLoop::quit);
    if (m_scanning)
        loop.exec();
}

void OfxRegistry::scan(bool retryBad, const QString& onlyBundle, bool async)
{
    if (m_scanning) { // one scan at a time: this one follows when the running one is done
        m_rescanPending = true;
        m_rescanRetry = m_rescanRetry || retryBad;
        m_rescanOnly = onlyBundle;
        if (!async)
            waitForScan();
        return;
    }
    m_scanning = true;
    const Jobs jobs = discover(retryBad, onlyBundle);
    registerAll(); // what the cache knows is there at once
    emit changed();
    if (jobs->isEmpty()) {
        finishScan(jobs);
        return;
    }
    auto remaining = std::make_shared<std::atomic<int>>(int(jobs->size()));
    for (qsizetype i = 0; i < jobs->size(); ++i) {
        m_pool.start(QRunnable::create([this, jobs, i, remaining, async] {
            Job& job = (*jobs)[i];
            job.result = probe(job.path, &job.failure);
            if (--*remaining == 0 && async)
                QMetaObject::invokeMethod(this, [this, jobs] { finishScan(jobs); }, Qt::QueuedConnection);
        }));
    }
    if (!async) {
        m_pool.waitForDone();
        finishScan(jobs);
    }
}

void OfxRegistry::finishScan(const Jobs& jobs)
{
    for (const Job& job : *jobs) {
        for (OfxBundleEntry& b : m_bundles) {
            if (b.path != job.path)
                continue;
            b.probing = false;
            b.info = job.result;
            b.probed = QDateTime::currentDateTime();
            b.bad = !job.failure.isEmpty();
            b.reason = job.failure;
            if (b.bad)
                qCWarning(lcOfxRegistry).noquote() << "OpenFX bundle" << b.path << "is a bad plugin:" << job.failure;
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
        scan(retryBad, only, true);
    }
}

OfxRegistry::Jobs OfxRegistry::discover(bool retryBad, const QString& onlyBundle)
{
    auto jobs = std::make_shared<QVector<Job>>();
    const QStringList dirs = searchDirs();
    const QString bundledMarker = QString::fromLocal8Bit(qgetenv("KK_OFX_BUNDLED_DIR"));
    QVector<OfxBundleEntry> found;
    QStringList seen;
    for (const QString& dir : dirs) {
        const QString source = [&] {
            if (dir == userDir())
                return QStringLiteral("user");
            if (dir.contains(QLatin1String("share/kinoklip/ofx")) || dir == bundledMarker)
                return QStringLiteral("bundled");
            if (dir == QLatin1String("/usr/OFX/Plugins"))
                return QStringLiteral("system");
            return QStringLiteral("other");
        }();
        QStringList bundlePaths;
        const QDir d(dir);
        for (const QString& name : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString p = d.filePath(name);
            if (name.endsWith(QLatin1String(".ofx.bundle"))) {
                bundlePaths << p;
            } else { // one level of folders (packs unpacked into a folder of their own)
                const QDir sub(p);
                for (const QString& n2 : sub.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
                    if (n2.endsWith(QLatin1String(".ofx.bundle")))
                        bundlePaths << sub.filePath(n2);
            }
        }
        for (const QString& raw : bundlePaths) {
            const QString path = QFileInfo(raw).absoluteFilePath();
            const QString canonical = QFileInfo(raw).canonicalFilePath();
            if (seen.contains(canonical))
                continue;
            seen << canonical;
            OfxBundleEntry entry;
            entry.path = path;
            entry.source = source;
            entry.binary = ofx::bundleBinary(path);
            const QFileInfo bin(entry.binary);
            entry.binarySize = bin.size();
            entry.binaryMtime = bin.lastModified().toSecsSinceEpoch();
            const OfxBundleEntry* cached = nullptr;
            for (const OfxBundleEntry& c : std::as_const(m_bundles)) {
                if (c.path == path) {
                    cached = &c;
                    break;
                }
            }
            const bool same = cached && cached->binary == entry.binary
                && cached->binarySize == entry.binarySize && cached->binaryMtime == entry.binaryMtime;
            if (entry.binary.isEmpty()) {
                entry.bad = true;
                entry.reason = QStringLiteral("the bundle has no binary for this machine (Contents/Linux-x86-64/*.ofx)");
                entry.info.bundlePath = path;
                entry.probed = QDateTime::currentDateTime();
            } else if (same && path != onlyBundle && !(retryBad && cached->bad)) {
                entry = *cached;
                entry.source = source;
            } else {
                entry.probing = true; // the answer comes from the probe (finishScan)
                entry.info.bundlePath = path;
                Job job;
                job.path = path;
                jobs->append(job);
            }
            found.append(entry);
        }
    }
    m_bundles = found;
    return jobs;
}

QVector<OfxBundleEntry> OfxRegistry::bundles() const
{
    return m_bundles;
}

const OfxBundleEntry* OfxRegistry::bundle(const QString& path) const
{
    for (const OfxBundleEntry& b : m_bundles) {
        if (b.path == path)
            return &b;
    }
    return nullptr;
}

QVector<OfxPluginEntry> OfxRegistry::allPlugins() const
{
    QMutexLocker lock(&m_mutex);
    return m_all;
}

std::shared_ptr<const OfxPluginEntry> OfxRegistry::plugin(const QString& fullId) const
{
    QMutexLocker lock(&m_mutex);
    return m_plugins.value(fullId);
}

QByteArray OfxRegistry::contentKey() const
{
    QMutexLocker lock(&m_mutex);
    return m_contentKey;
}

void OfxRegistry::registerAll()
{
    EffectRegistry& registry = EffectRegistry::instance();
    registry.markPluginUnavailable(QString::fromLatin1(kOfxPluginTag));
    QHash<QString, std::shared_ptr<const OfxPluginEntry>> plugins;
    QVector<OfxPluginEntry> all;
    QCryptographicHash hash(QCryptographicHash::Sha1);
    const bool on = enabled();
    for (const OfxBundleEntry& b : std::as_const(m_bundles)) {
        if (b.bad || b.probing)
            continue;
        // A bundle may carry several versions of one plugin (Natron's packs keep old ones for old
        // projects): the highest version is the one offered.
        QHash<QString, int> best;
        for (int k = 0; k < b.info.plugins.size(); ++k) {
            const ofx::PluginInfo& p = b.info.plugins[k];
            const auto it = best.constFind(p.identifier);
            if (it == best.constEnd()
                || std::pair(p.major, p.minor)
                    > std::pair(b.info.plugins[*it].major, b.info.plugins[*it].minor))
                best.insert(p.identifier, k);
        }
        for (int pk = 0; pk < b.info.plugins.size(); ++pk) {
            const ofx::PluginInfo& info = b.info.plugins[pk];
            OfxPluginEntry entry;
            entry.fullId = QString::fromLatin1(kOfxIdPrefix) + info.identifier;
            entry.info = info;
            entry.bundlePath = b.path;
            entry.binary = b.binary;
            entry.source = b.source;
            const ofx::ContextInfo* ctx = info.usableContext();
            if (best.value(info.identifier) != pk) {
                entry.info.error = QStringLiteral("a newer version of this plugin is offered");
                all.append(entry);
                continue;
            }
            if (!info.error.isEmpty() || !ctx) {
                all.append(entry);
                continue;
            }
            if (plugins.contains(entry.fullId)) {
                entry.info.error = QStringLiteral("another bundle with the same plugin comes first");
                all.append(entry);
                continue;
            }
            QVector<ParamDescriptor> params;
            mapParams(*ctx, &params, &entry.params);
            const bool transition = info.isTransition();
            if (transition) {
                // "Transition" is the progress the host sets: not a control of the Inspector.
                for (int i = entry.params.size() - 1; i >= 0; --i) {
                    if (entry.params[i].info.name == QLatin1String("Transition")) {
                        const QStringList keys = entry.params[i].keys;
                        params.erase(std::remove_if(params.begin(), params.end(),
                                         [&keys](const ParamDescriptor& d) {
                                             return keys.contains(d.key);
                                         }),
                            params.end());
                        entry.params.removeAt(i);
                    }
                }
            }
            all.append(entry);
            if (!on)
                continue;
            plugins.insert(entry.fullId, std::make_shared<const OfxPluginEntry>(entry));
            hash.addData(QStringLiteral("%1|%2|%3|%4.%5\n")
                             .arg(entry.fullId, b.path)
                             .arg(b.binarySize)
                             .arg(info.major)
                             .arg(info.minor)
                             .toUtf8());
            hash.addData(QByteArray::number(b.binaryMtime));
            EffectDescriptor d;
            d.id = entry.fullId;
            d.name = info.label;
            d.category = transition ? EffectCategory::Transition : EffectCategory::VideoEffect;
            d.description = info.description.isEmpty() ? info.group : info.description;
            d.group = QString::fromLatin1(effectgroups::OpenFX);
            d.plugin = QString::fromLatin1(kOfxPluginTag);
            d.params = params;
            const EffectDescriptor* old = registry.find(d.id);
            if (old && sameDescriptor(*old, d)) {
                if (old->unavailable)
                    registry.markAvailable(d.id);
            } else {
                registry.add(d);
                if (!transition)
                    NodeRegistry::registerEffectNode(*registry.find(d.id));
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
