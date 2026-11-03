// SPDX-License-Identifier: GPL-3.0-or-later
// test_container — the ZIP container of single-file projects: round trip, standard ZIP tools,
// crash safety (truncation at every few bytes of a save, torn records, interrupted media copy),
// ZIP64 with a sparse > 4 GiB entry, concurrent readers, entry names.
#include "pack/Container.h"
#include "pack/Crc32.h"
#include "pack/EmbeddedMedia.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrent>

#include <atomic>
#include <cstring>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

using namespace kk::pack;

namespace {

/// Deterministic pseudo-random bytes: byte i of stream `seed`.
QByteArray pattern(qint64 size, quint32 seed)
{
    QByteArray b(size, Qt::Uninitialized);
    quint32 x = seed * 2654435761u + 1;
    for (qint64 i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        b[i] = char(x >> 24);
    }
    return b;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray entryData(const Container& c, const QString& name)
{
    const ContainerEntry* e = c.find(name);
    return e ? c.read(*e, nullptr) : QByteArray();
}

/// Runs python3's zipfile on @p path: testzip() (every CRC) and prints "name size" lines.
/// @return the output, or a null QByteArray when python3 is unavailable.
QByteArray pythonZipListing(const QString& path, bool test, QString* error)
{
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
        return {};
    const QString script = QStringLiteral("import sys, zipfile\n"
                                          "z = zipfile.ZipFile(sys.argv[1])\n"
                                          "if sys.argv[2] == '1':\n"
                                          "    bad = z.testzip()\n"
                                          "    assert bad is None, bad\n"
                                          "for i in z.infolist():\n"
                                          "    print(i.filename, i.file_size, i.compress_type)\n"
                                          "print('comment', z.comment.decode())\n");
    QProcess p;
    p.start(python,
        {QStringLiteral("-I"), QStringLiteral("-c"), script, path,
            test ? QStringLiteral("1") : QStringLiteral("0")});
    if (!p.waitForFinished(120000) || p.exitCode() != 0) {
        *error = QString::fromUtf8(p.readAllStandardError());
        return QByteArray("");
    }
    return p.readAllStandardOutput();
}

} // namespace

class TestContainer : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const char* name) const { return m_dir.filePath(QString::fromLatin1(name)); }

private Q_SLOTS:
    void crc32KnownValues()
    {
        QCOMPARE(crc32Update(0, "", 0), 0u);
        QCOMPARE(crc32Update(0, "123456789", 9), 0xCBF43926u); // the standard check value
        // Incremental == one shot, for every split point of an odd-sized buffer.
        const QByteArray data = pattern(1037, 7);
        const uint32_t whole = crc32Update(0, data.constData(), size_t(data.size()));
        for (int split = 0; split <= data.size(); split += 13) {
            const uint32_t a = crc32Update(0, data.constData(), size_t(split));
            QCOMPARE(crc32Update(a, data.constData() + split, size_t(data.size() - split)), whole);
        }
    }

    void entryNames()
    {
        QVERIFY(isValidEntryName(QStringLiteral("media/clip.mp4")));
        QVERIFY(isValidEntryName(QStringLiteral("media/Ünïcode clip (2).mov")));
        QVERIFY(!isValidEntryName(QString()));
        QVERIFY(!isValidEntryName(QStringLiteral("/abs")));
        QVERIFY(!isValidEntryName(QStringLiteral("media/../x")));
        QVERIFY(!isValidEntryName(QStringLiteral("media//x")));
        QVERIFY(!isValidEntryName(QStringLiteral("media\\x")));
    }

    void roundTrip()
    {
        const QString file = path("round.kinoklip");
        QString error;
        {
            auto c = Container::create(file, &error);
            QVERIFY2(c, qPrintable(error));
            QCOMPARE(c->generation(), 1u);
            QVERIFY(c->addData(QStringLiteral("project.kkproj"), "{\"v\":1}", {}, &error));
            QVERIFY(writeFile(path("src.bin"), pattern(100000, 1)));
            QCOMPARE(c->addFile(QStringLiteral("media/src.bin"), path("src.bin"), {}, {}, &error),
                Status::Ok);
            QVERIFY(c->commit(&error));
            QCOMPARE(c->generation(), 2u);
        }
        QVERIFY(Container::looksLikeContainer(file));
        QVERIFY(!Container::looksLikeContainer(path("src.bin")));
        qint64 mediaOffset = 0;
        {
            auto c = Container::open(file, Container::Mode::ReadWrite, &error);
            QVERIFY2(c, qPrintable(error));
            QCOMPARE(c->recovery(), Recovery::Clean);
            QCOMPARE(c->entries().size(), 3);
            QCOMPARE(c->entries().front().name, QStringLiteral("mimetype"));
            QCOMPARE(entryData(*c, QStringLiteral("mimetype")), Container::mimeType());
            QCOMPARE(entryData(*c, QStringLiteral("project.kkproj")), QByteArray("{\"v\":1}"));
            QCOMPARE(entryData(*c, QStringLiteral("media/src.bin")), pattern(100000, 1));
            mediaOffset = c->find(QStringLiteral("media/src.bin"))->dataOffset;
            // Saving again appends a new document; the media bytes stay where they are.
            const qint64 before = QFileInfo(file).size();
            QVERIFY(c->addData(QStringLiteral("project.kkproj"), "{\"v\":2}", {}, &error));
            QVERIFY(c->commit(&error));
            QVERIFY(QFileInfo(file).size() - before < 1000);
        }
        auto c = Container::open(file, Container::Mode::ReadOnly, &error);
        QVERIFY2(c, qPrintable(error));
        QCOMPARE(c->generation(), 3u);
        QCOMPARE(entryData(*c, QStringLiteral("project.kkproj")), QByteArray("{\"v\":2}"));
        QCOMPARE(c->find(QStringLiteral("media/src.bin"))->dataOffset, mediaOffset);
        QVERIFY(c->liveSize() < c->committedSize()); // the first document is garbage now
        // Readers outside: location and a RangeFile over the entry.
        EntryLocation where;
        QVERIFY(locateEntry(file, QStringLiteral("media/src.bin"), &where, &error));
        QCOMPARE(where.dataOffset, mediaOffset);
        RangeFile range;
        QVERIFY(range.openPath(embeddedPath(file, QStringLiteral("media/src.bin"))));
        QCOMPARE(range.size(), 100000);
        QByteArray part(1000, '\0');
        QCOMPARE(range.read(99500, part.data(), 1000), 500); // clamped to the range
        QCOMPARE(part.left(500), pattern(100000, 1).right(500));
        // Read-only containers refuse writes.
        QVERIFY(!c->addData(QStringLiteral("x"), "y", {}, &error));
    }

    void embeddedPaths()
    {
        const QString file = path("paths.kinoklip");
        QVERIFY(Container::create(file, nullptr));
        QString container;
        QString entry;
        QVERIFY(splitEmbeddedPath(file + QStringLiteral("/media/a b.mp4"), &container, &entry));
        QCOMPARE(container, file);
        QCOMPARE(entry, QStringLiteral("media/a b.mp4"));
        QVERIFY(!splitEmbeddedPath(path("other.kinoklip/media/a.mp4"), nullptr, nullptr));
        QVERIFY(!splitEmbeddedPath(QStringLiteral("/tmp/plain.mp4"), nullptr, nullptr));
        QVERIFY(!mayBeEmbeddedPath(QStringLiteral("/tmp/plain.mp4")));
        QVERIFY(mayBeEmbeddedPath(QStringLiteral("/tmp/X.KINOKLIP/media/a.mp4")));
    }

    void standardZipTools()
    {
        const QString file = path("tools.kinoklip");
        QString error;
        {
            auto c = Container::create(file, &error);
            QVERIFY(c);
            QVERIFY(c->addData(QStringLiteral("project.kkproj"), "{}", {}, &error));
            QVERIFY(writeFile(path("tools.bin"), pattern(300000, 2)));
            QCOMPARE(
                c->addFile(QStringLiteral("media/tools.bin"), path("tools.bin"), {}, {}, &error),
                Status::Ok);
            QVERIFY(c->commit(&error));
            QVERIFY(c->addData(QStringLiteral("project.kkproj"), "{\"again\":1}", {}, &error));
            QVERIFY(c->commit(&error)); // garbage in the middle must not bother readers
        }
        const QByteArray listing = pythonZipListing(file, true, &error);
        if (listing.isNull())
            QSKIP("python3 not available");
        QVERIFY2(!listing.isEmpty(), qPrintable(error));
        QVERIFY2(listing.contains("mimetype 22 0\n"), listing.constData());
        QVERIFY2(listing.contains("media/tools.bin 300000 0\n"), listing.constData());
        QVERIFY2(listing.contains("project.kkproj 11 0\n"), listing.constData());
        QVERIFY(listing.contains("comment Kinoklip project container 1"));
        const QString unzip = QStandardPaths::findExecutable(QStringLiteral("unzip"));
        if (!unzip.isEmpty()) {
            QProcess p;
            p.start(unzip, {QStringLiteral("-tq"), file});
            QVERIFY(p.waitForFinished(60000));
            QVERIFY2(p.exitCode() == 0, p.readAllStandardOutput().constData());
            QTemporaryDir out;
            p.start(unzip, {QStringLiteral("-q"), file, QStringLiteral("-d"), out.path()});
            QVERIFY(p.waitForFinished(60000));
            QCOMPARE(p.exitCode(), 0);
            QCOMPARE(readFile(out.filePath(QStringLiteral("media/tools.bin"))), pattern(300000, 2));
            QCOMPARE(readFile(out.filePath(QStringLiteral("project.kkproj"))),
                QByteArray("{\"again\":1}"));
        }
    }

    /// Every truncation of the last save leaves the previous state readable.
    void truncatedSaveKeepsPreviousState()
    {
        const QString file = path("crash.kinoklip");
        QString error;
        qint64 previousEnd = 0;
        qint64 finalEnd = 0;
        const QByteArray v1 = "{\"version\":1}";
        const QByteArray v2 = pattern(5000, 3).toBase64(); // a larger second document
        {
            auto c = Container::create(file, &error);
            QVERIFY(c);
            QVERIFY(writeFile(path("crash.bin"), pattern(20000, 4)));
            QCOMPARE(
                c->addFile(QStringLiteral("media/crash.bin"), path("crash.bin"), {}, {}, &error),
                Status::Ok);
            QVERIFY(c->addData(QStringLiteral("project.kkproj"), v1, {}, &error));
            QVERIFY(c->commit(&error));
            previousEnd = c->committedSize();
            QVERIFY(c->addData(QStringLiteral("project.kkproj"), v2, {}, &error));
            QVERIFY(c->addData(QStringLiteral("manifest.json"), "{}", {}, &error));
            QVERIFY(c->commit(&error));
            finalEnd = c->committedSize();
        }
        const QByteArray full = readFile(file);
        QCOMPARE(qint64(full.size()), finalEnd);
        const QString cut = path("cut.kinoklip");
        int checked = 0;
        for (qint64 size = previousEnd; size <= finalEnd;
             size += (size - previousEnd < 64 || finalEnd - size < 200) ? 1 : 7) {
            QVERIFY(writeFile(cut, full.left(size)));
            auto c = Container::open(cut, Container::Mode::ReadOnly, &error);
            QVERIFY2(c, qPrintable(QStringLiteral("cut at %1: %2").arg(size).arg(error)));
            const QByteArray doc = entryData(*c, QStringLiteral("project.kkproj"));
            if (size == finalEnd) {
                QCOMPARE(doc, v2);
                QCOMPARE(c->recovery(), Recovery::Clean);
            } else {
                QVERIFY2(doc == v1, qPrintable(QStringLiteral("cut at %1").arg(size)));
                QCOMPARE(c->committedSize(), previousEnd);
                QCOMPARE(c->ignoredTailBytes(), size - previousEnd);
                QCOMPARE(entryData(*c, QStringLiteral("media/crash.bin")), pattern(20000, 4));
                QVERIFY(!c->find(QStringLiteral("manifest.json")));
            }
            ++checked;
        }
        qInfo("checked %d truncation points over %lld bytes of the last save", checked,
            finalEnd - previousEnd);
        // A recovered file can be saved again; the torn tail is cut off first.
        QVERIFY(writeFile(cut, full.left(finalEnd - 10)));
        {
            auto c = Container::open(cut, Container::Mode::ReadWrite, &error);
            QVERIFY(c);
            QCOMPARE(c->recovery(), Recovery::HeaderSlot);
            QVERIFY(c->addData(QStringLiteral("project.kkproj"), "{\"version\":3}", {}, &error));
            QVERIFY(c->commit(&error));
        }
        auto c = Container::open(cut, Container::Mode::ReadOnly, &error);
        QVERIFY(c);
        QCOMPARE(c->recovery(), Recovery::Clean);
        QCOMPARE(entryData(*c, QStringLiteral("project.kkproj")), QByteArray("{\"version\":3}"));
        QString pyError;
        const QByteArray listing = pythonZipListing(cut, true, &pyError);
        QVERIFY2(listing.isNull() || !listing.isEmpty(), qPrintable(pyError));
    }

    /// A crash while the end records are written (torn records after the data, header slot
    /// not yet updated), and damage inside the newest records: the previous state is used.
    void tornRecordsAndDamage()
    {
        QString error;
        for (const qint64 crashAfter :
            {qint64(0), qint64(10), qint64(46), qint64(120), qint64(100000)}) {
            const QString file = path("torn.kinoklip");
            {
                auto c = Container::create(file, &error);
                QVERIFY(c);
                QVERIFY(c->addData(QStringLiteral("project.kkproj"), "old", {}, &error));
                QVERIFY(c->commit(&error));
                QVERIFY(c->addData(QStringLiteral("project.kkproj"), "new", {}, &error));
                c->setCrashAfterBytesForTest(crashAfter);
                QVERIFY(!c->commit(&error));
            }
            auto c = Container::open(file, Container::Mode::ReadOnly, &error);
            QVERIFY2(c, qPrintable(error));
            // Records complete, only the header slot missing: the new state is valid.
            const QByteArray expected = crashAfter >= 100000 ? "new" : "old";
            QCOMPARE(entryData(*c, QStringLiteral("project.kkproj")), expected);
        }
        // A flipped bit in the newest central directory: its CRC no longer matches.
        const QString file = path("flip.kinoklip");
        qint64 cdStart = 0;
        {
            auto c = Container::create(file, &error);
            QVERIFY(c->addData(QStringLiteral("project.kkproj"), "old", {}, &error));
            QVERIFY(c->commit(&error));
            QVERIFY(c->addData(QStringLiteral("project.kkproj"), "new", {}, &error));
            cdStart = c->entries().back().dataOffset + 3;
            QVERIFY(c->commit(&error));
        }
        QByteArray bytes = readFile(file);
        bytes[cdStart + 20] = char(bytes[cdStart + 20] ^ 0x01);
        QVERIFY(writeFile(file, bytes));
        auto c = Container::open(file, Container::Mode::ReadOnly, &error);
        QVERIFY2(c, qPrintable(error));
        QCOMPARE(entryData(*c, QStringLiteral("project.kkproj")), QByteArray("old"));
        QCOMPARE(c->recovery(), Recovery::HeaderSlot);
        // Both header slots destroyed as well: the backward scan still finds the old state.
        bytes = readFile(file);
        for (int i = 42; i < 42 + 64; ++i)
            bytes[i] = '\0';
        QVERIFY(writeFile(file, bytes));
        c = Container::open(file, Container::Mode::ReadOnly, &error);
        QVERIFY2(c, qPrintable(error));
        QCOMPARE(c->recovery(), Recovery::Scan);
        QCOMPARE(entryData(*c, QStringLiteral("project.kkproj")), QByteArray("old"));
        // Not a container at all.
        QVERIFY(writeFile(path("junk.kinoklip"), pattern(5000, 9)));
        QVERIFY(!Container::open(path("junk.kinoklip"), Container::Mode::ReadOnly, &error));
        QVERIFY(!error.isEmpty());
    }

    /// Cancelling (or crashing during) a media copy leaves the file as it was.
    void cancelledCopy()
    {
        const QString file = path("cancel.kinoklip");
        QString error;
        auto c = Container::create(file, &error);
        QVERIFY(c->addData(QStringLiteral("project.kkproj"), "doc", {}, &error));
        QVERIFY(c->commit(&error));
        const qint64 size = QFileInfo(file).size();
        QVERIFY(writeFile(path("big.bin"), pattern(12 * 1024 * 1024, 5)));
        int calls = 0;
        const Status st = c->addFile(
            QStringLiteral("media/big.bin"), path("big.bin"), {},
            [&calls](qint64, qint64) { return ++calls < 2; }, &error);
        QCOMPARE(st, Status::Cancelled);
        QVERIFY(!c->find(QStringLiteral("media/big.bin")));
        QCOMPARE(QFileInfo(file).size(), size);
        c.reset();
        c = Container::open(file, Container::Mode::ReadOnly, &error);
        QVERIFY(c);
        QCOMPARE(c->recovery(), Recovery::Clean);
        // A crash in the middle of a copy (partial media after the committed end).
        QFile f(file);
        QVERIFY(f.open(QIODevice::Append));
        f.write(pattern(3 * 1024 * 1024, 6));
        f.close();
        c = Container::open(file, Container::Mode::ReadOnly, &error);
        QVERIFY(c);
        QCOMPARE(c->recovery(), Recovery::HeaderSlot);
        QCOMPARE(entryData(*c, QStringLiteral("project.kkproj")), QByteArray("doc"));
    }

    /// A media entry larger than 4 GiB stored after other data, followed by an entry whose
    /// offset is above 4 GiB: every ZIP64 field. Sparse files keep the disk use near zero.
    void zip64Sparse()
    {
        constexpr qint64 kBig = (4LL << 30) + 123457; // 4 GiB + a bit
        const QString source = path("sparse.bin");
        {
            QFile f(source);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("HEAD");
            QVERIFY(f.resize(kBig));
            QVERIFY(f.seek(kBig - 4));
            f.write("TAIL");
        }
#if defined(Q_OS_UNIX)
        struct stat st
        { };
        ::stat(QFile::encodeName(source).constData(), &st);
        if (qint64(st.st_blocks) * 512 > 64LL * 1024 * 1024)
            QSKIP("the temporary folder does not support sparse files");
#endif
        const QString file = path("zip64.kinoklip");
        QString error;
        QElapsedTimer timer;
        timer.start();
        {
            auto c = Container::create(file, &error);
            QVERIFY(c);
            QVERIFY(c->addData(QStringLiteral("project.kkproj"), "{}", {}, &error));
            uint32_t crc = 0;
            QCOMPARE(
                c->addFile(QStringLiteral("media/sparse.bin"), source, {}, {}, &error, 0, -1, &crc),
                Status::Ok);
            QVERIFY(writeFile(path("after.bin"), pattern(5000, 8)));
            QCOMPARE(
                c->addFile(QStringLiteral("media/after.bin"), path("after.bin"), {}, {}, &error),
                Status::Ok);
            QVERIFY(c->commit(&error));
            QVERIFY(c->find(QStringLiteral("media/after.bin"))->headerOffset > (4LL << 30));
        }
        qInfo("4 GiB sparse entry stored in %lld ms", timer.elapsed());
#if defined(Q_OS_UNIX)
        ::stat(QFile::encodeName(file).constData(), &st);
        QVERIFY2(qint64(st.st_blocks) * 512 < 64LL * 1024 * 1024, "container should be sparse");
#endif
        QVERIFY(QFileInfo(file).size() > kBig);
        auto c = Container::open(file, Container::Mode::ReadOnly, &error);
        QVERIFY2(c, qPrintable(error));
        const ContainerEntry* big = c->find(QStringLiteral("media/sparse.bin"));
        QVERIFY(big);
        QCOMPARE(big->size, kBig);
        QCOMPARE(entryData(*c, QStringLiteral("media/after.bin")), pattern(5000, 8));
        RangeFile range;
        QVERIFY(range.openPath(embeddedPath(file, QStringLiteral("media/sparse.bin"))));
        char buf[4];
        QCOMPARE(range.read(0, buf, 4), 4);
        QCOMPARE(QByteArray(buf, 4), QByteArray("HEAD"));
        QCOMPARE(range.read(kBig - 4, buf, 4), 4);
        QCOMPARE(QByteArray(buf, 4), QByteArray("TAIL"));
        QString pyError;
        const QByteArray listing = pythonZipListing(file, false, &pyError);
        if (!listing.isNull()) {
            QVERIFY2(!listing.isEmpty(), qPrintable(pyError));
            QVERIFY2(listing.contains("media/sparse.bin " + QByteArray::number(kBig) + " 0"),
                listing.constData());
            QVERIFY(listing.contains("media/after.bin 5000 0"));
            // Python reads (and CRC-checks) the entry beyond 4 GiB through the ZIP64 offset.
            QProcess p;
            p.start(QStandardPaths::findExecutable(QStringLiteral("python3")),
                {QStringLiteral("-I"), QStringLiteral("-c"),
                    QStringLiteral("import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); "
                                   "print(len(z.read('media/after.bin')))"),
                    file});
            QVERIFY(p.waitForFinished(60000));
            QCOMPARE(p.readAllStandardOutput().trimmed(), QByteArray("5000"));
        }
        QFile::remove(file);
        QFile::remove(source);
    }

    /// Many threads read one container at once, each with its own RangeFile.
    void concurrentReaders()
    {
        const QString file = path("concurrent.kinoklip");
        QString error;
        constexpr int kEntries = 4;
        constexpr qint64 kSize = 6 * 1024 * 1024;
        {
            auto c = Container::create(file, &error);
            for (int i = 0; i < kEntries; ++i) {
                const QString src = path("c.bin");
                QVERIFY(writeFile(src, pattern(kSize, 100 + i)));
                QCOMPARE(c->addFile(QStringLiteral("media/c%1.bin").arg(i), src, {}, {}, &error),
                    Status::Ok);
            }
            QVERIFY(c->commit(&error));
        }
        std::vector<QByteArray> expected;
        for (int i = 0; i < kEntries; ++i)
            expected.push_back(pattern(kSize, 100 + i));
        std::atomic<int> failures {0};
        QList<int> threads;
        for (int t = 0; t < 12; ++t)
            threads.append(t);
        QtConcurrent::blockingMap(threads, [&](int t) {
            QRandomGenerator rng(quint32(t + 1));
            const int i = t % kEntries;
            RangeFile r;
            if (!r.openPath(embeddedPath(file, QStringLiteral("media/c%1.bin").arg(i)))) {
                ++failures;
                return;
            }
            QByteArray buf(65536, '\0');
            for (int k = 0; k < 400; ++k) {
                const qint64 pos = qint64(rng.bounded(quint32(kSize - 1)));
                const qint64 n = r.read(pos, buf.data(), buf.size());
                if (n != std::min<qint64>(buf.size(), kSize - pos)
                    || std::memcmp(
                        buf.constData(), expected[size_t(i)].constData() + pos, size_t(n))) {
                    ++failures;
                    return;
                }
            }
        });
        QCOMPARE(failures.load(), 0);
    }
};

QTEST_GUILESS_MAIN(TestContainer)
#include "test_container.moc"
