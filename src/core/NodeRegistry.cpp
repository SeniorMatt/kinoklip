// SPDX-License-Identifier: GPL-3.0-or-later
// NodeRegistry implementation: the built-in Compositing page nodes and the generated effect
// nodes. Behaviour: docs/behavior-spec.md section 42a.
#include "core/NodeRegistry.h"

#include "core/EffectBuilders.h"

#include <QCoreApplication>
#include <QHash>

namespace kk {

using namespace builders;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::NodeRegistry", text);
}

const QString kPx = QStringLiteral("px");
const QString kDegrees = QStringLiteral("°");

NodePort port(const QString& id, const QString& label, PortKind kind, bool required,
    bool analysisOnly = false)
{
    return NodePort {id, label, kind, required, analysisOnly};
}

NodePort maskPort()
{
    return port(nodeports::Mask, tr("Effect Mask"), PortKind::Mask, false);
}

NodePort inputPort()
{
    return port(nodeports::Input, tr("Input"), PortKind::Image, true);
}

NodeDescriptor node(const QString& id, const QString& name, const QString& shortName,
    const QString& nameBase, NodeCategory category, const QString& description)
{
    NodeDescriptor d;
    d.id = id;
    d.name = name;
    d.shortName = shortName;
    d.nameBase = nameBase;
    d.category = category;
    d.description = description;
    return d;
}

/// Centre of the shared geometry (0.5, 0.5 = the middle of the frame).
QVector<ParamDescriptor> centre()
{
    return {number(nodekeys::CenterX, tr("Center X"), -10, 10, 0.5, 0.001),
        number(nodekeys::CenterY, tr("Center Y"), -10, 10, 0.5, 0.001)};
}

ParamDescriptor angle()
{
    return number(nodekeys::Angle, tr("Angle"), -3600, 3600, 0, 0.1, kDegrees);
}

/// The parameters every mask node shares (after its own shape parameters).
QVector<ParamDescriptor> maskCommon()
{
    return {number(nodekeys::SoftEdge, tr("Soft Edge"), 0, 1, 0, 0.001),
        boolean(nodekeys::Invert, tr("Invert"), false),
        number(nodekeys::Level, tr("Level"), 0, 1, 1, 0.01)};
}

QString shortNameOf(const QString& name)
{
    // "Gaussian Blur" -> "GBlu": initials of all words but the last + its first letters.
    const QStringList words = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return {};
    QString s;
    for (qsizetype i = 0; i + 1 < words.size(); ++i)
        s += words[i].left(1);
    s += words.last().left(std::max<qsizetype>(2, 4 - s.size()));
    return s.left(4);
}

QString nameBaseOf(const QString& name)
{
    QString s;
    for (const QChar c : name) {
        if (c.isLetterOrNumber())
            s += c;
    }
    return s.isEmpty() ? QStringLiteral("Node") : s;
}

/// Translated names of the blend modes in blendmodes::ids() order (the Merge choice list).
QStringList blendModeNames()
{
    QStringList names;
    for (const QString& id : blendmodes::ids())
        names.append(blendmodes::displayName(id));
    return names;
}

} // namespace

const QStringList& blendmodes::ids()
{
    static const QStringList modes {Normal, Screen, Multiply, Add, Overlay, Darken, Lighten,
        Difference, Subtract, HardLight, SoftLight, ColorDodge, ColorBurn, Exclusion, LinearBurn,
        LinearLight, VividLight, PinLight, HardMix};
    return modes;
}

int blendmodes::index(const QString& mode)
{
    return std::max<int>(0, int(ids().indexOf(mode)));
}

const QStringList& blendmodes::compositeOrder()
{
    static const QStringList order {Normal, Add, Subtract, Difference, Multiply, Screen, Overlay,
        HardLight, SoftLight, Darken, Lighten, ColorDodge, ColorBurn, Exclusion, LinearBurn,
        LinearLight, VividLight, PinLight, HardMix};
    return order;
}

QString blendmodes::displayName(const QString& id)
{
    static const QHash<QString, const char*> names {{Normal, QT_TR_NOOP("Normal")},
        {Screen, QT_TR_NOOP("Screen")}, {Multiply, QT_TR_NOOP("Multiply")},
        {Add, QT_TR_NOOP("Add")}, {Overlay, QT_TR_NOOP("Overlay")}, {Darken, QT_TR_NOOP("Darken")},
        {Lighten, QT_TR_NOOP("Lighten")}, {Difference, QT_TR_NOOP("Difference")},
        {Subtract, QT_TR_NOOP("Subtract")}, {HardLight, QT_TR_NOOP("Hard Light")},
        {SoftLight, QT_TR_NOOP("Soft Light")}, {ColorDodge, QT_TR_NOOP("Color Dodge")},
        {ColorBurn, QT_TR_NOOP("Color Burn")}, {Exclusion, QT_TR_NOOP("Exclusion")},
        {LinearBurn, QT_TR_NOOP("Linear Burn")}, {LinearLight, QT_TR_NOOP("Linear Light")},
        {VividLight, QT_TR_NOOP("Vivid Light")}, {PinLight, QT_TR_NOOP("Pin Light")},
        {HardMix, QT_TR_NOOP("Hard Mix")}};
    const auto it = names.constFind(id);
    return it == names.constEnd() ? id : QCoreApplication::translate("kk::blendmodes", it.value());
}

int channelsources::index(const QString& source)
{
    static const QStringList sources {Red, Green, Blue, Alpha, Luma, Black, White};
    return int(sources.indexOf(source));
}

const NodePort* NodeDescriptor::input(const QString& id) const
{
    for (const NodePort& p : inputs) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

const ParamDescriptor* NodeDescriptor::param(const QString& key) const
{
    for (const ParamDescriptor& p : params) {
        if (p.key == key)
            return &p;
    }
    return nullptr;
}

const NodeRegistry& NodeRegistry::instance()
{
    static const NodeRegistry registry;
    return registry;
}

void NodeRegistry::add(NodeDescriptor d)
{
    m_index.insert(d.id, m_descriptors.size());
    m_descriptors.append(std::move(d));
}

const NodeDescriptor* NodeRegistry::find(const QString& id) const
{
    const auto it = m_index.constFind(id);
    return it == m_index.constEnd() ? nullptr : &m_descriptors[*it];
}

QVector<const NodeDescriptor*> NodeRegistry::byCategory(NodeCategory category) const
{
    QVector<const NodeDescriptor*> out;
    for (const NodeDescriptor& d : m_descriptors) {
        if (d.category == category)
            out.append(&d);
    }
    return out;
}

ParamSet NodeRegistry::defaultParams(const QString& id) const
{
    ParamSet set;
    const NodeDescriptor* d = find(id);
    if (!d)
        return set;
    for (const ParamDescriptor& p : d->params) {
        if (p.isScalar())
            set.setBase(p.key, p.def);
        else
            set.setStatic(p.key, p.defStatic);
    }
    return set;
}

QString NodeRegistry::categoryName(NodeCategory category)
{
    switch (category) {
    case NodeCategory::InputOutput:
        return tr("I/O");
    case NodeCategory::Composite:
        return tr("Composite");
    case NodeCategory::Transform:
        return tr("Transform");
    case NodeCategory::Generator:
        return tr("Generator");
    case NodeCategory::Mask:
        return tr("Mask");
    case NodeCategory::Color:
        return tr("Color");
    case NodeCategory::Effect:
        return tr("Effects");
    }
    return {};
}

NodeRegistry::NodeRegistry()
{
    namespace k = nodekeys;
    using C = NodeCategory;

    // ---- I/O ----
    {
        NodeDescriptor d = node(nodetypes::MediaIn, tr("MediaIn"), QStringLiteral("MI"),
            QStringLiteral("MediaIn"), C::InputOutput,
            tr("The clip's picture (or another media pool item) at the composition's frame"));
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::MediaOut, tr("MediaOut"), QStringLiteral("MO"),
            QStringLiteral("MediaOut"), C::InputOutput,
            tr("The result of the composition: what the clip shows on the timeline"));
        d.inputs = {inputPort()};
        d.mainInput = nodeports::Input;
        d.outputs = 0;
        add(d);
    }

    // ---- Composite ----
    {
        NodeDescriptor d = node(nodetypes::Merge, tr("Merge"), QStringLiteral("Mrg"),
            QStringLiteral("Merge"), C::Composite, tr("Puts the foreground over the background"));
        d.inputs = {port(nodeports::Background, tr("Background"), PortKind::Image, true),
            port(nodeports::Foreground, tr("Foreground"), PortKind::Image, true), maskPort()};
        d.mainInput = nodeports::Background;
        d.params = {choice(k::BlendMode, tr("Apply Mode"), blendmodes::ids(), blendModeNames(),
                        blendmodes::Normal),
            number(k::Opacity, tr("Blend"), 0, 1, 1, 0.01)};
        d.params += centre();
        d.params += {number(k::Size, tr("Size"), 0, 100, 1, 0.01), angle()};
        d.controls = ViewerControls::Transform;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::Dissolve, tr("Dissolve"), QStringLiteral("Dx"),
            QStringLiteral("Dissolve"), C::Composite,
            tr("Mixes between the background and the foreground"));
        d.inputs = {port(nodeports::Background, tr("Background"), PortKind::Image, true),
            port(nodeports::Foreground, tr("Foreground"), PortKind::Image, true), maskPort()};
        d.mainInput = nodeports::Background;
        d.params = {number(k::Mix, tr("Background / Foreground"), 0, 1, 0, 0.01)};
        add(d);
    }

    // ---- Transform ----
    {
        NodeDescriptor d = node(nodetypes::Transform, tr("Transform"), QStringLiteral("Xf"),
            QStringLiteral("Transform"), C::Transform,
            tr("Moves, scales, rotates and flips the picture"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = centre();
        d.params += {number(k::PivotX, tr("Pivot X"), -10, 10, 0.5, 0.001),
            number(k::PivotY, tr("Pivot Y"), -10, 10, 0.5, 0.001),
            number(k::Size, tr("Size"), 0, 100, 1, 0.01),
            number(k::Aspect, tr("Aspect"), 0.01, 100, 1, 0.01), angle(),
            boolean(k::FlipH, tr("Flip Horizontal"), false),
            boolean(k::FlipV, tr("Flip Vertical"), false)};
        d.controls = ViewerControls::Transform;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::Tracker, tr("Tracker"), QStringLiteral("Trk"),
            QStringLiteral("Tracker"), C::Transform,
            tr("Follows a point in the picture; the input moves with it (Match Move) or is "
               "held steady (Stabilize)"));
        d.inputs = {inputPort(),
            port(nodeports::Source, tr("Tracking Source"), PortKind::Image, false, true)};
        d.mainInput = nodeports::Input;
        d.params = centre();
        d.params += {number(nodekeys::PatternSize, tr("Pattern Size"), 8, 400, 48, 1, kPx),
            number(nodekeys::SearchSize, tr("Search Size"), 16, 1600, 160, 1, kPx),
            choice(nodekeys::TrackMode, tr("Output"),
                {trackmodes::MatchMove, trackmodes::Stabilize}, {tr("Match Move"), tr("Stabilize")},
                trackmodes::MatchMove),
            integer(nodekeys::RefFrame, tr("Reference Frame"), 0, 1000000, 0)};
        for (const QString& key :
            {nodekeys::PatternSize, nodekeys::SearchSize, nodekeys::RefFrame}) {
            for (ParamDescriptor& p : d.params) {
                if (p.key == key)
                    p.animatable = false;
            }
        }
        d.controls = ViewerControls::Tracker;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::PlanarTracker, tr("Planar Tracker"),
            QStringLiteral("PTrk"), QStringLiteral("PlanarTracker"), C::Transform,
            tr("Follows a flat surface (four corners); the input is pinned onto it (Corner Pin) "
               "or moves with it (Unsteady) or is held steady (Steady)"));
        d.inputs = {inputPort(),
            port(nodeports::Source, tr("Tracking Source"), PortKind::Image, false, true)};
        d.mainInput = nodeports::Input;
        // Default region: the middle half of the frame. Corner order: top-left, top-right,
        // bottom-right, bottom-left (names of the Inspector rows).
        const char* const names[4] = {QT_TRANSLATE_NOOP("kk::NodeRegistry", "Top Left"),
            QT_TRANSLATE_NOOP("kk::NodeRegistry", "Top Right"),
            QT_TRANSLATE_NOOP("kk::NodeRegistry", "Bottom Right"),
            QT_TRANSLATE_NOOP("kk::NodeRegistry", "Bottom Left")};
        const double xs[4] = {0.25, 0.75, 0.75, 0.25}, ys[4] = {0.75, 0.75, 0.25, 0.25};
        for (int i = 0; i < nodekeys::kPlanarCorners; ++i) {
            d.params += number(
                nodekeys::planarCorner(i, false), tr(names[i]) + tr(" X"), -10, 10, xs[i], 0.001);
            d.params += number(
                nodekeys::planarCorner(i, true), tr(names[i]) + tr(" Y"), -10, 10, ys[i], 0.001);
        }
        d.params += {number(nodekeys::PatternSize, tr("Pattern Size"), 8, 400, 21, 1, kPx),
            number(nodekeys::SearchSize, tr("Search Size"), 4, 400, 48, 1, kPx),
            choice(nodekeys::TrackMode, tr("Output"),
                {trackmodes::CornerPin, trackmodes::MatchMove, trackmodes::Stabilize},
                {tr("Corner Pin"), tr("Unsteady (Match Move)"), tr("Steady (Stabilize)")},
                trackmodes::CornerPin),
            integer(nodekeys::RefFrame, tr("Reference Frame"), 0, 1000000, 0)};
        for (const QString& key :
            {nodekeys::PatternSize, nodekeys::SearchSize, nodekeys::RefFrame}) {
            for (ParamDescriptor& p : d.params) {
                if (p.key == key)
                    p.animatable = false;
            }
        }
        d.controls = ViewerControls::Planar;
        add(d);
    }
    {
        NodeDescriptor d
            = node(nodetypes::Crop, tr("Crop"), QStringLiteral("Crp"), QStringLiteral("Crop"),
                C::Transform, tr("Cuts the edges off the picture (they become transparent)"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {number(k::Left, tr("Left"), 0, 8192, 0, 1, kPx),
            number(k::Right, tr("Right"), 0, 8192, 0, 1, kPx),
            number(k::Top, tr("Top"), 0, 8192, 0, 1, kPx),
            number(k::Bottom, tr("Bottom"), 0, 8192, 0, 1, kPx)};
        add(d);
    }
    {
        NodeDescriptor d
            = node(nodetypes::Resize, tr("Resize"), QStringLiteral("Rsz"), QStringLiteral("Resize"),
                C::Transform, tr("Scales the picture to a size in pixels, centred in the frame"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {number(k::Width, tr("Width"), 1, 16384, 1920, 1, kPx),
            number(k::Height, tr("Height"), 1, 16384, 1080, 1, kPx),
            boolean(k::KeepAspect, tr("Keep Aspect"), false)};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::Letterbox, tr("Letterbox"), QStringLiteral("LBx"),
            QStringLiteral("Letterbox"), C::Transform,
            tr("Covers the picture with bars to a wider (or narrower) aspect ratio"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {number(k::TargetAspect, tr("Aspect Ratio"), 0.1, 10, 2.39, 0.01),
            color(k::Color, tr("Bar Color"), QColor(Qt::black))};
        add(d);
    }

    // ---- Generators ----
    {
        NodeDescriptor d = node(nodetypes::Background, tr("Background"), QStringLiteral("BG"),
            QStringLiteral("Background"), C::Generator,
            tr("A solid colour or a two or four colour gradient"));
        d.inputs = {maskPort()};
        d.params
            = {choice(k::Mode, tr("Type"),
                   {backgroundmodes::Solid, backgroundmodes::Gradient, backgroundmodes::FourCorner},
                   {tr("Solid Color"), tr("Gradient"), tr("Four Corner")}, backgroundmodes::Solid),
                color(k::Color, tr("Color"), QColor(0, 0, 0)),
                color(k::Color2, tr("Color 2"), QColor(255, 255, 255)),
                color(k::Color3, tr("Color 3"), QColor(255, 0, 0)),
                color(k::Color4, tr("Color 4"), QColor(0, 0, 255)),
                number(k::GradientAngle, tr("Gradient Angle"), -360, 360, 0, 0.5, kDegrees)};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::Text, tr("Text"), QStringLiteral("Txt"),
            QStringLiteral("Text"), C::Generator, tr("Text with outline and shadow"));
        d.inputs = {maskPort()};
        d.params = {text(k::Text, tr("Text"), tr("Custom Title")),
            text(k::Font, tr("Font"), QStringLiteral("Sans Serif")),
            number(k::FontSize, tr("Size"), 1, 1000, 96, 1, kPx),
            boolean(k::Bold, tr("Bold"), false), boolean(k::Italic, tr("Italic"), false),
            color(k::Color, tr("Color"), QColor(Qt::white)),
            color(k::OutlineColor, tr("Outline Color"), QColor(Qt::black)),
            number(k::OutlineWidth, tr("Outline Width"), 0, 100, 0, 0.5, kPx),
            boolean(k::Shadow, tr("Drop Shadow"), false),
            number(k::WriteOnStart, tr("Write On Start"), 0, 1, 0, 0.01),
            number(k::WriteOnEnd, tr("Write On End"), 0, 1, 1, 0.01)};
        d.params += centre();
        d.controls = ViewerControls::Transform;
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::FastNoise, tr("Fast Noise"), QStringLiteral("FN"),
            QStringLiteral("FastNoise"), C::Generator,
            tr("Smooth, optionally evolving fractal noise between two colours"));
        d.inputs = {maskPort()};
        d.params = {integer(k::Detail, tr("Detail"), 1, 8, 2),
            number(k::Contrast, tr("Contrast"), 0, 10, 1, 0.01),
            number(k::Brightness, tr("Brightness"), -1, 1, 0, 0.01),
            number(k::Scale, tr("Scale"), 0.1, 1000, 10, 0.1),
            number(k::Seethe, tr("Seethe Rate"), 0, 10, 0, 0.01),
            integer(k::Seed, tr("Seed"), 0, 100000, 0),
            color(k::Color, tr("Color 1"), QColor(Qt::black)),
            color(k::Color2, tr("Color 2"), QColor(Qt::white))};
        d.params += centre();
        d.params += {angle()};
        add(d);
    }

    // ---- Masks ----
    const auto shapeMask = [&](const QString& id, const QString& name, const QString& shortName,
                               const QString& base, const QString& description) {
        NodeDescriptor d = node(id, name, shortName, base, C::Mask, description);
        d.inputs = {port(nodeports::Mask, tr("Mask"), PortKind::Mask, false)};
        d.mainInput = nodeports::Mask;
        d.outputKind = PortKind::Mask;
        d.params = centre();
        d.params += {number(k::Width, tr("Width"), 0, 10, 0.5, 0.001),
            number(k::Height, tr("Height"), 0, 10, 0.5, 0.001), angle()};
        d.params += maskCommon();
        d.controls = ViewerControls::Rect;
        return d;
    };
    add(shapeMask(nodetypes::RectangleMask, tr("Rectangle"), QStringLiteral("Rct"),
        QStringLiteral("Rectangle"), tr("A rectangular mask")));
    add(shapeMask(nodetypes::EllipseMask, tr("Ellipse"), QStringLiteral("Elp"),
        QStringLiteral("Ellipse"), tr("An elliptical mask")));
    {
        NodeDescriptor d = node(nodetypes::PolygonMask, tr("Polygon"), QStringLiteral("Ply"),
            QStringLiteral("Polygon"), C::Mask, tr("A mask from a closed polygon"));
        d.inputs = {port(nodeports::Mask, tr("Mask"), PortKind::Mask, false)};
        d.mainInput = nodeports::Mask;
        d.outputKind = PortKind::Mask;
        d.params = centre();
        d.params += {number(k::Size, tr("Size"), 0, 100, 1, 0.01), angle()};
        d.params += maskCommon();
        d.controls = ViewerControls::Polygon;
        add(d);
    }

    // ---- Color ----
    {
        NodeDescriptor d = node(nodetypes::ColorCorrector, tr("Color Corrector"),
            QStringLiteral("CC"), QStringLiteral("ColorCorrector"), C::Color,
            tr("Lift, gamma, gain, saturation and levels in one node"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = {number(k::Lift, tr("Lift"), -1, 1, 0, 0.001),
            number(k::Gamma, tr("Gamma"), 0.1, 4, 1, 0.01),
            number(k::Gain, tr("Gain"), 0, 4, 1, 0.01),
            number(k::Saturation, tr("Saturation"), 0, 4, 1, 0.01),
            number(k::InBlack, tr("Input Black"), 0, 1, 0, 0.001),
            number(k::InWhite, tr("Input White"), 0, 1, 1, 0.001),
            number(k::LevelGamma, tr("Levels Gamma"), 0.1, 10, 1, 0.01),
            number(k::OutBlack, tr("Output Black"), 0, 1, 0, 0.001),
            number(k::OutWhite, tr("Output White"), 0, 1, 1, 0.001)};
        add(d);
    }
    {
        NodeDescriptor d = node(nodetypes::ChannelBooleans, tr("Channel Booleans"),
            QStringLiteral("Bol"), QStringLiteral("ChannelBooleans"), C::Color,
            tr("Copies, swaps or replaces the colour and alpha channels"));
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        const QStringList values {channelsources::Red, channelsources::Green, channelsources::Blue,
            channelsources::Alpha, channelsources::Luma, channelsources::Black,
            channelsources::White};
        const QStringList labels {
            tr("Red"), tr("Green"), tr("Blue"), tr("Alpha"), tr("Luma"), tr("Black"), tr("White")};
        d.params = {choice(k::ToRed, tr("To Red"), values, labels, channelsources::Red),
            choice(k::ToGreen, tr("To Green"), values, labels, channelsources::Green),
            choice(k::ToBlue, tr("To Blue"), values, labels, channelsources::Blue),
            choice(k::ToAlpha, tr("To Alpha"), values, labels, channelsources::Alpha)};
        add(d);
    }

    // ---- Every Edit page video effect as a node ----
    for (const EffectDescriptor* e :
        EffectRegistry::instance().byCategory(EffectCategory::VideoEffect)) {
        NodeDescriptor d = node(effectNodeType(e->id), e->name, shortNameOf(e->name),
            nameBaseOf(e->name), C::Effect, e->description);
        d.group = e->group;
        d.effectId = e->id;
        d.inputs = {inputPort(), maskPort()};
        d.mainInput = nodeports::Input;
        d.params = e->params;
        add(d);
    }
}

} // namespace kk
