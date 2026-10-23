// MainWindow.h — Qt6 Widgets front end for the Altium document reader.
#pragma once

#include <QMainWindow>
#include <QString>

#include "core/Model.h"

class QAction;
class QDragEnterEvent;
class QDropEvent;
class QLineEdit;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QTextBrowser;
class QPlainTextEdit;
class QDockWidget;
class QLabel;
class QTabWidget;
class QToolBar;

class BoardView;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    /// Loads a document (also used for the file passed on the command line).
    void openDocument(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void openFileDialog();
    void exportCsv();
    void exportJson();
    void exportHtml();
    void copyBomToClipboard();
    void exportSvg();
    void updateBoardOptions();
    void applyFilter(const QString& text);
    void showSelectedComponent();
    void showDocumentSummary();
    void selectComponentByIndex(int index);


private:
    void createActions();
    void createMenus();
    void createToolBar();
    void createDockPanels();
    void createComponentTable();
    void applyTheme();
    void populateComponents();
    void populateStructure();
    void log(const QString& message);
    void setDocument(const pcbv::Document& document);
    void ensureDocumentLoaded();
    void updateActionStates();
    QString componentDetailsHtml(const pcbv::Component& component) const;
    QString documentSummaryHtml() const;
    QString exportDirectory() const;

    pcbv::Document document_;
    bool hasDocument_ = false;

    QTabWidget* tabs_ = nullptr;
    BoardView* boardView_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QLabel* tableInfoLabel_ = nullptr;
    QTreeWidget* structureTree_ = nullptr;
    QTextBrowser* detailsView_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QDockWidget* structureDock_ = nullptr;
    QDockWidget* detailsDock_ = nullptr;
    QDockWidget* logDock_ = nullptr;

    QAction* openAction_ = nullptr;
    QAction* csvAction_ = nullptr;
    QAction* jsonAction_ = nullptr;
    QAction* htmlAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* summaryAction_ = nullptr;
    QAction* svgAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QAction* layersAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QAction* fitAction_ = nullptr;
    QAction* topLayerAction_ = nullptr;
    QAction* bottomLayerAction_ = nullptr;
    QAction* silkAction_ = nullptr;
    QAction* padsAction_ = nullptr;
    QAction* designatorsAction_ = nullptr;
    QAction* mechanicalAction_ = nullptr;
    QAction* exitAction_ = nullptr;
};
