// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorBackgroundRemoval — the AI label, the model status and the buttons under the parameters
// of the Background Removal effect (spec 78): Download Model, Process Whole Clip, Cancel, a
// progress bar and a status line. Part of InspectorPanel. The mattes themselves are made by the
// MatteService on its own thread; nothing here waits for them.
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "matting/MatteService.h"
#include "ui/common/AppContext.h"
#include "ui/dialogs/AiModelDialog.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>

#include <algorithm>

namespace kk {

void InspectorPanel::buildBackgroundRemovalControls(
    ParamGroupBox* group, const EffectInstance& fx, quint64 clipId)
{
    if (fx.type != QLatin1String(fxid::BackgroundRemoval))
        return;
    const quint64 effectId = fx.id;
    auto* badge = new QLabel(tr("AI tool. The model runs on this computer; nothing is uploaded."));
    badge->setObjectName(QStringLiteral("AiLabel"));
    badge->setWordWrap(true);
    group->addWidgetRow(tr("AI"), badge);

    auto* buttons = new QWidget;
    auto* layout = new QHBoxLayout(buttons);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* download = new QPushButton(tr("Download Model..."));
    download->setObjectName(QStringLiteral("BgDownload"));
    download->setToolTip(tr("Shows what is downloaded, from where and under which licence, and "
                            "only downloads it when you press Download there."));
    auto* process = new QPushButton(tr("Process Whole Clip"));
    process->setObjectName(QStringLiteral("BgProcess"));
    process->setToolTip(tr("Cut out every frame of the clip in the background, so playback "
                           "and the final render find the mattes ready."));
    auto* cancel = new QPushButton(tr("Cancel"));
    cancel->setObjectName(QStringLiteral("BgCancel"));
    layout->addWidget(download);
    layout->addWidget(process);
    layout->addWidget(cancel);
    layout->addStretch(1);
    group->addWidgetRow(QString(), buttons);
    auto* progress = new QProgressBar;
    progress->setObjectName(QStringLiteral("BgProgress"));
    group->addWidgetRow(QString(), progress);
    auto* status = new QLabel;
    status->setObjectName(QStringLiteral("BgStatus"));
    status->setWordWrap(true);
    group->addWidgetRow(QString(), status);

    // The model this effect uses now: its Model choice, and for Automatic the clip's media type.
    auto modelOf = [this, clipId, effectId]() {
        const ClipData* c = clip(clipId);
        QString choice = fxchoice::BgModelAuto;
        bool still = false;
        if (c) {
            for (const EffectInstance& x : c->effects)
                if (x.id == effectId)
                    choice
                        = x.params.staticValue(fxkeys::BgModel, fxchoice::BgModelAuto).toString();
            if (m_ctx.project && m_ctx.project->mediaPool())
                if (const MediaItem* item = m_ctx.project->mediaPool()->item(c->mediaId))
                    still = matting::isStillImagePath(item->path);
        }
        return matting::resolveModel(choice, still);
    };
    connect(download, &QPushButton::clicked, this, [this, modelOf] {
        ModelDownloadDialog::run(this, matting::modelInfo(modelOf()));
        refreshValues();
    });
    connect(cancel, &QPushButton::clicked, this,
        [] { matting::MatteService::instance().cancelBackground(); });
    connect(process, &QPushButton::clicked, this, [this, clipId, effectId, modelOf] {
        const ClipData* c = clip(clipId);
        if (!c || !m_ctx.project || !m_ctx.project->mediaPool())
            return;
        const MediaItem* item = m_ctx.project->mediaPool()->item(c->mediaId);
        if (!item)
            return;
        const Rational fps = m_ctx.project->timeline()->data().fps;
        const EffectInstance* e = nullptr;
        for (const EffectInstance& x : c->effects)
            if (x.id == effectId)
                e = &x;
        const bool full = e
            && e->params.staticValue(fxkeys::BgQuality, fxchoice::BgPreview).toString()
                == fxchoice::BgFull;
        const double from = double(c->sourceTimeUsAt(c->start, fps)) / 1e6;
        const double to = double(c->sourceTimeUsAt(c->start + c->duration - 1, fps)) / 1e6;
        matting::MatteService::instance().queueRange(item->path, std::min(from, to),
            std::max(from, to), full ? matting::MatteQuality::Full : matting::MatteQuality::Preview,
            modelOf());
    });
    // The progress bar follows the service even when nothing else repaints the panel.
    connect(&matting::MatteService::instance(), &matting::MatteService::backgroundProgressChanged,
        group, [progress](int done, int total) {
            progress->setMaximum(std::max(1, total));
            progress->setValue(std::min(done, total));
        });

    group->addHeaderRefresh([download, process, cancel, progress, status, modelOf](
                                const ClipData&) {
        auto& service = matting::MatteService::instance();
        const auto s = service.status(modelOf());
        const auto p = service.backgroundProgress();
        download->setVisible(!s.modelInstalled && s.runtimeCompiled);
        process->setEnabled(s.ready);
        cancel->setVisible(p.total > 0);
        progress->setVisible(p.total > 0);
        if (p.total > 0) {
            progress->setMaximum(p.total);
            progress->setValue(std::min(p.done, p.total));
        }
        QString text;
        if (!s.ready)
            text = QObject::tr("Not available: %1 The picture is left as it is.").arg(s.reason);
        else if (p.total > 0)
            text = QObject::tr("Cutting out the clip in the background...");
        else
            text = QObject::tr("Ready. Frames not cut out yet show the unprocessed picture and "
                               "fill in while you work; the final render waits for every frame.");
        if (status->text() != text)
            status->setText(text);
    });
}

} // namespace kk
