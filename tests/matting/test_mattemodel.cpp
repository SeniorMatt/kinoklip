// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the model catalogue, the download checks and the matte disk cache (spec 78): the
// hard-coded checksum, the URL rules, a download that must match size and SHA-256 before it is
// installed, removal, and the cache keys and trimming.
#include "matting/MatteCache.h"
#include "matting/ModelCatalog.h"
#include "matting/ModelDownload.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace kk::matting;

namespace {

ModelInfo fakeModel(const QByteArray& content)
{
    ModelInfo m = defaultModel();
    m.fileName = QStringLiteral("fake.onnx");
    m.sizeBytes = content.size();
    m.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    return m;
}

QString writePart(const QString& dir, const QByteArray& content)
{
    const QString path = dir + QStringLiteral("/fake.onnx.part");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(content);
    return path;
}

} // namespace

class TestMatteModel : public QObject
{
    Q_OBJECT
private slots:
    void catalogHasTheFactsTheDialogShows()
    {
        const ModelInfo& m = defaultModel();
        QVERIFY(!m.name.isEmpty());
        QCOMPARE(m.sha256.size(), 64);
        QVERIFY(m.sizeBytes > 1'000'000);
        QCOMPARE(m.licence, QStringLiteral("GPL-3.0"));
        QVERIFY(m.url.toString().startsWith(
            QStringLiteral("https://github.com/PeterL1n/RobustVideoMatting/")));
        QVERIFY(isAllowedDownloadUrl(m.url));
        QVERIFY(m.sourceUrl.isValid() && m.licenceUrl.isValid());
        QVERIFY(sizeText(m.sizeBytes).contains(QStringLiteral("14.")));
    }

    void onlyHttpsOnKnownHostsIsAllowed()
    {
        QVERIFY(isAllowedDownloadUrl(
            QUrl(QStringLiteral("https://github.com/a/b/releases/download/v1/x.onnx"))));
        QVERIFY(
            isAllowedDownloadUrl(QUrl(QStringLiteral("https://objects.githubusercontent.com/x"))));
        QVERIFY(!isAllowedDownloadUrl(QUrl(QStringLiteral("http://github.com/x"))));
        QVERIFY(!isAllowedDownloadUrl(QUrl(QStringLiteral("https://example.com/x"))));
        QVERIFY(!isAllowedDownloadUrl(QUrl(QStringLiteral("https://github.com.evil.example/x"))));
        QVERIFY(!isAllowedDownloadUrl(QUrl(QStringLiteral("https://user@github.com/x"))));
        QVERIFY(!isAllowedDownloadUrl(QUrl(QStringLiteral("file:///etc/passwd"))));
    }

    void downloadIsInstalledOnlyWhenSizeAndChecksumMatch()
    {
        QTemporaryDir dir;
        setModelsDirectoryForTests(dir.path());
        const QByteArray content = "not really a model, but data";
        const ModelInfo m = fakeModel(content);
        QVERIFY(!isInstalled(m));

        QString why;
        QVERIFY(installDownloaded(m, writePart(dir.path(), content), &why));
        QVERIFY(isInstalled(m));
        QCOMPARE(sha256OfFile(modelPath(m)), m.sha256);
        QVERIFY(!QFile::exists(dir.path() + QStringLiteral("/fake.onnx.part")));

        QVERIFY(removeModel(m));
        QVERIFY(!isInstalled(m));

        // Wrong content of the right size: refused and deleted.
        QByteArray evil = content;
        evil[0] = 'X';
        QVERIFY(!installDownloaded(m, writePart(dir.path(), evil), &why));
        QVERIFY(why.contains(QStringLiteral("checksum")));
        QVERIFY(!isInstalled(m));
        QVERIFY(!QFile::exists(dir.path() + QStringLiteral("/fake.onnx.part")));
        // Wrong size.
        QVERIFY(!installDownloaded(m, writePart(dir.path(), content + "x"), &why));
        QVERIFY(!isInstalled(m));
        setModelsDirectoryForTests({});
    }

    void aDownloadFromAForbiddenAddressNeverStarts()
    {
        QTemporaryDir dir;
        setModelsDirectoryForTests(dir.path());
        ModelInfo m = fakeModel("abc");
        m.url = QUrl(QStringLiteral("http://example.com/model.onnx"));
        ModelDownload job(m);
        QSignalSpy spy(&job, &ModelDownload::finished);
        job.start();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), false);
        QVERIFY(!job.isRunning());
        QVERIFY(!QFile::exists(modelPath(m)));
        setModelsDirectoryForTests({});
    }

    /// Opt-in (KK_TEST_DOWNLOAD=1): downloads the real model from its official address into a
    /// temporary folder and checks it is installed with the hard-coded checksum.
    void theRealDownloadWorks()
    {
        if (qEnvironmentVariable("KK_TEST_DOWNLOAD") != QLatin1String("1"))
            QSKIP("set KK_TEST_DOWNLOAD=1 to fetch the real model (network)");
        QTemporaryDir dir;
        setModelsDirectoryForTests(dir.path());
        ModelDownload job(defaultModel());
        QSignalSpy done(&job, &ModelDownload::finished);
        QSignalSpy progress(&job, &ModelDownload::progress);
        job.start();
        QVERIFY(done.wait(180000));
        QVERIFY2(done.at(0).at(0).toBool(), qPrintable(done.at(0).at(1).toString()));
        QVERIFY(progress.count() > 0);
        QVERIFY(isInstalled(defaultModel()));
        QCOMPARE(sha256OfFile(modelPath(defaultModel())), defaultModel().sha256);
        setModelsDirectoryForTests({});
    }

    void cacheKeysChangeWithTheFileTheModelAndTheQuality()
    {
        const QString a
            = MatteCache::folderKey(QStringLiteral("/v/a.mp4"), 100, 5, QStringLiteral("m"), 0);
        QCOMPARE(
            a, MatteCache::folderKey(QStringLiteral("/v/a.mp4"), 100, 5, QStringLiteral("m"), 0));
        QVERIFY(
            a != MatteCache::folderKey(QStringLiteral("/v/b.mp4"), 100, 5, QStringLiteral("m"), 0));
        QVERIFY(
            a != MatteCache::folderKey(QStringLiteral("/v/a.mp4"), 101, 5, QStringLiteral("m"), 0));
        QVERIFY(
            a != MatteCache::folderKey(QStringLiteral("/v/a.mp4"), 100, 6, QStringLiteral("m"), 0));
        QVERIFY(
            a != MatteCache::folderKey(QStringLiteral("/v/a.mp4"), 100, 5, QStringLiteral("n"), 0));
        QVERIFY(
            a != MatteCache::folderKey(QStringLiteral("/v/a.mp4"), 100, 5, QStringLiteral("m"), 1));
    }

    void cacheStoresLoadsListsAndTrims()
    {
        QTemporaryDir dir;
        MatteCache cache(dir.path());
        QImage matte(8, 4, QImage::Format_Grayscale8);
        matte.fill(200);
        QVERIFY(!cache.contains(QStringLiteral("k1"), 40));
        cache.store(QStringLiteral("k1"), 40, matte);
        cache.store(QStringLiteral("k1"), 0, matte);
        cache.store(QStringLiteral("k2"), 80, matte);
        QVERIFY(cache.contains(QStringLiteral("k1"), 40));
        const QImage back = cache.load(QStringLiteral("k1"), 40);
        QCOMPARE(back.size(), matte.size());
        QCOMPARE(qGray(back.pixel(3, 2)), 200);
        QCOMPARE(cache.times(QStringLiteral("k1")), (QList<int64_t> {0, 40}));
        QVERIFY(cache.load(QStringLiteral("k1"), 41).isNull());
        QVERIFY(cache.sizeBytes() > 0);
        cache.trim(0);
        QCOMPARE(cache.sizeBytes(), 0);
        cache.store(QStringLiteral("k1"), 0, matte);
        QVERIFY(cache.clear());
        QVERIFY(!cache.contains(QStringLiteral("k1"), 0));
    }
};

QTEST_MAIN(TestMatteModel)
#include "test_mattemodel.moc"
