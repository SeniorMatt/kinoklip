// SPDX-License-Identifier: GPL-3.0-or-later
// VimKeymap — the data-driven key table of the Vim layer (spec section 83): the default table
// (resources/vim/default.json), the user's changes (QSettings "vim/maps", edited in Keyboard
// Customization > Vim keys or with :nmap), the vimrc text file, and a trie per context for the
// matcher. No widgets.
//
// A binding is {context, mode, keys, run, type, desc}. Contexts: "global", "timeline" and
// "viewer"; a context's keys are those of itself, of the contexts it inherits (viewer ->
// timeline -> global), and the user's own, which win. Modes: 'n' NORMAL, 'v' VISUAL. `run` is a
// list of commands: "vim:<builtin>", "action:<registry id>", "ex:<command line>". Type
// "group" gives a key prefix a name for the which-key popup and runs nothing.
//
// The trie with exact / partial / ambiguous results and a timeout for the ambiguous case
// follows qutebrowser's BindingTrie and MatchResult (https://github.com/qutebrowser/qutebrowser,
// GPL-3.0-or-later; written anew in C++ for Kinoklip, see attribution.md). The layers as
// contexts and the {on, run, desc} entry shape follow yazi's keymap-default.toml (MIT).
#pragma once

#include <QChar>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include <map>
#include <memory>
#include <vector>

namespace kk {

class ActionRegistry;

struct VimBinding
{
    enum class Origin {
        Default,
        User,
        Vimrc
    };

    QString context; ///< "global", "timeline", "viewer"
    QChar mode = QLatin1Char('n'); ///< 'n' or 'v'
    QString keys; ///< as written ("<leader>ff"), with the leader as typed by the user
    QStringList tokens; ///< parsed, with the leader replaced
    QStringList run;
    QString type = QStringLiteral("command"); ///< motion, operator, command, mode, group
    QString desc;
    bool repeat = false; ///< a count repeats the action (motions, undo)
    Origin origin = Origin::Default;

    [[nodiscard]] bool isGroup() const { return type == QLatin1String("group"); }
};

/// Result of matching typed tokens in a trie.
struct VimMatch
{
    enum class Kind {
        None,
        Partial,
        Exact,
        ExactAndPartial
    };
    Kind kind = Kind::None;
    const VimBinding* binding = nullptr; ///< for Exact and ExactAndPartial
    QString arg; ///< the typed character that matched a class token ("{a-z}")
};

/// One way to continue a typed prefix (which-key).
struct VimContinuation
{
    QString token;
    const VimBinding* binding = nullptr; ///< a binding that ends here, if any
    QString desc; ///< the binding's description, or the group's name
    bool more = false; ///< longer sequences continue from here
};

class VimTrie
{
public:
    VimTrie();
    ~VimTrie();
    VimTrie(VimTrie&&) noexcept;
    VimTrie& operator=(VimTrie&&) noexcept;

    void clear();
    /// Adds @p binding; a later insert with the same tokens replaces the earlier one.
    void insert(const VimBinding* binding);
    [[nodiscard]] VimMatch match(const QStringList& typed) const;
    /// The tokens that can follow @p typed, sorted for display (groups and bindings).
    [[nodiscard]] QList<VimContinuation> continuations(const QStringList& typed) const;
    /// Whether any binding starts with @p token.
    [[nodiscard]] bool startsWith(const QString& token) const;

    struct Node;

private:
    std::unique_ptr<Node> m_root;
};

class VimKeymap : public QObject
{
    Q_OBJECT
public:
    explicit VimKeymap(QObject* parent = nullptr);
    ~VimKeymap() override;

    /// The keymap of @p actions' application: created on first use as a child of @p actions
    /// (no singleton), loaded from the resources, the settings and the vimrc.
    static VimKeymap* of(ActionRegistry& actions);

    static QStringList contexts();
    /// The names of the built-in commands a `vim:` entry can run (VimMachine implements them).
    static QStringList builtinNames();

    /// Reads the default table. False (with @p error) if the resource is broken.
    bool loadDefaults(QString* error = nullptr);
    /// Reads leader and timeout, the user's changes (QSettings) and the vimrc, rebuilds.
    void reload();

    [[nodiscard]] QString leader() const; ///< a token, "<Space>" by default
    [[nodiscard]] int timeoutMs() const;

    [[nodiscard]] const std::vector<VimBinding>& bindings() const { return m_effective; }
    [[nodiscard]] const std::vector<VimBinding>& defaults() const { return m_defaults; }
    /// The trie of one context and mode (its own keys and the inherited ones).
    [[nodiscard]] const VimTrie& trie(const QString& context, QChar mode) const;

    // ---- The user's changes ----

    /// One change on top of the defaults. @p op is "map" or "unmap"; context "*" (from :nmap
    /// and :nunmap) means every context.
    struct Op
    {
        QString op = QStringLiteral("map");
        QString context = QStringLiteral("global");
        QString modes = QStringLiteral("n");
        QString keys;
        QStringList run;
        QString type;
        QString desc;
    };
    [[nodiscard]] QList<Op> userOps() const { return m_userOps; }
    /// Adds (or replaces) a mapping. False with @p error for keys or commands that do not work.
    bool map(const QString& context, const QString& modes, const QString& keys,
        const QStringList& run, const QString& desc, QString* error = nullptr);
    /// Removes a key (a default or one of the user's own).
    bool unmap(const QString& context, const QString& modes, const QString& keys,
        QString* error = nullptr);
    /// Drops the user's changes of one key (the default comes back).
    void resetKeys(const QString& context, const QString& modes, const QString& keys);
    void resetAll();
    /// The Vim keys as JSON (the user's changes), and the other way round (replaces them).
    [[nodiscard]] QByteArray exportJson() const;
    bool importJson(const QByteArray& json, QString* error = nullptr);

    // ---- Options ----

    void setLeader(const QString& spec);
    void setTimeoutMs(int ms);

    // ---- The vimrc ----

    /// `<config>/kinoklip/vimrc`
    static QString vimrcPath();
    [[nodiscard]] QStringList vimrcMessages() const { return m_vimrcMessages; }
    /// Reads @p path (default vimrcPath()); mappings from it are marked as coming from the
    /// file and are not stored in the settings. Messages (errors per line) go to
    /// vimrcMessages(). Returns whether a file was read.
    bool sourceVimrc(const QString& path = {});
    /// Runs one `:nmap`-style line (also a vimrc line): nmap, nnoremap, vmap, xmap, map,
    /// nunmap, vunmap, unmap, set leader= / timeoutlen= / physicalkeys. Returns the message
    /// ("" = done silently); @p ok tells whether it worked. @p fromVimrc keeps the result out
    /// of the settings.
    QString executeLine(const QString& line, bool* ok = nullptr, bool fromVimrc = false);
    /// What `:map` prints: one line per user mapping.
    [[nodiscard]] QStringList listMappings() const;

    // ---- Checks ----

    struct Issue
    {
        enum class Severity {
            Error,
            Warning,
            Info
        };
        Severity severity = Severity::Info;
        QString context;
        QString keys;
        QString text;
    };
    /// Duplicates and unknown commands (errors), keys that are also the start of longer keys
    /// (warnings: the shorter one waits for timeoutlen), preset keys the layer shadows in
    /// NORMAL (info, with @p actions).
    [[nodiscard]] QList<Issue> issues(const ActionRegistry* actions = nullptr) const;
    /// Checks one command item; "" when it can run.
    [[nodiscard]] QString checkRunItem(const QString& item, const ActionRegistry* actions) const;
    /// "action:id" for a bare id, "ex:..." for ":w<CR>".
    [[nodiscard]] static QString normaliseRunItem(const QString& item);
    /// Display text of a command item: the action's menu text, or the item itself.
    [[nodiscard]] static QString describeRunItem(
        const QString& item, const ActionRegistry* actions);

signals:
    void changed();
    /// leader / timeout / physical-keys options changed by :set
    void optionsChanged();

private:
    struct OpResult;
    void rebuild();
    void applyOp(std::vector<VimBinding>& list, const Op& op, VimBinding::Origin origin) const;
    void saveUserOps();
    bool parseOp(const Op& op, std::vector<VimBinding>* entries, QString* error) const;

    std::vector<VimBinding> m_defaults; ///< defaults with the default leader
    QList<Op> m_userOps;
    QList<Op> m_vimrcOps;
    std::vector<VimBinding> m_effective;
    std::map<QString, VimTrie> m_tries; ///< "context/mode"
    VimTrie m_empty;
    QString m_leaderOverride; ///< from the vimrc
    int m_timeoutOverride = 0;
    QStringList m_vimrcMessages;
    bool m_loaded = false;
};

} // namespace kk
