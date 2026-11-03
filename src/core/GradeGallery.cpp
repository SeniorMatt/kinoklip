// SPDX-License-Identifier: GPL-3.0-or-later
// GradeGallery — stills list and its JSON form.
#include "core/GradeGallery.h"

#include "core/Serialization.h"

#include <QBuffer>
#include <QDateTime>
#include <QJsonObject>

#include <algorithm>

namespace kk {

namespace {
/// JPEG quality of stored stills (thumbnails; keeps project files small).
constexpr int kJpegQuality = 85;
} // namespace

GradeGallery::GradeGallery(QObject* parent)
    : QObject(parent)
{ }

const GalleryStill* GradeGallery::still(quint64 id) const
{
    for (const GalleryStill& s : m_stills) {
        if (s.id == id)
            return &s;
    }
    return nullptr;
}

quint64 GradeGallery::addStill(const QImage& image, const ClipGrade& grade)
{
    GalleryStill s;
    for (const GalleryStill& other : std::as_const(m_stills))
        s.id = std::max(s.id, other.id);
    ++s.id;
    s.name = QStringLiteral("1.%1").arg(s.id);
    s.createdMs = QDateTime::currentMSecsSinceEpoch();
    s.grade = grade;
    s.image = image.width() > kGalleryImageWidth
        ? image.scaledToWidth(kGalleryImageWidth, Qt::SmoothTransformation)
        : image;
    m_stills.append(s);
    emit changed();
    return s.id;
}

bool GradeGallery::removeStill(quint64 id)
{
    const qsizetype n = m_stills.removeIf([id](const GalleryStill& s) { return s.id == id; });
    if (n > 0)
        emit changed();
    return n > 0;
}

bool GradeGallery::renameStill(quint64 id, const QString& name)
{
    for (GalleryStill& s : m_stills) {
        if (s.id == id && s.name != name) {
            s.name = name;
            emit changed();
            return true;
        }
    }
    return false;
}

void GradeGallery::clear()
{
    if (m_stills.isEmpty())
        return;
    m_stills.clear();
    emit changed();
}

QJsonArray GradeGallery::toJson() const
{
    QJsonArray list;
    for (const GalleryStill& s : m_stills) {
        QJsonObject o {{QLatin1String("id"), qint64(s.id)}, {QLatin1String("name"), s.name},
            {QLatin1String("created"), s.createdMs},
            {QLatin1String("grade"), serialization::gradeToJson(s.grade)}};
        if (!s.image.isNull()) {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            s.image.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPG", kJpegQuality);
            o[QLatin1String("image")] = QString::fromLatin1(bytes.toBase64());
        }
        list.append(o);
    }
    return list;
}

void GradeGallery::fromJson(const QJsonArray& json)
{
    m_stills.clear();
    for (const QJsonValue& v : json) {
        const QJsonObject o = v.toObject();
        GalleryStill s;
        s.id = quint64(o.value(QLatin1String("id")).toInteger());
        if (s.id == 0 || still(s.id))
            continue;
        s.name = o.value(QLatin1String("name")).toString();
        s.createdMs = o.value(QLatin1String("created")).toInteger();
        s.grade = serialization::gradeFromJson(o.value(QLatin1String("grade")).toObject());
        const QByteArray image
            = QByteArray::fromBase64(o.value(QLatin1String("image")).toString().toLatin1());
        if (!image.isEmpty())
            s.image.loadFromData(image, "JPG");
        m_stills.append(s);
    }
    emit changed();
}

} // namespace kk
