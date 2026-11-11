// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/VimController.h"

#include "app/PageBar.h"
#include "app/VimModeLine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelFrame.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimKeymap.h"
#include "ui/common/VimOptions.h"
#include "ui/common/VimWhichKey.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/VimTimelineHost.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

#include <algorithm>

namespace kk {

namespace {

constexpr int kMaxHistory = 50;
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
    m_host->onCommandLine = [this] { showCommandLine(); };
    m_host->onEx = [this](const QString& line) {
        const QString message = runCommand(line);
        if (!message.isEmpty())
            m_modeLine->showMessage(message, kMessageMs);
    };
    m_input = new VimInput(ctx.actions, this);
    m_input->setHost(m_host);

    m_modeLine = new VimModeLine;
    m_pageBar->setLeadingWidget(m_modeLine);
    connect(m_input, &VimInput::statusChanged, this, [this] { updateModeLine(); });
    connect(m_input, &VimInput::displayChanged, this, [this] {
        updateModeLine();
        updateFocusFrame();
    });
    connect(m_input, &VimInput::activeChanged, this, [this] {
        updateModeLine();
        updateFocusFrame();
        updateWhichKey();
    });
    m_whichKey = new VimWhichKey(window);
    connect(&m_input->machine(), &VimMachine::whichKeyChanged, this, [this] { updateWhichKey(); });

    m_line = new QLineEdit;
    m_line->setObjectName(QStringLiteral("VimCommandLine"));
    m_line->setProperty(VimInput::kCommandLineProperty, true);
    m_line->setPlaceholderText(tr(": command (w, q, wq, or a Kinoklip command such as marker add)"));
    m_pageBar->setOverlayWidget(m_line);
    m_line->hide();
    m_line->installEventFilter(this);
    connect(m_line, &QLineEdit::returnPressed, this, [this] {
        const QString text = m_line->text().trimmed();
        if (!text.isEmpty() && m_history.value(m_history.size() - 1) != text) {
            m_history.append(text);
            if (m_history.size() > kMaxHistory)
                m_history.removeFirst();
        }
        closeCommandLine();
        const QString message = runCommand(text);
        if (!message.isEmpty())
            m_modeLine->showMessage(message, kMessageMs);
    });

    // A settings import or the migration of the old Vim preset leaves a one-time notice.
    QSettings settings;
    if (settings.value(QLatin1String(vimopts::kMigrationNoticeKey)).toBool()) {
        settings.remove(QLatin1String(vimopts::kMigrationNoticeKey));
        m_modeLine->showMessage(tr("Vim keys are now an option: Preferences > Keyboard"), 15000);
    }
    updateModeLine();
}

VimController::~VimController()
{
    if (m_framed)
        m_framed->setVimFocusFrame(false);
    delete m_host;
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
    m_modeLine->setMode(VimMachine::modeName(mode), key);
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
    if (m_framed)
        m_framed->setVimFocusFrame(false);
    m_framed = frame;
    if (m_framed)
        m_framed->setVimFocusFrame(true);
}

void VimController::showCommandLine()
{
    m_returnFocus = QApplication::focusWidget();
    m_input->machine().setCommandMode(true);
    m_line->setGeometry(m_pageBar->rect().adjusted(8, 3, -8, -3));
    m_line->clear();
    m_historyPos = m_history.size();
    m_line->show();
    m_line->raise();
    m_line->setFocus();
}

void VimController::closeCommandLine()
{
    if (!m_line->isVisible())
        return;
    m_line->hide();
    m_line->clear();
    m_input->machine().setCommandMode(false);
    if (m_returnFocus)
        m_returnFocus->setFocus(Qt::OtherFocusReason);
    m_returnFocus.clear();
}

bool VimController::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_line && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape
            || (key->key() == Qt::Key_BracketLeft && key->modifiers() == Qt::ControlModifier)
            || (key->key() == Qt::Key_C && key->modifiers() == Qt::ControlModifier)) {
            closeCommandLine();
            return true;
        }
        if (key->key() == Qt::Key_Up && !m_history.isEmpty()) {
            m_historyPos = std::max(0, m_historyPos - 1);
            m_line->setText(m_history.value(m_historyPos));
            return true;
        }
        if (key->key() == Qt::Key_Down && !m_history.isEmpty()) {
            m_historyPos = std::min<int>(m_history.size(), m_historyPos + 1);
            m_line->setText(m_history.value(m_historyPos));
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

QString VimController::runCommand(const QString& rawText)
{
    QString text = rawText.trimmed();
    if (text.startsWith(QLatin1Char(':')))
        text = text.mid(1).trimmed();
    if (text.isEmpty())
        return {};
    auto action = [this](const char* id) -> QAction* {
        const QString name = QLatin1String(id);
        return m_ctx.actions && m_ctx.actions->contains(name) ? m_ctx.actions->action(name) : nullptr;
    };
    if (text == QLatin1String("w")) {
        if (QAction* save = action("file.save")) {
            save->trigger();
            return tr("saved");
        }
        return tr("Save is not available");
    }
    if (text == QLatin1String("q") || text == QLatin1String("wq") || text == QLatin1String("x")) {
        if (text != QLatin1String("q")) {
            if (QAction* save = action("file.save"))
                save->trigger();
        }
        if (QAction* quit = action("file.quit")) {
            // Quit asks about unsaved changes as usual; run it after the line is closed.
            QTimer::singleShot(0, quit, &QAction::trigger);
            return tr("closing");
        }
        return tr("Quit is not available");
    }

    // The Vim keys' own commands: :nmap, :nunmap, :map, :set, :source.
    {
        static const QStringList own {QStringLiteral("nmap"), QStringLiteral("nnoremap"),
            QStringLiteral("vmap"), QStringLiteral("vnoremap"), QStringLiteral("xmap"),
            QStringLiteral("xnoremap"), QStringLiteral("map"), QStringLiteral("noremap"),
            QStringLiteral("nunmap"), QStringLiteral("vunmap"), QStringLiteral("xunmap"),
            QStringLiteral("unmap"), QStringLiteral("set")};
        const QString first = text.section(QRegularExpression(QStringLiteral("\\s")), 0, 0);
        const QString rest = text.mid(first.size()).trimmed();
        if (first == QLatin1String("set")
            && (rest == QLatin1String("novim") || rest == QLatin1String("vim"))) {
            if (QAction* option = action(vimopts::kActionId))
                option->setChecked(rest == QLatin1String("vim")); // stores vim/enabled
            return rest == QLatin1String("vim") ? tr("Vim mode on") : tr("Vim mode off");
        }
        if (own.contains(first) && keymap()) {
            bool ok = true;
            QString message = keymap()->executeLine(text, &ok);
            if (!ok)
                return message;
            return message.isEmpty() ? tr("done")
                                     : message.replace(QLatin1Char('\n'), QStringLiteral("  |  "));
        }
        if (first == QLatin1String("source") || first == QLatin1String("so")) {
            if (!keymap())
                return tr("No Vim keys");
            const QString path = rest.isEmpty() ? VimKeymap::vimrcPath() : rest;
            if (!keymap()->sourceVimrc(path))
                return tr("No vimrc at %1").arg(QDir::toNativeSeparators(path));
            const QStringList problems = keymap()->vimrcMessages();
            return problems.isEmpty() ? tr("vimrc read") : problems.first();
        }
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
    if (!command)
        return tr("Not a command: %1").arg(words[0]);
    if (!command->spec.remoteAllowed) // the line runs commands through the live path
        return tr("%1 is not available here: use Preferences > Import and Export").arg(command->spec.name);
    const QVector<commands::ParamSpec> params = commands::CommandRegistry::fullParams(command->spec, true);
    QJsonObject call;
    for (qsizetype i = consumed; i < words.size(); ++i) {
        QString token = words[i];
        if (!token.startsWith(QLatin1String("--")))
            return tr("Options start with --: %1").arg(token);
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
            return tr("Unknown option --%1").arg(token);
        if (!hasValue && spec->type == commands::ParamType::Boolean) {
            value = QStringLiteral("true");
            hasValue = true;
        }
        if (!hasValue) {
            if (i + 1 >= words.size())
                return tr("Option --%1 needs a value").arg(token);
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
        return result.error;
    return result.text.isEmpty() ? tr("done") : result.text;
}

} // namespace kk
