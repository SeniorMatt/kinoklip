// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimKeymap.h"

#include "ui/common/ActionRegistry.h"
#include "ui/common/VimKeys.h"
#include "ui/common/VimOptions.h"

#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <map>

namespace kk {

namespace {

const QStringList kContexts {QStringLiteral("global"), QStringLiteral("timeline"),
    QStringLiteral("viewer"), QStringLiteral("list"), QStringLiteral("panel"),
    QStringLiteral("form"), QStringLiteral("keyframes"), QStringLiteral("nodes"),
    QStringLiteral("mixer")};

/// The built-in commands of `vim:` items.
const QStringList kBuiltins {// motions (VimGrammar.h) and marks
    QStringLiteral("frame_prev"), QStringLiteral("frame_next"), QStringLiteral("second_prev"),
    QStringLiteral("second_next"), QStringLiteral("timeline_start"), QStringLiteral("timeline_end"),
    QStringLiteral("goto_frame"), QStringLiteral("first_clip"), QStringLiteral("clip_next"),
    QStringLiteral("clip_prev"), QStringLiteral("clip_end"), QStringLiteral("clip_end_prev"),
    QStringLiteral("edit_next"), QStringLiteral("edit_prev"), QStringLiteral("edit_end"),
    QStringLiteral("edit_end_prev"), QStringLiteral("marker_next"), QStringLiteral("marker_prev"),
    QStringLiteral("keyframe_next"), QStringLiteral("keyframe_prev"), QStringLiteral("track_down"),
    QStringLiteral("track_up"), QStringLiteral("track_first"), QStringLiteral("track_last"),
    QStringLiteral("set_mark"), QStringLiteral("goto_mark"), QStringLiteral("jump_back"),
    // text objects
    QStringLiteral("obj_iw"), QStringLiteral("obj_aw"), QStringLiteral("obj_iW"),
    QStringLiteral("obj_aW"), QStringLiteral("obj_ip"), QStringLiteral("obj_ap"),
    QStringLiteral("obj_im"), QStringLiteral("obj_ir"), QStringLiteral("obj_it"),
    // operators and the commands built on them
    QStringLiteral("op_delete"), QStringLiteral("op_change"), QStringLiteral("op_yank"),
    QStringLiteral("op_lift"), QStringLiteral("cmd_delete"), QStringLiteral("cmd_lift"),
    QStringLiteral("cmd_yank"), QStringLiteral("cmd_delete_to_end"), QStringLiteral("cmd_paste"),
    QStringLiteral("cmd_paste_over"), QStringLiteral("repeat"), QStringLiteral("cursor_select"),
    QStringLiteral("cursor_unselect"), QStringLiteral("copy_timecode"),
    // visual mode
    QStringLiteral("visual_char"), QStringLiteral("visual_line"), QStringLiteral("visual_block"),
    QStringLiteral("visual_other_end"), QStringLiteral("visual_reselect"),
    QStringLiteral("visual_paste"),
    // modes and lines
    QStringLiteral("insert_mode"), QStringLiteral("command_line"), QStringLiteral("search"),
    QStringLiteral("search_next"), QStringLiteral("search_prev"), QStringLiteral("search_back"),
    QStringLiteral("search_word"), QStringLiteral("search_word_back"),
    // find a clip by the first letter of its name; macros
    QStringLiteral("find_f"), QStringLiteral("find_F"), QStringLiteral("find_t"),
    QStringLiteral("find_T"), QStringLiteral("find_again"), QStringLiteral("find_reverse"),
    QStringLiteral("macro_record"), QStringLiteral("macro_play"), QStringLiteral("macro_repeat"),
    QStringLiteral("macro_ex"),
    // pickers (VimPicker.h) and the command history
    QStringLiteral("picker_media"), QStringLiteral("picker_commands"),
    QStringLiteral("picker_effects"), QStringLiteral("picker_timelines"),
    QStringLiteral("picker_markers"), QStringLiteral("picker_bins"), QStringLiteral("picker_help"),
    QStringLiteral("picker_keymaps"), QStringLiteral("picker_history"),
    // lists and trees (VimListHost.h)
    QStringLiteral("row_down"), QStringLiteral("row_up"), QStringLiteral("row_first"),
    QStringLiteral("row_last"), QStringLiteral("row_half_down"), QStringLiteral("row_half_up"),
    QStringLiteral("list_open"), QStringLiteral("list_right"), QStringLiteral("list_left"),
    QStringLiteral("list_parent"), QStringLiteral("list_new"), QStringLiteral("list_rename"),
    QStringLiteral("list_fold_toggle"), QStringLiteral("list_fold_open"),
    QStringLiteral("list_fold_close"), QStringLiteral("list_fold_open_all"),
    QStringLiteral("list_fold_close_all"), QStringLiteral("list_scroll_top"),
    QStringLiteral("list_scroll_middle"), QStringLiteral("list_scroll_bottom"),
    // forms: the Inspector and every panel of rows (VimFormHost.h)
    QStringLiteral("form_down"), QStringLiteral("form_up"), QStringLiteral("form_first"),
    QStringLiteral("form_last"), QStringLiteral("form_left"), QStringLiteral("form_right"),
    QStringLiteral("form_inc"), QStringLiteral("form_dec"), QStringLiteral("form_inc_coarse"),
    QStringLiteral("form_dec_coarse"), QStringLiteral("form_inc_fine"),
    QStringLiteral("form_dec_fine"), QStringLiteral("form_edit"), QStringLiteral("form_activate"),
    QStringLiteral("form_switch"), QStringLiteral("form_delete"), QStringLiteral("form_yank"),
    QStringLiteral("form_paste"), QStringLiteral("form_fold_toggle"),
    QStringLiteral("form_fold_open"), QStringLiteral("form_fold_close"),
    QStringLiteral("form_fold_open_all"), QStringLiteral("form_fold_close_all"),
    QStringLiteral("form_move_down"), QStringLiteral("form_move_up"),
    QStringLiteral("form_tab_next"), QStringLiteral("form_tab_prev"), QStringLiteral("form_help"),
    QStringLiteral("form_key_toggle"), QStringLiteral("form_key_delete"),
    QStringLiteral("form_key_next"), QStringLiteral("form_key_prev"),
    QStringLiteral("form_key_linear"), QStringLiteral("form_key_ease_in"),
    QStringLiteral("form_key_ease_out"), QStringLiteral("form_key_ease_in_out"),
    QStringLiteral("form_key_smooth"), QStringLiteral("form_key_hold"),
    // keyframe editor, node graphs, mixer, transitions (VimKeyframeHost.h, VimNodeHost.h,
    // VimMixerHost.h)
    QStringLiteral("kf_prev"), QStringLiteral("kf_next"), QStringLiteral("kf_row_down"),
    QStringLiteral("kf_row_up"), QStringLiteral("kf_first"), QStringLiteral("kf_last"),
    QStringLiteral("kf_row_first"), QStringLiteral("kf_row_last"),
    QStringLiteral("kf_select_toggle"), QStringLiteral("kf_select_row"),
    QStringLiteral("kf_select_none"), QStringLiteral("kf_delete"), QStringLiteral("kf_copy"),
    QStringLiteral("kf_paste"), QStringLiteral("kf_add"), QStringLiteral("kf_inc"),
    QStringLiteral("kf_dec"), QStringLiteral("kf_later"), QStringLiteral("kf_earlier"),
    QStringLiteral("kf_jump"), QStringLiteral("kf_toggle_view"), QStringLiteral("kf_fit"),
    QStringLiteral("kf_interp_linear"), QStringLiteral("kf_interp_ease_in"),
    QStringLiteral("kf_interp_ease_out"), QStringLiteral("kf_interp_ease_in_out"),
    QStringLiteral("kf_interp_smooth"), QStringLiteral("kf_interp_hold"),
    QStringLiteral("node_left"), QStringLiteral("node_down"), QStringLiteral("node_up"),
    QStringLiteral("node_right"), QStringLiteral("node_next"), QStringLiteral("node_prev"),
    QStringLiteral("node_first"), QStringLiteral("node_last"), QStringLiteral("node_add"),
    QStringLiteral("node_add_after"), QStringLiteral("node_add_before"),
    QStringLiteral("node_add_parallel"), QStringLiteral("node_add_layer"),
    QStringLiteral("node_delete"), QStringLiteral("node_copy"), QStringLiteral("node_paste"),
    QStringLiteral("node_rename"), QStringLiteral("node_bypass"), QStringLiteral("node_wipe"),
    QStringLiteral("node_connect"), QStringLiteral("node_disconnect"),
    QStringLiteral("node_disconnect_all"), QStringLiteral("node_select_toggle"),
    QStringLiteral("node_select_all"), QStringLiteral("node_fit"), QStringLiteral("mix_left"),
    QStringLiteral("mix_right"), QStringLiteral("mix_first"), QStringLiteral("mix_last"),
    QStringLiteral("mix_up"), QStringLiteral("mix_down"), QStringLiteral("mix_zero"),
    QStringLiteral("mix_pan_left"), QStringLiteral("mix_pan_right"), QStringLiteral("mix_mute"),
    QStringLiteral("mix_solo"), QStringLiteral("mix_arm"), QStringLiteral("mix_type"),
    QStringLiteral("mix_eq"), QStringLiteral("mix_dynamics"), QStringLiteral("transition_next"),
    QStringLiteral("transition_prev"), QStringLiteral("transition_delete"),
    QStringLiteral("transition_longer"), QStringLiteral("transition_shorter"),
    QStringLiteral("transition_align"),
    // panels, windows and pages
    QStringLiteral("win_left"), QStringLiteral("win_down"), QStringLiteral("win_up"),
    QStringLiteral("win_right"), QStringLiteral("win_next"), QStringLiteral("win_prev"),
    QStringLiteral("win_last"), QStringLiteral("win_only"), QStringLiteral("win_reset"),
    QStringLiteral("win_close"), QStringLiteral("focus_media"), QStringLiteral("focus_effects"),
    QStringLiteral("focus_inspector"), QStringLiteral("focus_timeline"),
    QStringLiteral("focus_viewer"), QStringLiteral("focus_keyframes"),
    QStringLiteral("focus_mixer"), QStringLiteral("focus_nodes"), QStringLiteral("focus_metadata"),
    QStringLiteral("focus_index"), QStringLiteral("page_goto"), QStringLiteral("page_prev")};

/// The contexts whose keys a context uses, least specific first.
QStringList chainOf(const QString& context)
{
    if (context == QLatin1String("viewer"))
        return {QStringLiteral("global"), QStringLiteral("timeline"), QStringLiteral("viewer")};
    if (context == QLatin1String("timeline"))
        return {QStringLiteral("global"), QStringLiteral("timeline")};
    if (context == QLatin1String("list") || context == QLatin1String("form")
        || context == QLatin1String("keyframes") || context == QLatin1String("nodes")
        || context == QLatin1String("mixer"))
        return {QStringLiteral("global"), context};
    return {QStringLiteral("global")};
}

QString trieKey(const QString& context, QChar mode)
{
    return context + QLatin1Char('/') + mode;
}

} // namespace

// ---- VimTrie -------------------------------------------------------------------------

struct VimTrie::Node
{
    std::map<QString, std::unique_ptr<Node>> children;
    const VimBinding* binding = nullptr;
    const VimBinding* group = nullptr;
};

VimTrie::VimTrie()
    : m_root(std::make_unique<Node>())
{ }

VimTrie::~VimTrie() = default;
VimTrie::VimTrie(VimTrie&&) noexcept = default;
VimTrie& VimTrie::operator=(VimTrie&&) noexcept = default;

void VimTrie::clear()
{
    m_root = std::make_unique<Node>();
}

void VimTrie::insert(const VimBinding* binding)
{
    Node* node = m_root.get();
    for (const QString& token : binding->tokens) {
        auto& child = node->children[token];
        if (!child)
            child = std::make_unique<Node>();
        node = child.get();
    }
    if (binding->isGroup())
        node->group = binding;
    else
        node->binding = binding;
}

VimMatch VimTrie::match(const QStringList& typed) const
{
    if (typed.isEmpty())
        return {};
    const Node* node = m_root.get();
    VimMatch result;
    for (const QString& token : typed) {
        const auto exact = node->children.find(token);
        if (exact != node->children.end()) {
            node = exact->second.get();
            continue;
        }
        const Node* viaClass = nullptr;
        for (const auto& [name, child] : node->children) {
            if (vimkeys::isClassToken(name) && vimkeys::classMatches(name, token)) {
                viaClass = child.get();
                result.arg = token;
                break;
            }
        }
        if (!viaClass)
            return {};
        node = viaClass;
    }
    const bool more = !node->children.empty();
    if (node->binding) {
        result.binding = node->binding;
        result.kind = more ? VimMatch::Kind::ExactAndPartial : VimMatch::Kind::Exact;
    } else if (more) {
        result.kind = VimMatch::Kind::Partial;
        result.arg.clear();
    } else {
        return {};
    }
    return result;
}

QList<VimContinuation> VimTrie::continuations(const QStringList& typed) const
{
    const Node* node = m_root.get();
    for (const QString& token : typed) {
        const auto it = node->children.find(token);
        if (it != node->children.end()) {
            node = it->second.get();
            continue;
        }
        const Node* viaClass = nullptr;
        for (const auto& [name, child] : node->children) {
            if (vimkeys::isClassToken(name) && vimkeys::classMatches(name, token)) {
                viaClass = child.get();
                break;
            }
        }
        if (!viaClass)
            return {};
        node = viaClass;
    }
    QList<VimContinuation> list;
    for (const auto& [token, child] : node->children) {
        VimContinuation c;
        c.token = token;
        c.binding = child->binding;
        c.more = !child->children.empty();
        c.desc = child->binding ? child->binding->desc
                                : (child->group ? child->group->desc : QString());
        if (c.desc.isEmpty() && c.more)
            c.desc = QObject::tr("more");
        list.append(c);
    }
    // Groups after plain bindings? Keep the key order of the trie (sorted by token text).
    return list;
}

bool VimTrie::startsWith(const QString& token) const
{
    if (m_root->children.count(token))
        return true;
    for (const auto& [name, child] : m_root->children) {
        if (vimkeys::isClassToken(name) && vimkeys::classMatches(name, token))
            return true;
    }
    return false;
}

// ---- VimKeymap -----------------------------------------------------------------------

VimKeymap::VimKeymap(QObject* parent)
    : QObject(parent)
{ }

VimKeymap::~VimKeymap() = default;

VimKeymap* VimKeymap::of(ActionRegistry& actions)
{
    if (auto* existing = actions.findChild<VimKeymap*>(QString(), Qt::FindDirectChildrenOnly))
        return existing;
    auto* keymap = new VimKeymap(&actions);
    keymap->reload();
    return keymap;
}

QStringList VimKeymap::contexts()
{
    return kContexts;
}

QStringList VimKeymap::builtinNames()
{
    return kBuiltins;
}

QString VimKeymap::leader() const
{
    return m_leaderOverride.isEmpty() ? vimopts::leader() : m_leaderOverride;
}

int VimKeymap::timeoutMs() const
{
    return m_timeoutOverride > 0 ? m_timeoutOverride : vimopts::timeoutMs();
}

QString VimKeymap::normaliseRunItem(const QString& rawItem)
{
    QString item = rawItem.trimmed();
    if (item.startsWith(QLatin1String("vim:")) || item.startsWith(QLatin1String("action:"))
        || item.startsWith(QLatin1String("ex:")))
        return item;
    if (item.startsWith(QLatin1Char(':'))) {
        item = item.mid(1);
        if (item.endsWith(QLatin1String("<CR>"), Qt::CaseInsensitive))
            item.chop(4);
        return QStringLiteral("ex:") + item.trimmed();
    }
    return QStringLiteral("action:") + item;
}

QString VimKeymap::checkRunItem(const QString& rawItem, const ActionRegistry* actions) const
{
    const QString item = normaliseRunItem(rawItem);
    if (item.startsWith(QLatin1String("vim:"))) {
        return kBuiltins.contains(item.mid(4)) ? QString() : tr("Unknown Vim command %1").arg(item);
    }
    if (item.startsWith(QLatin1String("action:"))) {
        const QString id = item.mid(7);
        if (id.isEmpty())
            return tr("No command");
        if (id.contains(QRegularExpression(QStringLiteral("\\s"))))
            return tr("No such command: %1").arg(id);
        if (actions && !actions->contains(id))
            return tr("No such command: %1").arg(id);
        return {};
    }
    return item.size() > 3 ? QString() : tr("No command");
}

QString VimKeymap::describeRunItem(const QString& rawItem, const ActionRegistry* actions)
{
    const QString item = normaliseRunItem(rawItem);
    if (item.startsWith(QLatin1String("action:")) && actions && actions->contains(item.mid(7))) {
        QString text = actions->action(item.mid(7))->text();
        text.remove(QLatin1Char('&'));
        return text;
    }
    return item;
}

bool VimKeymap::loadDefaults(QString* error)
{
    QFile file(QStringLiteral(":/vim/default.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("The default Vim keys are missing from the program");
        return false;
    }
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (doc.isNull()) {
        if (error)
            *error = tr("The default Vim keys are broken: %1").arg(parse.errorString());
        return false;
    }
    m_defaults.clear();
    const QString lead = QStringLiteral("<Space>");
    for (const QJsonValue& v : doc.object().value(QLatin1String("bindings")).toArray()) {
        const QJsonObject o = v.toObject();
        VimBinding proto;
        proto.context = o.value(QLatin1String("context")).toString();
        proto.keys = o.value(QLatin1String("keys")).toString();
        proto.type = o.value(QLatin1String("type")).toString(QStringLiteral("command"));
        proto.desc = o.value(QLatin1String("desc")).toString();
        for (const QJsonValue& r : o.value(QLatin1String("run")).toArray())
            proto.run.append(r.toString());
        proto.repeat = o.contains(QLatin1String("repeat"))
            ? o.value(QLatin1String("repeat")).toBool()
            : proto.type == QLatin1String("motion");
        proto.dot = o.value(QLatin1String("dot")).toBool();
        QString parseError;
        proto.tokens = vimkeys::parseKeys(proto.keys, lead, &parseError);
        if (proto.tokens.isEmpty() || !kContexts.contains(proto.context)) {
            if (error)
                *error = tr("Bad default Vim key %1: %2").arg(proto.keys, parseError);
            return false;
        }
        for (const QChar mode : o.value(QLatin1String("modes")).toString(QStringLiteral("n"))) {
            VimBinding b = proto;
            b.mode = mode;
            m_defaults.push_back(std::move(b));
        }
    }
    m_loaded = true;
    rebuild();
    return true;
}

void VimKeymap::reload()
{
    if (!m_loaded)
        loadDefaults();
    m_userOps.clear();
    const QByteArray stored = QSettings().value(QStringLiteral("vim/maps")).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(stored).array()) {
        const QJsonObject o = v.toObject();
        Op op;
        op.op = o.value(QLatin1String("op")).toString(QStringLiteral("map"));
        op.context = o.value(QLatin1String("context")).toString(QStringLiteral("global"));
        op.modes = o.value(QLatin1String("modes")).toString(QStringLiteral("n"));
        op.keys = o.value(QLatin1String("keys")).toString();
        op.type = o.value(QLatin1String("type")).toString();
        op.desc = o.value(QLatin1String("desc")).toString();
        for (const QJsonValue& r : o.value(QLatin1String("run")).toArray())
            op.run.append(r.toString());
        if (!op.keys.isEmpty())
            m_userOps.append(op);
    }
    m_vimrcOps.clear();
    m_leaderOverride.clear();
    m_timeoutOverride = 0;
    m_vimrcMessages.clear();
    // The vimrc is read inside rebuild() order: first the options, so <leader> is known.
    rebuild();
    sourceVimrc();
}

/// Applies one op to @p list. Class tokens ("{a-z}") take part in identity like any token.
void VimKeymap::applyOp(
    std::vector<VimBinding>& list, const Op& op, VimBinding::Origin origin) const
{
    QStringList tokens = vimkeys::parseKeys(op.keys, leader());
    if (tokens.isEmpty())
        return;
    const bool everywhere = op.context == QLatin1String("*");
    const QString context = everywhere ? QStringLiteral("global") : op.context;
    for (const QChar mode : op.modes) {
        auto same = [&](const VimBinding& b) {
            return b.mode == mode && b.tokens == tokens && (everywhere || b.context == context);
        };
        list.erase(std::remove_if(list.begin(), list.end(), same), list.end());
        if (op.op == QLatin1String("unmap"))
            continue;
        VimBinding b;
        b.context = context;
        b.mode = mode;
        b.keys = op.keys;
        b.tokens = tokens;
        b.run = op.run;
        b.type = op.type.isEmpty() ? QStringLiteral("command") : op.type;
        b.desc = op.desc;
        b.origin = origin;
        list.push_back(std::move(b));
    }
}

void VimKeymap::rebuild()
{
    // The defaults were parsed with <Space>; a different leader re-parses them.
    std::vector<VimBinding> list = m_defaults;
    if (leader() != QLatin1String("<Space>")) {
        for (VimBinding& b : list)
            b.tokens = vimkeys::parseKeys(b.keys, leader());
    }
    for (const Op& op : std::as_const(m_userOps))
        applyOp(list, op, VimBinding::Origin::User);
    for (const Op& op : std::as_const(m_vimrcOps))
        applyOp(list, op, VimBinding::Origin::Vimrc);
    // A mapping typed by the user wins over a default of a more specific context: stable sort
    // by origin keeps the order inside each group.
    std::stable_sort(list.begin(), list.end(),
        [](const VimBinding& a, const VimBinding& b) { return int(a.origin) < int(b.origin); });
    m_effective = std::move(list);
    m_tries.clear();
    for (const QString& context : kContexts) {
        const QStringList chain = chainOf(context);
        for (const QChar mode : {QLatin1Char('n'), QLatin1Char('v'), QLatin1Char('o')}) {
            VimTrie trie;
            // Defaults by chain order (inherited first), then user and vimrc entries.
            for (const VimBinding::Origin origin : {VimBinding::Origin::Default,
                     VimBinding::Origin::User, VimBinding::Origin::Vimrc}) {
                for (const QString& c : chain) {
                    for (const VimBinding& b : m_effective) {
                        if (b.origin == origin && b.mode == mode && b.context == c)
                            trie.insert(&b);
                    }
                }
            }
            m_tries.emplace(trieKey(context, mode), std::move(trie));
        }
    }
    emit changed();
}

const VimTrie& VimKeymap::trie(const QString& context, QChar mode) const
{
    const auto it = m_tries.find(
        trieKey(kContexts.contains(context) ? context : QStringLiteral("global"), mode));
    return it == m_tries.end() ? m_empty : it->second;
}

// ---- The user's changes ----

void VimKeymap::saveUserOps()
{
    QJsonArray array;
    for (const Op& op : std::as_const(m_userOps)) {
        QJsonObject o {{QLatin1String("op"), op.op}, {QLatin1String("context"), op.context},
            {QLatin1String("modes"), op.modes}, {QLatin1String("keys"), op.keys}};
        if (op.op == QLatin1String("map")) {
            o.insert(QLatin1String("run"), QJsonArray::fromStringList(op.run));
            if (!op.type.isEmpty())
                o.insert(QLatin1String("type"), op.type);
            if (!op.desc.isEmpty())
                o.insert(QLatin1String("desc"), op.desc);
        }
        array.append(o);
    }
    QSettings settings;
    if (array.isEmpty())
        settings.remove(QStringLiteral("vim/maps"));
    else
        settings.setValue(QStringLiteral("vim/maps"),
            QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

static bool validModes(const QString& modes)
{
    return !modes.isEmpty() && std::all_of(modes.begin(), modes.end(), [](QChar c) {
        return c == QLatin1Char('n') || c == QLatin1Char('v') || c == QLatin1Char('o');
    });
}

bool VimKeymap::map(const QString& context, const QString& modes, const QString& keys,
    const QStringList& rawRun, const QString& desc, QString* error)
{
    QString problem;
    const QStringList tokens = vimkeys::parseKeys(keys, leader(), &problem);
    QStringList run;
    for (const QString& item : rawRun) {
        if (!item.trimmed().isEmpty())
            run.append(normaliseRunItem(item));
    }
    if (tokens.isEmpty())
        problem = problem.isEmpty() ? tr("No keys") : problem;
    else if (context != QLatin1String("*") && !kContexts.contains(context))
        problem = tr("Unknown context %1").arg(context);
    else if (!validModes(modes))
        problem = tr("Modes are n and v");
    else if (run.isEmpty())
        problem = tr("No command");
    else {
        for (const QString& item : run) {
            problem = checkRunItem(item, nullptr);
            if (!problem.isEmpty())
                break;
        }
    }
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return false;
    }
    Op op;
    op.op = QStringLiteral("map");
    op.context = context;
    op.modes = modes;
    op.keys = keys;
    op.run = run;
    op.desc = desc;
    // The same key again replaces the earlier change.
    m_userOps.erase(std::remove_if(m_userOps.begin(), m_userOps.end(),
                        [&](const Op& o) {
                            return o.context == context && o.modes == modes
                                && vimkeys::parseKeys(o.keys, leader()) == tokens;
                        }),
        m_userOps.end());
    m_userOps.append(op);
    saveUserOps();
    rebuild();
    return true;
}

bool VimKeymap::unmap(
    const QString& context, const QString& modes, const QString& keys, QString* error)
{
    QString problem;
    const QStringList tokens = vimkeys::parseKeys(keys, leader(), &problem);
    if (tokens.isEmpty() || !validModes(modes)
        || (context != QLatin1String("*") && !kContexts.contains(context))) {
        if (error)
            *error = problem.isEmpty() ? tr("Not a key of the Vim keys") : problem;
        return false;
    }
    Op op;
    op.op = QStringLiteral("unmap");
    op.context = context;
    op.modes = modes;
    op.keys = keys;
    m_userOps.erase(std::remove_if(m_userOps.begin(), m_userOps.end(),
                        [&](const Op& o) {
                            return o.context == context && o.modes == modes
                                && vimkeys::parseKeys(o.keys, leader()) == tokens;
                        }),
        m_userOps.end());
    m_userOps.append(op);
    saveUserOps();
    rebuild();
    return true;
}

void VimKeymap::resetKeys(const QString& context, const QString& modes, const QString& keys)
{
    const QStringList tokens = vimkeys::parseKeys(keys, leader());
    const qsizetype before = m_userOps.size();
    m_userOps.erase(std::remove_if(m_userOps.begin(), m_userOps.end(),
                        [&](const Op& o) {
                            if (vimkeys::parseKeys(o.keys, leader()) != tokens)
                                return false;
                            const bool ctx
                                = o.context == context || o.context == QLatin1String("*");
                            bool overlap = false;
                            for (const QChar m : modes)
                                overlap = overlap || o.modes.contains(m);
                            return ctx && overlap;
                        }),
        m_userOps.end());
    if (m_userOps.size() != before) {
        saveUserOps();
        rebuild();
    }
}

void VimKeymap::resetAll()
{
    m_userOps.clear();
    saveUserOps();
    rebuild();
}

QByteArray VimKeymap::exportJson() const
{
    QJsonArray array;
    for (const Op& op : m_userOps) {
        QJsonObject o {{QLatin1String("op"), op.op}, {QLatin1String("context"), op.context},
            {QLatin1String("modes"), op.modes}, {QLatin1String("keys"), op.keys}};
        if (op.op == QLatin1String("map")) {
            o.insert(QLatin1String("run"), QJsonArray::fromStringList(op.run));
            if (!op.type.isEmpty())
                o.insert(QLatin1String("type"), op.type);
            if (!op.desc.isEmpty())
                o.insert(QLatin1String("desc"), op.desc);
        }
        array.append(o);
    }
    QJsonObject root {{QLatin1String("kinoklipVimKeys"), 1}, {QLatin1String("leader"), leader()},
        {QLatin1String("maps"), array}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool VimKeymap::importJson(const QByteArray& json, QString* error)
{
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    const QJsonObject root = doc.object();
    if (doc.isNull() || !root.contains(QLatin1String("kinoklipVimKeys"))) {
        if (error)
            *error = tr("This is not a Vim keys file");
        return false;
    }
    QList<Op> ops;
    for (const QJsonValue& v : root.value(QLatin1String("maps")).toArray()) {
        const QJsonObject o = v.toObject();
        Op op;
        op.op = o.value(QLatin1String("op")).toString(QStringLiteral("map"));
        op.context = o.value(QLatin1String("context")).toString(QStringLiteral("global"));
        op.modes = o.value(QLatin1String("modes")).toString(QStringLiteral("n"));
        op.keys = o.value(QLatin1String("keys")).toString();
        op.type = o.value(QLatin1String("type")).toString();
        op.desc = o.value(QLatin1String("desc")).toString();
        for (const QJsonValue& r : o.value(QLatin1String("run")).toArray())
            op.run.append(normaliseRunItem(r.toString()));
        QString problem;
        const QStringList tokens = vimkeys::parseKeys(op.keys, leader(), &problem);
        const bool mapOk = op.op == QLatin1String("unmap")
            || (op.op == QLatin1String("map") && !op.run.isEmpty()
                && std::all_of(op.run.begin(), op.run.end(),
                    [this](const QString& r) { return checkRunItem(r, nullptr).isEmpty(); }));
        if (tokens.isEmpty() || !mapOk || !validModes(op.modes)
            || (op.context != QLatin1String("*") && !kContexts.contains(op.context))) {
            if (error)
                *error = tr("Line %1 of the file is not a valid Vim key: %2")
                             .arg(ops.size() + 1)
                             .arg(op.keys);
            return false;
        }
        ops.append(op);
    }
    m_userOps = ops;
    saveUserOps();
    rebuild();
    return true;
}

void VimKeymap::setLeader(const QString& spec)
{
    const QStringList tokens = vimkeys::parseKeys(spec);
    if (tokens.size() != 1)
        return;
    vimopts::setLeader(tokens.first());
    rebuild();
    emit optionsChanged();
}

void VimKeymap::setTimeoutMs(int ms)
{
    vimopts::setTimeoutMs(ms);
    emit optionsChanged();
}

// ---- vimrc and :nmap ----

QString VimKeymap::vimrcPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/vimrc");
}

bool VimKeymap::sourceVimrc(const QString& path)
{
    const QString file = path.isEmpty() ? vimrcPath() : path;
    QFile f(file);
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    m_vimrcOps.clear();
    m_leaderOverride.clear();
    m_timeoutOverride = 0;
    m_vimrcMessages.clear();
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    // Options first (a `set leader=` after a mapping still applies to it, unlike Vim), then maps.
    for (int pass = 0; pass < 2; ++pass) {
        for (qsizetype i = 0; i < lines.size(); ++i) {
            const QString line = lines[i].trimmed();
            const bool isSet
                = line.startsWith(QLatin1String("set ")) || line.startsWith(QLatin1String(":set "));
            if ((pass == 0) != isSet)
                continue;
            bool ok = true;
            const QString message = executeLine(line, &ok, true);
            if (!ok)
                m_vimrcMessages.append(tr("vimrc line %1: %2").arg(i + 1).arg(message));
        }
    }
    rebuild();
    return true;
}

QString VimKeymap::executeLine(const QString& rawLine, bool* okOut, bool fromVimrc)
{
    bool ok = true;
    auto finish = [&](const QString& message, bool good) {
        ok = good;
        if (okOut)
            *okOut = ok;
        return message;
    };
    QString line = rawLine.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('"')))
        return finish({}, true);
    if (line.startsWith(QLatin1Char(':')))
        line = line.mid(1).trimmed();
    // Split into the command and the rest.
    const qsizetype space = line.indexOf(QRegularExpression(QStringLiteral("\\s")));
    QString command = space < 0 ? line : line.left(space);
    QString rest = space < 0 ? QString() : line.mid(space).trimmed();
    if (command.endsWith(QLatin1Char('!')))
        command.chop(1);

    if (command == QLatin1String("set")) {
        QString result;
        for (const QString& word :
            rest.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
            const qsizetype eq = word.indexOf(QLatin1Char('='));
            const QString name = eq < 0 ? word : word.left(eq);
            const QString value = eq < 0 ? QString() : word.mid(eq + 1);
            if (name == QLatin1String("leader")) {
                const QStringList t = vimkeys::parseKeys(value);
                if (t.size() != 1)
                    return finish(tr("leader wants one key, as in set leader=<Space>"), false);
                if (fromVimrc)
                    m_leaderOverride = t.first();
                else
                    vimopts::setLeader(t.first());
            } else if (name == QLatin1String("timeoutlen")) {
                bool numeric = false;
                const int ms = value.toInt(&numeric);
                if (!numeric || ms < vimopts::kMinTimeoutMs || ms > vimopts::kMaxTimeoutMs)
                    return finish(tr("timeoutlen is a number of milliseconds (%1 to %2)")
                                      .arg(vimopts::kMinTimeoutMs)
                                      .arg(vimopts::kMaxTimeoutMs),
                        false);
                if (fromVimrc)
                    m_timeoutOverride = ms;
                else
                    vimopts::setTimeoutMs(ms);
            } else if (name == QLatin1String("physicalkeys")
                || name == QLatin1String("nophysicalkeys")) {
                vimopts::setPhysicalKeys(name == QLatin1String("physicalkeys"));
            } else if (name == QLatin1String("vim") || name == QLatin1String("novim")) {
                if (fromVimrc)
                    return finish(tr("the vimrc cannot turn Vim mode on or off"), false);
                vimopts::setEnabled(name == QLatin1String("vim"));
            } else {
                return finish(tr("Unknown option: %1").arg(name), false);
            }
        }
        if (!fromVimrc) {
            rebuild();
            emit optionsChanged();
        }
        return finish({}, true);
    }

    if (command == QLatin1String("map") && rest.isEmpty())
        return finish(listMappings().join(QLatin1Char('\n')), true);

    static const QStringList mapNormal {QStringLiteral("nmap"), QStringLiteral("nnoremap"),
        QStringLiteral("nm"), QStringLiteral("nn")};
    static const QStringList mapVisual {QStringLiteral("vmap"), QStringLiteral("vnoremap"),
        QStringLiteral("xmap"), QStringLiteral("xnoremap"), QStringLiteral("vm"),
        QStringLiteral("xm")};
    static const QStringList mapBoth {QStringLiteral("map"), QStringLiteral("noremap")};
    static const QStringList unmapNormal {QStringLiteral("nunmap"), QStringLiteral("nun")};
    static const QStringList unmapVisual {QStringLiteral("vunmap"), QStringLiteral("xunmap")};
    static const QStringList unmapBoth {QStringLiteral("unmap")};
    QString modes;
    bool isMap = true;
    if (mapNormal.contains(command))
        modes = QStringLiteral("n");
    else if (mapVisual.contains(command))
        modes = QStringLiteral("v");
    else if (mapBoth.contains(command))
        modes = QStringLiteral("nv");
    else if (unmapNormal.contains(command))
        modes = QStringLiteral("n"), isMap = false;
    else if (unmapVisual.contains(command))
        modes = QStringLiteral("v"), isMap = false;
    else if (unmapBoth.contains(command))
        modes = QStringLiteral("nv"), isMap = false;
    else
        return finish(tr("Not a Vim keys command: %1").arg(command), false);

    // <silent>, <nowait>, <buffer>, <script>, <unique> are accepted and ignored.
    static const QRegularExpression modifier(
        QStringLiteral("^<(silent|nowait|buffer|script|unique|expr)>\\s*"),
        QRegularExpression::CaseInsensitiveOption);
    for (QRegularExpressionMatch m = modifier.match(rest); m.hasMatch(); m = modifier.match(rest))
        rest = rest.mid(m.capturedLength()).trimmed();
    if (rest.isEmpty())
        return finish(
            tr("%1 wants the keys%2").arg(command, isMap ? tr(" and a command") : QString()),
            false);
    const qsizetype split = rest.indexOf(QRegularExpression(QStringLiteral("\\s")));
    const QString keys = split < 0 ? rest : rest.left(split);
    const QString run = split < 0 ? QString() : rest.mid(split).trimmed();
    QString problem;
    if (vimkeys::parseKeys(keys, leader(), &problem).isEmpty())
        return finish(problem, false);

    if (!isMap) {
        if (fromVimrc) {
            Op op;
            op.op = QStringLiteral("unmap");
            op.context = QStringLiteral("*");
            op.modes = modes;
            op.keys = keys;
            m_vimrcOps.append(op);
            return finish({}, true);
        }
        if (!unmap(QStringLiteral("*"), modes, keys, &problem))
            return finish(problem, false);
        return finish({}, true);
    }
    if (run.isEmpty())
        return finish(tr("%1 wants the keys and a command").arg(command), false);
    if (fromVimrc) {
        const QString item = normaliseRunItem(run);
        problem = checkRunItem(item, nullptr);
        if (!problem.isEmpty())
            return finish(problem, false);
        Op op;
        op.op = QStringLiteral("map");
        op.context = QStringLiteral("*");
        op.modes = modes;
        op.keys = keys;
        op.run = {item};
        m_vimrcOps.append(op);
        return finish({}, true);
    }
    if (!map(QStringLiteral("*"), modes, keys, {run}, QString(), &problem))
        return finish(problem, false);
    return finish({}, true);
}

QStringList VimKeymap::listMappings() const
{
    QStringList lines;
    for (const VimBinding& b : m_effective) {
        if (b.origin == VimBinding::Origin::Default || b.isGroup())
            continue;
        lines.append(QStringLiteral("%1  %2  %3  %4%5")
                .arg(b.mode)
                .arg(b.context, -9)
                .arg(b.keys, -14)
                .arg(b.run.join(QLatin1String("; ")),
                    b.origin == VimBinding::Origin::Vimrc ? QStringLiteral("  (vimrc)")
                                                          : QString()));
    }
    for (const Op& op : m_userOps) {
        if (op.op == QLatin1String("unmap"))
            lines.append(QStringLiteral("%1  %2  %3  (unmapped)")
                    .arg(op.modes, -2)
                    .arg(op.context, -9)
                    .arg(op.keys, -14));
    }
    if (lines.isEmpty())
        lines.append(tr("No mappings of your own"));
    return lines;
}

// ---- Checks ----

QList<VimKeymap::Issue> VimKeymap::issues(const ActionRegistry* actions) const
{
    QList<Issue> list;
    // Duplicates inside one context and mode.
    for (qsizetype i = 0; i < qsizetype(m_effective.size()); ++i) {
        const VimBinding& a = m_effective[i];
        for (qsizetype j = i + 1; j < qsizetype(m_effective.size()); ++j) {
            const VimBinding& b = m_effective[j];
            if (a.context == b.context && a.mode == b.mode && a.tokens == b.tokens
                && a.isGroup() == b.isGroup())
                list.append({Issue::Severity::Error, a.context, a.keys,
                    tr("%1 is bound twice in %2").arg(a.keys, a.context)});
        }
    }
    for (const VimBinding& b : m_effective) {
        if (b.isGroup())
            continue;
        if (b.run.isEmpty())
            list.append(
                {Issue::Severity::Error, b.context, b.keys, tr("%1 has no command").arg(b.keys)});
        for (const QString& item : b.run) {
            const QString problem = checkRunItem(item, actions);
            if (!problem.isEmpty())
                list.append({Issue::Severity::Error, b.context, b.keys, problem});
        }
    }
    // Prefixes: a binding that is also the start of a longer one, as seen from each context.
    for (const QString& context : kContexts) {
        for (const QChar mode : {QLatin1Char('n'), QLatin1Char('v'), QLatin1Char('o')}) {
            const VimTrie& t = trie(context, mode);
            for (const VimBinding& b : m_effective) {
                if (b.isGroup() || b.mode != mode || b.context != context)
                    continue;
                const VimMatch m = t.match(b.tokens);
                if (m.kind == VimMatch::Kind::ExactAndPartial && m.binding == &b)
                    list.append({Issue::Severity::Warning, context, b.keys,
                        tr("%1 is also the start of longer keys: it runs after %2 ms without "
                           "another key")
                            .arg(b.keys)
                            .arg(timeoutMs())});
            }
        }
    }
    // Preset keys the layer shadows: a plain key bound in NORMAL.
    if (actions) {
        QMap<QString, QString> presetKeys; // "I" / "Shift+I" -> action id
        for (const QString& id : actions->ids()) {
            for (const QKeySequence& s : actions->shortcuts(id)) {
                if (s.count() == 1)
                    presetKeys.insert(s.toString(QKeySequence::PortableText), id);
            }
        }
        QSet<QString> seen;
        for (const VimBinding& b : m_effective) {
            if (b.isGroup() || b.mode != QLatin1Char('n') || b.tokens.isEmpty())
                continue;
            const QString first = b.tokens.first();
            if (!vimkeys::isCharToken(first) || !first.at(0).isLetterOrNumber())
                continue;
            const QString name
                = first.at(0).isUpper() ? QStringLiteral("Shift+") + first : first.toUpper();
            if (!presetKeys.contains(name) || seen.contains(name))
                continue;
            seen.insert(name);
            list.append({Issue::Severity::Info, b.context, first,
                tr("The profile's key %1 (%2) is inert in NORMAL; it works in INSERT")
                    .arg(name,
                        describeRunItem(
                            QStringLiteral("action:") + presetKeys.value(name), actions))});
        }
    }
    return list;
}

} // namespace kk
