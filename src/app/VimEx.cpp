// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/VimEx.h"

#include <QRegularExpression>

#include <algorithm>

namespace kk::vimex {

namespace {

QVector<Command> build()
{
    using A = Arg;
    // clang-format off
    return {
        {QStringLiteral("write"), 1, A::None, QStringLiteral("save the project")},
        {QStringLiteral("wall"), 2, A::None, QStringLiteral("save the project")},
        {QStringLiteral("quit"), 1, A::None, QStringLiteral("quit (asks to save; ! quits without saving)")},
        {QStringLiteral("qall"), 2, A::None, QStringLiteral("quit")},
        {QStringLiteral("wq"), 2, A::None, QStringLiteral("save and quit")},
        {QStringLiteral("xit"), 1, A::None, QStringLiteral("save and quit")},
        {QStringLiteral("wqall"), 3, A::None, QStringLiteral("save and quit")},
        {QStringLiteral("xall"), 2, A::None, QStringLiteral("save and quit")},
        {QStringLiteral("edit"), 1, A::Timeline, QStringLiteral("open a timeline by name, or a project file")},
        {QStringLiteral("enew"), 3, A::Text, QStringLiteral("new timeline (a name is optional)")},
        {QStringLiteral("new"), 3, A::Text, QStringLiteral("new timeline (a name is optional)")},
        {QStringLiteral("bnext"), 2, A::None, QStringLiteral("next timeline tab")},
        {QStringLiteral("bprevious"), 2, A::None, QStringLiteral("previous timeline tab")},
        {QStringLiteral("bNext"), 2, A::None, QStringLiteral("previous timeline tab")},
        {QStringLiteral("bdelete"), 2, A::None, QStringLiteral("close the timeline tab")},
        {QStringLiteral("buffer"), 1, A::Timeline, QStringLiteral("switch to timeline tab n or a name")},
        {QStringLiteral("ls"), 2, A::None, QStringLiteral("list the timeline tabs")},
        {QStringLiteral("buffers"), 7, A::None, QStringLiteral("list the timeline tabs")},
        {QStringLiteral("files"), 5, A::None, QStringLiteral("list the timeline tabs")},
        {QStringLiteral("page"), 2, A::Page, QStringLiteral("go to a page: media cut edit comp color audio deliver, next, prev, a number")},
        {QStringLiteral("undo"), 1, A::None, QStringLiteral("undo (a number: that many steps)")},
        {QStringLiteral("redo"), 3, A::None, QStringLiteral("redo (a number: that many steps)")},
        {QStringLiteral("history"), 3, A::None, QStringLiteral("open the edit history")},
        {QStringLiteral("render"), 3, A::Registry, QStringLiteral("go to Deliver (with --output: render to a file)")},
        {QStringLiteral("deliver"), 3, A::None, QStringLiteral("go to the Deliver page")},
        {QStringLiteral("play"), 4, A::None, QStringLiteral("play / pause")},
        {QStringLiteral("set"), 2, A::Option, QStringLiteral("options: set vim snap linked loop leader=")},
        {QStringLiteral("nmap"), 2, A::Text, QStringLiteral("map keys in NORMAL")},
        {QStringLiteral("nnoremap"), 2, A::Text, QStringLiteral("map keys in NORMAL")},
        {QStringLiteral("vmap"), 2, A::Text, QStringLiteral("map keys in VISUAL")},
        {QStringLiteral("vnoremap"), 2, A::Text, QStringLiteral("map keys in VISUAL")},
        {QStringLiteral("xmap"), 2, A::Text, QStringLiteral("map keys in VISUAL")},
        {QStringLiteral("xnoremap"), 2, A::Text, QStringLiteral("map keys in VISUAL")},
        {QStringLiteral("map"), 3, A::Text, QStringLiteral("map keys in NORMAL and VISUAL, or list the mappings")},
        {QStringLiteral("noremap"), 2, A::Text, QStringLiteral("map keys in NORMAL and VISUAL")},
        {QStringLiteral("nunmap"), 3, A::Text, QStringLiteral("remove a NORMAL mapping")},
        {QStringLiteral("vunmap"), 2, A::Text, QStringLiteral("remove a VISUAL mapping")},
        {QStringLiteral("xunmap"), 2, A::Text, QStringLiteral("remove a VISUAL mapping")},
        {QStringLiteral("unmap"), 3, A::Text, QStringLiteral("remove a mapping")},
        {QStringLiteral("source"), 2, A::File, QStringLiteral("read the vimrc again, or another file")},
        {QStringLiteral("help"), 1, A::Help, QStringLiteral("open the guide (a topic is searched)")},
        {QStringLiteral("cheat"), 2, A::None, QStringLiteral("open the Vim keys page of the guide")},
        {QStringLiteral("nohlsearch"), 3, A::None, QStringLiteral("clear the search")},
        {QStringLiteral("action"), 3, A::Action, QStringLiteral("run an action by id or name")},
        {QStringLiteral("export"), 3, A::Text, QStringLiteral("export: edl otio markers")},
        {QStringLiteral("import"), 3, A::Text, QStringLiteral("import: media timeline subtitle")},
    };
    // clang-format on
}

const QStringList kPages {QStringLiteral("media"), QStringLiteral("cut"), QStringLiteral("edit"),
    QStringLiteral("comp"), QStringLiteral("color"), QStringLiteral("audio"),
    QStringLiteral("deliver")};

QStringList filtered(const QStringList& list, const QString& prefix)
{
    QStringList out;
    for (const QString& s : list) {
        if (s.startsWith(prefix, Qt::CaseInsensitive))
            out.append(s);
    }
    out.removeDuplicates();
    return out;
}

} // namespace

const QVector<Command>& commands()
{
    static const QVector<Command> table = build();
    return table;
}

QStringList pageNames()
{
    return kPages;
}

QStringList panelNames()
{
    return {QStringLiteral("media"), QStringLiteral("effects"), QStringLiteral("inspector"),
        QStringLiteral("timeline"), QStringLiteral("viewer"), QStringLiteral("keyframes"),
        QStringLiteral("mixer"), QStringLiteral("nodes"), QStringLiteral("metadata"),
        QStringLiteral("index")};
}

QStringList optionNames()
{
    return {QStringLiteral("vim"), QStringLiteral("novim"), QStringLiteral("snap"),
        QStringLiteral("nosnap"), QStringLiteral("linked"), QStringLiteral("nolinked"),
        QStringLiteral("audioscrub"), QStringLiteral("noaudioscrub"), QStringLiteral("loop"),
        QStringLiteral("noloop"), QStringLiteral("leader="), QStringLiteral("timeoutlen="),
        QStringLiteral("physicalkeys"), QStringLiteral("nophysicalkeys")};
}

Parsed parse(const QString& rawLine)
{
    Parsed p;
    QString line = rawLine.trimmed();
    if (line.startsWith(QLatin1Char(':')))
        line = line.mid(1).trimmed();
    if (line.isEmpty())
        return p;
    // A go-to-time line: digits, a sign and digits, $, or a timecode.
    static const QRegularExpression gotoLine(
        QStringLiteral("^([+-]?\\d+|\\$|\\d{1,2}[:;.]\\d{1,2}([:;.]\\d{1,2}){0,2})$"));
    if (gotoLine.match(line).hasMatch()) {
        p.isGoto = true;
        p.name = QStringLiteral("goto");
        p.word = line;
        p.arg = line;
        return p;
    }
    // The word: letters, digits and dots ("marker", "w", "bn", "marker.add").
    static const QRegularExpression wordRx(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.]*"));
    const QRegularExpressionMatch m = wordRx.match(line);
    if (!m.hasMatch()) {
        p.word = line.section(QRegularExpression(QStringLiteral("\\s")), 0, 0);
        p.arg = line.mid(p.word.size()).trimmed();
        return p;
    }
    p.word = m.captured();
    QString rest = line.mid(p.word.size());
    if (rest.startsWith(QLatin1Char('!'))) {
        p.bang = true;
        rest = rest.mid(1);
    }
    p.arg = rest.trimmed();
    // A built-in command by exact name or abbreviation. Ambiguity is settled by table order,
    // as in Vim (the first command with that abbreviation wins).
    for (const Command& c : commands()) {
        if (p.word == c.name) {
            p.name = c.name;
            return p;
        }
    }
    for (const Command& c : commands()) {
        if (p.word.size() >= c.shortest && c.name.startsWith(p.word)) {
            p.name = c.name;
            return p;
        }
    }
    return p;
}

VimCompletion complete(const QString& before, const Sources& sources)
{
    VimCompletion out;
    const QString text = before;
    const qsizetype space = text.indexOf(QRegularExpression(QStringLiteral("\\s")));
    // The first word: ex commands, registry groups, and timecodes for a go-to line.
    if (space < 0) {
        out.start = 0;
        if (!text.isEmpty()
            && (text.at(0).isDigit() || text.at(0) == QLatin1Char('+')
                || text.at(0) == QLatin1Char('-'))) {
            if (sources.timecodes)
                out.candidates = filtered(sources.timecodes(), text);
            return out;
        }
        QStringList names;
        for (const Command& c : commands())
            names.append(c.name);
        if (sources.registryCommands) {
            for (const QString& r : sources.registryCommands())
                names.append(r.section(QLatin1Char(' '), 0, 0));
        }
        out.candidates = filtered(names, text);
        return out;
    }
    const Parsed p = parse(text);
    const QString rest = text.mid(space + 1);
    // The word being typed is the last one.
    const qsizetype lastSpace = text.lastIndexOf(QRegularExpression(QStringLiteral("\\s")));
    const QString current = text.mid(lastSpace + 1);
    out.start = int(lastSpace + 1);
    const Command* command = nullptr;
    for (const Command& c : commands()) {
        if (c.name == p.name)
            command = &c;
    }
    if (!command) {
        // A registry command: "marker " completes the second word, then the --options come
        // from the command line help (not listed here).
        if (sources.registryCommands && text.count(QLatin1Char(' ')) == 1) {
            QStringList seconds;
            for (const QString& r : sources.registryCommands()) {
                if (r.section(QLatin1Char(' '), 0, 0) == p.word && r.contains(QLatin1Char(' ')))
                    seconds.append(r.section(QLatin1Char(' '), 1));
            }
            out.candidates = filtered(seconds, current);
        }
        return out;
    }
    switch (command->arg) {
    case Arg::Action:
        if (sources.actionIds)
            out.candidates = filtered(sources.actionIds(), current);
        break;
    case Arg::Page: {
        QStringList pages = kPages;
        pages << QStringLiteral("next") << QStringLiteral("prev");
        out.candidates = filtered(pages, current);
        break;
    }
    case Arg::Panel:
        out.candidates = filtered(panelNames(), current);
        break;
    case Arg::Timeline:
        if (sources.timelines)
            out.candidates = filtered(sources.timelines(), current);
        break;
    case Arg::Option:
        out.candidates = filtered(optionNames(), current);
        break;
    case Arg::Help:
        if (sources.helpTopics)
            out.candidates = filtered(sources.helpTopics(), current);
        break;
    case Arg::Registry:
        out.candidates
            = filtered({QStringLiteral("--output"), QStringLiteral("--preset")}, current);
        break;
    case Arg::Text:
        if (p.name == QLatin1String("export"))
            out.candidates = filtered(
                {QStringLiteral("edl"), QStringLiteral("otio"), QStringLiteral("markers")},
                current);
        else if (p.name == QLatin1String("import"))
            out.candidates = filtered(
                {QStringLiteral("media"), QStringLiteral("timeline"), QStringLiteral("subtitle")},
                current);
        break;
    default:
        break;
    }
    Q_UNUSED(rest);
    return out;
}

} // namespace kk::vimex
