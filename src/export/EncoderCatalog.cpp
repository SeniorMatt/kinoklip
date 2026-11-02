// SPDX-License-Identifier: GPL-3.0-or-later
// EncoderCatalog — encoder tables and hardware encoder detection.
#include "export/EncoderCatalog.h"

#include "export/ExportFFmpeg.h"
#include "export/ExportLog.h"
#include "export/VideoEncoder.h"
#include "media/HwAccel.h"

#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QSet>

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
};

Cache& cache()
{
    static Cache* instance = new Cache; // intentionally leaked, see above
    return *instance;
}

bool hardwareDisabled()
{
    return qEnvironmentVariable("KINOKLIP_HWENC").compare(QLatin1String("off"), Qt::CaseInsensitive)
        == 0;
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
    for (const QString& name :
        dri.entryList({QStringLiteral("renderD*")}, QDir::System, QDir::Name)) {
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

ProbeResult probe(Cache& c, const HwRow& row)
{
    ProbeResult result;
    result.status.codecId = QString::fromLatin1(row.codec);
    result.status.encoderName = QString::fromLatin1(row.encoder);
    result.status.backend = row.backend;
    if (hardwareDisabled()) {
        result.status.error = QStringLiteral("disabled by KINOKLIP_HWENC=off");
        return result;
    }
    if (!avcodec_find_encoder_by_name(row.encoder)) {
        result.status.error = QStringLiteral("not in this FFmpeg build");
        return result;
    }
    // Do not even try what cannot work: FFmpeg would log CUDA_ERROR_NO_DEVICE / "Error
    // creating a MFX session" errors on every machine without the hardware.
    if (row.backend == EncoderBackend::Nvenc && !media::nvidiaDriverPresent()) {
        result.status.error = QStringLiteral("no NVIDIA driver");
        return result;
    }
    if (row.backend == EncoderBackend::Qsv && !media::qsvRuntimePresent()) {
        result.status.error = QStringLiteral("no Intel GPU with a oneVPL/MFX runtime");
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
    }
    qCDebug(lcExport).noquote() << "hardware encoder" << result.status.encoderName
                                << (result.status.available ? QStringLiteral("available")
                                                            : QStringLiteral("unavailable:"))
                                << (result.status.available ? result.status.device
                                                            : result.status.error)
                                << QStringLiteral("(%1 ms)").arg(timer.elapsed());
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
    if (auto it = c.results.constFind(key); it != c.results.constEnd()) {
        return *it;
    }
    ProbeResult result = probe(c, row);
    c.results.insert(key, result);
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
            QString why;
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
                } else if (why.isEmpty()) {
                    why = it->status.error;
                }
            }
            if (!any) {
                continue;
            }
            parts << (codecs.isEmpty()
                    ? QStringLiteral("%1 unavailable (%2)")
                          .arg(QLatin1String(backendLabel(backend)), why)
                    : QStringLiteral("%1 %2%3").arg(
                          QLatin1String(backendLabel(backend)), codecs.join(u'/'), where));
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

AVBufferRef* EncoderCatalog::vaapiDevice(const QString& encoderName, bool* lowPower)
{
    const HwRow* row = findRow(encoderName);
    if (!row || row->backend != EncoderBackend::Vaapi) {
        return nullptr;
    }
    const ProbeResult result = cachedProbe(*row);
    if (!result.device) {
        return nullptr;
    }
    if (lowPower) {
        *lowPower = result.lowPower;
    }
    return av_buffer_ref(result.device);
}

} // namespace kk
