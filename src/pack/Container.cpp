// SPDX-License-Identifier: GPL-3.0-or-later
// Container — ZIP writer/reader for single-file projects. Byte layout and the crash-safety
// argument: docs/single-file-project.md. All multi-byte ZIP fields are little-endian.
#include "pack/Container.h"

#include "pack/Crc32.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <io.h>
#endif

namespace kk::pack {

namespace {

constexpr uint32_t kLocalSig = 0x04034b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kEocd64Sig = 0x06064b50;
constexpr uint32_t kLocatorSig = 0x07064b50;

constexpr int kLocalHeaderSize = 30;
constexpr int kCentralHeaderSize = 46;
constexpr int kEocdSize = 22;
constexpr int kEocd64Size = 56;
constexpr int kLocatorSize = 20;

constexpr uint16_t kFlagUtf8 = 0x0800; ///< general purpose bit 11: names are UTF-8
constexpr uint16_t kVersionDefault = 20; ///< "version needed": 2.0 (stored entries)
constexpr uint16_t kVersionZip64 = 45; ///< 4.5: ZIP64
constexpr uint16_t kVersionMadeBy = (3 << 8) | 63; ///< Unix, APPNOTE 6.3
constexpr uint32_t kExternalAttributes = 0100644u << 16; ///< regular file, rw-r--r--
constexpr uint32_t kMax32 = 0xFFFFFFFFu;

/// Our private extra field in the local header of the first entry ("mimetype"): two header
/// slots of kSlotSize bytes. Extra field id 0x4b4b ("KK").
constexpr uint16_t kHeaderExtraId = 0x4b4b;
constexpr int kSlotSize = 32;
constexpr int kSlotCount = 2;
const QByteArray kMimeName = QByteArrayLiteral("mimetype");
/// File offset of slot 0: local header (30) + "mimetype" (8) + extra field id and size (4).
constexpr qint64 kSlotsOffset = kLocalHeaderSize + 8 + 4;
const QByteArray kSlotMagic = QByteArrayLiteral("KKS1");

/// The ZIP archive comment carries the generation and the CRC-32 of the central directory and
/// ZIP64 records, so an end record is only trusted when everything it points to is intact.
/// Fixed length: the tail check reads exactly one position.
const QByteArray kCommentPrefix = QByteArrayLiteral("Kinoklip project container 1; gen ");
constexpr int kCommentLength = 34 + 16 + 6 + 8; // prefix, 16 hex digits, "; crc ", 8 hex digits

/// Entries outside media/ smaller than this are CRC-checked when a state is loaded: a commit is
/// only accepted when the document and manifest it points to are intact.
constexpr qint64 kCheckedEntryLimit = 64LL * 1024 * 1024;
/// Upper bound for a central directory (about a million entries); anything bigger is damage.
constexpr qint64 kMaxCentralDirectory = 256LL * 1024 * 1024;
/// Copy buffer of addFile(): large sequential reads and writes (the copy is disk-bound).
constexpr qint64 kCopyChunk = 8LL * 1024 * 1024;

void put16(QByteArray& b, uint16_t v)
{
    b.append(char(v & 0xFF));
    b.append(char(v >> 8));
}
void put32(QByteArray& b, uint32_t v)
{
    put16(b, uint16_t(v & 0xFFFF));
    put16(b, uint16_t(v >> 16));
}
void put64(QByteArray& b, uint64_t v)
{
    put32(b, uint32_t(v & 0xFFFFFFFFu));
    put32(b, uint32_t(v >> 32));
}
uint16_t get16(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return uint16_t(u[0] | (u[1] << 8));
}
uint32_t get32(const char* p)
{
    return uint32_t(get16(p)) | (uint32_t(get16(p + 2)) << 16);
}
uint64_t get64(const char* p)
{
    return uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32);
}

void toDos(const QDateTime& when, uint16_t* time, uint16_t* date)
{
    const QDateTime local = (when.isValid() ? when : QDateTime::currentDateTime()).toLocalTime();
    const QDate d = local.date();
    const QTime t = local.time();
    if (d.year() < 1980) {
        *date = (0 << 9) | (1 << 5) | 1; // 1980-01-01, the earliest DOS date
        *time = 0;
        return;
    }
    const int year = std::min(d.year(), 2107);
    *date = uint16_t(((year - 1980) << 9) | (d.month() << 5) | d.day());
    *time = uint16_t((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2));
}

QByteArray makeComment(quint64 generation, uint32_t crc)
{
    QByteArray c = kCommentPrefix;
    c += QByteArray::number(generation, 16).rightJustified(16, '0');
    c += "; crc ";
    c += QByteArray::number(crc, 16).rightJustified(8, '0');
    return c;
}

bool parseComment(const QByteArray& c, quint64* generation, uint32_t* crc)
{
    if (c.size() != kCommentLength || !c.startsWith(kCommentPrefix)
        || c.mid(kCommentPrefix.size() + 16, 6) != "; crc ") {
        return false;
    }
    bool ok1 = false;
    bool ok2 = false;
    *generation = c.mid(kCommentPrefix.size(), 16).toULongLong(&ok1, 16);
    *crc = c.right(8).toUInt(&ok2, 16);
    return ok1 && ok2;
}

/// Local file header + name + extra fields. Sizes >= 4 GiB use a ZIP64 extra field.
QByteArray localHeader(const QByteArray& name, qint64 size, uint32_t crc, uint16_t time,
    uint16_t date, const QByteArray& privateExtra = {})
{
    const bool zip64 = uint64_t(size) >= kMax32;
    QByteArray extra;
    if (zip64) {
        put16(extra, 0x0001);
        put16(extra, 16);
        put64(extra, uint64_t(size));
        put64(extra, uint64_t(size));
    }
    extra += privateExtra;
    QByteArray h;
    h.reserve(kLocalHeaderSize + name.size() + extra.size());
    put32(h, kLocalSig);
    put16(h, zip64 ? kVersionZip64 : kVersionDefault);
    put16(h, kFlagUtf8);
    put16(h, 0); // method: stored
    put16(h, time);
    put16(h, date);
    put32(h, crc);
    put32(h, zip64 ? kMax32 : uint32_t(size));
    put32(h, zip64 ? kMax32 : uint32_t(size));
    put16(h, uint16_t(name.size()));
    put16(h, uint16_t(extra.size()));
    h += name;
    h += extra;
    return h;
}

/// Central directory header. ZIP64 extra field values appear only for fields that overflow,
/// in APPNOTE order (uncompressed size, compressed size, local header offset).
QByteArray centralHeader(const ContainerEntry& e)
{
    const QByteArray name = e.name.toUtf8();
    const bool bigSize = uint64_t(e.size) >= kMax32;
    const bool bigOffset = uint64_t(e.headerOffset) >= kMax32;
    QByteArray extra;
    if (bigSize || bigOffset) {
        put16(extra, 0x0001);
        put16(extra, uint16_t((bigSize ? 16 : 0) + (bigOffset ? 8 : 0)));
        if (bigSize) {
            put64(extra, uint64_t(e.size));
            put64(extra, uint64_t(e.size));
        }
        if (bigOffset)
            put64(extra, uint64_t(e.headerOffset));
    }
    QByteArray h;
    h.reserve(kCentralHeaderSize + name.size() + extra.size());
    put32(h, kCentralSig);
    put16(h, kVersionMadeBy);
    put16(h, (bigSize || bigOffset) ? kVersionZip64 : kVersionDefault);
    put16(h, kFlagUtf8);
    put16(h, 0);
    put16(h, e.dosTime);
    put16(h, e.dosDate);
    put32(h, e.crc32);
    put32(h, bigSize ? kMax32 : uint32_t(e.size));
    put32(h, bigSize ? kMax32 : uint32_t(e.size));
    put16(h, uint16_t(name.size()));
    put16(h, uint16_t(extra.size()));
    put16(h, 0); // comment
    put16(h, 0); // disk
    put16(h, 0); // internal attributes
    put32(h, kExternalAttributes);
    put32(h, bigOffset ? kMax32 : uint32_t(e.headerOffset));
    h += name;
    h += extra;
    return h;
}

bool allZero(const char* p, qint64 n)
{
    // Word-wise test (vectorised by the compiler); the tail byte-wise.
    qint64 i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        if (w)
            return false;
    }
    for (; i < n; ++i) {
        if (p[i])
            return false;
    }
    return true;
}

void setError(QString* error, const QString& text)
{
    if (error)
        *error = text;
}

} // namespace

// ---- Helpers -----------------------------------------------------------------------------------

QDateTime ContainerEntry::modified() const
{
    if (dosDate == 0)
        return {};
    const QDate d(1980 + (dosDate >> 9), (dosDate >> 5) & 0x0F, dosDate & 0x1F);
    const QTime t(dosTime >> 11, (dosTime >> 5) & 0x3F, (dosTime & 0x1F) * 2);
    return QDateTime(d, t);
}

bool isValidEntryName(const QString& name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QChar(0)) || name.toUtf8().size() > 0xFFFF) {
        return false;
    }
    for (const QStringView part : QStringView(name).split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == u"." || part == u"..")
            return false;
    }
    return true;
}

bool syncFile(QFile& file)
{
    if (!file.flush())
        return false;
#if defined(Q_OS_LINUX)
    return ::fdatasync(file.handle()) == 0;
#elif defined(Q_OS_UNIX)
    return ::fsync(file.handle()) == 0;
#elif defined(Q_OS_WIN)
    return ::_commit(file.handle()) == 0;
#else
    return true;
#endif
}

void syncDirectory(const QString& dir)
{
#if defined(Q_OS_UNIX)
    const int fd = ::open(QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    Q_UNUSED(dir); // NTFS renames are journaled; nothing portable to do
#endif
}

const QByteArray& Container::mimeType()
{
    static const QByteArray type = QByteArrayLiteral("application/x-kinoklip");
    return type;
}

// ---- Opening -----------------------------------------------------------------------------------

struct Container::State
{
    QVector<ContainerEntry> entries;
    quint64 generation = 0;
    qint64 end = 0;
};

Container::~Container() = default;

std::unique_ptr<Container> Container::create(const QString& path, QString* error)
{
    std::unique_ptr<Container> c(new Container);
    c->m_path = path;
    c->m_mode = Mode::ReadWrite;
    c->m_file.setFileName(path);
    if (!c->m_file.open(QIODevice::ReadWrite | QIODevice::Truncate | QIODevice::Unbuffered)) {
        setError(error, QStringLiteral("Cannot create %1: %2").arg(path, c->m_file.errorString()));
        return {};
    }
    c->m_tailCut = true;
    ContainerEntry e;
    e.name = QString::fromLatin1(kMimeName);
    e.size = mimeType().size();
    e.crc32 = crc32Update(0, mimeType().constData(), size_t(mimeType().size()));
    toDos(QDateTime::currentDateTime(), &e.dosTime, &e.dosDate);
    QByteArray slotField;
    put16(slotField, kHeaderExtraId);
    put16(slotField, kSlotSize * kSlotCount);
    slotField += QByteArray(kSlotSize * kSlotCount, '\0'); // both slots invalid until the commit
    const QByteArray header
        = localHeader(kMimeName, e.size, e.crc32, e.dosTime, e.dosDate, slotField);
    Q_ASSERT(header.size() == kSlotsOffset + kSlotSize * kSlotCount);
    if (!c->writeAt(0, header + mimeType(), error))
        return {};
    e.headerOffset = 0;
    e.dataOffset = header.size();
    c->m_writeEnd = e.dataOffset + e.size;
    c->putEntry(e);
    c->m_pending = true;
    if (!c->commit(error))
        return {};
    return c;
}

std::unique_ptr<Container> Container::open(const QString& path, Mode mode, QString* error)
{
    std::unique_ptr<Container> c(new Container);
    c->m_path = path;
    c->m_mode = mode;
    c->m_file.setFileName(path);
    const QIODevice::OpenMode flags
        = (mode == Mode::ReadWrite ? QIODevice::ReadWrite : QIODevice::ReadOnly)
        | QIODevice::Unbuffered;
    if (!QFileInfo(path).isFile() || !c->m_file.open(flags)) {
        setError(error, QStringLiteral("Cannot open %1: %2").arg(path, c->m_file.errorString()));
        return {};
    }
    if (!c->findCommittedState(error))
        return {};
    return c;
}

bool Container::looksLikeContainer(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(kSlotsOffset + kSlotSize * kSlotCount + mimeType().size());
    return head.size() == kSlotsOffset + kSlotSize * kSlotCount + mimeType().size()
        && get32(head.constData()) == kLocalSig && head.mid(kLocalHeaderSize, 8) == kMimeName
        && get16(head.constData() + kLocalHeaderSize + 8) == kHeaderExtraId
        && head.endsWith(mimeType());
}

bool Container::readAt(qint64 offset, char* dst, qint64 size) const
{
    if (offset < 0 || !m_file.seek(offset))
        return false;
    qint64 done = 0;
    while (done < size) {
        const qint64 n = m_file.read(dst + done, size - done);
        if (n <= 0)
            return false;
        done += n;
    }
    return true;
}

bool Container::loadState(qint64 eocdPos, quint64 expectedGeneration, State* state) const
{
    const qint64 fileSize = m_file.size();
    if (eocdPos < kLocatorSize + kEocd64Size || eocdPos + kEocdSize + kCommentLength > fileSize)
        return false;
    QByteArray eocd(kEocdSize + kCommentLength, Qt::Uninitialized);
    if (!readAt(eocdPos, eocd.data(), eocd.size()) || get32(eocd.constData()) != kEocdSig
        || get16(eocd.constData() + 20) != kCommentLength) {
        return false;
    }
    quint64 generation = 0;
    uint32_t recordsCrc = 0;
    if (!parseComment(eocd.mid(kEocdSize), &generation, &recordsCrc)
        || (expectedGeneration != 0 && generation != expectedGeneration)) {
        return false;
    }
    char locator[kLocatorSize];
    const qint64 locatorPos = eocdPos - kLocatorSize;
    if (!readAt(locatorPos, locator, kLocatorSize) || get32(locator) != kLocatorSig
        || qint64(get64(locator + 8)) != locatorPos - kEocd64Size) {
        return false;
    }
    const qint64 eocd64Pos = locatorPos - kEocd64Size;
    char eocd64[kEocd64Size];
    if (!readAt(eocd64Pos, eocd64, kEocd64Size) || get32(eocd64) != kEocd64Sig)
        return false;
    const uint64_t count = get64(eocd64 + 32);
    const qint64 cdSize = qint64(get64(eocd64 + 40));
    const qint64 cdOffset = qint64(get64(eocd64 + 48));
    if (cdSize < 0 || cdSize > kMaxCentralDirectory || cdOffset < 0
        || cdOffset + cdSize != eocd64Pos || count > uint64_t(cdSize / kCentralHeaderSize)) {
        return false;
    }
    // Central directory + ZIP64 records, checked against the CRC in the comment.
    QByteArray records(eocdPos - cdOffset, Qt::Uninitialized);
    if (!readAt(cdOffset, records.data(), records.size())
        || crc32Update(0, records.constData(), size_t(records.size())) != recordsCrc) {
        return false;
    }
    QVector<ContainerEntry> entries;
    entries.reserve(qsizetype(count));
    qint64 p = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (p + kCentralHeaderSize > cdSize)
            return false;
        const char* h = records.constData() + p;
        if (get32(h) != kCentralSig || get16(h + 10) != 0) // stored entries only
            return false;
        ContainerEntry e;
        e.dosTime = get16(h + 12);
        e.dosDate = get16(h + 14);
        e.crc32 = get32(h + 16);
        uint64_t size = get32(h + 24);
        uint64_t compressed = get32(h + 20);
        const int nameLen = get16(h + 28);
        const int extraLen = get16(h + 30);
        const int commentLen = get16(h + 32);
        uint64_t offset = get32(h + 42);
        if (p + kCentralHeaderSize + nameLen + extraLen + commentLen > cdSize)
            return false;
        const QByteArray name(h + kCentralHeaderSize, nameLen);
        const char* x = h + kCentralHeaderSize + nameLen;
        for (int q = 0; q + 4 <= extraLen;) {
            const uint16_t id = get16(x + q);
            const int len = get16(x + q + 2);
            if (q + 4 + len > extraLen)
                return false;
            if (id == 0x0001) {
                const char* v = x + q + 4;
                const char* vEnd = v + len;
                if (size == kMax32 && v + 8 <= vEnd) {
                    size = get64(v);
                    v += 8;
                }
                if (compressed == kMax32 && v + 8 <= vEnd) {
                    compressed = get64(v);
                    v += 8;
                }
                if (offset == kMax32 && v + 8 <= vEnd)
                    offset = get64(v);
            }
            q += 4 + len;
        }
        if (size != compressed)
            return false;
        e.name = QString::fromUtf8(name);
        e.size = qint64(size);
        e.headerOffset = qint64(offset);
        // Local header: same name; its extra field length gives the data offset.
        QByteArray local(kLocalHeaderSize + nameLen, Qt::Uninitialized);
        if (e.size < 0 || e.headerOffset < 0 || !readAt(e.headerOffset, local.data(), local.size())
            || get32(local.constData()) != kLocalSig || get16(local.constData() + 26) != nameLen
            || local.mid(kLocalHeaderSize) != name) {
            return false;
        }
        e.dataOffset = e.headerOffset + kLocalHeaderSize + nameLen + get16(local.constData() + 28);
        if (e.dataOffset + e.size > cdOffset)
            return false;
        entries.append(e);
        p += kCentralHeaderSize + nameLen + extraLen + commentLen;
    }
    if (p != cdSize || entries.isEmpty() || entries.front().name != QLatin1String(kMimeName)
        || entries.front().headerOffset != 0) {
        return false;
    }
    for (const ContainerEntry& e : entries) {
        if (e.size > kCheckedEntryLimit || e.name.startsWith(QLatin1String("media/")))
            continue;
        QString ignored;
        if (read(e, &ignored).isNull())
            return false;
    }
    state->entries = std::move(entries);
    state->generation = generation;
    state->end = eocdPos + kEocdSize + kCommentLength;
    return true;
}

bool Container::findCommittedState(QString* error)
{
    const qint64 fileSize = m_file.size();
    State state;
    // 1. The normal case: the file ends with a valid end record.
    bool found = loadState(fileSize - kEocdSize - kCommentLength, 0, &state);
    m_recovery = Recovery::Clean;
    // 2. A save was cut short: the header slots in the first entry point to the last commit.
    if (!found) {
        struct Slot
        {
            quint64 generation;
            qint64 eocdPos;
            qint64 end;
        };
        QVector<Slot> candidates;
        QByteArray head(kSlotsOffset + kSlotSize * kSlotCount, Qt::Uninitialized);
        if (readAt(0, head.data(), head.size()) && get32(head.constData()) == kLocalSig
            && get16(head.constData() + kLocalHeaderSize + 8) == kHeaderExtraId) {
            for (int i = 0; i < kSlotCount; ++i) {
                const char* s = head.constData() + kSlotsOffset + i * kSlotSize;
                if (QByteArray(s, 4) != kSlotMagic
                    || get32(s + 28) != crc32Update(0, s, kSlotSize - 4)) {
                    continue;
                }
                candidates.append({get64(s + 4), qint64(get64(s + 12)), qint64(get64(s + 20))});
            }
        }
        std::sort(candidates.begin(), candidates.end(),
            [](const Slot& a, const Slot& b) { return a.generation > b.generation; });
        for (const Slot& s : candidates) {
            if (s.end == s.eocdPos + kEocdSize + kCommentLength
                && loadState(s.eocdPos, s.generation, &state)) {
                found = true;
                m_recovery = Recovery::HeaderSlot;
                break;
            }
        }
    }
    // 3. Last resort: scan backwards for our end record signature.
    if (!found) {
        constexpr qint64 kChunk = 1024 * 1024;
        const QByteArray sig("PK\x05\x06", 4);
        QByteArray buf;
        qint64 hi = fileSize; // search [lo, hi)
        while (!found && hi > 0) {
            const qint64 lo = std::max<qint64>(0, hi - kChunk);
            // Read 3 bytes past hi so a signature straddling two chunks is found.
            const qint64 len = std::min<qint64>(hi + 3, fileSize) - lo;
            buf.resize(len);
            if (!readAt(lo, buf.data(), len))
                break;
            for (qint64 at = buf.lastIndexOf(sig); at >= 0 && !found;
                 at = at > 0 ? buf.lastIndexOf(sig, at - 1) : -1) {
                if (lo + at < hi && loadState(lo + at, 0, &state))
                    found = true;
            }
            hi = lo;
        }
        m_recovery = Recovery::Scan;
    }
    if (!found) {
        setError(error,
            QStringLiteral("%1 is not a Kinoklip project file or is damaged beyond repair")
                .arg(QFileInfo(m_path).fileName()));
        return false;
    }
    m_entries.clear();
    m_index.clear();
    for (const ContainerEntry& e : state.entries)
        putEntry(e);
    m_committedEntries = m_entries;
    m_generation = state.generation;
    m_committedEnd = state.end;
    m_writeEnd = state.end;
    m_ignoredTail = fileSize - state.end;
    m_pending = false;
    return true;
}

// ---- Reading -----------------------------------------------------------------------------------

const ContainerEntry* Container::find(const QString& name) const
{
    const auto it = m_index.constFind(name);
    return it == m_index.cend() ? nullptr : &m_entries[*it];
}

qint64 Container::liveSize() const
{
    qint64 total = kEocd64Size + kLocatorSize + kEocdSize + kCommentLength;
    for (const ContainerEntry& e : m_entries)
        total += e.footprint() + centralHeader(e).size();
    return total;
}

QByteArray Container::read(const ContainerEntry& entry, QString* error) const
{
    constexpr qint64 kMaxInMemory = 1LL << 30;
    if (entry.size > kMaxInMemory) {
        setError(error, QStringLiteral("%1 is too large to read into memory").arg(entry.name));
        return {};
    }
    QByteArray data(entry.size, Qt::Uninitialized);
    if (!readAt(entry.dataOffset, data.data(), entry.size)) {
        setError(error, QStringLiteral("Cannot read %1 from %2").arg(entry.name, m_path));
        return {};
    }
    if (crc32Update(0, data.constData(), size_t(data.size())) != entry.crc32) {
        setError(
            error, QStringLiteral("%1 in %2 is damaged (CRC mismatch)").arg(entry.name, m_path));
        return {};
    }
    if (data.isNull())
        data = QByteArray(""); // empty but not null: success
    return data;
}

// ---- Writing -----------------------------------------------------------------------------------

void Container::putEntry(const ContainerEntry& entry)
{
    const auto it = m_index.constFind(entry.name);
    if (it != m_index.cend()) {
        m_entries.removeAt(*it);
        m_index.clear();
        for (int i = 0; i < m_entries.size(); ++i)
            m_index.insert(m_entries[i].name, i);
    }
    m_index.insert(entry.name, int(m_entries.size()));
    m_entries.append(entry);
}

bool Container::prepareWrite(QString* error)
{
    if (m_mode != Mode::ReadWrite) {
        setError(error, QStringLiteral("%1 is open read-only").arg(m_path));
        return false;
    }
    if (!m_tailCut) {
        // Bytes after the committed state are a torn save: nothing refers to them.
        if (m_file.size() > m_committedEnd && !m_file.resize(m_committedEnd)) {
            setError(
                error, QStringLiteral("Cannot repair %1: %2").arg(m_path, m_file.errorString()));
            return false;
        }
        m_tailCut = true;
    }
    return true;
}

bool Container::writeAt(qint64 offset, const QByteArray& bytes, QString* error)
{
    if (!m_file.seek(offset) || m_file.write(bytes) != bytes.size()) {
        setError(error, QStringLiteral("Cannot write %1: %2").arg(m_path, m_file.errorString()));
        return false;
    }
    return true;
}

bool Container::addData(
    const QString& name, const QByteArray& data, const QDateTime& modified, QString* error)
{
    if (!prepareWrite(error))
        return false;
    if (!isValidEntryName(name)) {
        setError(error, QStringLiteral("Invalid entry name: %1").arg(name));
        return false;
    }
    ContainerEntry e;
    e.name = name;
    e.size = data.size();
    e.crc32 = crc32Update(0, data.constData(), size_t(data.size()));
    toDos(modified, &e.dosTime, &e.dosDate);
    e.headerOffset = m_writeEnd;
    const QByteArray header = localHeader(name.toUtf8(), e.size, e.crc32, e.dosTime, e.dosDate);
    if (!writeAt(e.headerOffset, header + data, error))
        return false;
    e.dataOffset = e.headerOffset + header.size();
    m_writeEnd = e.dataOffset + e.size;
    putEntry(e);
    m_pending = true;
    return true;
}

Status Container::addFile(const QString& name, const QString& sourcePath, const QDateTime& modified,
    const Progress& progress, QString* error, qint64 sourceOffset, qint64 sourceSize,
    uint32_t* crcOut, qint64 progressBase, qint64 progressTotal)
{
    if (!prepareWrite(error))
        return Status::Failed;
    if (!isValidEntryName(name)) {
        setError(error, QStringLiteral("Invalid entry name: %1").arg(name));
        return Status::Failed;
    }
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot read %1: %2").arg(sourcePath, source.errorString()));
        return Status::Failed;
    }
    const qint64 size = sourceSize >= 0 ? sourceSize : source.size() - sourceOffset;
    if (size < 0 || !source.seek(sourceOffset)) {
        setError(error, QStringLiteral("Cannot read %1").arg(sourcePath));
        return Status::Failed;
    }
    ContainerEntry e;
    e.name = name;
    e.size = size;
    toDos(modified, &e.dosTime, &e.dosDate);
    e.headerOffset = m_writeEnd;
    const QByteArray header = localHeader(name.toUtf8(), size, 0, e.dosTime, e.dosDate);
    if (!writeAt(e.headerOffset, header, error))
        return Status::Failed;
    e.dataOffset = e.headerOffset + header.size();

    const auto abandon = [this, &e] {
        // Nothing points at the partial copy: cut it off again.
        m_file.resize(std::max(m_committedEnd, e.headerOffset));
    };
    std::vector<char> buffer(size_t(std::min(kCopyChunk, std::max<qint64>(size, 1))));
    uint32_t crc = 0;
    qint64 done = 0;
    while (done < size) {
        const qint64 want = std::min<qint64>(qint64(buffer.size()), size - done);
        qint64 got = 0;
        while (got < want) {
            const qint64 n = source.read(buffer.data() + got, want - got);
            if (n <= 0)
                break;
            got += n;
        }
        if (got != want) {
            abandon();
            setError(error,
                QStringLiteral("%1 changed or became unreadable while copying")
                    .arg(QFileInfo(sourcePath).fileName()));
            return Status::Failed;
        }
        crc = crc32Update(crc, buffer.data(), size_t(want));
        // A run of zeros becomes a hole: the file is extended (or written after) later.
        if (!allZero(buffer.data(), want)
            && !writeAt(e.dataOffset + done, QByteArray::fromRawData(buffer.data(), want), error)) {
            abandon();
            return Status::Failed;
        }
        done += want;
        if (progress && !progress(progressBase + done, progressTotal > 0 ? progressTotal : size)) {
            abandon();
            setError(error, QStringLiteral("Cancelled"));
            return Status::Cancelled;
        }
    }
    const qint64 end = e.dataOffset + size;
    if (m_file.size() < end && !m_file.resize(end)) {
        abandon();
        setError(error, QStringLiteral("Cannot write %1: %2").arg(m_path, m_file.errorString()));
        return Status::Failed;
    }
    QByteArray crcBytes;
    put32(crcBytes, crc);
    if (!writeAt(e.headerOffset + 14, crcBytes, error)) {
        abandon();
        return Status::Failed;
    }
    e.crc32 = crc;
    if (crcOut)
        *crcOut = crc;
    m_writeEnd = end;
    putEntry(e);
    m_pending = true;
    return Status::Ok;
}

bool Container::remove(const QString& name)
{
    const auto it = m_index.constFind(name);
    if (it == m_index.cend() || name == QLatin1String(kMimeName))
        return false;
    m_entries.removeAt(*it);
    m_index.clear();
    for (int i = 0; i < m_entries.size(); ++i)
        m_index.insert(m_entries[i].name, i);
    m_pending = true;
    return true;
}

bool Container::writeHeaderSlot(quint64 generation, qint64 eocdPos, qint64 end, QString* error)
{
    QByteArray slot = kSlotMagic;
    put64(slot, generation);
    put64(slot, uint64_t(eocdPos));
    put64(slot, uint64_t(end));
    put32(slot, crc32Update(0, slot.constData(), size_t(slot.size())));
    Q_ASSERT(slot.size() == kSlotSize);
    return writeAt(kSlotsOffset + qint64(generation % kSlotCount) * kSlotSize, slot, error);
}

bool Container::commit(QString* error)
{
    if (!prepareWrite(error))
        return false;
    if (m_file.size() < m_writeEnd && !m_file.resize(m_writeEnd)) {
        setError(error, QStringLiteral("Cannot write %1: %2").arg(m_path, m_file.errorString()));
        return false;
    }
    // 1. The new entries' bytes are on disk before anything points at them.
    if (!syncFile(m_file)) {
        setError(error, QStringLiteral("Cannot flush %1 to disk").arg(m_path));
        return false;
    }
    // 2. Central directory, ZIP64 end record and locator, end record with the checksum comment.
    const qint64 cdOffset = m_writeEnd;
    QByteArray records;
    for (const ContainerEntry& e : m_entries)
        records += centralHeader(e);
    const qint64 cdSize = records.size();
    const qint64 eocd64Pos = cdOffset + cdSize;
    const uint64_t count = uint64_t(m_entries.size());
    put32(records, kEocd64Sig);
    put64(records, kEocd64Size - 12); // size of the rest of the record
    put16(records, kVersionMadeBy);
    put16(records, kVersionZip64);
    put32(records, 0); // this disk
    put32(records, 0); // disk with the central directory
    put64(records, count);
    put64(records, count);
    put64(records, uint64_t(cdSize));
    put64(records, uint64_t(cdOffset));
    put32(records, kLocatorSig);
    put32(records, 0);
    put64(records, uint64_t(eocd64Pos));
    put32(records, 1); // total disks
    const qint64 eocdPos = cdOffset + records.size();
    const quint64 generation = m_generation + 1;
    const uint32_t recordsCrc = crc32Update(0, records.constData(), size_t(records.size()));
    put32(records, kEocdSig);
    put16(records, 0);
    put16(records, 0);
    put16(records, uint16_t(std::min<uint64_t>(count, 0xFFFF)));
    put16(records, uint16_t(std::min<uint64_t>(count, 0xFFFF)));
    put32(records, uint32_t(std::min<uint64_t>(uint64_t(cdSize), kMax32)));
    put32(records, uint32_t(std::min<uint64_t>(uint64_t(cdOffset), kMax32)));
    const QByteArray comment = makeComment(generation, recordsCrc);
    Q_ASSERT(comment.size() == kCommentLength);
    put16(records, uint16_t(comment.size()));
    records += comment;
    const qint64 end = cdOffset + records.size();
    if (m_crashAfter >= 0) {
        writeAt(cdOffset, records.left(qsizetype(std::min<qint64>(m_crashAfter, records.size()))),
            nullptr);
        m_file.close();
        setError(error, QStringLiteral("simulated crash"));
        return false;
    }
    if (!writeAt(cdOffset, records, error))
        return false;
    if (!syncFile(m_file)) {
        setError(error, QStringLiteral("Cannot flush %1 to disk").arg(m_path));
        return false;
    }
    // 3. The header slot of this generation (the other slot keeps the previous commit).
    if (!writeHeaderSlot(generation, eocdPos, end, error) || !syncFile(m_file)) {
        setError(error, QStringLiteral("Cannot flush %1 to disk").arg(m_path));
        return false;
    }
    m_generation = generation;
    m_committedEnd = end;
    m_writeEnd = end;
    m_committedEntries = m_entries;
    m_pending = false;
    return true;
}

void Container::rollback()
{
    m_entries.clear();
    m_index.clear();
    for (const ContainerEntry& e : std::as_const(m_committedEntries))
        putEntry(e);
    m_writeEnd = m_committedEnd;
    m_pending = false;
    if (m_mode == Mode::ReadWrite && m_tailCut && m_file.size() > m_committedEnd)
        m_file.resize(m_committedEnd);
}

} // namespace kk::pack
