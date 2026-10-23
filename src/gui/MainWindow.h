// MainWindow.h — Qt6 Widgets front end for the Altium document reader.
#pragma once

#include <QMainWindow>
#include <QString>

#include "core/Compare.h"
#include "core/Model.h"

class QAction;
class QDragEnterEvent;
class QDropEvent;
class QCloseEvent;
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
    void closeEvent(QCloseEvent* event) override;

private slots:
    void openFileDialog();
    void exportCsv();
    void exportJson();
    void exportHtml();
    void copyBomToClipboard();
    void exportSvg();
    void exportGroupedBomCsv();
    void exportGroupedBomHtml();
    void compareWithFile();
    void updateBoardOptions();
    void applyFilter(const QString& text);
    void applyNetFilter(const QString& text);
    void applyBomFilter(const QString& text);
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
    void populateBom();
    void showComparison(const pcbv::CompareResult& result);
    void populateStructure();
    void populateLayers();
    void populateNets();
    void selectNet(const QString& net);
    void highlightNetRow(const QString& net);
    QString netDetailsHtml(const pcbv::NetSummary& net) const;
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
    QTableWidget* bomTable_ = nullptr;
    QLabel* bomInfo_ = nullptr;
    QTextBrowser* compareView_ = nullptr;
    QWidget* comparePage_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QLabel* tableInfoLabel_ = nullptr;
    QTreeWidget* structureTree_ = nullptr;
    QTextBrowser* detailsView_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QDockWidget* structureDock_ = nullptr;
    QDockWidget* layersDock_ = nullptr;
    QDockWidget* netsDock_ = nullptr;
    QTreeWidget* layersTree_ = nullptr;
    QTableWidget* netsTable_ = nullptr;
    QLineEdit* netFilter_ = nullptr;
    QLineEdit* bomFilter_ = nullptr;
    QLabel* netsInfo_ = nullptr;
    QDockWidget* detailsDock_ = nullptr;
    QDockWidget* logDock_ = nullptr;

    QAction* openAction_ = nullptr;
    QAction* csvAction_ = nullptr;
    QAction* jsonAction_ = nullptr;
    QAction* htmlAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* summaryAction_ = nullptr;
    QAction* svgAction_ = nullptr;
    QAction* bomCsvAction_ = nullptr;
    QAction* bomHtmlAction_ = nullptr;
    QAction* compareAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QAction* fitAction_ = nullptr;

    QAction* exitAction_ = nullptr;

    QString lastDirectory_;
    pcbv::CompareResult lastComparison_;
    QString lastComparisonHtml_;
};
