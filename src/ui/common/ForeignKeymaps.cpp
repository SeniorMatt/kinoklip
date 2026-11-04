// SPDX-License-Identifier: GPL-3.0-or-later
// ForeignKeymaps implementation: a small XML tree, a property list reader, command names and
// key codes of Premiere Pro and Final Cut Pro files mapped to Kinoklip's.
//
// Clean room: the file shapes and names below come from general knowledge of these formats (an
// XML file of shortcut entries with a command name, a Windows virtual key and modifiers; a
// property list of command names with a character and an NSEvent modifier mask). Nothing is
// copied from vendor files or documentation, and the parser accepts many spellings of each
// field, so unknown layouts degrade to "skipped" instead of failing.
#include "ui/common/ForeignKeymaps.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QVariant>
#include <QXmlStreamReader>

#include <memory>
#include <vector>

namespace kk::foreignkeys {

namespace {

Q_LOGGING_CATEGORY(lcForeign, "kk.ui.foreignkeys", QtInfoMsg)

enum class Format {
    Premiere,
    FinalCut
};

// ---- Command names ----

/// Normalised command names (lower case, letters and digits only) to Kinoklip actions. A file's
/// name matches when one of these equals the whole name or its last words ("cmd.edit.undo" ->
/// "undo"); the longest match wins.
const QHash<QString, QString>& aliases()
{
    static const QHash<QString, QString> table = [] {
        QHash<QString, QString> t;
        auto add = [&t](const char* action, std::initializer_list<const char*> names) {
            for (const char* n : names)
                t.insert(QLatin1String(n), QLatin1String(action));
        };
        add("edit.undo", {"undo"});
        add("edit.redo", {"redo"});
        add("edit.cut", {"cut", "editcut"});
        add("edit.copy", {"copy"});
        add("edit.paste", {"paste"});
        add("edit.paste_insert", {"pasteinsert"});
        add("edit.paste_attributes", {"pasteattributes", "pasteeffects"});
        add("edit.delete", {"clear", "delete", "lift", "deleteselection", "replacewithgap"});
        add("edit.ripple_delete", {"rippledelete", "extract", "ripplecleardelete"});
        add("edit.select_all", {"selectall"});
        add("edit.deselect_all", {"deselectall"});
        add("edit.insert", {"insert", "splicein", "insertedit"});
        add("edit.overwrite", {"overwrite", "overwriteedit"});
        add("edit.replace", {"replace", "replaceedit"});
        add("edit.append_at_end", {"append", "appendtostoryline", "appendtoend"});
        add("edit.place_on_top", {"connect", "connecttoprimarystoryline", "placeontop"});
        add("edit.split",
            {"addedit", "razoratcti", "razoratplayhead", "split", "splitatplayhead", "blade",
                "bladeatplayhead", "cutatplayhead"});
        add("tool.blade", {"bladetool", "razortool", "selectbladetool", "razor"});
        add("tool.select", {"selectiontool", "selecttool", "selectselecttool", "arrowtool"});
        add("tool.trim", {"trimtool", "rippleedittool", "selecttrimtool"});
        add("mark.in", {"markin", "setin", "setinpoint", "markinpoint"});
        add("mark.out", {"markout", "setout", "setoutpoint", "markoutpoint"});
        add("mark.clip", {"markclip", "selectclip"});
        add("mark.clear_in", {"clearin", "clearinpoint"});
        add("mark.clear_out", {"clearout", "clearoutpoint"});
        add("mark.clear_both",
            {"clearinandout", "clearinout", "clearrange", "clearinandoutpoints"});
        add("mark.go_to_in", {"gotoin", "gotoinpoint"});
        add("mark.go_to_out", {"gotoout", "gotooutpoint"});
        add("mark.add_marker", {"addmarker", "marker"});
        add("mark.modify_marker", {"addmarkerandmodify", "editmarker", "modifymarker"});
        add("mark.delete_marker", {"deletemarker", "clearmarker", "clearcurrentmarker"});
        add("transport.next_marker", {"nextmarker", "gotonextmarker"});
        add("transport.prev_marker", {"previousmarker", "prevmarker", "gotopreviousmarker"});
        add("transport.play_pause",
            {"playpause", "playstop", "play", "toggleplay", "playstoptoggle"});
        add("transport.play_reverse", {"playreverse", "playbackwards"});
        add("transport.shuttle_reverse", {"shuttleleft", "shufflereverse", "shuttlereverse"});
        add("transport.shuttle_forward", {"shuttleright", "shuttleforward"});
        add("transport.shuttle_stop", {"shuttlestop", "stop"});
        add("transport.prev_frame",
            {"stepback", "stepbackward", "previousframe", "prevframe", "stepleft", "backoneframe"});
        add("transport.next_frame", {"stepforward", "nextframe", "stepright", "forwardoneframe"});
        add("transport.prev_edit",
            {"previousedit", "gotopreviousedit", "previouseditpoint", "gotopreviouseditpoint",
                "previousedittoplayhead"});
        add("transport.next_edit",
            {"nextedit", "gotonextedit", "nexteditpoint", "gotonexteditpoint"});
        add("transport.go_to_start",
            {"gotostart", "gotobeginning", "gotosequencestart", "gototimelinestart"});
        add("transport.go_to_end", {"gotoend", "gotosequenceend", "gototimelineend"});
        add("transport.loop", {"loop", "toggleloop", "loopplayback"});
        add("transport.play_in_out", {"playinout", "playintoout", "playselection"});
        add("transport.play_around", {"playaround"});
        add("timeline.zoom_in", {"zoomin"});
        add("timeline.zoom_out", {"zoomout"});
        add("timeline.zoom_fit", {"zoomtofit", "zoomtosequence", "zoomtofitsequence"});
        add("timeline.snapping", {"snap", "snapping", "togglesnapping", "snapintimeline"});
        add("timeline.linked_selection", {"linkedselection", "togglelinkedselection"});
        add("timeline.add_transition",
            {"adddefaulttransition", "applyvideotransition", "adddefaultvideotransition",
                "addtransition", "addcrossdissolve"});
        add("clip.link", {"link", "linkunlink", "linkclips"});
        add("clip.unlink", {"unlink", "detachaudio", "unlinkclips"});
        add("clip.toggle_enabled", {"enable", "enabledisable", "clipenable", "toggleenable"});
        add("clip.match_frame", {"matchframe", "revealinbrowser"});
        add("clip.change_speed", {"speedduration", "changespeed", "clipspeedduration"});
        add("clip.change_duration", {"changeduration", "duration"});
        add("clip.retime_controls", {"retimeeditor", "showretimeeditor"});
        add("clip.freeze_frame", {"freezeframe", "addfreezeframe"});
        add("clip.new_compound", {"newcompoundclip", "nest"});
        add("clip.decompose", {"breakapart", "breakapartclipitems"});
        add("clip.add_keyframe", {"addkeyframe"});
        add("clip.next_keyframe", {"nextkeyframe"});
        add("clip.prev_keyframe", {"previouskeyframe"});
        add("trim.extend_edit",
            {"extendedit", "extendedittoplayhead", "extendselectededittoplayhead"});
        add("trim.select_nearest_edit", {"selectnearesteditpoint", "nearesteditpoint"});
        add("trim.select_nearest_clip", {"selectclipatplayhead"});
        add("trim.ripple_trim_start", {"rippletrimpreviousedittoplayhead", "trimtop"});
        add("trim.ripple_trim_end", {"rippletrimnextedittoplayhead", "trimtail"});
        add("trim.trim_start", {"trimpreviousedittoplayhead", "trimstart"});
        add("trim.trim_end", {"trimnextedittoplayhead", "trimend"});
        add("trim.nudge_left", {"nudgeleft", "nudgeclipselectionleft", "nudgeselectionleft"});
        add("trim.nudge_right", {"nudgeright", "nudgeclipselectionright", "nudgeselectionright"});
        add("file.new", {"newproject"});
        add("timeline.new", {"newsequence", "newtimeline"});
        add("file.open", {"open", "openproject"});
        add("file.save", {"save", "saveproject"});
        add("file.save_as", {"saveas", "saveprojectas"});
        add("file.import_media", {"import", "importmedia"});
        add("file.quit", {"quit", "exit"});
        add("view.page_deliver", {"exportmedia", "share", "exportfile", "sharedefault"});
        add("mediapool.new_bin", {"newbin", "newfolder"});
        add("edit.keyboard_customization",
            {"keyboardshortcuts", "commandeditor", "customizecommands", "editcommands"});
        add("edit.preferences", {"preferences"});
        add("view.fullscreen_viewer", {"playfullscreen", "togglefullscreen", "fullscreen"});
        add("view.fullscreen", {"enterfullscreen"});
        return t;
    }();
    return table;
}

/// Overrides of the generic names where an editor means something else by them.
QString formatSpecific(Format format, const QString& normalised)
{
    if (format == Format::FinalCut && normalised == QLatin1String("newproject"))
        return QStringLiteral("timeline.new"); // a Final Cut "project" is a timeline
    if (format == Format::FinalCut && normalised == QLatin1String("delete"))
        return QStringLiteral("edit.ripple_delete"); // the magnetic timeline closes the gap
    return {};
}

/// The words of a command name, lower case: "cmd.edit.razorAtCTI" -> cmd, edit, razor, at, cti.
QStringList words(const QString& name)
{
    QStringList list;
    QString current;
    auto flush = [&] {
        if (!current.isEmpty())
            list.append(current.toLower());
        current.clear();
    };
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (!c.isLetterOrNumber()) {
            flush();
            continue;
        }
        if (!current.isEmpty()) {
            const QChar prev = current.back();
            const bool lowerToUpper = prev.isLower() && c.isUpper();
            const bool acronymEnd
                = prev.isUpper() && c.isUpper() && i + 1 < name.size() && name.at(i + 1).isLower();
            if (lowerToUpper || acronymEnd)
                flush();
        }
        current.append(c);
    }
    flush();
    return list;
}

QString actionFor(Format format, const QString& command)
{
    const QStringList w = words(command);
    // Longest run of last words first: "rippletrimnextedittoplayhead" before "toplayhead".
    for (int first = 0; first < w.size(); ++first) {
        const QString tail = w.mid(first).join(QString());
        if (const QString specific = formatSpecific(format, tail); !specific.isEmpty())
            return specific;
        if (const auto it = aliases().constFind(tail); it != aliases().cend())
            return *it;
    }
    return {};
}

// ---- Keys ----

constexpr int kNoKey = 0;

/// Named keys as files write them ("Space", "PageUp", "Delete").
int namedKey(const QString& lower, Format format)
{
    static const QHash<QString, int> names {{QStringLiteral("space"), Qt::Key_Space},
        {QStringLiteral("spacebar"), Qt::Key_Space}, {QStringLiteral("left"), Qt::Key_Left},
        {QStringLiteral("leftarrow"), Qt::Key_Left}, {QStringLiteral("right"), Qt::Key_Right},
        {QStringLiteral("rightarrow"), Qt::Key_Right}, {QStringLiteral("up"), Qt::Key_Up},
        {QStringLiteral("uparrow"), Qt::Key_Up}, {QStringLiteral("down"), Qt::Key_Down},
        {QStringLiteral("downarrow"), Qt::Key_Down}, {QStringLiteral("home"), Qt::Key_Home},
        {QStringLiteral("end"), Qt::Key_End}, {QStringLiteral("pageup"), Qt::Key_PageUp},
        {QStringLiteral("pgup"), Qt::Key_PageUp}, {QStringLiteral("pagedown"), Qt::Key_PageDown},
        {QStringLiteral("pgdown"), Qt::Key_PageDown}, {QStringLiteral("pgdn"), Qt::Key_PageDown},
        {QStringLiteral("forwarddelete"), Qt::Key_Delete}, {QStringLiteral("del"), Qt::Key_Delete},
        {QStringLiteral("backspace"), Qt::Key_Backspace},
        {QStringLiteral("return"), Qt::Key_Return}, {QStringLiteral("enter"), Qt::Key_Return},
        {QStringLiteral("tab"), Qt::Key_Tab}, {QStringLiteral("escape"), Qt::Key_Escape},
        {QStringLiteral("esc"), Qt::Key_Escape}};
    if (lower == QLatin1String("delete"))
        return format == Format::FinalCut ? Qt::Key_Backspace : Qt::Key_Delete; // Mac "delete"
    if (const auto it = names.constFind(lower); it != names.cend())
        return *it;
    static const QRegularExpression fkey(QStringLiteral("^f([1-9]|1[0-9]|2[0-4])$"));
    if (const auto m = fkey.match(lower); m.hasMatch())
        return Qt::Key_F1 + m.captured(1).toInt() - 1;
    return kNoKey;
}

/// A key written as one character, including the Mac function-key characters of a property
/// list (U+F700 up ... U+F72D page down).
int characterKey(QChar c, bool* keypad = nullptr)
{
    if (keypad)
        *keypad = false;
    const char16_t u = c.unicode();
    switch (u) {
    case 0x20:
        return Qt::Key_Space;
    case 0x08:
    case 0x7f:
        return Qt::Key_Backspace;
    case 0x09:
        return Qt::Key_Tab;
    case 0x0d:
    case 0x03:
        return Qt::Key_Return;
    case 0x1b:
        return Qt::Key_Escape;
    case 0xf700:
        return Qt::Key_Up;
    case 0xf701:
        return Qt::Key_Down;
    case 0xf702:
        return Qt::Key_Left;
    case 0xf703:
        return Qt::Key_Right;
    case 0xf728:
        return Qt::Key_Delete;
    case 0xf729:
        return Qt::Key_Home;
    case 0xf72b:
        return Qt::Key_End;
    case 0xf72c:
        return Qt::Key_PageUp;
    case 0xf72d:
        return Qt::Key_PageDown;
    default:
        break;
    }
    if (u >= 0xf704 && u <= 0xf71b) // F1 ... F24
        return Qt::Key_F1 + (u - 0xf704);
    const QChar upper = c.toUpper();
    if ((upper >= QLatin1Char('A') && upper <= QLatin1Char('Z'))
        || (upper >= QLatin1Char('0') && upper <= QLatin1Char('9')))
        return upper.unicode(); // Qt::Key_A ... are the ASCII codes
    static const QString punctuation = QStringLiteral(",./;'[]\\-=`");
    if (punctuation.contains(c))
        return c.unicode();
    return kNoKey;
}

/// Windows virtual key codes (Premiere's files).
int virtualKey(int vk, bool* keypad)
{
    *keypad = false;
    if ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5a))
        return vk;
    if (vk >= 0x60 && vk <= 0x69) {
        *keypad = true;
        return Qt::Key_0 + (vk - 0x60);
    }
    if (vk >= 0x70 && vk <= 0x87)
        return Qt::Key_F1 + (vk - 0x70);
    static const QHash<int, int> codes {{0x08, Qt::Key_Backspace}, {0x09, Qt::Key_Tab},
        {0x0d, Qt::Key_Return}, {0x1b, Qt::Key_Escape}, {0x20, Qt::Key_Space},
        {0x21, Qt::Key_PageUp}, {0x22, Qt::Key_PageDown}, {0x23, Qt::Key_End}, {0x24, Qt::Key_Home},
        {0x25, Qt::Key_Left}, {0x26, Qt::Key_Up}, {0x27, Qt::Key_Right}, {0x28, Qt::Key_Down},
        {0x2e, Qt::Key_Delete}, {0xba, Qt::Key_Semicolon}, {0xbb, Qt::Key_Equal},
        {0xbc, Qt::Key_Comma}, {0xbd, Qt::Key_Minus}, {0xbe, Qt::Key_Period}, {0xbf, Qt::Key_Slash},
        {0xc0, Qt::Key_QuoteLeft}, {0xdb, Qt::Key_BracketLeft}, {0xdc, Qt::Key_Backslash},
        {0xdd, Qt::Key_BracketRight}, {0xde, Qt::Key_Apostrophe}};
    return codes.value(vk, kNoKey);
}

/// Mac virtual key codes of the keys that have no printable character (Final Cut's files).
int macKeyCode(int code)
{
    static const QHash<int, int> codes {{49, Qt::Key_Space}, {51, Qt::Key_Backspace},
        {117, Qt::Key_Delete}, {123, Qt::Key_Left}, {124, Qt::Key_Right}, {125, Qt::Key_Down},
        {126, Qt::Key_Up}, {115, Qt::Key_Home}, {119, Qt::Key_End}, {116, Qt::Key_PageUp},
        {121, Qt::Key_PageDown}, {36, Qt::Key_Return}, {48, Qt::Key_Tab}, {53, Qt::Key_Escape},
        {122, Qt::Key_F1}, {120, Qt::Key_F2}, {99, Qt::Key_F3}, {118, Qt::Key_F4}, {96, Qt::Key_F5},
        {97, Qt::Key_F6}, {98, Qt::Key_F7}, {100, Qt::Key_F8}, {101, Qt::Key_F9},
        {109, Qt::Key_F10}, {103, Qt::Key_F11}, {111, Qt::Key_F12}};
    return codes.value(code, kNoKey);
}

bool isNumber(const QString& s, int* value)
{
    bool ok = false;
    const QString t = s.trimmed();
    const int v = t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? t.mid(2).toInt(&ok, 16)
                                                                         : t.toInt(&ok);
    if (ok && value)
        *value = v;
    return ok;
}

/// Modifiers written as words or symbols ("Ctrl+Shift", "cmd shift", "⌘⇧"). Final Cut's
/// Command becomes Ctrl, Option Alt and Control Meta.
Qt::KeyboardModifiers modifierWords(const QString& text, Format format)
{
    Qt::KeyboardModifiers m;
    const QString t = text.toLower();
    const bool mac = format == Format::FinalCut;
    if (t.contains(QLatin1String("shift")) || t.contains(QChar(0x21e7)))
        m |= Qt::ShiftModifier;
    if (t.contains(QLatin1String("alt")) || t.contains(QLatin1String("opt"))
        || t.contains(QChar(0x2325)))
        m |= Qt::AltModifier;
    if (t.contains(QLatin1String("cmd")) || t.contains(QLatin1String("command"))
        || t.contains(QChar(0x2318)))
        m |= Qt::ControlModifier;
    if (t.contains(QLatin1String("ctrl")) || t.contains(QLatin1String("control"))
        || t.contains(QChar(0x2303)))
        m |= mac ? Qt::MetaModifier : Qt::ControlModifier;
    if (!mac && (t.contains(QLatin1String("meta")) || t.contains(QLatin1String("win"))))
        m |= Qt::MetaModifier;
    return m;
}

/// Numeric modifier masks. Final Cut: NSEvent flags (Shift 1 << 17, Control 1 << 18, Option
/// 1 << 19, Command 1 << 20). Premiere: a small bit set, read as Ctrl 1, Shift 2, Alt 4,
/// Meta 8 (an assumption: unknown bits are ignored).
Qt::KeyboardModifiers modifierMask(qint64 mask, Format format)
{
    Qt::KeyboardModifiers m;
    if (format == Format::FinalCut || mask >= (1 << 16)) {
        if (mask & (1 << 17))
            m |= Qt::ShiftModifier;
        if (mask & (1 << 18))
            m |= Qt::MetaModifier;
        if (mask & (1 << 19))
            m |= Qt::AltModifier;
        if (mask & (1 << 20))
            m |= Qt::ControlModifier;
        return m;
    }
    if (mask & 1)
        m |= Qt::ControlModifier;
    if (mask & 2)
        m |= Qt::ShiftModifier;
    if (mask & 4)
        m |= Qt::AltModifier;
    if (mask & 8)
        m |= Qt::MetaModifier;
    return m;
}

/// A key field's value as a Qt key, or kNoKey. @p numericIsVirtual: a number is a Windows
/// virtual key (Premiere) instead of a Mac key code (Final Cut "keyCode").
int parseKey(const QString& value, Format format, bool numericIsVirtual, bool* keypad)
{
    *keypad = false;
    if (value.isEmpty())
        return kNoKey;
    int number = 0;
    if (value.size() > 1 && isNumber(value, &number))
        return numericIsVirtual ? virtualKey(number, keypad) : macKeyCode(number);
    if (value.size() == 1) // a character; a single digit is the digit key itself
        return characterKey(value.at(0), keypad);
    QString lower = value.toLower().trimmed();
    if (lower.startsWith(QLatin1String("num")) || lower.startsWith(QLatin1String("keypad"))) {
        lower.remove(QRegularExpression(QStringLiteral("^(numpad|num|keypad)\\s*")));
        if (lower.size() == 1 && lower.at(0).isDigit()) {
            *keypad = true;
            return Qt::Key_0 + lower.at(0).digitValue();
        }
    }
    lower.remove(QRegularExpression(QStringLiteral("^(vk_|key_?|kvk_)")));
    lower.remove(QLatin1Char(' '));
    if (lower.size() == 1)
        return characterKey(lower.at(0), keypad);
    return namedKey(lower, format);
}

// ---- A tiny element tree (enough for shortcut entries) ----

struct Node
{
    QString name; ///< lower case
    QHash<QString, QString> attributes; ///< lower-case names
    QString text;
    std::vector<std::unique_ptr<Node>> children;
};

std::unique_ptr<Node> readTree(QXmlStreamReader& xml)
{
    auto rootNode = std::make_unique<Node>();
    std::vector<Node*> stack {rootNode.get()};
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            auto node = std::make_unique<Node>();
            node->name = xml.name().toString().toLower();
            for (const QXmlStreamAttribute& a : xml.attributes())
                node->attributes.insert(a.name().toString().toLower(), a.value().toString());
            Node* raw = node.get();
            stack.back()->children.push_back(std::move(node));
            stack.push_back(raw);
        } else if (token == QXmlStreamReader::EndElement) {
            if (stack.size() > 1)
                stack.pop_back();
        } else if (token == QXmlStreamReader::Characters && !xml.isWhitespace()) {
            stack.back()->text += xml.text();
        }
    }
    return rootNode;
}

/// Field name sets (lower case).
const QStringList kCommandFields {QStringLiteral("commandname"), QStringLiteral("command"),
    QStringLiteral("commandid"), QStringLiteral("cmd"), QStringLiteral("action"),
    QStringLiteral("actionname"), QStringLiteral("name"), QStringLiteral("id")};
const QStringList kKeyFields {QStringLiteral("virtualkey"), QStringLiteral("vk"),
    QStringLiteral("keycode"), QStringLiteral("key"), QStringLiteral("keyname"),
    QStringLiteral("character"), QStringLiteral("characterstring"), QStringLiteral("char"),
    QStringLiteral("keyequivalent"), QStringLiteral("shortcut"), QStringLiteral("keys")};
const QStringList kModifierFields {QStringLiteral("modifiers"), QStringLiteral("modifier"),
    QStringLiteral("mods"), QStringLiteral("modifierflags"), QStringLiteral("modifiermask")};
const QStringList kModifierFlags {QStringLiteral("ctrl"), QStringLiteral("control"),
    QStringLiteral("shift"), QStringLiteral("alt"), QStringLiteral("option"), QStringLiteral("cmd"),
    QStringLiteral("meta")};

bool isTrue(const QString& v)
{
    const QString t = v.trimmed().toLower();
    return t == QLatin1String("true") || t == QLatin1String("1") || t == QLatin1String("yes");
}

class Reader
{
public:
    Reader(Format format, Result& out)
        : m_format(format)
        , m_out(out)
    { }

    /// One shortcut entry: @p fields holds lower-case names and their values.
    void entry(const QString& command, const QHash<QString, QString>& fields)
    {
        if (command.trimmed().isEmpty()) {
            skip(QStringLiteral("(no command name)"));
            return;
        }
        Qt::KeyboardModifiers mods;
        int key = kNoKey;
        bool keypad = false;
        for (const QString& f : kKeyFields) {
            const QString value = fields.value(f);
            if (value.isEmpty())
                continue;
            if (f == QLatin1String("shortcut") || f == QLatin1String("keys")
                || (value.size() > 2 && value.contains(QLatin1Char('+')))) {
                // A whole chord in one field: "Ctrl+Shift+K".
                const QKeySequence seq
                    = QKeySequence::fromString(value, QKeySequence::PortableText);
                if (!seq.isEmpty() && seq.count() == 1) {
                    key = seq[0].key();
                    mods = seq[0].keyboardModifiers();
                    break;
                }
                continue;
            }
            const bool virtualCodes = m_format == Format::Premiere;
            key = parseKey(value, m_format, virtualCodes, &keypad);
            if (key != kNoKey)
                break;
        }
        for (const QString& f : kModifierFields) {
            const QString value = fields.value(f);
            if (value.isEmpty())
                continue;
            int mask = 0;
            mods |= isNumber(value, &mask) ? modifierMask(mask, m_format)
                                           : modifierWords(value, m_format);
        }
        for (const QString& f : kModifierFlags) {
            if (isTrue(fields.value(f)))
                mods |= modifierWords(f, m_format);
        }
        if (keypad)
            mods |= Qt::KeypadModifier;
        if (key == kNoKey) {
            skip(command);
            return;
        }
        const QString action = actionFor(m_format, command);
        if (action.isEmpty()) {
            skip(command);
            return;
        }
        m_out.bindings.append(
            Binding {command, action, QKeySequence(QKeyCombination(mods, Qt::Key(key)))});
    }

    void skip(const QString& command)
    {
        ++m_out.skipped;
        m_out.skippedCommands.append(command);
    }

    // ---- Premiere: XML elements ----

    void visitXml(const Node& node)
    {
        QHash<QString, QString> fields = node.attributes;
        for (const auto& child : node.children) {
            if (child->children.empty())
                fields.insert(child->name, child->text.trimmed());
        }
        bool hasKey = false;
        for (const QString& f : kKeyFields)
            hasKey = hasKey || !fields.value(f).isEmpty();
        QString command;
        for (const QString& f : kCommandFields) {
            if (!fields.value(f).isEmpty()) {
                command = fields.value(f);
                break;
            }
        }
        if (hasKey) {
            entry(command, fields); // a key without a command name counts as skipped
            return; // its children were the fields
        }
        for (const auto& child : node.children)
            visitXml(*child);
    }

    // ---- Final Cut: property list values ----

    void visitPlist(const QVariant& value, const QString& context)
    {
        if (value.typeId() == QMetaType::QVariantMap) {
            const QVariantMap map = value.toMap();
            QHash<QString, QString> fields;
            bool hasKey = false;
            for (auto it = map.cbegin(); it != map.cend(); ++it) {
                if (it.value().typeId() == QMetaType::QVariantMap
                    || it.value().typeId() == QMetaType::QVariantList)
                    continue;
                const QString name = it.key().toLower();
                fields.insert(name, it.value().toString());
                hasKey = hasKey || (kKeyFields.contains(name) && !it.value().toString().isEmpty());
            }
            if (hasKey) {
                QString command;
                for (const QString& f : kCommandFields) {
                    if (!fields.value(f).isEmpty()) {
                        command = fields.value(f);
                        break;
                    }
                }
                entry(command.isEmpty() ? context : command, fields);
                return;
            }
            for (auto it = map.cbegin(); it != map.cend(); ++it)
                visitPlist(it.value(), it.key());
        } else if (value.typeId() == QMetaType::QVariantList) {
            for (const QVariant& v : value.toList())
                visitPlist(v, context);
        }
    }

private:
    Format m_format;
    Result& m_out;
};

/// Converts a property list element tree into QVariant values.
QVariant plistValue(const Node& node)
{
    if (node.name == QLatin1String("dict")) {
        QVariantMap map;
        QString key;
        for (const auto& child : node.children) {
            if (child->name == QLatin1String("key"))
                key = child->text;
            else if (!key.isNull()) {
                map.insert(key, plistValue(*child));
                key = QString();
            }
        }
        return map;
    }
    if (node.name == QLatin1String("array")) {
        QVariantList list;
        for (const auto& child : node.children)
            list.append(plistValue(*child));
        return list;
    }
    if (node.name == QLatin1String("true"))
        return QStringLiteral("true");
    if (node.name == QLatin1String("false"))
        return QStringLiteral("false");
    return node.text; // string, integer, real, date, data
}

} // namespace

bool isForeignKeymapFile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("kys") || suffix == QLatin1String("commandset");
}

QString actionForCommand(const QString& command)
{
    return actionFor(Format::Premiere, command);
}

bool readFile(const QString& path, Result& out, QString* error)
{
    out = Result {};
    const bool finalCut = QFileInfo(path).suffix().toLower() == QLatin1String("commandset");
    const Format format = finalCut ? Format::FinalCut : Format::Premiere;
    out.format = finalCut ? QStringLiteral("Final Cut Pro") : QStringLiteral("Premiere Pro");
    out.basedOn = finalCut ? QStringLiteral("final-cut-pro") : QStringLiteral("premiere-pro");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot open %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray data = file.readAll();
    if (data.startsWith("bplist")) {
        if (error)
            *error = QObject::tr("%1 is a binary property list. Convert it to XML first, for "
                                 "example with: plutil -convert xml1 \"%1\"")
                         .arg(QFileInfo(path).fileName());
        return false;
    }
    QXmlStreamReader xml(data);
    const std::unique_ptr<Node> tree = readTree(xml);
    if (xml.hasError() && tree->children.empty()) {
        if (error)
            *error = QObject::tr("%1 is not a %2 keyboard file: %3")
                         .arg(QFileInfo(path).fileName(), out.format, xml.errorString());
        return false;
    }
    if (xml.hasError())
        qCWarning(lcForeign) << path << "read up to an XML error:" << xml.errorString();
    Reader reader(format, out);
    const Node* top = tree->children.empty() ? nullptr : tree->children.front().get();
    if (top && top->name == QLatin1String("plist")) {
        for (const auto& child : top->children)
            reader.visitPlist(plistValue(*child), {});
    } else {
        reader.visitXml(*tree);
    }
    return true;
}

} // namespace kk::foreignkeys
