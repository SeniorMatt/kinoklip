// SPDX-License-Identifier: GPL-3.0-or-later
// NodeRegistry — the catalogue of Compositing page node types (docs/compositing-page.md):
// ports, parameters (reusing ParamDescriptor) and viewer controls. Descriptors only; the
// pixel code is in src/render (GlComposition, CpuComposition), keyed by descriptor id.
#pragma once

#include "core/EffectRegistry.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace kk {

/// What a port carries. Image: a premultiplied RGBA picture. Mask: a matte (its alpha is
/// used; mask nodes output white with alpha = coverage).
enum class PortKind : quint8 {
    Image,
    Mask
};

/// One input port of a node type.
struct NodePort
{
    QString id; ///< key in CompNode::inputs ("background", "foreground", "mask", "input")
    QString label; ///< translated
    PortKind kind = PortKind::Image;
    /// The node needs it to produce anything useful (drawn highlighted). Evaluation never
    /// fails: an unconnected port reads as a transparent picture / an empty mask... except a
    /// "mask" port, which reads as "everywhere" when unconnected.
    bool required = false;
    /// The picture is only analysed (the Tracker's tracking source), never drawn into the
    /// node's output: evaluation of the node does not pull it.
    bool analysisOnly = false;
};

/// Node library sections (Nodes panel, Select Node list).
enum class NodeCategory : quint8 {
    InputOutput, ///< MediaIn, MediaOut
    Composite, ///< Merge, Dissolve
    Transform, ///< Transform, Crop, Resize, Letterbox
    Generator, ///< Background, Text, Fast Noise
    Mask, ///< Rectangle, Ellipse, Polygon
    Color, ///< Color Corrector, Channel Booleans
    Effect ///< every EffectRegistry video effect ("fx:<id>")
};

/// On-viewer controls the Compositing page viewers draw for the selected node.
enum class ViewerControls : quint8 {
    None,
    /// Centre / size / angle handles: nodekeys::CenterX, CenterY, Size, Angle (Transform,
    /// Merge (the foreground), Text (centre only), Polygon (whole shape)).
    Transform,
    /// A rotated box: CenterX, CenterY, Width, Height, Angle (Rectangle and Ellipse masks).
    Rect,
    /// The editable point path CompNode::path plus the Transform handles (Polygon mask).
    Polygon,
    /// The tracked point (CenterX, CenterY) with its pattern and search boxes (PatternSize,
    /// SearchSize): Tracker.
    Tracker
};

/// Describes one node type.
///
/// Coordinate conventions of every built-in node (like a Resolve user knows them from the
/// node compositor): positions are normalised frame coordinates, x 0..1 from left to right
/// and y 0..1 from BOTTOM to TOP (0.5, 0.5 = the centre of the frame); sizes are factors
/// (1 = unchanged); widths and heights of shapes are fractions of the frame width / height;
/// soft edges are fractions of the frame width; angles are degrees counter-clockwise. Effect
/// nodes ("fx:") keep their Edit page units (pixel sizes are timeline pixels).
struct NodeDescriptor
{
    QString id; ///< stable type id stored in CompNode::type ("merge", "fx:gaussian_blur")
    QString name; ///< translated display name ("Merge", "Gaussian Blur")
    QString shortName; ///< 2-4 letters for small tiles and the toolbar ("Mrg", "Blur")
    /// Base of new node names (name without spaces, untranslated: "Merge" -> "Merge1").
    QString nameBase;
    NodeCategory category = NodeCategory::Effect;
    QString description; ///< one-line tooltip
    /// Effect nodes: the effects library folder (effectgroups::); empty otherwise.
    QString group;
    /// Effect nodes: the EffectRegistry id ("gaussian_blur"); empty otherwise.
    QString effectId;
    QVector<NodePort> inputs; ///< in display order
    /// Port whose image a disabled node passes through, and that "add after the selected
    /// node" connects to; empty for nodes without an image input (generators, MediaIn).
    QString mainInput;
    int outputs = 1; ///< number of outputs (0 for MediaOut)
    PortKind outputKind = PortKind::Image; ///< Mask for the mask nodes
    QVector<ParamDescriptor> params;
    ViewerControls controls = ViewerControls::None;

    /// Port with id @p id, or nullptr.
    [[nodiscard]] const NodePort* input(const QString& id) const;
    /// Parameter descriptor with key @p key, or nullptr.
    [[nodiscard]] const ParamDescriptor* param(const QString& key) const;
    /// True for nodes that make a picture without an image input (Background, Text, noise,
    /// masks, MediaIn): adding one after a selected node merges it over that node.
    [[nodiscard]] bool isSource() const
    {
        return mainInput.isEmpty() || outputKind == PortKind::Mask;
    }
};

/// Well-known node type ids.
namespace nodetypes {
inline const QString MediaIn = QStringLiteral("media_in");
inline const QString MediaOut = QStringLiteral("media_out");
inline const QString Merge = QStringLiteral("merge");
inline const QString Dissolve = QStringLiteral("dissolve");
inline const QString Transform = QStringLiteral("transform");
inline const QString Crop = QStringLiteral("crop");
inline const QString Resize = QStringLiteral("resize");
inline const QString Letterbox = QStringLiteral("letterbox");
inline const QString Tracker = QStringLiteral("tracker");
inline const QString Background = QStringLiteral("background");
inline const QString Text = QStringLiteral("text");
inline const QString FastNoise = QStringLiteral("fast_noise");
inline const QString RectangleMask = QStringLiteral("rectangle_mask");
inline const QString EllipseMask = QStringLiteral("ellipse_mask");
inline const QString PolygonMask = QStringLiteral("polygon_mask");
inline const QString ColorCorrector = QStringLiteral("color_corrector");
inline const QString ChannelBooleans = QStringLiteral("channel_booleans");
/// Prefix of the effect nodes: "fx:" + EffectRegistry id.
inline const QString EffectPrefix = QStringLiteral("fx:");
} // namespace nodetypes

/// Well-known port ids.
namespace nodeports {
inline const QString Input = QStringLiteral("input");
inline const QString Background = QStringLiteral("background");
inline const QString Foreground = QStringLiteral("foreground");
inline const QString Mask = QStringLiteral("mask");
/// Tracker: the picture the point is followed in (the main input when not connected).
inline const QString Source = QStringLiteral("source");
} // namespace nodeports

/// Parameter keys of the built-in nodes (see NodeDescriptor for the units).
namespace nodekeys {
// Shared geometry.
inline const QString CenterX = QStringLiteral("centerX"); ///< 0..1, left to right
inline const QString CenterY = QStringLiteral("centerY"); ///< 0..1, bottom to top
inline const QString Size = QStringLiteral("size"); ///< factor
inline const QString Aspect = QStringLiteral("aspect"); ///< Transform: x / y size ratio
inline const QString Angle = QStringLiteral("angle"); ///< degrees counter-clockwise
inline const QString PivotX = QStringLiteral("pivotX"); ///< Transform: 0..1
inline const QString PivotY = QStringLiteral("pivotY");
inline const QString FlipH = QStringLiteral("flipH"); ///< bool
inline const QString FlipV = QStringLiteral("flipV"); ///< bool
// Merge / Dissolve.
inline const QString BlendMode = QStringLiteral("blendMode"); ///< choice (blendmodes::)
inline const QString Opacity = QStringLiteral("opacity"); ///< 0..1
inline const QString Mix = QStringLiteral("mix"); ///< Dissolve: 0 = background, 1 = foreground
// Crop (timeline pixels from each edge).
inline const QString Left = QStringLiteral("left");
inline const QString Right = QStringLiteral("right");
inline const QString Top = QStringLiteral("top");
inline const QString Bottom = QStringLiteral("bottom");
// Resize (timeline pixels).
inline const QString Width = QStringLiteral("width"); ///< Resize: px; masks: fraction of frame
inline const QString Height = QStringLiteral("height");
inline const QString KeepAspect = QStringLiteral("keepAspect"); ///< Resize: bool
// Tracker. The tracked point itself is CenterX / CenterY (keyframed by the tracking).
inline const QString PatternSize = QStringLiteral("patternSize"); ///< px, side of the pattern box
inline const QString SearchSize = QStringLiteral("searchSize"); ///< px, side of the search box
inline const QString TrackMode = QStringLiteral("trackMode"); ///< choice (trackmodes::)
inline const QString RefFrame = QStringLiteral("refFrame"); ///< comp frame where the offset is 0
// Letterbox.
inline const QString TargetAspect = QStringLiteral("targetAspect"); ///< width / height, e.g. 2.39
inline const QString Color = QStringLiteral("color"); ///< colour static
// Background.
inline const QString Mode = QStringLiteral("mode"); ///< Background: solid / gradient / four
inline const QString Color2 = QStringLiteral("color2");
inline const QString Color3 = QStringLiteral("color3");
inline const QString Color4 = QStringLiteral("color4");
inline const QString GradientAngle = QStringLiteral("gradientAngle"); ///< degrees ccw, 0 = l->r
// Text.
inline const QString Text = QStringLiteral("text"); ///< string
inline const QString Font = QStringLiteral("font"); ///< family name
inline const QString FontSize = QStringLiteral("fontSize"); ///< timeline px
inline const QString Bold = QStringLiteral("bold");
inline const QString Italic = QStringLiteral("italic");
inline const QString OutlineColor = QStringLiteral("outlineColor");
inline const QString OutlineWidth = QStringLiteral("outlineWidth"); ///< timeline px
inline const QString Shadow = QStringLiteral("shadow"); ///< bool
// Fast Noise.
inline const QString Detail = QStringLiteral("detail"); ///< octaves
inline const QString Contrast = QStringLiteral("contrast");
inline const QString Brightness = QStringLiteral("brightness");
inline const QString Scale = QStringLiteral("scale"); ///< noise cells across the frame width
inline const QString Seethe = QStringLiteral("seethe"); ///< evolution per second
inline const QString Seed = QStringLiteral("seed");
// Masks.
inline const QString SoftEdge = QStringLiteral("softEdge"); ///< fraction of the frame width
inline const QString Invert = QStringLiteral("invert"); ///< bool
inline const QString Level = QStringLiteral("level"); ///< mask opacity 0..1
// Color Corrector.
inline const QString Lift = QStringLiteral("lift"); ///< added to the shadows, -1..1
inline const QString Gamma = QStringLiteral("gamma"); ///< midtone power, 1 = unchanged
inline const QString Gain = QStringLiteral("gain"); ///< multiplier
inline const QString Saturation = QStringLiteral("saturation"); ///< 1 = unchanged
inline const QString InBlack = QStringLiteral("inBlack"); ///< levels, 0..1
inline const QString InWhite = QStringLiteral("inWhite");
inline const QString LevelGamma = QStringLiteral("levelGamma");
inline const QString OutBlack = QStringLiteral("outBlack");
inline const QString OutWhite = QStringLiteral("outWhite");
// Channel Booleans (choice of channelsources::).
inline const QString ToRed = QStringLiteral("toRed");
inline const QString ToGreen = QStringLiteral("toGreen");
inline const QString ToBlue = QStringLiteral("toBlue");
inline const QString ToAlpha = QStringLiteral("toAlpha");
} // namespace nodekeys

/// Merge blend modes (nodekeys::BlendMode values), in the order of the choice list.
namespace blendmodes {
inline const QString Normal = QStringLiteral("normal");
inline const QString Screen = QStringLiteral("screen");
inline const QString Multiply = QStringLiteral("multiply");
inline const QString Add = QStringLiteral("add");
inline const QString Overlay = QStringLiteral("overlay");
inline const QString Darken = QStringLiteral("darken");
inline const QString Lighten = QStringLiteral("lighten");
inline const QString Difference = QStringLiteral("difference");
inline const QString Subtract = QStringLiteral("subtract");
inline const QString HardLight = QStringLiteral("hardLight");
inline const QString SoftLight = QStringLiteral("softLight");
inline const QString ColorDodge = QStringLiteral("colorDodge");
inline const QString ColorBurn = QStringLiteral("colorBurn");
inline const QString Exclusion = QStringLiteral("exclusion");
inline const QString LinearBurn = QStringLiteral("linearBurn");
inline const QString LinearLight = QStringLiteral("linearLight");
inline const QString VividLight = QStringLiteral("vividLight");
inline const QString PinLight = QStringLiteral("pinLight");
inline const QString HardMix = QStringLiteral("hardMix");
/// Index of @p mode (0 = Normal for unknown values): the number the blend functions
/// (compmath::blendChannel(), the GLSL kBlendFunctionGlsl) switch on. Stable: new modes are
/// appended.
[[nodiscard]] int index(const QString& mode);
/// Every mode id in index() order.
[[nodiscard]] const QStringList& ids();
/// The modes in the order of the Inspector's Composite Mode list (Resolve's order: Normal,
/// Add, Subtract, Difference, Multiply, Screen, Overlay, Hard Light, Soft Light, ...).
[[nodiscard]] const QStringList& compositeOrder();
/// Translated name of mode @p id ("Soft Light"); @p id itself if unknown.
[[nodiscard]] QString displayName(const QString& id);
} // namespace blendmodes

/// Background modes (nodekeys::Mode).
namespace backgroundmodes {
inline const QString Solid = QStringLiteral("solid");
inline const QString Gradient = QStringLiteral("gradient"); ///< color -> color2 along the angle
inline const QString FourCorner = QStringLiteral("four"); ///< tl color, tr 2, bl 3, br 4
} // namespace backgroundmodes

/// Tracker output modes (nodekeys::TrackMode values).
namespace trackmodes {
inline const QString MatchMove = QStringLiteral("matchMove"); ///< the input follows the track
inline const QString Stabilize = QStringLiteral("stabilize"); ///< the input cancels the track
} // namespace trackmodes

/// Channel Booleans sources, in the order of the choice list.
namespace channelsources {
inline const QString Red = QStringLiteral("red");
inline const QString Green = QStringLiteral("green");
inline const QString Blue = QStringLiteral("blue");
inline const QString Alpha = QStringLiteral("alpha");
inline const QString Luma = QStringLiteral("luma");
inline const QString Black = QStringLiteral("black");
inline const QString White = QStringLiteral("white");
/// Index of @p source in the choice list (-1 for unknown values).
[[nodiscard]] int index(const QString& source);
} // namespace channelsources

/// Process-wide catalogue of node types: the built-in nodes plus one "fx:<id>" node per
/// EffectRegistry video effect (generated on first access, so a new effect appears on the
/// Compositing page without any change here). Read-only after construction; safe to read
/// from any thread.
class NodeRegistry
{
public:
    /// The global catalogue.
    static const NodeRegistry& instance();

    /// Descriptor of type @p id, or nullptr. O(1).
    [[nodiscard]] const NodeDescriptor* find(const QString& id) const;
    /// All descriptors: built-ins in library order, then the effect nodes.
    [[nodiscard]] const QVector<NodeDescriptor>& all() const { return m_descriptors; }
    /// Descriptors of @p category in library order.
    [[nodiscard]] QVector<const NodeDescriptor*> byCategory(NodeCategory category) const;
    /// ParamSet with the defaults of type @p id (empty if unknown).
    [[nodiscard]] ParamSet defaultParams(const QString& id) const;
    /// Translated name of a library section.
    [[nodiscard]] static QString categoryName(NodeCategory category);
    /// Node type id of EffectRegistry effect @p effectId ("fx:" + id).
    [[nodiscard]] static QString effectNodeType(const QString& effectId)
    {
        return nodetypes::EffectPrefix + effectId;
    }

private:
    NodeRegistry();
    void add(NodeDescriptor d);

    QVector<NodeDescriptor> m_descriptors;
    QHash<QString, qsizetype> m_index;
};

} // namespace kk
