// SPDX-License-Identifier: GPL-3.0-or-later
// The OpenFX host without a GPU (docs/behavior-spec.md section 158): a small test plugin
// (tests/ofx/kk_test_ofx.cpp) is discovered through the probe program, its parameters become
// Kinoklip parameters, a keyframed value reaches the plugin at the right time, the plugin's
// threading flag and the multi-thread suite work, a missing plugin keeps its data, and a plugin
// that crashes or hangs while loading goes on the bad-plugin list without taking the test down.
#include "core/EffectRegistry.h"
#include "core/Serialization.h"
#include "core/TimelineData.h"
#include "ofx/OfxHost.h"
#include "plugins/OfxRegistry.h"
#include "ui/dialogs/PluginsPage.h"

#include <QDir>
#include <QListWidget>
#include <QPushButton>
#include <QTextBrowser>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <dlfcn.h>
#include <future>

using namespace kk;
using namespace kk::plugins;

namespace {

const QString kGain = QStringLiteral("ofx:org.kinoklip.test.Gain");

QString bundleOf(const QString& name)
{
    return QStringLiteral(KK_OFX_TEST_DIR) + QLatin1Char('/') + name + QStringLiteral(".ofx.bundle");
}

/// A folder holding links to some of the test bundles (what a plugin folder looks like).
void linkBundles(const QString& dir, const QStringList& names)
{
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    for (const QString& n : names)
        QVERIFY(QFile::link(bundleOf(n), dir + QLatin1Char('/') + n + QStringLiteral(".ofx.bundle")));
}

template<typename T>
T plugSymbol(const char* name)
{
    // the test plugin is loaded already (RTLD_NOLOAD): its own counters
    void* h = dlopen(ofx::bundleBinary(bundleOf(QStringLiteral("KkTest"))).toLocal8Bit().constData(),
        RTLD_NOW | RTLD_NOLOAD);
    return h ? reinterpret_cast<T>(dlsym(h, name)) : nullptr;
}

ofx::ParamValue number(double v)
{
    ofx::ParamValue p;
    p.v = {v};
    return p;
}

struct Picture
{
    int w, h;
    std::vector<float> px;
    Picture(int width, int height)
        : w(width)
        , h(height)
        , px(size_t(width) * size_t(height) * 4)
    {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float* p = &px[(size_t(y) * w + x) * 4];
                p[0] = 0.2f;
                p[1] = 0.4f;
                p[2] = 0.6f;
                p[3] = 1.0f;
            }
    }
};

ofx::RenderArgs argsFor(const Picture& pic, double time = 0, double scale = 1.0)
{
    ofx::RenderArgs a;
    a.time = time;
    a.width = pic.w;
    a.height = pic.h;
    a.scale = scale;
    a.projectWidth = pic.w / scale;
    a.projectHeight = pic.h / scale;
    return a;
}

} // namespace

class TestOfx : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<ofx::Instance> m_inst;

    void useGoodPlugin()
    {
        OfxRegistry& reg = OfxRegistry::instance();
        const QString dir = m_dir.filePath(QStringLiteral("good"));
        linkBundles(dir, {QStringLiteral("KkTest")});
        reg.setSearchDirs({dir});
        reg.setCacheFile(m_dir.filePath(QStringLiteral("cache-good.json")));
        QFile::remove(m_dir.filePath(QStringLiteral("cache-good.json")));
        reg.reload();
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_ofx"));
        QSettings().clear();
        QVERIFY(m_dir.isValid());
        OfxRegistry& reg = OfxRegistry::instance();
        reg.setProbeProgram(QStringLiteral(KK_OFX_PROBE_PATH));
        reg.setProbeTimeoutMs(20000);
        useGoodPlugin();
    }

    void init() { OfxRegistry::instance().setProbeProgram(QStringLiteral(KK_OFX_PROBE_PATH)); }

    // ---- discovery and description ----

    void describeFindsThePluginAndItsParameters()
    {
        const ofx::BundleInfo b = ofx::describeBundle(bundleOf(QStringLiteral("KkTest")));
        QVERIFY2(b.error.isEmpty(), qPrintable(b.error));
        QCOMPARE(b.plugins.size(), 1);
        const ofx::PluginInfo& p = b.plugins[0];
        QCOMPARE(p.identifier, QStringLiteral("org.kinoklip.test.Gain"));
        QCOMPARE(p.label, QStringLiteral("Kinoklip Test Gain"));
        QCOMPARE(p.group, QStringLiteral("Kinoklip Test"));
        QCOMPARE(p.threadSafety, QStringLiteral("FullySafe"));
        QCOMPARE(p.pixelDepths, QStringList{QStringLiteral("Float")});
        QCOMPARE(p.major, 1);
        QCOMPARE(p.minor, 2);
        const ofx::ContextInfo* ctx = p.usableContext();
        QVERIFY(ctx);
        QCOMPARE(ctx->context, QStringLiteral("Filter"));
        QStringList names;
        for (const ofx::ParamInfo& pi : ctx->params)
            names << pi.name + QLatin1Char(':') + pi.type;
        QCOMPARE(names,
            (QStringList {QStringLiteral("gain:Double"), QStringLiteral("invert:Boolean"),
                QStringLiteral("mode:Choice"), QStringLiteral("offset:Double2D"),
                QStringLiteral("tint:RGB"), QStringLiteral("steps:Integer"),
                QStringLiteral("note:String")}));
        const ofx::ParamInfo& gain = ctx->params[0];
        QCOMPARE(gain.def, QVector<double>{1.0});
        QCOMPARE(gain.max, QVector<double>{10.0});
        QVERIFY(gain.animates);
        QCOMPARE(ctx->params[2].choiceLabels, (QStringList {QStringLiteral("Linear"), QStringLiteral("Squared")}));
        // the JSON the probe writes and the cache keeps loses nothing
        QCOMPARE(ofx::BundleInfo::fromJson(b.toJson()).toJson(), b.toJson());
    }

    void theRegistryListsThePluginAsAnEffect()
    {
        useGoodPlugin();
        const EffectDescriptor* d = EffectRegistry::instance().find(kGain);
        QVERIFY(d);
        QVERIFY(!d->unavailable);
        QCOMPARE(d->name, QStringLiteral("Kinoklip Test Gain"));
        QCOMPARE(d->group, QString::fromLatin1(effectgroups::OpenFX));
        QCOMPARE(EffectRegistry::groupName(d->group), QStringLiteral("OpenFX"));
        QCOMPARE(d->category, EffectCategory::VideoEffect);
        QStringList keys;
        for (const ParamDescriptor& p : d->params)
            keys << p.key;
        QCOMPARE(keys,
            (QStringList {QStringLiteral("gain"), QStringLiteral("invert"), QStringLiteral("mode"),
                QStringLiteral("offset_x"), QStringLiteral("offset_y"), QStringLiteral("tint"),
                QStringLiteral("steps"), QStringLiteral("note")}));
        const ParamDescriptor* gain = d->param(QStringLiteral("gain"));
        QCOMPARE(gain->type, ParamType::Double);
        QVERIFY(gain->animatable);
        QCOMPARE(gain->def, 1.0);
        QCOMPARE(gain->max, 10.0);
        QCOMPARE(d->param(QStringLiteral("invert"))->type, ParamType::Bool);
        const ParamDescriptor* mode = d->param(QStringLiteral("mode"));
        QCOMPARE(mode->type, ParamType::Choice);
        QCOMPARE(mode->defStatic.toString(), QStringLiteral("Linear"));
        QCOMPARE(d->param(QStringLiteral("tint"))->type, ParamType::Color);
        QCOMPARE(d->param(QStringLiteral("steps"))->type, ParamType::Int);
        QCOMPARE(d->param(QStringLiteral("note"))->defStatic.toString(), QStringLiteral("hello"));
        // a new effect of this type has the plugin's defaults
        const EffectInstance fx = EffectRegistry::instance().createEffect(kGain, 5);
        QCOMPARE(fx.type, kGain);
        QCOMPARE(fx.params.value(QStringLiteral("gain"), 0, -1.0), 1.0);
    }

    // ---- values the plugin sees ----

    void parametersConvertToWhatThePluginExpects()
    {
        const auto plugin = OfxRegistry::instance().plugin(kGain);
        QVERIFY(plugin);
        ParamSet set = EffectRegistry::instance().defaultParams(kGain);
        set.setBase(QStringLiteral("gain"), 25.0); // above the maximum: clamped
        set.setStatic(QStringLiteral("mode"), QStringLiteral("Squared"));
        set.setStatic(QStringLiteral("invert"), true);
        set.setStatic(QStringLiteral("tint"), QColor(255, 0, 128));
        auto find = [&](const QString& name) -> const OfxParamMap& {
            for (const OfxParamMap& m : plugin->params)
                if (m.info.name == name)
                    return m;
            static OfxParamMap none;
            return none;
        };
        QCOMPARE(OfxRegistry::valueFor(find(QStringLiteral("gain")), set, 0, 100, 50).v, QVector<double>{10.0});
        QCOMPARE(OfxRegistry::valueFor(find(QStringLiteral("mode")), set, 0, 100, 50).v, QVector<double>{1.0});
        QCOMPARE(OfxRegistry::valueFor(find(QStringLiteral("invert")), set, 0, 100, 50).v, QVector<double>{1.0});
        const QVector<double> tint = OfxRegistry::valueFor(find(QStringLiteral("tint")), set, 0, 100, 50).v;
        QCOMPARE(tint.size(), 3);
        QCOMPARE(tint[0], 1.0);
        QCOMPARE(tint[1], 0.0);
        QVERIFY(std::abs(tint[2] - 128.0 / 255.0) < 1e-6);
        QCOMPARE(OfxRegistry::valueFor(find(QStringLiteral("note")), set, 0, 100, 50).s, QStringLiteral("hello"));
        QCOMPARE(OfxRegistry::valueFor(find(QStringLiteral("steps")), set, 0, 100, 50).v, QVector<double>{1.0});
    }

    // ---- rendering ----

    void renderingAppliesTheGainAndKeepsAlpha()
    {
        const auto plugin = OfxRegistry::instance().plugin(kGain);
        QVERIFY(plugin);
        QString error;
        m_inst = ofx::Instance::create(plugin->bundleInfo(), plugin->info.index, &error);
        QVERIFY2(m_inst, qPrintable(error));
        QVERIFY(m_inst->setValue(QStringLiteral("gain"), number(0.5)));
        QVERIFY(!m_inst->setValue(QStringLiteral("nope"), number(1)));
        Picture src(4, 2);
        std::vector<float> dst(src.px.size(), -1.f);
        QCOMPARE(m_inst->render(argsFor(src), src.px.data(), dst.data(), &error), ofx::RenderResult::Rendered);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        for (size_t i = 0; i < dst.size(); i += 4) {
            QVERIFY(std::abs(dst[i] - 0.1f) < 1e-6f);
            QVERIFY(std::abs(dst[i + 1] - 0.2f) < 1e-6f);
            QVERIFY(std::abs(dst[i + 2] - 0.3f) < 1e-6f);
            QCOMPARE(dst[i + 3], 1.0f);
        }
        // invert (premultiplied: alpha minus colour) and the squared curve
        QVERIFY(m_inst->setValue(QStringLiteral("gain"), number(1.0)));
        QVERIFY(m_inst->setValue(QStringLiteral("invert"), number(1.0)));
        QCOMPARE(m_inst->render(argsFor(src), src.px.data(), dst.data(), &error), ofx::RenderResult::Rendered);
        QVERIFY(std::abs(dst[0] - 0.8f) < 1e-6f);
        QVERIFY(std::abs(dst[2] - 0.4f) < 1e-6f);
        QVERIFY(m_inst->setValue(QStringLiteral("invert"), number(0.0)));
        QVERIFY(m_inst->setValue(QStringLiteral("mode"), number(1.0)));
        QCOMPARE(m_inst->render(argsFor(src), src.px.data(), dst.data(), &error), ofx::RenderResult::Rendered);
        QVERIFY(std::abs(dst[2] - 0.36f) < 1e-6f);
        // the plugin was told about the edits (instance changed action)
        QVERIFY(plugSymbol<int (*)()>("kkTestChangedCount")() >= 3);
    }

    void aTransitionContextPluginIsATransitionOfTheEffectsLibrary()
    {
        OfxRegistry& reg = OfxRegistry::instance();
        const QString dir = m_dir.filePath(QStringLiteral("transition"));
        linkBundles(dir, {QStringLiteral("KkTestTransition")});
        reg.setSearchDirs({dir});
        reg.setCacheFile(m_dir.filePath(QStringLiteral("cache-transition.json")));
        QFile::remove(m_dir.filePath(QStringLiteral("cache-transition.json")));
        reg.reload();
        const QString id = QStringLiteral("ofx:org.kinoklip.test.Dissolve");
        const auto plugin = reg.plugin(id);
        QVERIFY(plugin);
        QVERIFY(plugin->info.isTransition());
        const EffectDescriptor* d = EffectRegistry::instance().find(id);
        QVERIFY(d);
        QCOMPARE(int(d->category), int(EffectCategory::Transition));
        // the progress is the host's: only the plugin's own controls are listed
        QCOMPARE(d->params.size(), 1);
        QCOMPARE(d->params[0].key, QString("softness"));
        // the two pictures and the progress reach the plugin
        QString error;
        auto inst = ofx::Instance::create(plugin->bundleInfo(), plugin->info.index, &error);
        QVERIFY2(inst, qPrintable(error));
        Picture from(4, 2), to(4, 2);
        for (size_t i = 0; i < to.px.size(); i += 4) {
            to.px[i] = 1.0f;
            to.px[i + 1] = 0.0f;
            to.px[i + 2] = 0.0f;
        }
        ofx::ParamValue progress;
        progress.v = {0.25};
        QVERIFY(inst->setValue(QStringLiteral("Transition"), progress));
        std::vector<float> dst(from.px.size(), -1.f);
        QCOMPARE(inst->render(argsFor(from), from.px.data(), dst.data(), &error, to.px.data()),
            ofx::RenderResult::Rendered);
        QVERIFY(std::abs(dst[0] - (0.2f * 0.75f + 0.25f)) < 1e-6f); // red: from 0.2, to 1.0
        QVERIFY(std::abs(dst[1] - 0.4f * 0.75f) < 1e-6f); // green: from 0.4, to 0
        useGoodPlugin();
    }

    void theRegionOfDefinitionTellsHowFarAnEffectGrowsPastTheClip()
    {
        const auto plugin = OfxRegistry::instance().plugin(kGain);
        QVERIFY(plugin);
        QString error;
        auto inst = ofx::Instance::create(plugin->bundleInfo(), plugin->info.index, &error);
        QVERIFY2(inst, qPrintable(error));
        double rod[4] = {};
        // the default: the plugin has no opinion, the output is the clip's rectangle
        QVERIFY(!inst->regionOfDefinition(0, 25, 200, 100, rod));
        QVERIFY(inst->setValue(QStringLiteral("steps"), number(4)));
        QVERIFY(inst->regionOfDefinition(0, 25, 200, 100, rod));
        QCOMPARE(rod[0], -30.0);
        QCOMPARE(rod[1], -30.0);
        QCOMPARE(rod[2], 230.0);
        QCOMPARE(rod[3], 130.0);
    }

    void aKeyframedParameterIsReadAtTheFrameBeingRendered()
    {
        const auto plugin = OfxRegistry::instance().plugin(kGain);
        QString error;
        auto inst = ofx::Instance::create(plugin->bundleInfo(), plugin->info.index, &error);
        QVERIFY2(inst, qPrintable(error));
        ParamSet set = EffectRegistry::instance().defaultParams(kGain);
        AnimatedValue& gain = set.animated(QStringLiteral("gain"), 1.0);
        gain.setKey(0, 0.0);
        gain.setKey(10, 1.0);
        const auto& maps = plugin->params;
        inst->setEvaluator([&](const QString& name, double time, ofx::ParamValue* out) {
            for (const OfxParamMap& m : maps)
                if (m.info.name == name) {
                    *out = OfxRegistry::valueFor(m, set, time, 4, 2);
                    return true;
                }
            return false;
        });
        Picture src(4, 2);
        std::vector<float> dst(src.px.size());
        for (const int frame : {0, 5, 10}) {
            QCOMPARE(inst->render(argsFor(src, frame), src.px.data(), dst.data(), &error),
                frame == 10 ? ofx::RenderResult::Identity : ofx::RenderResult::Rendered);
            if (frame != 10)
                QVERIFY2(std::abs(dst[0] - 0.2f * float(frame) / 10.f) < 1e-5f, qPrintable(QString::number(dst[0])));
        }
    }

    void aPluginThatChangesNothingIsReportedAsIdentity()
    {
        const auto plugin = OfxRegistry::instance().plugin(kGain);
        QString error;
        auto inst = ofx::Instance::create(plugin->bundleInfo(), plugin->info.index, &error);
        QVERIFY(inst);
        Picture src(8, 8);
        std::vector<float> dst(src.px.size(), -7.f);
        const int before = plugSymbol<int (*)()>("kkTestRenderCount")();
        QCOMPARE(inst->render(argsFor(src), src.px.data(), dst.data(), &error), ofx::RenderResult::Identity);
        QCOMPARE(dst[0], -7.f); // not written
        QCOMPARE(plugSymbol<int (*)()>("kkTestRenderCount")(), before); // and not rendered
    }

    void renderScaleAndSizeReachThePlugin()
    {
        const auto plugin = OfxRegistry::instance().plugin(kGain);
        QString error;
        auto inst = ofx::Instance::create(plugin->bundleInfo(), plugin->info.index, &error);
        QVERIFY(inst);
        QVERIFY(inst->setValue(QStringLiteral("gain"), number(2.0)));
        Picture src(16, 9);
        std::vector<float> dst(src.px.size());
        QCOMPARE(inst->render(argsFor(src, 3, 0.5), src.px.data(), dst.data(), &error), ofx::RenderResult::Rendered);
        QCOMPARE(plugSymbol<double (*)()>("kkTestLastScale")(), 0.5);
        QCOMPARE(plugSymbol<int (*)()>("kkTestLastWidth")(), 16);
    }

    void theMultiThreadSuiteSplitsTheWork()
    {
        if (QThread::idealThreadCount() < 2)
            QSKIP("one CPU");
        const auto plugin = OfxRegistry::instance().plugin(kGain);
        QString error;
        auto inst = ofx::Instance::create(plugin->bundleInfo(), plugin->info.index, &error);
        QVERIFY(inst);
        QVERIFY(inst->setValue(QStringLiteral("gain"), number(0.5)));
        Picture src(64, 64);
        std::vector<float> dst(src.px.size());
        QCOMPARE(inst->render(argsFor(src), src.px.data(), dst.data(), &error), ofx::RenderResult::Rendered);
        QVERIFY(plugSymbol<int (*)()>("kkTestThreadsUsed")() >= 2);
        for (size_t i = 0; i < dst.size(); i += 4)
            QVERIFY(std::abs(dst[i] - 0.1f) < 1e-6f); // every row was done, none twice
    }

    void aFullySafePluginRendersOnSeveralThreadsAtOnce()
    {
        const auto plugin = OfxRegistry::instance().plugin(kGain);
        std::vector<std::future<bool>> jobs;
        for (int t = 0; t < 4; ++t) {
            jobs.push_back(std::async(std::launch::async, [plugin, t] {
                QString error;
                auto inst = ofx::Instance::create(plugin->bundleInfo(), plugin->info.index, &error);
                if (!inst)
                    return false;
                inst->setValue(QStringLiteral("gain"), number(0.3 * (t + 1)));
                Picture src(32, 32);
                std::vector<float> dst(src.px.size());
                for (int k = 0; k < 20; ++k) {
                    if (inst->render(argsFor(src, k), src.px.data(), dst.data(), &error)
                        != ofx::RenderResult::Rendered)
                        return false;
                    if (std::abs(dst[0] - 0.2f * 0.3f * float(t + 1)) > 1e-5f)
                        return false;
                }
                return true;
            }));
        }
        for (auto& j : jobs)
            QVERIFY(j.get());
    }

    // ---- bad plugins ----

    void aPluginThatCrashesOrHangsWhileLoadingIsOnTheBadList()
    {
        OfxRegistry& reg = OfxRegistry::instance();
        const QString dir = m_dir.filePath(QStringLiteral("bad"));
        linkBundles(dir, {QStringLiteral("KkTest"), QStringLiteral("KkTestCrash"), QStringLiteral("KkTestHang")});
        reg.setSearchDirs({dir});
        reg.setCacheFile(m_dir.filePath(QStringLiteral("cache-bad.json")));
        QFile::remove(m_dir.filePath(QStringLiteral("cache-bad.json")));
        reg.setProbeTimeoutMs(1500);
        reg.reload();
        reg.setProbeTimeoutMs(20000);
        const OfxBundleEntry* crash = reg.bundle(dir + QStringLiteral("/KkTestCrash.ofx.bundle"));
        const OfxBundleEntry* hang = reg.bundle(dir + QStringLiteral("/KkTestHang.ofx.bundle"));
        const OfxBundleEntry* good = reg.bundle(dir + QStringLiteral("/KkTest.ofx.bundle"));
        QVERIFY(crash && hang && good);
        QVERIFY(crash->bad);
        QVERIFY2(crash->reason.contains(QStringLiteral("crashed")), qPrintable(crash->reason));
        QVERIFY(hang->bad);
        QVERIFY2(hang->reason.contains(QStringLiteral("hang")), qPrintable(hang->reason));
        QVERIFY(!good->bad);
        // the good plugin still works
        QVERIFY(reg.plugin(kGain));
        QVERIFY(!EffectRegistry::instance().find(kGain)->unavailable);
    }

    void theScanIsCachedAndRetryProbesAgain()
    {
        OfxRegistry& reg = OfxRegistry::instance();
        const QString dir = m_dir.filePath(QStringLiteral("bad"));
        const QString crashPath = dir + QStringLiteral("/KkTestCrash.ofx.bundle");
        // Without a probe program a reload still knows everything: it comes from the cache.
        reg.setProbeProgram(QStringLiteral("/nonexistent/probe"));
        reg.reload();
        QVERIFY(reg.bundle(crashPath)->bad);
        QVERIFY(reg.bundle(crashPath)->reason.contains(QStringLiteral("crashed")));
        QVERIFY(reg.plugin(kGain));
        // Retry runs the probe again: now it cannot start, so the reason changes.
        reg.retry(crashPath);
        QVERIFY(reg.bundle(crashPath)->bad);
        QVERIFY2(reg.bundle(crashPath)->reason.contains(QStringLiteral("probe")),
            qPrintable(reg.bundle(crashPath)->reason));
        reg.setProbeProgram(QStringLiteral(KK_OFX_PROBE_PATH));
        reg.retry(crashPath);
        QVERIFY(reg.bundle(crashPath)->reason.contains(QStringLiteral("crashed")));
    }

    void aCrashWhileTheEditorHadThePluginLoadedPutsItOnTheBadList()
    {
        OfxRegistry& reg = OfxRegistry::instance();
        useGoodPlugin();
        const QString path = m_dir.filePath(QStringLiteral("good/KkTest.ofx.bundle"));
        QVERIFY(!reg.bundle(path)->bad);
        reg.markLoading(path); // the editor started to load it ... and died
        reg.resetForTesting(); // the next start
        reg.load();
        QVERIFY(reg.bundle(path)->bad);
        QVERIFY(reg.bundle(path)->reason.contains(QStringLiteral("crashed")));
        QVERIFY(!reg.plugin(kGain));
        reg.retry(path);
        QVERIFY(!reg.bundle(path)->bad);
        QVERIFY(reg.plugin(kGain));
        // a plugin that came through its first frame leaves no mark
        reg.markLoading(path);
        reg.markLoaded(path);
        reg.resetForTesting();
        reg.load();
        QVERIFY(!reg.bundle(path)->bad);
    }

    void thePluginsPageListsBundlesAndTheBadOnesWithRetry()
    {
        OfxRegistry& reg = OfxRegistry::instance();
        const QString dir = m_dir.filePath(QStringLiteral("page"));
        linkBundles(dir, {QStringLiteral("KkTest"), QStringLiteral("KkTestCrash")});
        reg.setSearchDirs({dir});
        reg.setCacheFile(m_dir.filePath(QStringLiteral("cache-page.json")));
        QFile::remove(m_dir.filePath(QStringLiteral("cache-page.json")));
        reg.reload();
        PluginsPage page;
        auto* list = page.findChild<QListWidget*>(QStringLiteral("PluginsList"));
        auto* details = page.findChild<QTextBrowser*>(QStringLiteral("PluginDetails"));
        auto* retry = page.findChild<QPushButton*>(QStringLiteral("OfxRetry"));
        QVERIFY(list && details && retry);
        QListWidgetItem* bad = nullptr;
        QListWidgetItem* good = nullptr;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->text().contains(QStringLiteral("KkTestCrash")))
                bad = list->item(i);
            else if (list->item(i)->text().contains(QStringLiteral("KkTest.ofx")))
                good = list->item(i);
        }
        QVERIFY(bad && good);
        QVERIFY(bad->text().contains(QStringLiteral("bad plugin")));
        QVERIFY2(good->text().contains(QStringLiteral("1 effects")), qPrintable(good->text()));
        list->setCurrentItem(bad);
        QVERIFY(retry->isEnabled());
        QVERIFY2(details->toPlainText().contains(QStringLiteral("crashed while loading")),
            qPrintable(details->toPlainText()));
        QVERIFY(details->toPlainText().contains(QStringLiteral("bad plugin")));
        list->setCurrentItem(good);
        QVERIFY(!retry->isEnabled());
        QVERIFY(details->toPlainText().contains(QStringLiteral("Kinoklip Test Gain")));
        QVERIFY(details->toPlainText().contains(QStringLiteral("org.kinoklip.test.Gain")));
        // Retry probes the bad bundle again: it is still bad, and the page is rebuilt
        list->setCurrentItem(bad);
        retry->click(); // probes in the background
        reg.waitForScan();
        QVERIFY(reg.bundle(dir + QStringLiteral("/KkTestCrash.ofx.bundle"))->bad);
    }

    void aBundleWithoutABinaryForThisMachineIsReported()
    {
        OfxRegistry& reg = OfxRegistry::instance();
        const QString dir = m_dir.filePath(QStringLiteral("empty"));
        QDir().mkpath(dir + QStringLiteral("/Nothing.ofx.bundle/Contents/Win64"));
        reg.setSearchDirs({dir});
        reg.reload();
        QCOMPARE(reg.bundles().size(), 1);
        QVERIFY(reg.bundles()[0].bad);
        QVERIFY(reg.bundles()[0].reason.contains(QStringLiteral("no binary")));
    }

    // ---- missing plugin ----

    void aMissingPluginKeepsTheEffectDataAndBringsItBack()
    {
        OfxRegistry& reg = OfxRegistry::instance();
        useGoodPlugin();
        EffectInstance fx = EffectRegistry::instance().createEffect(kGain, 21);
        fx.params.setBase(QStringLiteral("gain"), 3.5);
        fx.params.animated(QStringLiteral("gain"), 3.5).setKey(10, 2.0);
        fx.params.setStatic(QStringLiteral("mode"), QStringLiteral("Squared"));
        ClipData clip;
        clip.id = 1;
        clip.kind = ClipKind::Solid;
        clip.duration = 50;
        clip.effects.append(fx);
        const QJsonObject json = serialization::clipToJson(clip);
        // The plugin folder is gone (another computer).
        reg.setSearchDirs({});
        reg.reload();
        const EffectDescriptor* d = EffectRegistry::instance().find(kGain);
        QVERIFY(!d || d->unavailable);
        const ClipData back = serialization::clipFromJson(json);
        QCOMPARE(back.effects.size(), 1);
        QCOMPARE(back.effects[0].type, kGain);
        QCOMPARE(back.effects[0].params.value(QStringLiteral("gain"), 10, -1.0), 2.0);
        QCOMPARE(back.effects[0].params.statics.value(QStringLiteral("mode")).toString(), QStringLiteral("Squared"));
        QCOMPARE(serialization::clipToJson(back), json);
        QVERIFY(!reg.plugin(kGain));
        // and it is back when the plugin is
        useGoodPlugin();
        QVERIFY(!EffectRegistry::instance().find(kGain)->unavailable);
        // switched off as a whole: unavailable again, nothing lost
        reg.setEnabled(false);
        QVERIFY(EffectRegistry::instance().find(kGain)->unavailable);
        QVERIFY(!reg.plugin(kGain));
        reg.setEnabled(true);
        QVERIFY(!EffectRegistry::instance().find(kGain)->unavailable);
    }

    // ---- a real pack (skipped when openfx-misc is not built: scripts/build_openfx_misc.sh) ----

    static QString miscDir()
    {
        QStringList candidates;
        if (qEnvironmentVariableIsSet("KK_OFX_MISC_DIR"))
            candidates << qEnvironmentVariable("KK_OFX_MISC_DIR");
        candidates << QStringLiteral("/usr/OFX/Plugins") << QStringLiteral("/usr/local/OFX/Plugins");
        for (const QString& c : candidates) {
            if (QFileInfo(c + QStringLiteral("/Misc.ofx.bundle")).isDir())
                return c;
        }
        return {};
    }

    static QStringList miscEffects()
    {
        QStringList ids;
        for (const OfxPluginEntry& p : OfxRegistry::instance().allPlugins()) {
            if (OfxRegistry::instance().plugin(p.fullId) && !ids.contains(p.fullId))
                ids << p.fullId;
        }
        return ids;
    }

    void openfxMiscIsDescribedAndItsEffectsAreListed()
    {
        const QString dir = miscDir();
        if (dir.isEmpty())
            QSKIP("openfx-misc is not built here (scripts/build_openfx_misc.sh, KK_OFX_MISC_DIR)");
        OfxRegistry& reg = OfxRegistry::instance();
        reg.setSearchDirs({dir});
        reg.setCacheFile(m_dir.filePath(QStringLiteral("cache-misc.json")));
        QFile::remove(m_dir.filePath(QStringLiteral("cache-misc.json")));
        reg.setProbeTimeoutMs(120000);
        reg.reload();
        const OfxBundleEntry* misc
            = reg.bundle(QFileInfo(dir + QStringLiteral("/Misc.ofx.bundle")).absoluteFilePath());
        QVERIFY(misc);
        QVERIFY2(!misc->bad, qPrintable(misc->reason));
        const QStringList effects = miscEffects();
        qInfo("openfx-misc: %d plugins in the bundle, %d effects offered",
            int(misc->info.plugins.size()), int(effects.size()));
        QVERIFY2(effects.size() >= 100, qPrintable(QString::number(effects.size())));
        for (const char* id : {"GradePlugin", "GammaPlugin", "Invert", "MultiplyPlugin",
                 "SaturationPlugin", "TransformPlugin", "Threshold"})
            QVERIFY2(effects.contains(QStringLiteral("ofx:net.sf.openfx.") + QLatin1String(id)), id);
    }

    void openfxMiscEffectsRenderThroughTheHost()
    {
        if (miscDir().isEmpty())
            QSKIP("openfx-misc is not built here");
        OfxRegistry& reg = OfxRegistry::instance();
        const auto multiply = reg.plugin(QStringLiteral("ofx:net.sf.openfx.MultiplyPlugin"));
        QVERIFY(multiply);
        QString error;
        auto inst = ofx::Instance::create(multiply->bundleInfo(), multiply->info.index, &error);
        QVERIFY2(inst, qPrintable(error));
        ofx::ParamValue half;
        half.v = {0.5, 0.5, 0.5, 1.0};
        QVERIFY(inst->setValue(QStringLiteral("value"), half));
        Picture src(32, 16);
        std::vector<float> dst(src.px.size(), -1.f);
        QCOMPARE(inst->render(argsFor(src), src.px.data(), dst.data(), &error),
            ofx::RenderResult::Rendered);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        for (size_t i = 0; i < dst.size(); i += 4) {
            QVERIFY2(std::abs(dst[i] - 0.1f) < 1e-4f, qPrintable(QString::number(dst[i])));
            QVERIFY(std::abs(dst[i + 1] - 0.2f) < 1e-4f);
            QVERIFY(std::abs(dst[i + 2] - 0.3f) < 1e-4f);
            QVERIFY(std::abs(dst[i + 3] - 1.0f) < 1e-4f);
        }
        // Saturation 0 makes the colour grey
        const auto sat = reg.plugin(QStringLiteral("ofx:net.sf.openfx.SaturationPlugin"));
        QVERIFY(sat);
        auto satInst = ofx::Instance::create(sat->bundleInfo(), sat->info.index, &error);
        QVERIFY2(satInst, qPrintable(error));
        QVERIFY(satInst->setValue(QStringLiteral("saturation"), number(0.0)));
        QCOMPARE(satInst->render(argsFor(src), src.px.data(), dst.data(), &error),
            ofx::RenderResult::Rendered);
        QVERIFY(std::abs(dst[0] - dst[1]) < 1e-4f && std::abs(dst[1] - dst[2]) < 1e-4f);
        // At half render scale the plugin gets the scale and still fills the picture.
        QVERIFY(satInst->setValue(QStringLiteral("saturation"), number(0.5)));
        Picture small(16, 8);
        std::vector<float> out(small.px.size(), -1.f);
        QCOMPARE(satInst->render(argsFor(small, 0, 0.5), small.px.data(), out.data(), &error),
            ofx::RenderResult::Rendered);
        QVERIFY(out[0] >= 0.f);
    }

    void everyOpenfxMiscEffectRendersOnceWithItsDefaults()
    {
        if (miscDir().isEmpty())
            QSKIP("openfx-misc is not built here");
        OfxRegistry& reg = OfxRegistry::instance();
        Picture src(40, 24);
        for (size_t y = 0; y < size_t(src.h); ++y)
            for (size_t x = 0; x < size_t(src.w); ++x)
                src.px[(y * size_t(src.w) + x) * 4] = float(x) / float(src.w);
        QStringList failures;
        int rendered = 0;
        for (const QString& id : miscEffects()) {
            const auto p = reg.plugin(id);
            QString error;
            auto inst = ofx::Instance::create(p->bundleInfo(), p->info.index, &error);
            if (!inst) {
                failures << id + QStringLiteral(": ") + error;
                continue;
            }
            std::vector<float> dst(src.px.size(), 0.f);
            const ofx::RenderResult r = inst->render(argsFor(src, 3), src.px.data(), dst.data(), &error);
            if (r == ofx::RenderResult::Failed)
                failures << id + QStringLiteral(": ") + error;
            else
                ++rendered;
        }
        qInfo("openfx-misc: %d effects rendered with their defaults, %d did not", rendered,
            int(failures.size()));
        if (!failures.isEmpty())
            qInfo("did not render: %s", qPrintable(failures.join(QStringLiteral("; "))));
        // A few effects need what this host does not give (a second input, a region larger than the clip).
        QVERIFY2(failures.size() <= rendered / 10, qPrintable(failures.join(QLatin1Char('\n'))));
    }

    // ---- G'MIC-OFX (skipped when it is not built: scripts/build_gmic_ofx.sh, KK_OFX_GMIC_DIR) ----

    static QString gmicDir()
    {
        QStringList candidates;
        if (qEnvironmentVariableIsSet("KK_OFX_GMIC_DIR"))
            candidates << qEnvironmentVariable("KK_OFX_GMIC_DIR");
        candidates << QStringLiteral("/usr/OFX/Plugins") << QStringLiteral("/usr/local/OFX/Plugins");
        for (const QString& c : candidates) {
            if (QFileInfo(c + QStringLiteral("/GMIC.ofx.bundle")).isDir())
                return c;
        }
        return {};
    }

    void gmicIsDescribedAndAFewFiltersRender()
    {
        const QString dir = gmicDir();
        if (dir.isEmpty())
            QSKIP("G'MIC-OFX is not built here (scripts/build_gmic_ofx.sh, KK_OFX_GMIC_DIR)");
        OfxRegistry& reg = OfxRegistry::instance();
        reg.setSearchDirs({dir});
        reg.setCacheFile(m_dir.filePath(QStringLiteral("cache-gmic.json")));
        QFile::remove(m_dir.filePath(QStringLiteral("cache-gmic.json")));
        reg.setProbeTimeoutMs(300000);
        reg.reload();
        const OfxBundleEntry* gmic
            = reg.bundle(QFileInfo(dir + QStringLiteral("/GMIC.ofx.bundle")).absoluteFilePath());
        QVERIFY(gmic);
        QVERIFY2(!gmic->bad, qPrintable(gmic->reason));
        const QStringList effects = miscEffects();
        qInfo("G'MIC-OFX: %d plugins in the bundle, %d effects offered",
            int(gmic->info.plugins.size()), int(effects.size()));
        QVERIFY2(effects.size() >= 20, qPrintable(QString::number(effects.size())));
        // a few filters run with their defaults and give a finite picture of the same size
        Picture src(64, 48);
        for (size_t y = 0; y < size_t(src.h); ++y)
            for (size_t x = 0; x < size_t(src.w); ++x)
                src.px[(y * size_t(src.w) + x) * 4] = float(x) / float(src.w);
        int rendered = 0, tried = 0;
        QStringList failures;
        for (const QString& id : effects) {
            if (tried >= 6)
                break;
            const auto p = reg.plugin(id);
            QString error;
            auto inst = ofx::Instance::create(p->bundleInfo(), p->info.index, &error);
            if (!inst) {
                failures << id + QStringLiteral(": ") + error;
                continue;
            }
            ++tried;
            std::vector<float> dst(src.px.size(), 0.f);
            const ofx::RenderResult r = inst->render(argsFor(src, 0), src.px.data(), dst.data(), &error);
            if (r == ofx::RenderResult::Failed) {
                failures << id + QStringLiteral(": ") + error;
                continue;
            }
            if (r == ofx::RenderResult::Rendered) {
                for (float v : dst)
                    QVERIFY2(std::isfinite(v), qPrintable(id));
            }
            ++rendered;
        }
        qInfo("G'MIC-OFX: %d of %d filters rendered; %s", rendered, tried,
            qPrintable(failures.join(QStringLiteral("; "))));
        QVERIFY2(rendered >= 3, qPrintable(failures.join(QLatin1Char('\n'))));
    }

    void theRenderCacheKeyChangesWithThePlugins()
    {
        OfxRegistry& reg = OfxRegistry::instance();
        useGoodPlugin();
        const QByteArray with = reg.contentKey();
        reg.setSearchDirs({});
        reg.reload();
        QVERIFY(reg.contentKey() != with);
        useGoodPlugin();
        QCOMPARE(reg.contentKey(), with);
    }
};

QTEST_MAIN(TestOfx)
#include "test_ofx.moc"
