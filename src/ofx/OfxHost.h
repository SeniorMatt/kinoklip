// SPDX-License-Identifier: GPL-3.0-or-later
// OfxHost — Kinoklip's OpenFX (https://openfx.readthedocs.io) host for image effect plugins:
// loads *.ofx.bundle binaries, describes their plugins (parameters, clips, contexts) and renders
// them on the CPU. Built into its own small library (kk_ofx, Qt Core only) so the probe
// program (kinoklip-ofx-probe) can load a plugin in a throw-away process: a plugin that crashes
// while loading or describing must never take the editor down (spec 158).
// This file knows nothing of Kinoklip's project types; src/plugins/OfxRegistry.* connects it.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

namespace kk::ofx {

/// One parameter of an effect as the plugin described it.
struct ParamInfo
{
    QString name; ///< the OFX parameter name (unique in the effect)
    QString label;
    QString hint;
    QString parent; ///< name of the group the parameter is in, or empty
    /// OFX type without the "OfxParamType" prefix: Integer, Double, Boolean, Choice, StrChoice,
    /// RGBA, RGB, Double2D, Integer2D, Double3D, Integer3D, String, Custom, PushButton, Group,
    /// Page.
    QString type;
    int dims = 1; ///< number of values (RGBA 4, Double2D 2, ...)
    QVector<double> def, min, max, displayMin, displayMax;
    QString defString;
    QStringList choices, choiceLabels;
    double increment = 1.0;
    int digits = 2;
    QString doubleType; ///< Plain, Scale, Angle, Time, AbsoluteTime, X, Y, XAbsolute, ... XYAbsolute
    QString coordinates; ///< "Canonical" or "Normalised" for the spatial doubles, else empty
    QString stringMode;
    QStringList dimLabels;
    bool animates = false;
    bool secret = false;
    bool enabled = true;
    bool evaluateOnChange = true;
    bool persistent = true;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static ParamInfo fromJson(const QJsonObject& o);
    /// True when the host can show and store the parameter (not a group, page, push button, custom).
    [[nodiscard]] bool isValueParam() const;
};

struct ClipInfo
{
    QString name; ///< "Source", "Output", "Mask", ...
    bool optional = false;
    bool mask = false;
    bool temporal = false;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static ClipInfo fromJson(const QJsonObject& o);
};

/// What describeInContext() of one context (Filter, General, ...) told.
struct ContextInfo
{
    QString context; ///< "Filter", "General", "Transition", ...
    QVector<ClipInfo> clips;
    QVector<ParamInfo> params;
    /// True when Kinoklip can run the plugin in this context (a Filter, or a General effect whose
    /// only mandatory input is Source).
    bool usable = false;
    QString whyNot;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static ContextInfo fromJson(const QJsonObject& o);
};

struct PluginInfo
{
    QString identifier; ///< "net.sf.openfx.GainPlugin"
    QString label, shortLabel, group, description, versionLabel;
    int index = 0; ///< position in the bundle
    int major = 1, minor = 0;
    QStringList pixelDepths; ///< "Float", "Byte", "Short", "Half"
    QString threadSafety; ///< "Unsafe", "InstanceSafe" or "FullySafe"
    bool multiResolution = false;
    bool tiles = false;
    bool temporalAccess = false;
    QVector<ContextInfo> contexts;
    QString error; ///< why describing failed (the plugin is not usable)

    /// The context Kinoklip runs the plugin in: Filter first, then General, then Transition;
    /// null when none.
    [[nodiscard]] const ContextInfo* usableContext() const;
    /// True when that context is the Transition context (the plugin is then a transition of the
    /// Effects library, not an effect).
    [[nodiscard]] bool isTransition() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static PluginInfo fromJson(const QJsonObject& o);
};

struct BundleInfo
{
    QString bundlePath; ///< the *.ofx.bundle folder
    QString binary; ///< Contents/<arch>/<name>.ofx
    QVector<PluginInfo> plugins;
    QString error; ///< the binary could not be loaded
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static BundleInfo fromJson(const QJsonObject& o);
};

/// The binary of bundle folder @p bundleDir for this machine (Contents/Linux-x86-64/x.ofx, or the
/// ARM folder), empty if the bundle has none.
[[nodiscard]] QString bundleBinary(const QString& bundleDir);

/// Loads the binary of @p bundleDir (stays loaded: plugins do not like to be unloaded) and
/// describes every image effect plugin in it. Never throws; problems are in the result.
[[nodiscard]] BundleInfo describeBundle(const QString& bundleDir);

/// The value of one parameter: numbers (a choice is its index, a boolean 0 / 1) or text.
struct ParamValue
{
    QVector<double> v;
    QString s;
};

/// What the plugin gets for one render.
struct RenderArgs
{
    double time = 0.0; ///< the frame (clip-local), OFX time
    double fps = 25.0;
    int width = 0, height = 0; ///< size of the buffers in pixels (also the render window)
    double scale = 1.0; ///< render scale: buffer pixels per canonical pixel
    /// Position of the buffer's bottom-left corner in canonical coordinates (negative when the
    /// buffer has a margin around the clip).
    double originX = 0.0, originY = 0.0;
    /// The project / clip size in canonical coordinates (the extent without the margin).
    double projectWidth = 0.0, projectHeight = 0.0;
    bool interactive = true; ///< a preview render (false: an export)
    double duration = 1.0e6; ///< frames of the effect, for kOfxImageEffectInstancePropEffectDuration
};

enum class RenderResult {
    Rendered, ///< dst holds the result
    Identity, ///< the plugin changes nothing with these values: dst is not written
    Failed ///< see error
};

/// A plugin instance (one effect on one clip). Methods may be used from any thread, but one
/// instance renders one frame at a time; the plugin's thread-safety flag decides how many of
/// its instances render at once.
class Instance
{
public:
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    /// Creates an instance of plugin @p pluginIndex of @p bundle in its usable context. Loads the
    /// binary (again) in this process. Null with @p error on failure.
    [[nodiscard]] static std::unique_ptr<Instance> create(
        const BundleInfo& bundle, int pluginIndex, QString* error);

    [[nodiscard]] const PluginInfo& info() const;

    /// Supplies the value of an animated parameter at a time (called by the plugin through
    /// paramGetValueAtTime, from any thread); return false to use the stored value.
    using Evaluator = std::function<bool(const QString& name, double time, ParamValue* out)>;
    void setEvaluator(Evaluator evaluator);
    /// Stores the value of a parameter; tells the plugin (changedParam) when it differs from what
    /// it had, so plugins that derive state from parameters see it. Returns false for an unknown name.
    bool setValue(const QString& name, const ParamValue& value);
    /// The value last stored (the default after creation).
    [[nodiscard]] ParamValue value(const QString& name) const;

    /// Renders @p src (RGBA float, premultiplied, rows bottom to top, width x height) into
    /// @p dst (same layout). @p src may be null when the plugin takes no input.
    /// In the Transition context @p src is the outgoing picture (SourceFrom) and @p srcTo the
    /// incoming one (SourceTo); the plugin's "Transition" parameter is the progress.
    [[nodiscard]] RenderResult render(const RenderArgs& args, const float* src, float* dst,
        QString* error, const float* srcTo = nullptr);

    /// The region of definition the plugin gives its output for a source of @p width x @p height
    /// canonical pixels at its origin (OFX getRegionOfDefinition), as x1, y1, x2, y2 in @p out.
    /// False when the plugin has no opinion (the output is the source's rectangle). Effects that
    /// grow the picture (blurs, glows) return a larger rectangle.
    [[nodiscard]] bool regionOfDefinition(
        double time, double fps, double width, double height, double out[4]);

    /// The last message the plugin showed (message suite), for the Plugins page.
    [[nodiscard]] QString lastMessage() const;

    struct Impl; ///< private state (public only so the host's C callbacks can reach it)

private:
    Instance();
    std::unique_ptr<Impl> d;
};

/// Number of CPU threads the host offers to plugins (multi-thread suite).
[[nodiscard]] int hostThreadCount();

} // namespace kk::ofx
