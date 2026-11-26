// SPDX-License-Identifier: GPL-3.0-or-later
// CloneTool — the Media page's Clone Tool (Resolve: Media page > Clone Tool): copies camera
// cards and other folders to one or more destinations and proves every copy with a checksum.
// Library only (QtCore): the Clone Tool dialog, the command line and the MCP server call it.
//
// One job: each source (a folder or a file) is read once; every block read goes to every
// destination and into the checksum. After that each copy is read back and its checksum is
// compared with the source's (Verify). A checksum list "<source name>.<algorithm>" in the
// md5sum / sha1sum / sha256sum format is written next to each copy (`md5sum -c` checks it).
// Nothing is overwritten: a file that already exists in a destination counts as failed.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace kk::clone {

enum class Checksum {
    Md5,
    Sha1,
    Sha256,
    Crc32
};

/// "MD5", "SHA-1", "SHA-256", "CRC-32" (the list in the dialog) and the list file suffix.
[[nodiscard]] QString checksumName(Checksum c);
[[nodiscard]] QString checksumSuffix(Checksum c);
/// The command line spelling ("md5", "sha1", "sha256", "crc32"); false when unknown.
[[nodiscard]] bool parseChecksum(const QString& text, Checksum& out);

struct Job
{
    QStringList sources; ///< folders and files
    QStringList destinations; ///< folders (created when missing); at least one
    Checksum checksum = Checksum::Md5;
    bool verify = true; ///< read every copy back and compare
    bool writeList = true; ///< write the checksum list next to each copy
};

struct FileResult
{
    QString relative; ///< path under the destination, e.g. "A001/clip.mov"
    qint64 size = 0;
    QString checksum; ///< of the source, lower-case hex
    /// One entry per destination that failed: "<destination>: <reason>".
    QStringList failures;
};

struct Result
{
    bool ok = false; ///< every file reached every destination (and verified)
    bool cancelled = false;
    QString error; ///< job level problem (no source, no destination, ...)
    int files = 0;
    qint64 bytes = 0; ///< bytes of the sources (once)
    int verified = 0; ///< copies read back and found equal
    QStringList failures; ///< all failures, human readable
    QList<FileResult> results;
};

/// Called with the bytes handled and the bytes planned (all copies and read-backs together);
/// return false to cancel.
using Progress = std::function<bool(qint64 done, qint64 total)>;

/// The files a job would copy (relative to the destination), in copy order, with their size.
[[nodiscard]] QList<FileResult> plan(const Job& job, QString* error = nullptr);

/// Runs the job on the calling thread (call it from a worker thread in the GUI).
Result run(const Job& job, const Progress& progress = {});

/// The checksum of a file as lower-case hex, empty when it cannot be read.
[[nodiscard]] QString fileChecksum(const QString& path, Checksum c);

} // namespace kk::clone
