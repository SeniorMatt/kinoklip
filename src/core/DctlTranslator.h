// SPDX-License-Identifier: GPL-3.0-or-later
// DctlTranslator — turns a DaVinci CTL file (.dctl, the C-like colour language of Resolve) into
// a GLSL fragment shader for the DCTL effect and the DCTL transition (docs/behavior-spec.md
// section 205). Pure text work, no OpenGL: the translator tokenises the file, reads the
// DEFINE_UI_PARAMS declarations, rewrites the C constructs GLSL does not have (casts, typedef
// struct, array initialisers, literal suffixes, names GLSL reserves) and wraps the file's
// transform() or transition() in a main(). What it cannot translate is reported with the line
// and the reason instead of producing a shader that fails later.
//
// Supported: the types float, int, bool, float2/3/4, int2/3/4; the math functions of the
// DCTL reference (_powf, _fabs, _fmaxf, _sqrtf, _mix, _clampf, ...); make_float2/3/4; typedef
// struct and struct; helper functions with __DEVICE__; __CONSTANT__ and #define constants;
// one-dimensional arrays; if / for / while / switch; DEFINE_UI_PARAMS with sliders, value
// boxes, check boxes, combo boxes and colour pickers. Not supported (clear error): pointers
// and references, multi-dimensional arrays, #include, double / long, atomics, printf,
// CUDA / OpenCL / Metal specific code.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace kk::dctl {

enum class Entry {
    Transform, ///< transform(): colour in, colour out
    Transition ///< transition(): two pictures and a progress in, one picture out
};

enum class UiKind { SliderFloat, SliderInt, ValueBox, CheckBox, ComboBox, ColorPicker };

/// One DEFINE_UI_PARAMS of the file.
struct UiParam
{
    QString name; ///< the variable of the file
    QString label; ///< the label of the file
    UiKind kind = UiKind::SliderFloat;
    double def = 0, min = 0, max = 1, step = 0.01;
    QStringList options; ///< combo box labels
    float color[3] {1, 1, 1}; ///< colour picker default
    /// Index among the numeric parameters (all kinds but the colour picker) or among the colour
    /// pickers; the effect's "Parameter N" / "Color Parameter N".
    int slot = 0;
    QString uniform; ///< the GLSL uniform that carries the value
};

struct Result
{
    bool ok = false;
    QString error; ///< "line N: reason" when !ok
    int errorLine = 0;
    Entry entry = Entry::Transform;
    QVector<UiParam> params;
    /// The fragment shader after the common header of the renderer (GlEffectBase kGlslCommon
    /// for an effect, nothing for a transition). Uses uTex (effect) or uA / uB / uProgress
    /// (transition) and declares the uniforms of @c params.
    QString glsl;
};

inline constexpr int kMaxNumericParams = 8;
inline constexpr int kMaxColorParams = 2;

/// Translates @p source; @p entry is the function the effect needs (transform / transition).
[[nodiscard]] Result translate(const QString& source, Entry entry);

/// Reads the file at @p path (UTF-8) and translates it.
[[nodiscard]] Result translateFile(const QString& path, Entry entry);

} // namespace kk::dctl
