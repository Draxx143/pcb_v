// main.cpp — GUI entry point.
#include <QApplication>
#include <QIcon>
#include <QStringList>

#include "Icons.h"
#include "MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("PCB Viewer");
    app.setApplicationDisplayName("PCB Viewer");
    app.setOrganizationName("pcbv");
    app.setApplicationVersion("0.3.0");
    app.setWindowIcon(pcbv::gui::appIcon());

    MainWindow window;
    window.show();

    const QStringList args = app.arguments();
    if (args.size() > 1) {
        const QString path = args.at(1);
        if (!path.isEmpty() && !path.startsWith('-')) window.openDocument(path);
    }
    return app.exec();
}
