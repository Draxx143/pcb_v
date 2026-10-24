#include "MainWindow.h"
#include "OleParser.h"
#include <QVBoxLayout>
#include <QApplication>
#include <QPalette>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsLineItem>
#include <QGraphicsEllipseItem>
#include <QPen>
#include <QBrush>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("PCB AI Viewer (Linux Industrial Edition)");
    resize(1400, 900);

    setupIndustrialTheme();
    createCentralCanvas();
    createDockPanels();
    createMenus();
}

void MainWindow::setupIndustrialTheme() {
    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, QColor(43, 43, 43));
    darkPalette.setColor(QPalette::WindowText, QColor(212, 212, 212));
    darkPalette.setColor(QPalette::Base, QColor(30, 30, 30));
    darkPalette.setColor(QPalette::AlternateBase, QColor(45, 45, 45));
    darkPalette.setColor(QPalette::ToolTipBase, QColor(212, 212, 212));
    darkPalette.setColor(QPalette::ToolTipText, QColor(212, 212, 212));
    darkPalette.setColor(QPalette::Text, QColor(212, 212, 212));
    darkPalette.setColor(QPalette::Button, QColor(53, 53, 53));
    darkPalette.setColor(QPalette::ButtonText, QColor(212, 212, 212));
    darkPalette.setColor(QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);
    
    qApp->setPalette(darkPalette);
}

void MainWindow::createCentralCanvas() {
    scene = new QGraphicsScene(this);
    scene->setBackgroundBrush(QColor(20, 20, 20));
    
    graphicsView = new QGraphicsView(scene, this);
    graphicsView->setRenderHint(QPainter::Antialiasing);
    graphicsView->setDragMode(QGraphicsView::ScrollHandDrag);
    
    setCentralWidget(graphicsView);
}

void MainWindow::createDockPanels() {
    projectDock = new QDockWidget("Projects", this);
    projectTreeView = new QTreeView(projectDock);
    projectDock->setWidget(projectTreeView);
    addDockWidget(Qt::LeftDockWidgetArea, projectDock);

    propertiesDock = new QDockWidget("Properties", this);
    propertiesTextEdit = new QTextEdit(propertiesDock);
    propertiesTextEdit->setReadOnly(true);
    propertiesTextEdit->setPlaceholderText("Select a component or net to view properties...");
    propertiesDock->setWidget(propertiesTextEdit);
    addDockWidget(Qt::RightDockWidgetArea, propertiesDock);

    aiChatDock = new QDockWidget("AI Assistant & Troubleshooting", this);
    aiChatLog = new QTextEdit(aiChatDock);
    aiChatLog->setPlaceholderText("AI Engine placeholder. Ready for future integration...");
    aiChatDock->setWidget(aiChatLog);
    addDockWidget(Qt::BottomDockWidgetArea, aiChatDock);
}

void MainWindow::createMenus() {
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    
    QAction *openAction = fileMenu->addAction(tr("&Open PCB / Schematic..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);

    fileMenu->addSeparator();
    
    QAction *exitAction = fileMenu->addAction(tr("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &MainWindow::close);
}

void MainWindow::renderBoard(const OleParser &parser) {
    scene->clear();
    
    QPen trackPen(QColor(0, 200, 100), 1.5);
    QPen padPen(QColor(200, 180, 50), 1);
    QBrush padBrush(QColor(200, 180, 50));

    for (const auto &el : parser.getElements()) {
        if (el.type == "Track") {
            scene->addLine(QLineF(el.p1, el.p2), trackPen);
        } else if (el.type == "Pad") {
            scene->addEllipse(el.p1.x() - 3, el.p1.y() - 3, 6, 6, padPen, padBrush);
        }
    }

    if (!parser.getElements().isEmpty()) {
        graphicsView->setSceneRect(scene->itemsBoundingRect());
        graphicsView->fitInView(scene->itemsBoundingRect(), Qt::KeepAspectRatio);
    }
}

void MainWindow::openFile() {
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Open Altium File"), "",
        tr("Altium Files (*.PcbDoc *.SchDoc);;All Files (*)"));

    if (!fileName.isEmpty()) {
        QFileInfo fileInfo(fileName);
        
        OleParser parser(fileName);
        parser.parse();

        renderBoard(parser);

        QString infoText = QString("File Name: %1\nPath: %2\n\n--- Altium Parser Status ---\n%3")
                           .arg(fileInfo.fileName())
                           .arg(fileInfo.absoluteFilePath())
                           .arg(parser.getSummaryInfo());
        
        propertiesTextEdit->setText(infoText);
        aiChatLog->append(QString("System: Parsed and rendered layout for -> %1").arg(fileInfo.fileName()));
    }
}
