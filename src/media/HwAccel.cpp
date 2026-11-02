// SPDX-License-Identifier: GPL-3.0-or-later
// HwAccel — backend table, device discovery (VAAPI render nodes...), shared device contexts.
#include "media/HwAccel.h"

#include "media/HwDevices.h"
#include "media/MediaLog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#ifdef Q_OS_UNIX
#include <dlfcn.h>
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

// Vulkan headers are optional at build time; without them any Vulkan device is accepted and a
// device without video decode support simply falls back to software per stream.
#if __has_include(<vulkan/vulkan.h>) && __has_include(<libavutil/hwcontext_vulkan.h>)
#define KK_HAVE_VULKAN_HEADERS 1
extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}
#include <cstring>
#endif

#include <algorithm>
#include <atomic>

namespace kk::media {

namespace {

/// One hardware decoding backend. The table below is the ONLY place that knows which
/// backends exist and in which order Auto tries them; add Windows/macOS entries here.
struct Backend
{
    HwAccelMode mode;
    AVHWDeviceType type;
    const char* name;
};

// Auto order: the integrated/display GPU's native API first (most efficient, no copy across
// PCIe, works on the target laptops), then vendor APIs, then Vulkan Video (newest, least
// mature drivers).
// TODO(media): Windows → {D3D11VA, AV_HWDEVICE_TYPE_D3D11VA}; macOS → VideoToolbox.
constexpr Backend kBackends[] = {
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    {HwAccelMode::Vaapi, AV_HWDEVICE_TYPE_VAAPI, "VAAPI"},
#endif
    {HwAccelMode::Cuda, AV_HWDEVICE_TYPE_CUDA, "CUDA"},
    {HwAccelMode::Vulkan, AV_HWDEVICE_TYPE_VULKAN, "Vulkan"},
};

/// Codecs listed in HwDeviceInfo::codecs when FFmpeg has a hwaccel for them.
constexpr AVCodecID kInterestingCodecs[] = {
    AV_CODEC_ID_H264,
    AV_CODEC_ID_HEVC,
    AV_CODEC_ID_VP9,
    AV_CODEC_ID_AV1,
    AV_CODEC_ID_VP8,
    AV_CODEC_ID_MPEG2VIDEO,
    AV_CODEC_ID_VC1,
    AV_CODEC_ID_MJPEG,
};

/// Vulkan physical devices tried, by index (FFmpeg's device string for Vulkan).
constexpr int kMaxVulkanDevices = 4;

/// PCI vendor ids (sysfs) used to rank VAAPI render nodes.
constexpr quint32 kVendorIntel = 0x8086;
constexpr quint32 kVendorAmd = 0x1002;

const Backend* findBackend(HwAccelMode mode)
{
    for (const Backend& backend : kBackends) {
        if (backend.mode == mode) {
            return &backend;
        }
    }
    return nullptr;
}

/// Initial preference: Auto, overridable with KINOKLIP_HWACCEL=off|auto|vaapi|cuda|vulkan
/// (handy to work around a broken driver without touching settings).
HwAccelMode initialPreference()
{
    const QByteArray env = qgetenv("KINOKLIP_HWACCEL").toLower();
    for (HwAccelMode mode : {HwAccelMode::Off, HwAccelMode::Auto, HwAccelMode::Vaapi,
             HwAccelMode::Cuda, HwAccelMode::Vulkan}) {
        if (!env.isEmpty() && env == hwAccelName(mode).toLower().toLatin1()) {
            return mode;
        }
    }
    return HwAccelMode::Auto;
}

std::atomic<HwAccelMode>& preference()
{
    static std::atomic<HwAccelMode> s_preference {initialPreference()};
    return s_preference;
}

/// DRM render nodes in VAAPI preference order: the GPU driving the display first (on laptops
/// the iGPU, which also shares memory with the CPU → cheap downloads), then other Intel/AMD
/// GPUs, then the rest (NVIDIA only works through the third-party nvidia-vaapi-driver).
/// Linux sysfs specific; TODO(platform): move to src/platform when it exists.
QStringList vaapiRenderNodes()
{
    struct Node
    {
        QString path;
        int rank;
    };
    QList<Node> nodes;
    const QDir dri(QStringLiteral("/dev/dri"));
    const QStringList names = dri.entryList({QStringLiteral("renderD*")}, QDir::System, QDir::Name);
    for (const QString& name : names) {
        const QString sys = QStringLiteral("/sys/class/drm/%1/device/").arg(name);
        auto readSys = [&](const char* file) {
            QFile f(sys + QLatin1String(file));
            return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed() : QByteArray();
        };
        bool ok = false;
        const quint32 vendor = readSys("vendor").toUInt(&ok, 16);
        const bool bootVga = readSys("boot_vga") == "1";
        const int rank = bootVga ? 0 : (vendor == kVendorIntel || vendor == kVendorAmd) ? 1 : 2;
        nodes.append({dri.filePath(name), rank});
    }
    std::stable_sort(
        nodes.begin(), nodes.end(), [](const Node& a, const Node& b) { return a.rank < b.rank; });
    QStringList paths;
    for (const Node& node : nodes) {
        paths.append(node.path);
    }
    return paths;
}

/// Device strings to try for a backend, best first.
QStringList candidateDevices(HwAccelMode mode)
{
    if (mode == HwAccelMode::Vaapi) {
        return vaapiRenderNodes();
    }
    if (mode == HwAccelMode::Vulkan) {
        // Hybrid laptops expose several GPUs; often only one has Vulkan Video (e.g. an older
        // iGPU without it next to an NVIDIA dGPU), so try them all.
        QStringList indices;
        for (int i = 0; i < kMaxVulkanDevices; ++i) {
            indices.append(QString::number(i));
        }
        return indices;
    }
    return {QString()}; // the backend's default device
}

/// False if a created device cannot decode at all (Vulkan device without video queue).
bool deviceCanDecode(HwAccelMode mode, AVBufferRef* device)
{
#ifdef KK_HAVE_VULKAN_HEADERS
    if (mode == HwAccelMode::Vulkan) {
        const auto* ctx = reinterpret_cast<const AVHWDeviceContext*>(device->data);
        const auto* vk = static_cast<const AVVulkanDeviceContext*>(ctx->hwctx);
        for (int i = 0; i < vk->nb_enabled_dev_extensions; ++i) {
            if (std::strcmp(vk->enabled_dev_extensions[i], "VK_KHR_video_decode_queue") == 0) {
                return true;
            }
        }
        return false;
    }
#endif
    Q_UNUSED(mode)
    Q_UNUSED(device)
    return true;
}

/// Serialisation policy from KINOKLIP_HW_SERIALIZE (default: VAAPI only).
detail::HwSerialise initialSerialise()
{
    const QByteArray env = qgetenv("KINOKLIP_HW_SERIALIZE").toLower();
    if (env == "off" || env == "0" || env == "none") {
        return detail::HwSerialise::Off;
    }
    if (env == "all") {
        return detail::HwSerialise::All;
    }
    return detail::HwSerialise::Vaapi;
}

std::atomic<int> s_injectedDownloadFailures {0};

/// Looks up a libva function in the already loaded libva (libavutil links it), without a
/// build-time dependency on the libva headers.
void* libvaSymbol(const char* name)
{
#ifdef Q_OS_UNIX
    return dlsym(RTLD_DEFAULT, name);
#else
    Q_UNUSED(name)
    return nullptr;
#endif
}

/// "Intel iHD driver for Intel(R) Gen Graphics - 24.1.0 ()" -> "iHD 24.1.0";
/// "Mesa Gallium driver 24.2.1 for AMD Radeon ..." -> "Mesa Gallium 24.2.1 (AMD Radeon ...)".
QString shortDriverName(const QString& vendor)
{
    static const QRegularExpression intel(QStringLiteral(R"((iHD|i965)\b.*?-\s*([0-9][0-9.]*))"));
    if (const auto m = intel.match(vendor); m.hasMatch()) {
        return m.captured(1) + u' ' + m.captured(2);
    }
    static const QRegularExpression mesa(
        QStringLiteral(R"(Mesa Gallium driver ([0-9][^ ]*)(?: for (.*))?)"));
    if (const auto m = mesa.match(vendor); m.hasMatch()) {
        return QStringLiteral("Mesa Gallium %1").arg(m.captured(1))
            + (m.captured(2).isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m.captured(2)));
    }
    return vendor.trimmed();
}

/// FFmpeg's get_format callback: pick the hardware surface format if offered, otherwise
/// (profile unsupported by the GPU, hwaccel init failed) the first software format.
AVPixelFormat selectHwFormat(AVCodecContext* ctx, const AVPixelFormat* formats)
{
    auto* state = static_cast<detail::HwDecodeState*>(ctx->opaque);
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == state->hwFormat) {
            return *f;
        }
    }
    state->rejected = true;
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*f);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            return *f;
        }
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

// ---- Public API -----------------------------------------------------------------------------

QString hwAccelName(HwAccelMode mode)
{
    switch (mode) {
    case HwAccelMode::Auto:
        return QStringLiteral("Auto");
    case HwAccelMode::Off:
        return QStringLiteral("Off");
    case HwAccelMode::Cuda:
        return QStringLiteral("CUDA");
    case HwAccelMode::Vaapi:
        return QStringLiteral("VAAPI");
    case HwAccelMode::Vulkan:
        return QStringLiteral("Vulkan");
    }
    return {};
}

void setPreferredHwAccel(HwAccelMode mode)
{
    preference().store(mode);
}

HwAccelMode preferredHwAccel()
{
    return preference().load();
}

QList<HwDeviceInfo> hwCapabilities()
{
    QList<HwDeviceInfo> result;
    for (const Backend& backend : kBackends) {
        result.append(detail::HwDeviceManager::instance().info(backend.mode));
    }
    return result;
}

bool nvidiaDriverPresent()
{
#ifdef Q_OS_LINUX
    // The control node, not /proc/driver/nvidia: containers see the host's /proc but get the
    // device nodes only when the GPU is passed through.
    return QFile::exists(QStringLiteral("/dev/nvidiactl"));
#else
    return true;
#endif
}

bool qsvRuntimePresent()
{
#ifdef Q_OS_LINUX
    static const bool present = [] {
        bool intelGpu = false;
        const QDir dri(QStringLiteral("/dev/dri"));
        for (const QString& name : dri.entryList({QStringLiteral("renderD*")}, QDir::System)) {
            QFile vendor(QStringLiteral("/sys/class/drm/%1/device/vendor").arg(name));
            if (vendor.open(QIODevice::ReadOnly)) {
                bool ok = false;
                intelGpu = intelGpu || vendor.readAll().trimmed().toUInt(&ok, 16) == kVendorIntel;
            }
        }
        if (!intelGpu) {
            return false;
        }
        if (qEnvironmentVariableIsSet("ONEVPL_SEARCH_PATH")) {
            return true; // the dispatcher looks there too: let it try
        }
        // The oneVPL GPU runtime (Gen12+, e.g. Iris Xe) or the legacy Media SDK runtime.
        for (const char* lib : {"libmfx-gen.so.1.2", "libmfxhw64.so.1"}) {
            if (void* handle = dlopen(lib, RTLD_LAZY | RTLD_LOCAL)) {
                dlclose(handle);
                return true;
            }
        }
        return false;
    }();
    return present;
#else
    return true;
#endif
}

QString vaapiDriverName(AVBufferRef* device)
{
    if (!device) {
        return {};
    }
    const auto* ctx = reinterpret_cast<const AVHWDeviceContext*>(device->data);
    if (ctx->type != AV_HWDEVICE_TYPE_VAAPI || !ctx->hwctx) {
        return {};
    }
    // AVVAAPIDeviceContext starts with `VADisplay display` (public, stable FFmpeg ABI).
    void* display = *static_cast<void* const*>(ctx->hwctx);
    using VendorFn = const char* (*)(void*);
    auto vendor = reinterpret_cast<VendorFn>(libvaSymbol("vaQueryVendorString"));
    const char* text = display && vendor ? vendor(display) : nullptr;
    return text ? shortDriverName(QString::fromUtf8(text)) : QString();
}

QString libvaDescription()
{
#ifdef Q_OS_UNIX
    void* symbol = libvaSymbol("vaInitialize");
    Dl_info dl {};
    if (!symbol || !dladdr(symbol, &dl) || !dl.dli_fname) {
        return {};
    }
    const QString path = QFileInfo(QString::fromLocal8Bit(dl.dli_fname)).canonicalFilePath();
    // libva.so.2.<minor * 100 + micro>.0 (libtool versioning) -> 2.<minor>.<micro>
    QString version = QStringLiteral("?");
    static const QRegularExpression re(QStringLiteral(R"(libva\.so\.(\d+)\.(\d+)\.)"));
    if (const auto m = re.match(path); m.hasMatch()) {
        const int lt = m.captured(2).toInt();
        version = QStringLiteral("%1.%2.%3").arg(m.captured(1)).arg(lt / 100).arg(lt % 100);
    }
    const QString appDir = qEnvironmentVariable("KINOKLIP_APPDIR");
    const bool bundled
        = !appDir.isEmpty() && path.startsWith(QFileInfo(appDir).canonicalFilePath() + u'/');
    return QStringLiteral("libva %1 (%2, %3)")
        .arg(version, bundled ? QStringLiteral("bundled") : QStringLiteral("host"), path);
#else
    return {};
#endif
}

// ---- Internal helpers -----------------------------------------------------------------------

namespace detail {

QList<HwAccelMode> hwCandidates(HwAccelMode mode)
{
    QList<HwAccelMode> modes;
    if (mode == HwAccelMode::Auto) {
        for (const Backend& backend : kBackends) {
            modes.append(backend.mode);
        }
    } else if (mode != HwAccelMode::Off && findBackend(mode)) {
        modes.append(mode);
    }
    return modes;
}

AVPixelFormat hwPixelFormat(const AVCodec* codec, HwAccelMode mode)
{
    const Backend* backend = findBackend(mode);
    if (!codec || !backend) {
        return AV_PIX_FMT_NONE;
    }
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            return AV_PIX_FMT_NONE;
        }
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
            && config->device_type == backend->type) {
            return config->pix_fmt;
        }
    }
}

void attachHwDevice(AVCodecContext* ctx, AVBufferRef* device, HwDecodeState* state)
{
    // Surfaces we keep referenced (current + look-ahead frame) on top of the decoder's DPB;
    // VAAPI surface pools are fixed-size, so without these decoding would stall.
    constexpr int kExtraHwFrames = 4;
    ctx->hw_device_ctx = av_buffer_ref(device);
    ctx->get_format = selectHwFormat;
    ctx->opaque = state;
    ctx->extra_hw_frames = kExtraHwFrames;
}

HwDeviceManager::HwDeviceManager()
    : m_serialise(initialSerialise())
{ }

HwDeviceManager& HwDeviceManager::instance()
{
    static auto* s_instance = new HwDeviceManager; // intentionally never destroyed
    return *s_instance;
}

AVBufferRef* HwDeviceManager::acquire(HwAccelMode mode)
{
    const std::lock_guard lock(m_mutex);
    Entry& entry = ensure(mode);
    return entry.device && !entry.disabled ? av_buffer_ref(entry.device) : nullptr;
}

QString HwDeviceManager::describe(HwAccelMode mode)
{
    const std::lock_guard lock(m_mutex);
    const HwDeviceInfo& info = ensure(mode).info;
    QStringList parts;
    if (!info.driver.isEmpty()) {
        parts << info.driver;
    }
    if (!info.device.isEmpty()) {
        parts << info.device;
    }
    return parts.isEmpty()
        ? info.name
        : QStringLiteral("%1 (%2)").arg(info.name, parts.join(QStringLiteral(", ")));
}

std::mutex* HwDeviceManager::callLock(HwAccelMode mode)
{
    const std::lock_guard lock(m_mutex);
    const bool serialise = m_serialise == HwSerialise::All
        || (m_serialise == HwSerialise::Vaapi && mode == HwAccelMode::Vaapi);
    return serialise && mode != HwAccelMode::Off && mode != HwAccelMode::Auto
        ? &m_entries[int(mode)].callMutex
        : nullptr;
}

bool HwDeviceManager::reportFailure(HwAccelMode mode, const QString& reason)
{
    const std::lock_guard lock(m_mutex);
    Entry& entry = m_entries[int(mode)];
    ++entry.failures;
    if (!entry.disabled && entry.failures >= kMaxDecoderFailures) {
        entry.disabled = true;
        qCWarning(lcMedia).noquote()
            << QStringLiteral("hardware decoding %1 disabled for this session after %2 failures "
                              "(last: %3); new decoders use software")
                   .arg(entry.info.name)
                   .arg(entry.failures)
                   .arg(reason);
    }
    return entry.disabled;
}

int HwDeviceManager::failureCount(HwAccelMode mode)
{
    const std::lock_guard lock(m_mutex);
    return m_entries[int(mode)].failures;
}

void HwDeviceManager::resetFailures()
{
    const std::lock_guard lock(m_mutex);
    for (Entry& entry : m_entries) {
        entry.failures = 0;
        entry.disabled = false;
    }
}

void HwDeviceManager::setSerialise(HwSerialise policy)
{
    const std::lock_guard lock(m_mutex);
    m_serialise = policy;
}

void injectDownloadFailures(int count)
{
    s_injectedDownloadFailures.store(count);
}

bool takeInjectedDownloadFailure()
{
    int left = s_injectedDownloadFailures.load();
    while (left > 0) {
        if (s_injectedDownloadFailures.compare_exchange_weak(left, left - 1)) {
            return true;
        }
    }
    return false;
}

HwDeviceInfo HwDeviceManager::info(HwAccelMode mode)
{
    const std::lock_guard lock(m_mutex);
    return ensure(mode).info;
}

HwDeviceManager::Entry& HwDeviceManager::ensure(HwAccelMode mode)
{
    Entry& entry = m_entries[int(mode)];
    if (entry.tried) {
        return entry;
    }
    entry.tried = true;
    initFfmpeg();
    const Backend* backend = findBackend(mode);
    entry.info.mode = mode;
    entry.info.name = hwAccelName(mode);
    if (!backend) {
        entry.info.error = QStringLiteral("Not supported on this platform");
        return entry;
    }
    QStringList devices = candidateDevices(mode);
    if (devices.isEmpty()) {
        entry.info.error = QStringLiteral("No device found");
    }
    if (mode == HwAccelMode::Cuda && !nvidiaDriverPresent()) {
        devices.clear(); // creating a CUDA device would only log CUDA_ERROR_NO_DEVICE
        entry.info.error = QStringLiteral("no NVIDIA driver");
    }
    for (const QString& device : devices) {
        AVBufferRef* ref = nullptr;
        const QByteArray deviceName = device.toUtf8();
        // Device creation failures are expected (no such GPU, no permission): keep FFmpeg's
        // messages out of the log and report them in our one line below.
        FfmpegLogCapture capture;
        const int ret = av_hwdevice_ctx_create(
            &ref, backend->type, device.isEmpty() ? nullptr : deviceName.constData(), nullptr, 0);
        if (ret >= 0 && !deviceCanDecode(mode, ref)) {
            av_buffer_unref(&ref);
            entry.info.error = QStringLiteral("device %1 has no video decode support").arg(device);
            continue;
        }
        if (ret >= 0) {
            entry.device = ref;
            entry.info.available = true;
            entry.info.device = device;
            entry.info.error.clear();
            if (mode == HwAccelMode::Vaapi) {
                entry.info.driver = vaapiDriverName(ref);
            }
            break;
        }
        QString reason = avErrorString(ret);
        if (!capture.firstError().isEmpty()) {
            reason += QStringLiteral(": ") + capture.firstError();
        }
        entry.info.error = device.isEmpty() ? reason : QStringLiteral("%1: %2").arg(device, reason);
        if (mode == HwAccelMode::Vulkan) {
            // Physical devices are numbered densely: if index i cannot be created, no higher
            // index exists either (and without any Vulkan driver every attempt would create and
            // destroy a whole VkInstance and log "No devices found"). Only a device that was
            // created but lacks video decode moves on to the next index (hybrid laptops).
            break;
        }
    }
    for (AVCodecID id : kInterestingCodecs) {
        if (hwPixelFormat(avcodec_find_decoder(id), mode) != AV_PIX_FMT_NONE) {
            entry.info.codecs.append(QString::fromUtf8(avcodec_get_name(id)));
        }
    }
    if (entry.info.available) {
        QString details
            = entry.info.device.isEmpty() ? QStringLiteral("default device") : entry.info.device;
        if (mode == HwAccelMode::Vaapi) {
            // Bundled libva + host driver is the usual suspect for VAAPI trouble: say which
            // libva and which driver are really in use (and any override the user set).
            details
                += QStringLiteral(", driver %1, %2")
                       .arg(entry.info.driver.isEmpty() ? QStringLiteral("?") : entry.info.driver,
                           libvaDescription());
            for (const char* var : {"LIBVA_DRIVER_NAME", "LIBVA_DRIVERS_PATH"}) {
                if (qEnvironmentVariableIsSet(var)) {
                    details += QStringLiteral(", %1=%2").arg(
                        QLatin1String(var), qEnvironmentVariable(var));
                }
            }
        }
        qCInfo(lcMedia).noquote() << "hardware decoding" << entry.info.name
                                  << "available:" << details;
    } else {
        qCInfo(lcMedia).noquote() << "hardware decoding" << entry.info.name
                                  << "unavailable:" << entry.info.error;
    }
    return entry;
}

} // namespace detail

} // namespace kk::media
