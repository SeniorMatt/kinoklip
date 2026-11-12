// SPDX-License-Identifier: GPL-3.0-or-later
// The pickers of <leader>f (spec section 83): what each one lists and what choosing does.
#include "app/VimController.h"
#include "app/VimModeLine.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimKeymap.h"
#include "ui/common/VimPicker.h"
#include "ui/effects/EffectActions.h"
#include "ui/help/Guide.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/VimTimelineHost.h"

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QTimer>

#include <algorithm>

namespace kk {

namespace {

QString plain(QString text)
{
    text.remove(QLatin1Char('&'));
    return text.trimmed();
}

MediaPoolPanel* mediaPoolOf(QWidget* window)
{
    MediaPoolPanel* any = nullptr;
    for (MediaPoolPanel* panel : window->findChildren<MediaPoolPanel*>()) {
        if (panel->isVisible())
            return panel;
        if (!any)
            any = panel;
    }
    return any;
}

QString binPath(const MediaPool& pool, const QString& binId)
{
    QStringList parts;
    QString id = binId;
    for (int guard = 0; !id.isEmpty() && guard < 32; ++guard) {
        const Bin* bin = pool.bin(id);
        if (!bin)
            break;
        parts.prepend(bin->name);
        id = bin->parentId;
    }
    return parts.isEmpty() ? QStringLiteral("Master") : parts.join(QLatin1Char('/'));
}

} // namespace

QVector<quint64> VimController::cursorClips() const
{
    const QVector<VimClipSpan> spans = m_host->clipSpans(m_host->cursorTrack());
    const int64_t at = m_host->playhead();
    for (const VimClipSpan& c : spans) {
        if (c.start <= at && at < c.end)
            return {c.id};
    }
    return m_host->selectedClips();
}

bool VimController::handleCommand(const QString& name, int count, bool hasCount, const QString& arg)
{
    Q_UNUSED(count);
    Q_UNUSED(hasCount);
    Q_UNUSED(arg);
    if (name.startsWith(QLatin1String("picker_"))) {
        QTimer::singleShot(0, this, [this, kind = name.mid(7)] { openPicker(kind); });
        return true;
    }
    return false;
}

bool VimController::openPicker(const QString& kind)
{
    Project* project = m_ctx.project;
    QVector<VimPickItem> items;
    VimPicker::Chosen chosen;
    QString title;
    const auto say = [this](const QString& text, bool ok = true) { showResult(text, ok); };

    if (kind == QLatin1String("media")) {
        title = tr("Find media");
        const MediaPool& pool = *project->mediaPool();
        for (const MediaItem& item : pool.items()) {
            if (item.isTimeline())
                continue;
            items.append({item.name, binPath(pool, item.binId), item.id, {}});
        }
        chosen = [this, say](const VimPickItem& item, bool) {
            MediaPoolPanel* panel = mediaPoolOf(m_window);
            if (!panel) {
                say(tr("No Media Pool here"), false);
                return;
            }
            if (const MediaItem* media = m_ctx.project->mediaPool()->item(item.id))
                panel->setCurrentBin(media->binId);
            panel->setSelectedMediaIds({item.id});
            emit panel->openInSourceViewer(item.id);
            say(tr("%1 in the Source viewer").arg(item.text));
        };
    } else if (kind == QLatin1String("commands")) {
        title = tr("Find command");
        if (m_ctx.actions) {
            for (const QString& id : m_ctx.actions->ids()) {
                QAction* a = m_ctx.actions->action(id);
                const QString text = a ? plain(a->text()) : QString();
                if (text.isEmpty() || a->menu())
                    continue;
                QString detail = id;
                const QString keys = a->shortcut().toString(QKeySequence::NativeText);
                if (!keys.isEmpty())
                    detail += QStringLiteral("    ") + keys;
                items.append({text, detail, id, {}});
            }
        }
        chosen = [this, say](const VimPickItem& item, bool) {
            if (!m_ctx.actions || !m_ctx.actions->contains(item.id))
                return;
            QAction* a = m_ctx.actions->action(item.id);
            if (!a->isEnabled())
                say(tr("%1 is not available now").arg(item.text), false);
            else
                a->trigger();
        };
    } else if (kind == QLatin1String("effects")) {
        title = tr("Find effect, transition, title or generator");
        for (const EffectDescriptor& d : EffectRegistry::instance().all()) {
            if (d.category == EffectCategory::ClipProperties)
                continue;
            items.append({d.name, EffectRegistry::categoryName(d.category), d.id,
                QVariant::fromValue(int(d.category))});
        }
        chosen = [this, say](const VimPickItem& item, bool alternative) {
            if (alternative) {
                say(tr("%1 (%2)").arg(item.text, item.detail));
                return;
            }
            const auto category = EffectCategory(item.data.toInt());
            Project& project = *m_ctx.project;
            if (category == EffectCategory::VideoEffect
                || category == EffectCategory::AudioEffect) {
                const QVector<quint64> clips = cursorClips();
                if (clips.isEmpty()) {
                    say(tr("No clip under the cursor"), false);
                    return;
                }
                const int n = effectactions::applyEffect(project, clips, item.id);
                if (n > 0)
                    say(tr("%1 applied").arg(item.text));
                else
                    say(tr("%1 does not fit this track").arg(item.text), false);
            } else if (category == EffectCategory::Transition
                || category == EffectCategory::AudioTransition) {
                // At the edit point under the cursor: the clip that starts there (its head),
                // else the one that ends there (its tail), else the nearer end of the clip
                // under the cursor.
                const QVector<VimClipSpan> spans = m_host->clipSpans(m_host->cursorTrack());
                const int64_t at = m_host->playhead();
                quint64 clip = 0;
                bool head = true;
                for (const VimClipSpan& c : spans) {
                    if (c.start == at) {
                        clip = c.id;
                        head = true;
                        break;
                    }
                }
                if (!clip) {
                    for (const VimClipSpan& c : spans) {
                        if (c.end == at) {
                            clip = c.id;
                            head = false;
                            break;
                        }
                    }
                }
                if (!clip) {
                    for (const VimClipSpan& c : spans) {
                        if (c.start < at && at < c.end) {
                            clip = c.id;
                            head = at - c.start <= c.end - at;
                            break;
                        }
                    }
                }
                if (!clip) {
                    say(tr("No clip or edit point under the cursor"), false);
                    return;
                }
                if (effectactions::applyTransition(project, clip, item.id, head))
                    say(tr("%1 added").arg(item.text));
                else
                    say(tr("%1 does not fit this track").arg(item.text), false);
            } else if (effectactions::activate(m_ctx, item.id)) {
                say(tr("%1 added").arg(item.text));
            } else {
                say(tr("%1 could not be added").arg(item.text), false);
            }
        };
    } else if (kind == QLatin1String("timelines")) {
        title = tr("Find timeline");
        for (const quint64 uid : project->timelineUids()) {
            const auto data = project->timelineData(uid);
            if (!data)
                continue;
            const int open = int(project->tabs().indexOf(uid)) + 1;
            items.append({data->name, open > 0 ? tr("tab %1").arg(open) : QString(),
                QString::number(uid), QVariant::fromValue(uid)});
        }
        chosen = [this, say](const VimPickItem& item, bool) {
            const quint64 uid = item.data.toULongLong();
            if (uid != m_ctx.project->activeTimelineUid() && !m_ctx.project->openTab(uid))
                say(tr("Cannot switch now"), false);
        };
    } else if (kind == QLatin1String("markers")) {
        title = tr("Find marker");
        for (const Marker& m : project->timeline()->data().markers) {
            items.append({m.name.isEmpty() ? tr("Marker") : m.name,
                m_host->timecode(m.frame)
                    + (m.note.isEmpty() ? QString() : QStringLiteral("  ") + m.note),
                QString::number(m.frame), QVariant::fromValue(qint64(m.frame))});
        }
        std::sort(items.begin(), items.end(), [](const VimPickItem& a, const VimPickItem& b) {
            return a.data.toLongLong() < b.data.toLongLong();
        });
        chosen = [this](const VimPickItem& item, bool) {
            m_host->setPlayhead(item.data.toLongLong());
        };
    } else if (kind == QLatin1String("bins")) {
        title = tr("Find bin");
        const MediaPool& pool = *project->mediaPool();
        items.append({QStringLiteral("Master"), QString(), QString(), {}});
        for (const Bin& b : pool.bins())
            items.append({binPath(pool, b.id), QString(), b.id, {}});
        chosen = [this, say](const VimPickItem& item, bool) {
            MediaPoolPanel* panel = mediaPoolOf(m_window);
            if (!panel) {
                say(tr("No Media Pool here"), false);
                return;
            }
            panel->setCurrentBin(item.id);
            say(item.text);
        };
    } else if (kind == QLatin1String("help")) {
        title = tr("Find help");
        for (const GuideArticle& a : guide::index().articles())
            items.append({a.title, a.id, a.id, {}});
        chosen = [this](const VimPickItem& item, bool) {
            guide::openArticle(guide::index().article(item.id), m_window);
        };
    } else if (kind == QLatin1String("keymaps")) {
        title = tr("Find Vim key");
        if (keymap()) {
            for (const VimBinding& b : keymap()->bindings()) {
                if (b.isGroup() || b.run.isEmpty())
                    continue;
                const QString modes = b.mode == QLatin1Char('n')
                    ? QString()
                    : QStringLiteral(" (") + QString(b.mode) + QLatin1Char(')');
                items.append({b.keys + modes + QStringLiteral("  ") + b.desc, b.context, QString(),
                    QVariant::fromValue(b.run)});
            }
        }
        chosen = [this, say](const VimPickItem& item, bool) {
            const QStringList run = item.data.toStringList();
            for (const QString& r : run) {
                const QString n = VimKeymap::normaliseRunItem(r);
                if (n.startsWith(QLatin1String("action:"))) {
                    if (m_ctx.actions && m_ctx.actions->contains(n.mid(7)))
                        m_ctx.actions->action(n.mid(7))->trigger();
                } else if (n.startsWith(QLatin1String("ex:"))) {
                    bool ok = true;
                    const QString message = runCommand(n.mid(3), &ok);
                    say(message, ok);
                } else {
                    say(tr("Type the keys: %1").arg(item.text.section(QLatin1String("  "), 0, 0)));
                    return;
                }
            }
        };
    } else if (kind == QLatin1String("history")) {
        title = tr("Command history");
        const QStringList lines = m_exHistory;
        for (qsizetype i = lines.size() - 1; i >= 0; --i)
            items.append({lines[i], QString(), lines[i], {}});
        chosen = [this, say](const VimPickItem& item, bool) {
            bool ok = true;
            const QString message = runCommand(item.text, &ok);
            say(message, ok);
        };
    } else {
        return false;
    }
    if (items.isEmpty()) {
        showResult(tr("Nothing to pick: %1").arg(title), false);
        return true;
    }
    m_picker->open(title, items, chosen);
    return true;
}

} // namespace kk
