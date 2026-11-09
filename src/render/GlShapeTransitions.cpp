// SPDX-License-Identifier: GPL-3.0-or-later
// GlShapeTransitions — the Iris, Shape and Wipe folders in one parameterised shader.
//
// Every one of them gives each pixel an "arrival" s in [0, 1]: how late the moving edge reaches
// it (0 = where the edge starts, 1 = the last pixels). The edge travels from 0 to 1 + border +
// feather as the progress goes 0 -> 1:
//   s >= edge          outgoing picture
//   edge - border <= s < edge   border colour
//   s < edge - border  incoming picture
// with the feather softening both boundaries. Only the field s differs between the transitions.
// For the iris and shape ones s is a "gauge" of the shape: the factor by which the shape
// (centred, turned, stretched) has to be scaled to touch the pixel, divided by the factor that
// reaches the farthest corner; so the shape itself grows with the edge.
#include "core/TransitionKeys.h"
#include "render/GlShaderTransition.h"

#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

namespace k = tkeys;

// Field ids (uField). Keep in step with the table below.
enum Field {
    FieldPolygon = 0, // regular polygon with uN sides, one vertex up before rotation
    FieldOval,
    FieldCross,
    FieldEye,
    FieldArrow,
    FieldHeart,
    FieldStar, // uN points
    FieldBox, // grows from uAnchor
    FieldTriangleSide, // uFlag 0 = from the left edge, 1 = from the right edge
    FieldBand, // uN bands, uFlag 0/1 horizontal/vertical, uFlag2 1 = bilinear
    FieldCenterWipe,
    FieldClock,
    FieldRadial,
    FieldSpiral,
    FieldVenetian,
    FieldX
};

const char* const kShapeFamily = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
uniform int uField;
uniform float uAspect;       // output width / height
uniform vec2 uCenter;        // centre offset, frame heights (x already times the aspect)
uniform float uShapeAspect;  // shape width : height
uniform float uRot;          // radians clockwise
uniform float uAngle;        // radians
uniform int uN;
uniform float uFlag;
uniform float uFlag2;
uniform vec2 uAnchor;        // Box: 0 / 0.5 / 1 per axis
uniform float uBorder;       // in units of s
uniform float uFeather;      // in units of s
uniform float uReverse;
uniform vec4 uBorderColor;   // premultiplied

const float PI = 3.14159265;
const int FieldPolygon = 0, FieldOval = 1, FieldCross = 2, FieldEye = 3, FieldArrow = 4,
    FieldHeart = 5, FieldStar = 6, FieldBox = 7, FieldTriangleSide = 8, FieldBand = 9,
    FieldCenterWipe = 10, FieldClock = 11, FieldRadial = 12, FieldSpiral = 13,
    FieldVenetian = 14, FieldX = 15;

float polygonGauge(vec2 q, int n)
{
    float g = -1e9;
    float stepAngle = 2.0 * PI / float(n);
    for (int i = 0; i < 8; ++i) {
        if (i >= n)
            break;
        float a = -0.5 * PI + (float(i) + 0.5) * stepAngle; // face normals, vertex 0 straight up
        g = max(g, dot(q, vec2(cos(a), sin(a))));
    }
    return g;
}

float arrowGauge(vec2 q)
{
    // Head: triangle (0,-1) (-0.7,0.1) (0.7,0.1); shaft: |x| <= 0.3, -0.5 <= y <= 1. A union of
    // convex shapes that contain the centre has the smaller of their gauges.
    const vec2 n = vec2(0.8437, 0.5369);
    float head = max(max(dot(q, vec2(-n.x, -n.y)), dot(q, vec2(n.x, -n.y))) / 0.5369, q.y / 0.1);
    float shaft = max(max(abs(q.x) / 0.3, q.y), -q.y / 0.5);
    return min(head, shaft);
}

bool insideHeart(vec2 q, float r)
{
    float x = q.x / r;
    float y = -q.y / r; // the classic curve has y up
    float a = x * x + y * y - 1.0;
    return a * a * a - x * x * y * y * y <= 0.0;
}

float heartGauge(vec2 q)
{
    float lo = 0.0;
    float hi = 20.0;
    for (int i = 0; i < 16; ++i) {
        float mid = 0.5 * (lo + hi);
        if (insideHeart(q, mid))
            hi = mid;
        else
            lo = mid;
    }
    return hi;
}

float starGauge(vec2 q, int n)
{
    float sector = 2.0 * PI / float(n);
    float theta = atan(q.x, -q.y);
    float a = abs(mod(theta + 0.5 * sector, sector) - 0.5 * sector); // angle to the nearest tip
    float inner = n == 4 ? 0.45 : (n == 5 ? 0.4 : 0.55);
    vec2 tip = vec2(1.0, 0.0);
    vec2 valley = inner * vec2(cos(0.5 * sector), sin(0.5 * sector));
    vec2 d = vec2(cos(a), sin(a));
    vec2 e = valley - tip;
    float radius = (tip.x * valley.y - tip.y * valley.x) / (d.x * e.y - d.y * e.x);
    return length(q) / radius;
}

// Gauge of the centred shapes; q is in the shape's own frame.
float shapeGauge(vec2 q)
{
    if (uField == FieldPolygon)
        return polygonGauge(q, uN);
    if (uField == FieldOval)
        return length(q);
    if (uField == FieldCross)
        return min(max(4.0 * abs(q.x), abs(q.y)), max(abs(q.x), 4.0 * abs(q.y)));
    if (uField == FieldEye)
        return 1.5 * abs(q.y) + length(q);
    if (uField == FieldArrow)
        return arrowGauge(q);
    if (uField == FieldHeart)
        return heartGauge(q);
    return starGauge(q, uN);
}

vec2 shapeFrame(vec2 uv)
{
    vec2 p = (uv - 0.5) * vec2(uAspect, 1.0) - uCenter;
    float c = cos(uRot);
    float s = sin(uRot);
    vec2 q = vec2(c * p.x + s * p.y, -s * p.x + c * p.y);
    return vec2(q.x / uShapeAspect, q.y);
}

float arrival(vec2 uv)
{
    vec2 p = (uv - 0.5) * vec2(uAspect, 1.0) - uCenter;
    if (uField <= FieldStar) {
        // Normalise by the farthest corner and edge middle of the frame.
        float farthest = 0.0;
        for (int i = 0; i < 8; ++i) {
            vec2 c = i < 4 ? vec2(float(i & 1) - 0.5, float(i >> 1) - 0.5) * 2.0
                           : (i == 4 ? vec2(0.0, 1.0) : (i == 5 ? vec2(0.0, -1.0)
                                     : (i == 6 ? vec2(1.0, 0.0) : vec2(-1.0, 0.0))));
            farthest = max(farthest, shapeGauge(shapeFrame(0.5 + 0.5 * c)));
        }
        return shapeGauge(shapeFrame(uv)) / farthest;
    }
    if (uField == FieldBox) {
        vec2 d = abs(uv - uAnchor) / max(uAnchor, 1.0 - uAnchor);
        return max(d.x, d.y);
    }
    if (uField == FieldTriangleSide) {
        float u = uFlag > 0.5 ? 1.0 - uv.x : uv.x;
        return (u + 2.0 * abs(uv.y - 0.5)) * 0.5;
    }
    if (uField == FieldBand) {
        // Bands across the frame (uFlag 0: rows moving sideways, 1: columns moving up / down).
        float along = uFlag > 0.5 ? uv.y : uv.x;
        float across = uFlag > 0.5 ? uv.x : uv.y;
        float band = floor(across * float(uN));
        bool odd = mod(band, 2.0) > 0.5;
        if (uFlag2 > 0.5) {
            float centred = abs(along - 0.5) * 2.0;
            return odd ? 1.0 - centred : centred;
        }
        return odd ? 1.0 - along : along;
    }
    if (uField == FieldCenterWipe) {
        vec2 dir = vec2(cos(uAngle), sin(uAngle));
        vec2 w = (uv - 0.5) * vec2(uAspect, 1.0);
        float extent = 0.5 * (abs(dir.x) * uAspect + abs(dir.y));
        return abs(dot(w, dir)) / extent;
    }
    if (uField == FieldClock) {
        float theta = atan(p.x, -p.y) - uAngle; // clockwise from straight up
        float s = fract(theta / (2.0 * PI));
        return uFlag > 0.5 ? s : 1.0 - s;
    }
    if (uField == FieldRadial) {
        vec2 d = vec2((uv.x - 0.5) * uAspect, 1.0 - uv.y); // from the bottom centre, y up
        return 1.0 - atan(d.y, d.x) / PI;
    }
    if (uField == FieldSpiral) {
        float rmax = 0.5 * length(vec2(uAspect, 1.0));
        float turn = fract(atan(p.x, -p.y) / (2.0 * PI));
        return (turn + 2.0 * length(p) / rmax) / 3.0;
    }
    if (uField == FieldVenetian) {
        vec2 dir = vec2(cos(uAngle), sin(uAngle));
        vec2 w = (uv - 0.5) * vec2(uAspect, 1.0);
        return fract(dot(w, dir) * float(uN));
    }
    // X wipe: opens along the two diagonals.
    vec2 d = abs(uv - 0.5) * 2.0;
    return abs(d.x - d.y);
}

void main()
{
    float s = clamp(arrival(vUv), 0.0, 1.0);
    s = mix(s, 1.0 - s, uReverse);
    float feather = max(uFeather, 1e-4);
    float edge = uProgress * (1.0 + uBorder + feather);
    float outgoing = smoothstep(edge - feather, edge, s);
    float incoming = 1.0 - smoothstep(edge - uBorder - feather, edge - uBorder, s);
    float border = clamp(1.0 - outgoing - incoming, 0.0, 1.0);
    fragColor = texture(uA, vUv) * outgoing + texture(uB, vUv) * incoming + uBorderColor * border;
}
)";

constexpr double kDegToRad = std::numbers::pi / 180.0;

/// Border and feather in units of s: the border is given in pixels of the output, measured
/// against the half diagonal (where s reaches 1 for the centred shapes).
void setEdge(GlKit& kit, const GlProgram& program, const TransitionInput& in)
{
    const double halfDiagonal = 0.5 * std::hypot(double(in.size.width()), double(in.size.height()));
    setFloat(kit, program, "uBorder",
        std::clamp(numberParam(in, k::Border, 0), 0.0, 200.0) / halfDiagonal);
    setFloat(
        kit, program, "uFeather", std::clamp(numberParam(in, k::Feather, 0), 0.0, 100.0) / 400.0);
    setFloat(kit, program, "uReverse", staticBool(*in.params, k::Reverse, false) ? 1.0f : 0.0f);
    setColor(kit, program, "uBorderColor", staticColor(*in.params, k::Color, QColor(Qt::white)));
}

void setCommon(GlKit& kit, const GlProgram& program, const TransitionInput& in, Field field)
{
    const double aspect = double(in.size.width()) / double(in.size.height());
    setInt(kit, program, "uField", field);
    setFloat(kit, program, "uAspect", aspect);
    setVec2(kit, program, "uCenter", numberParam(in, k::OffsetX, 0) / 100.0 * aspect,
        numberParam(in, k::OffsetY, 0) / 100.0);
    setFloat(kit, program, "uShapeAspect", std::clamp(numberParam(in, k::Aspect, 1), 0.2, 5.0));
    setFloat(kit, program, "uRot", numberParam(in, k::Rotation, 0) * kDegToRad);
    setFloat(kit, program, "uAngle", numberParam(in, k::Angle, 0) * kDegToRad);
    setVec2(kit, program, "uAnchor", 0, 0);
    setFloat(kit, program, "uFlag", 0);
    setFloat(kit, program, "uFlag2", 0);
    setInt(kit, program, "uN", 4);
    setEdge(kit, program, in);
}

/// Preset name -> clockwise rotation of the preset (degrees) for the shapes that point up.
float presetRotation(const QString& preset)
{
    if (preset == QLatin1String("down") || preset == QLatin1String("bottom"))
        return 180;
    if (preset == QLatin1String("right"))
        return 90;
    if (preset == QLatin1String("left"))
        return 270;
    return 0;
}

using Setter = std::function<void(GlKit&, const GlProgram&, const TransitionInput&)>;

std::unique_ptr<GlTransition> make(const char* id, Field field, Setter extra)
{
    return std::make_unique<ShaderTransition>(id, kShapeFamily,
        [field, extra = std::move(extra)](
            GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setCommon(kit, program, in, field);
            if (extra)
                extra(kit, program, in);
        });
}

void addRegular(std::vector<GlTransitionFactory>& out, const char* id, int sides,
    std::function<float(const QString&)> presetToRotation)
{
    out.push_back({QLatin1String(id), [=] {
                       return make(id, FieldPolygon,
                           [=](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
                               const QString preset = staticChoice(*in.params, k::Preset);
                               const float turn
                                   = presetToRotation ? presetToRotation(preset) : 0.0f;
                               setInt(kit, program, "uN", sides);
                               setFloat(kit, program, "uRot",
                                   (numberParam(in, k::Rotation, 0) + turn) * kDegToRad);
                           });
                   }});
}

void addSimple(
    std::vector<GlTransitionFactory>& out, const char* id, Field field, Setter extra = {})
{
    out.push_back({QLatin1String(id), [=] { return make(id, field, extra); }});
}

} // namespace

void appendShapeTransitions(std::vector<GlTransitionFactory>& out)
{
    // ---- Iris ----
    addRegular(out, "diamond_iris", 4, nullptr);
    addRegular(out, "square_iris", 4,
        [](const QString& p) { return p == QLatin1String("point") ? 0.0f : 45.0f; });
    addRegular(out, "hexagon_iris", 6,
        [](const QString& p) { return p == QLatin1String("hexagon_vertical") ? 0.0f : 30.0f; });
    addRegular(out, "pentagon_iris", 5,
        [](const QString& p) { return p == QLatin1String("down") ? 180.0f : 0.0f; });
    addRegular(out, "triangle_iris", 3, &presetRotation);
    addSimple(out, "oval_iris", FieldOval);
    addSimple(out, "cross_iris", FieldCross);
    addSimple(out, "eye_iris", FieldEye);
    addSimple(out, "arrow_iris", FieldArrow,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setFloat(kit, program, "uRot",
                (numberParam(in, k::Rotation, 0)
                    + presetRotation(staticChoice(*in.params, k::Preset)))
                    * kDegToRad);
        });

    // ---- Shape ----
    addSimple(out, "heart_shape", FieldHeart);
    addSimple(out, "star_shape", FieldStar,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setInt(kit, program, "uN",
                std::clamp(staticChoice(*in.params, k::Preset, QStringLiteral("5")).toInt(), 4, 6));
        });
    addSimple(out, "box_shape", FieldBox,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            static const struct
            {
                const char* mode;
                float x, y;
            } kAnchors[] = {{"upper_left", 0, 0}, {"upper_right", 1, 0}, {"lower_left", 0, 1},
                {"lower_right", 1, 1}, {"left_center", 0, 0.5f}, {"top_center", 0.5f, 0},
                {"right_center", 1, 0.5f}, {"bottom_center", 0.5f, 1}};
            const QString mode = staticChoice(*in.params, k::Mode);
            for (const auto& a : kAnchors) {
                if (mode == QLatin1String(a.mode))
                    setVec2(kit, program, "uAnchor", a.x, a.y);
            }
        });
    addSimple(out, "triangle_left_shape", FieldTriangleSide,
        [](GlKit& kit, const GlProgram& program, const TransitionInput&) {
            setFloat(kit, program, "uFlag", 1);
        });
    addSimple(out, "triangle_right_shape", FieldTriangleSide);

    // ---- Wipe ----
    addSimple(out, "band_wipe", FieldBand,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            const QString preset = staticChoice(*in.params, k::Preset);
            setFloat(kit, program, "uFlag", preset.startsWith(QLatin1String("vertical")) ? 1 : 0);
            setFloat(kit, program, "uFlag2", preset.endsWith(QLatin1String("bilinear")) ? 1 : 0);
            setInt(kit, program, "uN", int(std::clamp(numberParam(in, k::Repeat, 8), 2.0, 50.0)));
        });
    addSimple(out, "center_wipe", FieldCenterWipe);
    addSimple(out, "clock_wipe", FieldClock,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setFloat(kit, program, "uFlag", staticBool(*in.params, k::Clockwise, true) ? 1 : 0);
        });
    addSimple(out, "radial_wipe", FieldRadial);
    addSimple(out, "spiral_wipe", FieldSpiral);
    addSimple(out, "venetian_blind_wipe", FieldVenetian,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setInt(kit, program, "uN", int(std::clamp(numberParam(in, k::Repeat, 8), 1.0, 50.0)));
            setFloat(kit, program, "uAngle", numberParam(in, k::Angle, 90) * kDegToRad);
        });
    addSimple(out, "x_wipe", FieldX);
}

} // namespace kk::render
