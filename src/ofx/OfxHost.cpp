// SPDX-License-Identifier: GPL-3.0-or-later
// OfxHost — see OfxHost.h. The OpenFX host: property sets, the suites a plugin fetches, the
// description of plugins and the render of one instance on the CPU.
//
// Conventions of the host (what a plugin sees): image coordinates have their origin at the
// bottom left; pixels are premultiplied RGBA, float (or 8 bit for a plugin that has no float);
// one clip "Source" in, one "Output"; time is the clip-local frame; the host has no overlays,
// no OpenGL rendering, no tiles, no keyframe editing by the plugin.
#include "ofx/OfxHost.h"

#include "ofxCore.h"
#include "ofxGPURender.h"
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProgress.h"
#include "ofxProperty.h"
#include "ofxTimeLine.h"

#include <QLoggingCategory>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

Q_LOGGING_CATEGORY(lcOfx, "kk.ofx")

// ---- handle types (opaque in the OFX headers) ----

struct Prop
{
    enum Type { None, Str, Int, Dbl, Ptr } type = None;
    std::vector<std::string> s;
    std::vector<int> i;
    std::vector<double> d;
    std::vector<void*> p;
    [[nodiscard]] int size() const
    {
        switch (type) {
        case Str: return int(s.size());
        case Int: return int(i.size());
        case Dbl: return int(d.size());
        case Ptr: return int(p.size());
        default: return 0;
        }
    }
};

struct OfxPropertySetStruct
{
    std::map<std::string, Prop> m;

    void setS(const char* k, const std::string& v, int idx = 0)
    {
        Prop& p = slot(k, Prop::Str);
        if (int(p.s.size()) <= idx)
            p.s.resize(size_t(idx) + 1);
        p.s[size_t(idx)] = v;
    }
    void setI(const char* k, int v, int idx = 0)
    {
        Prop& p = slot(k, Prop::Int);
        if (int(p.i.size()) <= idx)
            p.i.resize(size_t(idx) + 1);
        p.i[size_t(idx)] = v;
    }
    void setD(const char* k, double v, int idx = 0)
    {
        Prop& p = slot(k, Prop::Dbl);
        if (int(p.d.size()) <= idx)
            p.d.resize(size_t(idx) + 1);
        p.d[size_t(idx)] = v;
    }
    void setP(const char* k, void* v, int idx = 0)
    {
        Prop& p = slot(k, Prop::Ptr);
        if (int(p.p.size()) <= idx)
            p.p.resize(size_t(idx) + 1);
        p.p[size_t(idx)] = v;
    }
    [[nodiscard]] const Prop* find(const char* k) const
    {
        const auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::string getS(const char* k, const std::string& def = {}, int idx = 0) const
    {
        const Prop* p = find(k);
        return p && p->type == Prop::Str && idx < int(p->s.size()) ? p->s[size_t(idx)] : def;
    }
    [[nodiscard]] int getI(const char* k, int def = 0, int idx = 0) const
    {
        const Prop* p = find(k);
        return p && p->type == Prop::Int && idx < int(p->i.size()) ? p->i[size_t(idx)] : def;
    }
    [[nodiscard]] double getD(const char* k, double def = 0, int idx = 0) const
    {
        const Prop* p = find(k);
        if (p && p->type == Prop::Dbl && idx < int(p->d.size()))
            return p->d[size_t(idx)];
        if (p && p->type == Prop::Int && idx < int(p->i.size()))
            return p->i[size_t(idx)];
        return def;
    }
    [[nodiscard]] int count(const char* k) const
    {
        const Prop* p = find(k);
        return p ? p->size() : 0;
    }

private:
    Prop& slot(const char* k, Prop::Type t)
    {
        Prop& p = m[k];
        if (p.type != t) {
            p = Prop();
            p.type = t;
        }
        return p;
    }
};

namespace kk::ofx {
struct ImplAccess;
}

struct OfxParamStruct
{
    std::string name;
    std::string type; ///< "OfxParamTypeDouble", ...
    OfxPropertySetStruct props;
    kk::ofx::ParamValue value;
    std::string cache; ///< storage for a string handed to the plugin
    struct OfxParamSetStruct* set = nullptr;
};

struct OfxParamSetStruct
{
    std::vector<std::unique_ptr<OfxParamStruct>> list;
    std::map<std::string, OfxParamStruct*> byName;
    struct OfxImageEffectStruct* effect = nullptr;
};

struct OfxImageClipStruct
{
    std::string name;
    OfxPropertySetStruct props;
    struct OfxImageEffectStruct* effect = nullptr;
};

struct OfxImageEffectStruct
{
    OfxPropertySetStruct props;
    OfxParamSetStruct params;
    std::vector<std::unique_ptr<OfxImageClipStruct>> clips;
    kk::ofx::Instance* instance = nullptr; ///< null for a descriptor
    void* implPtr = nullptr; ///< Instance::Impl of an instance
    OfxImageEffectStruct() { params.effect = this; }
};

struct OfxImageMemoryStruct
{
    void* data = nullptr;
    size_t size = 0;
};

struct OfxMutex
{
    std::recursive_mutex m;
};

namespace kk::ofx {

namespace {

// ---- helpers ----

QString qs(const std::string& s)
{
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}

std::string ss(const QString& s)
{
    return s.toStdString();
}

QString stripPrefix(const std::string& value, const char* prefix)
{
    const QString v = qs(value);
    return v.startsWith(QLatin1String(prefix)) ? v.mid(int(std::strlen(prefix))) : v;
}

// ---- messages ----

std::mutex g_messageMutex;
std::map<void*, QString> g_messages; ///< per effect handle: the last message

void rememberMessage(void* handle, const QString& text)
{
    std::lock_guard<std::mutex> lock(g_messageMutex);
    g_messages[handle] = text;
}

OfxStatus messageV(void* handle, const char* type, const char* format, va_list args)
{
    char buffer[2048];
    std::vsnprintf(buffer, sizeof buffer, format ? format : "", args);
    const QString text = QString::fromUtf8(buffer);
    const QString t = QString::fromUtf8(type ? type : "");
    if (t == QLatin1String(kOfxMessageLog))
        qCDebug(lcOfx).noquote() << "plugin:" << text;
    else if (t == QLatin1String(kOfxMessageError) || t == QLatin1String(kOfxMessageFatal))
        qCWarning(lcOfx).noquote() << "plugin error:" << text;
    else
        qCInfo(lcOfx).noquote() << "plugin:" << text;
    if (t != QLatin1String(kOfxMessageLog))
        rememberMessage(handle, text);
    return t == QLatin1String(kOfxMessageQuestion) ? kOfxStatReplyYes : kOfxStatOK;
}

OfxStatus sMessage(void* handle, const char* type, const char*, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const OfxStatus st = messageV(handle, type, format, args);
    va_end(args);
    return st;
}

OfxStatus sSetPersistentMessage(void* handle, const char* type, const char*, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const OfxStatus st = messageV(handle, type, format, args);
    va_end(args);
    return st;
}

OfxStatus sClearPersistentMessage(void* handle)
{
    rememberMessage(handle, {});
    return kOfxStatOK;
}

const OfxMessageSuiteV2 kMessageSuite = {sMessage, sSetPersistentMessage, sClearPersistentMessage};

// ---- property suite ----

/// KK_OFX_TRACE=1 prints what a plugin asked for and did not get (for plugin authors and bug reports).
bool traceOn()
{
    static const bool on = qEnvironmentVariableIsSet("KK_OFX_TRACE");
    return on;
}

OfxStatus propSet(OfxPropertySetHandle h, const char* name, Prop::Type t, int index,
    const std::function<void(OfxPropertySetStruct&)>& set)
{
    if (!h || !name)
        return kOfxStatErrBadHandle;
    if (index < 0)
        return kOfxStatErrBadIndex;
    const Prop* p = h->find(name);
    if (p && p->type != Prop::None && p->type != t) {
        if (traceOn())
            std::fprintf(stderr, "ofx: property %s set with the wrong type\n", name);
        return kOfxStatErrValue;
    }
    set(*h);
    return kOfxStatOK;
}

OfxStatus pSetPointer(OfxPropertySetHandle h, const char* n, int i, void* v)
{
    return propSet(h, n, Prop::Ptr, i, [&](OfxPropertySetStruct& s) { s.setP(n, v, i); });
}
OfxStatus pSetString(OfxPropertySetHandle h, const char* n, int i, const char* v)
{
    return propSet(h, n, Prop::Str, i, [&](OfxPropertySetStruct& s) { s.setS(n, v ? v : "", i); });
}
OfxStatus pSetDouble(OfxPropertySetHandle h, const char* n, int i, double v)
{
    return propSet(h, n, Prop::Dbl, i, [&](OfxPropertySetStruct& s) { s.setD(n, v, i); });
}
OfxStatus pSetInt(OfxPropertySetHandle h, const char* n, int i, int v)
{
    return propSet(h, n, Prop::Int, i, [&](OfxPropertySetStruct& s) { s.setI(n, v, i); });
}
OfxStatus pSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v)
{
    for (int k = 0; k < c; ++k)
        if (OfxStatus st = pSetPointer(h, n, k, v[k]))
            return st;
    return kOfxStatOK;
}
OfxStatus pSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v)
{
    for (int k = 0; k < c; ++k)
        if (OfxStatus st = pSetString(h, n, k, v[k]))
            return st;
    return kOfxStatOK;
}
OfxStatus pSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v)
{
    for (int k = 0; k < c; ++k)
        if (OfxStatus st = pSetDouble(h, n, k, v[k]))
            return st;
    return kOfxStatOK;
}
OfxStatus pSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v)
{
    for (int k = 0; k < c; ++k)
        if (OfxStatus st = pSetInt(h, n, k, v[k]))
            return st;
    return kOfxStatOK;
}

const Prop* getPropQuiet(OfxPropertySetHandle h, const char* n, int i, Prop::Type t, OfxStatus* st);

const Prop* getProp(OfxPropertySetHandle h, const char* n, int i, Prop::Type t, OfxStatus* st)
{
    const Prop* p = getPropQuiet(h, n, i, t, st);
    if (!p && traceOn())
        std::fprintf(stderr, "ofx: property %s[%d] not given (status %d)\n", n ? n : "?", i, int(*st));
    return p;
}

const Prop* getPropQuiet(OfxPropertySetHandle h, const char* n, int i, Prop::Type t, OfxStatus* st)
{
    if (!h || !n) {
        *st = kOfxStatErrBadHandle;
        return nullptr;
    }
    const Prop* p = h->find(n);
    if (!p || p->type == Prop::None) {
        *st = kOfxStatErrUnknown;
        return nullptr;
    }
    if (p->type != t && !(t == Prop::Dbl && p->type == Prop::Int)) {
        *st = kOfxStatErrValue;
        return nullptr;
    }
    if (i < 0 || i >= p->size()) {
        *st = kOfxStatErrBadIndex;
        return nullptr;
    }
    *st = kOfxStatOK;
    return p;
}

OfxStatus pGetPointer(OfxPropertySetHandle h, const char* n, int i, void** v)
{
    OfxStatus st;
    const Prop* p = getProp(h, n, i, Prop::Ptr, &st);
    if (p)
        *v = p->p[size_t(i)];
    return st;
}
OfxStatus pGetString(OfxPropertySetHandle h, const char* n, int i, char** v)
{
    OfxStatus st;
    const Prop* p = getProp(h, n, i, Prop::Str, &st);
    if (p)
        *v = const_cast<char*>(p->s[size_t(i)].c_str());
    return st;
}
OfxStatus pGetDouble(OfxPropertySetHandle h, const char* n, int i, double* v)
{
    OfxStatus st;
    const Prop* p = getProp(h, n, i, Prop::Dbl, &st);
    if (p)
        *v = p->type == Prop::Int ? p->i[size_t(i)] : p->d[size_t(i)];
    return st;
}
OfxStatus pGetInt(OfxPropertySetHandle h, const char* n, int i, int* v)
{
    OfxStatus st;
    const Prop* p = getProp(h, n, i, Prop::Int, &st);
    if (p)
        *v = p->i[size_t(i)];
    return st;
}
OfxStatus pGetPointerN(OfxPropertySetHandle h, const char* n, int c, void** v)
{
    for (int k = 0; k < c; ++k)
        if (OfxStatus st = pGetPointer(h, n, k, v + k))
            return st;
    return kOfxStatOK;
}
OfxStatus pGetStringN(OfxPropertySetHandle h, const char* n, int c, char** v)
{
    for (int k = 0; k < c; ++k)
        if (OfxStatus st = pGetString(h, n, k, v + k))
            return st;
    return kOfxStatOK;
}
OfxStatus pGetDoubleN(OfxPropertySetHandle h, const char* n, int c, double* v)
{
    for (int k = 0; k < c; ++k)
        if (OfxStatus st = pGetDouble(h, n, k, v + k))
            return st;
    return kOfxStatOK;
}
OfxStatus pGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v)
{
    for (int k = 0; k < c; ++k)
        if (OfxStatus st = pGetInt(h, n, k, v + k))
            return st;
    return kOfxStatOK;
}
OfxStatus pReset(OfxPropertySetHandle h, const char* n)
{
    if (!h || !n)
        return kOfxStatErrBadHandle;
    h->m.erase(n);
    return kOfxStatOK;
}
OfxStatus pGetDimension(OfxPropertySetHandle h, const char* n, int* count)
{
    if (!h || !n)
        return kOfxStatErrBadHandle;
    const Prop* p = h->find(n);
    // A property that has no value yet has dimension 0 (plugins ask before they set, e.g. the
    // supported contexts of their own descriptor).
    *count = p ? p->size() : 0;
    return kOfxStatOK;
}

const OfxPropertySuiteV1 kPropertySuite = {pSetPointer, pSetString, pSetDouble, pSetInt,
    pSetPointerN, pSetStringN, pSetDoubleN, pSetIntN, pGetPointer, pGetString, pGetDouble, pGetInt,
    pGetPointerN, pGetStringN, pGetDoubleN, pGetIntN, pReset, pGetDimension};

// ---- memory and threads ----

OfxStatus mAlloc(void*, size_t n, void** out)
{
    *out = std::malloc(n ? n : 1);
    return *out ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus mFree(void* p)
{
    std::free(p);
    return kOfxStatOK;
}
const OfxMemorySuiteV1 kMemorySuite = {mAlloc, mFree};

thread_local unsigned t_threadIndex = 0;
thread_local bool t_spawned = false;

OfxStatus mtRun(OfxThreadFunctionV1 func, unsigned int n, void* arg)
{
    if (!func || n == 0)
        return kOfxStatFailed;
    if (n == 1) {
        func(0, 1, arg);
        return kOfxStatOK;
    }
    std::vector<std::thread> threads;
    threads.reserve(n - 1);
    for (unsigned k = 1; k < n; ++k) {
        threads.emplace_back([=] {
            t_threadIndex = k;
            t_spawned = true;
            func(k, n, arg);
        });
    }
    const unsigned oldIndex = t_threadIndex;
    t_threadIndex = 0;
    func(0, n, arg);
    t_threadIndex = oldIndex;
    for (std::thread& t : threads)
        t.join();
    return kOfxStatOK;
}
OfxStatus mtNumCPUs(unsigned int* n)
{
    *n = unsigned(hostThreadCount());
    return kOfxStatOK;
}
OfxStatus mtIndex(unsigned int* i)
{
    *i = t_threadIndex;
    return t_spawned || t_threadIndex == 0 ? kOfxStatOK : kOfxStatFailed;
}
int mtIsSpawned()
{
    return t_spawned ? 1 : 0;
}
OfxStatus mtCreate(OfxMutexHandle* m, int lockCount)
{
    auto* mutex = new OfxMutex;
    for (int k = 0; k < lockCount; ++k)
        mutex->m.lock();
    *m = mutex;
    return kOfxStatOK;
}
OfxStatus mtDestroy(const OfxMutexHandle m)
{
    delete m;
    return kOfxStatOK;
}
OfxStatus mtLock(const OfxMutexHandle m)
{
    m->m.lock();
    return kOfxStatOK;
}
OfxStatus mtUnlock(const OfxMutexHandle m)
{
    m->m.unlock();
    return kOfxStatOK;
}
OfxStatus mtTryLock(const OfxMutexHandle m)
{
    return m->m.try_lock() ? kOfxStatOK : kOfxStatFailed;
}
const OfxMultiThreadSuiteV1 kMultiThreadSuite = {
    mtRun, mtNumCPUs, mtIndex, mtIsSpawned, mtCreate, mtDestroy, mtLock, mtUnlock, mtTryLock};

OfxStatus prStart(void*, const char*)
{
    return kOfxStatOK;
}
OfxStatus prUpdate(void*, double)
{
    return kOfxStatOK;
}
OfxStatus prEnd(void*)
{
    return kOfxStatOK;
}
const OfxProgressSuiteV1 kProgressSuite = {prStart, prUpdate, prEnd};

OfxStatus tlGetTime(void*, double* t)
{
    *t = 0;
    return kOfxStatOK;
}
OfxStatus tlGoto(void*, double)
{
    return kOfxStatOK;
}
OfxStatus tlBounds(void*, double* a, double* b)
{
    *a = 0;
    *b = 1.0e6;
    return kOfxStatOK;
}
const OfxTimeLineSuiteV1 kTimeLineSuite = {tlGetTime, tlGoto, tlBounds};

} // namespace

int hostThreadCount()
{
    return std::max(1, int(std::thread::hardware_concurrency()));
}

// ---- the instance's private state (needed by the suites) ----

struct Instance::Impl
{
    PluginInfo info;
    const ContextInfo* ctx = nullptr;
    OfxPlugin* plugin = nullptr;
    OfxImageEffectStruct effect; ///< the instance handle
    Evaluator evaluator;
    std::mutex renderMutex;
    std::mutex stateMutex; ///< guards images and the current render
    std::set<OfxPropertySetStruct*> images;
    std::set<OfxImageMemoryStruct*> memories;
    std::string chosenDepth = kOfxBitDepthFloat;

    // the render in progress
    struct Current
    {
        RenderArgs args;
        const float* srcFloat = nullptr;
        const float* srcToFloat = nullptr; ///< transition context: the incoming picture
        float* dstFloat = nullptr;
        std::vector<unsigned char> srcByte, srcToByte, dstByte;
        bool byte = false;
    };
    Current* current = nullptr;
};

namespace {

// ---- loaded binaries and described plugins ----

struct Described
{
    PluginInfo info;
    /// The context descriptors (kept: the instance copies parameters and clips from them).
    std::vector<std::unique_ptr<OfxImageEffectStruct>> contexts;
    std::vector<int> contextIndex; ///< parallel to info.contexts (-1: no descriptor)
};

struct Binary
{
    void* dl = nullptr;
    QString path;
    std::vector<OfxPlugin*> plugins;
    std::vector<bool> loadedOk;
    std::vector<std::unique_ptr<Described>> described;
    std::mutex mutex;
    QString error;
};

OfxPropertySetStruct g_hostProps;
OfxHost g_host;
std::once_flag g_hostOnce;
std::mutex g_binariesMutex;
std::map<QString, std::unique_ptr<Binary>> g_binaries;
std::mutex g_pluginLockMutex;
std::map<std::string, std::unique_ptr<std::mutex>> g_pluginLocks; ///< for Unsafe plugins

const void* fetchSuite(OfxPropertySetHandle, const char* name, int version);
const void* fetchSuiteImpl(const char* name, int version);

void initHost()
{
    std::call_once(g_hostOnce, [] {
        OfxPropertySetStruct& h = g_hostProps;
        h.setS(kOfxPropName, "Kinoklip");
        h.setS(kOfxPropLabel, "Kinoklip");
        h.setI(kOfxPropVersion, 1, 0);
        h.setI(kOfxPropVersion, 0, 1);
        h.setI(kOfxPropVersion, 0, 2);
        h.setS(kOfxPropVersionLabel, "1");
        h.setI(kOfxPropAPIVersion, 1, 0);
        h.setI(kOfxPropAPIVersion, 5, 1);
        h.setS(kOfxPropType, kOfxTypeImageEffectHost);
        h.setI(kOfxImageEffectHostPropIsBackground, 0);
        h.setI(kOfxImageEffectPropSupportsOverlays, 0);
        h.setI(kOfxImageEffectPropSupportsMultiResolution, 1);
        h.setI(kOfxImageEffectPropSupportsTiles, 0);
        h.setI(kOfxImageEffectPropTemporalClipAccess, 0);
        h.setS(kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0);
        h.setS(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter, 0);
        h.setS(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGeneral, 1);
        h.setS(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextTransition, 2);
        h.setS(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 0);
        h.setS(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthByte, 1);
        h.setI(kOfxImageEffectPropSetableFrameRate, 0);
        h.setI(kOfxImageEffectPropSetableFielding, 0);
        h.setI(kOfxImageEffectPropSupportsMultipleClipDepths, 0);
        h.setI(kOfxImageEffectPropSupportsMultipleClipPARs, 0);
        h.setS(kOfxImageEffectHostPropNativeOrigin, kOfxHostNativeOriginBottomLeft);
        h.setI(kOfxParamHostPropSupportsCustomInteract, 0);
        h.setI(kOfxParamHostPropSupportsStringAnimation, 0);
        h.setI(kOfxParamHostPropSupportsCustomAnimation, 0);
        h.setI(kOfxParamHostPropSupportsBooleanAnimation, 0);
        h.setI(kOfxParamHostPropSupportsChoiceAnimation, 0);
        h.setI(kOfxParamHostPropSupportsStrChoice, 0);
        h.setI(kOfxParamHostPropSupportsStrChoiceAnimation, 0);
        h.setI(kOfxParamHostPropMaxParameters, -1);
        h.setI(kOfxParamHostPropMaxPages, 0);
        h.setI(kOfxParamHostPropPageRowColumnCount, 0, 0);
        h.setI(kOfxParamHostPropPageRowColumnCount, 0, 1);
        h.setS(kOfxImageEffectPropOpenGLRenderSupported, "false");
        g_host.host = &g_hostProps;
        g_host.fetchSuite = fetchSuite;
    });
}

bool failed(OfxStatus st)
{
    return st != kOfxStatOK && st != kOfxStatReplyDefault;
}

OfxStatus callPlugin(OfxPlugin* p, const char* action, const void* handle,
    OfxPropertySetHandle in = nullptr, OfxPropertySetHandle out = nullptr)
{
    const OfxStatus st = p->mainEntry(action, handle, in, out);
    if (traceOn() && failed(st))
        std::fprintf(stderr, "ofx: %s of %s returned status %d\n", action, p->pluginIdentifier, int(st));
    return st;
}

// ---- parameters ----

int dimsOf(const std::string& type)
{
    if (type == kOfxParamTypeDouble2D || type == kOfxParamTypeInteger2D)
        return 2;
    if (type == kOfxParamTypeDouble3D || type == kOfxParamTypeInteger3D || type == kOfxParamTypeRGB)
        return 3;
    if (type == kOfxParamTypeRGBA)
        return 4;
    return 1;
}

bool isIntType(const std::string& type)
{
    return type == kOfxParamTypeInteger || type == kOfxParamTypeInteger2D
        || type == kOfxParamTypeInteger3D || type == kOfxParamTypeBoolean
        || type == kOfxParamTypeChoice;
}

bool isStringType(const std::string& type)
{
    return type == kOfxParamTypeString || type == kOfxParamTypeStrChoice
        || type == kOfxParamTypeCustom;
}

OfxParamStruct* newParam(OfxParamSetStruct* set, const std::string& type, const std::string& name)
{
    auto p = std::make_unique<OfxParamStruct>();
    p->name = name;
    p->type = type;
    p->set = set;
    OfxPropertySetStruct& pr = p->props;
    pr.setS(kOfxPropType, kOfxTypeParameter);
    pr.setS(kOfxPropName, name);
    pr.setS(kOfxParamPropType, type);
    pr.setS(kOfxPropLabel, name);
    pr.setI(kOfxParamPropSecret, 0);
    pr.setI(kOfxParamPropEnabled, 1);
    pr.setI(kOfxParamPropAnimates, 1);
    pr.setI(kOfxParamPropEvaluateOnChange, 1);
    pr.setI(kOfxParamPropCanUndo, 1);
    pr.setI(kOfxParamPropPersistant, 1);
    pr.setI(kOfxParamPropIsAnimating, 0);
    pr.setI(kOfxParamPropIsAutoKeying, 0);
    pr.setS(kOfxParamPropDefaultCoordinateSystem, kOfxParamCoordinatesCanonical);
    pr.setS(kOfxParamPropHint, "");
    pr.setS(kOfxParamPropScriptName, name);
    pr.setS(kOfxParamPropParent, "");
    const int dims = dimsOf(type);
    p->value.v = QVector<double>(dims, 0.0);
    OfxParamStruct* raw = p.get();
    set->byName[name] = raw;
    set->list.push_back(std::move(p));
    return raw;
}

/// Defaults written into the property set of a new parameter, so describe code that only sets
/// what differs works; also what the value starts as.
void applyDefaults(OfxParamStruct* p)
{
    OfxPropertySetStruct& pr = p->props;
    const std::string& t = p->type;
    const int dims = dimsOf(t);
    if (isStringType(t))
        return;
    for (int k = 0; k < dims; ++k)
        p->value.v[k] = pr.getD(kOfxParamPropDefault, 0.0, k);
}

// ---- Describe ----

void readParamInfo(const OfxParamStruct& p, ParamInfo* out)
{
    const OfxPropertySetStruct& pr = p.props;
    const std::string& t = p.type;
    ParamInfo info;
    info.name = qs(p.name);
    info.type = stripPrefix(t, "OfxParamType");
    info.label = qs(pr.getS(kOfxPropLabel, p.name));
    if (info.label.isEmpty())
        info.label = info.name;
    info.hint = qs(pr.getS(kOfxParamPropHint));
    info.parent = qs(pr.getS(kOfxParamPropParent));
    info.dims = dimsOf(t);
    info.animates = pr.getI(kOfxParamPropAnimates, 1) != 0;
    info.secret = pr.getI(kOfxParamPropSecret, 0) != 0;
    info.enabled = pr.getI(kOfxParamPropEnabled, 1) != 0;
    info.evaluateOnChange = pr.getI(kOfxParamPropEvaluateOnChange, 1) != 0;
    info.persistent = pr.getI(kOfxParamPropPersistant, 1) != 0;
    info.increment = pr.getD(kOfxParamPropIncrement, 1.0);
    info.digits = pr.getI(kOfxParamPropDigits, 2);
    const std::string dt = pr.getS(kOfxParamPropDoubleType);
    info.doubleType = stripPrefix(dt, "OfxParamDoubleType");
    info.coordinates = stripPrefix(pr.getS(kOfxParamPropDefaultCoordinateSystem), "OfxParamCoordinates");
    info.stringMode = stripPrefix(pr.getS(kOfxParamPropStringMode), "OfxParamStringIs");
    for (int k = 0; k < pr.count(kOfxParamPropDimensionLabel); ++k)
        info.dimLabels.append(qs(pr.getS(kOfxParamPropDimensionLabel, {}, k)));
    const bool isInt = isIntType(t);
    for (int k = 0; k < info.dims; ++k) {
        info.def.append(pr.getD(kOfxParamPropDefault, 0.0, k));
        if (t == kOfxParamTypeRGB || t == kOfxParamTypeRGBA) {
            info.min.append(pr.getD(kOfxParamPropMin, 0.0, k));
            info.max.append(pr.getD(kOfxParamPropMax, 1.0, k));
        } else if (isInt) {
            info.min.append(pr.getD(kOfxParamPropMin, INT_MIN, k));
            info.max.append(pr.getD(kOfxParamPropMax, INT_MAX, k));
        } else {
            info.min.append(pr.getD(kOfxParamPropMin, -DBL_MAX, k));
            info.max.append(pr.getD(kOfxParamPropMax, DBL_MAX, k));
        }
        info.displayMin.append(pr.getD(kOfxParamPropDisplayMin, info.min.last(), k));
        info.displayMax.append(pr.getD(kOfxParamPropDisplayMax, info.max.last(), k));
    }
    if (isStringType(t))
        info.defString = qs(pr.getS(kOfxParamPropDefault));
    if (t == kOfxParamTypeChoice || t == kOfxParamTypeStrChoice) {
        const int n = pr.count(kOfxParamPropChoiceOption);
        for (int k = 0; k < n; ++k) {
            info.choices.append(qs(pr.getS(kOfxParamPropChoiceOption, {}, k)));
            info.choiceLabels.append(info.choices.last());
        }
    }
    *out = info;
}

QStringList listOf(const OfxPropertySetStruct& p, const char* key)
{
    QStringList out;
    for (int k = 0; k < p.count(key); ++k)
        out.append(qs(p.getS(key, {}, k)));
    return out;
}

/// Describe + describeInContext of plugin @p idx (once per process).
Described* describe(Binary& bin, int idx)
{
    if (bin.described.size() <= size_t(idx))
        bin.described.resize(size_t(idx) + 1);
    if (bin.described[size_t(idx)])
        return bin.described[size_t(idx)].get();
    auto d = std::make_unique<Described>();
    PluginInfo& info = d->info;
    OfxPlugin* plugin = bin.plugins[size_t(idx)];
    info.index = idx;
    info.identifier = QString::fromUtf8(plugin->pluginIdentifier);
    info.major = int(plugin->pluginVersionMajor);
    info.minor = int(plugin->pluginVersionMinor);
    info.label = info.identifier;
    if (!bin.loadedOk[size_t(idx)]) {
        info.error = QStringLiteral(
            "the plugin refused to load (it needs a host feature Kinoklip does not have, such as random access to other frames)");
        bin.described[size_t(idx)] = std::move(d);
        return bin.described[size_t(idx)].get();
    }

    OfxImageEffectStruct desc;
    desc.props.setS(kOfxPropType, kOfxTypeImageEffect);
    desc.props.setS(kOfxPropLabel, "");
    desc.props.setS(kOfxImageEffectPluginRenderThreadSafety, kOfxImageEffectRenderInstanceSafe);
    desc.props.setI(kOfxImageEffectPropSupportsMultiResolution, 1);
    desc.props.setI(kOfxImageEffectPropSupportsTiles, 1);
    desc.props.setI(kOfxImageEffectPropTemporalClipAccess, 0);
    desc.props.setI(kOfxImageEffectPluginPropSingleInstance, 0);
    desc.props.setI(kOfxImageEffectPluginPropHostFrameThreading, 1);
    desc.props.setS(kOfxImageEffectPluginPropFieldRenderTwiceAlways, "");
    desc.props.m.erase(kOfxImageEffectPluginPropFieldRenderTwiceAlways);
    desc.props.setI(kOfxImageEffectPluginPropFieldRenderTwiceAlways, 1);
    desc.props.setP(kOfxImageEffectPluginPropOverlayInteractV1, nullptr);
    desc.props.m.erase(kOfxImageEffectPluginPropOverlayInteractV1);

    OfxStatus st = callPlugin(plugin, kOfxActionDescribe, &desc);
    if (failed(st)) {
        info.error = st == kOfxStatErrMissingHostFeature
            ? QStringLiteral("it needs a host feature Kinoklip does not have")
            : QStringLiteral("the describe action failed (status %1)").arg(st);
        bin.described[size_t(idx)] = std::move(d);
        return bin.described[size_t(idx)].get();
    }
    const OfxPropertySetStruct& pr = desc.props;
    info.label = qs(pr.getS(kOfxPropLabel, ss(info.identifier)));
    if (info.label.isEmpty())
        info.label = qs(pr.getS(kOfxPropShortLabel, ss(info.identifier)));
    if (info.label.isEmpty())
        info.label = info.identifier;
    info.shortLabel = qs(pr.getS(kOfxPropShortLabel));
    info.group = qs(pr.getS(kOfxImageEffectPluginPropGrouping));
    info.description = qs(pr.getS(kOfxPropPluginDescription));
    info.versionLabel = qs(pr.getS(kOfxPropVersionLabel));
    for (const QString& depth : listOf(pr, kOfxImageEffectPropSupportedPixelDepths))
        info.pixelDepths.append(stripPrefix(ss(depth), "OfxBitDepth"));
    info.threadSafety = stripPrefix(
        pr.getS(kOfxImageEffectPluginRenderThreadSafety, kOfxImageEffectRenderInstanceSafe),
        "OfxImageEffectRender");
    info.multiResolution = pr.getI(kOfxImageEffectPropSupportsMultiResolution, 1) != 0;
    info.tiles = pr.getI(kOfxImageEffectPropSupportsTiles, 1) != 0;
    info.temporalAccess = pr.getI(kOfxImageEffectPropTemporalClipAccess, 0) != 0;

    const QStringList contexts = listOf(pr, kOfxImageEffectPropSupportedContexts);
    for (const QString& ctxId : contexts) {
        ContextInfo ci;
        ci.context = stripPrefix(ss(ctxId), "OfxImageEffectContext");
        int descIdx = -1;
        if (ci.context != QLatin1String("Filter") && ci.context != QLatin1String("General")
            && ci.context != QLatin1String("Transition")) {
            ci.whyNot = QStringLiteral("the %1 context is not supported yet").arg(ci.context);
            info.contexts.append(ci);
            d->contextIndex.push_back(-1);
            continue;
        }
        auto ctxEffect = std::make_unique<OfxImageEffectStruct>();
        ctxEffect->props = desc.props;
        ctxEffect->props.setS(kOfxImageEffectPropContext, ss(ctxId));
        OfxPropertySetStruct in;
        in.setS(kOfxImageEffectPropContext, ss(ctxId));
        st = callPlugin(plugin, kOfxImageEffectActionDescribeInContext, ctxEffect.get(), &in);
        if (failed(st)) {
            ci.whyNot = st == kOfxStatErrUnsupported || st == kOfxStatErrMissingHostFeature
                ? QStringLiteral("it needs a host feature Kinoklip does not have (such as parametric parameters)")
                : QStringLiteral("describeInContext failed (status %1)").arg(st);
            info.contexts.append(ci);
            d->contextIndex.push_back(-1);
            continue;
        }
        bool hasSource = false, hasOutput = false, hasFrom = false, hasTo = false;
        QStringList blockers;
        for (const auto& clip : ctxEffect->clips) {
            ClipInfo c;
            c.name = qs(clip->name);
            c.optional = clip->props.getI(kOfxImageClipPropOptional, 0) != 0;
            c.mask = clip->props.getI(kOfxImageClipPropIsMask, 0) != 0;
            c.temporal = clip->props.getI(kOfxImageEffectPropTemporalClipAccess, 0) != 0;
            ci.clips.append(c);
            if (clip->name == kOfxImageEffectSimpleSourceClipName)
                hasSource = true;
            else if (clip->name == kOfxImageEffectOutputClipName)
                hasOutput = true;
            else if (clip->name == kOfxImageEffectTransitionSourceFromClipName)
                hasFrom = true;
            else if (clip->name == kOfxImageEffectTransitionSourceToClipName)
                hasTo = true;
            else if (!c.optional)
                blockers.append(c.name);
        }
        for (const auto& p : ctxEffect->params.list) {
            ParamInfo pi;
            readParamInfo(*p, &pi);
            ci.params.append(pi);
        }
        const bool transition = ci.context == QLatin1String("Transition");
        ci.usable = hasOutput
            && (transition ? (hasFrom && hasTo) : (hasSource || ci.context == QLatin1String("General")))
            && blockers.isEmpty();
        if (!hasOutput)
            ci.whyNot = QStringLiteral("no Output clip");
        else if (transition && !(hasFrom && hasTo))
            ci.whyNot = QStringLiteral("no SourceFrom and SourceTo clips");
        else if (!hasSource && ci.context == QLatin1String("Filter"))
            ci.whyNot = QStringLiteral("no Source clip");
        else if (!blockers.isEmpty())
            ci.whyNot = QStringLiteral("needs more than one input (%1)").arg(blockers.join(QLatin1String(", ")));
        descIdx = int(d->contexts.size());
        d->contexts.push_back(std::move(ctxEffect));
        info.contexts.append(ci);
        d->contextIndex.push_back(descIdx);
    }
    if (info.usableContext() == nullptr && info.error.isEmpty()) {
        QStringList reasons;
        for (const ContextInfo& c : info.contexts)
            reasons.append(c.context + QLatin1String(": ") + c.whyNot);
        info.error = reasons.isEmpty() ? QStringLiteral("no supported context")
                                       : reasons.join(QLatin1String("; "));
    }
    bin.described[size_t(idx)] = std::move(d);
    return bin.described[size_t(idx)].get();
}

Binary* loadBinary(const QString& path, QString* error)
{
    initHost();
    std::lock_guard<std::mutex> lock(g_binariesMutex);
    if (auto it = g_binaries.find(path); it != g_binaries.end()) {
        if (!it->second->error.isEmpty() && error)
            *error = it->second->error;
        return it->second->error.isEmpty() ? it->second.get() : nullptr;
    }
    auto bin = std::make_unique<Binary>();
    bin->path = path;
    auto fail = [&](const QString& why) -> Binary* {
        bin->error = why;
        if (error)
            *error = why;
        g_binaries[path] = std::move(bin);
        return nullptr;
    };
#ifdef _WIN32
    return fail(QStringLiteral("OpenFX plugins are only loaded on Linux so far"));
#else
    bin->dl = dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
    if (!bin->dl) {
        const char* e = dlerror();
        return fail(QStringLiteral("cannot load the binary: %1").arg(QString::fromUtf8(e ? e : "?")));
    }
    using GetNumber = int (*)(void);
    using GetPlugin = OfxPlugin* (*)(int);
    auto getNumber = reinterpret_cast<GetNumber>(dlsym(bin->dl, "OfxGetNumberOfPlugins"));
    auto getPlugin = reinterpret_cast<GetPlugin>(dlsym(bin->dl, "OfxGetPlugin"));
    if (!getNumber || !getPlugin)
        return fail(QStringLiteral("not an OpenFX binary (OfxGetNumberOfPlugins / OfxGetPlugin missing)"));
    const int n = getNumber();
    for (int k = 0; k < n; ++k) {
        OfxPlugin* p = getPlugin(k);
        if (!p || !p->pluginApi || std::strcmp(p->pluginApi, kOfxImageEffectPluginApi) != 0
            || p->apiVersion != 1 || !p->mainEntry)
            continue; // another kind of plugin, or one this host does not know
        bin->plugins.push_back(p);
    }
#endif
    bin->loadedOk.assign(bin->plugins.size(), false);
    for (size_t k = 0; k < bin->plugins.size(); ++k) {
        OfxPlugin* p = bin->plugins[k];
        if (p->setHost)
            p->setHost(&g_host);
        bin->loadedOk[k] = !failed(callPlugin(p, kOfxActionLoad, nullptr));
    }
    Binary* raw = bin.get();
    g_binaries[path] = std::move(bin);
    return raw;
}

} // namespace

BundleInfo describeBundle(const QString& bundleDir)
{
    BundleInfo out;
    out.bundlePath = bundleDir;
    out.binary = bundleBinary(bundleDir);
    if (out.binary.isEmpty()) {
        out.error = QStringLiteral("the bundle has no binary for this machine (Contents/Linux-x86-64/*.ofx)");
        return out;
    }
    Binary* bin = loadBinary(out.binary, &out.error);
    if (!bin)
        return out;
    std::lock_guard<std::mutex> lock(bin->mutex);
    for (size_t k = 0; k < bin->plugins.size(); ++k)
        out.plugins.append(describe(*bin, int(k))->info);
    return out;
}

// ---- parameter suite ----

namespace {

OfxStatus prDefine(OfxParamSetHandle set, const char* type, const char* name, OfxPropertySetHandle* props)
{
    if (!set || !type || !name)
        return kOfxStatErrBadHandle;
    if (set->byName.count(name))
        return kOfxStatErrExists;
    const std::string t = type;
    static const std::set<std::string> known = {kOfxParamTypeInteger, kOfxParamTypeDouble,
        kOfxParamTypeBoolean, kOfxParamTypeChoice, kOfxParamTypeStrChoice, kOfxParamTypeRGBA,
        kOfxParamTypeRGB, kOfxParamTypeDouble2D, kOfxParamTypeInteger2D, kOfxParamTypeDouble3D,
        kOfxParamTypeInteger3D, kOfxParamTypeString, kOfxParamTypeCustom, kOfxParamTypeGroup,
        kOfxParamTypePage, kOfxParamTypePushButton, kOfxParamTypeBytes};
    if (!known.count(t))
        return kOfxStatErrUnknown;
    OfxParamStruct* p = newParam(set, t, name);
    if (props)
        *props = &p->props;
    return kOfxStatOK;
}

OfxStatus prGetHandle(OfxParamSetHandle set, const char* name, OfxParamHandle* param, OfxPropertySetHandle* props)
{
    if (!set || !name)
        return kOfxStatErrBadHandle;
    const auto it = set->byName.find(name);
    if (it == set->byName.end())
        return kOfxStatErrUnknown;
    *param = it->second;
    if (props)
        *props = &it->second->props;
    return kOfxStatOK;
}

OfxStatus prSetProps(OfxParamSetHandle set, OfxPropertySetHandle* props)
{
    if (!set)
        return kOfxStatErrBadHandle;
    *props = &set->effect->props;
    return kOfxStatOK;
}

OfxStatus prParamProps(OfxParamHandle p, OfxPropertySetHandle* props)
{
    if (!p)
        return kOfxStatErrBadHandle;
    *props = &p->props;
    return kOfxStatOK;
}

Instance::Impl* implOf(OfxParamStruct* p)
{
    return p && p->set && p->set->effect ? static_cast<Instance::Impl*>(p->set->effect->implPtr) : nullptr;
}

/// Current value of @p p at @p time (the stored one, or the evaluator's for animated ones).
ParamValue valueAt(OfxParamStruct* p, bool useTime, double time)
{
    ParamValue v = p->value;
    if (useTime) {
        if (Instance::Impl* impl = implOf(p); impl && impl->evaluator
            && p->props.getI(kOfxParamPropAnimates, 1)) {
            ParamValue out;
            if (impl->evaluator(qs(p->name), time, &out)) {
                if (!out.v.isEmpty())
                    v.v = out.v;
                if (isStringType(p->type))
                    v.s = out.s;
            }
        }
    }
    return v;
}

OfxStatus readValue(OfxParamHandle p, bool useTime, double time, va_list args)
{
    if (!p)
        return kOfxStatErrBadHandle;
    const ParamValue v = valueAt(p, useTime, time);
    const std::string& t = p->type;
    const int dims = dimsOf(t);
    if (isStringType(t)) {
        p->cache = ss(v.s);
        *va_arg(args, const char**) = p->cache.c_str();
        return kOfxStatOK;
    }
    if (t == kOfxParamTypeGroup || t == kOfxParamTypePage || t == kOfxParamTypePushButton
        || t == kOfxParamTypeBytes)
        return kOfxStatErrBadHandle;
    for (int k = 0; k < dims; ++k) {
        const double x = k < v.v.size() ? v.v[k] : 0.0;
        if (isIntType(t))
            *va_arg(args, int*) = int(std::lround(x));
        else
            *va_arg(args, double*) = x;
    }
    return kOfxStatOK;
}

OfxStatus prGetValue(OfxParamHandle p, ...)
{
    va_list args;
    va_start(args, p);
    const OfxStatus st = readValue(p, false, 0, args);
    va_end(args);
    return st;
}

OfxStatus prGetValueAtTime(OfxParamHandle p, OfxTime time, ...)
{
    va_list args;
    va_start(args, time);
    const OfxStatus st = readValue(p, true, time, args);
    va_end(args);
    return st;
}

OfxStatus prGetDerivative(OfxParamHandle, OfxTime, ...)
{
    return kOfxStatErrUnsupported;
}
OfxStatus prGetIntegral(OfxParamHandle, OfxTime, OfxTime, ...)
{
    return kOfxStatErrUnsupported;
}

OfxStatus writeValue(OfxParamHandle p, va_list args)
{
    if (!p)
        return kOfxStatErrBadHandle;
    const std::string& t = p->type;
    const int dims = dimsOf(t);
    if (isStringType(t)) {
        const char* s = va_arg(args, const char*);
        p->value.s = QString::fromUtf8(s ? s : "");
        return kOfxStatOK;
    }
    if (t == kOfxParamTypeGroup || t == kOfxParamTypePage || t == kOfxParamTypePushButton
        || t == kOfxParamTypeBytes)
        return kOfxStatErrBadHandle;
    for (int k = 0; k < dims; ++k) {
        if (isIntType(t))
            p->value.v[k] = va_arg(args, int);
        else
            p->value.v[k] = va_arg(args, double);
    }
    return kOfxStatOK;
}

OfxStatus prSetValue(OfxParamHandle p, ...)
{
    va_list args;
    va_start(args, p);
    const OfxStatus st = writeValue(p, args);
    va_end(args);
    return st;
}

OfxStatus prSetValueAtTime(OfxParamHandle p, OfxTime time, ...)
{
    va_list args;
    va_start(args, time);
    // the argument after the time is read from the same list
    const OfxStatus st = writeValue(p, args);
    va_end(args);
    return st;
}

OfxStatus prNumKeys(OfxParamHandle, unsigned int* n)
{
    *n = 0;
    return kOfxStatOK;
}
OfxStatus prKeyTime(OfxParamHandle, unsigned int, OfxTime*)
{
    return kOfxStatErrBadIndex;
}
OfxStatus prKeyIndex(OfxParamHandle, OfxTime, int, int* index)
{
    *index = -1;
    return kOfxStatFailed;
}
OfxStatus prDeleteKey(OfxParamHandle, OfxTime)
{
    return kOfxStatOK;
}
OfxStatus prDeleteAllKeys(OfxParamHandle)
{
    return kOfxStatOK;
}
OfxStatus prCopy(OfxParamHandle to, OfxParamHandle from, OfxTime, const OfxRangeD*)
{
    if (!to || !from)
        return kOfxStatErrBadHandle;
    to->value = from->value;
    return kOfxStatOK;
}
OfxStatus prEditBegin(OfxParamSetHandle, const char*)
{
    return kOfxStatOK;
}
OfxStatus prEditEnd(OfxParamSetHandle)
{
    return kOfxStatOK;
}

} // namespace

namespace {
const OfxParameterSuiteV1 kParameterSuite = {prDefine, prGetHandle, prSetProps, prParamProps,
    prGetValue, prGetValueAtTime, prGetDerivative, prGetIntegral, prSetValue, prSetValueAtTime,
    prNumKeys, prKeyTime, prKeyIndex, prDeleteKey, prDeleteAllKeys, prCopy, prEditBegin, prEditEnd};
}

// ---- image effect suite ----

namespace {

OfxStatus ieGetProps(OfxImageEffectHandle e, OfxPropertySetHandle* p)
{
    if (!e)
        return kOfxStatErrBadHandle;
    *p = &e->props;
    return kOfxStatOK;
}
OfxStatus ieGetParamSet(OfxImageEffectHandle e, OfxParamSetHandle* p)
{
    if (!e)
        return kOfxStatErrBadHandle;
    *p = &e->params;
    return kOfxStatOK;
}
OfxStatus ieClipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* props)
{
    if (!e || !name)
        return kOfxStatErrBadHandle;
    for (const auto& c : e->clips)
        if (c->name == name)
            return kOfxStatErrExists;
    auto c = std::make_unique<OfxImageClipStruct>();
    c->name = name;
    c->effect = e;
    c->props.setS(kOfxPropType, kOfxTypeClip);
    c->props.setS(kOfxPropName, name);
    c->props.setS(kOfxPropLabel, name);
    c->props.setI(kOfxImageClipPropOptional, 0);
    c->props.setI(kOfxImageClipPropIsMask, 0);
    c->props.setI(kOfxImageEffectPropTemporalClipAccess, 0);
    if (props)
        *props = &c->props;
    e->clips.push_back(std::move(c));
    return kOfxStatOK;
}
OfxStatus ieClipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* props)
{
    if (!e || !name)
        return kOfxStatErrBadHandle;
    for (const auto& c : e->clips) {
        if (c->name == name) {
            *clip = c.get();
            if (props)
                *props = &c->props;
            return kOfxStatOK;
        }
    }
    return kOfxStatErrUnknown;
}
OfxStatus ieClipProps(OfxImageClipHandle c, OfxPropertySetHandle* p)
{
    if (!c)
        return kOfxStatErrBadHandle;
    *p = &c->props;
    return kOfxStatOK;
}

const char* depthName(bool byte)
{
    return byte ? kOfxBitDepthByte : kOfxBitDepthFloat;
}

OfxStatus ieClipGetImage(OfxImageClipHandle c, OfxTime, const OfxRectD*, OfxPropertySetHandle* out)
{
    if (!c || !c->effect)
        return kOfxStatErrBadHandle;
    auto* impl = static_cast<Instance::Impl*>(c->effect->implPtr);
    if (!impl)
        return kOfxStatErrBadHandle;
    std::lock_guard<std::mutex> lock(impl->stateMutex);
    Instance::Impl::Current* cur = impl->current;
    const bool isOutput = c->name == kOfxImageEffectOutputClipName;
    const bool isTo = c->name == kOfxImageEffectTransitionSourceToClipName;
    const bool isSource = c->name == kOfxImageEffectSimpleSourceClipName
        || c->name == kOfxImageEffectTransitionSourceFromClipName || isTo;
    if (!cur || !(isOutput || isSource))
        return kOfxStatFailed; // not connected: the plugin carries on without
    const RenderArgs& a = cur->args;
    auto* img = new OfxPropertySetStruct;
    const int bytesPerPixel = cur->byte ? 4 : 16;
    void* data = nullptr;
    if (cur->byte)
        data = isOutput ? static_cast<void*>(cur->dstByte.data())
            : isTo      ? static_cast<void*>(cur->srcToByte.data())
                        : static_cast<void*>(cur->srcByte.data());
    else
        data = isOutput ? static_cast<void*>(cur->dstFloat)
            : isTo      ? const_cast<float*>(cur->srcToFloat)
                        : const_cast<float*>(cur->srcFloat);
    const int x1 = int(std::lround(a.originX * a.scale));
    const int y1 = int(std::lround(a.originY * a.scale));
    img->setS(kOfxPropType, "OfxTypeImage");
    img->setS(kOfxImageEffectPropPixelDepth, depthName(cur->byte));
    img->setS(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
    img->setS(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
    img->setD(kOfxImageEffectPropRenderScale, a.scale, 0);
    img->setD(kOfxImageEffectPropRenderScale, a.scale, 1);
    img->setD(kOfxImagePropPixelAspectRatio, 1.0);
    img->setP(kOfxImagePropData, data);
    const int bounds[4] = {x1, y1, x1 + a.width, y1 + a.height};
    for (int k = 0; k < 4; ++k) {
        img->setI(kOfxImagePropBounds, bounds[k], k);
        img->setI(kOfxImagePropRegionOfDefinition, bounds[k], k);
    }
    img->setI(kOfxImagePropRowBytes, a.width * bytesPerPixel);
    img->setS(kOfxImagePropField, kOfxImageFieldNone);
    img->setS(kOfxImagePropUniqueIdentifier, isOutput ? "kk-output" : isTo ? "kk-source-to" : "kk-source");
    img->setP("kk.impl", impl);
    impl->images.insert(img);
    *out = img;
    return kOfxStatOK;
}

OfxStatus ieClipReleaseImage(OfxPropertySetHandle img)
{
    if (!img)
        return kOfxStatErrBadHandle;
    void* owner = nullptr;
    if (img->find("kk.impl") && !img->find("kk.impl")->p.empty())
        owner = img->find("kk.impl")->p[0];
    if (auto* impl = static_cast<Instance::Impl*>(owner)) {
        std::lock_guard<std::mutex> lock(impl->stateMutex);
        impl->images.erase(img);
    }
    delete img;
    return kOfxStatOK;
}

OfxStatus ieClipRoD(OfxImageClipHandle c, OfxTime, OfxRectD* b)
{
    if (!c || !c->effect)
        return kOfxStatErrBadHandle;
    auto* impl = static_cast<Instance::Impl*>(c->effect->implPtr);
    if (!impl || !impl->current)
        return kOfxStatFailed;
    const RenderArgs& a = impl->current->args;
    b->x1 = a.originX;
    b->y1 = a.originY;
    b->x2 = a.originX + a.width / a.scale;
    b->y2 = a.originY + a.height / a.scale;
    return kOfxStatOK;
}

int ieAbort(OfxImageEffectHandle)
{
    return 0;
}

OfxStatus ieMemAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* h)
{
    auto* m = new OfxImageMemoryStruct;
    m->data = std::malloc(n ? n : 1);
    m->size = n;
    if (!m->data) {
        delete m;
        return kOfxStatErrMemory;
    }
    *h = m;
    return kOfxStatOK;
}
OfxStatus ieMemFree(OfxImageMemoryHandle h)
{
    if (!h)
        return kOfxStatErrBadHandle;
    std::free(h->data);
    delete h;
    return kOfxStatOK;
}
OfxStatus ieMemLock(OfxImageMemoryHandle h, void** p)
{
    if (!h)
        return kOfxStatErrBadHandle;
    *p = h->data;
    return kOfxStatOK;
}
OfxStatus ieMemUnlock(OfxImageMemoryHandle h)
{
    return h ? kOfxStatOK : kOfxStatErrBadHandle;
}

const OfxImageEffectSuiteV1 kImageEffectSuite = {ieGetProps, ieGetParamSet, ieClipDefine,
    ieClipGetHandle, ieClipProps, ieClipGetImage, ieClipReleaseImage, ieClipRoD, ieAbort,
    ieMemAlloc, ieMemFree, ieMemLock, ieMemUnlock};


const void* fetchSuite(OfxPropertySetHandle, const char* name, int version)
{
    const void* found = fetchSuiteImpl(name, version);
    if (!found && traceOn())
        std::fprintf(stderr, "ofx: suite %s v%d not provided\n", name ? name : "?", version);
    return found;
}

const void* fetchSuiteImpl(const char* name, int version)
{
    const std::string n = name ? name : "";
    if (n == kOfxPropertySuite && version == 1)
        return &kPropertySuite;
    if (n == kOfxImageEffectSuite && version == 1)
        return &kImageEffectSuite;
    if (n == kOfxParameterSuite && version == 1)
        return &kParameterSuite;
    if (n == kOfxMemorySuite && version == 1)
        return &kMemorySuite;
    if (n == kOfxMultiThreadSuite && version == 1)
        return &kMultiThreadSuite;
    if (n == kOfxMessageSuite && (version == 1 || version == 2))
        return &kMessageSuite;
    if (n == kOfxProgressSuite && version == 1)
        return &kProgressSuite;
    if (n == kOfxTimeLineSuite && version == 1)
        return &kTimeLineSuite;
    return nullptr;
}

} // namespace

// ---- Instance ----

Instance::Instance() = default;

Instance::~Instance()
{
    if (d && d->plugin) {
        callPlugin(d->plugin, kOfxActionDestroyInstance, &d->effect);
    }
}

const PluginInfo& Instance::info() const
{
    return d->info;
}

void Instance::setEvaluator(Evaluator evaluator)
{
    d->evaluator = std::move(evaluator);
}

QString Instance::lastMessage() const
{
    std::lock_guard<std::mutex> lock(g_messageMutex);
    const auto it = g_messages.find(&d->effect);
    return it == g_messages.end() ? QString() : it->second;
}

ParamValue Instance::value(const QString& name) const
{
    const auto it = d->effect.params.byName.find(ss(name));
    return it == d->effect.params.byName.end() ? ParamValue() : it->second->value;
}

bool Instance::setValue(const QString& name, const ParamValue& value)
{
    const auto it = d->effect.params.byName.find(ss(name));
    if (it == d->effect.params.byName.end())
        return false;
    OfxParamStruct* p = it->second;
    const bool same = p->value.v == value.v && p->value.s == value.s;
    p->value.v = value.v;
    while (p->value.v.size() < dimsOf(p->type))
        p->value.v.append(0.0);
    p->value.s = value.s;
    if (same)
        return true;
    // tell the plugin: begin / changed / end, as a user edit at time 0
    OfxPropertySetStruct in;
    in.setS(kOfxPropType, kOfxTypeParameter);
    in.setS(kOfxPropName, p->name);
    in.setS(kOfxPropChangeReason, kOfxChangeUserEdited);
    in.setD(kOfxPropTime, 0.0);
    in.setD(kOfxImageEffectPropRenderScale, 1.0, 0);
    in.setD(kOfxImageEffectPropRenderScale, 1.0, 1);
    std::lock_guard<std::mutex> lock(d->renderMutex);
    callPlugin(d->plugin, kOfxActionBeginInstanceChanged, &d->effect, &in);
    callPlugin(d->plugin, kOfxActionInstanceChanged, &d->effect, &in);
    callPlugin(d->plugin, kOfxActionEndInstanceChanged, &d->effect, &in);
    return true;
}

std::unique_ptr<Instance> Instance::create(const BundleInfo& bundle, int pluginIndex, QString* error)
{
    auto fail = [&](const QString& why) -> std::unique_ptr<Instance> {
        if (error)
            *error = why;
        return nullptr;
    };
    QString why;
    Binary* bin = loadBinary(bundle.binary.isEmpty() ? bundleBinary(bundle.bundlePath) : bundle.binary, &why);
    if (!bin)
        return fail(why);
    std::lock_guard<std::mutex> lock(bin->mutex);
    if (pluginIndex < 0 || size_t(pluginIndex) >= bin->plugins.size())
        return fail(QStringLiteral("the bundle has no plugin number %1").arg(pluginIndex));
    Described* desc = describe(*bin, pluginIndex);
    if (!desc->info.error.isEmpty())
        return fail(desc->info.error);
    const ContextInfo* ctx = desc->info.usableContext();
    int ctxPos = -1;
    for (int k = 0; k < desc->info.contexts.size(); ++k)
        if (&desc->info.contexts[k] == ctx)
            ctxPos = k;
    if (ctxPos < 0 || desc->contextIndex[size_t(ctxPos)] < 0)
        return fail(QStringLiteral("no usable context"));
    const OfxImageEffectStruct& src = *desc->contexts[size_t(desc->contextIndex[size_t(ctxPos)])];

    std::unique_ptr<Instance> inst(new Instance);
    inst->d = std::make_unique<Impl>();
    Impl& im = *inst->d;
    im.info = desc->info;
    im.ctx = &im.info.contexts[ctxPos];
    im.plugin = bin->plugins[size_t(pluginIndex)];
    im.effect.implPtr = &im;
    im.effect.instance = inst.get();
    im.effect.props = src.props;
    OfxPropertySetStruct& pr = im.effect.props;
    pr.setS(kOfxPropType, kOfxTypeImageEffectInstance);
    pr.setP(kOfxPropInstanceData, nullptr);
    pr.setI(kOfxPropIsInteractive, 0);
    pr.setD(kOfxImageEffectPropProjectSize, 1920, 0);
    pr.setD(kOfxImageEffectPropProjectSize, 1080, 1);
    pr.setD(kOfxImageEffectPropProjectOffset, 0, 0);
    pr.setD(kOfxImageEffectPropProjectOffset, 0, 1);
    pr.setD(kOfxImageEffectPropProjectExtent, 1920, 0);
    pr.setD(kOfxImageEffectPropProjectExtent, 1080, 1);
    pr.setD(kOfxImageEffectPropProjectPixelAspectRatio, 1.0);
    pr.setD(kOfxImageEffectInstancePropEffectDuration, 1.0e6);
    pr.setD(kOfxImageEffectPropFrameRate, 25.0);
    pr.setI(kOfxImageEffectInstancePropSequentialRender, 0);
    pr.setS(kOfxImageEffectPropContext, ss(QStringLiteral("OfxImageEffectContext") + ctx->context));
    pr.setI(kOfxImageEffectPropOpenGLEnabled, 0);
    const bool floatOk = im.info.pixelDepths.contains(QLatin1String("Float"));
    const bool byteOk = im.info.pixelDepths.contains(QLatin1String("Byte"));
    if (!floatOk && !byteOk)
        return fail(QStringLiteral("the plugin supports neither float nor 8 bit pixels"));
    im.chosenDepth = floatOk ? kOfxBitDepthFloat : kOfxBitDepthByte;

    // parameters and clips, copied from the descriptor
    for (const auto& sp : src.params.list) {
        OfxParamStruct* p = newParam(&im.effect.params, sp->type, sp->name);
        p->props = sp->props;
        p->value.v = QVector<double>(dimsOf(p->type), 0.0);
        if (isStringType(p->type))
            p->value.s = qs(p->props.getS(kOfxParamPropDefault));
        else
            applyDefaults(p);
    }
    for (const auto& sc : src.clips) {
        auto c = std::make_unique<OfxImageClipStruct>();
        c->name = sc->name;
        c->effect = &im.effect;
        c->props = sc->props;
        const bool connected = sc->name == kOfxImageEffectSimpleSourceClipName
            || sc->name == kOfxImageEffectOutputClipName
            || sc->name == kOfxImageEffectTransitionSourceFromClipName
            || sc->name == kOfxImageEffectTransitionSourceToClipName;
        c->props.setS(kOfxPropType, kOfxTypeClip);
        c->props.setI(kOfxImageClipPropConnected, connected ? 1 : 0);
        c->props.setS(kOfxImageEffectPropPixelDepth, im.chosenDepth);
        c->props.setS(kOfxImageClipPropUnmappedPixelDepth, im.chosenDepth);
        c->props.setS(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
        c->props.setS(kOfxImageClipPropUnmappedComponents, kOfxImageComponentRGBA);
        c->props.setS(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
        c->props.setD(kOfxImagePropPixelAspectRatio, 1.0);
        c->props.setD(kOfxImageEffectPropFrameRate, 25.0);
        c->props.setD(kOfxImageEffectPropUnmappedFrameRate, 25.0);
        c->props.setD(kOfxImageEffectPropFrameRange, 0.0, 0);
        c->props.setD(kOfxImageEffectPropFrameRange, 1.0e6, 1);
        c->props.setD(kOfxImageEffectPropUnmappedFrameRange, 0.0, 0);
        c->props.setD(kOfxImageEffectPropUnmappedFrameRange, 1.0e6, 1);
        c->props.setS(kOfxImageClipPropFieldOrder, kOfxImageFieldNone);
        c->props.setI(kOfxImageClipPropContinuousSamples, 0);
        im.effect.clips.push_back(std::move(c));
    }
    if (failed(callPlugin(im.plugin, kOfxActionCreateInstance, &im.effect)))
        return fail(QStringLiteral("the create instance action failed"));
    OfxPropertySetStruct prefs;
    callPlugin(im.plugin, kOfxImageEffectActionGetClipPreferences, &im.effect, nullptr, &prefs);
    return inst;
}

RenderResult Instance::render(
    const RenderArgs& a, const float* src, float* dst, QString* error, const float* srcTo)
{
    Impl& im = *d;
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return RenderResult::Failed;
    };
    if (a.width <= 0 || a.height <= 0 || !dst)
        return fail(QStringLiteral("empty picture"));
    // thread-safety flag of the plugin
    std::unique_lock<std::mutex> instanceLock(im.renderMutex, std::defer_lock);
    std::unique_lock<std::mutex> pluginLock;
    if (im.info.threadSafety == QLatin1String("Unsafe")) {
        std::mutex* m;
        {
            std::lock_guard<std::mutex> g(g_pluginLockMutex);
            auto& slot = g_pluginLocks[ss(im.info.identifier)];
            if (!slot)
                slot = std::make_unique<std::mutex>();
            m = slot.get();
        }
        pluginLock = std::unique_lock<std::mutex>(*m);
    }
    instanceLock.lock();

    Impl::Current cur;
    cur.args = a;
    cur.byte = im.chosenDepth == kOfxBitDepthByte;
    const size_t pixels = size_t(a.width) * size_t(a.height);
    if (cur.byte) {
        cur.srcByte.resize(pixels * 4);
        cur.srcToByte.resize(pixels * 4);
        cur.dstByte.assign(pixels * 4, 0);
        const auto toBytes = [pixels](const float* from, std::vector<unsigned char>& to) {
            if (!from)
                return;
            for (size_t k = 0; k < pixels * 4; ++k)
                to[k] = static_cast<unsigned char>(
                    std::lround(std::clamp(from[k], 0.0f, 1.0f) * 255.0f));
        };
        toBytes(src, cur.srcByte);
        toBytes(srcTo, cur.srcToByte);
    } else {
        cur.srcFloat = src;
        cur.srcToFloat = srcTo;
        cur.dstFloat = dst;
    }
    {
        std::lock_guard<std::mutex> lock(im.stateMutex);
        im.current = &cur;
    }
    // per-render properties of the instance
    OfxPropertySetStruct& pr = im.effect.props;
    pr.setD(kOfxImageEffectPropProjectSize, a.projectWidth, 0);
    pr.setD(kOfxImageEffectPropProjectSize, a.projectHeight, 1);
    pr.setD(kOfxImageEffectPropProjectExtent, a.projectWidth, 0);
    pr.setD(kOfxImageEffectPropProjectExtent, a.projectHeight, 1);
    pr.setD(kOfxImageEffectPropFrameRate, a.fps);
    pr.setD(kOfxImageEffectInstancePropEffectDuration, a.duration);
    for (const auto& clip : im.effect.clips) {
        clip->props.setD(kOfxImageEffectPropFrameRate, a.fps);
        clip->props.setD(kOfxImageEffectPropUnmappedFrameRate, a.fps);
    }
    const int x1 = int(std::lround(a.originX * a.scale));
    const int y1 = int(std::lround(a.originY * a.scale));
    auto windowArgs = [&](OfxPropertySetStruct& in) {
        in.setD(kOfxPropTime, a.time);
        in.setS(kOfxImageEffectPropFieldToRender, kOfxImageFieldNone);
        const int w[4] = {x1, y1, x1 + a.width, y1 + a.height};
        for (int k = 0; k < 4; ++k)
            in.setI(kOfxImageEffectPropRenderWindow, w[k], k);
        in.setD(kOfxImageEffectPropRenderScale, a.scale, 0);
        in.setD(kOfxImageEffectPropRenderScale, a.scale, 1);
        in.setI(kOfxImageEffectPropSequentialRenderStatus, 0);
        in.setI(kOfxImageEffectPropInteractiveRenderStatus, a.interactive ? 1 : 0);
        in.setI(kOfxImageEffectPropRenderQualityDraft, 0);
    };

    RenderResult result = RenderResult::Rendered;
    // identity check
    {
        OfxPropertySetStruct in, out;
        windowArgs(in);
        out.setS(kOfxPropName, "");
        out.setD(kOfxPropTime, a.time);
        const OfxStatus st = callPlugin(im.plugin, kOfxImageEffectActionIsIdentity, &im.effect, &in, &out);
        if (st == kOfxStatOK && out.getS(kOfxPropName) == kOfxImageEffectSimpleSourceClipName
            && out.getD(kOfxPropTime, a.time) == a.time)
            result = RenderResult::Identity;
    }
    if (result == RenderResult::Rendered) {
        OfxPropertySetStruct in;
        windowArgs(in);
        const OfxStatus st = callPlugin(im.plugin, kOfxImageEffectActionRender, &im.effect, &in);
        if (failed(st))
            result = fail(QStringLiteral("the plugin's render failed (status %1)").arg(st));
    }
    {
        std::lock_guard<std::mutex> lock(im.stateMutex);
        im.current = nullptr;
        for (OfxPropertySetStruct* img : im.images)
            delete img; // images the plugin forgot to release
        im.images.clear();
    }
    if (result == RenderResult::Rendered && cur.byte) {
        for (size_t k = 0; k < pixels * 4; ++k)
            dst[k] = cur.dstByte[k] / 255.0f;
    }
    return result;
}

bool Instance::regionOfDefinition(
    double time, double fps, double width, double height, double out[4])
{
    Impl& im = *d;
    if (width <= 0 || height <= 0)
        return false;
    std::lock_guard<std::mutex> instanceLock(im.renderMutex);
    Impl::Current cur;
    cur.args.time = time;
    cur.args.fps = fps;
    cur.args.width = int(std::ceil(width));
    cur.args.height = int(std::ceil(height));
    cur.args.projectWidth = width;
    cur.args.projectHeight = height;
    cur.byte = im.chosenDepth == kOfxBitDepthByte;
    {
        std::lock_guard<std::mutex> lock(im.stateMutex);
        im.current = &cur;
    }
    OfxPropertySetStruct& pr = im.effect.props;
    pr.setD(kOfxImageEffectPropProjectSize, width, 0);
    pr.setD(kOfxImageEffectPropProjectSize, height, 1);
    pr.setD(kOfxImageEffectPropProjectExtent, width, 0);
    pr.setD(kOfxImageEffectPropProjectExtent, height, 1);
    pr.setD(kOfxImageEffectPropFrameRate, fps);
    OfxPropertySetStruct in, res;
    in.setD(kOfxPropTime, time);
    in.setD(kOfxImageEffectPropRenderScale, 1.0, 0);
    in.setD(kOfxImageEffectPropRenderScale, 1.0, 1);
    for (int k = 0; k < 4; ++k)
        res.setD(kOfxImageEffectPropRegionOfDefinition, 0.0, k);
    const OfxStatus st
        = callPlugin(im.plugin, kOfxImageEffectActionGetRegionOfDefinition, &im.effect, &in, &res);
    {
        std::lock_guard<std::mutex> lock(im.stateMutex);
        im.current = nullptr;
    }
    if (st != kOfxStatOK)
        return false; // kOfxStatReplyDefault or a failure: the rectangle of the source
    for (int k = 0; k < 4; ++k)
        out[k] = res.getD(kOfxImageEffectPropRegionOfDefinition, 0.0, k);
    return out[2] > out[0] && out[3] > out[1];
}

} // namespace kk::ofx

