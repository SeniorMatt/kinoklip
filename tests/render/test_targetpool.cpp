// SPDX-License-Identifier: GPL-3.0-or-later
// Byte limit of the GL render-target pool: targets of ever-changing sizes (a keyframed zoom on a
// generator or a retimed clip gave every frame its own size) must not pile up beyond the limit.
// Needs OpenGL 3.3 (skips without).
#include "render/GlKit.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>
#include <QtTest>

using namespace kk::render;

class TestTargetPool : public QObject
{
    Q_OBJECT

    QOffscreenSurface m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    GlFunctions* m_gl = nullptr;

private Q_SLOTS:
    void initTestCase()
    {
        m_surface.create();
        m_context = std::make_unique<QOpenGLContext>();
        if (!m_context->create() || !m_context->makeCurrent(&m_surface) || m_context->isOpenGLES()
            || m_context->format().version() < qMakePair(3, 3))
            QSKIP("OpenGL 3.3 core is not available");
        m_gl = QOpenGLVersionFunctionsFactory::get<GlFunctions>(m_context.get());
        if (!m_gl || !m_gl->initializeOpenGLFunctions())
            QSKIP("OpenGL 3.3 functions are not available");
    }

    void bytesPerFormat()
    {
        QCOMPARE(targetBytes({100, 10}, GL_RGBA16F), 8000);
        QCOMPARE(targetBytes({100, 10}, GL_RGBA8), 4000);
    }

    void freeTargetsAreEvictedAtTheLimit()
    {
        TargetPool pool(m_gl);
        pool.setByteLimit(8LL << 20); // 8 MiB: four 512x512 RGBA16F targets
        for (int i = 0; i < 200; ++i) {
            PooledTarget t = pool.acquire({512 + i, 512}, GL_RGBA16F); // a new size every "frame"
            QVERIFY(t);
            pool.trim();
            QVERIFY2(pool.bytes() <= pool.byteLimit() + targetBytes({712, 512}, GL_RGBA16F),
                qPrintable(QString::number(pool.bytes())));
        }
        QVERIFY(pool.targetCount() <= 5);
        pool.destroyAll();
        QCOMPARE(pool.bytes(), 0);
    }

    void targetsInUseAreNeverEvicted()
    {
        TargetPool pool(m_gl);
        pool.setByteLimit(1 << 20);
        std::vector<PooledTarget> held;
        for (int i = 0; i < 4; ++i)
            held.push_back(pool.acquire({256, 256 + i}, GL_RGBA16F)); // 2 MiB in use
        QCOMPARE(pool.targetCount(), size_t(4));
        for (const PooledTarget& t : held)
            QVERIFY(t);
        held.clear();
        [[maybe_unused]] const auto small
            = pool.acquire({128, 128}, GL_RGBA8); // frees the idle ones to make room
        QVERIFY(pool.targetCount() <= 2);
        pool.destroyAll();
    }

    void poolsOfTheProcessShareALimit()
    {
        // Spec 85.7: each Compositor has its own pool; each pool stops growing once the pools
        // together are at the process limit.
        const int64_t before = TargetPool::processByteLimit();
        TargetPool a(m_gl), b(m_gl);
        const int64_t base = TargetPool::processBytes();
        TargetPool::setProcessByteLimit(base + (6LL << 20)); // three 512x512 RGBA16F targets
        for (int i = 0; i < 6; ++i) {
            PooledTarget t = a.acquire({512, 512 + i}, GL_RGBA16F);
        }
        QVERIFY(a.bytes() <= (6LL << 20) + targetBytes({512, 520}, GL_RGBA16F));
        {
            PooledTarget t = b.acquire({512, 512}, GL_RGBA16F); // b has nothing idle to evict
            QCOMPARE(a.bytes() + b.bytes(), TargetPool::processBytes() - base);
        }
        a.trimAll();
        QCOMPARE(a.bytes(), 0);
        b.destroyAll();
        a.destroyAll();
        QCOMPARE(TargetPool::processBytes(), base);
        TargetPool::setProcessByteLimit(before);
    }

    void sameSizeIsReused()
    {
        TargetPool pool(m_gl);
        {
            PooledTarget a = pool.acquire({64, 64}, GL_RGBA16F);
        }
        {
            PooledTarget b = pool.acquire({64, 64}, GL_RGBA16F);
        }
        QCOMPARE(pool.targetCount(), size_t(1));
        pool.destroyAll();
    }
};

int main(int argc, char** argv)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    TestTargetPool test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_targetpool.moc"
