#pragma once

#include <QMainWindow>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QDockWidget>
#include <QTextEdit>
#include <QTreeView>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow() = default;

private slots:
    void openFile();

private:
    void setupIndustrialTheme();
    void createDockPanels();
    void createCentralCanvas();
    void createMenus();
    void renderBoard(const class OleParser &parser);

    QGraphicsView *graphicsView;
    QGraphicsScene *scene;

    QDockWidget *projectDock;
    QDockWidget *propertiesDock;
    QDockWidget *aiChatDock;

    QTreeView *projectTreeView;
    QTextEdit *propertiesTextEdit;
    QTextEdit *aiChatLog;
};
