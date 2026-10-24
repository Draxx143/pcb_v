#include "MainWindow.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QSettings>
#include <QSignalBlocker>
#include <QPlainTextEdit>
#include <QPageLayout>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QPalette>
#include <QStyleFactory>
#include <QStringList>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <memory>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

#include "BoardView.h"
#include "Icons.h"
#include "core/LayerStyle.h"
#include "core/AltiumParser.h"
#include "core/Bom.h"
#include "core/Export.h"
#include "core/Netlist.h"
#include "core/Recent.h"

namespace {

QString qs(const std::string& value) { return QString::fromStdString(value); }

QString formatSize(std::uint64_t bytes) {
    if (bytes < 1024) return QString::number(bytes) + " B";
    if (bytes < 1024ull * 1024ull)
        return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 2) + " MB";
}

/// Table item that sorts by a hidden numeric key.
class NumericItem : public QTableWidgetItem {
public:
    explicit NumericItem(const QString& text, int value) : QTableWidgetItem(text) {
        setData(Qt::UserRole, value);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    bool operator<(const QTableWidgetItem& other) const override {
        return data(Qt::UserRole).toInt() < other.data(Qt::UserRole).toInt();
    }
};

/// Table item that sorts by a hidden floating point key (track lengths, mm).
class MeasureItem : public QTableWidgetItem {
public:
    MeasureItem(const QString& text, double value) : QTableWidgetItem(text) {
        setData(Qt::UserRole, value);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    bool operator<(const QTableWidgetItem& other) const override {
        return data(Qt::UserRole).toDouble() < other.data(Qt::UserRole).toDouble();
    }
};

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("PCB Viewer");
    resize(1500, 900);
    setAcceptDrops(true);
    setDockNestingEnabled(true);

    applyTheme();
    createComponentTable();
    createDockPanels();
    createActions();
    createMenus();
    createToolBar();
    updateActionStates();

    statusBar()->showMessage(
        tr("Open Altium .PcbDoc / .SchDoc files (Ctrl+O) or drop them here — one tab per file."));

    detailsView_->setHtml(
        "<div style='color:#9aa0a6;font-family:sans-serif;padding:18px'>"
        "<h2 style='color:#e6e6e6;margin-top:0'>No document loaded</h2>"
        "<p>Open an Altium <b>.PcbDoc</b> (PCB) or <b>.SchDoc</b> (schematic) file.</p>"
        "<p>Every component is listed with its designator, value/comment, footprint, "
        "manufacturer, part number, pins/pads and the complete raw parameter set.</p>"
        "</div>");

    // Window size, dock layout and the last folder are remembered between sessions.
    QSettings settings;
    lastDirectory_ = settings.value(QStringLiteral("ui/lastDirectory")).toString();
    const QByteArray geometry = settings.value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const QByteArray state = settings.value(QStringLiteral("ui/windowState")).toByteArray();
    if (!state.isEmpty()) restoreState(state);

    // Bring back the documents that were open when the app was closed.
    restoreSession();
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

void MainWindow::applyTheme() {
    qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // A complete dark palette matters as much as the style sheet: anything the sheet
    // does not mention (alternating tree rows, dialogs, scroll-area corners, ...)
    // falls back to the palette. Without it the tree showed white cells.
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0x1b, 0x1e, 0x24));
    palette.setColor(QPalette::WindowText, QColor(0xe3, 0xe6, 0xea));
    palette.setColor(QPalette::Base, QColor(0x17, 0x1a, 0x20));
    palette.setColor(QPalette::AlternateBase, QColor(0x1f, 0x23, 0x2a));
    palette.setColor(QPalette::ToolTipBase, QColor(0x23, 0x26, 0x2d));
    palette.setColor(QPalette::ToolTipText, QColor(0xe3, 0xe6, 0xea));
    palette.setColor(QPalette::Text, QColor(0xe3, 0xe6, 0xea));
    palette.setColor(QPalette::Button, QColor(0x23, 0x26, 0x2d));
    palette.setColor(QPalette::ButtonText, QColor(0xe3, 0xe6, 0xea));
    palette.setColor(QPalette::BrightText, QColor(0xff, 0x6b, 0x6b));
    palette.setColor(QPalette::Link, QColor(0x8a, 0xb4, 0xf8));
    palette.setColor(QPalette::Highlight, QColor(0x2a, 0x5d, 0x9f));
    palette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::PlaceholderText, QColor(0x7c, 0x84, 0x90));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6b, 0x72, 0x7c));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x6b, 0x72, 0x7c));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x6b, 0x72, 0x7c));
    qApp->setPalette(palette);

    qApp->setStyleSheet(R"(
QMainWindow, QDialog, QMessageBox { background: #1b1e24; }
QWidget { color: #e3e6ea; font-size: 13px; }
QMenuBar { background: #23262d; border-bottom: 1px solid #33383f; }
QMenuBar::item { padding: 5px 10px; background: transparent; }
QMenuBar::item:selected { background: #2f353e; border-radius: 4px; }
QMenu { background: #23262d; border: 1px solid #33383f; padding: 4px; }
QMenu::item { padding: 6px 22px; border-radius: 4px; }
QMenu::item:selected { background: #2a5d9f; }
QMenu::separator { height: 1px; background: #33383f; margin: 4px 8px; }
QToolBar { background: #23262d; border-bottom: 1px solid #33383f; spacing: 2px; padding: 4px 8px; }
QToolBar QToolButton { background: transparent; border: 0; border-radius: 5px; padding: 5px 6px; }
QToolBar QToolButton:hover { background: #2f353e; }
QToolBar QToolButton:pressed { background: #26476f; }
QToolBar QToolButton:checked { background: #2a5d9f; }
QToolBar QToolButton::menu-indicator { subcontrol-origin: padding; subcontrol-position: bottom right;
                                       width: 5px; height: 5px; right: 2px; bottom: 1px; }
QToolBar::separator { width: 1px; background: #33383f; margin: 5px 7px; }
QMenu::item:disabled { color: #6b727c; }
QMenu::item:checked { color: #ffffff; font-weight: 600; }
QTabWidget::pane { border: 1px solid #2c313a; background: #1b1e24; }
QTabBar::tab { background: #23262d; color: #b0bec5; padding: 6px 16px; border: 1px solid #2c313a;
               border-bottom: 0; border-top-left-radius: 5px; border-top-right-radius: 5px;
               margin-right: 2px; }
QTabBar::tab:selected { background: #2a5d9f; color: #ffffff; }
QTabBar::tab:hover { background: #2f353e; }
QTabBar::tab:selected:hover { background: #356bb5; }
QDockWidget { color: #cfd8dc; titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title { background: #23262d; padding: 6px 10px; border: 1px solid #2c313a; }
QTableWidget, QTableView { background: #171a20; alternate-background-color: #1f232a;
               gridline-color: #262b34; selection-background-color: #2a5d9f;
               selection-color: #ffffff; border: 1px solid #2c313a; }
QHeaderView::section { background: #242a33; color: #cfd8dc; padding: 6px; border: 0;
                       border-right: 1px solid #2c313a; border-bottom: 1px solid #2c313a; }
QLineEdit, QPlainTextEdit, QTextBrowser, QTextEdit, QTreeWidget, QTreeView, QListWidget {
                       background: #171a20; alternate-background-color: #1f232a;
                       border: 1px solid #2c313a; border-radius: 4px;
                       selection-background-color: #2a5d9f; selection-color: #ffffff; }
QLineEdit { padding: 6px 8px; }
QTreeWidget::item, QTreeView::item { padding: 3px 2px; }
QTreeWidget::item:alternate, QTreeView::item:alternate { background: #1f232a; }
QTreeWidget::item:hover, QTreeView::item:hover { background: #262c35; }
QTreeWidget::item:selected, QTreeView::item:selected { background: #2a5d9f; color: #ffffff; }
QCheckBox { spacing: 6px; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #3a414b; border-radius: 3px;
                       background: #171a20; }
QCheckBox::indicator:checked { background: #2a5d9f; border-color: #4c86d9; }
QPushButton { background: #262c35; border: 1px solid #3a414b; border-radius: 4px; padding: 5px 12px; }
QPushButton:hover { background: #2f353e; }
QPushButton:pressed { background: #2a5d9f; }
QStatusBar { background: #23262d; color: #9aa0a6; }
QSplitter::handle { background: #2c313a; }
QScrollBar:vertical { background: #171a20; width: 12px; margin: 0; }
QScrollBar::handle:vertical { background: #3a414b; border-radius: 6px; min-height: 24px; }
QScrollBar:horizontal { background: #171a20; height: 12px; }
QScrollBar::handle:horizontal { background: #3a414b; border-radius: 6px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::corner, QScrollArea::corner { background: #171a20; }
QToolTip { background: #23262d; color: #e3e6ea; border: 1px solid #3a414b; padding: 4px; }
)");
}

// ---------------------------------------------------------------------------
// Actions / menus
// ---------------------------------------------------------------------------

void MainWindow::createActions() {
    openAction_ = new QAction(pcbv::gui::openIcon(), tr("&Open PCB / Schematic…"), this);
    openAction_->setShortcut(QKeySequence::Open);
    openAction_->setToolTip(tr("Open an Altium .PcbDoc or .SchDoc document  (Ctrl+O)"));
    openAction_->setStatusTip(tr("Open an Altium .PcbDoc or .SchDoc document"));
    connect(openAction_, &QAction::triggered, this, &MainWindow::openFileDialog);

    csvAction_ = new QAction(tr("Export BOM as &CSV…"), this);
    csvAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    csvAction_->setToolTip(tr("BOM as CSV, ready for Excel  (Ctrl+Shift+C)"));
    connect(csvAction_, &QAction::triggered, this, &MainWindow::exportCsv);

    bomCsvAction_ = new QAction(tr("Export &grouped BOM as CSV…"), this);
    bomCsvAction_->setToolTip(tr("One line per unique part, with quantity and designators"));
    connect(bomCsvAction_, &QAction::triggered, this, &MainWindow::exportGroupedBomCsv);

    bomHtmlAction_ = new QAction(tr("Export grouped BOM as &HTML…"), this);
    bomHtmlAction_->setToolTip(tr("Printable grouped BOM for a supplier or a review"));
    connect(bomHtmlAction_, &QAction::triggered, this, &MainWindow::exportGroupedBomHtml);

    measureAction_ = new QAction(pcbv::gui::measureIcon(), tr("&Measure"), this);
    measureAction_->setCheckable(true);
    measureAction_->setShortcut(QKeySequence(QStringLiteral("M")));
    measureAction_->setToolTip(tr("Measure between two points, snapping to pads, vias and track "
                                  "ends  (M)"));
    connect(measureAction_, &QAction::toggled, this, [this](bool on) {
        if (boardView_ != nullptr) {
            if (on) boardView_->setThreeD(false);
            boardView_->setMeasureMode(on);
        }
        if (on) statusBar()->showMessage(tr("Measure: click two points, Esc to finish"), 6000);
    });

    threeDAction_ = new QAction(pcbv::gui::threeDIcon(), tr("3&D view"), this);
    threeDAction_->setCheckable(true);
    threeDAction_->setShortcut(QKeySequence(QStringLiteral("3")));
    threeDAction_->setToolTip(tr("Simple 3D preview of the board: drag to rotate, wheel to zoom  "
                                 "(3)"));
    connect(threeDAction_, &QAction::toggled, this, [this](bool on) {
        if (boardView_ == nullptr) return;
        if (on && measureAction_ != nullptr && measureAction_->isChecked()) {
            measureAction_->setChecked(false);
        }
        boardView_->setThreeD(on);
    });

    zoomPartAction_ = new QAction(tr("Zoom to selected &part"), this);
    zoomPartAction_->setShortcut(QKeySequence(QStringLiteral("Z")));
    zoomPartAction_->setToolTip(tr("Frame the part selected in the table  (Z)"));
    connect(zoomPartAction_, &QAction::triggered, this, &MainWindow::zoomToSelectedPart);

    netlistCsvAction_ = new QAction(tr("Export &netlist as CSV…"), this);
    netlistCsvAction_->setToolTip(tr("Every pad with its net, layer and position"));
    connect(netlistCsvAction_, &QAction::triggered, this, &MainWindow::exportNetlistCsv);

    printAction_ = new QAction(pcbv::gui::printIcon(), tr("&Print board (1:1)…"), this);
    printAction_->setShortcut(QKeySequence::Print);
    printAction_->setToolTip(tr("Print the board at its real size, or scaled to fit the page  "
                                "(Ctrl+P)"));
    connect(printAction_, &QAction::triggered, this, &MainWindow::printBoard);

    compareAction_ = new QAction(tr("&Compare with another document…"), this);
    compareAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    compareAction_->setToolTip(
        tr("Show what was added, removed or changed between two revisions  (Ctrl+Shift+D)"));
    connect(compareAction_, &QAction::triggered, this, &MainWindow::compareWithFile);

    jsonAction_ = new QAction(tr("Export as &JSON…"), this);
    jsonAction_->setToolTip(tr("Full document model as JSON"));
    connect(jsonAction_, &QAction::triggered, this, &MainWindow::exportJson);

    htmlAction_ = new QAction(tr("Export &HTML report…"), this);
    htmlAction_->setToolTip(tr("Printable HTML report of the document"));
    connect(htmlAction_, &QAction::triggered, this, &MainWindow::exportHtml);

    copyAction_ = new QAction(tr("&Copy component table"), this);
    copyAction_->setShortcut(QKeySequence::Copy);
    connect(copyAction_, &QAction::triggered, this, &MainWindow::copyBomToClipboard);

    summaryAction_ = new QAction(tr("Document s&ummary"), this);
    summaryAction_->setShortcut(QKeySequence(QStringLiteral("F5")));
    summaryAction_->setToolTip(tr("Show the document summary and statistics  (F5)"));
    connect(summaryAction_, &QAction::triggered, this, &MainWindow::showDocumentSummary);

    svgAction_ = new QAction(tr("Export board as S&VG…"), this);
    svgAction_->setToolTip(tr("Scalable drawing of the PCB layers"));
    connect(svgAction_, &QAction::triggered, this, &MainWindow::exportSvg);

    zoomInAction_ = new QAction(pcbv::gui::zoomInIcon(), tr("Zoom &in"), this);
    zoomInAction_->setShortcut(QKeySequence::ZoomIn);
    zoomInAction_->setToolTip(tr("Zoom in  (Ctrl++)"));
    connect(zoomInAction_, &QAction::triggered, this, [this]() {
        if (boardView_ != nullptr) boardView_->zoomIn();
    });

    zoomOutAction_ = new QAction(pcbv::gui::zoomOutIcon(), tr("Zoom &out"), this);
    zoomOutAction_->setShortcut(QKeySequence::ZoomOut);
    zoomOutAction_->setToolTip(tr("Zoom out  (Ctrl+-)"));
    connect(zoomOutAction_, &QAction::triggered, this, [this]() {
        if (boardView_ != nullptr) boardView_->zoomOut();
    });

    fitAction_ = new QAction(pcbv::gui::fitIcon(), tr("&Fit board to window"), this);
    fitAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    fitAction_->setToolTip(tr("Fit the whole board in the window  (Ctrl+0)"));
    connect(fitAction_, &QAction::triggered, this, [this]() {
        if (boardView_ != nullptr) boardView_->fitToWindow();
    });

    QMenu* exportMenu = new QMenu(tr("Export"), this);
    exportMenu->addAction(bomCsvAction_);
    exportMenu->addAction(bomHtmlAction_);
    exportMenu->addAction(netlistCsvAction_);
    exportMenu->addAction(csvAction_);
    exportMenu->addAction(jsonAction_);
    exportMenu->addAction(htmlAction_);
    exportMenu->addAction(svgAction_);

    exportAction_ = new QAction(pcbv::gui::exportIcon(), tr("&Export"), this);
    exportAction_->setToolTip(tr("Export the BOM, a report or the board drawing"));
    exportAction_->setMenu(exportMenu);

    closeAction_ = new QAction(tr("&Close document"), this);
    closeAction_->setShortcut(QKeySequence::Close);
    closeAction_->setToolTip(tr("Close the current document tab  (Ctrl+W)"));
    connect(closeAction_, &QAction::triggered, this, &MainWindow::closeCurrentDocument);

    closeAllAction_ = new QAction(tr("Close a&ll documents"), this);
    closeAllAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    connect(closeAllAction_, &QAction::triggered, this, &MainWindow::closeAllDocuments);

    nextDocumentAction_ = new QAction(tr("&Next document"), this);
    nextDocumentAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+PageDown")));
    connect(nextDocumentAction_, &QAction::triggered, this, [this]() {
        if (slots_.size() < 2) return;
        activateDocument((active_ + 1) % static_cast<int>(slots_.size()));
    });

    previousDocumentAction_ = new QAction(tr("&Previous document"), this);
    previousDocumentAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+PageUp")));
    connect(previousDocumentAction_, &QAction::triggered, this, [this]() {
        const int count = static_cast<int>(slots_.size());
        if (count < 2) return;
        activateDocument((active_ - 1 + count) % count);
    });

    exitAction_ = new QAction(tr("E&xit"), this);
    exitAction_->setShortcut(QKeySequence::Quit);
    connect(exitAction_, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::createMenus() {
    // File: open, one export submenu, quit.
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(openAction_);
    recentMenu_ = fileMenu->addMenu(tr("Open &recent"));
    recentMenu_->setToolTipsVisible(true);
    // The list changes while the app runs, so it is rebuilt each time the menu opens.
    connect(recentMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
    rebuildRecentMenu();
    fileMenu->addSeparator();
    fileMenu->addAction(closeAction_);
    fileMenu->addAction(closeAllAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(printAction_);
    fileMenu->addAction(compareAction_);
    fileMenu->addSeparator();
    QMenu* exportMenu = fileMenu->addMenu(tr("&Export"));
    exportMenu->addAction(bomCsvAction_);
    exportMenu->addAction(bomHtmlAction_);
    exportMenu->addAction(netlistCsvAction_);
    exportMenu->addAction(csvAction_);
    exportMenu->addAction(jsonAction_);
    exportMenu->addAction(htmlAction_);
    exportMenu->addAction(svgAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(exitAction_);

    // Edit: find and copy (the shortcuts live on these actions).
    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    findAction_ = new QAction(tr("&Find in components"), this);
    findAction_->setShortcut(QKeySequence::Find);
    connect(findAction_, &QAction::triggered, this, [this]() {
        if (filterEdit_ == nullptr) return;
        filterEdit_->setFocus();
        filterEdit_->selectAll();
    });
    globalFindAction_ = new QAction(tr("Find in all open &documents"), this);
    globalFindAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    globalFindAction_->setToolTip(
        tr("Search components, nets, layers and library items in every open tab  (Ctrl+Shift+F)"));
    connect(globalFindAction_, &QAction::triggered, this, &MainWindow::showSearchDialog);
    editMenu->addAction(findAction_);
    editMenu->addAction(globalFindAction_);
    editMenu->addAction(copyAction_);
    addAction(findAction_);
    addAction(globalFindAction_);

    // View: panel visibility, board fit, summary.
    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    QMenu* panelsMenu = viewMenu->addMenu(tr("&Panels"));
    if (structureDock_) panelsMenu->addAction(structureDock_->toggleViewAction());
    if (detailsDock_) panelsMenu->addAction(detailsDock_->toggleViewAction());
    if (layersDock_) panelsMenu->addAction(layersDock_->toggleViewAction());
    if (logDock_) panelsMenu->addAction(logDock_->toggleViewAction());
    viewMenu->addSeparator();
    viewMenu->addAction(fitAction_);
    viewMenu->addAction(nextDocumentAction_);
    viewMenu->addAction(previousDocumentAction_);
    viewMenu->addSeparator();
    viewMenu->addAction(zoomPartAction_);
    viewMenu->addAction(threeDAction_);
    viewMenu->addSeparator();
    viewMenu->addAction(summaryAction_);

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction* shortcutsAction = helpMenu->addAction(tr("&Keyboard shortcuts"));
    connect(shortcutsAction, &QAction::triggered, this, [this]() {
        QMessageBox::information(this, tr("Keyboard shortcuts"),
                                 tr("<table cellpadding='3'>"
                                    "<tr><td><b>Ctrl+O</b></td><td>open a document</td></tr>"
                                    "<tr><td><b>Ctrl+F</b></td><td>jump to the filter field</td></tr>"
                                    "<tr><td><b>Ctrl+Shift+F</b></td><td>find in every open document</td></tr>"
                                    "<tr><td><b>F5</b></td><td>document summary</td></tr>"
                                    "<tr><td><b>Ctrl+C</b></td><td>copy the component table</td></tr>"
                                    "<tr><td><b>Ctrl+Shift+C</b></td><td>export the BOM as CSV</td></tr>"
                                    "<tr><td><b>Ctrl++ / Ctrl+-</b></td><td>zoom the board</td></tr>"
                                    "<tr><td><b>Ctrl+0</b></td><td>fit the board in the window</td></tr>"
                                    "<tr><td><b>M</b></td><td>measure between two points</td></tr>"
                                    "<tr><td><b>3</b></td><td>3D preview (Esc returns)</td></tr>"
                                    "<tr><td><b>Z</b></td><td>zoom to the selected part</td></tr>"
                                    "<tr><td><b>Ctrl+P</b></td><td>print the board at 1:1</td></tr>"
                                    "<tr><td><b>Click a track or pad</b></td><td>highlight that net</td></tr>"
                                    "<tr><td><b>Esc</b></td><td>clear the net highlight</td></tr>"
                                    "<tr><td><b>Ctrl+Shift+D</b></td><td>compare with another document</td></tr>"
                                    "<tr><td><b>Ctrl+W</b></td><td>close the current document tab</td></tr>"
                                    "<tr><td><b>Ctrl+Shift+W</b></td><td>close every document</td></tr>"
                                    "<tr><td><b>Ctrl+PageDown / PageUp</b></td><td>next / previous document</td></tr>"
                                    "<tr><td><b>Ctrl+Q</b></td><td>quit</td></tr>"
                                    "</table>"));
    });
    QAction* aboutAction = helpMenu->addAction(tr("&About"));
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, tr("About PCB Viewer"),
                           tr("<h3>PCB Viewer %1</h3>"
                              "<p>Reads Altium <b>.PcbDoc</b> and <b>.SchDoc</b> files directly — both the "
                              "binary (OLE2) format and the ASCII export — and lists every component "
                              "with its full parameter set.</p>"
                              "<p>The <b>Board</b> tab draws copper, pads, vias and silkscreen from the "
                              "same data, so no Altium installation is required.</p>"
                              "<p><b>.PcbLib</b> footprint and <b>.SchLib</b> symbol libraries open as well: "
                              "the <b>Library</b> tab lists every item and each footprint can be "
                              "inspected on its own.</p>"
                              "<p>Coordinates are shown in millimetres (1 mil = 0.0254 mm).</p>")
                               .arg(QCoreApplication::applicationVersion()));
    });
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::createComponentTable() {
    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);

    // --- tab 1: component table ---------------------------------------------
    QWidget* componentPage = new QWidget(tabs_);
    QVBoxLayout* layout = new QVBoxLayout(componentPage);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    QHBoxLayout* filterRow = new QHBoxLayout();
    filterEdit_ = new QLineEdit(componentPage);
    filterEdit_->setPlaceholderText(tr("Filter by designator, value, footprint, manufacturer…"));
    filterEdit_->setClearButtonEnabled(true);
    tableInfoLabel_ = new QLabel(tr("no document"), componentPage);
    tableInfoLabel_->setStyleSheet("color:#8a929e;");
    tableInfoLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    filterRow->addWidget(filterEdit_, 1);
    filterRow->addWidget(tableInfoLabel_);
    layout->addLayout(filterRow);

    // No "#" column: the row number is meaningless once the table is sorted, and the
    // designator already identifies a part.
    table_ = new QTableWidget(0, 9, componentPage);
    table_->setHorizontalHeaderLabels({tr("Designator"), tr("Comment / Value"), tr("Footprint"),
                                       tr("Manufacturer"), tr("MPN"), tr("Pins"), tr("Pads"),
                                       tr("Layer"), tr("Description")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setSortingEnabled(true);
    table_->setShowGrid(false);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setColumnWidth(0, 110);
    table_->setColumnWidth(1, 170);
    table_->setColumnWidth(2, 240);
    table_->setColumnWidth(3, 170);
    table_->setColumnWidth(4, 170);
    table_->setColumnWidth(5, 60);
    table_->setColumnWidth(6, 60);
    table_->setColumnWidth(7, 90);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &MainWindow::showSelectedComponent);
    connect(filterEdit_, &QLineEdit::textChanged, this, &MainWindow::applyFilter);

    layout->addWidget(table_, 1);
    tabs_->addTab(componentPage, tr("Components"));

    // --- tab 2: grouped BOM --------------------------------------------------
    QWidget* bomPage = new QWidget(tabs_);
    QVBoxLayout* bomLayout = new QVBoxLayout(bomPage);
    bomLayout->setContentsMargins(8, 8, 8, 8);
    bomLayout->setSpacing(6);

    QHBoxLayout* bomFilterRow = new QHBoxLayout();
    bomFilter_ = new QLineEdit(bomPage);
    bomFilter_->setPlaceholderText(tr("Filter by value, designator, footprint or part number…"));
    bomFilter_->setClearButtonEnabled(true);
    bomInfo_ = new QLabel(tr("no document"), bomPage);
    bomInfo_->setStyleSheet("color:#8a929e;");
    bomInfo_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bomFilterRow->addWidget(bomFilter_, 1);
    bomFilterRow->addWidget(bomInfo_);
    bomLayout->addLayout(bomFilterRow);

    bomTable_ = new QTableWidget(0, 8, bomPage);
    bomTable_->setHorizontalHeaderLabels({tr("Qty"), tr("Designators"), tr("Value"), tr("Footprint"),
                                          tr("Manufacturer"), tr("MPN"), tr("Layers"), tr("Note")});
    bomTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    bomTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    bomTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    bomTable_->setAlternatingRowColors(true);
    bomTable_->setSortingEnabled(true);
    bomTable_->setShowGrid(false);
    bomTable_->verticalHeader()->setVisible(false);
    bomTable_->horizontalHeader()->setHighlightSections(false);
    bomTable_->horizontalHeader()->setStretchLastSection(true);
    bomTable_->setColumnWidth(0, 52);
    bomTable_->setColumnWidth(1, 220);
    bomTable_->setColumnWidth(2, 170);
    bomTable_->setColumnWidth(3, 230);
    bomTable_->setColumnWidth(4, 150);
    bomTable_->setColumnWidth(5, 160);
    bomTable_->setColumnWidth(6, 90);
    connect(bomFilter_, &QLineEdit::textChanged, this, &MainWindow::applyBomFilter);
    // Sorting rearranges the rows, so the filter has to be re-applied afterwards.
    connect(bomTable_->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
            [this](int, Qt::SortOrder) {
                applyBomFilter(bomFilter_ != nullptr ? bomFilter_->text() : QString());
            });
    bomLayout->addWidget(bomTable_, 1);
    tabs_->addTab(bomPage, tr("BOM"));

    // --- tab 3: netlist / connections ----------------------------------------
    netlistPage_ = new QWidget(tabs_);
    QVBoxLayout* netlistLayout = new QVBoxLayout(netlistPage_);
    netlistLayout->setContentsMargins(8, 8, 8, 8);
    netlistLayout->setSpacing(6);

    QHBoxLayout* netlistFilterRow = new QHBoxLayout();
    netlistFilter_ = new QLineEdit(netlistPage_);
    netlistFilter_->setPlaceholderText(tr("Filter by net, designator, pad or layer…"));
    netlistFilter_->setClearButtonEnabled(true);
    netlistInfo_ = new QLabel(tr("no document"), netlistPage_);
    netlistInfo_->setStyleSheet("color:#8a929e;");
    netlistInfo_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    netlistFilterRow->addWidget(netlistFilter_, 1);
    netlistFilterRow->addWidget(netlistInfo_);
    netlistLayout->addLayout(netlistFilterRow);

    netlistTree_ = new QTreeWidget(netlistPage_);
    netlistTree_->setHeaderLabels({tr("Net / connection"), tr("Pads"), tr("Tracks"), tr("Vias"),
                                   tr("Length (mm)"), tr("Layer"), tr("X (mm)"), tr("Y (mm)")});
    netlistTree_->setRootIsDecorated(true);
    netlistTree_->setUniformRowHeights(true);
    netlistTree_->setAlternatingRowColors(true);
    netlistTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    netlistTree_->setColumnWidth(0, 250);
    netlistTree_->setColumnWidth(1, 55);
    netlistTree_->setColumnWidth(2, 60);
    netlistTree_->setColumnWidth(3, 50);
    netlistTree_->setColumnWidth(4, 95);
    netlistTree_->setColumnWidth(5, 140);
    netlistTree_->setColumnWidth(6, 80);
    netlistTree_->setColumnWidth(7, 80);
    connect(netlistTree_, &QTreeWidget::itemSelectionChanged, this,
            &MainWindow::selectNetlistItem);
    connect(netlistTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (item == nullptr || boardView_ == nullptr) return;
                const QString net = item->data(0, Qt::UserRole).toString();
                if (!net.isEmpty()) selectNet(net);
                if (tabs_ != nullptr) tabs_->setCurrentWidget(boardView_);
            });
    connect(netlistFilter_, &QLineEdit::textChanged, this, &MainWindow::applyNetlistFilter);
    netlistLayout->addWidget(netlistTree_, 1);
    tabs_->addTab(netlistPage_, tr("Netlist"));
    tabs_->setTabVisible(tabs_->indexOf(netlistPage_), false);

    // --- tab 2: board drawing ------------------------------------------------
    // The page is always part of the tab widget and only its tab button is hidden
    // when a document has no geometry. Removing the page instead would reparent the
    // widget and leave it floating as a stray window.
    boardView_ = new BoardView(tabs_);
    connect(boardView_, &BoardView::componentClicked, this, &MainWindow::selectComponentByIndex);
    connect(boardView_, &BoardView::netClicked, this, &MainWindow::selectNet);
    connect(boardView_, &BoardView::threeDChanged, this, [this](bool on) {
        if (threeDAction_ == nullptr || threeDAction_->isChecked() == on) return;
        QSignalBlocker blocker(threeDAction_);
        threeDAction_->setChecked(on);
        if (on) statusBar()->showMessage(tr("3D view: drag to rotate, wheel to zoom, Esc to leave"),
                                         6000);
    });
    connect(boardView_, &BoardView::measureChanged, this, [this](const QString& text) {
        if (text.isEmpty()) {
            statusBar()->clearMessage();
        } else {
            statusBar()->showMessage(tr("Measure: %1").arg(text));
        }
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (table_ == nullptr || boardView_ == nullptr || !boardView_->hasGeometry()) return;
        const QTableWidgetItem* item = table_->item(row, 0);
        if (item == nullptr) return;
        const int index = item->data(Qt::UserRole + 1).toInt();
        if (index < 0) return;
        boardView_->setSelectedComponent(index);
        boardView_->zoomToComponent(index);
        if (tabs_ != nullptr) tabs_->setCurrentWidget(boardView_);
        statusBar()->showMessage(tr("Zoomed to %1").arg(item->text()), 4000);
    });
    tabs_->addTab(boardView_, tr("Board"));
    tabs_->setTabVisible(tabs_->indexOf(boardView_), false);

    // --- tab 4: revision comparison (only shown once two files were compared) --
    comparePage_ = new QWidget(tabs_);
    QVBoxLayout* compareLayout = new QVBoxLayout(comparePage_);
    compareLayout->setContentsMargins(8, 8, 8, 8);
    compareLayout->setSpacing(6);

    QHBoxLayout* compareHeader = new QHBoxLayout();
    compareHint_ = new QLabel(
        tr("Use <b>File → Compare with another document…</b> to diff this document against "
           "another revision."),
        comparePage_);
    compareHint_->setStyleSheet("color:#8a929e;");
    QPushButton* saveCompareButton = new QPushButton(tr("Save as HTML…"), comparePage_);
    compareHeader->addWidget(compareHint_, 1);
    compareHeader->addWidget(saveCompareButton);
    compareLayout->addLayout(compareHeader);

    compareView_ = new QTextBrowser(comparePage_);
    compareView_->setOpenExternalLinks(false);
    compareLayout->addWidget(compareView_, 1);
    connect(saveCompareButton, &QPushButton::clicked, this, [this]() {
        if (lastComparisonHtml_.isEmpty()) return;
        const QString suggested =
            exportDirectory() + "/" +
            QFileInfo(qs(document_.fileName)).completeBaseName() + "-comparison.html";
        const QString path = QFileDialog::getSaveFileName(this, tr("Save comparison"), suggested,
                                                         tr("HTML page (*.html);;All files (*)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, tr("Save failed"), tr("Cannot write:\n%1").arg(path));
            return;
        }
        file.write(lastComparisonHtml_.toUtf8());
        file.close();
        log(tr("Saved comparison: %1").arg(path));
    });
    tabs_->addTab(comparePage_, tr("Comparison"));
    tabs_->setTabVisible(tabs_->indexOf(comparePage_), false);

    // --- tab: library items (.PcbLib / .SchLib) ------------------------------
    libraryPage_ = new QWidget(tabs_);
    QVBoxLayout* libraryLayout = new QVBoxLayout(libraryPage_);
    libraryLayout->setContentsMargins(8, 8, 8, 8);
    libraryLayout->setSpacing(6);

    QHBoxLayout* libraryFilterRow = new QHBoxLayout();
    libraryFilter_ = new QLineEdit(libraryPage_);
    libraryFilter_->setPlaceholderText(tr("Filter by name or description…"));
    libraryFilter_->setClearButtonEnabled(true);
    libraryInfo_ = new QLabel(tr("no library"), libraryPage_);
    libraryInfo_->setStyleSheet("color:#8a929e;");
    libraryInfo_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    libraryFilterRow->addWidget(libraryFilter_, 1);
    libraryFilterRow->addWidget(libraryInfo_);
    libraryLayout->addLayout(libraryFilterRow);

    libraryTable_ = new QTableWidget(0, 6, libraryPage_);
    libraryTable_->setHorizontalHeaderLabels({tr("Name"), tr("Pads"), tr("Pins"), tr("Size (mm)"),
                                             tr("Height (mm)"), tr("Description")});
    libraryTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    libraryTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    libraryTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    libraryTable_->setAlternatingRowColors(true);
    libraryTable_->setSortingEnabled(true);
    libraryTable_->setShowGrid(false);
    libraryTable_->verticalHeader()->setVisible(false);
    libraryTable_->horizontalHeader()->setHighlightSections(false);
    libraryTable_->horizontalHeader()->setStretchLastSection(true);
    libraryTable_->setColumnWidth(0, 300);
    libraryTable_->setColumnWidth(1, 60);
    libraryTable_->setColumnWidth(2, 60);
    libraryTable_->setColumnWidth(3, 130);
    libraryTable_->setColumnWidth(4, 100);
    connect(libraryTable_, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::showSelectedLibraryItem);
    connect(libraryFilter_, &QLineEdit::textChanged, this, &MainWindow::applyLibraryFilter);
    libraryLayout->addWidget(libraryTable_, 1);
    tabs_->addTab(libraryPage_, tr("Library"));
    tabs_->setTabVisible(tabs_->indexOf(libraryPage_), false);

    // --- document tabs -------------------------------------------------------
    // Several files stay open at the same time; the tab bar picks which one the views
    // below (and the docks around them) show.
    docTabs_ = new QTabBar(this);
    docTabs_->setDocumentMode(true);
    docTabs_->setExpanding(false);
    docTabs_->setMovable(true);
    docTabs_->setTabsClosable(true);
    docTabs_->setUsesScrollButtons(true);
    docTabs_->setDrawBase(false);
    docTabs_->setVisible(false);
    connect(docTabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (switching_ || index < 0) return;
        activateDocument(index);
    });
    connect(docTabs_, &QTabBar::tabCloseRequested, this, &MainWindow::closeDocument);
    // Tabs can be dragged around; keep the document slots in the same order.
    connect(docTabs_, &QTabBar::tabMoved, this, [this](int from, int to) {
        const int count = static_cast<int>(slots_.size());
        if (from == to || from < 0 || to < 0 || from >= count || to >= count) return;
        DocumentSlot moved = slots_[static_cast<std::size_t>(from)];
        slots_.erase(slots_.begin() + from);
        slots_.insert(slots_.begin() + to, moved);
        if (active_ == from) {
            active_ = to;
        } else if (from < active_ && to >= active_) {
            --active_;
        } else if (from > active_ && to <= active_) {
            ++active_;
        }
        updateDocumentTitle();
    });

    QWidget* central = new QWidget(this);
    QVBoxLayout* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 4, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(docTabs_);
    centralLayout->addWidget(tabs_, 1);
    setCentralWidget(central);
}

void MainWindow::createToolBar() {
    QToolBar* toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName(QStringLiteral("mainToolBar"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(20, 20));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    // Icon text is what the "toolbar overflow" menu and tooltips fall back to, so the
    // buttons stay labelled even though only the glyph is drawn.
    struct Named { QAction* action; const char* name; };
    for (const Named& entry : {Named{openAction_, "Open"},
                               Named{zoomOutAction_, "Zoom out"},
                               Named{zoomInAction_, "Zoom in"},
                               Named{fitAction_, "Fit board"},
                               Named{measureAction_, "Measure"},
                               Named{threeDAction_, "3D view"},
                               Named{exportAction_, "Export"}}) {
        entry.action->setIconText(QString::fromLatin1(entry.name));
    }

    toolbar->addAction(openAction_);
    toolbar->addSeparator();
    toolbar->addAction(zoomOutAction_);
    toolbar->addAction(zoomInAction_);
    toolbar->addAction(fitAction_);
    toolbar->addAction(measureAction_);
    toolbar->addAction(threeDAction_);
    toolbar->addSeparator();
    toolbar->addAction(exportAction_);

    // Actions that own a menu must open it on a single click (the default is a
    // long-press "delayed popup", which feels broken for a toolbar button).
    for (QAction* action : {exportAction_}) {
        if (auto* button = qobject_cast<QToolButton*>(toolbar->widgetForAction(action))) {
            button->setPopupMode(QToolButton::InstantPopup);
        }
    }
}

void MainWindow::updateBoardOptions() {
    if (boardView_ == nullptr) return;
    pcbv::BoardRenderOptions options;
    if (layersTree_ != nullptr) {
        QTreeWidgetItemIterator it(layersTree_);
        while (*it != nullptr) {
            QTreeWidgetItem* item = *it;
            const int layer = item->data(0, Qt::UserRole).toInt();
            const QString extra = item->data(0, Qt::UserRole + 1).toString();
            const bool on = item->checkState(0) == Qt::Checked;
            if (!extra.isEmpty()) {
                if (extra == QLatin1String("pads")) options.pads = on;
                else if (extra == QLatin1String("designators")) options.designators = on;
                else if (extra == QLatin1String("silk")) options.silk = on;
            } else if (!on) {
                options.hiddenLayers.insert(layer);
            }
            ++it;
        }
    }
    boardView_->setRenderOptions(options);
}

void MainWindow::populateLayers() {
    if (layersTree_ == nullptr) return;
    layersTree_->clear();

    const std::set<int> hidden = pcbv::defaultHiddenLayers();
    for (const auto& usage : document_.layerUsage) {
        auto* item = new QTreeWidgetItem(layersTree_);
        item->setText(0, qs(usage.name));
        item->setText(1, QString::number(usage.primitives));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setData(0, Qt::UserRole, usage.layer);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, hidden.count(usage.layer) != 0 ? Qt::Unchecked : Qt::Checked);

        // Colour swatch: the same colour the board view paints this layer with.
        QPixmap swatch(12, 12);
        const std::uint32_t rgb = pcbv::layerColor(usage.layer);
        swatch.fill(QColor(static_cast<int>((rgb >> 16) & 0xFF),
                           static_cast<int>((rgb >> 8) & 0xFF), static_cast<int>(rgb & 0xFF)));
        item->setIcon(0, QIcon(swatch));
        item->setForeground(1, QColor(0x8a, 0x92, 0x9e));
        if (!usage.copper) item->setForeground(0, QColor(0xb0, 0xbe, 0xc5));
    }

    // Draw options that are not a layer but belong next to them. A schematic has no
    // board geometry, so the panel stays empty for those documents.
    struct Extra { const char* key; const char* label; bool on; };
    for (const Extra& extra : {Extra{"pads", QT_TR_NOOP("Pads"), true},
                               Extra{"designators", QT_TR_NOOP("Designators"), true},
                               Extra{"silk", QT_TR_NOOP("Silkscreen text"), true}}) {
        if (document_.geometry.empty()) break;
        auto* item = new QTreeWidgetItem(layersTree_);
        item->setText(0, tr(extra.label));
        item->setData(0, Qt::UserRole, -1);
        item->setData(0, Qt::UserRole + 1, QString::fromLatin1(extra.key));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, extra.on ? Qt::Checked : Qt::Unchecked);
        item->setForeground(0, QColor(0x8a, 0xb4, 0xf8));
    }

    layersTree_->resizeColumnToContents(0);
    updateBoardOptions();
}

void MainWindow::exportSvg() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    if (boardView_ == nullptr || !boardView_->hasGeometry()) {
        QMessageBox::information(this, tr("Nothing to draw"),
                                 tr("This document has no PCB geometry to export."));
        return;
    }
    const QString suggested = exportDirectory() + "/" +
                              QFileInfo(qs(document_.fileName)).completeBaseName() + ".svg";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export board drawing"), suggested,
        tr("SVG image (*.svg);;All files (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export failed"), tr("Cannot write:\n%1").arg(path));
        return;
    }
    const pcbv::BoardRenderOptions options =
        boardView_ != nullptr ? boardView_->renderOptions() : pcbv::BoardRenderOptions();
    file.write(QByteArray::fromStdString(pcbv::toSvg(document_, options)));
    file.close();
    log(tr("Exported board SVG: %1").arg(path));
    statusBar()->showMessage(tr("Exported %1").arg(path), 6000);
}

void MainWindow::exportGroupedBomCsv() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    const QString suggested = exportDirectory() + "/" +
                              QFileInfo(qs(document_.fileName)).completeBaseName() + "-bom.csv";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export grouped BOM"), suggested, tr("CSV file (*.csv);;All files (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export failed"), tr("Cannot write:\n%1").arg(path));
        return;
    }
    file.write(QByteArray::fromStdString(pcbv::bomToCsv(document_)));
    file.close();
    log(tr("Exported grouped BOM: %1 (%2)").arg(path, qs(pcbv::bomSummary(document_))));
    statusBar()->showMessage(tr("Exported %1").arg(path), 6000);
}

void MainWindow::exportGroupedBomHtml() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    const QString suggested = exportDirectory() + "/" +
                              QFileInfo(qs(document_.fileName)).completeBaseName() + "-bom.html";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export grouped BOM"), suggested, tr("HTML page (*.html);;All files (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export failed"), tr("Cannot write:\n%1").arg(path));
        return;
    }
    file.write(QByteArray::fromStdString(pcbv::bomToHtml(document_)));
    file.close();
    log(tr("Exported grouped BOM page: %1").arg(path));
    statusBar()->showMessage(tr("Exported %1").arg(path), 6000);
}

void MainWindow::exportNetlistCsv() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    const QString suggested =
        exportDirectory() + "/" + QFileInfo(qs(document_.fileName)).completeBaseName() + "-netlist.csv";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export netlist"), suggested, tr("CSV file (*.csv);;All files (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export failed"), tr("Cannot write:\n%1").arg(path));
        return;
    }
    file.write(QByteArray::fromStdString(pcbv::netlistToCsv(document_)));
    file.close();
    log(tr("Exported netlist: %1").arg(path));
    statusBar()->showMessage(tr("Exported %1").arg(path), 6000);
}

void MainWindow::printBoard() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    if (boardView_ == nullptr || !boardView_->hasGeometry() || !document_.bounds.valid) {
        QMessageBox::information(this, tr("Nothing to print"),
                                 tr("This document has no PCB geometry to print."));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(qs(document_.fileName));
    if (document_.bounds.width() > document_.bounds.height()) {
        printer.setPageOrientation(QPageLayout::Landscape);
    }

    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("Print board"));
    if (dialog.exec() != QDialog::Accepted) return;

    QPainter painter;
    if (!painter.begin(&printer)) {
        QMessageBox::warning(this, tr("Print failed"), tr("The printer could not be opened."));
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF page = printer.pageLayout().paintRectPixels(printer.resolution());
    painter.translate(page.topLeft());
    const double dpi = printer.resolution();
    const double devicePerMm = dpi / 25.4;

    const double boardWidthMm = document_.bounds.width() * 0.0254;
    const double boardHeightMm = document_.bounds.height() * 0.0254;
    const double marginX = 6.0 * devicePerMm;
    const double marginY = 6.0 * devicePerMm;
    const double captionHeight = 9.0 * devicePerMm;
    const double availableW = std::max(page.width() - marginX * 2.0, 10.0);
    const double availableH = std::max(page.height() - marginY * 2.0 - captionHeight, 10.0);

    // 1:1 unless the board is larger than the page; then scale down and say so.
    double mmScale = devicePerMm;
    QString scaleNote = tr("1:1");
    const double neededW = boardWidthMm * devicePerMm;
    const double neededH = boardHeightMm * devicePerMm;
    if (neededW > availableW || neededH > availableH) {
        const double factor = std::min(availableW / neededW, availableH / neededH);
        mmScale = devicePerMm * factor;
        scaleNote = tr("scaled to %1% of real size").arg(factor * 100.0, 0, 'f', 0);
    }

    QFont caption = painter.font();
    caption.setPointSizeF(9.0);
    painter.setFont(caption);
    painter.setPen(QColor(0x33, 0x37, 0x3d));
    const QString line = tr("%1  ·  board %2 × %3 mm  ·  %4")
                             .arg(qs(document_.fileName))
                             .arg(boardWidthMm, 0, 'f', 1)
                             .arg(boardHeightMm, 0, 'f', 1)
                             .arg(scaleNote);
    painter.drawText(QRectF(marginX, marginY, availableW, captionHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, line);

    const QPointF origin(marginX, marginY + captionHeight);
    boardView_->renderForPrint(painter, origin, mmScale);

    QFont footer = painter.font();
    footer.setPointSizeF(7.5);
    painter.setFont(footer);
    painter.setPen(QColor(0x6b, 0x72, 0x7c));
    painter.drawText(QRectF(marginX, page.height() - marginY + 1.0 * devicePerMm, availableW,
                            5.0 * devicePerMm),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     tr("Printed with PCB Viewer · %1")
                         .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm")));
    painter.end();

    log(tr("Printed %1 (%2)").arg(qs(document_.fileName), scaleNote));
    statusBar()->showMessage(tr("Sent to the printer — %1").arg(scaleNote), 6000);
}

void MainWindow::compareWithFile() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Compare with another document"), lastDirectory_,
        tr("Altium documents (*.PcbDoc *.pcbdoc *.SchDoc *.schdoc);;All files (*)"));
    if (path.isEmpty()) return;

    const pcbv::ParseOutcome outcome = pcbv::parseFile(path.toStdString());
    if (!outcome.ok) {
        QMessageBox::warning(this, tr("Cannot compare"),
                             tr("%1 could not be opened.\n\n%2")
                                 .arg(QFileInfo(path).fileName(), qs(outcome.error)));
        log(tr("ERROR  %1 — %2").arg(QFileInfo(path).fileName(), qs(outcome.error)));
        return;
    }
    if (outcome.document.type != document_.type) {
        QMessageBox::information(
            this, tr("Cannot compare"),
            tr("Both documents have to be the same kind — a board can only be compared with "
               "another board, a schematic with another schematic."));
        return;
    }

    lastComparison_ = pcbv::compareDocuments(document_, outcome.document);
    showComparison(lastComparison_);
    log(tr("Compared %1 → %2: %3")
            .arg(qs(document_.fileName), qs(outcome.document.fileName),
                 lastComparison_.identical()
                     ? tr("no differences")
                     : tr("%1 difference(s)").arg(lastComparison_.differenceCount())));
}

void MainWindow::showComparison(const pcbv::CompareResult& result) {
    if (tabs_ == nullptr || comparePage_ == nullptr || compareView_ == nullptr) return;
    lastComparisonHtml_ = qs(pcbv::compareToHtml(result));
    compareView_->setHtml(lastComparisonHtml_);
    if (compareHint_ != nullptr) {
        compareHint_->setText(tr("<b>%1</b> → <b>%2</b> · %3")
                                  .arg(qs(result.leftName).toHtmlEscaped(),
                                       qs(result.rightName).toHtmlEscaped(),
                                       result.identical()
                                           ? tr("no differences")
                                           : tr("%1 difference(s)").arg(
                                                 static_cast<int>(result.differenceCount()))));
    }

    const int index = tabs_->indexOf(comparePage_);
    tabs_->setTabVisible(index, true);
    tabs_->setCurrentIndex(index);

    statusBar()->showMessage(
        result.identical()
            ? tr("No differences between the two documents")
            : tr("%1 difference(s) found").arg(static_cast<int>(result.differenceCount())),
        8000);
}

void MainWindow::selectComponentByIndex(int index) {
    if (table_ == nullptr) return;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const QTableWidgetItem* item = table_->item(row, 0);
        if (item != nullptr && item->data(Qt::UserRole + 1).toInt() == index) {
            table_->selectRow(row);
            table_->scrollToItem(item);
            return;
        }
    }
}

void MainWindow::createDockPanels() {
    structureTree_ = new QTreeWidget(this);
    structureTree_->setHeaderLabels({tr("Stream / storage"), tr("Size")});
    structureTree_->setColumnWidth(0, 220);
    structureTree_->setAlternatingRowColors(true);

    structureDock_ = new QDockWidget(tr("Document structure"), this);
    structureDock_->setObjectName("structureDock");
    structureDock_->setWidget(structureTree_);
    structureDock_->setMinimumWidth(260);
    addDockWidget(Qt::LeftDockWidgetArea, structureDock_);

    detailsView_ = new QTextBrowser(this);
    detailsView_->setOpenExternalLinks(true);

    detailsDock_ = new QDockWidget(tr("Properties"), this);
    detailsDock_->setObjectName("detailsDock");
    detailsDock_->setWidget(detailsView_);
    detailsDock_->setMinimumWidth(380);
    addDockWidget(Qt::RightDockWidgetArea, detailsDock_);

    logView_ = new QPlainTextEdit(this);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(500);
    logView_->setStyleSheet("font-family: ui-monospace, Consolas, monospace; font-size: 12px;");

    layersTree_ = new QTreeWidget(this);
    layersTree_->setHeaderLabels({tr("Layer"), tr("Items")});
    layersTree_->setRootIsDecorated(false);
    layersTree_->setAlternatingRowColors(true);
    layersTree_->setUniformRowHeights(true);
    connect(layersTree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int) {
        updateBoardOptions();
    });

    layersDock_ = new QDockWidget(tr("Layers"), this);
    layersDock_->setObjectName("layersDock");
    layersDock_->setWidget(layersTree_);
    addDockWidget(Qt::LeftDockWidgetArea, layersDock_);

    logDock_ = new QDockWidget(tr("Parser log"), this);
    logDock_->setObjectName("logDock");
    logDock_->setWidget(logView_);
    addDockWidget(Qt::BottomDockWidgetArea, logDock_);
    resizeDocks({logDock_}, {140}, Qt::Vertical);
    logDock_->hide();  // diagnostics only: it shows itself when something goes wrong

    // Two narrow columns instead of three: Structure/Layers on the left, Properties on the
    // right. That hands the board and the tables the width they need.
    tabifyDockWidget(structureDock_, layersDock_);
    layersDock_->raise();
    structureDock_->setMinimumWidth(200);
    detailsDock_->setMinimumWidth(300);
    resizeDocks({structureDock_, detailsDock_}, {240, 330}, Qt::Horizontal);
}

// ---------------------------------------------------------------------------
// Document loading
// ---------------------------------------------------------------------------

void MainWindow::openFileDialog() {
    const QString startDir = lastDirectory_;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Open Altium documents"), startDir,
        tr("Altium documents and libraries (*.PcbDoc *.SchDoc *.PcbLib *.SchLib *.pcbdoc *.schdoc "
           "*.pcblib *.schlib);;All files (*)"));
    for (const QString& file : files) openDocument(file);
}

void MainWindow::openDocument(const QString& path) {
    const bool quiet = restoringSession_;  // silent reopen: no modal dialogs at start-up
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        if (quiet) {
            log(tr("warning: %1 is no longer there — skipped").arg(path));
            return;
        }
        QMessageBox::warning(this, tr("Open document"),
                             tr("File not found:\n%1").arg(path));
        return;
    }
    const pcbv::DocType type = pcbv::detectType(path.toStdString());
    if (type == pcbv::DocType::Unknown) {
        if (!quiet) {
            QMessageBox::warning(this, tr("Unknown file type"),
                                 tr("%1\n\nExpected an Altium .PcbDoc, .SchDoc, .PcbLib or .SchLib file.")
                                     .arg(info.fileName()));
        }
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pcbv::ParseOutcome outcome = pcbv::parseFile(path.toStdString());
    QApplication::restoreOverrideCursor();

    if (!outcome.ok) {
        QString details = qs(outcome.error);
        if (!outcome.detected.empty())
            details += tr("\n\nDetected: %1").arg(qs(outcome.detected));
        if (!outcome.signature.empty())
            details += tr("\nFirst bytes: %1").arg(qs(outcome.signature));
        if (!quiet) {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Critical);
            box.setWindowTitle(tr("Cannot read document"));
            box.setText(tr("%1 could not be opened.").arg(info.fileName()));
            box.setInformativeText(details);
            if (!outcome.detected.empty())
                box.setDetailedText(
                    tr("File: %1\nSize: %2 bytes\nDetected: %3\nSignature: %4\n\n%5")
                        .arg(info.absoluteFilePath())
                        .arg(info.size())
                        .arg(qs(outcome.detected), qs(outcome.signature), qs(outcome.error)));
            box.exec();
        }
        log(tr("ERROR  %1 — %2").arg(info.fileName(), qs(outcome.error)));
        if (!outcome.detected.empty()) log(tr("       detected: %1").arg(qs(outcome.detected)));
        return;
    }
    // Already open? Bring that tab forward instead of loading the file twice.
    const QString absolute = info.absoluteFilePath();
    rememberRecent(absolute);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].path == absolute) {
            activateDocument(static_cast<int>(i));
            return;
        }
    }

    DocumentSlot slot;
    slot.document = outcome.document;
    slot.library = outcome.library;
    slot.path = absolute;
    slot.title = info.fileName();
    int counter = 1;
    while (std::any_of(slots_.begin(), slots_.end(),
                       [&slot](const DocumentSlot& other) { return other.title == slot.title; })) {
        slot.title = QStringLiteral("%1 (%2)").arg(info.fileName()).arg(++counter);
    }
    slots_.push_back(slot);

    switching_ = true;
    docTabs_->addTab(slots_.back().title);
    docTabs_->setTabToolTip(docTabs_->count() - 1, absolute);
    docTabs_->setVisible(true);
    docTabs_->setCurrentIndex(static_cast<int>(slots_.size()) - 1);
    switching_ = false;

    active_ = -1;
    activateDocument(static_cast<int>(slots_.size()) - 1);
}

void MainWindow::setDocument(const pcbv::Document& document) {
    document_ = document;
    hasDocument_ = true;
    lastDirectory_ = QFileInfo(qs(document_.path)).absolutePath();

    setWindowTitle(QString("%1 — PCB Viewer").arg(qs(document_.fileName)));

    // Libraries are not boards: they show their own tab and have no BOM, nets or
    // comparison. Footprints are drawn in the Board tab one at a time.
    if (document_.type == pcbv::DocType::PcbLib || document_.type == pcbv::DocType::SchLib) {
        showLibraryDocument();
        return;
    }

    // A comparison belongs to the previous pair of documents.
    if (tabs_ != nullptr && comparePage_ != nullptr) {
        tabs_->setTabVisible(tabs_->indexOf(comparePage_), false);
    }
    if (tabs_ != nullptr) {
        // Back from a library: the document tabs are visible again.
        if (table_ != nullptr && table_->parentWidget() != nullptr)
            tabs_->setTabVisible(tabs_->indexOf(table_->parentWidget()), true);
        if (bomTable_ != nullptr && bomTable_->parentWidget() != nullptr)
            tabs_->setTabVisible(tabs_->indexOf(bomTable_->parentWidget()), true);
        if (libraryPage_ != nullptr) tabs_->setTabVisible(tabs_->indexOf(libraryPage_), false);
    }
    populateComponents();
    populateBom();
    populateNetlist();
    populateStructure();
    populateLayers();
    showDocumentSummary();

    if (measureAction_ != nullptr && measureAction_->isChecked()) measureAction_->setChecked(false);
    if (threeDAction_ != nullptr && threeDAction_->isChecked()) threeDAction_->setChecked(false);
    if (boardView_ != nullptr) {
        boardView_->setDocument(document_);
        updateBoardOptions();

        const bool hasBoard = !document_.geometry.empty();
        const int boardTab = tabs_ != nullptr ? tabs_->indexOf(boardView_) : -1;
        if (tabs_ != nullptr && boardTab >= 0) {
            tabs_->setTabVisible(boardTab, hasBoard);
            tabs_->setCurrentIndex(hasBoard ? boardTab : 0);
        }
        // The connectivity table only makes sense when pads were parsed.
        if (tabs_ != nullptr && netlistPage_ != nullptr) {
            const int netlistTab = tabs_->indexOf(netlistPage_);
            if (netlistTab >= 0) tabs_->setTabVisible(netlistTab, !document_.geometry.pads.empty());
        }
    }

    log(tr("Opened %1  (%2)").arg(qs(document_.fileName), qs(pcbv::docTypeName(document_.type))));
    log(tr("Container: %1").arg(qs(pcbv::containerName(document_.container))));
    log(tr("Format: %1").arg(qs(document_.formatVersion)));
    for (const auto& warning : document_.warnings) log(tr("warning: %1").arg(qs(warning)));
    if (document_.type == pcbv::DocType::PcbDoc) {
        log(tr("Components %1 · pads %2 · tracks %3 · vias %4 · nets %5")
                .arg(document_.components.size())
                .arg(document_.pcb.pads)
                .arg(document_.pcb.tracks)
                .arg(document_.pcb.vias)
                .arg(document_.pcb.nets));
    } else {
        log(tr("Components %1 · pins %2 · wires %3 · net labels %4")
                .arg(document_.components.size())
                .arg(document_.sch.pins)
                .arg(document_.sch.wires)
                .arg(document_.sch.netLabels));
    }

    statusBar()->showMessage(tr("%1  ·  %2").arg(qs(document_.fileName),
                                                 qs(pcbv::docTypeName(document_.type))));
    updateActionStates();
}

// ---------------------------------------------------------------------------
// Find in all open documents
// ---------------------------------------------------------------------------

void MainWindow::showSearchDialog() {
    if (slots_.empty()) {
        QMessageBox::information(this, tr("Find"), tr("Open a document first."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Find in all open documents"));
    dialog.resize(860, 480);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    QHBoxLayout* inputRow = new QHBoxLayout();
    QLineEdit* input = new QLineEdit(&dialog);
    input->setPlaceholderText(tr("Designator, value, footprint, net, layer or library item…"));
    input->setClearButtonEnabled(true);
    QLabel* counter = new QLabel(&dialog);
    counter->setStyleSheet("color:#8a929e;");
    inputRow->addWidget(input, 1);
    inputRow->addWidget(counter);
    layout->addLayout(inputRow);

    QTreeWidget* results = new QTreeWidget(&dialog);
    results->setHeaderLabels({tr("Document"), tr("Kind"), tr("Name"), tr("Details")});
    results->setRootIsDecorated(false);
    results->setUniformRowHeights(true);
    results->setAlternatingRowColors(true);
    results->setSelectionMode(QAbstractItemView::SingleSelection);
    results->setColumnWidth(0, 220);
    results->setColumnWidth(1, 110);
    results->setColumnWidth(2, 240);
    results->header()->setStretchLastSection(true);
    layout->addWidget(results, 1);

    QHBoxLayout* buttonRow = new QHBoxLayout();
    QLabel* hint = new QLabel(
        tr("Double click (or press Enter) to open the tab and select the item."), &dialog);
    hint->setStyleSheet("color:#8a929e;");
    QPushButton* closeButton = new QPushButton(tr("Close"), &dialog);
    QPushButton* openButton = new QPushButton(tr("Open"), &dialog);
    openButton->setDefault(true);
    buttonRow->addWidget(hint, 1);
    buttonRow->addWidget(closeButton);
    buttonRow->addWidget(openButton);
    layout->addLayout(buttonRow);
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::reject);

    // Navigation target of every row: which tab, and what to select in it.
    struct Target {
        int slot = -1;
        pcbv::SearchHit hit;
        QString title;
    };
    auto targets = std::make_shared<std::vector<Target>>();

    auto runSearch = [this, input, results, counter, targets]() {
        const QString needle = input->text().trimmed();
        targets->clear();
        results->clear();
        if (needle.isEmpty()) {
            counter->setText(QString());
            return;
        }
        int documentsWithHits = 0;
        int total = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const DocumentSlot& slot = slots_[i];
            const std::vector<pcbv::SearchHit> hits = pcbv::searchDocument(
                slot.document, slot.library, needle.toStdString(), 300);
            if (hits.empty()) continue;
            ++documentsWithHits;
            for (const pcbv::SearchHit& hit : hits) {
                auto* item = new QTreeWidgetItem(results);
                item->setText(0, slot.title);
                item->setText(1, qs(pcbv::searchKindName(hit.kind)));
                item->setText(2, qs(hit.name));
                item->setText(3, qs(hit.detail));
                item->setData(0, Qt::UserRole, static_cast<int>(targets->size()));
                targets->push_back(Target{static_cast<int>(i), hit, slot.title});
                ++total;
            }
        }
        if (total == 0) {
            counter->setText(tr("no matches in %1 open document(s)")
                                 .arg(static_cast<int>(slots_.size())));
        } else {
            counter->setText(tr("%1 match(es) in %2 document(s)")
                                 .arg(total)
                                 .arg(documentsWithHits));
        }
        if (results->topLevelItemCount() > 0) results->setCurrentItem(results->topLevelItem(0));
    };

    auto activate = [this, &dialog, results, targets]() {
        const QTreeWidgetItem* item = results->currentItem();
        if (item == nullptr) return;
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0 || index >= static_cast<int>(targets->size())) return;
        const Target& target = (*targets)[static_cast<std::size_t>(index)];

        activateDocument(target.slot);
        if (target.slot < 0 || target.slot >= static_cast<int>(slots_.size())) return;
        const DocumentSlot& slot = slots_[static_cast<std::size_t>(target.slot)];

        switch (target.hit.kind) {
            case pcbv::SearchHit::Kind::Component:
                if (!slot.library.footprints.empty()) {
                    if (libraryPage_ != nullptr && tabs_ != nullptr)
                        tabs_->setCurrentWidget(libraryPage_);
                    selectLibraryItem(target.hit.index);
                } else if (slot.library.symbols.empty()) {
                    selectComponentByIndex(target.hit.index);
                    if (table_ != nullptr && tabs_ != nullptr) tabs_->setCurrentWidget(table_->parentWidget());
                }
                break;
            case pcbv::SearchHit::Kind::Footprint:
            case pcbv::SearchHit::Kind::Symbol:
                if (libraryPage_ != nullptr && tabs_ != nullptr) tabs_->setCurrentWidget(libraryPage_);
                selectLibraryItem(target.hit.index);
                break;
            case pcbv::SearchHit::Kind::Net:
                selectNet(qs(target.hit.name));
                if (netlistPage_ != nullptr && tabs_ != nullptr) tabs_->setCurrentWidget(netlistPage_);
                break;
            case pcbv::SearchHit::Kind::Layer:
                if (layersTree_ != nullptr) {
                    for (int row = 0; row < layersTree_->topLevelItemCount(); ++row) {
                        QTreeWidgetItem* layerItem = layersTree_->topLevelItem(row);
                        if (layerItem->text(0) == qs(target.hit.name)) {
                            layersTree_->setCurrentItem(layerItem);
                            break;
                        }
                    }
                }
                if (boardView_ != nullptr && tabs_ != nullptr) tabs_->setCurrentWidget(boardView_);
                break;
        }
        statusBar()->showMessage(tr("%1: %2 %3").arg(target.title,
                                                    qs(pcbv::searchKindName(target.hit.kind)),
                                                    qs(target.hit.name)),
                                 5000);
        dialog.accept();
    };

    connect(input, &QLineEdit::textChanged, &dialog, [runSearch]() { runSearch(); });
    connect(input, &QLineEdit::returnPressed, &dialog, [activate]() { activate(); });
    connect(results, &QTreeWidget::itemDoubleClicked, &dialog,
            [activate](QTreeWidgetItem*, int) { activate(); });
    connect(openButton, &QPushButton::clicked, &dialog, [activate]() { activate(); });

    // Pre-fill with the current filter text: the user usually searches for what they see.
    const QString seed = filterEdit_ != nullptr ? filterEdit_->text().trimmed() : QString();
    if (!seed.isEmpty()) input->setText(seed);
    input->setFocus();
    input->selectAll();
    runSearch();
    dialog.exec();
}

// ---------------------------------------------------------------------------
// Libraries (.PcbLib / .SchLib)
// ---------------------------------------------------------------------------

void MainWindow::populateLibrary() {
    if (libraryTable_ == nullptr || active_ < 0 || active_ >= static_cast<int>(slots_.size()))
        return;
    const pcbv::Library& library = slots_[static_cast<std::size_t>(active_)].library;
    const bool footprintLibrary = library.isFootprintLibrary();

    QSignalBlocker blocker(libraryTable_);
    libraryTable_->setSortingEnabled(false);
    libraryTable_->setRowCount(0);

    if (footprintLibrary) {
        libraryTable_->setHorizontalHeaderLabels({tr("Footprint"), tr("Pads"), tr("Tracks"),
                                                  tr("Size (mm)"), tr("Height (mm)"),
                                                  tr("Description")});
        libraryTable_->setRowCount(static_cast<int>(library.footprints.size()));
        for (int row = 0; row < static_cast<int>(library.footprints.size()); ++row) {
            const pcbv::LibraryFootprint& footprint = library.footprints[static_cast<std::size_t>(row)];
            auto* nameItem = new QTableWidgetItem(qs(footprint.name));
            nameItem->setData(Qt::UserRole + 1, row);
            nameItem->setToolTip(qs(footprint.description));
            libraryTable_->setItem(row, 0, nameItem);
            libraryTable_->setItem(row, 1, new NumericItem(QString::number(footprint.padCount), footprint.padCount));
            libraryTable_->setItem(row, 2, new NumericItem(QString::number(footprint.trackCount), footprint.trackCount));
            const QString size = footprint.bounds.valid
                                     ? QString("%1 x %2")
                                           .arg(qs(pcbv::formatMm(footprint.bounds.width())),
                                                qs(pcbv::formatMm(footprint.bounds.height())))
                                     : QString();
            libraryTable_->setItem(row, 3, new QTableWidgetItem(size));
            libraryTable_->setItem(row, 4,
                                   new NumericItem(footprint.height > 0.0 ? QString::number(footprint.height * 0.0254, 'f', 2) : QString(),
                                                   static_cast<int>(footprint.height)));
            libraryTable_->setItem(row, 5, new QTableWidgetItem(qs(footprint.description)));
        }
        libraryInfo_->setText(tr("%1 footprints · %2")
                                  .arg(library.footprints.size())
                                  .arg(slots_[static_cast<std::size_t>(active_)].title));
    } else {
        libraryTable_->setHorizontalHeaderLabels({tr("Symbol"), tr("Pins"), tr("Parts"),
                                                  tr("Designator"), QString(),
                                                  tr("Description")});
        libraryTable_->setRowCount(static_cast<int>(library.symbols.size()));
        for (int row = 0; row < static_cast<int>(library.symbols.size()); ++row) {
            const pcbv::LibrarySymbol& symbol = library.symbols[static_cast<std::size_t>(row)];
            auto* nameItem = new QTableWidgetItem(qs(symbol.name));
            nameItem->setData(Qt::UserRole + 1, row);
            nameItem->setToolTip(qs(symbol.description));
            libraryTable_->setItem(row, 0, nameItem);
            libraryTable_->setItem(row, 1, new NumericItem(QString::number(symbol.pins.size()),
                                                           static_cast<int>(symbol.pins.size())));
            libraryTable_->setItem(row, 2, new NumericItem(QString::number(symbol.partCount), symbol.partCount));
            libraryTable_->setItem(row, 3, new QTableWidgetItem(qs(symbol.designatorPrefix)));
            libraryTable_->setItem(row, 4, new QTableWidgetItem(QString()));
            libraryTable_->setItem(row, 5, new QTableWidgetItem(qs(symbol.description)));
        }
        libraryInfo_->setText(tr("%1 symbols · %2")
                                  .arg(library.symbols.size())
                                  .arg(slots_[static_cast<std::size_t>(active_)].title));
    }

    // The "Height" column only means something for footprints.
    libraryTable_->setColumnHidden(4, !footprintLibrary);
    libraryTable_->setSortingEnabled(true);
    applyLibraryFilter(libraryFilter_ != nullptr ? libraryFilter_->text() : QString());
}

void MainWindow::applyLibraryFilter(const QString& text) {
    if (libraryTable_ == nullptr) return;
    const QString needle = text.trimmed();
    for (int row = 0; row < libraryTable_->rowCount(); ++row) {
        bool matches = needle.isEmpty();
        for (int column = 0; column < libraryTable_->columnCount() && !matches; ++column) {
            const QTableWidgetItem* item = libraryTable_->item(row, column);
            if (item != nullptr && item->text().contains(needle, Qt::CaseInsensitive)) matches = true;
        }
        libraryTable_->setRowHidden(row, !matches);
    }
}

QString MainWindow::libraryItemDetailsHtml(int index) const {
    if (active_ < 0 || active_ >= static_cast<int>(slots_.size())) return {};
    const pcbv::Library& library = slots_[static_cast<std::size_t>(active_)].library;

    QString html = "<div style='font-family:sans-serif;padding:10px 14px'>";
    auto row = [&html](const QString& key, const QString& value) {
        if (value.trimmed().isEmpty()) return;
        html += QString("<tr><td style='color:#9aa0a6;padding:3px 12px 3px 0;white-space:nowrap;"
                        "vertical-align:top'>%1</td><td style='color:#e3e6ea'>%2</td></tr>")
                    .arg(key, value);
    };

    if (library.isFootprintLibrary()) {
        if (index < 0 || index >= static_cast<int>(library.footprints.size())) return {};
        const pcbv::LibraryFootprint& footprint = library.footprints[static_cast<std::size_t>(index)];
        html += QString("<h2 style='margin:0 0 2px 0;color:#ffffff'>%1</h2>").arg(qs(footprint.name));
        html += QString("<div style='color:#8ab4f8;font-size:13px;margin-bottom:10px'>%1</div>")
                    .arg(tr("PCB footprint"));
        html += "<table style='font-size:13px;border-collapse:collapse'>";
        row(tr("Description"), qs(footprint.description));
        if (footprint.bounds.valid) {
            row(tr("Size"), QString("%1 × %2 mm").arg(qs(pcbv::formatMm(footprint.bounds.width())),
                                                      qs(pcbv::formatMm(footprint.bounds.height()))));
        }
        if (footprint.height > 0.0)
            row(tr("Height"), QString("%1 mm").arg(qs(pcbv::formatMm(footprint.height))));
        row(tr("Pads"), QString("%1  (%2 tracks · %3 arcs · %4 vias)")
                            .arg(footprint.padCount)
                            .arg(footprint.trackCount)
                            .arg(footprint.arcCount)
                            .arg(footprint.viaCount));
        if (footprint.regionCount > 0 || footprint.fillCount > 0) {
            row(tr("Regions"), QString("%1 region(s) · %2 fill(s)")
                                   .arg(footprint.regionCount)
                                   .arg(footprint.fillCount));
        }
        html += "</table>";

        if (!footprint.geometry.pads.empty()) {
            html += QString("<h3 style='color:#e3e6ea;margin:14px 0 4px 0;font-size:13px'>%1</h3>")
                        .arg(tr("Pads"));
            html += "<table style='font-size:12px;border-collapse:collapse'>";
            html += QString("<tr><td style='color:#9aa0a6;padding:2px 12px 2px 0'>%1</td>"
                            "<td style='color:#9aa0a6;padding:2px 12px 2px 0'>%2</td>"
                            "<td style='color:#9aa0a6;padding:2px 12px 2px 0'>%3</td>"
                            "<td style='color:#9aa0a6;padding:2px 12px 2px 0'>%4</td></tr>")
                        .arg(tr("Pad"), tr("Layer"), tr("Size (mm)"), tr("Position (mm)"));
            int shown = 0;
            for (const pcbv::Pad& pad : footprint.geometry.pads) {
                if (++shown > 200) break;
                html += QString("<tr><td style='color:#e3e6ea;padding:1px 12px 1px 0'>%1</td>"
                                "<td style='color:#c8cdd4;padding:1px 12px 1px 0'>%2</td>"
                                "<td style='color:#c8cdd4;padding:1px 12px 1px 0'>%3 × %4</td>"
                                "<td style='color:#c8cdd4;padding:1px 12px 1px 0'>%5, %6</td></tr>")
                            .arg(qs(pad.designator), qs(pad.layerName),
                                 qs(pcbv::formatMm(pad.width)), qs(pcbv::formatMm(pad.height)),
                                 qs(pcbv::formatMm(pad.x)), qs(pcbv::formatMm(pad.y)));
            }
            html += "</table>";
            if (static_cast<int>(footprint.geometry.pads.size()) > shown) {
                html += QString("<div style='color:#8a929e;font-size:12px;margin-top:4px'>%1</div>")
                            .arg(tr("… %1 more pads; the board view shows all of them.")
                                     .arg(static_cast<int>(footprint.geometry.pads.size()) - shown));
            }
        }
        if (!footprint.parameters.empty()) {
            html += QString("<h3 style='color:#e3e6ea;margin:14px 0 4px 0;font-size:13px'>%1</h3>")
                        .arg(tr("Parameters"));
            html += "<table style='font-size:12px;border-collapse:collapse'>";
            for (const pcbv::Property& property : footprint.parameters) {
                row(qs(property.name), qs(property.value));
            }
            html += "</table>";
        }
    } else {
        if (index < 0 || index >= static_cast<int>(library.symbols.size())) return {};
        const pcbv::LibrarySymbol& symbol = library.symbols[static_cast<std::size_t>(index)];
        html += QString("<h2 style='margin:0 0 2px 0;color:#ffffff'>%1</h2>").arg(qs(symbol.name));
        html += QString("<div style='color:#8ab4f8;font-size:13px;margin-bottom:10px'>%1</div>")
                    .arg(tr("Schematic symbol"));
        html += "<table style='font-size:13px;border-collapse:collapse'>";
        row(tr("Description"), qs(symbol.description));
        row(tr("Designator"), qs(symbol.designatorPrefix));
        row(tr("Pins"), QString::number(symbol.pins.size()));
        if (symbol.partCount > 1) row(tr("Sub-parts"), QString::number(symbol.partCount));
        html += "</table>";

        if (!symbol.pins.empty()) {
            html += QString("<h3 style='color:#e3e6ea;margin:14px 0 4px 0;font-size:13px'>%1</h3>")
                        .arg(tr("Pins"));
            html += "<table style='font-size:12px;border-collapse:collapse'>";
            html += QString("<tr><td style='color:#9aa0a6;padding:2px 12px 2px 0'>%1</td>"
                            "<td style='color:#9aa0a6;padding:2px 12px 2px 0'>%2</td>"
                            "<td style='color:#9aa0a6;padding:2px 12px 2px 0'>%3</td></tr>")
                        .arg(tr("Designator"), tr("Name"), tr("Type"));
            for (const pcbv::Pin& pin : symbol.pins) {
                html += QString("<tr><td style='color:#e3e6ea;padding:1px 12px 1px 0'>%1</td>"
                                "<td style='color:#c8cdd4;padding:1px 12px 1px 0'>%2</td>"
                                "<td style='color:#c8cdd4;padding:1px 12px 1px 0'>%3</td></tr>")
                            .arg(qs(pin.designator), qs(pin.name), qs(pin.electrical));
            }
            html += "</table>";
        }
        if (!symbol.parameters.empty()) {
            html += QString("<h3 style='color:#e3e6ea;margin:14px 0 4px 0;font-size:13px'>%1</h3>")
                        .arg(tr("Parameters"));
            html += "<table style='font-size:12px;border-collapse:collapse'>";
            for (const pcbv::Property& property : symbol.parameters) {
                if (property.value.empty()) continue;
                row(qs(property.name), qs(property.value));
            }
            html += "</table>";
        }
    }
    html += "</div>";
    return html;
}

void MainWindow::selectLibraryItem(int index) {
    if (active_ < 0 || active_ >= static_cast<int>(slots_.size()) || libraryTable_ == nullptr) return;
    DocumentSlot& slot = slots_[static_cast<std::size_t>(active_)];
    const pcbv::Library& library = slot.library;
    const int count = library.isFootprintLibrary() ? static_cast<int>(library.footprints.size())
                                                   : static_cast<int>(library.symbols.size());
    if (count == 0) return;
    index = std::clamp(index, 0, count - 1);
    slot.libraryItem = index;

    // Select the row that belongs to this item (row order may differ after sorting).
    {
        QSignalBlocker blocker(libraryTable_);
        for (int row = 0; row < libraryTable_->rowCount(); ++row) {
            const QTableWidgetItem* item = libraryTable_->item(row, 0);
            if (item != nullptr && item->data(Qt::UserRole + 1).toInt() == index) {
                libraryTable_->selectRow(row);
                libraryTable_->scrollToItem(item);
                break;
            }
        }
    }

    if (detailsView_ != nullptr) detailsView_->setHtml(libraryItemDetailsHtml(index));

    if (boardView_ != nullptr) {
        const bool footprintLibrary = library.isFootprintLibrary();
        const pcbv::Document preview =
            footprintLibrary
                ? pcbv::footprintDocument(library.footprints[static_cast<std::size_t>(index)],
                                          slot.title.toStdString())
                : pcbv::symbolDocument(library.symbols[static_cast<std::size_t>(index)],
                                       slot.title.toStdString());
        boardView_->setDocument(preview);
        // The layers panel and the summary describe what is on screen.
        document_.geometry = preview.geometry;
        document_.bounds = preview.bounds;
        document_.nets.clear();
        document_.netSummaries.clear();
        updateBoardOptions();
        populateLayers();
    }
}

void MainWindow::showSelectedLibraryItem() {
    if (libraryTable_ == nullptr) return;
    const int row = libraryTable_->currentRow();
    if (row < 0) return;
    const QTableWidgetItem* item = libraryTable_->item(row, 0);
    if (item == nullptr) return;
    selectLibraryItem(item->data(Qt::UserRole + 1).toInt());
}

void MainWindow::showLibraryDocument() {
    if (active_ < 0 || active_ >= static_cast<int>(slots_.size())) return;
    DocumentSlot& slot = slots_[static_cast<std::size_t>(active_)];
    const bool footprintLibrary = slot.library.isFootprintLibrary();
    const bool hasDrawing = footprintLibrary || !slot.library.symbols.empty();
    (void)hasDrawing;

    if (tabs_ != nullptr) {
        // Documents keep their own tabs; a library shows one list of items instead.
        if (table_ != nullptr && table_->parentWidget() != nullptr)
            tabs_->setTabVisible(tabs_->indexOf(table_->parentWidget()), false);
        if (bomTable_ != nullptr && bomTable_->parentWidget() != nullptr)
            tabs_->setTabVisible(tabs_->indexOf(bomTable_->parentWidget()), false);
        if (netlistPage_ != nullptr) tabs_->setTabVisible(tabs_->indexOf(netlistPage_), false);
        if (comparePage_ != nullptr) tabs_->setTabVisible(tabs_->indexOf(comparePage_), false);
        if (libraryPage_ != nullptr) {
            tabs_->setTabVisible(tabs_->indexOf(libraryPage_), true);
            tabs_->setCurrentWidget(libraryPage_);
        }
        const int boardTab = tabs_->indexOf(boardView_);
        if (boardTab >= 0) tabs_->setTabVisible(boardTab, true);
    }

    if (measureAction_ != nullptr && measureAction_->isChecked()) measureAction_->setChecked(false);
    if (threeDAction_ != nullptr && threeDAction_->isChecked()) threeDAction_->setChecked(false);

    populateLibrary();
    populateStructure();
    selectLibraryItem(slot.libraryItem);

    log(tr("Opened %1  (%2)").arg(qs(document_.fileName), qs(pcbv::docTypeName(document_.type))));
    log(tr("Library: %1").arg(qs(pcbv::librarySummary(slot.library))));
    for (const std::string& warning : slot.library.warnings) log(tr("warning: %1").arg(qs(warning)));
    statusBar()->showMessage(
        footprintLibrary
            ? tr("%1  ·  %2  ·  pick a footprint to see it on the Board tab")
                  .arg(qs(document_.fileName), qs(pcbv::librarySummary(slot.library)))
            : tr("%1  ·  %2  ·  pick a symbol to see its drawing on the Board tab")
                  .arg(qs(document_.fileName), qs(pcbv::librarySummary(slot.library))),
        12000);
    updateActionStates();
}

// ---------------------------------------------------------------------------
// Recent files and the last session
// ---------------------------------------------------------------------------

void MainWindow::rememberRecent(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (absolute.isEmpty() || !QFileInfo::exists(absolute)) return;

    std::vector<std::string> stored;
    stored.reserve(static_cast<std::size_t>(recentFiles_.size()));
    for (const QString& entry : recentFiles_) stored.push_back(entry.toStdString());

    stored = pcbv::pushRecent(std::move(stored), absolute.toStdString(), 10);

    recentFiles_.clear();
    for (const std::string& entry : stored) recentFiles_.append(qs(entry));

    QSettings settings;
    settings.setValue(QStringLiteral("files/recent"), recentFiles_);
    // The menu is rebuilt when it opens; it only has to become reachable again.
    if (recentMenu_ != nullptr) recentMenu_->setEnabled(true);
}

void MainWindow::rebuildRecentMenu() {
    if (recentMenu_ == nullptr) return;

    QSettings settings;
    recentFiles_ = settings.value(QStringLiteral("files/recent")).toStringList();

    // Files that were moved or deleted since they were remembered drop out silently.
    QStringList alive;
    for (const QString& path : recentFiles_) {
        if (QFileInfo::exists(path)) alive.append(path);
    }
    if (alive != recentFiles_) {
        recentFiles_ = alive;
        settings.setValue(QStringLiteral("files/recent"), recentFiles_);
    }

    recentMenu_->clear();
    int number = 1;
    for (const QString& path : recentFiles_) {
        const QString label = QStringLiteral("&%1  %2").arg(number).arg(QFileInfo(path).fileName());
        QAction* entry = recentMenu_->addAction(label);
        entry->setToolTip(path);
        entry->setStatusTip(path);
        // Open from the event loop instead of inside the menu: opening reorders the list and
        // rebuilds this very menu, which must not happen while the action is being emitted.
        connect(entry, &QAction::triggered, this, [this, path]() {
            QMetaObject::invokeMethod(this, [this, path]() { openDocument(path); },
                                      Qt::QueuedConnection);
        });
        if (++number > 10) break;
    }

    if (recentFiles_.isEmpty()) {
        QAction* empty = recentMenu_->addAction(tr("(nothing yet)"));
        empty->setEnabled(false);
        recentMenu_->setEnabled(false);
        return;
    }
    recentMenu_->setEnabled(true);
    recentMenu_->addSeparator();
    QAction* clear = recentMenu_->addAction(tr("Clear the &list"));
    connect(clear, &QAction::triggered, this, [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            recentFiles_.clear();
            QSettings().remove(QStringLiteral("files/recent"));
            rebuildRecentMenu();
        }, Qt::QueuedConnection);
    });
}

void MainWindow::restoreSession() {
    QSettings settings;
    const QStringList files = settings.value(QStringLiteral("session/open")).toStringList();
    if (files.isEmpty()) return;

    const int wantedIndex = settings.value(QStringLiteral("session/active"), 0).toInt();
    const QString wanted =
        wantedIndex >= 0 && wantedIndex < files.size()
            ? QFileInfo(files.at(wantedIndex)).absoluteFilePath()
            : QString();

    restoringSession_ = true;
    QStringList reopened;
    for (const QString& path : files) {
        if (path.isEmpty() || !QFileInfo::exists(path)) continue;
        const int before = static_cast<int>(slots_.size());
        openDocument(path);
        if (static_cast<int>(slots_.size()) > before)
            reopened.append(QFileInfo(path).absoluteFilePath());
    }
    restoringSession_ = false;

    if (reopened.isEmpty()) return;
    const int wantedSlot = reopened.indexOf(wanted);
    if (wantedSlot > 0) activateDocument(wantedSlot);
    log(tr("Reopened %1 document(s) from the last session.").arg(reopened.size()));
}

void MainWindow::saveSession() {
    QStringList open;
    for (const DocumentSlot& slot : slots_) open.append(slot.path);
    QSettings settings;
    if (open.isEmpty()) {
        settings.remove(QStringLiteral("session/open"));
        settings.remove(QStringLiteral("session/active"));
        return;
    }
    settings.setValue(QStringLiteral("session/open"), open);
    settings.setValue(QStringLiteral("session/active"), active_);
}

// ---------------------------------------------------------------------------
// Open documents
// ---------------------------------------------------------------------------

void MainWindow::captureViewState() {
    if (active_ < 0 || active_ >= static_cast<int>(slots_.size()) || !hasDocument_) return;
    DocumentSlot& slot = slots_[static_cast<std::size_t>(active_)];
    if (boardView_ != nullptr) {
        slot.zoom = boardView_->zoom();
        slot.pan = boardView_->pan();
        slot.selectedComponent = boardView_->selectedComponent();
        slot.highlightNet = boardView_->highlightNet();
    }
    if (filterEdit_ != nullptr) slot.componentFilter = filterEdit_->text();
    if (bomFilter_ != nullptr) slot.bomFilter = bomFilter_->text();
    if (netlistFilter_ != nullptr) slot.netlistFilter = netlistFilter_->text();
    if (tabs_ != nullptr) slot.viewTab = tabs_->currentIndex();

    const bool compareShown = tabs_ != nullptr && comparePage_ != nullptr &&
                              tabs_->isTabVisible(tabs_->indexOf(comparePage_));
    slot.comparisonHtml = compareShown ? lastComparisonHtml_ : QString();
    slot.comparisonHint = compareShown && compareHint_ != nullptr ? compareHint_->text() : QString();
}

void MainWindow::restoreViewState() {
    if (active_ < 0 || active_ >= static_cast<int>(slots_.size())) return;
    const DocumentSlot& slot = slots_[static_cast<std::size_t>(active_)];

    if (boardView_ != nullptr) {
        boardView_->setView(slot.zoom, slot.pan);
        boardView_->setSelectedComponent(slot.selectedComponent);
        boardView_->setHighlightNet(slot.highlightNet);
    }
    if (filterEdit_ != nullptr) filterEdit_->setText(slot.componentFilter);
    if (bomFilter_ != nullptr) bomFilter_->setText(slot.bomFilter);
    if (netlistFilter_ != nullptr) netlistFilter_->setText(slot.netlistFilter);

    // Bring back the part that was selected: the table row drives the details pane and the
    // board highlight, so restoring it restores the whole selection.
    if (slot.selectedComponent >= 0 && slot.library.itemCount() == 0)
        selectComponentByIndex(slot.selectedComponent);

    if (tabs_ != nullptr) {
        const int tab = std::clamp(slot.viewTab, 0, std::max(tabs_->count() - 1, 0));
        if (tab >= 0 && tabs_->isTabVisible(tab)) tabs_->setCurrentIndex(tab);
    }
    // A document keeps its own comparison; switching away must not show the previous one.
    if (tabs_ != nullptr && comparePage_ != nullptr) {
        const int compareTab = tabs_->indexOf(comparePage_);
        const bool hasComparison = !slot.comparisonHtml.isEmpty();
        if (!hasComparison) {
            lastComparisonHtml_.clear();
            if (compareView_ != nullptr) compareView_->clear();
            if (compareHint_ != nullptr) compareHint_->clear();
        }
        if (compareTab >= 0) tabs_->setTabVisible(compareTab, hasComparison);
    }
    if (!slot.comparisonHtml.isEmpty() && tabs_ != nullptr && comparePage_ != nullptr &&
        compareView_ != nullptr) {
        lastComparisonHtml_ = slot.comparisonHtml;
        compareView_->setHtml(slot.comparisonHtml);
        if (compareHint_ != nullptr) compareHint_->setText(slot.comparisonHint);
        tabs_->setTabVisible(tabs_->indexOf(comparePage_), true);
    }
}

void MainWindow::activateDocument(int index) {
    if (index < 0 || index >= static_cast<int>(slots_.size())) return;

    switching_ = true;
    if (docTabs_ != nullptr) docTabs_->setCurrentIndex(index);
    switching_ = false;

    if (index == active_) return;
    captureViewState();
    active_ = index;

    setDocument(slots_[static_cast<std::size_t>(index)].document);
    restoreViewState();
    updateDocumentTitle();
    statusBar()->showMessage(
        tr("%1 of %2: %3")
            .arg(active_ + 1)
            .arg(static_cast<int>(slots_.size()))
            .arg(slots_[static_cast<std::size_t>(index)].title),
        4000);
}

void MainWindow::closeDocument(int index) {
    if (index < 0 || index >= static_cast<int>(slots_.size())) return;
    const bool wasActive = index == active_;
    if (wasActive) captureViewState();

    switching_ = true;
    if (docTabs_ != nullptr) docTabs_->removeTab(index);
    switching_ = false;
    slots_.erase(slots_.begin() + index);

    if (slots_.empty()) {
        active_ = -1;
        if (docTabs_ != nullptr) docTabs_->setVisible(false);
        clearDocument();
        return;
    }

    if (!wasActive) {
        // Closing a background tab keeps the user where they were.
        if (index < active_) --active_;
        switching_ = true;
        if (docTabs_ != nullptr) docTabs_->setCurrentIndex(active_);
        switching_ = false;
        updateDocumentTitle();
        updateActionStates();
        return;
    }

    const int next = std::min(index, static_cast<int>(slots_.size()) - 1);
    active_ = -1;  // force a full reload of the neighbouring document
    activateDocument(next);
}

void MainWindow::closeCurrentDocument() {
    if (active_ >= 0) closeDocument(active_);
}

void MainWindow::closeAllDocuments() {
    while (!slots_.empty()) closeDocument(static_cast<int>(slots_.size()) - 1);
}

void MainWindow::clearDocument() {
    document_ = pcbv::Document();
    hasDocument_ = false;
    lastComparisonHtml_.clear();

    if (boardView_ != nullptr) boardView_->clear();
    if (table_ != nullptr) table_->setRowCount(0);
    if (tableInfoLabel_ != nullptr) tableInfoLabel_->setText(tr("no document"));
    if (bomTable_ != nullptr) bomTable_->setRowCount(0);
    if (bomInfo_ != nullptr) bomInfo_->setText(tr("no document"));
    if (netlistTree_ != nullptr) netlistTree_->clear();
    if (netlistInfo_ != nullptr) netlistInfo_->setText(tr("no document"));
    if (layersTree_ != nullptr) layersTree_->clear();
    if (structureTree_ != nullptr) structureTree_->clear();
    if (detailsView_ != nullptr) {
        detailsView_->setHtml(
            tr("<div style='color:#9aa0a6;font-family:sans-serif;padding:18px'>"
               "No document open.<br>Use <b>File → Open</b> (Ctrl+O) or drop a .PcbDoc / "
               ".SchDoc here — every file gets its own tab.</div>"));
    }
    if (tabs_ != nullptr && comparePage_ != nullptr) {
        tabs_->setTabVisible(tabs_->indexOf(comparePage_), false);
    }
    if (libraryTable_ != nullptr) libraryTable_->setRowCount(0);
    if (libraryInfo_ != nullptr) libraryInfo_->setText(tr("no library"));
    if (tabs_ != nullptr) {
        if (libraryPage_ != nullptr) tabs_->setTabVisible(tabs_->indexOf(libraryPage_), false);
        // Back to the normal, empty document layout.
        if (table_ != nullptr && table_->parentWidget() != nullptr)
            tabs_->setTabVisible(tabs_->indexOf(table_->parentWidget()), true);
        if (bomTable_ != nullptr && bomTable_->parentWidget() != nullptr)
            tabs_->setTabVisible(tabs_->indexOf(bomTable_->parentWidget()), true);
        tabs_->setCurrentIndex(0);
    }
    updateDocumentTitle();
    updateActionStates();
}

void MainWindow::updateDocumentTitle() {
    if (active_ < 0 || active_ >= static_cast<int>(slots_.size())) {
        setWindowTitle(tr("PCB Viewer"));
        return;
    }
    const DocumentSlot& slot = slots_[static_cast<std::size_t>(active_)];
    setWindowTitle(slots_.size() > 1
                       ? tr("%1 — PCB Viewer (%2 of %3)")
                             .arg(slot.title)
                             .arg(active_ + 1)
                             .arg(static_cast<int>(slots_.size()))
                       : tr("%1 — PCB Viewer").arg(slot.title));
}

void MainWindow::updateActionStates() {
    const bool hasDocument = hasDocument_;
    const bool isLibrary = document_.type == pcbv::DocType::PcbLib ||
                           document_.type == pcbv::DocType::SchLib;
    const bool hasBoard = hasDocument && boardView_ != nullptr && boardView_->hasGeometry();

    if (isLibrary) {
        // A library has no nets, no BOM and nothing to compare; it only shows items.
        for (QAction* action : {csvAction_, jsonAction_, htmlAction_, bomCsvAction_, bomHtmlAction_,
                                netlistCsvAction_, exportAction_, copyAction_, compareAction_,
                                findAction_}) {
            if (action != nullptr) action->setEnabled(false);
        }
        for (QAction* action : {svgAction_, printAction_, fitAction_, zoomInAction_,
                                zoomOutAction_, measureAction_, zoomPartAction_}) {
            if (action != nullptr) action->setEnabled(hasBoard);
        }
        if (layersDock_ != nullptr) layersDock_->setEnabled(hasBoard);
        if (summaryAction_ != nullptr) summaryAction_->setEnabled(hasDocument);
        for (QAction* action : {closeAction_, closeAllAction_}) {
            if (action != nullptr) action->setEnabled(hasDocument);
        }
        if (globalFindAction_ != nullptr) globalFindAction_->setEnabled(hasDocument);
        const bool many = slots_.size() > 1;
        if (nextDocumentAction_ != nullptr) nextDocumentAction_->setEnabled(many);
        if (previousDocumentAction_ != nullptr) previousDocumentAction_->setEnabled(many);
        return;
    }

    // Document level commands (BOM, reports, summary).
    for (QAction* action : {csvAction_, bomCsvAction_, bomHtmlAction_, netlistCsvAction_,
                            jsonAction_, htmlAction_, exportAction_, copyAction_,
                            summaryAction_, compareAction_, closeAction_, closeAllAction_}) {
        if (action != nullptr) action->setEnabled(hasDocument);
    }
    // Board level commands only make sense for documents with drawable geometry.
    for (QAction* action : {svgAction_, netlistCsvAction_, printAction_, fitAction_,
                            zoomInAction_, zoomOutAction_, measureAction_, zoomPartAction_}) {
        if (action != nullptr) action->setEnabled(hasBoard);
    }
    if (layersDock_ != nullptr) layersDock_->setEnabled(hasBoard);

    if (globalFindAction_ != nullptr) globalFindAction_->setEnabled(hasDocument);

    const bool many = slots_.size() > 1;
    if (nextDocumentAction_ != nullptr) nextDocumentAction_->setEnabled(many);
    if (previousDocumentAction_ != nullptr) previousDocumentAction_->setEnabled(many);
}

void MainWindow::ensureDocumentLoaded() {
    if (!hasDocument_) {
        QMessageBox::information(this, tr("No document"),
                                 tr("Please open an Altium .PcbDoc or .SchDoc file first."));
    }
}

void MainWindow::populateComponents() {
    table_->setSortingEnabled(false);
    table_->setRowCount(0);
    table_->setRowCount(static_cast<int>(document_.components.size()));
    const bool pcb = document_.type == pcbv::DocType::PcbDoc;

    for (int row = 0; row < static_cast<int>(document_.components.size()); ++row) {
        const pcbv::Component& c = document_.components[static_cast<std::size_t>(row)];

        auto* designatorItem = new QTableWidgetItem(qs(c.designator));
        designatorItem->setData(Qt::UserRole + 1, row);  // index into document.components
        table_->setItem(row, 0, designatorItem);
        table_->setItem(row, 1, new QTableWidgetItem(qs(c.comment)));
        table_->setItem(row, 2, new QTableWidgetItem(qs(c.footprint)));
        table_->setItem(row, 3, new QTableWidgetItem(qs(c.manufacturer)));
        table_->setItem(row, 4, new QTableWidgetItem(qs(c.partNumber)));

        const int pins = c.pinCount > 0 ? c.pinCount : -1;
        const int pads = c.padCount > 0 ? c.padCount : -1;
        table_->setItem(row, 5, new NumericItem(c.pinCount > 0 ? QString::number(c.pinCount) : QString(), pins));
        table_->setItem(row, 6, new NumericItem(c.padCount > 0 ? QString::number(c.padCount) : QString(), pads));
        table_->setItem(row, 7, new QTableWidgetItem(qs(c.layer)));
        table_->setItem(row, 8, new QTableWidgetItem(qs(c.description)));

        QString tooltip = tr("<b>%1</b><br>%2").arg(qs(c.designator), qs(c.description));
        if (!c.supplierPartNumber.empty())
            tooltip += tr("<br>Supplier P/N: %1").arg(qs(c.supplierPartNumber));
        if (!c.datasheet.empty()) tooltip += tr("<br>%1").arg(qs(c.datasheet));
        for (int col = 0; col < 9; ++col) {
            if (QTableWidgetItem* item = table_->item(row, col)) item->setToolTip(tooltip);
        }
    }

    table_->setSortingEnabled(true);

    tableInfoLabel_->setText(tr("%1 components · %2")
                                 .arg(document_.components.size())
                                 .arg(pcb ? tr("PCB") : tr("schematic")));
    applyFilter(filterEdit_ ? filterEdit_->text() : QString());
}

void MainWindow::populateBom() {
    if (bomTable_ == nullptr) return;
    const std::vector<pcbv::BomEntry> entries = pcbv::buildBom(document_);

    bomTable_->setSortingEnabled(false);
    bomTable_->setRowCount(0);
    bomTable_->setRowCount(static_cast<int>(entries.size()));

    const QColor dim(0x8a, 0x92, 0x9e);
    for (int row = 0; row < static_cast<int>(entries.size()); ++row) {
        const pcbv::BomEntry& entry = entries[static_cast<std::size_t>(row)];
        auto* quantity = new NumericItem(QString::number(entry.quantity), entry.quantity);
        quantity->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        bomTable_->setItem(row, 0, quantity);

        auto* designators = new QTableWidgetItem(qs(entry.designatorList()));
        designators->setToolTip(qs(entry.designatorList()));
        bomTable_->setItem(row, 1, designators);

        bomTable_->setItem(row, 2, new QTableWidgetItem(qs(entry.label())));
        bomTable_->setItem(row, 3, new QTableWidgetItem(qs(entry.footprint)));
        bomTable_->setItem(row, 4, new QTableWidgetItem(qs(entry.manufacturer)));
        bomTable_->setItem(row, 5, new QTableWidgetItem(qs(entry.partNumber)));
        bomTable_->setItem(row, 6, new QTableWidgetItem(qs(entry.layers)));

        QString note;
        if (entry.dnp) note = tr("DNP");
        else if (entry.throughHole) note = tr("THT");
        auto* noteItem = new QTableWidgetItem(note);
        if (entry.dnp) noteItem->setForeground(QColor(0xe5, 0x7c, 0x73));
        bomTable_->setItem(row, 7, noteItem);

        if (entry.dnp) {
            for (int column = 1; column < 7; ++column) {
                if (QTableWidgetItem* item = bomTable_->item(row, column)) item->setForeground(dim);
            }
        }
    }

    bomTable_->setSortingEnabled(true);
    if (bomInfo_ != nullptr) {
        bomInfo_->setText(document_.components.empty() ? tr("no parts")
                                                       : qs(pcbv::bomSummary(document_)));
    }
    applyBomFilter(bomFilter_ != nullptr ? bomFilter_->text() : QString());
}

void MainWindow::applyBomFilter(const QString& text) {
    if (bomTable_ == nullptr) return;
    const QString needle = text.trimmed();
    for (int row = 0; row < bomTable_->rowCount(); ++row) {
        bool match = needle.isEmpty();
        for (int column = 1; column < 7 && !match; ++column) {
            const QTableWidgetItem* item = bomTable_->item(row, column);
            if (item != nullptr && item->text().contains(needle, Qt::CaseInsensitive)) match = true;
        }
        bomTable_->setRowHidden(row, !match);
    }
}

void MainWindow::populateNetlist() {
    if (netlistTree_ == nullptr) return;
    netlistTree_->clear();

    const std::vector<pcbv::NetConnection> netlist = pcbv::buildNetlist(document_);

    // Copper statistics per net come from the board index, the pad list from the netlist,
    // so the Netlist tab carries everything the old Nets panel used to show.
    std::map<std::string, const pcbv::NetSummary*> summaries;
    for (const auto& summary : document_.netSummaries) summaries[summary.name] = &summary;

    std::size_t pads = 0;
    int named = 0;
    int unconnected = 0;
    int dangling = 0;

    for (const auto& net : netlist) {
        pads += net.nodes.size();
        const bool noNet = net.name == pcbv::noNetName();
        if (noNet) unconnected = static_cast<int>(net.nodes.size());
        else ++named;
        if (!noNet && net.nodes.size() == 1) ++dangling;

        auto* netItem = new QTreeWidgetItem(netlistTree_);
        netItem->setText(0, QStringLiteral("%1  (%2)").arg(qs(net.name)).arg(net.nodes.size()));
        netItem->setData(0, Qt::UserRole, qs(net.name));
        netItem->setData(0, Qt::UserRole + 1, -1);
        QFont netFont = netItem->font(0);
        netFont.setBold(true);
        netItem->setFont(0, netFont);

        const auto summary = summaries.find(net.name);
        if (summary != summaries.end()) {
            const pcbv::NetSummary& copper = *summary->second;
            netItem->setText(1, QString::number(copper.pads));
            netItem->setText(2, QString::number(copper.tracks));
            netItem->setText(3, QString::number(copper.vias));
            netItem->setText(4, qs(pcbv::formatMm(copper.trackLength)));
            for (int column = 1; column <= 4; ++column) {
                netItem->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
                netItem->setForeground(column, QColor(0x9a, 0xa0, 0xa6));
            }
        }
        if (!noNet && net.nodes.size() == 1) {
            netItem->setForeground(0, QColor(0xe5, 0x7c, 0x73));  // a single pad = unfinished
            netItem->setToolTip(0, tr("Only one pad on this net — the routing looks unfinished."));
        }

        for (const auto& node : net.nodes) {
            auto* child = new QTreeWidgetItem(netItem);
            child->setText(0, qs(node.label));
            child->setText(5, qs(node.layer));
            child->setText(6, qs(pcbv::formatMm(node.x)));
            child->setText(7, qs(pcbv::formatMm(node.y)));
            child->setTextAlignment(6, Qt::AlignRight | Qt::AlignVCenter);
            child->setTextAlignment(7, Qt::AlignRight | Qt::AlignVCenter);
            child->setForeground(5, QColor(0x9a, 0xa0, 0xa6));
            child->setForeground(6, QColor(0x9a, 0xa0, 0xa6));
            child->setForeground(7, QColor(0x9a, 0xa0, 0xa6));
            child->setData(0, Qt::UserRole, qs(net.name));
            child->setData(0, Qt::UserRole + 1, node.componentIndex);
        }
    }

    if (netlistInfo_ != nullptr) {
        if (pads == 0) {
            netlistInfo_->setText(tr("no pads"));
        } else {
            QString info = tr("%1 nets · %2 pads").arg(named).arg(pads);
            if (dangling > 0) info += tr(" · %1 single-pad").arg(dangling);
            if (unconnected > 0) info += tr(" · %1 unconnected").arg(unconnected);
            netlistInfo_->setText(info);
        }
    }
    applyNetlistFilter(netlistFilter_ != nullptr ? netlistFilter_->text() : QString());
}

void MainWindow::applyNetlistFilter(const QString& text) {
    if (netlistTree_ == nullptr) return;
    const QString needle = text.trimmed();

    for (int i = 0; i < netlistTree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* netItem = netlistTree_->topLevelItem(i);
        const bool netMatches = needle.isEmpty() ||
                                netItem->text(0).contains(needle, Qt::CaseInsensitive);
        int visibleChildren = 0;
        for (int child = 0; child < netItem->childCount(); ++child) {
            QTreeWidgetItem* node = netItem->child(child);
            bool match = netMatches;
            for (int column : {0, 5}) {
                if (match) break;
                if (node->text(column).contains(needle, Qt::CaseInsensitive)) match = true;
            }
            node->setHidden(!match);
            if (match) ++visibleChildren;
        }
        netItem->setHidden(visibleChildren == 0 && !netMatches);
        if (!needle.isEmpty()) netItem->setExpanded(visibleChildren > 0);
    }
}

void MainWindow::selectNetlistItem() {
    if (netlistTree_ == nullptr) return;
    const QList<QTreeWidgetItem*> items = netlistTree_->selectedItems();
    if (items.isEmpty()) return;
    QTreeWidgetItem* item = items.first();

    const QString net = item->data(0, Qt::UserRole).toString();
    if (!net.isEmpty() && net != pcbv::noNetName() && boardView_ != nullptr) {
        boardView_->setHighlightNet(net);
        highlightNetRow(net);
    }
    const int componentIndex = item->data(0, Qt::UserRole + 1).toInt();
    if (componentIndex >= 0) selectComponentByIndex(componentIndex);
}

void MainWindow::zoomToSelectedPart() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    if (boardView_ == nullptr || !boardView_->hasGeometry()) {
        QMessageBox::information(this, tr("No board geometry"),
                                 tr("This document has no PCB geometry to zoom into."));
        return;
    }
    if (table_ == nullptr) return;
    const int row = table_->currentRow();
    const QTableWidgetItem* item = row >= 0 ? table_->item(row, 0) : nullptr;
    if (item == nullptr) {
        statusBar()->showMessage(tr("Select a component in the table first"), 4000);
        return;
    }
    const int index = item->data(Qt::UserRole + 1).toInt();
    if (index < 0) return;
    boardView_->setSelectedComponent(index);
    boardView_->zoomToComponent(index);
    if (tabs_ != nullptr) tabs_->setCurrentWidget(boardView_);
    statusBar()->showMessage(tr("Zoomed to %1").arg(item->text()), 4000);
}

void MainWindow::populateStructure() {
    structureTree_->clear();

    const bool asciiOnly = document_.structure.empty() && !document_.recordTypes.empty();
    structureTree_->setHeaderLabels(asciiOnly ? QStringList{tr("Record type"), tr("Count")}
                                              : QStringList{tr("Stream / storage"), tr("Size")});

    struct Builder {
        static void add(QTreeWidgetItem* parent, const pcbv::FileEntry& entry) {
            auto* item = new QTreeWidgetItem(parent);
            item->setText(0, qs(entry.name));
            if (entry.isStorage) {
                item->setText(1, "");
                item->setForeground(0, QColor("#8ab4f8"));
                for (const auto& child : entry.children) add(item, child);
                item->setExpanded(item->childCount() <= 12);
            } else {
                item->setText(1, formatSize(entry.size));
                item->setForeground(1, QColor("#9aa0a6"));
            }
        }
    };

    for (const auto& entry : document_.structure) {
        Builder::add(structureTree_->invisibleRootItem(), entry);
    }

    if (asciiOnly) {
        // ASCII export: no OLE storages — show the record-type histogram instead.
        int total = 0;
        for (const auto& entry : document_.recordTypes) {
            auto* item = new QTreeWidgetItem(structureTree_->invisibleRootItem());
            item->setText(0, qs(entry.first));
            item->setText(1, QString::number(entry.second));
            item->setForeground(1, QColor("#9aa0a6"));
            total += entry.second;
        }
        auto* totalItem = new QTreeWidgetItem(structureTree_->invisibleRootItem());
        totalItem->setText(0, tr("Total records"));
        totalItem->setText(1, QString::number(total));
        totalItem->setForeground(0, QColor("#8ab4f8"));
    }

    structureTree_->resizeColumnToContents(1);
}

// ---------------------------------------------------------------------------
// Selection / details
// ---------------------------------------------------------------------------

void MainWindow::highlightNetRow(const QString& net) {
    if (netlistTree_ == nullptr) return;
    for (int i = 0; i < netlistTree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = netlistTree_->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() != net) continue;
        QSignalBlocker blocker(netlistTree_);
        netlistTree_->setCurrentItem(item);
        netlistTree_->scrollToItem(item);
        return;
    }
}

void MainWindow::selectNet(const QString& net) {
    if (boardView_ != nullptr) boardView_->setHighlightNet(net);
    if (net.isEmpty()) {
        if (netlistTree_ != nullptr) {
            QSignalBlocker blocker(netlistTree_);
            netlistTree_->clearSelection();
        }
        return;
    }

    highlightNetRow(net);
    for (const auto& summary : document_.netSummaries) {
        if (qs(summary.name) == net) {
            detailsView_->setHtml(netDetailsHtml(summary));
            return;
        }
    }
}

QString MainWindow::netDetailsHtml(const pcbv::NetSummary& net) const {
    QString html;
    html += "<div style='font-family:sans-serif;padding:10px 14px'>";
    html += QString("<h2 style='margin:0 0 2px 0;color:#ffffff'>%1</h2>")
                .arg(qs(net.name).toHtmlEscaped());
    html += QString("<div style='color:#8ab4f8;font-size:14px;margin-bottom:10px'>%1</div>")
                .arg(tr("net"));

    auto row = [&html](const QString& key, const QString& value) {
        if (value.trimmed().isEmpty()) return;
        html += QString(
                    "<tr><td style='color:#9aa0a6;padding:3px 12px 3px 0;white-space:nowrap;"
                    "vertical-align:top'>%1</td><td style='color:#e3e6ea'>%2</td></tr>")
                    .arg(key, value);
    };
    html += "<table style='font-size:13px;border-collapse:collapse'>";
    row(tr("Pads"), QString::number(net.pads));
    row(tr("Track segments"), QString::number(net.tracks));
    row(tr("Vias"), QString::number(net.vias));
    row(tr("Copper length"), tr("%1 mm").arg(qs(pcbv::formatMm(net.trackLength))));
    html += "</table>";

    if (!net.connections.empty()) {
        html += tr("<h3 style='color:#8ab4f8;font-size:13px;margin:16px 0 6px 0;"
                   "text-transform:uppercase;letter-spacing:.06em'>Connected pads (%1)</h3>")
                    .arg(static_cast<int>(net.connections.size()));
        html += "<div style='font-size:12.5px;color:#cfd8dc;line-height:1.7'>";
        for (std::size_t i = 0; i < net.connections.size(); ++i) {
            if (i > 0) html += "&nbsp;·&nbsp;";
            html += qs(net.connections[i]).toHtmlEscaped();
        }
        html += "</div>";
    }
    html += "<p style='color:#8a929e;font-size:12px;margin-top:16px'>"
            + tr("Click an empty spot on the board to clear the highlight.") + "</p>";
    html += "</div>";
    return html;
}

void MainWindow::applyFilter(const QString& text) {
    if (table_ == nullptr) return;
    const QString needle = text.trimmed();
    int visible = 0;
    for (int row = 0; row < table_->rowCount(); ++row) {
        bool match = needle.isEmpty();
        if (!match) {
            for (int col = 0; col < table_->columnCount(); ++col) {
                const QTableWidgetItem* item = table_->item(row, col);
                if (item != nullptr && item->text().contains(needle, Qt::CaseInsensitive)) {
                    match = true;
                    break;
                }
            }
        }
        table_->setRowHidden(row, !match);
        if (match) ++visible;
    }
    if (hasDocument_) {
        tableInfoLabel_->setText(needle.isEmpty()
                                     ? tr("%1 components").arg(document_.components.size())
                                     : tr("%1 of %2 shown").arg(visible).arg(document_.components.size()));
    }
}

void MainWindow::showSelectedComponent() {
    if (!hasDocument_ || table_ == nullptr) return;
    const int row = table_->currentRow();
    if (row < 0) return;
    const QTableWidgetItem* designatorItem = table_->item(row, 0);
    if (designatorItem == nullptr) return;
    const int index = designatorItem->data(Qt::UserRole + 1).toInt();
    if (index < 0 || index >= static_cast<int>(document_.components.size())) return;
    detailsView_->setHtml(
        componentDetailsHtml(document_.components[static_cast<std::size_t>(index)]));
    if (boardView_ != nullptr) boardView_->setSelectedComponent(index);
}

QString MainWindow::componentDetailsHtml(const pcbv::Component& c) const {
    QString html;
    html += "<div style='font-family:sans-serif;padding:10px 14px'>";
    html += QString("<h2 style='margin:0 0 2px 0;color:#ffffff'>%1</h2>")
                .arg(c.designator.empty() ? tr("(no designator)") : qs(c.designator));
    if (!c.comment.empty() || !c.name.empty()) {
        html += QString("<div style='color:#8ab4f8;font-size:14px;margin-bottom:10px'>%1</div>")
                    .arg(qs(c.comment.empty() ? c.name : c.comment));
    }

    auto row = [&html](const QString& key, const QString& value) {
        if (value.trimmed().isEmpty()) return;
        html += QString(
                    "<tr><td style='color:#9aa0a6;padding:3px 12px 3px 0;white-space:nowrap;"
                    "vertical-align:top'>%1</td><td style='color:#e3e6ea'>%2</td></tr>")
                    .arg(key, value);
    };

    html += "<table style='font-size:13px;border-collapse:collapse'>";
    row(tr("Footprint"), qs(c.footprint));
    row(tr("Description"), qs(c.description));
    row(tr("Manufacturer"), qs(c.manufacturer));
    row(tr("Part number"), qs(c.partNumber));
    row(tr("Supplier"), qs(c.supplier));
    row(tr("Supplier P/N"), qs(c.supplierPartNumber));
    if (!c.datasheet.empty()) {
        row(tr("Datasheet"),
            QString("<a style='color:#8ab4f8' href=\"%1\">%1</a>").arg(qs(c.datasheet).toHtmlEscaped()));
    }
    row(tr("Layer"), qs(c.layer));
    if (c.x != 0.0 || c.y != 0.0) {
        row(tr("Position"), QString("%1, %2 mm &nbsp;<span style='color:#9aa0a6'>(%3, %4 mil)</span>")
                                .arg(qs(pcbv::formatMm(c.x)), qs(pcbv::formatMm(c.y)),
                                     qs(pcbv::formatMil(c.x)), qs(pcbv::formatMil(c.y))));
    }
    if (c.rotation != 0.0) row(tr("Rotation"), QString("%1°").arg(c.rotation, 0, 'f', 2));
    if (c.height > 0.0) row(tr("Height"), QString("%1 mm").arg(qs(pcbv::formatMm(c.height))));
    if (c.pinCount > 0) row(tr("Pins"), QString::number(c.pinCount));
    if (c.padCount > 0) {
        row(tr("Pads"), QString("%1 (%2 through-hole)").arg(c.padCount).arg(c.throughHolePads));
    }
    if (c.partCount > 1) row(tr("Sub-parts"), QString::number(c.partCount));
    row(tr("Source library"), qs(c.sourceLibrary));
    row(tr("Footprint library"), qs(c.sourceFootprintLibrary));
    row(tr("Unique ID"), qs(c.uniqueId));
    html += "</table>";

    if (!c.pins.empty()) {
        html += tr("<h3 style='color:#8ab4f8;font-size:13px;margin:16px 0 6px 0;"
                   "text-transform:uppercase;letter-spacing:.06em'>Pins (%1)</h3>")
                    .arg(c.pins.size());
        html += "<table style='font-size:12.5px;border-collapse:collapse;width:100%'>";
        html += tr("<tr style='color:#9aa0a6;text-align:left'><th>#</th><th>Name</th>"
                   "<th>Type</th></tr>");
        for (const auto& pin : c.pins) {
            html += QString("<tr><td style='color:#b0bec5;padding:2px 10px 2px 0'>%1</td>"
                            "<td style='padding:2px 10px 2px 0'>%2</td>"
                            "<td style='color:#9aa0a6'>%3</td></tr>")
                        .arg(qs(pin.designator).toHtmlEscaped(), qs(pin.name).toHtmlEscaped(),
                             qs(pin.electrical));
        }
        html += "</table>";
    }

    if (!c.pads.empty()) {
        html += tr("<h3 style='color:#8ab4f8;font-size:13px;margin:16px 0 6px 0;"
                   "text-transform:uppercase;letter-spacing:.06em'>Pads (%1)</h3>")
                    .arg(c.pads.size());
        html += "<table style='font-size:12.5px;border-collapse:collapse;width:100%'>";
        html += tr("<tr style='color:#9aa0a6;text-align:left'><th>#</th><th>Layer</th><th>Net</th>"
                   "<th>Size (mm)</th><th>Hole</th></tr>");
        for (const auto& pad : c.pads) {
            html += QString("<tr><td style='color:#b0bec5;padding:2px 8px 2px 0'>%1</td>"
                            "<td style='padding:2px 8px 2px 0'>%2</td>"
                            "<td style='padding:2px 8px 2px 0;color:#a5d6a7'>%3</td>"
                            "<td style='padding:2px 8px 2px 0'>%4 × %5</td>"
                            "<td style='color:#9aa0a6'>%6</td></tr>")
                        .arg(qs(pad.designator).toHtmlEscaped(), qs(pad.layerName),
                             qs(pad.net).toHtmlEscaped(), qs(pcbv::formatMm(pad.width)),
                             qs(pcbv::formatMm(pad.height)),
                             pad.holeSize > 0 ? qs(pcbv::formatMm(pad.holeSize)) : QString("—"));
        }
        html += "</table>";
    }

    if (!c.properties.empty()) {
        html += tr("<h3 style='color:#8ab4f8;font-size:13px;margin:16px 0 6px 0;"
                   "text-transform:uppercase;letter-spacing:.06em'>All parameters (%1)</h3>")
                    .arg(c.properties.size());
        html += "<table style='font-size:12px;border-collapse:collapse;width:100%'>";
        for (const auto& p : c.properties) {
            html += QString("<tr><td style='color:#9aa0a6;padding:2px 10px 2px 0;"
                            "white-space:nowrap;vertical-align:top'>%1</td>"
                            "<td style='color:#d7dce1;word-break:break-word'>%2</td></tr>")
                        .arg(qs(p.name).toHtmlEscaped(), qs(p.value).toHtmlEscaped());
        }
        html += "</table>";
    }

    html += "</div>";
    return html;
}

QString MainWindow::documentSummaryHtml() const {
    if (!hasDocument_) return QString();
    const pcbv::Document& d = document_;
    QString html = "<div style='font-family:sans-serif;padding:12px 16px'>";
    html += QString("<h2 style='margin:0 0 4px 0;color:#fff'>%1</h2>").arg(qs(d.fileName).toHtmlEscaped());
    html += QString("<div style='color:#8ab4f8;margin-bottom:12px'>%1</div>")
                .arg(qs(pcbv::docTypeName(d.type)));

    auto row = [&html](const QString& key, const QString& value) {
        if (value.isEmpty()) return;
        html += QString("<tr><td style='color:#9aa0a6;padding:3px 14px 3px 0;white-space:nowrap'>%1</td>"
                        "<td style='color:#e3e6ea'>%2</td></tr>")
                    .arg(key, value);
    };

    html += "<table style='font-size:13px;border-collapse:collapse'>";
    row(tr("Path"), qs(d.path).toHtmlEscaped());
    row(tr("Container"), qs(pcbv::containerName(d.container)));
    row(tr("Format"), qs(d.formatVersion).toHtmlEscaped());
    if (!d.boardVersion.empty()) row(tr("Board format"), qs(d.boardVersion));
    if (!d.lastSaved.empty()) row(tr("Last saved"), qs(d.lastSaved));
    row(tr("File size"), formatSize(d.fileSize));
    if (!d.structure.empty()) row(tr("Root entries"), QString::number(d.structure.size()));
    if (!d.recordTypes.empty()) {
        int total = 0;
        for (const auto& entry : d.recordTypes) total += entry.second;
        row(tr("Records"), QString::number(total));
    }
    html += "</table>";

    html += "<div style='display:flex;flex-wrap:wrap;gap:8px;margin:16px 0'>";
    auto card = [&html](const QString& key, const QString& value) {
        html += QString("<div style='background:#1f232b;border:1px solid #2c313a;border-radius:8px;"
                        "padding:8px 14px;min-width:86px'><div style='font-size:19px;font-weight:600;"
                        "color:#fff'>%1</div><div style='font-size:10.5px;color:#9aa0a6;"
                        "text-transform:uppercase;letter-spacing:.06em'>%2</div></div>")
                    .arg(value, key);
    };
    card(tr("Components"), QString::number(d.components.size()));
    if (d.type == pcbv::DocType::PcbDoc) {
        card(tr("Pads"), QString::number(d.pcb.pads));
        card(tr("Tracks"), QString::number(d.pcb.tracks));
        card(tr("Vias"), QString::number(d.pcb.vias));
        card(tr("Nets"), QString::number(d.pcb.nets));
        if (d.bounds.valid) {
            card(tr("Board (mm)"),
                 QString("%1 × %2").arg(qs(pcbv::formatMm(d.bounds.width())),
                                        qs(pcbv::formatMm(d.bounds.height()))));
        }
    } else {
        card(tr("Pins"), QString::number(d.sch.pins));
        card(tr("Wires"), QString::number(d.sch.wires));
        card(tr("Net labels"), QString::number(d.sch.netLabels));
        card(tr("Ports"), QString::number(d.sch.ports));
    }
    html += "</div>";

    html += QString("<p style='color:#9aa0a6;font-size:12px'>%1</p>")
                .arg(tr("Select a component in the table to inspect every parameter, pin and pad."));
    html += "</div>";
    return html;
}

void MainWindow::showDocumentSummary() {
    if (!hasDocument_) return;
    table_->clearSelection();
    detailsView_->setHtml(documentSummaryHtml());
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

QString MainWindow::exportDirectory() const {
    if (!hasDocument_) return QDir::homePath();
    return QFileInfo(qs(document_.path)).absolutePath();
}

void MainWindow::exportCsv() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    const QString suggested = exportDirectory() + "/" +
                              QFileInfo(qs(document_.fileName)).completeBaseName() + "_bom.csv";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export BOM as CSV"), suggested, tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export failed"), tr("Cannot write:\n%1").arg(path));
        return;
    }
    const QByteArray data = QByteArray::fromStdString(pcbv::toCsv(document_));
    file.write(data);
    file.close();
    log(tr("Exported CSV: %1").arg(path));
    statusBar()->showMessage(tr("Exported %1").arg(path), 6000);
}

void MainWindow::exportJson() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    const QString suggested = exportDirectory() + "/" +
                              QFileInfo(qs(document_.fileName)).completeBaseName() + ".json";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export as JSON"), suggested, tr("JSON files (*.json);;All files (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export failed"), tr("Cannot write:\n%1").arg(path));
        return;
    }
    file.write(QByteArray::fromStdString(pcbv::toJson(document_)));
    file.close();
    log(tr("Exported JSON: %1").arg(path));
    statusBar()->showMessage(tr("Exported %1").arg(path), 6000);
}

void MainWindow::exportHtml() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    const QString suggested = exportDirectory() + "/" +
                              QFileInfo(qs(document_.fileName)).completeBaseName() + "_report.html";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export HTML report"), suggested, tr("HTML files (*.html);;All files (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export failed"), tr("Cannot write:\n%1").arg(path));
        return;
    }
    file.write(QByteArray::fromStdString(pcbv::toHtml(document_)));
    file.close();
    log(tr("Exported HTML: %1").arg(path));
    statusBar()->showMessage(tr("Exported %1").arg(path), 6000);
}

void MainWindow::copyBomToClipboard() {
    if (!hasDocument_) {
        ensureDocumentLoaded();
        return;
    }
    QStringList header;
    for (int col = 0; col < table_->columnCount(); ++col) {
        const QTableWidgetItem* headerItem = table_->horizontalHeaderItem(col);
        header << (headerItem != nullptr ? headerItem->text() : QString());
    }
    QString text = header.join(QStringLiteral("\t")) + QStringLiteral("\n");
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (table_->isRowHidden(row)) continue;
        QStringList fields;
        for (int col = 0; col < table_->columnCount(); ++col) {
            const QTableWidgetItem* item = table_->item(row, col);
            fields << (item != nullptr ? item->text() : QString());
        }
        text += fields.join(QStringLiteral("\t")) + QStringLiteral("\n");
    }
    QApplication::clipboard()->setText(text);
    statusBar()->showMessage(tr("Copied the visible component table to the clipboard"), 5000);
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

void MainWindow::log(const QString& message) {
    // The log is a diagnostics panel: it stays out of the way until it has bad news.
    if (logDock_ != nullptr && !logDock_->isVisible() &&
        (message.startsWith(QLatin1String("ERROR")) || message.startsWith(QLatin1String("warning")))) {
        logDock_->show();
    }
    if (logView_ == nullptr) return;
    logView_->appendPlainText(message);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/windowState"), saveState());
    settings.setValue(QStringLiteral("ui/lastDirectory"), lastDirectory_);
    saveSession();
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty()) return;
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) openDocument(url.toLocalFile());
    }
    event->acceptProposedAction();
}
