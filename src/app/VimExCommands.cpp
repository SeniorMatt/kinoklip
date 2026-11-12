// SPDX-License-Identifier: GPL-3.0-or-later
// The built-in ex commands of the `:` line (spec section 83), one function. The Neovim
// documents (cmdline.txt, editing.txt, windows.txt) are the model for the names; where a
// command means something else here (`:{n}` goes to frame n, `:q` quits the program, `:e`
// opens a timeline) the guide says so.
#include "app/MainWindow.h"
#include "app/ProjectController.h"
#include "app/VimController.h"
#include "app/VimEx.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimKeymap.h"
#include "ui/common/VimOptions.h"
#include "ui/help/Guide.h"
#include "ui/timeline/VimTimelineHost.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>

namespace kk {

namespace {

const char* pageActionId(const QString& page)
{
    if (page == QLatin1String("media"))
        return "view.page_media";
    if (page == QLatin1String("cut"))
        return "view.page_cut";
    if (page == QLatin1String("edit"))
        return "view.page_edit";
    if (page == QLatin1String("comp") || page == QLatin1String("compositing"))
        return "view.page_compositing";
    if (page == QLatin1String("color"))
        return "view.page_color";
    if (page == QLatin1String("audio"))
        return "view.page_audio";
    if (page == QLatin1String("deliver"))
        return "view.page_deliver";
    return nullptr;
}

} // namespace

QStringList VimController::timelineNames() const
{
    QStringList names;
    if (!m_ctx.project)
        return names;
    for (const quint64 uid : m_ctx.project->timelineUids()) {
        if (const auto data = m_ctx.project->timelineData(uid))
            names.append(data->name);
    }
    return names;
}

QStringList VimController::timecodes() const
{
    QStringList list;
    if (!m_ctx.project)
        return list;
    const TimelineData& data = m_ctx.project->timeline()->data();
    list.append(m_host->timecode(m_host->playhead()));
    for (const Marker& m : data.markers)
        list.append(m_host->timecode(m.frame));
    return list;
}

QString VimController::gotoTime(const QString& text, bool* ok)
{
    *ok = true;
    const TimelineData& data = m_ctx.project->timeline()->data();
    int64_t frame = m_host->playhead();
    static const QRegularExpression plain(QStringLiteral("^[+-]?\\d+$"));
    if (text == QLatin1String("$")) {
        frame = std::max<int64_t>(0, m_host->timelineEnd());
    } else if (plain.match(text).hasMatch()) {
        const int64_t n = text.toLongLong();
        frame = text.at(0) == QLatin1Char('+') || text.at(0) == QLatin1Char('-') ? frame + n : n;
    } else {
        const auto parsed = timecode::parse(text, data.fps);
        if (!parsed) {
            *ok = false;
            return tr("Not a timecode: %1").arg(text);
        }
        frame = timecode::recordToFrame(*parsed, data);
    }
    frame = std::max<int64_t>(0, frame);
    m_host->setPlayhead(frame);
    return m_host->timecode(frame);
}

QString VimController::runBuiltin(const vimex::Parsed& p, bool* ok)
{
    *ok = true;
    const auto fail = [&](const QString& message) {
        *ok = false;
        return message;
    };
    const auto action = [this](const char* id) -> QAction* {
        const QString name = QLatin1String(id);
        return m_ctx.actions && m_ctx.actions->contains(name) ? m_ctx.actions->action(name)
                                                              : nullptr;
    };
    const auto run = [&](const char* id, const QString& done = {}) -> QString {
        QAction* a = action(id);
        if (!a)
            return fail(tr("%1 is not available").arg(QLatin1String(id)));
        if (!a->isEnabled())
            return fail(tr("%1 is not available now").arg(a->text().remove(QLatin1Char('&'))));
        a->trigger();
        return done;
    };
    auto* main = qobject_cast<MainWindow*>(m_window);
    Project* project = m_ctx.project;
    const QString& name = p.name;

    if (p.isGoto)
        return gotoTime(p.arg, ok);

    // ---- files, quitting ----
    if (name == QLatin1String("write") || name == QLatin1String("wall"))
        return run("file.save", tr("saved"));
    if (name == QLatin1String("quit") || name == QLatin1String("qall")
        || name == QLatin1String("wq") || name == QLatin1String("xit")
        || name == QLatin1String("wqall") || name == QLatin1String("xall")) {
        const bool save = name != QLatin1String("quit") && name != QLatin1String("qall");
        if (save) {
            if (QAction* saveAction = action("file.save"))
                saveAction->trigger();
        }
        QAction* quit = action("file.quit");
        if (!quit)
            return fail(tr("Quit is not available"));
        if (p.bang && !save)
            m_window->setProperty("kkDiscardOnClose", true); // :q! closes without asking
        // Quit asks about unsaved changes as usual; run it after the line is closed.
        QTimer::singleShot(0, quit, &QAction::trigger);
        return tr("closing");
    }
    if (name == QLatin1String("edit")) {
        if (p.arg.isEmpty())
            return run("file.project_manager");
        // A timeline by name, else a project file.
        const QString wanted = p.arg;
        for (const quint64 uid : project->timelineUids()) {
            const auto data = project->timelineData(uid);
            if (data && data->name.compare(wanted, Qt::CaseInsensitive) == 0) {
                if (!project->openTab(uid))
                    return fail(tr("Cannot open %1 now").arg(wanted));
                return tr("\"%1\"").arg(data->name);
            }
        }
        const QString path = QDir::cleanPath(QDir::current().absoluteFilePath(wanted));
        if (QFileInfo::exists(path) && QFileInfo(path).isFile()) {
            if (!main)
                return fail(tr("Cannot open projects here"));
            QTimer::singleShot(
                0, main, [main, path] { main->projectController()->openFile(path); });
            return tr("opening %1").arg(QDir::toNativeSeparators(path));
        }
        return fail(tr("No timeline named %1").arg(wanted));
    }
    if (name == QLatin1String("enew") || name == QLatin1String("new")) {
        const QString title
            = p.arg.isEmpty() ? tr("Timeline %1").arg(project->timelineUids().size() + 1) : p.arg;
        const quint64 uid = project->createTimeline(title);
        if (!uid || !project->openTab(uid))
            return fail(tr("Cannot make a timeline now"));
        return tr("\"%1\" new timeline").arg(title);
    }

    // ---- the timelines are the buffers ----
    if (name == QLatin1String("bnext"))
        return run("timeline.next_tab");
    if (name == QLatin1String("bprevious") || name == QLatin1String("bNext"))
        return run("timeline.prev_tab");
    if (name == QLatin1String("bdelete"))
        return run("timeline.close_tab");
    if (name == QLatin1String("ls") || name == QLatin1String("buffers")
        || name == QLatin1String("files")) {
        QStringList cells;
        int n = 1;
        for (const quint64 uid : project->tabs()) {
            const auto data = project->timelineData(uid);
            const QString title = data ? data->name : QString();
            cells.append(uid == project->activeTimelineUid()
                    ? QStringLiteral("%1 %a \"%2\"").arg(n).arg(title)
                    : QStringLiteral("%1   \"%2\"").arg(n).arg(title));
            ++n;
        }
        return cells.join(QStringLiteral("  |  "));
    }
    if (name == QLatin1String("buffer")) {
        if (p.arg.isEmpty())
            return fail(tr("b wants a tab number or a timeline name"));
        bool numeric = false;
        const int n = p.arg.toInt(&numeric);
        const QVector<quint64>& tabs = project->tabs();
        quint64 uid = 0;
        if (numeric) {
            if (n < 1 || n > tabs.size())
                return fail(tr("No tab %1").arg(n));
            uid = tabs[n - 1];
        } else {
            int hits = 0;
            for (const quint64 u : project->timelineUids()) {
                const auto data = project->timelineData(u);
                if (!data)
                    continue;
                if (data->name.compare(p.arg, Qt::CaseInsensitive) == 0) {
                    uid = u;
                    hits = 1;
                    break;
                }
                if (data->name.contains(p.arg, Qt::CaseInsensitive)) {
                    uid = u;
                    ++hits;
                }
            }
            if (hits == 0)
                return fail(tr("No timeline matches %1").arg(p.arg));
            if (hits > 1)
                return fail(tr("More than one timeline matches %1").arg(p.arg));
        }
        if (uid != project->activeTimelineUid() && !project->openTab(uid))
            return fail(tr("Cannot switch now"));
        return {};
    }

    // ---- pages, panels ----
    if (name == QLatin1String("page")) {
        if (p.arg.isEmpty())
            return fail(tr("page wants media, cut, edit, comp, color, audio, deliver, next, prev "
                           "or a number"));
        bool numeric = false;
        const int n = p.arg.toInt(&numeric);
        const QStringList pages = vimex::pageNames();
        int target = -1;
        if (numeric)
            target = n - 1;
        else if (p.arg == QLatin1String("next") || p.arg == QLatin1String("prev")) {
            int current = 0;
            for (int i = 0; i < pages.size(); ++i) {
                QAction* a = action(pageActionId(pages[i]));
                if (a && a->isChecked())
                    current = i;
            }
            const int count = int(pages.size());
            target = (current + (p.arg == QLatin1String("next") ? 1 : count - 1)) % count;
        } else {
            target = int(pages.indexOf(p.arg));
            if (target < 0 && p.arg == QLatin1String("compositing"))
                target = int(pages.indexOf(QStringLiteral("comp")));
        }
        if (target < 0 || target >= pages.size())
            return fail(tr("No such page: %1").arg(p.arg));
        const QString done = run(pageActionId(pages[target]), tr("page %1").arg(pages[target]));
        if (*ok)
            afterPageChange();
        return done;
    }
    if (name == QLatin1String("panel") || name == QLatin1String("only")) {
        const QString what = name == QLatin1String("only") ? QStringLiteral("only") : p.arg;
        if (what.isEmpty())
            return fail(
                tr("panel wants a name: %1").arg(vimex::panelNames().join(QLatin1Char(' '))));
        if (!handleCommand(name == QLatin1String("only") ? QStringLiteral("win_only")
                                                         : QStringLiteral("win_panel"),
                1, false, what))
            return fail(tr("No such panel on this page: %1").arg(what));
        return {};
    }

    // ---- undo ----
    if (name == QLatin1String("undo") || name == QLatin1String("redo")) {
        const int times = p.arg.isEmpty() ? 1 : std::max(1, p.arg.toInt());
        const char* id = name == QLatin1String("undo") ? "edit.undo" : "edit.redo";
        QAction* a = action(id);
        if (!a)
            return fail(tr("%1 is not available").arg(name));
        for (int i = 0; i < times && a->isEnabled(); ++i)
            a->trigger();
        return {};
    }
    if (name == QLatin1String("history"))
        return run("edit.history_window");
    if (name == QLatin1String("play"))
        return run("transport.play_pause");

    // ---- render, import, export ----
    if (name == QLatin1String("deliver"))
        return run("view.page_deliver", tr("Deliver"));
    if (name == QLatin1String("render")) {
        if (p.arg.contains(QLatin1String("--"))) {
            // `render --output f.mp4 --preset ...` is the registry command of that name.
            return runCommand(QStringLiteral("render ") + p.arg, ok);
        }
        return run("view.page_deliver",
            tr("Deliver: choose a preset and add the job to the render queue"));
    }
    if (name == QLatin1String("export") || name == QLatin1String("import")) {
        static const QMap<QString, const char*> ids {
            {QStringLiteral("export edl"), "file.export_edl"},
            {QStringLiteral("export otio"), "file.export_otio"},
            {QStringLiteral("export markers"), "file.export_markers_csv"},
            {QStringLiteral("import media"), "file.import_media"},
            {QStringLiteral("import timeline"), "file.import_timeline"},
            {QStringLiteral("import subtitle"), "file.import_subtitle"}};
        const auto it = ids.find(name + QLatin1Char(' ') + p.arg);
        if (it == ids.end())
            return fail(name == QLatin1String("export")
                    ? tr("export wants edl, otio or markers")
                    : tr("import wants media, timeline or subtitle"));
        return run(*it);
    }

    // ---- help ----
    if (name == QLatin1String("help") || name == QLatin1String("cheat")) {
        const GuideArticle* article = nullptr;
        if (name == QLatin1String("cheat"))
            article = guide::index().article(QStringLiteral("vim-cheat-sheet"));
        else if (p.arg.isEmpty())
            article = guide::index().article(QStringLiteral("edit-with-vim-keys"));
        else if (const GuideArticle* exact = guide::index().article(p.arg))
            article = exact;
        else {
            const auto found = guide::index().search(p.arg);
            if (found.isEmpty())
                return fail(tr("No help for %1").arg(p.arg));
            article = found.first();
        }
        guide::openArticle(article, m_window);
        return article ? article->title : QString();
    }
    if (name == QLatin1String("nohlsearch")) {
        m_host->clearSearch();
        return {};
    }
    if (name == QLatin1String("action")) {
        if (p.arg.isEmpty())
            return fail(tr("action wants an action id or name"));
        QAction* found = nullptr;
        if (m_ctx.actions && m_ctx.actions->contains(p.arg)) {
            found = m_ctx.actions->action(p.arg);
        } else if (m_ctx.actions) {
            // By menu text; of several with that text the first that can run.
            for (QAction* a : m_ctx.actions->actions()) {
                QString text = a->text();
                text.remove(QLatin1Char('&'));
                text.remove(QChar(0x2026));
                if (text.trimmed().compare(p.arg, Qt::CaseInsensitive) != 0)
                    continue;
                if (!found || (!found->isEnabled() && a->isEnabled()))
                    found = a;
            }
        }
        if (!found)
            return fail(tr("No such action: %1").arg(p.arg));
        if (!found->isEnabled())
            return fail(tr("%1 is not available now").arg(p.arg));
        found->trigger();
        return {};
    }

    // ---- options and mappings ----
    if (name == QLatin1String("set")) {
        // The options that are Kinoklip's own editing toggles.
        struct Toggle
        {
            const char* word;
            const char* actionId;
        };
        static const Toggle toggles[]
            = {{"snap", "timeline.snapping"}, {"linked", "timeline.linked_selection"},
                {"audioscrub", "timeline.audio_scrubbing"}, {"loop", "transport.loop"}};
        QStringList words
            = p.arg.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        QStringList rest;
        QStringList report;
        for (const QString& word : std::as_const(words)) {
            bool matched = false;
            for (const Toggle& t : toggles) {
                const QString w = QLatin1String(t.word);
                QAction* a = action(t.actionId);
                if (!a)
                    continue;
                QString base = word;
                bool query = false;
                bool invert = false;
                if (base.endsWith(QLatin1Char('?'))) {
                    query = true;
                    base.chop(1);
                } else if (base.endsWith(QLatin1Char('!'))) {
                    invert = true;
                    base.chop(1);
                }
                const bool off = base.startsWith(QLatin1String("no")) && base.mid(2) == w;
                if (base != w && !off && base != QLatin1String("inv") + w)
                    continue;
                matched = true;
                const bool inverse = invert || base.startsWith(QLatin1String("inv"));
                if (!query) {
                    const bool want = inverse ? !a->isChecked() : !off;
                    if (a->isChecked() != want)
                        a->trigger();
                }
                report.append((a->isChecked() ? QString() : QStringLiteral("no")) + w);
                break;
            }
            if (matched)
                continue;
            // vim? leader? timeoutlen? are read-only queries.
            if (word == QLatin1String("vim?") || word == QLatin1String("novim?")) {
                report.append(vimopts::enabled() ? QStringLiteral("vim") : QStringLiteral("novim"));
            } else if (word == QLatin1String("leader?")) {
                report.append(
                    QStringLiteral("leader=") + (keymap() ? keymap()->leader() : QString()));
            } else if (word == QLatin1String("timeoutlen?")) {
                report.append(
                    QStringLiteral("timeoutlen=") + QString::number(vimopts::timeoutMs()));
            } else {
                rest.append(word);
            }
        }
        if (rest.isEmpty())
            return report.join(QStringLiteral("  "));
        if (rest.size() == 1
            && (rest[0] == QLatin1String("novim") || rest[0] == QLatin1String("vim"))) {
            if (QAction* option = action(vimopts::kActionId))
                option->setChecked(rest[0] == QLatin1String("vim")); // stores vim/enabled
            return rest[0] == QLatin1String("vim") ? tr("Vim mode on") : tr("Vim mode off");
        }
        if (!keymap())
            return fail(tr("No Vim keys"));
        bool good = true;
        const QString message
            = keymap()->executeLine(QStringLiteral("set ") + rest.join(QLatin1Char(' ')), &good);
        *ok = good;
        const QString all = (report + (message.isEmpty() ? QStringList() : QStringList {message}))
                                .join(QStringLiteral("  "));
        return good ? (all.isEmpty() ? tr("done") : all) : message;
    }
    if (name == QLatin1String("source")) {
        if (!keymap())
            return fail(tr("No Vim keys"));
        const QString path = p.arg.isEmpty() ? VimKeymap::vimrcPath() : p.arg;
        if (!keymap()->sourceVimrc(path))
            return fail(tr("No vimrc at %1").arg(QDir::toNativeSeparators(path)));
        const QStringList problems = keymap()->vimrcMessages();
        if (!problems.isEmpty())
            return fail(problems.first());
        return tr("vimrc read");
    }
    // :nmap and the rest of the mapping commands.
    {
        if (!keymap())
            return fail(tr("No Vim keys"));
        QString line
            = p.word + (p.bang ? QStringLiteral("!") : QString()) + QLatin1Char(' ') + p.arg;
        bool good = true;
        QString message = keymap()->executeLine(line.trimmed(), &good);
        *ok = good;
        if (!good)
            return message;
        return message.isEmpty() ? tr("done")
                                 : message.replace(QLatin1Char('\n'), QStringLiteral("  |  "));
    }
}

} // namespace kk
