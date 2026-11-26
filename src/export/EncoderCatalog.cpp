// SPDX-License-Identifier: GPL-3.0-or-later
// EncoderCatalog — encoder tables and hardware encoder detection.
#include "export/EncoderCatalog.h"

#include "export/ExportFFmpeg.h"
#include "export/ExportLog.h"
#include "export/VideoEncoder.h"
#include "app/AppInfo.h"
#include "media/HwAccel.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryFile>

#include <mutex>

namespace kk {

using namespace kk::exporter;

namespace {

/// Size of the detection picture: above every encoder's minimum (NVENC ~145x49, VAAPI 16x16)
/// and small enough to probe in a few ms.
constexpr int kProbeWidth = 320;
constexpr int kProbeHeight = 240;

struct HwRow
{
    const char* codec;
    const char* encoder;
    EncoderBackend backend;
};

/// Hardware encoders in Auto order per codec: NVENC (best quality/speed where present),
/// VAAPI (Intel/AMD — the target laptop's Iris Xe), QSV (Intel via oneVPL).
constexpr HwRow kHardware[] = {
    {"h264", "h264_nvenc", EncoderBackend::Nvenc},
    {"h264", "h264_vaapi", EncoderBackend::Vaapi},
    {"h264", "h264_qsv", EncoderBackend::Qsv},
    {"hevc", "hevc_nvenc", EncoderBackend::Nvenc},
    {"hevc", "hevc_vaapi", EncoderBackend::Vaapi},
    {"hevc", "hevc_qsv", EncoderBackend::Qsv},
    {"av1", "av1_nvenc", EncoderBackend::Nvenc},
    {"av1", "av1_vaapi", EncoderBackend::Vaapi},
    {"av1", "av1_qsv", EncoderBackend::Qsv},
    {"vp9", "vp9_vaapi", EncoderBackend::Vaapi},
    {"vp9", "vp9_qsv", EncoderBackend::Qsv},
};

/// Software encoders per codec, best first.
QStringList softwareNames(const QString& codec)
{
    static const QHash<QString, QStringList> table {
        {QStringLiteral("h264"), {QStringLiteral("libx264")}},
        {QStringLiteral("hevc"), {QStringLiteral("libx265")}},
        {QStringLiteral("av1"), {QStringLiteral("libsvtav1"), QStringLiteral("libaom-av1")}},
        {QStringLiteral("vp9"), {QStringLiteral("libvpx-vp9")}},
        {QStringLiteral("prores"), {QStringLiteral("prores_ks")}},
        {QStringLiteral("dnxhr"), {QStringLiteral("dnxhd")}},
        {QStringLiteral("ffv1"), {QStringLiteral("ffv1")}},
        {QStringLiteral("png"), {QStringLiteral("png")}},
        {QStringLiteral("mjpeg"), {QStringLiteral("mjpeg")}},
        {QStringLiteral("tiff"), {QStringLiteral("tiff")}},
        {QStringLiteral("exr"), {QStringLiteral("exr")}},
        {QStringLiteral("gif"), {QStringLiteral("gif")}},
    };
    return table.value(codec);
}

struct ProbeResult
{
    HwEncoderStatus status;
    AVBufferRef* device = nullptr; ///< VAAPI device (kept for the process lifetime)
    bool lowPower = false;
    QString driver; ///< VAAPI driver ("iHD 24.1.0") for the summary line
};

/// Process-wide cache. Devices are never released (drivers must not be torn down during
/// static destruction; see media's HwDeviceManager for the same reasoning).
struct Cache
{
    std::mutex mutex;
    QHash<QString, ProbeResult> results;
    QHash<QString, AVBufferRef*> vaapiDevices; ///< render node → device (nullptr = failed)
    QSet<QString> summarised; ///< summary scopes already logged ("all", "h264", ...)
    // The test program and the disk cache.
    QString probeProgram; ///< empty: kinoklip-hwenc-probe next to the executable
    int probeTimeoutMs = 10000;
    QString cacheFile; ///< empty: <cache dir>/hwenc-probe.json
    bool diskLoaded = false;
    int probeRuns = 0;
    QString vaapiFault; ///< set once a VAAPI test crashed or hung: why the other ones are skipped
};

Cache& cache()
{
    static Cache* instance = new Cache; // intentionally leaked, see above
    return *instance;
}

bool envIsOff(const char* name)
{
    return qEnvironmentVariable(name).compare(QLatin1String("off"), Qt::CaseInsensitive) == 0;
}

/// KINOKLIP_HWENC=off, and KINOKLIP_HWACCEL=off (users expect "no hardware" to mean no hardware).
bool hardwareDisabled()
{
    return envIsOff("KINOKLIP_HWENC") || envIsOff("KINOKLIP_HWACCEL");
}

QString disabledReason()
{
    return envIsOff("KINOKLIP_HWENC") ? QStringLiteral("disabled by KINOKLIP_HWENC=off")
                                      : QStringLiteral("disabled by KINOKLIP_HWACCEL=off");
}

/// Opens @p config and encodes one grey frame. @return true if a packet came out.
/// FFmpeg's messages during the probe (an unsupported profile, a missing entry point...) are
/// expected failures: they are captured, the first one becomes part of @p error.
bool tryEncode(VideoEncoderConfig config, QString* error)
{
    FfmpegQuiet capture;
    const bool ok = [&] {
        config.size = {kProbeWidth, kProbeHeight};
        config.fps = {25, 1};
        config.settings.rateControl = RateControl::Quality;
        config.settings.quality = 30;
        config.settings.speed = QStringLiteral("fast");
        VideoEncoder encoder;
        if (!encoder.open(config, error)) {
            return false;
        }
        QImage frame(kProbeWidth, kProbeHeight, QImage::Format_ARGB32_Premultiplied);
        frame.fill(QColor(128, 128, 128));
        int packets = 0;
        const PacketSink sink = [&](AVPacket*) {
            ++packets;
            return true;
        };
        if (!encoder.encode(frame, 0, sink, error) || !encoder.flush(sink, error)) {
            return false;
        }
        if (packets == 0 && error) {
            *error = QStringLiteral("no output");
        }
        return packets > 0;
    }();
    if (!ok && error && !capture.firstError().isEmpty()) {
        *error += QStringLiteral(" (%1)").arg(capture.firstError());
    }
    return ok;
}

QStringList renderNodes()
{
#ifdef Q_OS_LINUX
    const QDir dri(QStringLiteral("/dev/dri"));
    QStringList nodes;
    // System | Files: Qt 6.12 lists no device node with System alone (spec 125.3).
    for (const QString& name :
        dri.entryList({QStringLiteral("renderD*")}, QDir::System | QDir::Files, QDir::Name)) {
        nodes << dri.absoluteFilePath(name);
    }
    return nodes;
#else
    return {};
#endif
}

/// Device for @p node (cache.mutex held). Created once; failures are remembered.
AVBufferRef* vaapiDeviceFor(Cache& c, const QString& node, QString* error)
{
    if (auto it = c.vaapiDevices.constFind(node); it != c.vaapiDevices.constEnd()) {
        if (!*it && error) {
            *error = QStringLiteral("cannot open VAAPI on %1").arg(node);
        }
        return *it;
    }
    AVBufferRef* device = nullptr;
    FfmpegQuiet capture; // e.g. no permission on the node: say it once
    const int err = av_hwdevice_ctx_create(
        &device, AV_HWDEVICE_TYPE_VAAPI, node.toUtf8().constData(), nullptr, 0);
    if (err < 0) {
        device = nullptr;
        if (error) {
            *error = QStringLiteral("cannot open VAAPI on %1: %2").arg(node, avError(err));
            if (!capture.firstError().isEmpty()) {
                *error += QStringLiteral(" (%1)").arg(capture.firstError());
            }
        }
    }
    c.vaapiDevices.insert(node, device);
    return device;
}

/// Answers without opening anything: disabled, not built in, no hardware. @return true if
/// @p result is final.
bool quickAnswer(const HwRow& row, ProbeResult* result)
{
    result->status.codecId = QString::fromLatin1(row.codec);
    result->status.encoderName = QString::fromLatin1(row.encoder);
    result->status.backend = row.backend;
    if (hardwareDisabled()) {
        result->status.error = disabledReason();
        return true;
    }
    if (!avcodec_find_encoder_by_name(row.encoder)) {
        result->status.error = QStringLiteral("not in this FFmpeg build");
        return true;
    }
    // Do not even try what cannot work: FFmpeg would log CUDA_ERROR_NO_DEVICE / "Error
    // creating a MFX session" errors on every machine without the hardware.
    if (row.backend == EncoderBackend::Nvenc && !media::nvidiaDriverPresent()) {
        result->status.error = QStringLiteral("no NVIDIA driver");
        return true;
    }
    if (row.backend == EncoderBackend::Qsv && !media::qsvRuntimePresent()) {
        result->status.error = QStringLiteral("no Intel GPU with a oneVPL/MFX runtime");
        return true;
    }
    if (row.backend == EncoderBackend::Vaapi && renderNodes().isEmpty()) {
        result->status.error = QStringLiteral("no render node in /dev/dri");
        return true;
    }
    return false;
}

/// Opens the encoder and encodes a frame in THIS process (what kinoklip-hwenc-probe runs).
ProbeResult probeLocal(Cache& c, const HwRow& row)
{
    ProbeResult result;
    if (quickAnswer(row, &result)) {
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    VideoEncoderConfig config;
    config.encoderName = result.status.encoderName;
    config.backend = row.backend;
    config.settings.codec = result.status.codecId;
    QString error;
    if (row.backend == EncoderBackend::Vaapi) {
        const QStringList nodes = renderNodes();
        if (nodes.isEmpty()) {
            error = QStringLiteral("no render node in /dev/dri");
        }
        for (const QString& node : nodes) {
            AVBufferRef* device = vaapiDeviceFor(c, node, &error);
            if (!device) {
                continue;
            }
            config.hwDevice = device;
            for (bool lowPower : {false, true}) {
                config.lowPower = lowPower;
                if (tryEncode(config, &error)) {
                    result.status.available = true;
                    result.status.device = node;
                    result.device = device;
                    result.lowPower = lowPower;
                    result.status.lowPower = lowPower;
                    break;
                }
            }
            if (result.status.available) {
                break;
            }
        }
    } else {
        result.status.available = tryEncode(config, &error);
    }
    if (!result.status.available) {
        result.status.error = error;
    }
    if (result.status.available && result.device) {
        result.driver = media::vaapiDriverName(result.device);
        result.status.driver = result.driver;
    }
    qCDebug(lcExport).noquote() << "hardware encoder" << result.status.encoderName
                                << (result.status.available ? QStringLiteral("available")
                                                            : QStringLiteral("unavailable:"))
                                << (result.status.available ? result.status.device
                                                            : result.status.error)
                                << QStringLiteral("(%1 ms)").arg(timer.elapsed());
    return result;
}

QJsonObject statusToJson(const ProbeResult& r)
{
    return {{QLatin1String("available"), r.status.available},
        {QLatin1String("device"), r.status.device}, {QLatin1String("lowPower"), r.lowPower},
        {QLatin1String("driver"), r.driver}, {QLatin1String("error"), r.status.error},
        {QLatin1String("probeFailed"), r.status.probeFailed}};
}

ProbeResult statusFromJson(const HwRow& row, const QJsonObject& o)
{
    ProbeResult r;
    r.status.codecId = QString::fromLatin1(row.codec);
    r.status.encoderName = QString::fromLatin1(row.encoder);
    r.status.backend = row.backend;
    r.status.available = o[QLatin1String("available")].toBool();
    r.status.device = o[QLatin1String("device")].toString();
    r.status.error = o[QLatin1String("error")].toString();
    r.status.probeFailed = o[QLatin1String("probeFailed")].toBool();
    r.lowPower = o[QLatin1String("lowPower")].toBool();
    r.status.lowPower = r.lowPower;
    r.driver = o[QLatin1String("driver")].toString();
    r.status.driver = r.driver;
    return r;
}

QString cacheFilePath(const Cache& c)
{
    if (!c.cacheFile.isEmpty()) {
        return c.cacheFile;
    }
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/hwenc-probe.json");
}

QString sysfsText(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(256)).trimmed() : QString();
}

/// Everything that decides whether a hardware encoder works, read without touching the GPU.
QString computeDriverKey()
{
    QStringList parts;
    parts << appinfo::displayVersion() << QSysInfo::kernelVersion()
          << QString::number(avcodec_version());
#ifdef Q_OS_LINUX
    for (const QString& node : renderNodes()) {
        const QString base
            = QStringLiteral("/sys/class/drm/%1/device").arg(QFileInfo(node).fileName());
        parts << QStringLiteral("%1 %2:%3 %4")
                     .arg(QFileInfo(node).fileName(), sysfsText(base + QStringLiteral("/vendor")),
                         sysfsText(base + QStringLiteral("/device")),
                         QFileInfo(base + QStringLiteral("/driver")).symLinkTarget());
    }
    parts << sysfsText(QStringLiteral("/proc/driver/nvidia/version")).left(120);
#endif
    // Mesa / Intel media driver updates replace these files.
    QStringList dirs;
    if (const QString path = qEnvironmentVariable("LIBVA_DRIVERS_PATH"); !path.isEmpty()) {
        dirs = path.split(u':', Qt::SkipEmptyParts);
    } else {
        dirs = {QStringLiteral("/usr/lib/x86_64-linux-gnu/dri"), QStringLiteral("/usr/lib64/dri"),
            QStringLiteral("/usr/lib/dri")};
    }
    for (const QString& dir : std::as_const(dirs)) {
        for (const QFileInfo& file : QDir(dir).entryInfoList({QStringLiteral("*_drv_video.so")})) {
            parts << QStringLiteral("%1 %2 %3")
                         .arg(file.canonicalFilePath())
                         .arg(file.size())
                         .arg(file.lastModified().toSecsSinceEpoch());
        }
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(parts.join(u'\n').toUtf8(), QCryptographicHash::Sha1).toHex());
}

/// Loads the answers of the disk cache once (c.mutex held); ignored when the key differs.
void loadDiskCache(Cache& c)
{
    if (c.diskLoaded) {
        return;
    }
    c.diskLoaded = true;
    QFile f(cacheFilePath(c));
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root[QLatin1String("key")].toString() != computeDriverKey()) {
        return;
    }
    const QJsonObject results = root[QLatin1String("results")].toObject();
    for (const HwRow& row : kHardware) {
        const QString name = QString::fromLatin1(row.encoder);
        if (results.contains(name) && !c.results.contains(name)) {
            c.results.insert(name, statusFromJson(row, results[name].toObject()));
        }
    }
    c.vaapiFault = root[QLatin1String("vaapiFault")].toString();
}

void saveDiskCache(Cache& c)
{
    QJsonObject results;
    for (const HwRow& row : kHardware) {
        const auto it = c.results.constFind(QString::fromLatin1(row.encoder));
        if (it != c.results.constEnd()) {
            results.insert(QString::fromLatin1(row.encoder), statusToJson(*it));
        }
    }
    const QString path = cacheFilePath(c);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(QJsonObject {{QLatin1String("key"), computeDriverKey()},
                                   {QLatin1String("results"), results},
                                   {QLatin1String("vaapiFault"), c.vaapiFault}})
                    .toJson(QJsonDocument::Compact));
    }
}

/// Runs kinoklip-hwenc-probe for @p row: a crash, a hang or a bad exit becomes "unavailable" with
/// the reason. @p cacheable is false when the answer must not be remembered at all (the probe
/// program is missing or could not start: nothing was tested).
ProbeResult probe(Cache& c, const HwRow& row, bool* cacheable)
{
    *cacheable = true;
    ProbeResult result;
    if (quickAnswer(row, &result)) {
        return result;
    }
    const QString name = QString::fromLatin1(row.encoder);
    auto fail = [&](const QString& why, bool fault) {
        result.status.available = false;
        result.status.error = why;
        result.status.probeFailed = fault;
        return result;
    };
    if (row.backend == EncoderBackend::Vaapi && !c.vaapiFault.isEmpty()) {
        return fail(c.vaapiFault, true);
    }
    QString program = c.probeProgram;
    if (program.isEmpty()) {
        program = qEnvironmentVariable("KK_HWENC_PROBE");
    }
    if (program.isEmpty()) {
        program = QCoreApplication::applicationDirPath() + QStringLiteral("/kinoklip-hwenc-probe");
    }
    if (!QFileInfo(program).isExecutable()) {
        *cacheable = false;
        return fail(QStringLiteral("the probe program %1 is missing").arg(program), false);
    }
    QTemporaryFile out;
    out.setFileTemplate(QDir::tempPath() + QStringLiteral("/kk-hwenc-probe-XXXXXX.json"));
    if (!out.open()) {
        *cacheable = false;
        return fail(QStringLiteral("cannot make a temporary file"), false);
    }
    out.close();
    ++c.probeRuns;
    auto* process = new QProcess; // leaked if it cannot be killed (stuck in the driver)
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->start(program, {name, out.fileName()});
    if (!process->waitForStarted(10000)) {
        delete process;
        *cacheable = false;
        return fail(QStringLiteral("the probe program could not be started"), false);
    }
    if (!process->waitForFinished(c.probeTimeoutMs)) {
        process->kill();
        if (process->waitForFinished(2000)) {
            delete process;
        } // else: a process stuck in the kernel; let it be
        result = fail(QStringLiteral("the driver did not answer within %1 seconds while testing "
                                     "the encoder (%2), it hangs")
                          .arg(c.probeTimeoutMs / 1000.0, 0, 'g', 3)
                          .arg(name),
            true);
    } else if (process->exitStatus() != QProcess::NormalExit) {
        delete process;
        result = fail(QStringLiteral("the driver crashed while testing the encoder (%1)").arg(name),
            true);
    } else {
        const int code = process->exitCode();
        delete process;
        QFile f(out.fileName());
        const QJsonDocument doc
            = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()) : QJsonDocument();
        if (code != 0 || !doc.isObject()) {
            result = fail(QStringLiteral("the encoder test failed (exit code %1)").arg(code), true);
        } else {
            result = statusFromJson(row, doc.object());
        }
    }
    if (result.status.probeFailed && row.backend == EncoderBackend::Vaapi) {
        // The driver is in trouble: do not hand it the other VAAPI encoders (a VCN timeout resets
        // the GPU for everybody).
        c.vaapiFault = QStringLiteral("not tested: the VAAPI driver failed while testing %1 (%2)")
                           .arg(name, result.status.error);
    }
    qCDebug(lcExport).noquote() << "hardware encoder" << name
                                << (result.status.available ? QStringLiteral("available")
                                                            : QStringLiteral("unavailable:"))
                                << (result.status.available ? result.status.device
                                                            : result.status.error);
    return result;
}

const HwRow* findRow(const QString& encoderName)
{
    for (const HwRow& row : kHardware) {
        if (encoderName == QLatin1String(row.encoder)) {
            return &row;
        }
    }
    return nullptr;
}

/// Cached probe result of a hardware encoder (probes on first use).
ProbeResult cachedProbe(const HwRow& row)
{
    Cache& c = cache();
    // Probing under the lock serialises detection (GPU drivers dislike concurrent device
    // creation, and a second caller would only probe the same thing again).
    std::lock_guard lock(c.mutex);
    const QString key = QString::fromLatin1(row.encoder);
    if (hardwareDisabled()) {
        ProbeResult off;
        quickAnswer(row, &off);
        return off;
    }
    loadDiskCache(c);
    if (auto it = c.results.constFind(key); it != c.results.constEnd()) {
        return *it;
    }
    bool cacheable = true;
    ProbeResult result = probe(c, row, &cacheable);
    if (cacheable) {
        c.results.insert(key, result);
        saveDiskCache(c);
    }
    return result;
}

const char* backendLabel(EncoderBackend backend)
{
    switch (backend) {
    case EncoderBackend::Nvenc:
        return "NVENC";
    case EncoderBackend::Vaapi:
        return "VAAPI";
    case EncoderBackend::Qsv:
        return "QSV";
    default:
        return "?";
    }
}

/// One info line per probe scope instead of one per encoder (and none of FFmpeg's), e.g.
/// "hardware encoders: VAAPI h264/hevc (iHD 24.1.0, /dev/dri/renderD128), NVENC unavailable
/// (no NVIDIA driver), QSV unavailable (no Intel GPU with a oneVPL/MFX runtime)".
/// @p scope is "" for every encoder or a codec id; logged once per scope.
void logSummary(const QString& scope)
{
    Cache& c = cache();
    QStringList parts;
    {
        std::lock_guard lock(c.mutex);
        if (c.summarised.contains(scope) || c.summarised.contains(QString())) {
            return; // said already (the full summary covers every codec)
        }
        c.summarised.insert(scope);
        for (EncoderBackend backend :
            {EncoderBackend::Vaapi, EncoderBackend::Nvenc, EncoderBackend::Qsv}) {
            QStringList codecs;
            QString where;
            QStringList why; ///< "codec: reason" of every codec that failed
            bool any = false;
            for (const HwRow& row : kHardware) {
                if (row.backend != backend
                    || (!scope.isEmpty() && scope != QLatin1String(row.codec))) {
                    continue;
                }
                const auto it = c.results.constFind(QString::fromLatin1(row.encoder));
                if (it == c.results.constEnd()) {
                    continue;
                }
                any = true;
                if (it->status.available) {
                    codecs << QString::fromLatin1(row.codec);
                    QStringList details;
                    if (!it->driver.isEmpty()) {
                        details << it->driver;
                    }
                    if (!it->status.device.isEmpty()) {
                        details << it->status.device;
                    }
                    if (!details.isEmpty()) {
                        where = QStringLiteral(" (%1)").arg(details.join(QStringLiteral(", ")));
                    }
                } else {
                    why << QStringLiteral("%1: %2").arg(
                        QString::fromLatin1(row.codec), it->status.error);
                }
            }
            if (!any) {
                continue;
            }
            parts << (codecs.isEmpty()
                    ? QStringLiteral("%1 unavailable (%2)")
                          .arg(QLatin1String(backendLabel(backend)), why.join(QStringLiteral("; ")))
                    : QStringLiteral("%1 %2%3%4")
                          .arg(QLatin1String(backendLabel(backend)), codecs.join(u'/'), where,
                              why.isEmpty() ? QString()
                                            : QStringLiteral(" [failed: %1]")
                                                  .arg(why.join(QStringLiteral("; ")))));
        }
    }
    qCInfo(lcExport).noquote() << (scope.isEmpty()
            ? QStringLiteral("hardware encoders:")
            : QStringLiteral("hardware encoders for %1:").arg(scope))
                               << (parts.isEmpty() ? QStringLiteral("none")
                                                   : parts.join(QStringLiteral(", ")));
}

} // namespace

QList<EncoderChoice> EncoderCatalog::softwareEncoders(const QString& codecId)
{
    initExportFfmpeg();
    QList<EncoderChoice> out;
    for (const QString& name : softwareNames(codecId)) {
        if (avcodec_find_encoder_by_name(name.toLatin1().constData())) {
            out.push_back({name, EncoderBackend::Software});
        }
    }
    return out;
}

QList<EncoderChoice> EncoderCatalog::candidates(const QString& codecId, EncoderBackend preference)
{
    QList<EncoderChoice> out;
    if (preference != EncoderBackend::Software) {
        for (const HwRow& row : kHardware) {
            if (codecId != QLatin1String(row.codec)) {
                continue;
            }
            if (preference != EncoderBackend::Auto && preference != row.backend) {
                continue;
            }
            if (cachedProbe(row).status.available) {
                out.push_back({QString::fromLatin1(row.encoder), row.backend});
            }
        }
        if (!hardwareDisabled()) {
            logSummary(codecId);
        }
    }
    out += softwareEncoders(codecId);
    return out;
}

QList<HwEncoderStatus> EncoderCatalog::hardwareEncoders()
{
    initExportFfmpeg();
    QList<HwEncoderStatus> out;
    for (const HwRow& row : kHardware) {
        out.push_back(cachedProbe(row).status);
    }
    if (!hardwareDisabled()) {
        logSummary(QString());
    }
    return out;
}

HwEncoderStatus EncoderCatalog::hardwareStatus(const QString& encoderName)
{
    const HwRow* row = findRow(encoderName);
    if (!row) {
        HwEncoderStatus status;
        status.encoderName = encoderName;
        status.error = QStringLiteral("not a hardware encoder");
        return status;
    }
    return cachedProbe(*row).status;
}

bool EncoderCatalog::isAvailable(const EncoderChoice& choice)
{
    if (choice.backend == EncoderBackend::Software) {
        initExportFfmpeg();
        return avcodec_find_encoder_by_name(choice.encoderName.toLatin1().constData()) != nullptr;
    }
    return hardwareStatus(choice.encoderName).available;
}

void EncoderCatalog::markFailed(const QString& encoderName, const QString& reason)
{
    const HwRow* row = findRow(encoderName);
    if (!row) {
        return;
    }
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    ProbeResult& r = c.results[encoderName];
    r.status.encoderName = encoderName;
    r.status.available = false;
    r.status.probeFailed = true;
    r.status.error = QStringLiteral("failed while rendering: %1").arg(reason);
    r.device = nullptr;
}

QList<HwEncoderStatus> EncoderCatalog::cachedEncoders()
{
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    loadDiskCache(c);
    QList<HwEncoderStatus> out;
    for (const HwRow& row : kHardware) {
        if (const auto it = c.results.constFind(QString::fromLatin1(row.encoder));
            it != c.results.constEnd()) {
            out.push_back(it->status);
        } else if (hardwareDisabled()) {
            ProbeResult off;
            quickAnswer(row, &off);
            out.push_back(off.status);
        }
    }
    return out.size() == int(std::size(kHardware)) ? out : QList<HwEncoderStatus>();
}

bool EncoderCatalog::probeWasRun()
{
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    return c.probeRuns > 0;
}

void EncoderCatalog::retest()
{
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    c.results.clear();
    c.summarised.clear();
    c.vaapiFault.clear();
    c.probeRuns = 0;
    c.diskLoaded = true; // do not read the old answers back
    QFile::remove(cacheFilePath(c));
}

bool EncoderCatalog::hardwareDisabledByEnvironment()
{
    return hardwareDisabled();
}

QString EncoderCatalog::driverKey()
{
    return computeDriverKey();
}

void EncoderCatalog::setProbeProgram(const QString& path)
{
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    c.probeProgram = path;
}

void EncoderCatalog::setProbeTimeoutMs(int ms)
{
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    c.probeTimeoutMs = ms;
}

void EncoderCatalog::setCacheFile(const QString& path)
{
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    c.cacheFile = path;
    c.diskLoaded = false;
}

void EncoderCatalog::resetMemory()
{
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    c.results.clear();
    c.summarised.clear();
    c.vaapiFault.clear();
    c.probeRuns = 0;
    c.diskLoaded = false;
}

QByteArray EncoderCatalog::probeHere(const QString& encoderName)
{
    initExportFfmpeg();
    const HwRow* row = findRow(encoderName);
    if (!row) {
        return QJsonDocument(
            QJsonObject {{QLatin1String("error"), QStringLiteral("not a hardware encoder")}})
            .toJson(QJsonDocument::Compact);
    }
    Cache local;
    const ProbeResult r = probeLocal(local, *row);
    return QJsonDocument(statusToJson(r)).toJson(QJsonDocument::Compact);
}

AVBufferRef* EncoderCatalog::vaapiDevice(const QString& encoderName, bool* lowPower)
{
    const HwRow* row = findRow(encoderName);
    if (!row || row->backend != EncoderBackend::Vaapi) {
        return nullptr;
    }
    const ProbeResult result = cachedProbe(*row);
    if (!result.status.available || result.status.device.isEmpty()) {
        return nullptr;
    }
    // The test ran in another process: open the device it found here (opening only starts the
    // driver; the encoder test that can crash was done there).
    Cache& c = cache();
    AVBufferRef* device = nullptr;
    {
        std::lock_guard lock(c.mutex);
        QString error;
        device = vaapiDeviceFor(c, result.status.device, &error);
    }
    if (!device) {
        return nullptr;
    }
    if (lowPower) {
        *lowPower = result.lowPower;
    }
    return av_buffer_ref(device);
}

} // namespace kk
