// SPDX-License-Identifier: GPL-3.0-or-later
// DctlTranslator — see DctlTranslator.h.
#include "core/DctlTranslator.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace kk::dctl {

namespace {

enum class Kind { Ident, Number, Punct, Directive, Str };

struct Tok
{
    Kind kind = Kind::Punct;
    QString t;
    int line = 0;
    bool is(const char* s) const { return t == QLatin1String(s); }
};

using Toks = QVector<Tok>;

struct Failure
{
    int line;
    QString why;
};

[[noreturn]] void fail(int line, const QString& why)
{
    throw Failure {line, why};
}

// ---- tokenizer ----

QString stripComments(const QString& s)
{
    QString out;
    out.reserve(s.size());
    int i = 0;
    const int n = s.size();
    while (i < n) {
        if (s[i] == QLatin1Char('/') && i + 1 < n && s[i + 1] == QLatin1Char('/')) {
            while (i < n && s[i] != QLatin1Char('\n'))
                ++i;
        } else if (s[i] == QLatin1Char('/') && i + 1 < n && s[i + 1] == QLatin1Char('*')) {
            i += 2;
            while (i + 1 < n && !(s[i] == QLatin1Char('*') && s[i + 1] == QLatin1Char('/'))) {
                if (s[i] == QLatin1Char('\n'))
                    out += QLatin1Char('\n');
                ++i;
            }
            i += 2;
            out += QLatin1Char(' ');
        } else {
            out += s[i++];
        }
    }
    return out;
}

Toks tokenize(const QString& s)
{
    Toks out;
    int i = 0, line = 1;
    const int n = s.size();
    bool lineStart = true;
    static const QStringList twoChar = {QStringLiteral("&&"), QStringLiteral("||"),
        QStringLiteral("=="), QStringLiteral("!="), QStringLiteral("<="), QStringLiteral(">="),
        QStringLiteral("++"), QStringLiteral("--"), QStringLiteral("+="), QStringLiteral("-="),
        QStringLiteral("*="), QStringLiteral("/="), QStringLiteral("<<"), QStringLiteral(">>"),
        QStringLiteral("->"), QStringLiteral("%="), QStringLiteral("&="), QStringLiteral("|="),
        QStringLiteral("^=")};
    while (i < n) {
        const QChar c = s[i];
        if (c == QLatin1Char('\n')) {
            ++line;
            ++i;
            lineStart = true;
            continue;
        }
        if (c.isSpace()) {
            ++i;
            continue;
        }
        if (c == QLatin1Char('#') && lineStart) {
            QString text;
            const int startLine = line;
            while (i < n && s[i] != QLatin1Char('\n')) {
                if (s[i] == QLatin1Char('\\') && i + 1 < n && s[i + 1] == QLatin1Char('\n')) {
                    text += QLatin1Char(' ');
                    i += 2;
                    ++line;
                    continue;
                }
                text += s[i++];
            }
            out.push_back({Kind::Directive, text.trimmed(), startLine});
            continue;
        }
        lineStart = false;
        if (c.isLetter() || c == QLatin1Char('_')) {
            int j = i;
            while (j < n && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('_')))
                ++j;
            out.push_back({Kind::Ident, s.mid(i, j - i), line});
            i = j;
        } else if (c.isDigit() || (c == QLatin1Char('.') && i + 1 < n && s[i + 1].isDigit())) {
            int j = i;
            while (j < n
                && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('.')
                    || ((s[j] == QLatin1Char('+') || s[j] == QLatin1Char('-'))
                        && (s[j - 1] == QLatin1Char('e') || s[j - 1] == QLatin1Char('E'))
                        && !s.mid(i, j - i).startsWith(QLatin1String("0x")))))
                ++j;
            out.push_back({Kind::Number, s.mid(i, j - i), line});
            i = j;
        } else if (c == QLatin1Char('"')) {
            int j = i + 1;
            while (j < n && s[j] != QLatin1Char('"') && s[j] != QLatin1Char('\n'))
                ++j;
            out.push_back({Kind::Str, s.mid(i + 1, j - i - 1), line});
            i = j + 1;
        } else {
            QString two = s.mid(i, 2);
            if (twoChar.contains(two)) {
                out.push_back({Kind::Punct, two, line});
                i += 2;
            } else {
                out.push_back({Kind::Punct, QString(c), line});
                ++i;
            }
        }
    }
    return out;
}

int matching(const Toks& t, int open)
{
    const QString o = t[open].t;
    const QString c = o == QLatin1String("(") ? QStringLiteral(")")
        : o == QLatin1String("{")             ? QStringLiteral("}")
                                              : QStringLiteral("]");
    int depth = 0;
    for (int i = open; i < t.size(); ++i) {
        if (t[i].kind != Kind::Punct)
            continue;
        if (t[i].t == o)
            ++depth;
        else if (t[i].t == c && --depth == 0)
            return i;
    }
    fail(t[open].line, QStringLiteral("unbalanced '%1'").arg(o));
}

/// Splits t[from, to) at top-level commas.
QVector<Toks> splitCommas(const Toks& t, int from, int to)
{
    QVector<Toks> parts(1);
    int depth = 0;
    for (int i = from; i < to; ++i) {
        const Tok& k = t[i];
        if (k.kind == Kind::Punct) {
            if (k.t == QLatin1String("(") || k.t == QLatin1String("{") || k.t == QLatin1String("["))
                ++depth;
            else if (k.t == QLatin1String(")") || k.t == QLatin1String("}")
                || k.t == QLatin1String("]"))
                --depth;
            else if (k.t == QLatin1String(",") && depth == 0) {
                parts.push_back({});
                continue;
            }
        }
        parts.back().push_back(k);
    }
    if (parts.size() == 1 && parts[0].isEmpty())
        parts.clear();
    return parts;
}

QString joinText(const Toks& t)
{
    QStringList w;
    for (const Tok& k : t)
        w << k.t;
    return w.join(QLatin1Char(' ')).trimmed();
}

double number(const Toks& t, int line, double fallback, bool* have = nullptr)
{
    QString s;
    for (const Tok& k : t)
        if (!k.is("{") && !k.is("}"))
            s += k.t;
    s = s.trimmed();
    if (s.endsWith(QLatin1Char('f')) || s.endsWith(QLatin1Char('F')))
        s.chop(1);
    if (s.isEmpty()) {
        if (have)
            *have = false;
        return fallback;
    }
    bool ok = false;
    const double v = s.toDouble(&ok);
    if (!ok) {
        if (s == QLatin1String("true") || s == QLatin1String("false")) {
            if (have)
                *have = true;
            return s == QLatin1String("true") ? 1.0 : 0.0;
        }
        fail(line, QStringLiteral("cannot read the number '%1' in DEFINE_UI_PARAMS").arg(s));
    }
    if (have)
        *have = true;
    return v;
}

// ---- tables ----

const QSet<QString>& builtinTypes()
{
    static const QSet<QString> s = {QStringLiteral("float"), QStringLiteral("int"),
        QStringLiteral("bool"), QStringLiteral("uint"), QStringLiteral("float2"),
        QStringLiteral("float3"), QStringLiteral("float4"), QStringLiteral("int2"),
        QStringLiteral("int3"), QStringLiteral("int4"), QStringLiteral("void")};
    return s;
}

QString mapType(const QString& id)
{
    static const QHash<QString, QString> m = {{QStringLiteral("float2"), QStringLiteral("vec2")},
        {QStringLiteral("float3"), QStringLiteral("vec3")},
        {QStringLiteral("float4"), QStringLiteral("vec4")},
        {QStringLiteral("int2"), QStringLiteral("ivec2")},
        {QStringLiteral("int3"), QStringLiteral("ivec3")},
        {QStringLiteral("int4"), QStringLiteral("ivec4")},
        {QStringLiteral("double"), QStringLiteral("float")},
        {QStringLiteral("__CONSTANT__"), QStringLiteral("const")},
        {QStringLiteral("__TEXTURE__"), QStringLiteral("int")}};
    return m.value(id, id);
}

const QSet<QString>& reservedWords()
{
    static const QSet<QString> s = {QStringLiteral("input"), QStringLiteral("output"),
        QStringLiteral("in"), QStringLiteral("out"), QStringLiteral("inout"),
        QStringLiteral("sample"), QStringLiteral("smooth"), QStringLiteral("flat"),
        QStringLiteral("half"), QStringLiteral("fixed"), QStringLiteral("filter"),
        QStringLiteral("texture"), QStringLiteral("common"), QStringLiteral("partition"),
        QStringLiteral("active"), QStringLiteral("layout"), QStringLiteral("centroid"),
        QStringLiteral("patch"), QStringLiteral("precise"), QStringLiteral("coherent"),
        QStringLiteral("volatile"), QStringLiteral("restrict"), QStringLiteral("readonly"),
        QStringLiteral("writeonly"), QStringLiteral("shared"), QStringLiteral("attribute"),
        QStringLiteral("varying"), QStringLiteral("uniform"), QStringLiteral("buffer"),
        QStringLiteral("resource"), QStringLiteral("lowp"), QStringLiteral("mediump"),
        QStringLiteral("highp"), QStringLiteral("precision"), QStringLiteral("invariant"),
        QStringLiteral("noperspective"), QStringLiteral("subroutine"), QStringLiteral("namespace"),
        QStringLiteral("using"), QStringLiteral("template"), QStringLiteral("this"),
        QStringLiteral("goto"), QStringLiteral("enum"), QStringLiteral("union"),
        QStringLiteral("unsigned"), QStringLiteral("cast"), QStringLiteral("extern"),
        QStringLiteral("external"), QStringLiteral("interface"), QStringLiteral("mat2"),
        QStringLiteral("mat3"), QStringLiteral("mat4"), QStringLiteral("sampler2D"),
        QStringLiteral("discard"), QStringLiteral("main"), QStringLiteral("fragColor"),
        QStringLiteral("vUv"), QStringLiteral("straight"), QStringLiteral("luma"),
        QStringLiteral("keyOutput"), QStringLiteral("kLuma")};
    return s;
}

const QSet<QString>& unsupportedWords()
{
    static const QSet<QString> s = {QStringLiteral("printf"), QStringLiteral("__global__"),
        QStringLiteral("__kernel"), QStringLiteral("__shared__"), QStringLiteral("__local"),
        QStringLiteral("__global"), QStringLiteral("atomicAdd"), QStringLiteral("sizeof"),
        QStringLiteral("long"), QStringLiteral("short"), QStringLiteral("char"),
        QStringLiteral("malloc"), QStringLiteral("new"), QStringLiteral("delete"),
        QStringLiteral("class"), QStringLiteral("template"), QStringLiteral("__syncthreads")};
    return s;
}

// ---- GLSL support code ----

const char* const kMathPrelude = R"(
// ---- DCTL support (not part of your file) ----
float _powf(float x, float y) { return pow(x, y); }
float _fabs(float x) { return abs(x); }
float _fabsf(float x) { return abs(x); }
int _abs(int x) { return abs(x); }
float _fmaxf(float a, float b) { return max(a, b); }
float _fminf(float a, float b) { return min(a, b); }
float _sqrtf(float x) { return sqrt(x); }
float _rsqrtf(float x) { return inversesqrt(x); }
float _expf(float x) { return exp(x); }
float _exp2f(float x) { return exp2(x); }
float _exp10f(float x) { return pow(10.0, x); }
float _logf(float x) { return log(x); }
float _log2f(float x) { return log2(x); }
float _log10f(float x) { return log(x) / log(10.0); }
float _sinf(float x) { return sin(x); }
float _cosf(float x) { return cos(x); }
float _tanf(float x) { return tan(x); }
float _asinf(float x) { return asin(x); }
float _acosf(float x) { return acos(x); }
float _atanf(float x) { return atan(x); }
float _atan2f(float y, float x) { return atan(y, x); }
float _sinhf(float x) { return sinh(x); }
float _coshf(float x) { return cosh(x); }
float _tanhf(float x) { return tanh(x); }
float _floorf(float x) { return floor(x); }
float _ceilf(float x) { return ceil(x); }
float _round(float x) { return floor(x + 0.5); }
float _roundf(float x) { return floor(x + 0.5); }
float _truncf(float x) { return trunc(x); }
float _fmod(float x, float y) { return x - y * trunc(x / y); }
float _fmodf(float x, float y) { return x - y * trunc(x / y); }
float _fdividef(float x, float y) { return x / y; }
float _hypotf(float x, float y) { return sqrt(x * x + y * y); }
float _copysignf(float x, float y) { return y < 0.0 ? -abs(x) : abs(x); }
float _clampf(float x, float lo, float hi) { return clamp(x, lo, hi); }
float _saturatef(float x) { return clamp(x, 0.0, 1.0); }
float _fracf(float x) { return x - floor(x); }
float _mix(float a, float b, float t) { return a + (b - a) * t; }
float _lerp(float a, float b, float t) { return a + (b - a) * t; }
vec2 make_float2(float x, float y) { return vec2(x, y); }
vec2 make_float2(float x) { return vec2(x); }
vec3 make_float3(float x, float y, float z) { return vec3(x, y, z); }
vec3 make_float3(float x) { return vec3(x); }
vec4 make_float4(float x, float y, float z, float w) { return vec4(x, y, z, w); }
vec4 make_float4(float x) { return vec4(x); }
ivec2 make_int2(int x, int y) { return ivec2(x, y); }
ivec3 make_int3(int x, int y, int z) { return ivec3(x, y, z); }
ivec4 make_int4(int x, int y, int z, int w) { return ivec4(x, y, z, w); }
)";

const char* const kEffectPrelude = R"(
uniform sampler2D uTex;
ivec2 dctl_size = ivec2(1);
float _tex2D(int ch, int x, int y)
{
    vec4 t = texelFetch(uTex, clamp(ivec2(x, y), ivec2(0), dctl_size - 1), 0);
    if (ch == 3) return t.a;
    return t.a > 0.0 ? t[ch] / t.a : 0.0;
}
float _tex2D(int ch, float x, float y) { return _tex2D(ch, int(floor(x)), int(floor(y))); }
)";

const char* const kTransitionPrelude = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
ivec2 dctl_size = ivec2(1);
vec4 _tex2DVec4(int tex, float x, float y)
{
    vec2 uv = clamp((vec2(x, y) + 0.5) / vec2(dctl_size), 0.0, 1.0);
    return tex == 0 ? texture(uA, uv) : texture(uB, uv);
}
vec4 _tex2DVec4(int tex, int x, int y) { return _tex2DVec4(tex, float(x), float(y)); }
float _tex2D(int tex, float x, float y) { return _tex2DVec4(tex, x, y).r; }
float _tex2D(int tex, int x, int y) { return _tex2DVec4(tex, float(x), float(y)).r; }
)";

// ---- the translator ----

struct Struct
{
    QString name; ///< renamed
    QStringList memberTypes; ///< GLSL type of every member, in order
};

class Translator
{
public:
    Translator(const QString& source, Entry entry)
        : m_entry(entry)
    {
        m_toks = tokenize(stripComments(source));
    }

    Result run()
    {
        Result r;
        r.entry = m_entry;
        try {
            checkDirectives();
            readUiParams();
            collectNames();
            rewriteStructs();
            rewriteInitializers();
            rewriteCasts();
            const QString body = emitText();
            r.glsl = compose(body);
            r.params = m_params;
            r.ok = true;
        } catch (const Failure& f) {
            r.ok = false;
            r.errorLine = f.line;
            r.error = QStringLiteral("line %1: %2").arg(f.line).arg(f.why);
        }
        return r;
    }

private:
    Entry m_entry;
    Toks m_toks;
    QVector<UiParam> m_params;
    QHash<QString, QString> m_rename; ///< identifier -> GLSL identifier
    QHash<QString, Struct> m_structs; ///< by renamed name
    QString m_entryParams;
    QString m_entryReturn;

    // -- directives --

    void checkDirectives()
    {
        for (const Tok& t : m_toks) {
            if (t.kind != Kind::Directive)
                continue;
            static const QRegularExpression re(QStringLiteral("^#\\s*(\\w+)"));
            const QString d = re.match(t.t).captured(1);
            if (d == QLatin1String("include"))
                fail(t.line, QStringLiteral("#include is not supported; put everything in one file"));
            if (d == QLatin1String("pragma") || d == QLatin1String("version"))
                fail(t.line, QStringLiteral("#%1 is not supported").arg(d));
        }
    }

    // -- DEFINE_UI_PARAMS --

    void readUiParams()
    {
        Toks out;
        int numeric = 0, colors = 0;
        for (int i = 0; i < m_toks.size(); ++i) {
            const Tok& t = m_toks[i];
            if (t.kind != Kind::Ident || t.t != QLatin1String("DEFINE_UI_PARAMS")) {
                out.push_back(t);
                continue;
            }
            if (i + 1 >= m_toks.size() || !m_toks[i + 1].is("("))
                fail(t.line, QStringLiteral("DEFINE_UI_PARAMS needs a parameter list"));
            const int close = matching(m_toks, i + 1);
            const QVector<Toks> a = splitCommas(m_toks, i + 2, close);
            if (a.size() < 3)
                fail(t.line,
                    QStringLiteral("DEFINE_UI_PARAMS needs a name, a label and a type"));
            UiParam p;
            p.name = joinText(a[0]);
            p.label = joinText(a[1]);
            if (a[1].size() == 1 && a[1][0].kind == Kind::Str)
                p.label = a[1][0].t;
            const QString kind = joinText(a[2]);
            auto num = [&](int idx, double fb) {
                return idx < a.size() ? number(a[idx], t.line, fb) : fb;
            };
            if (kind == QLatin1String("DCTLUI_SLIDER_FLOAT")) {
                p.kind = UiKind::SliderFloat;
                p.def = num(3, 0);
                p.min = num(4, 0);
                p.max = num(5, 1);
                p.step = num(6, 0.01);
            } else if (kind == QLatin1String("DCTLUI_SLIDER_INT")) {
                p.kind = UiKind::SliderInt;
                p.def = num(3, 0);
                p.min = num(4, 0);
                p.max = num(5, 100);
                p.step = 1;
            } else if (kind == QLatin1String("DCTLUI_VALUE_BOX")) {
                p.kind = UiKind::ValueBox;
                p.def = num(3, 0);
                p.min = -1e6;
                p.max = 1e6;
            } else if (kind == QLatin1String("DCTLUI_CHECK_BOX")) {
                p.kind = UiKind::CheckBox;
                p.def = num(3, 0) != 0.0 ? 1.0 : 0.0;
                p.min = 0;
                p.max = 1;
            } else if (kind == QLatin1String("DCTLUI_COMBO_BOX")) {
                p.kind = UiKind::ComboBox;
                p.def = num(3, 0);
                if (a.size() > 5) {
                    for (const Toks& part : splitCommas(a[5], 0, int(a[5].size()))) {
                        Toks inner = part;
                        inner.erase(std::remove_if(inner.begin(), inner.end(),
                                        [](const Tok& k) { return k.is("{") || k.is("}"); }),
                            inner.end());
                        p.options << joinText(inner);
                    }
                    // The labels of a combo box sit inside one pair of braces: re-split them.
                    Toks flat = a[5];
                    if (!flat.isEmpty() && flat.first().is("{") && flat.last().is("}")) {
                        p.options.clear();
                        for (const Toks& part :
                            splitCommas(flat, 1, int(flat.size()) - 1))
                            p.options << (part.size() == 1 && part[0].kind == Kind::Str
                                    ? part[0].t
                                    : joinText(part));
                    }
                }
                if (p.options.isEmpty() && a.size() > 4) {
                    // {values}, {labels}: the labels are the second group; a lone group is both.
                    Toks flat = a[4];
                    if (!flat.isEmpty() && flat.first().is("{"))
                        for (const Toks& part : splitCommas(flat, 1, int(flat.size()) - 1))
                            p.options << joinText(part);
                }
                p.min = 0;
                p.max = std::max(0, int(p.options.size()) - 1);
            } else if (kind == QLatin1String("DCTLUI_COLOR_PICKER")) {
                p.kind = UiKind::ColorPicker;
                QVector<double> v;
                for (int k = 3; k < a.size(); ++k) {
                    Toks flat = a[k];
                    for (const Toks& part : splitCommas(flat, 0, int(flat.size())))
                        v << number(part, t.line, 1.0);
                }
                for (int k = 0; k < 3; ++k)
                    p.color[k] = k < v.size() ? float(v[k]) : 1.f;
            } else {
                fail(t.line, QStringLiteral("unknown DEFINE_UI_PARAMS type '%1'").arg(kind));
            }
            if (p.kind == UiKind::ColorPicker) {
                if (colors >= kMaxColorParams)
                    fail(t.line,
                        QStringLiteral("more than %1 colour pickers are not supported")
                            .arg(kMaxColorParams));
                p.slot = colors++;
            } else {
                if (numeric >= kMaxNumericParams)
                    fail(t.line,
                        QStringLiteral("more than %1 sliders, boxes and check boxes are not "
                                       "supported")
                            .arg(kMaxNumericParams));
                p.slot = numeric++;
            }
            p.uniform = QStringLiteral("dctl_u_") + p.name;
            m_rename.insert(p.name, p.uniform);
            m_params.push_back(p);
            i = close;
            if (i + 1 < m_toks.size() && m_toks[i + 1].is(";"))
                ++i;
        }
        m_toks = out;
    }

    // -- user names --

    void collectNames()
    {
        int depth = 0;
        for (int i = 0; i < m_toks.size(); ++i) {
            const Tok& t = m_toks[i];
            if (t.kind == Kind::Punct) {
                if (t.is("{"))
                    ++depth;
                else if (t.is("}"))
                    --depth;
                continue;
            }
            if (t.kind != Kind::Ident)
                continue;
            if (unsupportedWords().contains(t.t))
                fail(t.line, QStringLiteral("'%1' is not supported").arg(t.t));
            if (depth == 0 && i + 1 < m_toks.size() && m_toks[i + 1].is("(") && i > 0
                && m_toks[i - 1].kind == Kind::Ident && !m_toks[i - 1].is("return")
                && !m_toks[i - 1].is("else")) {
                // a function definition or declaration: type name(
                const QString name = t.t;
                if (name == QLatin1String("DEFINE_UI_PARAMS"))
                    continue;
                if (name == QLatin1String("transform") || name == QLatin1String("transition")) {
                    const int close = matching(m_toks, i + 1);
                    const bool isMe = (name == QLatin1String("transform"))
                        == (m_entry == Entry::Transform);
                    if (isMe) {
                        m_entryParams = joinText(m_toks.mid(i + 2, close - i - 2));
                        m_entryReturn = m_toks[i - 1].t;
                        m_entryLine = t.line;
                    }
                }
                m_rename.insert(name, QStringLiteral("dctl_f_") + name);
            }
        }
        if (m_entryParams.isEmpty() && m_entryReturn.isEmpty())
            fail(1,
                m_entry == Entry::Transform
                    ? QStringLiteral("the file has no transform() function")
                    : QStringLiteral("the file has no transition() function"));
    }
    int m_entryLine = 1;

    // -- typedef struct --

    void rewriteStructs()
    {
        Toks out;
        for (int i = 0; i < m_toks.size(); ++i) {
            const Tok& t = m_toks[i];
            const bool isTypedef = t.kind == Kind::Ident && t.is("typedef");
            const bool isStruct = t.kind == Kind::Ident && t.is("struct");
            if (isTypedef && i + 1 < m_toks.size() && !m_toks[i + 1].is("struct"))
                fail(t.line, QStringLiteral("typedef is only supported for structs"));
            if (!isTypedef && !isStruct) {
                out.push_back(t);
                continue;
            }
            int s = isTypedef ? i + 1 : i; // index of 'struct'
            int open = s + 1;
            QString tag;
            if (open < m_toks.size() && m_toks[open].kind == Kind::Ident) {
                tag = m_toks[open].t;
                ++open;
            }
            if (open >= m_toks.size() || !m_toks[open].is("{")) {
                // "struct Name var" (a use, not a definition): drop the keyword
                if (isStruct) {
                    continue;
                }
                fail(t.line, QStringLiteral("unsupported struct declaration"));
            }
            const int close = matching(m_toks, open);
            QString alias;
            int end = close;
            if (isTypedef) {
                if (close + 1 >= m_toks.size() || m_toks[close + 1].kind != Kind::Ident)
                    fail(t.line, QStringLiteral("typedef struct needs a name"));
                alias = m_toks[close + 1].t;
                end = close + 1;
            }
            const QString base = alias.isEmpty() ? tag : alias;
            if (base.isEmpty())
                fail(t.line, QStringLiteral("anonymous structs are not supported"));
            const QString glsl = QStringLiteral("dctl_t_") + base;
            if (!tag.isEmpty())
                m_rename.insert(tag, glsl);
            m_rename.insert(base, glsl);
            Struct st;
            st.name = glsl;
            // members: "type a, b;"
            Toks body = m_toks.mid(open + 1, close - open - 1);
            QVector<Toks> decls;
            decls.push_back({});
            for (const Tok& b : body) {
                if (b.is(";"))
                    decls.push_back({});
                else
                    decls.back().push_back(b);
            }
            for (const Toks& d : decls) {
                if (d.isEmpty())
                    continue;
                const QString type = mapType(d[0].t);
                const QString renamed = m_rename.value(d[0].t, type);
                int names = 1;
                for (int k = 1; k < d.size(); ++k)
                    if (d[k].is(","))
                        ++names;
                    else if (d[k].is("["))
                        fail(d[k].line,
                            QStringLiteral("arrays inside a struct are not supported"));
                for (int k = 0; k < names; ++k)
                    st.memberTypes << renamed;
            }
            m_structs.insert(glsl, st);
            out.push_back({Kind::Ident, QStringLiteral("struct"), t.line});
            out.push_back({Kind::Ident, base, t.line});
            for (int k = open; k <= close; ++k)
                out.push_back(m_toks[k]);
            out.push_back({Kind::Punct, QStringLiteral(";"), t.line});
            i = end;
            if (!isTypedef && close + 1 < m_toks.size() && m_toks[close + 1].is(";"))
                i = close + 1;
            else if (isTypedef && end + 1 < m_toks.size() && m_toks[end + 1].is(";"))
                i = end + 1;
        }
        m_toks = out;
    }

    // -- = { ... } --

    QString glslName(const QString& id) const
    {
        const QString r = m_rename.value(id);
        if (!r.isEmpty())
            return r;
        return mapType(id);
    }

    bool isTypeName(const QString& id) const
    {
        return builtinTypes().contains(id) || m_structs.contains(m_rename.value(id));
    }

    Toks convertBraces(const Toks& t, int open, int close, const QString& type, bool isArray,
        int declaredCount)
    {
        const QVector<Toks> parts = splitCommas(t, open + 1, close);
        Toks out;
        const int line = t[open].line;
        QString elemType = type;
        const bool structInit = !isArray && m_structs.contains(type);
        QVector<Toks> use = parts;
        if (!use.isEmpty() && use.last().isEmpty())
            use.removeLast(); // trailing comma
        if (isArray) {
            const int count = declaredCount > 0 ? declaredCount : int(use.size());
            out.push_back({Kind::Ident, QStringLiteral("%1[%2]").arg(type).arg(count), line});
        } else if (structInit) {
            out.push_back({Kind::Ident, type, line});
        } else if (use.size() > 1 || type.startsWith(QLatin1String("vec"))
            || type.startsWith(QLatin1String("ivec"))) {
            out.push_back({Kind::Ident, type, line});
        } else {
            // a scalar in braces
            out.push_back({Kind::Punct, QStringLiteral("("), line});
            for (const Tok& k : use.value(0))
                out.push_back(k);
            out.push_back({Kind::Punct, QStringLiteral(")"), line});
            return out;
        }
        out.push_back({Kind::Punct, QStringLiteral("("), line});
        for (int n = 0; n < use.size(); ++n) {
            if (n)
                out.push_back({Kind::Punct, QStringLiteral(","), line});
            const Toks& part = use[n];
            if (!part.isEmpty() && part[0].is("{")) {
                QString inner = elemType;
                if (structInit) {
                    const Struct& st = m_structs[type];
                    if (n >= st.memberTypes.size())
                        fail(line, QStringLiteral("too many values in the initializer"));
                    inner = st.memberTypes[n];
                }
                const int c = matching(part, 0);
                const bool innerStruct = m_structs.contains(inner);
                for (const Tok& k : convertBraces(part, 0, c, innerStruct ? inner : mapType(inner),
                         false, 0))
                    out.push_back(k);
            } else {
                for (const Tok& k : part)
                    out.push_back(k);
            }
        }
        out.push_back({Kind::Punct, QStringLiteral(")"), line});
        return out;
    }

    void rewriteInitializers()
    {
        for (int i = 1; i + 1 < m_toks.size(); ++i) {
            // a multi-dimensional array
            if (m_toks[i].is("]") && m_toks[i + 1].is("[")) {
                fail(m_toks[i].line,
                    QStringLiteral("multi-dimensional arrays are not supported; use a struct "
                                   "of float3 or a flat array"));
            }
        }
        for (int i = 0; i + 1 < m_toks.size(); ++i) {
            if (!(m_toks[i].is("=") && m_toks[i + 1].is("{")))
                continue;
            // find the declaration: Type name [ N ] = {   or   Type name = {
            int j = i - 1;
            int count = 0;
            bool isArray = false;
            if (j >= 0 && m_toks[j].is("]")) {
                isArray = true;
                int k = j;
                while (k >= 0 && !m_toks[k].is("["))
                    --k;
                if (k < 0)
                    continue;
                if (k + 1 < j && m_toks[k + 1].kind == Kind::Number)
                    count = m_toks[k + 1].t.toInt();
                j = k - 1;
            }
            if (j < 1 || m_toks[j].kind != Kind::Ident)
                continue;
            const QString typeTok = m_toks[j - 1].t;
            if (!isTypeName(typeTok))
                continue;
            const QString type = m_structs.contains(m_rename.value(typeTok))
                ? m_rename.value(typeTok)
                : mapType(typeTok);
            const int close = matching(m_toks, i + 1);
            const Toks repl = convertBraces(m_toks, i + 1, close, type, isArray, count);
            Toks out = m_toks.mid(0, i + 1);
            out += repl;
            out += m_toks.mid(close + 1);
            m_toks = out;
        }
    }

    // -- casts --

    void rewriteCasts()
    {
        for (int i = int(m_toks.size()) - 3; i >= 0; --i) {
            if (!(m_toks[i].is("(") && m_toks[i + 1].kind == Kind::Ident && m_toks[i + 2].is(")")))
                continue;
            const QString ty = m_toks[i + 1].t;
            if (ty != QLatin1String("float") && ty != QLatin1String("int")
                && ty != QLatin1String("bool") && ty != QLatin1String("uint")
                && ty != QLatin1String("double"))
                continue;
            // not a cast when the previous token makes this a call or a declaration "f(float)"
            if (i > 0 && m_toks[i - 1].kind == Kind::Ident && !m_toks[i - 1].is("return"))
                continue;
            int j = i + 3;
            if (j >= m_toks.size())
                continue;
            // the operand: unary prefixes, a primary and its postfix chain
            int k = j;
            while (k < m_toks.size()
                && (m_toks[k].is("-") || m_toks[k].is("+") || m_toks[k].is("!")
                    || m_toks[k].is("~")))
                ++k;
            if (k >= m_toks.size())
                continue;
            if (m_toks[k].is("(")) {
                k = matching(m_toks, k) + 1;
            } else if (m_toks[k].kind == Kind::Ident || m_toks[k].kind == Kind::Number) {
                ++k;
            } else {
                continue;
            }
            for (;;) {
                if (k < m_toks.size() && (m_toks[k].is("(") || m_toks[k].is("[")))
                    k = matching(m_toks, k) + 1;
                else if (k + 1 < m_toks.size() && m_toks[k].is(".")
                    && m_toks[k + 1].kind == Kind::Ident)
                    k += 2;
                else
                    break;
            }
            Toks out = m_toks.mid(0, i);
            out.push_back({Kind::Ident, ty == QLatin1String("double") ? QStringLiteral("float") : ty,
                m_toks[i].line});
            out.push_back({Kind::Punct, QStringLiteral("("), m_toks[i].line});
            out += m_toks.mid(j, k - j);
            out.push_back({Kind::Punct, QStringLiteral(")"), m_toks[i].line});
            out += m_toks.mid(k);
            m_toks = out;
        }
    }

    // -- text --

    QString identText(const Tok& t, const Toks& all, int i) const
    {
        const QString& id = t.t;
        if (i > 0 && all[i - 1].is(".")) // member access: x, y, z, r, g, b, user members
            return id;
        if (id == QLatin1String("__DEVICE__") || id == QLatin1String("static")
            || id == QLatin1String("inline") || id == QLatin1String("__inline__")
            || id == QLatin1String("extern"))
            return {};
        if (id == QLatin1String("unsigned"))
            return QStringLiteral("uint");
        const QString r = m_rename.value(id);
        if (!r.isEmpty())
            return r;
        const QString mapped = mapType(id);
        if (mapped != id)
            return mapped;
        if (reservedWords().contains(id))
            return QStringLiteral("dctl_r_") + id;
        return id;
    }

    QString emitText() const
    {
        QString out;
        int line = 1;
        for (int i = 0; i < m_toks.size(); ++i) {
            const Tok& t = m_toks[i];
            while (line < t.line) {
                out += QLatin1Char('\n');
                ++line;
            }
            switch (t.kind) {
            case Kind::Directive: {
                // identifiers of #define bodies follow the same renames; the directive word and
                // the name of a macro stay as they are
                const QString d = t.t;
                static const QRegularExpression idRe(QStringLiteral("[A-Za-z_]\\w*"));
                static const QRegularExpression suffixRe(QStringLiteral("(\\d)[fF]\\b"));
                const bool isDefine
                    = d.contains(QRegularExpression(QStringLiteral("^#\\s*define\\b")));
                QString res;
                int pos = 0, index = 0;
                auto it = idRe.globalMatch(d);
                while (it.hasNext()) {
                    const auto m = it.next();
                    res += d.mid(pos, m.capturedStart() - pos);
                    const QString id = m.captured();
                    if (index == 0 || (isDefine && index == 1)) {
                        res += id;
                    } else {
                        const Tok fake {Kind::Ident, id, t.line};
                        res += identText(fake, Toks {fake}, 0);
                    }
                    ++index;
                    pos = m.capturedEnd();
                }
                res += d.mid(pos);
                res.replace(suffixRe, QStringLiteral("\\1"));
                out += QLatin1Char('\n') + res + QLatin1Char('\n');
                ++line;
                break;
            }
            case Kind::Number: {
                QString n = t.t;
                if (!n.startsWith(QLatin1String("0x")) && !n.startsWith(QLatin1String("0X"))
                    && (n.endsWith(QLatin1Char('f')) || n.endsWith(QLatin1Char('F'))))
                    n.chop(1);
                out += n + QLatin1Char(' ');
                break;
            }
            case Kind::Str:
                fail(t.line, QStringLiteral("text strings are not supported"));
            case Kind::Punct:
                if (t.is("->"))
                    fail(t.line, QStringLiteral("pointers are not supported"));
                if (t.is("&") && i > 0
                    && (m_toks[i - 1].is(",") || m_toks[i - 1].is("(") || m_toks[i - 1].is("=")))
                    fail(t.line, QStringLiteral("pointers and references are not supported"));
                if (t.is("*") && i > 0 && i + 1 < m_toks.size()
                    && m_toks[i - 1].kind == Kind::Ident && isTypeName(m_toks[i - 1].t)
                    && m_toks[i + 1].kind == Kind::Ident)
                    fail(t.line, QStringLiteral("pointers are not supported"));
                out += t.t + QLatin1Char(' ');
                break;
            case Kind::Ident:
                if (t.is("unsigned") && i + 1 < m_toks.size() && m_toks[i + 1].is("int"))
                    ++i; // "unsigned int" is "uint"
                out += identText(t, m_toks, i) + QLatin1Char(' ');
                break;
            }
        }
        return out;
    }

    // -- wrapper --

    QString uniformDecls() const
    {
        QString s;
        for (const UiParam& p : m_params) {
            switch (p.kind) {
            case UiKind::SliderInt:
            case UiKind::ComboBox:
                s += QStringLiteral("uniform int %1;\n").arg(p.uniform);
                break;
            case UiKind::CheckBox:
                s += QStringLiteral("uniform bool %1;\n").arg(p.uniform);
                break;
            case UiKind::ColorPicker:
                s += QStringLiteral("uniform vec3 %1;\n").arg(p.uniform);
                break;
            default:
                s += QStringLiteral("uniform float %1;\n").arg(p.uniform);
            }
        }
        return s;
    }

    QString entryCall() const
    {
        QStringList args;
        const QVector<Toks> parts
            = splitCommas(tokenize(m_entryParams), 0, int(tokenize(m_entryParams).size()));
        for (const Toks& p : parts) {
            if (p.isEmpty())
                continue;
            const QString name = p.last().t;
            if (name == QLatin1String("p_Width"))
                args << QStringLiteral("dctl_size.x");
            else if (name == QLatin1String("p_Height"))
                args << QStringLiteral("dctl_size.y");
            else if (name == QLatin1String("p_X"))
                args << QStringLiteral("dctl_p.x");
            else if (name == QLatin1String("p_Y"))
                args << QStringLiteral("dctl_p.y");
            else if (name == QLatin1String("p_R"))
                args << QStringLiteral("dctl_c.r");
            else if (name == QLatin1String("p_G"))
                args << QStringLiteral("dctl_c.g");
            else if (name == QLatin1String("p_B"))
                args << QStringLiteral("dctl_c.b");
            else if (name == QLatin1String("p_A"))
                args << QStringLiteral("dctl_a");
            else if (name == QLatin1String("p_TexR") || name == QLatin1String("p_TexFrom"))
                args << QStringLiteral("0");
            else if (name == QLatin1String("p_TexG") || name == QLatin1String("p_TexTo"))
                args << QStringLiteral("1");
            else if (name == QLatin1String("p_TexB"))
                args << QStringLiteral("2");
            else if (name == QLatin1String("p_TexA"))
                args << QStringLiteral("3");
            else if (name == QLatin1String("p_Progress") || name == QLatin1String("p_TransitionProgress")
                || name == QLatin1String("p_TransitionAmount"))
                args << QStringLiteral("uProgress");
            else
                fail(m_entryLine,
                    QStringLiteral("the parameter '%1' of the %2() function is not known")
                        .arg(name, m_entry == Entry::Transform ? QStringLiteral("transform")
                                                               : QStringLiteral("transition")));
        }
        const QString fn = QStringLiteral("dctl_f_")
            + (m_entry == Entry::Transform ? QStringLiteral("transform")
                                           : QStringLiteral("transition"));
        return fn + QLatin1Char('(') + args.join(QStringLiteral(", ")) + QLatin1Char(')');
    }

    QString compose(const QString& body) const
    {
        QString s;
        s += QString::fromLatin1(kMathPrelude);
        s += m_entry == Entry::Transform ? QString::fromLatin1(kEffectPrelude)
                                         : QString::fromLatin1(kTransitionPrelude);
        s += uniformDecls();
        s += QStringLiteral("// ---- your file, translated ----\n");
        s += body;
        s += QStringLiteral("\n// ---- end of your file ----\nvoid main()\n{\n");
        const bool four = m_entryReturn == QLatin1String("float4");
        if (m_entry == Entry::Transform) {
            s += QStringLiteral(
                "    dctl_size = textureSize(uTex, 0);\n"
                "    vec4 dctl_t = texture(uTex, vUv);\n"
                "    if (dctl_t.a <= 0.0) { fragColor = vec4(0.0); return; }\n"
                "    vec3 dctl_c = straight(dctl_t);\n"
                "    float dctl_a = dctl_t.a;\n"
                "    ivec2 dctl_p = ivec2(floor(vUv * vec2(dctl_size)));\n");
            if (four)
                s += QStringLiteral("    vec4 r = %1;\n"
                                    "    float a = clamp(r.w, 0.0, 1.0);\n"
                                    "    fragColor = vec4(clamp(r.xyz, vec3(0.0), vec3(60000.0)) "
                                    "* a, a);\n")
                         .arg(entryCall());
            else
                s += QStringLiteral("    vec3 r = %1;\n"
                                    "    fragColor = vec4(clamp(r, vec3(0.0), vec3(60000.0)) * "
                                    "dctl_a, dctl_a);\n")
                         .arg(entryCall());
        } else {
            s += QStringLiteral(
                "    dctl_size = textureSize(uA, 0);\n"
                "    ivec2 dctl_p = ivec2(floor(vUv * vec2(dctl_size)));\n"
                "    vec4 r = %1;\n"
                "    fragColor = clamp(r, vec4(0.0), vec4(60000.0));\n")
                         .arg(four ? entryCall() : QStringLiteral("vec4(%1, 1.0)").arg(entryCall()));
        }
        s += QStringLiteral("}\n");
        return s;
    }
};

} // namespace

Result translate(const QString& source, Entry entry)
{
    return Translator(source, entry).run();
}

Result translateFile(const QString& path, Entry entry)
{
    QFile f(path);
    Result r;
    r.entry = entry;
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        r.error = path.isEmpty() ? QStringLiteral("no DCTL file chosen")
                                 : QStringLiteral("cannot read %1").arg(path);
        return r;
    }
    return translate(QString::fromUtf8(f.readAll()), entry);
}

} // namespace kk::dctl
