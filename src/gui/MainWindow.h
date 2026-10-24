// MainWindow.h — Qt6 Widgets front end for the Altium document reader.
#pragma once

#include <QMainWindow>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <vector>

#include "core/Compare.h"
#include "core/Library.h"
#include "core/Model.h"
#include "core/Search.h"

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
class QMenu;
class QTabBar;
class QToolBar;

class BoardView;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    /// Loads a document into its own tab (also used for the command line file and drops).
    /// Opening an already open file just switches to it.
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
    void export3DImage();
    void exportGroupedBomCsv();
    void exportGroupedBomHtml();
    void exportNetlistCsv();
    void compareWithFile();
    void printBoard();
    void updateBoardOptions();
    void applyFilter(const QString& text);
    void applyBomFilter(const QString& text);
    void applyNetlistFilter(const QString& text);
    void selectNetlistItem();
    void showSelectedComponent();
    void showDocumentSummary();
    void selectComponentByIndex(int index);
    void closeCurrentDocument();
    void closeAllDocuments();


private:
    /// One open file: the parsed document plus the view state that belongs to it, so
    /// switching tabs brings the zoom, selection and filters back exactly as they were.
    struct DocumentSlot {
        pcbv::Document document;
        QString path;
        QString title;
        double zoom = 1.0;
        QPointF pan;
        int selectedComponent = -1;
        QString highlightNet;
        QString componentFilter;
        QString bomFilter;
        QString netlistFilter;
        int viewTab = 0;
        pcbv::Library library;     ///< footprints / symbols when this tab is a library
        int libraryItem = 0;       ///< selected footprint / symbol
        QString comparisonHtml;
        QString comparisonHint;
    };

    void createActions();
    void createMenus();
    void createToolBar();
    void createDockPanels();
    void createComponentTable();
    void applyTheme();
    void populateComponents();
    void populateBom();
    void populateNetlist();
    void zoomToSelectedPart();
    void showComparison(const pcbv::CompareResult& result);
    void populateStructure();
    void populateLayers();
    void selectNet(const QString& net);
    void highlightNetRow(const QString& net);
    QString netDetailsHtml(const pcbv::NetSummary& net) const;
    void log(const QString& message);
    void setDocument(const pcbv::Document& document);
    void activateDocument(int index);
    void attachLibraryModels();
    void closeDocument(int index);
    void captureViewState();
    void restoreViewState();
    void clearDocument();
    void updateDocumentTitle();
    void showSearchDialog();
    void showLibraryDocument();
    void populateLibrary();
    void selectLibraryItem(int index);
    void showSelectedLibraryItem();
    void applyLibraryFilter(const QString& text);
    QString libraryItemDetailsHtml(int index) const;
    void rememberRecent(const QString& path);
    void rebuildRecentMenu();
    void restoreSession();
    void saveSession();
    void ensureDocumentLoaded();
    void updateActionStates();
    QString componentDetailsHtml(const pcbv::Component& component) const;
    QString documentSummaryHtml() const;
    QString exportDirectory() const;

    pcbv::Document document_;      ///< the document of the active tab
    bool hasDocument_ = false;
    std::vector<DocumentSlot> slots_;
    int active_ = -1;
    QTabBar* docTabs_ = nullptr;
    bool switching_ = false;
    bool restoringSession_ = false;   ///< reopening last session: stay quiet on errors
    QStringList recentFiles_;         ///< most recently opened first
    QMenu* recentMenu_ = nullptr;

    QTabWidget* tabs_ = nullptr;
    QWidget* libraryPage_ = nullptr;
    QTableWidget* libraryTable_ = nullptr;
    QLabel* libraryInfo_ = nullptr;
    QLineEdit* libraryFilter_ = nullptr;
    BoardView* boardView_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTableWidget* bomTable_ = nullptr;
    QLabel* bomInfo_ = nullptr;
    QWidget* netlistPage_ = nullptr;
    QTreeWidget* netlistTree_ = nullptr;
    QLineEdit* netlistFilter_ = nullptr;
    QLabel* netlistInfo_ = nullptr;
    QTextBrowser* compareView_ = nullptr;
    QLabel* compareHint_ = nullptr;
    QWidget* comparePage_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QLabel* tableInfoLabel_ = nullptr;
    QTreeWidget* structureTree_ = nullptr;
    QTextBrowser* detailsView_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QDockWidget* structureDock_ = nullptr;
    QDockWidget* layersDock_ = nullptr;
    QTreeWidget* layersTree_ = nullptr;
    QLineEdit* bomFilter_ = nullptr;
    QDockWidget* detailsDock_ = nullptr;
    QDockWidget* logDock_ = nullptr;

    QAction* openAction_ = nullptr;
    QAction* csvAction_ = nullptr;
    QAction* jsonAction_ = nullptr;
    QAction* htmlAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* summaryAction_ = nullptr;
    QAction* svgAction_ = nullptr;
    QAction* threeDImageAction_ = nullptr;
    QAction* bomCsvAction_ = nullptr;
    QAction* netlistCsvAction_ = nullptr;
    QAction* measureAction_ = nullptr;
    QAction* threeDAction_ = nullptr;
    QAction* zoomPartAction_ = nullptr;
    QAction* printAction_ = nullptr;
    QAction* bomHtmlAction_ = nullptr;
    QAction* compareAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QAction* globalFindAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QAction* fitAction_ = nullptr;

    QAction* exitAction_ = nullptr;
    QAction* closeAction_ = nullptr;
    QAction* closeAllAction_ = nullptr;
    QAction* nextDocumentAction_ = nullptr;
    QAction* previousDocumentAction_ = nullptr;

    QString lastDirectory_;
    pcbv::CompareResult lastComparison_;
    QString lastComparisonHtml_;
};
