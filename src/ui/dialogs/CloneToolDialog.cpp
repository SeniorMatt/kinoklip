// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/CloneToolDialog.h"

#include "core/Plural.h"
#include "platform/FileDialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace kk {

namespace {

constexpr char kDirKey[] = "mediaPage/cloneDir";

/// A list with its own add / remove buttons.
QWidget* listWithButtons(QListWidget* list, const QString& addFolder, const QString& addFile,
    const std::function<void()>& onFolder, const std::function<void()>& onFile)
{
    auto* box = new QWidget;
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(list, 1);
    auto* buttons = new QVBoxLayout;
    auto* folder = new QPushButton(addFolder);
    folder->setObjectName(QStringLiteral("cloneAddFolder"));
    buttons->addWidget(folder);
    QObject::connect(folder, &QPushButton::clicked, box, onFolder);
    if (!addFile.isEmpty()) {
        auto* file = new QPushButton(addFile);
        file->setObjectName(QStringLiteral("cloneAddFile"));
        buttons->addWidget(file);
        QObject::connect(file, &QPushButton::clicked, box, onFile);
    }
    auto* remove = new QPushButton(QObject::tr("Remove"));
    buttons->addWidget(remove);
    QObject::connect(
        remove, &QPushButton::clicked, list, [list] { qDeleteAll(list->selectedItems()); });
    buttons->addStretch(1);
    row->addLayout(buttons);
    return box;
}

} // namespace

CloneToolDialog::CloneToolDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("CloneToolDialog"));
    setWindowTitle(tr("Clone Tool"));
    resize(640, 560);
    auto* layout = new QVBoxLayout(this);

    auto* sourceGroup = new QGroupBox(tr("Source"));
    auto* sourceLayout = new QVBoxLayout(sourceGroup);
    m_sources = new QListWidget;
    m_sources->setObjectName(QStringLiteral("cloneSources"));
    m_sources->setSelectionMode(QAbstractItemView::ExtendedSelection);
    sourceLayout->addWidget(listWithButtons(
        m_sources, tr("Add Folder..."), tr("Add File..."),
        [this] {
            const QString dir = platform::getExistingDirectory(
                this, tr("Clone Tool: Source Folder"), QLatin1String(kDirKey));
            if (!dir.isEmpty())
                addSource(dir);
        },
        [this] {
            for (const QString& f : platform::getOpenFileNames(
                     this, tr("Clone Tool: Source Files"), QString(), QLatin1String(kDirKey)))
                addSource(f);
        }));
    layout->addWidget(sourceGroup);

    auto* destinationGroup = new QGroupBox(tr("Destination"));
    auto* destinationLayout = new QVBoxLayout(destinationGroup);
    m_destinations = new QListWidget;
    m_destinations->setObjectName(QStringLiteral("cloneDestinations"));
    m_destinations->setSelectionMode(QAbstractItemView::ExtendedSelection);
    destinationLayout->addWidget(listWithButtons(m_destinations, tr("Add Folder..."), QString(),
        [this] {
            const QString dir = platform::getExistingDirectory(
                this, tr("Clone Tool: Destination Folder"), QLatin1String(kDirKey));
            if (!dir.isEmpty())
                addDestination(dir);
        },
        {}));
    layout->addWidget(destinationGroup);

    auto* form = new QFormLayout;
    m_checksum = new QComboBox;
    m_checksum->setObjectName(QStringLiteral("cloneChecksum"));
    for (clone::Checksum c : {clone::Checksum::Md5, clone::Checksum::Sha1, clone::Checksum::Sha256,
             clone::Checksum::Crc32})
        m_checksum->addItem(clone::checksumName(c), int(c));
    form->addRow(tr("Checksum Type"), m_checksum);
    m_verify = new QCheckBox(tr("Verify copies after cloning"));
    m_verify->setObjectName(QStringLiteral("cloneVerify"));
    m_verify->setChecked(true);
    form->addRow(QString(), m_verify);
    layout->addLayout(form);

    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    layout->addWidget(m_progress);
    m_log = new QPlainTextEdit;
    m_log->setObjectName(QStringLiteral("cloneLog"));
    m_log->setReadOnly(true);
    layout->addWidget(m_log, 1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_clone = new QPushButton(tr("Clone"));
    m_clone->setObjectName(QStringLiteral("cloneStart"));
    m_cancel = new QPushButton(tr("Cancel"));
    m_cancel->setObjectName(QStringLiteral("cloneCancel"));
    auto* close = new QPushButton(tr("Close"));
    buttons->addWidget(m_clone);
    buttons->addWidget(m_cancel);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_clone, &QPushButton::clicked, this, [this] { start(); });
    connect(m_cancel, &QPushButton::clicked, this, [this] { cancel(); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    m_poll = new QTimer(this);
    m_poll->setInterval(100);
    connect(m_poll, &QTimer::timeout, this, &CloneToolDialog::pollJob);
    updateButtons();
}

CloneToolDialog::~CloneToolDialog()
{
    if (m_future.valid()) {
        m_cancelled = true;
        m_future.wait(); // the worker reads our atomics
    }
}

void CloneToolDialog::addSource(const QString& path)
{
    for (int i = 0; i < m_sources->count(); ++i)
        if (m_sources->item(i)->text() == path)
            return;
    m_sources->addItem(path);
    updateButtons();
}

void CloneToolDialog::addDestination(const QString& path)
{
    for (int i = 0; i < m_destinations->count(); ++i)
        if (m_destinations->item(i)->text() == path)
            return;
    m_destinations->addItem(path);
    updateButtons();
}

clone::Job CloneToolDialog::job() const
{
    clone::Job j;
    for (int i = 0; i < m_sources->count(); ++i)
        j.sources << m_sources->item(i)->text();
    for (int i = 0; i < m_destinations->count(); ++i)
        j.destinations << m_destinations->item(i)->text();
    j.checksum = clone::Checksum(m_checksum->currentData().toInt());
    j.verify = m_verify->isChecked();
    return j;
}

bool CloneToolDialog::start()
{
    if (running())
        return false;
    const clone::Job j = job();
    if (j.sources.isEmpty() || j.destinations.isEmpty()) {
        m_log->setPlainText(tr("Add a source and a destination first."));
        return false;
    }
    m_log->clear();
    m_done = 0;
    m_total = 1;
    m_cancelled = false;
    m_progress->setValue(0);
    m_future = std::async(std::launch::async, [this, j] {
        return clone::run(j, [this](qint64 done, qint64 total) {
            m_done = done;
            m_total = std::max<qint64>(total, 1);
            return !m_cancelled.load();
        });
    });
    m_poll->start();
    updateButtons();
    return true;
}

void CloneToolDialog::cancel()
{
    m_cancelled = true;
}

void CloneToolDialog::pollJob()
{
    m_progress->setValue(int(1000 * m_done.load() / std::max<qint64>(m_total.load(), 1)));
    if (!m_future.valid()
        || m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    m_poll->stop();
    m_result = m_future.get();
    m_future = {};
    QString text;
    if (m_result.cancelled)
        text = tr("Cancelled.");
    else if (!m_result.error.isEmpty())
        text = m_result.error;
    else if (m_result.ok)
        text = plural(m_result.files, tr("Cloned %n file; every copy matches its source."),
            tr("Cloned %n files; every copy matches its source."));
    else
        text = tr("Cloned with problems:");
    if (!m_result.ok && !m_result.failures.isEmpty())
        text += QLatin1Char('\n') + m_result.failures.join(QLatin1Char('\n'));
    m_log->setPlainText(text);
    if (m_result.ok)
        m_progress->setValue(1000);
    updateButtons();
    emit cloneFinished(m_result.ok);
}

void CloneToolDialog::updateButtons()
{
    const bool busy = running();
    m_clone->setEnabled(!busy && m_sources->count() > 0 && m_destinations->count() > 0);
    m_cancel->setEnabled(busy);
}

} // namespace kk
