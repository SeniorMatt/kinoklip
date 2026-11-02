// SPDX-License-Identifier: GPL-3.0-or-later
// MetadataPanel implementation: read-only clip details rebuilt per selection, persistent
// editors for the user metadata (so typing is never interrupted by a refresh).
#include "ui/mediapool/MetadataPanel.h"

#include "core/ClipColors.h"
#include "core/MediaPool.h"
#include "core/PowerBins.h"
#include "core/Project.h"
#include "ui/common/AppContext.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr Rational kFallbackFps {25, 1};
constexpr int kCommentsHeight = 64; ///< px, about three lines
constexpr int kMaxRating = 5;

QLabel* sectionLabel(const QString& title, bool first)
{
    auto* label = new QLabel(title);
    QFont bold = label->font();
    bold.setBold(true);
    label->setFont(bold);
    label->setContentsMargins(0, first ? 0 : 8, 0, 0);
    return label;
}

QLabel* dimLabel(const QString& text)
{
    auto* name = new QLabel(text);
    // Dimmed through a palette ROLE, not a copied colour: an own palette would keep the
    // previous scheme's text colour after a colour scheme switch.
    name->setForegroundRole(QPalette::PlaceholderText);
    return name;
}

/// The value all @p items share for @p get, or nothing when they differ.
template<typename T, typename Get>
std::optional<T> shared(const QVector<const MediaItem*>& items, Get get)
{
    if (items.isEmpty())
        return std::nullopt;
    const T first = get(*items.front());
    for (const MediaItem* m : items) {
        if (!(get(*m) == first))
            return std::nullopt;
    }
    return first;
}
} // namespace

MetadataPanel::MetadataPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Metadata"), parent)
    , m_ctx(ctx)
{
    m_stack = new QStackedWidget;
    m_stack->addWidget(createPlaceholderContent(tr("Select a clip in the media pool.")));
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget;
    auto* column = new QVBoxLayout(host);
    column->setContentsMargins(12, 10, 12, 10);
    column->setSpacing(4);
    m_heading = new QLabel;
    m_heading->setObjectName(QStringLiteral("MetadataHeading"));
    m_heading->setWordWrap(true);
    QFont big = m_heading->font();
    big.setBold(true);
    m_heading->setFont(big);
    column->addWidget(m_heading);
    auto* detailsHost = new QWidget;
    m_details = new QFormLayout(detailsHost);
    m_details->setContentsMargins(0, 0, 0, 0);
    m_details->setLabelAlignment(Qt::AlignRight);
    m_details->setVerticalSpacing(5);
    column->addWidget(detailsHost);
    auto* editorsHost = new QWidget;
    editorsHost->setObjectName(QStringLiteral("MetadataEditors"));
    column->addWidget(editorsHost);
    column->addStretch(1);
    scroll->setWidget(host);
    m_stack->addWidget(scroll);
    setContentWidget(m_stack);
    buildEditors();

    // Keep the shown items up to date (rename, undo, removal, batch edits).
    MediaPool* pool = ctx.project->mediaPool();
    connect(pool, &MediaPool::changed, this, &MetadataPanel::refresh);
    if (ctx.powerBins)
        connect(ctx.powerBins->pool(), &MediaPool::changed, this, &MetadataPanel::refresh);
}

void MetadataPanel::buildEditors()
{
    auto* host = findChild<QWidget*>(QStringLiteral("MetadataEditors"));
    auto* form = new QFormLayout(host);
    form->setContentsMargins(0, 0, 0, 0);
    form->setLabelAlignment(Qt::AlignRight);
    form->setVerticalSpacing(5);
    auto line = [this](const char* name, Field field) {
        auto* edit = new QLineEdit;
        edit->setObjectName(QLatin1String(name));
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, field] {
            if (!m_filling && edit->isModified()) {
                edit->setModified(false);
                applyField(field, edit->text());
            }
        });
        return edit;
    };
    form->addRow(sectionLabel(tr("Shot and Scene"), false));
    m_scene = line("MetadataScene", Field::Scene);
    form->addRow(dimLabel(tr("Scene")), m_scene);
    m_shot = line("MetadataShot", Field::Shot);
    form->addRow(dimLabel(tr("Shot")), m_shot);
    m_take = line("MetadataTake", Field::Take);
    form->addRow(dimLabel(tr("Take")), m_take);
    m_camera = line("MetadataCamera", Field::Camera);
    form->addRow(dimLabel(tr("Camera")), m_camera);
    m_angle = line("MetadataAngle", Field::Angle);
    form->addRow(dimLabel(tr("Angle")), m_angle);
    m_goodTake = new QCheckBox(tr("Good take"));
    m_goodTake->setObjectName(QStringLiteral("MetadataGoodTake"));
    connect(m_goodTake, &QCheckBox::clicked, this, [this](bool on) {
        if (!m_filling) {
            m_goodTake->setTristate(false);
            applyField(Field::GoodTake, on);
        }
    });
    form->addRow(QString(), m_goodTake);

    form->addRow(sectionLabel(tr("Notes"), false));
    m_description = line("MetadataDescription", Field::Description);
    form->addRow(dimLabel(tr("Description")), m_description);
    m_comments = new QPlainTextEdit;
    m_comments->setObjectName(QStringLiteral("MetadataComments"));
    m_comments->setFixedHeight(kCommentsHeight);
    m_comments->setTabChangesFocus(true);
    m_comments->installEventFilter(this);
    form->addRow(dimLabel(tr("Comments")), m_comments);
    m_keywords = line("MetadataKeywords", Field::Keywords);
    m_keywords->setToolTip(tr("Separate keywords with commas"));
    form->addRow(dimLabel(tr("Keywords")), m_keywords);
    m_rating = new QComboBox;
    m_rating->setObjectName(QStringLiteral("MetadataRating"));
    m_rating->addItem(tr("None"), 0);
    for (int r = 1; r <= kMaxRating; ++r)
        m_rating->addItem(QString(r, QChar(0x2605)), r); // BLACK STAR
    connect(m_rating, &QComboBox::activated, this, [this](int index) {
        if (!m_filling && index >= 0)
            applyField(Field::Rating, m_rating->itemData(index).toInt());
    });
    form->addRow(dimLabel(tr("Rating")), m_rating);
}

bool MetadataPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_comments && event->type() == QEvent::FocusOut && !m_filling
        && m_comments->document()->isModified()) {
        m_comments->document()->setModified(false);
        applyField(Field::Comments, m_comments->toPlainText());
    }
    return PanelFrame::eventFilter(watched, event);
}

void MetadataPanel::attach(MediaPoolPanel* pool)
{
    m_pool = pool;
    connect(pool, &MediaPoolPanel::selectedMediaChanged, this, [this](const QStringList& ids) {
        if (ids.size() > 1)
            showMediaItems(ids);
        else
            showMedia(m_pool->currentMediaId());
    });
    connect(pool, &MediaPoolPanel::currentMediaChanged, this, [this](const QString& id) {
        if (m_pool->selectedMediaIds().size() <= 1)
            showMedia(id);
    });
}

const MediaItem* MetadataPanel::lookup(const QString& id, MediaPool** pool) const
{
    MediaPool* project = m_ctx.project->mediaPool();
    if (const MediaItem* item = project->item(id)) {
        if (pool)
            *pool = project;
        return item;
    }
    MediaPool* power = m_ctx.powerBins ? m_ctx.powerBins->pool() : nullptr;
    const MediaItem* item = power ? power->item(id) : nullptr;
    if (pool)
        *pool = item ? power : nullptr;
    return item;
}

QWidget* MetadataPanel::editor(Field field) const
{
    switch (field) {
    case Field::Scene:
        return m_scene;
    case Field::Shot:
        return m_shot;
    case Field::Take:
        return m_take;
    case Field::Camera:
        return m_camera;
    case Field::Angle:
        return m_angle;
    case Field::GoodTake:
        return m_goodTake;
    case Field::Description:
        return m_description;
    case Field::Comments:
        return m_comments;
    case Field::Keywords:
        return m_keywords;
    case Field::Rating:
        return m_rating;
    }
    return nullptr;
}

void MetadataPanel::showMedia(const QString& mediaId)
{
    showMediaItems(mediaId.isEmpty() ? QStringList() : QStringList {mediaId});
}

void MetadataPanel::showMediaItems(const QStringList& ids)
{
    m_ids = ids;
    refresh();
}

void MetadataPanel::refresh()
{
    QVector<const MediaItem*> items;
    for (const QString& id : std::as_const(m_ids)) {
        if (const MediaItem* item = lookup(id))
            items.append(item);
    }
    if (items.isEmpty()) {
        m_stack->setCurrentIndex(0);
        return;
    }
    m_heading->setText(items.size() == 1
            ? items.front()->name
            : tr("%n clips selected: edits apply to all of them", nullptr, int(items.size())));
    fillDetails(items);
    fillEditors(items);
    m_stack->setCurrentIndex(1);
}

void MetadataPanel::fillDetails(const QVector<const MediaItem*>& items)
{
    while (m_details->rowCount() > 0)
        m_details->removeRow(0);
    auto addRow = [this](const QString& label, const QString& value) {
        if (value.isEmpty())
            return;
        auto* field = new QLabel(value);
        field->setTextInteractionFlags(Qt::TextSelectableByMouse);
        field->setWordWrap(true);
        m_details->addRow(dimLabel(label), field);
    };
    m_details->addRow(sectionLabel(tr("Clip Details"), true));
    if (items.size() > 1) {
        // Batch: only what the items share.
        const auto codec = shared<QString>(items, [](const MediaItem& m) {
            return m.info.hasVideo ? m.info.videoCodec : m.info.audioCodec;
        });
        const auto size = shared<QString>(items, [](const MediaItem& m) {
            return m.info.hasVideo ? QStringLiteral("%1 x %2").arg(m.info.width).arg(m.info.height)
                                   : QString();
        });
        const auto fps = shared<double>(items, [](const MediaItem& m) {
            return m.info.frameRate.isValid() ? m.info.frameRate.toDouble() : 0.0;
        });
        const QString multiple = tr("(multiple)");
        addRow(tr("Codec"), codec ? *codec : multiple);
        addRow(tr("Resolution"), size ? *size : multiple);
        addRow(tr("Frame rate"),
            fps ? (*fps > 0 ? tr("%1 fps").arg(QString::number(*fps, 'g', 5)) : QString())
                : multiple);
        return;
    }
    const MediaItem* item = items.front();
    const media::MediaInfo& info = item->info;
    addRow(tr("Name"), item->name);
    if (!item->isTimeline()) {
        // The file is checked once per selection (not per paint).
        const QFileInfo file(item->path);
        addRow(tr("File"), file.fileName());
        addRow(tr("Folder"), file.absolutePath());
        if (file.exists())
            addRow(tr("Size"), QLocale().formattedDataSize(file.size()));
        else
            addRow(tr("Status"), tr("Media offline (file not found)"));
        addRow(tr("Container"), info.container);
    }
    if (info.durationUs > 0) {
        const Rational fps = info.frameRate.isValid() ? info.frameRate : kFallbackFps;
        addRow(tr("Duration"), timecode::format(usToFrames(info.durationUs, fps), fps));
    }
    if (info.hasVideo) {
        addRow(tr("Video codec"), info.videoCodec);
        addRow(tr("Resolution"), tr("%1 x %2").arg(info.width).arg(info.height));
        if (info.frameRate.isValid() && !info.isStillImage)
            addRow(tr("Frame rate"),
                tr("%1 fps").arg(QString::number(info.frameRate.toDouble(), 'g', 5)));
        if (info.rotation != 0)
            addRow(tr("Rotation"), tr("%1 degrees").arg(info.rotation));
    }
    if (info.hasAudio) {
        addRow(tr("Audio codec"), info.audioCodec);
        addRow(tr("Audio channels"), QString::number(info.audioChannels));
        addRow(tr("Sample rate"), tr("%1 Hz").arg(info.audioSampleRate));
    }
    addRow(tr("Clip color"), clipColorName(item->clipColor));
    QStringList flags;
    for (int i = 0; i < int(flagPalette().size()); ++i) {
        if (item->flags & (1u << i))
            flags.append(flagName(i));
    }
    addRow(tr("Flags"), flags.join(QStringLiteral(", ")));
    if (m_pool && m_ctx.project->mediaPool()->item(item->id)) {
        const int uses = m_pool->model()->usageCounts().value(item->id, 0);
        addRow(tr("Usage"),
            uses > 0 ? tr("%n timeline clip(s)", nullptr, uses) : tr("Not used in any timeline"));
    }
    if (!info.error.isEmpty())
        addRow(tr("Probe"), info.error);
}

void MetadataPanel::fillEditors(const QVector<const MediaItem*>& items)
{
    m_filling = true;
    const QString multiple = items.size() > 1 ? tr("Multiple values") : QString();
    auto setLine = [&](QLineEdit* edit, std::optional<QString> value) {
        if (edit->hasFocus() && edit->isModified())
            return; // the user is typing: do not overwrite
        edit->setText(value.value_or(QString()));
        edit->setPlaceholderText(value ? QString() : multiple);
        edit->setModified(false);
    };
    using M = MediaItem;
    setLine(m_scene, shared<QString>(items, [](const M& m) { return m.metadata.scene; }));
    setLine(m_shot, shared<QString>(items, [](const M& m) { return m.metadata.shot; }));
    setLine(m_take, shared<QString>(items, [](const M& m) { return m.metadata.take; }));
    setLine(m_camera, shared<QString>(items, [](const M& m) { return m.metadata.camera; }));
    setLine(m_angle, shared<QString>(items, [](const M& m) { return m.metadata.angle; }));
    setLine(
        m_description, shared<QString>(items, [](const M& m) { return m.metadata.description; }));
    setLine(m_keywords, shared<QString>(items, [](const M& m) {
        return m.metadata.keywords.join(QStringLiteral(", "));
    }));
    if (!(m_comments->hasFocus() && m_comments->document()->isModified())) {
        const auto comments
            = shared<QString>(items, [](const M& m) { return m.metadata.comments; });
        m_comments->setPlainText(comments.value_or(QString()));
        m_comments->setPlaceholderText(comments ? QString() : multiple);
        m_comments->document()->setModified(false);
    }
    const auto good = shared<bool>(items, [](const M& m) { return m.metadata.goodTake; });
    m_goodTake->setTristate(!good);
    m_goodTake->setCheckState(good ? (*good ? Qt::Checked : Qt::Unchecked) : Qt::PartiallyChecked);
    const auto rating = shared<int>(items, [](const M& m) { return m.metadata.rating; });
    m_rating->setCurrentIndex(rating ? *rating : -1);
    m_rating->setPlaceholderText(multiple);
    m_filling = false;
}

void MetadataPanel::applyField(Field field, const QVariant& value)
{
    // One undo step for the project's items; power bin items are written directly.
    auto change = [field, &value](MediaMetadata md) {
        const QString text = value.toString().trimmed();
        switch (field) {
        case Field::Scene:
            md.scene = text;
            break;
        case Field::Shot:
            md.shot = text;
            break;
        case Field::Take:
            md.take = text;
            break;
        case Field::Camera:
            md.camera = text;
            break;
        case Field::Angle:
            md.angle = text;
            break;
        case Field::GoodTake:
            md.goodTake = value.toBool();
            break;
        case Field::Description:
            md.description = text;
            break;
        case Field::Comments:
            md.comments = value.toString();
            break;
        case Field::Keywords:
            md.keywords = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
            break;
        case Field::Rating:
            md.rating = value.toInt();
            break;
        }
        return md;
    };
    const QStringList ids = m_ids;
    bool opened = false;
    for (const QString& id : ids) {
        MediaPool* pool = nullptr;
        const MediaItem* item = lookup(id, &pool);
        if (!item)
            continue;
        const MediaMetadata md = change(item->metadata);
        if (pool == m_ctx.project->mediaPool() && !opened) {
            m_ctx.project->beginEdit(ids.size() > 1
                    ? tr("Edit Metadata of %n Clips", nullptr, int(ids.size()))
                    : tr("Edit Metadata"));
            opened = true;
        }
        pool->setMetadata(id, md);
    }
    if (opened)
        m_ctx.project->endEdit();
}

} // namespace kk
