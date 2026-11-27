// SPDX-License-Identifier: GPL-3.0-or-later
// HDR export (1.13 Deliver: HDR export, first slice): Rec.2020 / PQ / HLG settings and their
// JSON form (old files keep loading), the transfer functions and the SDR to HDR conversion,
// the encoder parameter strings, and the encoders themselves: 10-bit pixel format, colour tags
// and HDR10 side data on the AVCodecContext; codecs that cannot carry HDR are refused.
#include "export/EncoderCatalog.h"
#include "export/ExportSettings.h"
#include "export/HdrConvert.h"
#include "export/VideoEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <QImage>
#include <QJsonDocument>
#include <QTest>

using namespace kk;
using namespace kk::exporter;

namespace {

ExportSettings hdrSettings(const QString& codec, GammaTag gamma)
{
    ExportSettings s;
    s.outputPath = QStringLiteral("/tmp/out.mp4");
    s.video.codec = codec;
    s.video.gamma = gamma;
    s.video.colorSpace = ColorSpaceTag::Rec2020;
    return s;
}

bool hasEncoder(const char* name)
{
    return avcodec_find_encoder_by_name(name) != nullptr;
}

const AVPacketSideData* sideData(const AVCodecContext* ctx, AVPacketSideDataType type)
{
    return av_packet_side_data_get(ctx->coded_side_data, ctx->nb_coded_side_data, type);
}

/// Opens @p encoder for @p video at @p size, encodes a few grey frames and drains.
/// @return the opened context's data through @p enc; false with @p error.
bool openAndEncode(VideoEncoder& enc, const QString& encoder, const VideoExportSettings& video,
    QSize size, int* packets, QString* error)
{
    VideoEncoderConfig config;
    config.encoderName = encoder;
    config.backend = EncoderBackend::Software;
    config.settings = video;
    config.size = size;
    config.fps = Rational {25, 1};
    if (!enc.open(config, error))
        return false;
    QImage frame(size, QImage::Format_ARGB32_Premultiplied);
    frame.fill(qRgb(128, 128, 128));
    int count = 0;
    const PacketSink sink = [&](AVPacket*) {
        ++count;
        return true;
    };
    for (int i = 0; i < 4; ++i) {
        if (!enc.encode(frame, i, sink, error))
            return false;
    }
    if (!enc.flush(sink, error))
        return false;
    *packets = count;
    return true;
}

} // namespace

class TestHdrExport : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void settingsRoundTrip()
    {
        ExportSettings s = hdrSettings(QStringLiteral("hevc"), GammaTag::Pq);
        s.video.hdr.maxCll = 900;
        s.video.hdr.maxFall = 300;
        s.video.hdr.masterMaxNits = 4000.0;
        const QByteArray json = QJsonDocument(s.toJson()).toJson();
        const ExportSettings back
            = ExportSettings::fromJson(QJsonDocument::fromJson(json).object(), ExportSettings {});
        QCOMPARE(back.video.colorSpace, ColorSpaceTag::Rec2020);
        QCOMPARE(back.video.gamma, GammaTag::Pq);
        QCOMPARE(back.video.hdr, s.video.hdr);
        QVERIFY(back == s);

        ExportSettings hlg = hdrSettings(QStringLiteral("av1"), GammaTag::Hlg);
        const ExportSettings hlgBack = ExportSettings::fromJson(hlg.toJson(), ExportSettings {});
        QCOMPARE(hlgBack.video.gamma, GammaTag::Hlg);
    }

    void oldFilesKeepDefaults()
    {
        // Settings of versions before 1.13 have none of the keys, and a default render does
        // not write them (older versions never see them either).
        const QJsonObject plain = ExportSettings {}.toJson();
        const QJsonObject video = plain.value(QLatin1String("video")).toObject();
        QVERIFY(!video.contains(QLatin1String("colorSpace")));
        QVERIFY(!video.contains(QLatin1String("gamma")));
        QVERIFY(!video.contains(QLatin1String("hdr")));
        const ExportSettings loaded = ExportSettings::fromJson(plain, ExportSettings {});
        QCOMPARE(loaded.video.colorSpace, ColorSpaceTag::Rec709);
        QCOMPARE(loaded.video.gamma, GammaTag::Rec709);
        QVERIFY(!loaded.video.wideGamut());

        // Unknown ids keep the base values; out-of-range metadata is clamped.
        QJsonObject odd = plain;
        QJsonObject v = video;
        v[QStringLiteral("gamma")] = QStringLiteral("dolby");
        QJsonObject h;
        h[QStringLiteral("maxCll")] = 999999;
        v[QStringLiteral("hdr")] = h;
        odd[QStringLiteral("video")] = v;
        const ExportSettings oddLoaded = ExportSettings::fromJson(odd, ExportSettings {});
        QCOMPARE(oddLoaded.video.gamma, GammaTag::Rec709);
        QCOMPARE(oddLoaded.video.hdr.maxCll, 10000);
    }

    void masteringDisplayPrimaries()
    {
        ExportSettings s = hdrSettings(QStringLiteral("hevc"), GammaTag::Pq);
        QCOMPARE(s.video.hdr.primaries, MasterPrimaries::P3D65);
        QVERIFY(x265MasterDisplay(s.video.hdr).startsWith("G(13250,34500)B(7500,3000)R(34000,16000)"));
        s.video.hdr.primaries = MasterPrimaries::Rec2020;
        QVERIFY(x265MasterDisplay(s.video.hdr).startsWith("G(8500,39850)B(6550,2300)R(35400,14600)"));
        QVERIFY(x265HdrParams(s.video).contains("G(8500,39850)"));
        QVERIFY(svtAv1HdrParams(s.video).contains("R(0.7080,0.2920)"));
        const ExportSettings back = ExportSettings::fromJson(s.toJson(), ExportSettings {});
        QCOMPARE(back.video.hdr.primaries, MasterPrimaries::Rec2020);
        QCOMPARE(masterPrimariesFromId(QStringLiteral("nonsense"), MasterPrimaries::P3D65),
            MasterPrimaries::P3D65);
    }

    void onlyHevcAv1AndProResCarryHdr()
    {
        QVERIFY(codecCarriesHdr(QStringLiteral("hevc")));
        QVERIFY(codecCarriesHdr(QStringLiteral("av1")));
        QVERIFY(codecCarriesHdr(QStringLiteral("prores")));
        for (const char* id : {"h264", "vp9", "dnxhr", "png", "exr", "gif"})
            QVERIFY2(!codecCarriesHdr(QString::fromLatin1(id)), id);

        ExportSettings ok = hdrSettings(QStringLiteral("hevc"), GammaTag::Pq);
        QString why;
        QVERIFY2(ok.validate(&why), qPrintable(why));

        ExportSettings bad = hdrSettings(QStringLiteral("h264"), GammaTag::Pq);
        QVERIFY(!bad.validate(&why));
        QVERIFY(!why.isEmpty());
        bad.normalize(); // the Deliver page does this: the tags fall back to Rec.709
        QCOMPARE(bad.video.gamma, GammaTag::Rec709);
        QCOMPARE(bad.video.colorSpace, ColorSpaceTag::Rec709);
        QVERIFY(bad.validate(&why));

        // PQ and HLG imply Rec.2020.
        ExportSettings tagged = hdrSettings(QStringLiteral("hevc"), GammaTag::Hlg);
        tagged.video.colorSpace = ColorSpaceTag::Rec709;
        QVERIFY(!tagged.validate(&why));
        tagged.normalize();
        QCOMPARE(tagged.video.colorSpace, ColorSpaceTag::Rec2020);
    }

    void transferFunctions()
    {
        // BT.2408: 203 nits is PQ signal 0.58 (code 58 %), HLG reference white is 0.75.
        QVERIFY(qAbs(pqEncode(kHdrReferenceWhiteNits) - 0.5807) < 0.002);
        QVERIFY(qAbs(pqEncode(10000.0) - 1.0) < 1e-6);
        QCOMPARE(pqEncode(0.0) < 0.001, true);
        QVERIFY(qAbs(hlgEncode(kHlgReferenceWhiteScene) - 0.75) < 0.002);
        QVERIFY(qAbs(hlgEncode(1.0) - 1.0) < 1e-6);
        QVERIFY(qAbs(srgbEncode(srgbDecode(0.37)) - 0.37) < 1e-9);
    }

    void sdrToHdrConversion()
    {
        const SdrToHdr pq(HdrTransfer::Pq, true);
        const auto white = pq.convertPixel(255, 255, 255);
        for (uint16_t c : white)
            QVERIFY(qAbs(int(c) - int(0.5807 * 65535.0)) < 200);
        const auto black = pq.convertPixel(0, 0, 0);
        QVERIFY(black[0] < 100 && black[1] < 100 && black[2] < 100);
        // Pure sRGB red becomes a less saturated BT.2020 red: green and blue rise.
        const auto red = pq.convertPixel(255, 0, 0);
        QVERIFY(red[0] > red[1] && red[1] > black[1]);

        const SdrToHdr hlg(HdrTransfer::Hlg, true);
        QVERIFY(qAbs(int(hlg.convertPixel(255, 255, 255)[1]) - int(0.75 * 65535.0)) < 200);

        // Rec.2020 tag with the SDR curve: white and grey stay neutral and in place.
        const SdrToHdr sdr(HdrTransfer::Sdr, true);
        const auto grey = sdr.convertPixel(128, 128, 128);
        QVERIFY(qAbs(int(grey[1]) - int(128 * 257)) < 300);
        QVERIFY(qAbs(int(grey[0]) - int(grey[2])) < 100);
    }

    void encoderParameters()
    {
        VideoExportSettings v;
        QVERIFY(x265HdrParams(v).isEmpty());
        QVERIFY(svtAv1HdrParams(v).isEmpty());
        v.colorSpace = ColorSpaceTag::Rec2020;
        v.gamma = GammaTag::Pq;
        const QByteArray x265 = x265HdrParams(v);
        QVERIFY(x265.contains("hdr10=1"));
        QVERIFY(x265.contains("transfer=smpte2084"));
        QVERIFY(x265.contains("colorprim=bt2020"));
        QVERIFY(x265.contains("colormatrix=bt2020nc"));
        QVERIFY(x265.contains("master-display=G(13250,34500)B(7500,3000)R(34000,16000)"
                              "WP(15635,16450)L(10000000,1)"));
        QVERIFY(x265.contains("max-cll=1000,400"));
        const QByteArray svt = svtAv1HdrParams(v);
        QVERIFY(svt.contains("transfer-characteristics=16"));
        QVERIFY(svt.contains("content-light=1000,400"));
        QVERIFY(svt.contains("L(1000.0000,0.0001)"));
        v.gamma = GammaTag::Hlg;
        QVERIFY(x265HdrParams(v).contains("transfer=arib-std-b67"));
        QVERIFY(!x265HdrParams(v).contains("hdr10=1"));
        QVERIFY(svtAv1HdrParams(v).contains("transfer-characteristics=18"));
    }

    void x265TagsPq()
    {
        if (!hasEncoder("libx265"))
            QSKIP("libx265 is not in this FFmpeg build");
        VideoExportSettings video;
        video.codec = QStringLiteral("hevc");
        video.colorSpace = ColorSpaceTag::Rec2020;
        video.gamma = GammaTag::Pq;
        video.hdr.maxCll = 700;
        video.hdr.maxFall = 250;
        VideoEncoder enc;
        QString error;
        int packets = 0;
        QVERIFY2(openAndEncode(enc, QStringLiteral("libx265"), video, QSize(128, 72), &packets,
                     &error),
            qPrintable(error));
        QVERIFY(packets > 0);
        const AVCodecContext* ctx = enc.context();
        QCOMPARE(int(ctx->pix_fmt), int(AV_PIX_FMT_YUV420P10LE));
        QCOMPARE(int(ctx->color_primaries), int(AVCOL_PRI_BT2020));
        QCOMPARE(int(ctx->color_trc), int(AVCOL_TRC_SMPTE2084));
        QCOMPARE(int(ctx->colorspace), int(AVCOL_SPC_BT2020_NCL));
        QCOMPARE(int(ctx->color_range), int(AVCOL_RANGE_MPEG));

        const AVPacketSideData* mdcv = sideData(ctx, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
        QVERIFY(mdcv);
        const auto* m = reinterpret_cast<const AVMasteringDisplayMetadata*>(mdcv->data);
        QVERIFY(m->has_luminance && m->has_primaries);
        QVERIFY(qAbs(av_q2d(m->max_luminance) - 1000.0) < 0.01);
        QVERIFY(qAbs(av_q2d(m->min_luminance) - 0.0001) < 0.00005);
        const AVPacketSideData* clli = sideData(ctx, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
        QVERIFY(clli);
        const auto* l = reinterpret_cast<const AVContentLightMetadata*>(clli->data);
        QCOMPARE(int(l->MaxCLL), 700);
        QCOMPARE(int(l->MaxFALL), 250);
    }

    void x265TagsHlg()
    {
        if (!hasEncoder("libx265"))
            QSKIP("libx265 is not in this FFmpeg build");
        VideoExportSettings video;
        video.codec = QStringLiteral("hevc");
        video.colorSpace = ColorSpaceTag::Rec2020;
        video.gamma = GammaTag::Hlg;
        VideoEncoder enc;
        QString error;
        int packets = 0;
        QVERIFY2(openAndEncode(enc, QStringLiteral("libx265"), video, QSize(128, 72), &packets,
                     &error),
            qPrintable(error));
        const AVCodecContext* ctx = enc.context();
        QCOMPARE(int(ctx->pix_fmt), int(AV_PIX_FMT_YUV420P10LE));
        QCOMPARE(int(ctx->color_trc), int(AVCOL_TRC_ARIB_STD_B67));
        QCOMPARE(int(ctx->color_primaries), int(AVCOL_PRI_BT2020));
        QVERIFY(!sideData(ctx, AV_PKT_DATA_MASTERING_DISPLAY_METADATA)); // HDR10 is PQ only
    }

    void svtAv1TagsPq()
    {
        if (!hasEncoder("libsvtav1"))
            QSKIP("libsvtav1 is not in this FFmpeg build");
        VideoExportSettings video;
        video.codec = QStringLiteral("av1");
        video.colorSpace = ColorSpaceTag::Rec2020;
        video.gamma = GammaTag::Pq;
        VideoEncoder enc;
        QString error;
        int packets = 0;
        QVERIFY2(openAndEncode(enc, QStringLiteral("libsvtav1"), video, QSize(256, 144), &packets,
                     &error),
            qPrintable(error));
        const AVCodecContext* ctx = enc.context();
        QCOMPARE(int(ctx->pix_fmt), int(AV_PIX_FMT_YUV420P10LE));
        QCOMPARE(int(ctx->color_trc), int(AVCOL_TRC_SMPTE2084));
        QCOMPARE(int(ctx->colorspace), int(AVCOL_SPC_BT2020_NCL));
        QVERIFY(sideData(ctx, AV_PKT_DATA_CONTENT_LIGHT_LEVEL));
    }

    void sdrStaysEightBitBt709()
    {
        if (!hasEncoder("libx265"))
            QSKIP("libx265 is not in this FFmpeg build");
        VideoExportSettings video;
        video.codec = QStringLiteral("hevc");
        VideoEncoder enc;
        QString error;
        int packets = 0;
        QVERIFY2(openAndEncode(enc, QStringLiteral("libx265"), video, QSize(128, 72), &packets,
                     &error),
            qPrintable(error));
        const AVCodecContext* ctx = enc.context();
        QCOMPARE(int(ctx->pix_fmt), int(AV_PIX_FMT_YUV420P));
        QCOMPARE(int(ctx->color_primaries), int(AVCOL_PRI_BT709));
        QCOMPARE(int(ctx->color_trc), int(AVCOL_TRC_BT709));
        QVERIFY(!sideData(ctx, AV_PKT_DATA_MASTERING_DISPLAY_METADATA));
    }

    void hardwareBackendRefusesHdr()
    {
        // 8-bit hardware encoders must never silently write an SDR file for an HDR request.
        VideoExportSettings video;
        video.codec = QStringLiteral("hevc");
        video.colorSpace = ColorSpaceTag::Rec2020;
        video.gamma = GammaTag::Pq;
        VideoEncoderConfig config;
        config.encoderName = QStringLiteral("libx265"); // any encoder that exists
        config.backend = EncoderBackend::Nvenc;
        config.settings = video;
        config.size = QSize(128, 72);
        if (!hasEncoder("libx265"))
            QSKIP("libx265 is not in this FFmpeg build");
        VideoEncoder enc;
        QString error;
        QVERIFY(!enc.open(config, &error));
        QVERIFY(error.contains(QLatin1String("Rec.2020")));
    }
};

QTEST_GUILESS_MAIN(TestHdrExport)
#include "test_hdrexport.moc"
