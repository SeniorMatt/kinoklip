// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/VimController.h"

#include "app/PageBar.h"
#include "app/VimEx.h"
#include "app/VimModeLine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelFrame.h"
#include "ui/common/VimCommandLine.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimKeymap.h"
#include "ui/common/VimListHost.h"
#include "ui/common/VimOptions.h"
#include "ui/common/VimPicker.h"
#include "ui/common/VimWhichKey.h"
#include "ui/help/Guide.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/VimTimelineHost.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {

constexpr int kMessageMs = 4000;

/// The value of an option as the JSON type the parameter wants (the command line gives text).
QJsonValue typed(const commands::ParamSpec& spec, const QString& text)
{
    switch (spec.type) {
    case commands::ParamType::Integer:
        return QJsonValue(double(text.toLongLong()));
    case commands::ParamType::Number:
        return QJsonValue(text.toDouble());
    case commands::ParamType::Boolean:
        return QJsonValue(text != QLatin1String("false"));
    default:
        return QJsonValue(text);
    }
}

} // namespace

VimController::VimController(AppContext& ctx, TimelinePanel* timeline, QWidget* window,
    PageBar* pageBar, commands::LiveTarget target, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_window(window)
    , m_target(std::move(target))
    , m_pageBar(pageBar)
{
    m_host = new VimTimelineHost(ctx, timeline->controller());
    m_host->onCommandLine = [this] { showCommandLine(false); };
    m_host->onSearch = [this] {
        m_searchInList = false;
        showCommandLine(true);
    };
    m_host->onMessage = [this](const QString& text) { m_modeLine->showMessage(text, kMessageMs); };
    m_host->onEx = [this](const QString& line) {
        bool ok = true;
        const QString message = runCommand(line, &ok);
        showResult(message, ok);
    };
    m_input = new VimInput(ctx.actions, this);
    m_input->setHost(m_host);
    m_listHost = new VimListHost(ctx.actions);
    m_listHost->onCommandLine = [this] { showCommandLine(false); };
    m_listHost->onSearch = [this] { searchInList(); };
    m_listHost->onMessage
        = [this](const QString& text) { m_modeLine->showMessage(text, kMessageMs); };
    m_listHost->onEx = [this](const QString& line) {
        bool ok = true;
        const QString message = runCommand(line, &ok);
        showResult(message, ok);
    };
    m_listHost->onUpLevel = [this] { return upOneBin(); };
    m_listHost->onNewItem = [this] { return newBin(); };
    m_input->setListHost(m_listHost);
    if (qApp)
        qApp->installEventFilter(this);

    m_modeLine = new VimModeLine;
    m_pageBar->setLeadingWidget(m_modeLine);
    connect(m_input, &VimInput::statusChanged, this, [this] { updateModeLine(); });
    connect(m_input, &VimInput::displayChanged, this, [this] {
        updateModeLine();
        updateFocusFrame();
    });
    connect(m_input, &VimInput::activeChanged, this, [this] {
        m_host->showCursor(m_input->active());
        updateModeLine();
        updateFocusFrame();
        updateWhichKey();
    });
    m_whichKey = new VimWhichKey(window);
    connect(&m_input->machine(), &VimMachine::whichKeyChanged, this, [this] { updateWhichKey(); });

    m_line = new VimCommandLine;
    m_line->setObjectName(QStringLiteral("VimCommandLine"));
    m_line->setProperty(VimInput::kCommandLineProperty, true);
    m_line->setPlaceholderText(tr(": command (w, q, wq, or a Kinoklip command such as marker add)"));
    m_pageBar->setOverlayWidget(m_line);
    m_line->hide();
    {
        QSettings history;
        m_exHistory = history.value(QStringLiteral("vim/history")).toStringList();
        m_searchHistory = history.value(QStringLiteral("vim/searchHistory")).toStringList();
    }
    m_line->setCompleter([this](const QString& before, int) {
        if (m_searchLine)
            return VimCompletion {};
        vimex::Sources sources;
        sources.actionIds = [this] { return m_ctx.actions ? m_ctx.actions->ids() : QStringList(); };
        sources.registryCommands = [] {
            QStringList names;
            for (const commands::Command& c : commands::CommandRegistry::builtin().all())
                names.append(QString(c.spec.name).replace(QLatin1Char('.'), QLatin1Char(' ')));
            return names;
        };
        sources.timelines = [this] { return timelineNames(); };
        sources.timecodes = [this] { return timecodes(); };
        sources.helpTopics = [] {
            QStringList topics;
            for (const GuideArticle& a : guide::index().articles())
                topics.append(a.id);
            return topics;
        };
        return vimex::complete(before, sources);
    });
    m_wild = new QFrame(window);
    m_wild->setObjectName(QStringLiteral("VimWildMenu"));
    m_wild->setFrameShape(QFrame::StyledPanel);
    m_wild->setAutoFillBackground(true);
    m_wild->setAttribute(Qt::WA_ShowWithoutActivating);
    m_wild->setFocusPolicy(Qt::NoFocus);
    auto* wildLayout = new QVBoxLayout(m_wild);
    wildLayout->setContentsMargins(8, 3, 8, 3);
    m_wildText = new QLabel(m_wild);
    m_wildText->setTextFormat(Qt::RichText);
    m_wildText->setWordWrap(true);
    wildLayout->addWidget(m_wildText);
    m_wild->hide();
    connect(m_line, &VimCommandLine::candidatesChanged, this,
        [this](const QStringList& list, int index) {
            if (list.size() < 2) {
                m_wild->hide();
                return;
            }
            QStringList cells;
            for (int i = 0; i < list.size(); ++i) {
                const QString t = list[i].toHtmlEscaped();
                cells.append(i == index ? QStringLiteral("<b><u>%1</u></b>").arg(t) : t);
            }
            m_wildText->setText(cells.join(QStringLiteral("&nbsp;&nbsp; ")));
            placeWildMenu();
        });
    connect(m_line, &VimCommandLine::cancelled, this, [this] { closeCommandLine(); });
    connect(m_line, &QLineEdit::returnPressed, this, [this] {
        const QString text = m_line->text().trimmed();
        const bool search = m_searchLine;
        const bool inList = m_searchInList;
        m_line->addHistory(text);
        (search ? m_searchHistory : m_exHistory) = m_line->history();
        saveHistory();
        closeCommandLine();
        if (search) {
            if (!(inList ? m_listHost->search(text) : m_host->search(text)))
                m_modeLine->showError(tr("No match: %1").arg(text), kMessageMs);
            return;
        }
        bool ok = true;
        const QString message = runCommand(text, &ok);
        showResult(message, ok);
    });

    m_picker = new VimPicker(window);
    connect(m_picker, &VimPicker::openedChanged, this, [this](bool open) {
        m_input->machine().setCommandMode(open);
        updateModeLine();
    });
    m_input->machine().setCommandHandler(
        [this](const QString& name, int count, bool hasCount, const QString& arg) {
            return handleCommand(name, count, hasCount, arg);
        });

    // A settings import or the migration of the old Vim preset leaves a one-time notice.
    QSettings settings;
    if (settings.value(QLatin1String(vimopts::kMigrationNoticeKey)).toBool()) {
        settings.remove(QLatin1String(vimopts::kMigrationNoticeKey));
        m_modeLine->showMessage(tr("Vim keys are now an option: Preferences > Keyboard"), 15000);
    }
    m_host->showCursor(m_input->active());
    updateModeLine();
}

VimController::~VimController()
{
    if (qApp)
        qApp->removeEventFilter(this);
    if (m_framed)
        m_framed->setVimFocusFrame(false);
    delete m_host;
    delete m_listHost;
}

void VimController::updateModeLine()
{
    const bool on = m_input->active();
    m_modeLine->setVisible(on);
    if (!on)
        return;
    const QWidget* focus = QApplication::focusWidget();
    const VimMachine::Mode mode = m_input->displayMode(focus);
    QString key = QStringLiteral("NORMAL");
    if (mode == VimMachine::Mode::Visual)
        key = QStringLiteral("VISUAL");
    else if (mode == VimMachine::Mode::Insert)
        key = QStringLiteral("INSERT");
    else if (mode == VimMachine::Mode::Command)
        key = QStringLiteral("COMMAND");
    m_modeLine->setMode(
        mode == VimMachine::Mode::Visual ? m_input->machine().displayName()
                                         : VimMachine::modeName(mode),
        key);
    m_modeLine->setPending(m_input->machine().pending());
    QString panel;
    for (const QWidget* w = focus; w; w = w->parentWidget()) {
        if (const auto* frame = qobject_cast<const PanelFrame*>(w)) {
            panel = frame->title().toLower();
            break;
        }
    }
    m_modeLine->setPanel(panel);
}

VimKeymap* VimController::keymap() const
{
    return m_input->keymap();
}

void VimController::updateWhichKey()
{
    const VimMachine& machine = m_input->machine();
    if (!m_input->active() || !machine.whichKeyShown()) {
        m_whichKey->hideEntries();
        return;
    }
    m_whichKey->showEntries(
        machine.whichKeyPrefix(), machine.whichKeyEntries(), m_pageBar->height());
}

void VimController::updateFocusFrame()
{
    PanelFrame* frame = nullptr;
    if (m_input->active()) {
        for (QWidget* w = QApplication::focusWidget(); w; w = w->parentWidget()) {
            if ((frame = qobject_cast<PanelFrame*>(w)))
                break;
        }
    }
    if (frame == m_framed.data())
        return;
    if (m_framed) {
        m_framed->setVimFocusFrame(false);
        m_previousFrame = m_framed;
    }
    m_framed = frame;
    if (m_framed)
        m_framed->setVimFocusFrame(true);
}

void VimController::showCommandLine(bool search)
{
    m_searchLine = search;
    m_line->setPlaceholderText(
        search ? tr("/ search marker and clip names")
               : tr(": command (w, q, wq, or a Kinoklip command such as marker add)"));
    m_returnFocus = QApplication::focusWidget();
    m_input->machine().setCommandMode(true);
    m_line->setGeometry(m_pageBar->rect().adjusted(8, 3, -8, -3));
    m_line->setHistory(search ? m_searchHistory : m_exHistory);
    m_line->begin();
    m_line->show();
    m_line->raise();
    m_line->setFocus();
}

void VimController::closeCommandLine()
{
    if (!m_line->isVisible())
        return;
    m_line->hide();
    m_line->begin();
    m_wild->hide();
    m_searchLine = false;
    m_searchInList = false;
    m_input->machine().setCommandMode(false);
    if (m_returnFocus)
        m_returnFocus->setFocus(Qt::OtherFocusReason);
    m_returnFocus.clear();
}

void VimController::placeWildMenu()
{
    const QPoint top = m_pageBar->mapTo(m_window, QPoint(0, 0));
    m_wild->setFixedWidth(m_window->width() - 16);
    m_wild->adjustSize();
    m_wild->move(8, top.y() - m_wild->height() - 2);
    m_wild->raise();
    m_wild->show();
}

void VimController::showResult(const QString& message, bool ok)
{
    if (message.isEmpty())
        return;
    if (ok)
        m_modeLine->showMessage(message, kMessageMs);
    else
        m_modeLine->showError(message, kMessageMs);
}

QStringList VimController::history() const
{
    return m_exHistory;
}

void VimController::saveHistory() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("vim/history"), m_exHistory);
    settings.setValue(QStringLiteral("vim/searchHistory"), m_searchHistory);
}

QString VimController::runCommand(const QString& rawText, bool* okOut)
{
    bool ok = true;
    const auto done = [&](const QString& message, bool good) {
        ok = good;
        if (okOut)
            *okOut = good;
        return message;
    };
    QString text = rawText.trimmed();
    if (text.startsWith(QLatin1Char(':')))
        text = text.mid(1).trimmed();
    if (text.isEmpty())
        return done({}, true);

    const vimex::Parsed parsed = vimex::parse(text);
    if (!parsed.name.isEmpty()) {
        const QString message = runBuiltin(parsed, &ok);
        return done(message, ok);
    }

    // A registry command: "marker add --frame 120" (the same words as the command line).
    const QStringList words = QProcess::splitCommand(text);
    const commands::CommandRegistry& registry = commands::CommandRegistry::builtin();
    int consumed = 1;
    const commands::Command* command = nullptr;
    if (words.size() > 1)
        command = registry.find(words[0] + QLatin1Char('.') + words[1]);
    if (command)
        consumed = 2;
    else
        command = registry.find(words[0]);
    if (!command) {
        // Not a registry command: an action id typed bare (`:edit.undo`).
        if (m_ctx.actions && m_ctx.actions->contains(words[0])) {
            QAction* action = m_ctx.actions->action(words[0]);
            if (!action->isEnabled())
                return done(tr("%1 is not available now").arg(words[0]), false);
            action->trigger();
            return done({}, true);
        }
        return done(tr("Not a command: %1").arg(words[0]), false);
    }
    if (!command->spec.remoteAllowed) // the line runs commands through the live path
        return done(tr("%1 is not available here: use Preferences > Import and Export")
                        .arg(command->spec.name),
            false);
    const QVector<commands::ParamSpec> params = commands::CommandRegistry::fullParams(command->spec, true);
    QJsonObject call;
    for (qsizetype i = consumed; i < words.size(); ++i) {
        QString token = words[i];
        if (!token.startsWith(QLatin1String("--")))
            return done(tr("Options start with --: %1").arg(token), false);
        token = token.mid(2);
        QString value;
        bool hasValue = false;
        if (const qsizetype eq = token.indexOf(QLatin1Char('=')); eq >= 0) {
            value = token.mid(eq + 1);
            token = token.left(eq);
            hasValue = true;
        }
        const auto spec = std::find_if(params.begin(), params.end(),
            [&](const commands::ParamSpec& p) { return p.name == token; });
        if (spec == params.end())
            return done(tr("Unknown option --%1").arg(token), false);
        if (!hasValue && spec->type == commands::ParamType::Boolean) {
            value = QStringLiteral("true");
            hasValue = true;
        }
        if (!hasValue) {
            if (i + 1 >= words.size())
                return done(tr("Option --%1 needs a value").arg(token), false);
            value = words[++i];
        }
        if (spec->type == commands::ParamType::StringList) {
            QJsonArray list = call.value(token).toArray();
            list.append(value);
            call.insert(token, list);
        } else {
            call.insert(token, typed(*spec, value));
        }
    }
    const commands::CommandResult result
        = commands::CommandRegistry::builtin().executeLive(command->spec.name, call, m_target);
    if (!result.ok)
        return done(result.error, false);
    return done(result.text.isEmpty() ? tr("done") : result.text, true);
}

} // namespace kk
