// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/InputLutMenu.h"

#include "platform/FileDialogs.h"
#include "ui/color/ColorPalettes.h"

#include <QAction>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QMenu>

namespace kk::inputlut {

namespace {

/// Display name of a LUT: the file name without its extension.
QString lutName(const QString& path)
{
    return QFileInfo(path).completeBaseName();
}

} // namespace

void populate(QMenu* menu, const QString& current, QWidget* dialogParent,
    std::function<void(const QString& path)> pick)
{
    menu->setObjectName(QStringLiteral("InputLutMenu"));
    auto addEntry = [&](QMenu* into, const QString& text, const QString& path) {
        QAction* a = into->addAction(text);
        a->setCheckable(true);
        a->setChecked(path == current);
        a->setData(path);
        if (!path.isEmpty())
            a->setToolTip(path);
        QObject::connect(a, &QAction::triggered, menu, [pick, path] { pick(path); });
        return a;
    };
    addEntry(menu, QObject::tr("None"), QString());
    menu->addSeparator();

    bool inLibrary = false;
    bool any = false;
    for (const QString& folder : LutBrowser::folders()) {
        // Subfolders become submenus (a folder of hundreds of LUTs stays usable).
        QHash<QString, QMenu*> subs;
        QStringList files;
        QDirIterator it(folder, {QStringLiteral("*.cube"), QStringLiteral("*.CUBE")}, QDir::Files,
            QDirIterator::Subdirectories);
        while (it.hasNext())
            files.append(it.next());
        files.sort(Qt::CaseInsensitive);
        for (const QString& f : std::as_const(files)) {
            const QString rel = QDir(folder).relativeFilePath(f);
            const QString dir = QFileInfo(rel).path();
            QMenu* into = menu;
            if (dir != QLatin1String(".")) {
                into = subs.value(dir);
                if (!into) {
                    into = menu->addMenu(dir);
                    subs.insert(dir, into);
                }
            }
            addEntry(into, lutName(f), f);
            any = true;
            inLibrary = inLibrary || f == current;
        }
    }
    if (!any) {
        QAction* empty = menu->addAction(QObject::tr("(no .cube files in the LUT folders)"));
        empty->setEnabled(false);
    }
    if (!current.isEmpty() && !inLibrary) {
        menu->addSeparator();
        addEntry(menu, lutName(current), current);
    }
    menu->addSeparator();
    QAction* browse = menu->addAction(QObject::tr("Browse…"));
    browse->setObjectName(QStringLiteral("InputLutBrowse"));
    QObject::connect(browse, &QAction::triggered, menu, [pick, dialogParent] {
        const QString file = platform::getOpenFileName(dialogParent, QObject::tr("Load LUT"),
            QObject::tr("LUT files (*.cube)"), QStringLiteral("color/inputLutDir"),
            LutBrowser::defaultFolder());
        if (!file.isEmpty())
            pick(file);
    });
}

} // namespace kk::inputlut
