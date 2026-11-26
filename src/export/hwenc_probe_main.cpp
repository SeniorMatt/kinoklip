// SPDX-License-Identifier: GPL-3.0-or-later
// kinoklip-hwenc-probe <encoder> <result file> — opens one hardware encoder (h264_vaapi, hevc_nvenc,
// ...) at a small size, encodes a frame and writes the answer as JSON. Kinoklip runs it the first
// time hardware encoding is needed, never at start-up: a driver that crashes or hangs here (an
// AMD VCN timeout, issue 18) kills only this process and the encoder is marked unavailable with
// the reason (spec 160).
//
// KK_HWENC_PROBE_TEST=crash|hang|exit makes it misbehave on purpose (the tests use it).
#include "export/EncoderCatalog.h"

#include <QCoreApplication>
#include <QFile>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) {
        std::fprintf(stderr, "usage: kinoklip-hwenc-probe <encoder> <result file>\n");
        return 2;
    }
    const QByteArray test = qgetenv("KK_HWENC_PROBE_TEST");
    if (test == "crash")
        std::abort();
    if (test == "hang") {
        for (;;)
            ::pause();
    }
    if (test == "exit")
        return 7;
    const QByteArray answer = kk::EncoderCatalog::probeHere(QString::fromLocal8Bit(argv[1]));
    QFile out(QString::fromLocal8Bit(argv[2]));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 3;
    out.write(answer);
    out.close();
    // Leave without running the drivers' static destructors (some crash at exit).
    std::fflush(nullptr);
    std::_Exit(0);
}
