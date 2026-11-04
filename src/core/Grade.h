// SPDX-License-Identifier: GPL-3.0-or-later
// Grade — the colour grade of the Color page (docs/color-page.md): a small node graph of
// corrector nodes (primaries, curves, HSL qualifier, power windows, LUT) and parallel / layer
// mixer nodes. A clip has one (ClipData::grade), a timeline has one applied after every clip
// (TimelineData::grade). Plain value types plus pure graph helpers; the maths lives in
// core/GradeMath.h, rendering in render/GlGrade (GPU) and render/CpuGrade (CPU fallback).
#pragma once

#include "core/Animation.h"

#include <QPointF>
#include <QString>
#include <QVector>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace kk {

/// What a grade node does.
enum class GradeNodeType : quint8 {
    /// A corrector: primaries, curves and LUT, limited by its key (qualifier x windows) and
    /// mixed with its input by the node's Mix. Exactly one input (0 = the grade's input
    /// picture: the clip after its Edit page effects, or the timeline composite).
    Corrector,
    /// Parallel mixer: the equal-weight average of its inputs (usually correctors fed from
    /// the same picture), so corrections made in parallel do not stack.
    Parallel,
    /// Layer mixer: input 0 at the bottom, every further input laid over the result with its
    /// node's key as alpha (1 for nodes without a key, and for mixers); the last input is on
    /// top.
    Layer
};

/// One control point of a curve; x and y in 0..1.
struct CurvePoint
{
    double x = 0.0;
    double y = 0.0;
    friend bool operator==(const CurvePoint&, const CurvePoint&) = default;
};

/// Control points of one curve, sorted by x. Empty = neutral (identity / no change).
using CurvePoints = QVector<CurvePoint>;

/// The curves of a corrector node (Curves palette).
enum class GradeCurve : quint8 {
    Luma, ///< Y (custom curve): applied to R, G and B alike, before the channel curves
    Red,
    Green,
    Blue,
    HueHue, ///< hue vs hue: y 0.5 = no shift, 0 / 1 = -/+ half a turn
    HueSat, ///< hue vs saturation: y 0.5 = unchanged, 0 = grey, 1 = double
    HueLum, ///< hue vs luminance: y 0.5 = unchanged, 0 = luma to 0, 1 = double luma
    LumSat, ///< luma vs saturation: y 0.5 = unchanged, 0 = grey, 1 = double
    SatSat ///< saturation vs saturation: y 0.5 = unchanged, 0 = grey, 1 = double
};
/// Number of GradeCurve values.
inline constexpr int kGradeCurveCount = 9;

/// True for the curves whose x is a hue (periodic: x = 0 and x = 1 are the same hue).
[[nodiscard]] constexpr bool isHueCurve(GradeCurve c)
{
    return c == GradeCurve::HueHue || c == GradeCurve::HueSat || c == GradeCurve::HueLum;
}
/// True for the Y / R / G / B curves (output = input when neutral).
[[nodiscard]] constexpr bool isChannelCurve(GradeCurve c)
{
    return int(c) <= int(GradeCurve::Blue);
}
/// The neutral output of curve @p c: y = x for the Y/R/G/B curves (returned as -1), 0.5 for
/// the hue / luma / saturation curves.
[[nodiscard]] constexpr double curveNeutral(GradeCurve c)
{
    return isChannelCurve(c) ? -1.0 : 0.5;
}

/// The curves of a node. Plain value type.
struct GradeCurves
{
    std::array<CurvePoints, kGradeCurveCount> points;

    [[nodiscard]] const CurvePoints& operator[](GradeCurve c) const { return points[size_t(c)]; }
    [[nodiscard]] CurvePoints& operator[](GradeCurve c) { return points[size_t(c)]; }
    /// True if curve @p c is neutral (no points, or every point on the neutral line).
    [[nodiscard]] bool isIdentity(GradeCurve c) const;
    /// True if every curve is neutral.
    [[nodiscard]] bool isIdentity() const;
    friend bool operator==(const GradeCurves&, const GradeCurves&) = default;
};

/// HSL qualifier of a corrector node: a soft key selecting pixels of the node's INPUT by hue,
/// saturation and lightness (HSL). Ranges are 0..1 (hue in turns, 0 = red, 1/3 = green,
/// 2/3 = blue). Softness widens each edge by a smooth ramp outside the range. Not
/// keyframeable (a qualifier follows the picture, not the time).
struct GradeQualifier
{
    bool enabled = false;
    bool invert = false;
    bool useHue = true;
    bool useSat = true;
    bool useLum = true;
    double hueCenter = 0.0; ///< turns
    double hueWidth = 0.1; ///< full width of the selected hue range, turns (0..1)
    double hueSoft = 0.05; ///< turns
    double satLow = 0.2;
    double satHigh = 1.0;
    double satSoft = 0.05;
    double lumLow = 0.0;
    double lumHigh = 1.0;
    double lumSoft = 0.05;

    friend bool operator==(const GradeQualifier&, const GradeQualifier&) = default;
};

/// Shape of a power window.
enum class GradeWindowShape : quint8 {
    Circle, ///< ellipse of Width x Height
    Linear, ///< rectangle of Width x Height (Resolve's "linear" window)
    Polygon, ///< closed polygon (GradeWindow::points)
    Gradient ///< half plane: selects the side above the line through the centre (before rotation)
};

/// Well-known keys of GradeWindow::params (all keyframeable). Positions and sizes are
/// fractions of the graded picture (the clip's content or the timeline frame): x 0 = left,
/// 1 = right; y 0 = top, 1 = bottom. Softness is a fraction of the picture height.
namespace gradewindowkeys {
inline const QString CenterX = QStringLiteral("centerX"); ///< default 0.5
inline const QString CenterY = QStringLiteral("centerY"); ///< default 0.5
inline const QString Width = QStringLiteral("width"); ///< fraction of the width; default 0.5
inline const QString Height = QStringLiteral("height"); ///< of the height; default 0.5
inline const QString Rotation = QStringLiteral("rotation"); ///< degrees clockwise; default 0
inline const QString SoftIn = QStringLiteral("softIn"); ///< soft edge inside; default 0
inline const QString SoftOut = QStringLiteral("softOut"); ///< soft edge outside; default 0.05
inline const QString Opacity = QStringLiteral("opacity"); ///< 0..1; default 1
/// Documented default of @p key (unknown keys: 0).
[[nodiscard]] double defaultValue(QStringView key);
/// Every window key, in inspector order.
[[nodiscard]] const QVector<QString>& allKeys();
/// Translated label of @p key.
[[nodiscard]] QString label(const QString& key);
} // namespace gradewindowkeys

/// Largest number of windows a node renders (more are ignored) and of polygon points.
inline constexpr int kMaxGradeWindows = 8;
inline constexpr int kMaxWindowPoints = 32;

/// A power window of a corrector node. Windows combine as a union (m = M + w - M w, each
/// window's coverage w after its invert and opacity); the result multiplies the qualifier key.
struct GradeWindow
{
    quint64 id = 0; ///< unique within the grade (shares ClipGrade::allocateId() with nodes)
    GradeWindowShape shape = GradeWindowShape::Circle;
    bool enabled = true;
    bool invert = false;
    ParamSet params; ///< gradewindowkeys::*
    /// Polygon only: points in window units (-0.5..0.5 of Width / Height around the centre,
    /// y down, before rotation); at most kMaxWindowPoints are used.
    QVector<QPointF> points;

    /// Value of @p key at @p localFrame with its documented default.
    [[nodiscard]] double value(const QString& key, int64_t localFrame) const
    {
        return params.value(key, localFrame, gradewindowkeys::defaultValue(key));
    }
    /// True if the window contributes (enabled; a polygon needs 3 points).
    [[nodiscard]] bool isRenderable() const;
    friend bool operator==(const GradeWindow&, const GradeWindow&) = default;
};

/// Stable string ids of window shapes ("circle", "linear", "polygon", "gradient").
[[nodiscard]] QString gradeWindowShapeToString(GradeWindowShape shape);
[[nodiscard]] GradeWindowShape gradeWindowShapeFromString(const QString& text);

/// Interpolation of a node's 3D LUT.
enum class LutInterpolation : quint8 {
    Tetrahedral, ///< the default (smoother on neutral axes)
    Trilinear
};

/// Well-known keys of GradeNode::params (all keyframeable; RGB channels R/G/B and the master
/// Y of each wheel). Units: lift / gamma / offset are additive offsets (0 = neutral), gain a
/// factor (1 = neutral), contrast a factor around the pivot, saturation a factor, hue degrees,
/// temperature / tint -1..1 (0 = neutral), lum mix 0..1, mix 0..1 (the node's opacity).
namespace gradekeys {
inline const QString LiftR = QStringLiteral("liftR");
inline const QString LiftG = QStringLiteral("liftG");
inline const QString LiftB = QStringLiteral("liftB");
inline const QString LiftY = QStringLiteral("liftY");
inline const QString GammaR = QStringLiteral("gammaR");
inline const QString GammaG = QStringLiteral("gammaG");
inline const QString GammaB = QStringLiteral("gammaB");
inline const QString GammaY = QStringLiteral("gammaY");
inline const QString GainR = QStringLiteral("gainR");
inline const QString GainG = QStringLiteral("gainG");
inline const QString GainB = QStringLiteral("gainB");
inline const QString GainY = QStringLiteral("gainY");
inline const QString OffsetR = QStringLiteral("offsetR");
inline const QString OffsetG = QStringLiteral("offsetG");
inline const QString OffsetB = QStringLiteral("offsetB");
inline const QString OffsetY = QStringLiteral("offsetY");
inline const QString Contrast = QStringLiteral("contrast"); ///< factor; default 1
inline const QString Pivot = QStringLiteral("pivot"); ///< 0..1; default 0.435
inline const QString Saturation = QStringLiteral("saturation"); ///< factor; default 1
inline const QString Hue = QStringLiteral("hue"); ///< degrees; default 0
inline const QString Temperature = QStringLiteral("temperature"); ///< -1..1; default 0
inline const QString Tint = QStringLiteral("tint"); ///< -1..1 (+ = magenta); default 0
inline const QString LumMix = QStringLiteral("lumMix"); ///< 0..1; default 1
inline const QString Mix = QStringLiteral("mix"); ///< 0..1 node opacity; default 1

/// The four colour wheels (Primaries palette), in palette order.
enum class Wheel : quint8 {
    Lift,
    Gamma,
    Gain,
    Offset
};
/// Key of channel @p channel (0 R, 1 G, 2 B, 3 master Y) of wheel @p wheel.
[[nodiscard]] const QString& wheelKey(Wheel wheel, int channel);
/// Documented default of grade parameter @p key (unknown keys: 0).
[[nodiscard]] double defaultValue(QStringView key);
/// Every node key, wheels first (keyframe editors, reset, tests).
[[nodiscard]] const QVector<QString>& allKeys();
/// Translated label of @p key ("Lift R", "Contrast", ...).
[[nodiscard]] QString label(const QString& key);
/// Useful editing range of @p key (curve editor fallback range, field limits).
[[nodiscard]] std::pair<double, double> range(const QString& key);
} // namespace gradekeys

/// One node of a grade.
struct GradeNode
{
    quint64 id = 0; ///< unique within the grade (ClipGrade::allocateId()); never 0
    GradeNodeType type = GradeNodeType::Corrector;
    QString label; ///< user label; empty = the node number ("01", "02", ...)
    bool enabled = true; ///< false = bypassed: passes its input through (Ctrl+D)
    ParamSet params; ///< gradekeys::*
    GradeCurves curves;
    GradeQualifier qualifier;
    QVector<GradeWindow> windows;
    /// .cube LUT applied last in the node (empty = none); absolute path.
    QString lutPath;
    LutInterpolation lutInterpolation = LutInterpolation::Tetrahedral;
    /// Input node ids, 0 = the grade's input picture. Corrector: exactly one; mixers: one or
    /// more (at most kMaxMixerInputs are used), in mixing order.
    QVector<quint64> inputs {0};
    QPointF pos; ///< position in the node editor (editor units)

    /// Value of parameter @p key at @p localFrame with its documented default.
    [[nodiscard]] double value(const QString& key, int64_t localFrame) const
    {
        return params.value(key, localFrame, gradekeys::defaultValue(key));
    }
    /// True for Parallel / Layer nodes.
    [[nodiscard]] bool isMixer() const { return type != GradeNodeType::Corrector; }
    /// True if the node has a key that limits it (enabled qualifier or a renderable window).
    [[nodiscard]] bool hasKey() const;
    /// True if any parameter (node or window) has keyframes.
    [[nodiscard]] bool isAnimated() const;
    /// Shifts every key (node and window parameters) by @p deltaFrames.
    void shiftKeys(int64_t deltaFrames);
    friend bool operator==(const GradeNode&, const GradeNode&) = default;
};

/// Largest number of inputs a mixer uses.
inline constexpr int kMaxMixerInputs = 8;

/// A grade (of a clip or of a timeline). Empty (no nodes) = ungraded: renders exactly as
/// before. Graph: every node names its inputs; `output` is the node whose result leaves the
/// grade (0 with nodes present = disconnected, the picture passes ungraded). Nodes the output
/// does not reach are kept but ignored. Frames of keyframes are clip-local for clip grades and
/// timeline frames for the timeline grade. Plain value type.
struct ClipGrade
{
    QVector<GradeNode> nodes;
    quint64 output = 0;

    [[nodiscard]] bool isEmpty() const { return nodes.isEmpty(); }
    /// A fresh node / window id (largest id in use + 1).
    [[nodiscard]] quint64 allocateId() const;
    /// Index of node @p id, or -1.
    [[nodiscard]] int indexOf(quint64 id) const;
    [[nodiscard]] const GradeNode* node(quint64 id) const;
    [[nodiscard]] GradeNode* node(quint64 id);
    /// The window with @p id in any node, or nullptr (@p owner gets its node).
    [[nodiscard]] const GradeWindow* window(quint64 id, const GradeNode** owner = nullptr) const;
    [[nodiscard]] GradeWindow* window(quint64 id, GradeNode** owner = nullptr);
    /// True if any node has keyframes.
    [[nodiscard]] bool isAnimated() const;
    /// Shifts every key by @p deltaFrames (head trim, split; like ParamSet::shiftKeys).
    void shiftKeys(int64_t deltaFrames);
    friend bool operator==(const ClipGrade&, const ClipGrade&) = default;
};

/// Display name of a node: its label, or the corrector's 1-based number among the correctors
/// of the node list ("01"; mixers are not numbered: empty without a label).
[[nodiscard]] QString gradeNodeName(const ClipGrade& grade, int index);

/// Stable string id of a node type in project files ("corrector", "parallel", "layer").
[[nodiscard]] QString gradeNodeTypeToString(GradeNodeType type);
/// Parses gradeNodeTypeToString(); unknown = Corrector.
[[nodiscard]] GradeNodeType gradeNodeTypeFromString(const QString& text);

namespace gradeops {

/// Indices into grade.nodes of the nodes the output reaches, inputs before their consumers
/// (the evaluation order). Cycles (damaged data) are cut. Empty when output is 0.
[[nodiscard]] std::vector<int> evaluationOrder(const ClipGrade& grade);
/// True if corrector @p node changes nothing at @p localFrame (bypassed, mix 0, or neutral
/// primaries and curves and no LUT). Mixers are never identity.
[[nodiscard]] bool isNodeIdentity(const GradeNode& node, int64_t localFrame);
/// True if rendering @p grade at @p localFrame would change nothing (fast path: the picture
/// is drawn without a grade pass). An empty or disconnected grade is identity.
[[nodiscard]] bool isIdentity(const ClipGrade& grade, int64_t localFrame);

/// A grade with one neutral corrector connected source -> node -> output. Returns its id.
quint64 initialize(ClipGrade& grade);
/// Adds a serial corrector after node @p after (Alt+S): it takes @p after's output and
/// everything that used @p after now uses the new node. @p after = 0 or unknown: after the
/// output node (or a new graph). Returns the new id.
quint64 addSerialNode(ClipGrade& grade, quint64 after);
/// Adds a serial corrector before node @p before (Shift+S in Resolve): it takes @p before's
/// first input and feeds @p before. Unknown @p before: like addSerialNode(). Returns its id.
quint64 addSerialNodeBefore(ClipGrade& grade, quint64 before);
/// Adds a corrector parallel to @p node (Alt+P): fed from @p node's input and mixed with it in
/// a Parallel mixer (an existing Parallel mixer directly after @p node gets one more input).
/// Returns the new corrector's id.
quint64 addParallelNode(ClipGrade& grade, quint64 node);
/// Like addParallelNode() with a Layer mixer (Alt+L); the new node is the top layer.
quint64 addLayerNode(ClipGrade& grade, quint64 node);
/// Deletes node @p id; its consumers (and the output) take its first input instead. Mixer
/// inputs that end up identical are merged; a mixer left with one input is removed as well.
/// Returns false if there is no such node.
bool removeNode(ClipGrade& grade, quint64 id);
/// Connects the output of node @p from (0 = the input picture) to input @p inputIndex of node
/// @p to (0 = the grade output; the index is then ignored). For mixers an index equal to the
/// input count appends an input. Refused (false) when it would create a cycle or the target
/// does not exist.
bool connect(ClipGrade& grade, quint64 from, quint64 to, int inputIndex);
/// Removes input @p inputIndex of mixer @p to (keeping at least one), or resets a
/// corrector's input to the source. Returns false if nothing changed.
bool disconnect(ClipGrade& grade, quint64 to, int inputIndex);
/// True if node @p id is upstream of (or equal to) @p of: following inputs from @p of reaches it.
[[nodiscard]] bool reaches(const ClipGrade& grade, quint64 of, quint64 id);
/// Resets the corrections of node @p id (primaries, curves, qualifier, windows, LUT, mix)
/// keeping its place, label, connections and bypass state. False if there is no such node.
bool resetNode(ClipGrade& grade, quint64 id);
/// Bypasses (false) or enables node @p id. False if there is no such node or no change.
bool setNodeEnabled(ClipGrade& grade, quint64 id, bool enabled);
/// Sets a default editor position for node @p id right of its first input.
void placeNode(ClipGrade& grade, quint64 id);
/// Adds a window of @p shape to corrector @p nodeId with default parameters (a polygon gets a
/// diamond). Returns its id, 0 when the node does not exist, is a mixer or is full.
quint64 addWindow(ClipGrade& grade, quint64 nodeId, GradeWindowShape shape);
/// Removes window @p windowId (from whichever node has it). False if there is none.
bool removeWindow(ClipGrade& grade, quint64 windowId);
/// The node a new serial / parallel / layer node goes after when nothing is selected: the
/// output node, else the last node; 0 for an empty grade.
[[nodiscard]] quint64 defaultSelection(const ClipGrade& grade);

} // namespace gradeops

} // namespace kk
