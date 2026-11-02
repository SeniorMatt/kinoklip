// SPDX-License-Identifier: GPL-3.0-or-later
// kk_makeproject -- writes a project with the given media files imported (and nothing on the
// timeline), for the nested Wayland session checks that need media pool items to drag.
//
//   kk_makeproject out.kkproj media...
#include "core/Project.h"

#include <QGuiApplication>
#include <QTextStream>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (args.size() < 2) {
        QTextStream(stderr) << "usage: kk_makeproject out.kkproj media...\n";
        return 2;
    }
    kk::Project project;
    project.importMedia(args.mid(1));
    QString error;
    if (!project.save(args.first(), &error)) {
        QTextStream(stderr) << "kk_makeproject: " << error << "\n";
        return 1;
    }
    return 0;
}
