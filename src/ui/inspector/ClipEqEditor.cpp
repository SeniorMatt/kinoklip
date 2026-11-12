// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/inspector/ClipEqEditor.h"

#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"
#include "ui/audio/EqCurveView.h"
#include "ui/inspector/ParamEditHost.h"

#include <QVBoxLayout>

namespace kk {

namespace {
constexpr EqBandType kTypes[4]
    = {EqBandType::LowShelf, EqBandType::Bell, EqBandType::Bell, EqBandType::HighShelf};
constexpr int kBands = 4;
} // namespace

ClipEqEditor::ClipEqEditor(ParamEditHost& host, quint64 clipId, QWidget* parent)
    : QWidget(parent)
    , m_host(host)
    , m_clipId(clipId)
{
    setObjectName(QStringLiteral("ClipEqEditor"));
    if (const EffectDescriptor* d = EffectRegistry::instance().find(QLatin1String(groups::AudioEq)))
        m_descs = d->params;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 2);
    m_curve = new EqCurveView;
    m_curve->setBandCount(kBands);
    m_curve->setShowBandCurves(true);
    m_curve->setMinimumHeight(170);
    layout->addWidget(m_curve);
    for (int b = 0; b < kBands; ++b) {
        m_settings.bands[size_t(b)].type = kTypes[b];
        m_settings.bands[size_t(b)].enabled = true;
    }
    m_curve->setSettings(m_settings);

    connect(m_curve, &EqCurveView::bandPressed, this,
        [this](int) { m_host.beginParamEdit(tr("Change Equalizer")); });
    connect(m_curve, &EqCurveView::bandDragged, this,
        [this](int band, double hz, double db) { applyBand(band, &hz, &db, nullptr); });
    connect(m_curve, &EqCurveView::bandQDragged, this,
        [this](int band, double q) { applyBand(band, nullptr, nullptr, &q); });
    connect(m_curve, &EqCurveView::bandReleased, this, [this](int) { m_host.endParamEdit(); });
    connect(m_curve, &EqCurveView::bandQRequested, this, [this](int band, double q) {
        m_host.beginParamEdit(tr("Change Equalizer Q"));
        applyBand(band, nullptr, nullptr, &q);
        m_host.endParamEdit();
    });
    connect(m_curve, &EqCurveView::bandResetRequested, this, [this](int band) { resetBand(band); });
}

ParamRef ClipEqEditor::ref(const QString& key) const
{
    return ParamRef {m_clipId, ParamScope::Audio, 0, key};
}

void ClipEqEditor::refresh(const ClipData& clip)
{
    EqSettings s;
    s.enabled = paramaccess::staticValue(clip, ref(keys::EqEnabled), {}).toBool();
    for (const ParamDescriptor& d : std::as_const(m_descs)) {
        for (int b = 0; b < kBands; ++b) {
            EqBand& band = s.bands[size_t(b)];
            band.type = kTypes[b];
            band.enabled = true;
            if (d.key == keys::EqBandFreq[b])
                band.frequencyHz = paramaccess::scalar(clip, ref(d.key), d, 0);
            else if (d.key == keys::EqBandGain[b])
                band.gainDb = paramaccess::scalar(clip, ref(d.key), d, 0);
            else if (d.key == keys::EqBandQ[b])
                band.q = paramaccess::scalar(clip, ref(d.key), d, 0);
        }
    }
    for (int b = kBands; b < kEqBandCount; ++b)
        s.bands[size_t(b)].enabled = false;
    if (s == m_settings)
        return;
    m_settings = s;
    m_curve->setSettings(s);
}

void ClipEqEditor::applyBand(int band, const double* hz, const double* db, const double* q)
{
    if (band < 0 || band >= kBands)
        return;
    const ParamDescriptor* fd = nullptr;
    const ParamDescriptor* gd = nullptr;
    const ParamDescriptor* qd = nullptr;
    for (const ParamDescriptor& d : std::as_const(m_descs)) {
        if (d.key == keys::EqBandFreq[band])
            fd = &d;
        else if (d.key == keys::EqBandGain[band])
            gd = &d;
        else if (d.key == keys::EqBandQ[band])
            qd = &d;
    }
    const double fHz = hz ? *hz : 0.0;
    const double fDb = db ? *db : 0.0;
    const double fQ = q ? *q : 0.0;
    const int64_t frame = m_host.localFrame(m_clipId);
    m_host.modifyClip(m_clipId, [&](ClipData& c) {
        if (hz && fd)
            paramaccess::setScalar(c, ref(fd->key), *fd, frame, fHz);
        if (db && gd)
            paramaccess::setScalar(c, ref(gd->key), *gd, frame, fDb);
        if (q && qd)
            paramaccess::setScalar(c, ref(qd->key), *qd, frame, fQ);
        paramaccess::setStatic(c, ref(keys::EqEnabled), true);
    });
}

void ClipEqEditor::resetBand(int band)
{
    if (band < 0 || band >= kBands)
        return;
    m_host.editClip(tr("Reset Equalizer Band %1").arg(band + 1), m_clipId, [&](ClipData& c) {
        for (const ParamDescriptor& d : std::as_const(m_descs)) {
            if (d.key == keys::EqBandFreq[band] || d.key == keys::EqBandGain[band]
                || d.key == keys::EqBandQ[band])
                paramaccess::reset(c, ref(d.key), d);
        }
    });
}

} // namespace kk
