// SPDX-License-Identifier: GPL-3.0-or-later
// Plugin effects and transitions on the GPU (docs/behavior-spec.md section 157): the example
// plugins draw exactly what the built-in effects they port draw (the preview rig, and the real
// compositor with keyframes), compile errors come back with the line, an edited shader shows after
// a reload, a missing or disabled plugin renders as bypass, and every example compiles for
// desktop OpenGL 3.3 and, where this machine has it, OpenGL ES 3.0. GPU cases skip without OpenGL.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "plugins/PluginRegistry.h"
#include "render/Compositor.h"
#include "render/PluginPreview.h"
#include "render/PluginShader.h"
#include "render/PluginValidate.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;
using namespace kk::render;
using kk::plugins::PluginRegistry;

namespace {

QString examplesDir()
{
    return QFileInfo(QStringLiteral(__FILE__)).absolutePath()
        + QStringLiteral("/../../plugins/examples");
}

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

/// Largest channel difference between two pictures of the same size.
int maxDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
        }
    }
    return worst;
}

QString bundledDir()
{
    return QFileInfo(QStringLiteral(__FILE__)).absolutePath()
        + QStringLiteral("/../../plugins/bundled");
}

/// Mean absolute channel difference of two pictures of the same size (0..255).
double meanDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255.0;
    double sum = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            sum += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q))
                + std::abs(qBlue(p) - qBlue(q)) + std::abs(qAlpha(p) - qAlpha(q));
        }
    }
    return sum / (4.0 * a.width() * a.height());
}

const QString kInvert = QStringLiteral("plugin:dev.kinoklip.invert/invert");
const QString kPosterize = QStringLiteral("plugin:dev.kinoklip.posterize/posterize");
const QString kDip = QStringLiteral("plugin:dev.kinoklip.dip-to-color/dip_to_color");

} // namespace

class TestPluginRender : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    PluginPreviewRig m_rig;
    bool m_gl = false;
    std::unique_ptr<Compositor> m_gpu;
    QImage m_chart;

    bool gpuReady() const { return m_gl && m_gpu && m_gpu->isGpu(); }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_pluginrender"));
        QSettings().clear();
        QVERIFY(m_dir.isValid());
        PluginRegistry& reg = PluginRegistry::instance();
        reg.setDirectories({}, m_dir.filePath(QStringLiteral("user")));
        reg.reload();
        for (const char* name :
            {"dev.kinoklip.invert", "dev.kinoklip.posterize", "dev.kinoklip.dip-to-color"}) {
            QString error;
            QVERIFY2(reg.install(examplesDir() + QLatin1Char('/') + QLatin1String(name), &error),
                qPrintable(error));
        }
        QString why;
        m_gl = m_rig.initialize(&why);
        if (!m_gl)
            qInfo("no OpenGL: %s", qPrintable(why));
        m_gpu = std::make_unique<Compositor>();
        m_chart = previewTestChart(QSize(320, 180));
    }

    void cleanupTestCase()
    {
        m_gpu.reset();
        m_rig.destroy();
    }

    void invertMatchesTheBuiltInByteForByte()
    {
        if (!m_gl)
            QSKIP("no OpenGL");
        for (const double amount : {100.0, 33.5, 60.0, 25.0, 1.0}) {
            ParamSet p;
            p.setBase(QStringLiteral("amount"), amount);
            QString why;
            const QImage builtin
                = m_rig.renderEffect(QStringLiteral("invert"), p, m_chart, 0, 25, &why);
            const QImage plugin = m_rig.renderEffect(kInvert, p, m_chart, 0, 25, &why);
            QVERIFY2(!builtin.isNull() && !plugin.isNull(), qPrintable(why));
            QVERIFY2(builtin != m_chart, "the effect did nothing");
            QCOMPARE(maxDifference(builtin, plugin), 0);
        }
    }

    void posterizeMatchesTheBuiltInByteForByte()
    {
        if (!m_gl)
            QSKIP("no OpenGL");
        for (const int levels : {2, 4, 6, 17, 64}) {
            ParamSet p;
            p.setBase(QStringLiteral("levels"), levels);
            QString why;
            const QImage builtin
                = m_rig.renderEffect(QStringLiteral("posterize"), p, m_chart, 0, 25, &why);
            const QImage plugin = m_rig.renderEffect(kPosterize, p, m_chart, 0, 25, &why);
            QVERIFY2(!builtin.isNull() && !plugin.isNull(), qPrintable(why));
            QCOMPARE(maxDifference(builtin, plugin), 0);
        }
    }

    void dipToColorMatchesTheBuiltInByteForByte()
    {
        if (!m_gl)
            QSKIP("no OpenGL");
        const QImage a = m_chart;
        QImage b = m_chart;
        b.invertPixels(QImage::InvertRgb);
        for (const QColor& color :
            {QColor(Qt::black), QColor(230, 30, 40), QColor(40, 80, 220, 128)}) {
            for (const double progress : {0.0, 0.2, 0.5, 0.51, 0.8, 1.0}) {
                ParamSet p;
                p.setStatic(QStringLiteral("color"), color);
                QString why;
                const QImage builtin = m_rig.renderTransition(
                    QStringLiteral("dip_to_color"), p, progress, a, b, &why);
                const QImage plugin = m_rig.renderTransition(kDip, p, progress, a, b, &why);
                QVERIFY2(!builtin.isNull() && !plugin.isNull(), qPrintable(why));
                QCOMPARE(maxDifference(builtin, plugin), 0);
            }
        }
    }

    void pluginEffectInTheCompositorMatchesWithKeyframes()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const auto build = [](const QString& type) {
            TimelineData tl = emptyTimeline(160, 90);
            ClipData clip = solidClip(tl, QColor(200, 100, 50), 0, 20);
            EffectInstance fx = EffectRegistry::instance().createEffect(type, tl.allocateId());
            // The amount moves from 20 to 100 over the clip: a keyframed plugin parameter.
            AnimatedValue& amount = fx.params.animated(QStringLiteral("amount"), 100);
            amount.setKey(0, 20);
            amount.setKey(19, 100);
            clip.effects.append(fx);
            addClip(tl, TrackType::Video, 0, std::move(clip));
            return tl;
        };
        for (const int frame : {0, 7, 12, 19}) {
            const QImage builtin = m_gpu->renderImage(snap(build(QStringLiteral("invert"))), frame);
            const QImage plugin = m_gpu->renderImage(snap(build(kInvert)), frame);
            QVERIFY(!builtin.isNull() && !plugin.isNull());
            QCOMPARE(maxDifference(builtin, plugin), 0);
        }
        // The keyframes made a difference between frames.
        QVERIFY(m_gpu->renderImage(snap(build(kInvert)), 0)
            != m_gpu->renderImage(snap(build(kInvert)), 19));
    }

    void missingOrDisabledPluginRendersAsBypass()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        PluginRegistry& reg = PluginRegistry::instance();
        const auto build = [](const QString& type) {
            TimelineData tl = emptyTimeline(160, 90);
            ClipData clip = solidClip(tl, QColor(200, 100, 50), 0, 10);
            if (!type.isEmpty())
                clip.effects.append(EffectRegistry::instance().createEffect(type, tl.allocateId()));
            addClip(tl, TrackType::Video, 0, std::move(clip));
            return tl;
        };
        const QImage plain = m_gpu->renderImage(snap(build({})), 0);
        const QImage inverted = m_gpu->renderImage(snap(build(kInvert)), 0);
        QVERIFY(plain != inverted);
        // Disabled: the effect stays on the clip and draws nothing.
        reg.setEnabled(QStringLiteral("dev.kinoklip.invert"), false);
        QCOMPARE(m_gpu->renderImage(snap(build(kInvert)), 0), plain);
        reg.setEnabled(QStringLiteral("dev.kinoklip.invert"), true);
        QCOMPARE(m_gpu->renderImage(snap(build(kInvert)), 0), inverted);
        // An id no plugin ever had: bypass, no crash.
        TimelineData tl = build({});
        tl.videoTracks[0].clips[0].effects.append(
            EffectRegistry::instance().createEffect(QStringLiteral("plugin:nobody.has/this"), 99));
        tl.videoTracks[0].clips[0].effects.last().type = QStringLiteral("plugin:nobody.has/this");
        QCOMPARE(m_gpu->renderImage(snap(tl), 0), plain);
    }

    void aShaderErrorComesBackWithItsLine()
    {
        const QString dir = m_dir.filePath(QStringLiteral("user/test.broken"));
        writeFile(dir + QStringLiteral("/plugin.json"),
            "{\"id\":\"test.broken\",\"name\":\"Broken\",\"version\":\"1.0\",\"author\":\"A\","
            "\"license\":\"MIT\",\"api\":1,"
            "\"effects\":[{\"id\":\"fx\",\"name\":\"Broken\",\"file\":\"fx.glsl\"}]}");
        writeFile(dir + QStringLiteral("/fx.glsl"),
            "// line 1\n"
            "vec4 effect(vec2 uv)\n"
            "{\n"
            "    return texture(uTex, uv) * undeclaredThing;\n"
            "}\n");
        PluginRegistry::instance().reload();
        // Static checks pass; the compile check names line 4 (when a context exists).
        const PluginValidation v = validatePluginFolder(dir, true);
        QVERIFY(v.problems.isEmpty() || !v.valid || v.items[0].desktop == QLatin1String("skipped"));
        if (m_gl) {
            QVERIFY(!v.valid);
            bool found = false;
            for (const plugins::Problem& p : v.problems)
                found |= p.line == 4 && p.message.contains("undeclaredThing");
            QVERIFY2(found,
                qPrintable(
                    v.problems.isEmpty() ? QStringLiteral("no problems") : v.problems[0].text()));
            // The renderer reports it too, and the registry remembers it for the dialog.
            QString why;
            QVERIFY(m_rig
                        .renderEffect(QStringLiteral("plugin:test.broken/fx"), ParamSet(), m_chart,
                            0, 25, &why)
                        .isNull());
            QVERIFY2(why.contains("undeclaredThing"), qPrintable(why));
            QVERIFY(!PluginRegistry::instance()
                         .shaderError(QStringLiteral("plugin:test.broken/fx"))
                         .isEmpty());
        }
        QDir(dir).removeRecursively();
        PluginRegistry::instance().reload();
    }

    void anEditedShaderShowsAfterReload()
    {
        if (!m_gl)
            QSKIP("no OpenGL");
        const QString dir = m_dir.filePath(QStringLiteral("user/test.hot"));
        writeFile(dir + QStringLiteral("/plugin.json"),
            "{\"id\":\"test.hot\",\"name\":\"Hot\",\"version\":\"1.0\",\"author\":\"A\","
            "\"license\":\"MIT\",\"api\":1,"
            "\"effects\":[{\"id\":\"fx\",\"name\":\"Hot\",\"file\":\"fx.glsl\"}]}");
        writeFile(dir + QStringLiteral("/fx.glsl"),
            "vec4 effect(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }\n");
        PluginRegistry& reg = PluginRegistry::instance();
        reg.reload();
        const QString id = QStringLiteral("plugin:test.hot/fx");
        QString why;
        const QImage red = m_rig.renderEffect(id, ParamSet(), m_chart, 0, 25, &why);
        QVERIFY2(!red.isNull(), qPrintable(why));
        QCOMPARE(qRed(red.pixel(10, 10)), 255);
        QCOMPARE(qGreen(red.pixel(10, 10)), 0);
        writeFile(dir + QStringLiteral("/fx.glsl"),
            "vec4 effect(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }\n");
        reg.reload();
        const QImage green = m_rig.renderEffect(id, ParamSet(), m_chart, 0, 25, &why);
        QVERIFY2(!green.isNull(), qPrintable(why));
        QCOMPARE(qGreen(green.pixel(10, 10)), 255);
        QCOMPARE(qRed(green.pixel(10, 10)), 0);
        QDir(dir).removeRecursively();
        reg.reload();
    }

    void parametersOfEveryTypeReachTheShader()
    {
        if (!m_gl)
            QSKIP("no OpenGL");
        const QString dir = m_dir.filePath(QStringLiteral("user/test.params"));
        writeFile(dir + QStringLiteral("/plugin.json"),
            "{\"id\":\"test.params\",\"name\":\"Params\",\"version\":\"1.0\",\"author\":\"A\","
            "\"license\":\"MIT\",\"api\":1,"
            "\"effects\":[{\"id\":\"fx\",\"name\":\"Params\",\"file\":\"fx.glsl\",\"params\":["
            "{\"key\":\"r\",\"type\":\"number\",\"min\":0,\"max\":1,\"default\":0.25},"
            "{\"key\":\"count\",\"type\":\"integer\",\"min\":0,\"max\":10,\"default\":3},"
            "{\"key\":\"flag\",\"type\":\"boolean\",\"default\":true},"
            "{\"key\":\"tint\",\"type\":\"color\",\"default\":\"#0000ff\"},"
            "{\"key\":\"mode\",\"type\":\"choice\",\"choices\":[\"a\",\"b\",\"c\"],\"default\":"
            "\"b\"}]}]}");
        // r -> red, count/10 -> green, flag -> alpha stays 1 (0.5 when off), tint.b -> blue
        // times (mode == 2 ? 1 : 0.5).
        writeFile(dir + QStringLiteral("/fx.glsl"),
            "vec4 effect(vec2 uv)\n{\n"
            "    float b = u_tint.b * (u_mode == 2 ? 1.0 : 0.5);\n"
            "    float a = u_flag ? 1.0 : 0.5;\n"
            "    return vec4(u_r * a, float(u_count) / 10.0 * a, b * a, a);\n}\n");
        PluginRegistry::instance().reload();
        const QString id = QStringLiteral("plugin:test.params/fx");
        ParamSet p = EffectRegistry::instance().defaultParams(id);
        QString why;
        QImage image = m_rig.renderEffect(id, p, m_chart, 0, 25, &why);
        QVERIFY2(!image.isNull(), qPrintable(why));
        const QRgb d = image.pixel(5, 5);
        QVERIFY(std::abs(qRed(d) - 64) <= 1); // 0.25
        QVERIFY(std::abs(qGreen(d) - 77) <= 1); // 0.3
        QVERIFY(std::abs(qBlue(d) - 128) <= 1); // 1.0 * 0.5
        QCOMPARE(qAlpha(d), 255);
        p.setBase(QStringLiteral("r"), 1.0);
        p.setBase(QStringLiteral("count"), 10);
        p.setStatic(QStringLiteral("flag"), false);
        p.setStatic(QStringLiteral("mode"), QStringLiteral("c"));
        p.setStatic(QStringLiteral("tint"), QColor(0, 0, 255));
        image = m_rig.renderEffect(id, p, m_chart, 0, 25, &why);
        const QRgb e = image.pixel(5, 5);
        QCOMPARE(qAlpha(e), 128);
        QVERIFY(std::abs(qRed(e) - 128) <= 1); // premultiplied: 1.0 * 0.5
        QVERIFY(std::abs(qBlue(e) - 128) <= 1);
        // A value outside the declared range is clamped before it reaches the shader.
        p.setBase(QStringLiteral("r"), 50.0);
        image = m_rig.renderEffect(id, p, m_chart, 0, 25, &why);
        QVERIFY(std::abs(qRed(image.pixel(5, 5)) - 128) <= 1);
        QDir(dir).removeRecursively();
        PluginRegistry::instance().reload();
    }

    // The bundled gl-transitions pack (spec 160): every transition compiles for both APIs, draws
    // the outgoing picture at progress 0 and the incoming one at 1, and nothing in between
    // comes out empty (a NaN in a shader gives transparent black in an 8 bit target).
    void glTransitionsPackCompilesAndRenders()
    {
        PluginRegistry& reg = PluginRegistry::instance();
        QString error;
        QVector<ValidatedItem> items;
        QString text;
        for (const char* name : {"dev.kinoklip.gl-transitions", "dev.kinoklip.gl-transitions-2"}) {
            const QString folder = bundledDir() + QLatin1Char('/') + QLatin1String(name);
            QVERIFY(QFileInfo(folder + QStringLiteral("/plugin.json")).exists());
            QVERIFY(QFileInfo(folder + QStringLiteral("/LICENSE")).exists());
            QVERIFY2(reg.install(folder, &error), qPrintable(error));
            const PluginValidation v = validatePluginFolder(folder, m_gl);
            for (const plugins::Problem& p : v.problems)
                text += p.text() + QLatin1Char('\n');
            items += v.items;
        }
        QVERIFY2(text.isEmpty(), qPrintable(text.left(6000)));
        QVERIFY2(items.size() > 300, "the pack lost its transitions");
        for (const ValidatedItem& item : std::as_const(items)) {
            QVERIFY2(item.desktop != QLatin1String("error"), qPrintable(item.fullId));
            QVERIFY2(item.gles != QLatin1String("error"), qPrintable(item.fullId));
        }
        if (!m_gl)
            QSKIP("no OpenGL: the pack was only checked for its manifest");
        const QImage a = previewTestChart(QSize(160, 90));
        QImage b = a;
        b.invertPixels(QImage::InvertRgb);
        QStringList startBad, endBad, emptyBad, failed;
        int count = 0;
        for (const ValidatedItem& item : std::as_const(items)) {
            QString why;
            ParamSet defaults; // the manifest defaults
            if (const EffectDescriptor* d = EffectRegistry::instance().find(item.fullId)) {
                for (const ParamDescriptor& pd : d->params) {
                    if (pd.isScalar())
                        defaults.setBase(pd.key, pd.def);
                    else
                        defaults.setStatic(pd.key, pd.defStatic);
                }
            }
            const QImage at0 = m_rig.renderTransition(item.fullId, defaults, 0.0, a, b, &why);
            if (at0.isNull()) {
                failed << item.fullId + QStringLiteral(": ") + why;
                continue;
            }
            ++count;
            const QImage mid = m_rig.renderTransition(item.fullId, defaults, 0.5, a, b, &why);
            const QImage at1 = m_rig.renderTransition(item.fullId, defaults, 1.0, a, b, &why);
            if (meanDifference(at0, a) > 6.0)
                startBad << item.fullId;
            if (meanDifference(at1, b) > 6.0)
                endBad << item.fullId;
            int transparent = 0;
            for (int y = 0; y < mid.height(); ++y)
                for (int x = 0; x < mid.width(); ++x)
                    transparent += qAlpha(mid.pixel(x, y)) == 0 ? 1 : 0;
            if (transparent > mid.width() * mid.height() / 2)
                emptyBad << item.fullId;
        }
        qInfo("gl-transitions rendered: %d; start differs: %s; end differs: %s; empty: %s", count,
            qPrintable(startBad.join(QLatin1Char(' '))), qPrintable(endBad.join(QLatin1Char(' '))),
            qPrintable(emptyBad.join(QLatin1Char(' '))));
        QVERIFY2(failed.isEmpty(), qPrintable(failed.join(QLatin1Char('\n'))));
        QVERIFY2(startBad.isEmpty(), qPrintable(startBad.join(QLatin1Char(' '))));
        QVERIFY2(endBad.isEmpty(), qPrintable(endBad.join(QLatin1Char(' '))));
        QVERIFY2(emptyBad.isEmpty(), qPrintable(emptyBad.join(QLatin1Char(' '))));
        QVERIFY(reg.uninstall(QStringLiteral("dev.kinoklip.gl-transitions"), &error));
        QVERIFY(reg.uninstall(QStringLiteral("dev.kinoklip.gl-transitions-2"), &error));
    }

    void everyExampleCompilesForBothApis()
    {
        for (const QFileInfo& e :
            QDir(examplesDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const PluginValidation v = validatePluginFolder(e.absoluteFilePath(), m_gl);
            QString text;
            for (const plugins::Problem& p : v.problems)
                text += p.text() + QLatin1Char('\n');
            QVERIFY2(v.valid, qPrintable(e.fileName() + QStringLiteral(":\n") + text));
            for (const ValidatedItem& item : v.items) {
                QVERIFY2(item.desktop != QLatin1String("error"), qPrintable(item.fullId));
                // OpenGL ES 3.0 is checked where the machine can make such a context; the run
                // with KK_GL_API=gles (docs/building.md) makes the whole test use it.
                QVERIFY2(item.gles != QLatin1String("error"), qPrintable(item.fullId));
                if (m_gl)
                    QVERIFY(item.desktop == QLatin1String("ok") || m_rig.isEs());
            }
        }
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestPluginRender>(argc, argv);
}

#include "test_pluginrender.moc"
