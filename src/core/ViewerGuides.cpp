// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerGuides — guide JSON, presets and overlay geometry (pure functions; no widgets).
#include "core/ViewerGuides.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace kk::guides {

namespace {
constexpr int kPresetFileVersion = 1; ///< "formatVersion" of guide-presets.json
/// Largest |position| accepted from a file (frame px): far beyond any frame, keeps nonsense
/// values (1e300) out of the maths.
constexpr double kMaxPosition = 1e6;

QJsonObject guideToJson(const ViewerGuide& g)
{
    return {{QLatin1String("o"),
                g.orientation == Qt::Horizontal ? QLatin1String("h") : QLatin1String("v")},
        {QLatin1String("pos"), g.position}};
}

std::optional<ViewerGuide> guideFromJson(const QJsonObject& o)
{
    const QString kind = o.value(QLatin1String("o")).toString();
    const QJsonValue pos = o.value(QLatin1String("pos"));
    if ((kind != QLatin1String("h") && kind != QLatin1String("v")) || !pos.isDouble())
        return std::nullopt;
    const double value = pos.toDouble();
    if (!std::isfinite(value) || std::abs(value) > kMaxPosition)
        return std::nullopt;
    return ViewerGuide {kind == QLatin1String("h") ? Qt::Horizontal : Qt::Vertical, value};
}

double extent(QSizeF frame, Qt::Orientation o)
{
    // A horizontal guide moves along y (the frame height), a vertical one along x.
    return o == Qt::Horizontal ? frame.height() : frame.width();
}
} // namespace

QJsonArray toJson(const std::vector<ViewerGuide>& list)
{
    QJsonArray array;
    for (const ViewerGuide& g : list)
        array.append(guideToJson(g));
    return array;
}

std::vector<ViewerGuide> fromJson(const QJsonArray& array)
{
    std::vector<ViewerGuide> list;
    list.reserve(size_t(array.size()));
    for (const QJsonValue& v : array) {
        if (auto g = guideFromJson(v.toObject()))
            list.push_back(*g);
    }
    return list;
}

int nearestGuide(const std::vector<ViewerGuide>& list, double x, double y, double tolerance)
{
    int best = -1;
    double bestDistance = tolerance;
    for (int i = 0; i < int(list.size()); ++i) {
        const ViewerGuide& g = list[size_t(i)];
        const double d = std::abs((g.orientation == Qt::Horizontal ? y : x) - g.position);
        if (d <= bestDistance) { // <=: a later guide wins a tie (it is drawn on top)
            best = i;
            bestDistance = d;
        }
    }
    return best;
}

std::vector<ViewerGuide> presetGuides(const GuidePreset& preset, QSizeF frame)
{
    std::vector<ViewerGuide> list;
    list.reserve(preset.fractions.size());
    for (const ViewerGuide& f : preset.fractions)
        list.push_back({f.orientation, f.position * extent(frame, f.orientation)});
    return list;
}

GuidePreset makeUserPreset(const QString& name, const std::vector<ViewerGuide>& list, QSizeF frame)
{
    GuidePreset preset;
    preset.id = QStringLiteral("user:") + name;
    preset.name = name;
    for (const ViewerGuide& g : list) {
        const double e = extent(frame, g.orientation);
        preset.fractions.push_back({g.orientation, e > 0 ? g.position / e : 0.0});
    }
    return preset;
}

std::vector<GuidePreset> builtInPresets()
{
    const ViewerGuide h50 {Qt::Horizontal, 0.5}, v50 {Qt::Vertical, 0.5};
    const ViewerGuide h1 {Qt::Horizontal, 1.0 / 3}, h2 {Qt::Horizontal, 2.0 / 3};
    const ViewerGuide v1 {Qt::Vertical, 1.0 / 3}, v2 {Qt::Vertical, 2.0 / 3};
    return {
        {QStringLiteral("builtin:centre"),
            QCoreApplication::translate("kk::guides", "Centre Cross"), true, {h50, v50}},
        {QStringLiteral("builtin:thirds"),
            QCoreApplication::translate("kk::guides", "Rule of Thirds"), true, {h1, h2, v1, v2}},
        {QStringLiteral("builtin:centre_thirds"),
            QCoreApplication::translate("kk::guides", "Centre and Thirds"), true,
            {h1, h50, h2, v1, v50, v2}},
    };
}

QString userPresetPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/guide-presets.json");
}

std::vector<GuidePreset> loadUserPresets(const QString& path)
{
    std::vector<GuidePreset> presets;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return presets;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QLatin1String("formatVersion")).toInt() > kPresetFileVersion)
        return presets; // a newer build's file: leave it alone, show none
    for (const QJsonValue& v : root.value(QLatin1String("presets")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QLatin1String("name")).toString().trimmed();
        if (name.isEmpty())
            continue;
        GuidePreset p;
        p.id = QStringLiteral("user:") + name;
        p.name = name;
        p.fractions = fromJson(o.value(QLatin1String("guides")).toArray());
        presets.push_back(std::move(p));
    }
    return presets;
}

bool saveUserPresets(const std::vector<GuidePreset>& presets, const QString& path, QString* error)
{
    QJsonArray list;
    for (const GuidePreset& p : presets) {
        if (p.builtIn)
            continue;
        list.append(QJsonObject {
            {QLatin1String("name"), p.name}, {QLatin1String("guides"), toJson(p.fractions)}});
    }
    const QJsonObject root {
        {QLatin1String("formatVersion"), kPresetFileVersion}, {QLatin1String("presets"), list}};
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson());
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

std::vector<GuidePreset> allPresets(const QString& path)
{
    std::vector<GuidePreset> presets = builtInPresets();
    for (GuidePreset& p : loadUserPresets(path))
        presets.push_back(std::move(p));
    return presets;
}

QRectF safeArea(QSizeF frame, double fraction)
{
    const QSizeF size = frame * fraction;
    return QRectF(
        QPointF((frame.width() - size.width()) / 2, (frame.height() - size.height()) / 2), size);
}

QRectF aspectMarker(QSizeF frame, double aspect)
{
    if (aspect <= 0 || frame.isEmpty())
        return QRectF(QPointF(0, 0), frame);
    QSizeF size(frame.width(), frame.width() / aspect);
    if (size.height() > frame.height())
        size = QSizeF(frame.height() * aspect, frame.height());
    return QRectF(
        QPointF((frame.width() - size.width()) / 2, (frame.height() - size.height()) / 2), size);
}

void gridLines(QSizeF frame, int divisions, std::vector<double>& xs, std::vector<double>& ys)
{
    const int n = std::clamp(divisions, 2, kMaxGridDivisions);
    xs.clear();
    ys.clear();
    for (int i = 1; i < n; ++i) {
        xs.push_back(frame.width() * i / n);
        ys.push_back(frame.height() * i / n);
    }
}

std::optional<double> snapOffset(
    std::span<const double> features, std::span<const double> targets, double threshold)
{
    std::optional<double> best;
    for (const double f : features) {
        for (const double t : targets) {
            const double d = t - f;
            if (std::abs(d) <= threshold && (!best || std::abs(d) < std::abs(*best)))
                best = d;
        }
    }
    return best;
}

std::vector<double> positions(const std::vector<ViewerGuide>& list, Qt::Orientation orientation)
{
    std::vector<double> out;
    for (const ViewerGuide& g : list) {
        if (g.orientation == orientation)
            out.push_back(g.position);
    }
    return out;
}

double rulerStep(double scale, double minLabelSpacing)
{
    if (scale <= 0)
        return 1.0;
    // Walk 1, 2, 5, 10, 20, 50 ... until the labels are far enough apart. Bounded: frames are
    // at most a few ten thousand px, so this ends within ~20 steps.
    double decade = 1.0;
    while (decade < 1e7) {
        for (const double m : {1.0, 2.0, 5.0}) {
            if (m * decade * scale >= minLabelSpacing)
                return m * decade;
        }
        decade *= 10;
    }
    return decade;
}

} // namespace kk::guides
