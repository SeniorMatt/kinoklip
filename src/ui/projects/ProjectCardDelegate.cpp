// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectCardDelegate implementation.
#include "ui/projects/ProjectCardDelegate.h"

#include "ui/common/Icons.h"
#include "ui/common/SchemeColors.h"
#include "ui/projects/ProjectLibraryModel.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>

#include <algorithm>

namespace kk {

namespace {
constexpr int kPadding = 6; ///< inside the card, around the poster and the text
constexpr int kRadius = 4;
constexpr int kLineGap = 2;

/// A small rounded label on the poster ("Open", "Media offline").
void drawBadge(QPainter* p, const QPoint& anchor, bool alignRight, const QString& text,
    const QColor& fill, const QColor& textColor, const QFont& font)
{
    QFont f = font;
    f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.85));
    f.setBold(true);
    const QFontMetrics fm(f);
    const int w = fm.horizontalAdvance(text) + 2 * kPadding;
    const int h = fm.height() + 2;
    QRect r(alignRight ? anchor.x() - w : anchor.x(), anchor.y(), w, h);
    p->setPen(Qt::NoPen);
    p->setBrush(fill);
    p->drawRoundedRect(r, kRadius, kRadius);
    p->setFont(f);
    p->setPen(textColor);
    p->drawText(r, Qt::AlignCenter, text);
}
} // namespace

ProjectCardDelegate::ProjectCardDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{ }

void ProjectCardDelegate::setCardWidth(int width)
{
    m_width = std::clamp(width, kMinWidth, kMaxWidth);
}

QSize ProjectCardDelegate::cardSize() const
{
    const QFontMetrics fm(QApplication::font());
    const int poster = (m_width - 2 * kPadding) * 9 / 16;
    return {m_width, kPadding + poster + kPadding + 3 * fm.height() + 2 * kLineGap + kPadding};
}

QSize ProjectCardDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const
{
    return cardSize();
}

QRect ProjectCardDelegate::posterRect(const QRect& card) const
{
    const int w = card.width() - 2 * kPadding;
    return {card.left() + kPadding, card.top() + kPadding, w, w * 9 / 16};
}

QRect ProjectCardDelegate::nameRect(const QRect& card, const QFontMetrics& fm) const
{
    const QRect poster = posterRect(card);
    return {poster.left(), poster.bottom() + 1 + kPadding, poster.width(), fm.height()};
}

void ProjectCardDelegate::paint(
    QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const SchemeColors& sc = schemeColors();
    const QRect card = option.rect.adjusted(1, 1, -1, -1);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const bool isFolder = index.data(ProjectLibraryModel::IsFolderRole).toBool();
    const bool exists = index.data(ProjectLibraryModel::ExistsRole).toBool();
    const QString error = index.data(ProjectLibraryModel::ErrorRole).toString();

    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    // Card body: the view background, lifted on hover, the selection colour when selected.
    QColor body = mixColors(sc.view, sc.window, 0.5);
    if (hovered)
        body = mixColors(body, sc.hover, 0.18);
    if (selected)
        body = mixColors(sc.view, sc.highlight, 0.35);
    p->setPen(selected ? QPen(sc.highlight, 1.5) : QPen(sc.line, 1));
    p->setBrush(body);
    p->drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);

    // Poster area.
    const QRect poster = posterRect(card);
    p->setPen(Qt::NoPen);
    p->setBrush(isFolder ? mixColors(body, sc.window, 0.5) : sc.viewerSurround);
    p->drawRoundedRect(poster, 2, 2);
    const int glyph = std::max(24, poster.height() / 2);
    const QRect glyphRect(
        poster.center().x() - glyph / 2, poster.center().y() - glyph / 2, glyph, glyph);
    if (isFolder) {
        icon(QStringLiteral("folder")).paint(p, glyphRect);
    } else if (!error.isEmpty()) {
        // Error card: tinted poster, warning glyph; the error text is the details line.
        p->setBrush(withAlpha(sc.negative, 60));
        p->drawRoundedRect(poster, 2, 2);
        const int small = std::max(20, poster.height() / 3);
        const QString warning = QStringLiteral("warning");
        (QIcon::hasThemeIcon(themeIconName(warning))
                ? icon(warning)
                : QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning))
            .paint(p,
                QRect(poster.center().x() - small / 2, poster.center().y() - small / 2, small,
                    small));
    } else {
        const QImage image = index.data(ProjectLibraryModel::PosterRole).value<QImage>();
        if (!image.isNull()) {
            // Letterbox: the whole frame, centred, aspect kept.
            QSize s = image.size().scaled(poster.size(), Qt::KeepAspectRatio);
            const QRect target(poster.center().x() - s.width() / 2 + 1,
                poster.center().y() - s.height() / 2 + 1, s.width(), s.height());
            p->setRenderHint(QPainter::SmoothPixmapTransform);
            p->drawImage(target, image);
        } else {
            p->setOpacity(0.35);
            icon(QStringLiteral("film")).paint(p, glyphRect);
            p->setOpacity(1.0);
        }
    }

    // Badges.
    const QFont font = option.font;
    const QPoint topLeft = poster.topLeft() + QPoint(kPadding / 2, kPadding / 2);
    const QPoint topRight = poster.topRight() + QPoint(-kPadding / 2, kPadding / 2);
    if (index.data(ProjectLibraryModel::IsOpenRole).toBool())
        drawBadge(p, topLeft, false, tr("Open"), sc.focus, sc.textOn(sc.focus), font);
    if (const int missing = index.data(ProjectLibraryModel::MissingMediaRole).toInt();
        missing > 0) {
        drawBadge(p, topRight, true, tr("Media offline (%1)").arg(missing), sc.negative,
            sc.textOn(sc.negative), font);
    } else if (!exists) {
        drawBadge(p, topRight, true, tr("Missing"), sc.negative, sc.textOn(sc.negative), font);
    }

    // Kind of project at the poster's bottom left ("Linked", "Single file").
    if (const QString kind = index.data(ProjectLibraryModel::KindBadgeRole).toString();
        !kind.isEmpty()) {
        const int h = QFontMetrics(font).height() + 2;
        const QColor plate = withAlpha(sc.overlay, 200);
        drawBadge(p, QPoint(poster.left() + kPadding / 2, poster.bottom() - kPadding / 2 - h),
            false, kind, plate, sc.overlayText, font);
    }

    // Text: name (bold), modified, details.
    QFont nameFont = font;
    nameFont.setBold(true);
    const QFontMetrics fm(font);
    QRect line = nameRect(card, fm);
    const QColor text = selected ? sc.textOn(body) : sc.text;
    p->setFont(nameFont);
    p->setPen(text);
    p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetrics(nameFont).elidedText(
            index.data(Qt::EditRole).toString(), Qt::ElideMiddle, line.width()));
    p->setFont(font);
    p->setPen(selected ? text : sc.textDim);
    line.translate(0, fm.height() + kLineGap);
    p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
        fm.elidedText(index.data(ProjectLibraryModel::ModifiedTextRole).toString(), Qt::ElideRight,
            line.width()));
    line.translate(0, fm.height() + kLineGap);
    const QString details
        = isFolder ? tr("Folder") : index.data(ProjectLibraryModel::DetailsRole).toString();
    if (!error.isEmpty())
        p->setPen(sc.negative);
    p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
        fm.elidedText(details, Qt::ElideRight, line.width()));

    if (option.state & QStyle::State_HasFocus && selected) {
        p->setPen(QPen(sc.focus, 1, Qt::DotLine));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(card).adjusted(2.5, 2.5, -2.5, -2.5), kRadius, kRadius);
    }
    p->restore();
}

bool ProjectCardDelegate::editorAlreadyFilled(QWidget* editor)
{
    if (editor->property("kkEditorFilled").toBool())
        return true;
    editor->setProperty("kkEditorFilled", true);
    return false;
}

void ProjectCardDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
    if (!editorAlreadyFilled(editor))
        QStyledItemDelegate::setEditorData(editor, index);
}

void ProjectCardDelegate::updateEditorGeometry(
    QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const
{
    const QFontMetrics fm(option.font);
    QRect r = nameRect(option.rect.adjusted(1, 1, -1, -1), fm);
    r.adjust(-2, -3, 2, 3);
    editor->setGeometry(r);
}

} // namespace kk
