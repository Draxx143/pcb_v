#include "MainWindow.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QColor>
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
#include <QPushButton>
#include <QPalette>
#include <QStyleFactory>
#include <QStringList>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <set>

#include "BoardView.h"
#include "Icons.h"
#include "core/LayerStyle.h"
#include "core/AltiumParser.h"
#include "core/Bom.h"
#include "core/Export.h"
#include "core/Netlist.h"

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
        tr("Open an Altium .PcbDoc or .SchDoc file (Ctrl+O), or drag & drop it here."));

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

    netlistCsvAction_ = new QAction(tr("Export &netlist as CSV…"), this);
    netlistCsvAction_->setToolTip(tr("Every pad with its net, layer and position"));
    connect(netlistCsvAction_, &QAction::triggered, this, &MainWindow::exportNetlistCsv);

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

    exitAction_ = new QAction(tr("E&xit"), this);
    exitAction_->setShortcut(QKeySequence::Quit);
    connect(exitAction_, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::createMenus() {
    // File: open, one export submenu, quit.
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(openAction_);
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
    editMenu->addAction(findAction_);
    editMenu->addAction(copyAction_);
    addAction(findAction_);

    // View: panel visibility, board fit, summary.
    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    QMenu* panelsMenu = viewMenu->addMenu(tr("&Panels"));
    if (structureDock_) panelsMenu->addAction(structureDock_->toggleViewAction());
    if (detailsDock_) panelsMenu->addAction(detailsDock_->toggleViewAction());
    if (layersDock_) panelsMenu->addAction(layersDock_->toggleViewAction());
    if (netsDock_) panelsMenu->addAction(netsDock_->toggleViewAction());
    if (logDock_) panelsMenu->addAction(logDock_->toggleViewAction());
    viewMenu->addSeparator();
    viewMenu->addAction(fitAction_);
    viewMenu->addSeparator();
    viewMenu->addAction(summaryAction_);

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction* shortcutsAction = helpMenu->addAction(tr("&Keyboard shortcuts"));
    connect(shortcutsAction, &QAction::triggered, this, [this]() {
        QMessageBox::information(this, tr("Keyboard shortcuts"),
                                 tr("<table cellpadding='3'>"
                                    "<tr><td><b>Ctrl+O</b></td><td>open a document</td></tr>"
                                    "<tr><td><b>Ctrl+F</b></td><td>jump to the filter field</td></tr>"
                                    "<tr><td><b>F5</b></td><td>document summary</td></tr>"
                                    "<tr><td><b>Ctrl+C</b></td><td>copy the component table</td></tr>"
                                    "<tr><td><b>Ctrl+Shift+C</b></td><td>export the BOM as CSV</td></tr>"
                                    "<tr><td><b>Ctrl++ / Ctrl+-</b></td><td>zoom the board</td></tr>"
                                    "<tr><td><b>Ctrl+0</b></td><td>fit the board in the window</td></tr>"
                                    "<tr><td><b>Click a track or pad</b></td><td>highlight that net</td></tr>"
                                    "<tr><td><b>Esc</b></td><td>clear the net highlight</td></tr>"
                                    "<tr><td><b>Ctrl+Shift+D</b></td><td>compare with another document</td></tr>"
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
    netlistTree_->setHeaderLabels({tr("Net / connection"), tr("Layer"), tr("X (mm)"), tr("Y (mm)")});
    netlistTree_->setRootIsDecorated(true);
    netlistTree_->setUniformRowHeights(true);
    netlistTree_->setAlternatingRowColors(true);
    netlistTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    netlistTree_->setColumnWidth(0, 300);
    netlistTree_->setColumnWidth(1, 150);
    netlistTree_->setColumnWidth(2, 90);
    netlistTree_->setColumnWidth(3, 90);
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

    setCentralWidget(tabs_);
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
                               Named{exportAction_, "Export"}}) {
        entry.action->setIconText(QString::fromLatin1(entry.name));
    }

    toolbar->addAction(openAction_);
    toolbar->addSeparator();
    toolbar->addAction(zoomOutAction_);
    toolbar->addAction(zoomInAction_);
    toolbar->addAction(fitAction_);
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

void MainWindow::populateNets() {
    if (netsTable_ == nullptr) return;
    netsTable_->setSortingEnabled(false);
    netsTable_->setRowCount(0);
    netsTable_->setRowCount(static_cast<int>(document_.netSummaries.size()));

    for (int row = 0; row < static_cast<int>(document_.netSummaries.size()); ++row) {
        const pcbv::NetSummary& net = document_.netSummaries[static_cast<std::size_t>(row)];
        auto* nameItem = new QTableWidgetItem(qs(net.name));
        nameItem->setData(Qt::UserRole, QString::fromStdString(net.name));
        netsTable_->setItem(row, 0, nameItem);
        netsTable_->setItem(row, 1, new NumericItem(QString::number(net.pads), net.pads));
        netsTable_->setItem(row, 2, new NumericItem(QString::number(net.tracks), net.tracks));
        netsTable_->setItem(row, 3, new NumericItem(QString::number(net.vias), net.vias));
        netsTable_->setItem(row, 4,
                            new MeasureItem(qs(pcbv::formatMm(net.trackLength)), net.trackLength));
    }
    netsTable_->setSortingEnabled(true);
    if (netsInfo_ != nullptr) {
        netsInfo_->setText(document_.netSummaries.empty()
                               ? tr("no nets")
                               : tr("%1 nets").arg(static_cast<int>(document_.netSummaries.size())));
    }
    applyNetFilter(netFilter_ != nullptr ? netFilter_->text() : QString());
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

    // --- nets ---------------------------------------------------------------
    QWidget* netsPage = new QWidget(this);
    QVBoxLayout* netsLayout = new QVBoxLayout(netsPage);
    netsLayout->setContentsMargins(4, 4, 4, 4);
    netsLayout->setSpacing(4);

    QHBoxLayout* netFilterRow = new QHBoxLayout();
    netFilter_ = new QLineEdit(netsPage);
    netFilter_->setPlaceholderText(tr("Filter nets…"));
    netFilter_->setClearButtonEnabled(true);
    netsInfo_ = new QLabel(tr("no document"), netsPage);
    netsInfo_->setStyleSheet("color:#8a929e;");
    netFilterRow->addWidget(netFilter_, 1);
    netFilterRow->addWidget(netsInfo_);
    netsLayout->addLayout(netFilterRow);

    netsTable_ = new QTableWidget(0, 5, netsPage);
    netsTable_->setHorizontalHeaderLabels({tr("Net"), tr("Pads"), tr("Tracks"), tr("Vias"),
                                           tr("Length (mm)")});
    netsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    netsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    netsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    netsTable_->setAlternatingRowColors(true);
    netsTable_->setSortingEnabled(true);
    netsTable_->setShowGrid(false);
    netsTable_->verticalHeader()->setVisible(false);
    netsTable_->horizontalHeader()->setStretchLastSection(true);
    netsTable_->setColumnWidth(0, 130);
    netsTable_->setColumnWidth(1, 50);
    netsTable_->setColumnWidth(2, 55);
    netsTable_->setColumnWidth(3, 45);
    netsLayout->addWidget(netsTable_, 1);
    connect(netFilter_, &QLineEdit::textChanged, this, &MainWindow::applyNetFilter);
    connect(netsTable_, &QTableWidget::itemSelectionChanged, this, [this]() {
        const auto items = netsTable_->selectedItems();
        if (items.isEmpty()) return;
        const QTableWidgetItem* item = netsTable_->item(netsTable_->currentRow(), 0);
        if (item == nullptr) return;
        selectNet(item->data(Qt::UserRole).toString());
        // Picking a net is a request to look at it, so bring the board forward.
        if (tabs_ != nullptr && boardView_ != nullptr && boardView_->hasGeometry()) {
            tabs_->setCurrentWidget(boardView_);
        }
    });

    netsDock_ = new QDockWidget(tr("Nets"), this);
    netsDock_->setObjectName("netsDock");
    netsDock_->setWidget(netsPage);
    addDockWidget(Qt::RightDockWidgetArea, netsDock_);

    logDock_ = new QDockWidget(tr("Parser log"), this);
    logDock_->setObjectName("logDock");
    logDock_->setWidget(logView_);
    addDockWidget(Qt::BottomDockWidgetArea, logDock_);
    resizeDocks({logDock_}, {150}, Qt::Vertical);

    // Structure and Layers share the left side; Nets sits on the right so a net can be
    // picked while the board stays visible.
    tabifyDockWidget(structureDock_, layersDock_);
    structureDock_->raise();
    resizeDocks({structureDock_, netsDock_}, {320, 300}, Qt::Horizontal);
    resizeDocks({detailsDock_, netsDock_}, {420, 260}, Qt::Vertical);
}

// ---------------------------------------------------------------------------
// Document loading
// ---------------------------------------------------------------------------

void MainWindow::openFileDialog() {
    const QString startDir = lastDirectory_;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Altium document"), startDir,
        tr("Altium documents (*.PcbDoc *.SchDoc *.pcbdoc *.schdoc);;All files (*)"));
    if (!path.isEmpty()) openDocument(path);
}

void MainWindow::openDocument(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        QMessageBox::warning(this, tr("Open document"),
                             tr("File not found:\n%1").arg(path));
        return;
    }
    const pcbv::DocType type = pcbv::detectType(path.toStdString());
    if (type == pcbv::DocType::PcbLib || type == pcbv::DocType::SchLib) {
        QMessageBox::information(this, tr("Not supported yet"),
                                 tr("%1\n\nLibrary files (.PcbLib/.SchLib) are planned for a later "
                                    "step. Please open a .PcbDoc or .SchDoc.").arg(info.fileName()));
        return;
    }
    if (type == pcbv::DocType::Unknown) {
        QMessageBox::warning(this, tr("Unknown file type"),
                             tr("%1\n\nExpected an Altium .PcbDoc or .SchDoc file.").arg(info.fileName()));
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
        QMessageBox box(this);
        box.setIcon(QMessageBox::Critical);
        box.setWindowTitle(tr("Cannot read document"));
        box.setText(tr("%1 could not be opened.").arg(info.fileName()));
        box.setInformativeText(details);
        if (!outcome.detected.empty())
            box.setDetailedText(tr("File: %1\nSize: %2 bytes\nDetected: %3\nSignature: %4\n\n%5")
                                    .arg(info.absoluteFilePath())
                                    .arg(info.size())
                                    .arg(qs(outcome.detected), qs(outcome.signature), qs(outcome.error)));
        box.exec();
        log(tr("ERROR  %1 — %2").arg(info.fileName(), qs(outcome.error)));
        if (!outcome.detected.empty()) log(tr("       detected: %1").arg(qs(outcome.detected)));
        return;
    }
    setDocument(outcome.document);
}

void MainWindow::setDocument(const pcbv::Document& document) {
    document_ = document;
    hasDocument_ = true;
    lastDirectory_ = QFileInfo(qs(document_.path)).absolutePath();

    setWindowTitle(QString("%1 — PCB Viewer").arg(qs(document_.fileName)));

    // A comparison belongs to the previous pair of documents.
    if (tabs_ != nullptr && comparePage_ != nullptr) {
        tabs_->setTabVisible(tabs_->indexOf(comparePage_), false);
    }
    populateComponents();
    populateBom();
    populateNetlist();
    populateStructure();
    populateLayers();
    populateNets();
    showDocumentSummary();

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

void MainWindow::updateActionStates() {
    const bool hasDocument = hasDocument_;
    const bool hasBoard = hasDocument && boardView_ != nullptr && boardView_->hasGeometry();

    // Document level commands (BOM, reports, summary).
    for (QAction* action : {csvAction_, bomCsvAction_, bomHtmlAction_, netlistCsvAction_, jsonAction_,
                            htmlAction_,
                            exportAction_, copyAction_, summaryAction_, compareAction_}) {
        if (action != nullptr) action->setEnabled(hasDocument);
    }
    // Board level commands only make sense for documents with drawable geometry.
    for (QAction* action : {svgAction_, netlistCsvAction_, fitAction_, zoomInAction_,
                            zoomOutAction_}) {
        if (action != nullptr) action->setEnabled(hasBoard);
    }
    if (netsDock_ != nullptr) netsDock_->setEnabled(hasBoard);
    if (layersDock_ != nullptr) layersDock_->setEnabled(hasBoard);
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
        if (!noNet && net.nodes.size() == 1) {
            netItem->setForeground(0, QColor(0xe5, 0x7c, 0x73));  // a single pad = unfinished
            netItem->setToolTip(0, tr("Only one pad on this net — the routing looks unfinished."));
        }

        for (const auto& node : net.nodes) {
            auto* child = new QTreeWidgetItem(netItem);
            child->setText(0, qs(node.label));
            child->setText(1, qs(node.layer));
            child->setText(2, qs(pcbv::formatMm(node.x)));
            child->setText(3, qs(pcbv::formatMm(node.y)));
            child->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            child->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
            child->setForeground(2, QColor(0x9a, 0xa0, 0xa6));
            child->setForeground(3, QColor(0x9a, 0xa0, 0xa6));
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
            for (int column = 0; column < 2 && !match; ++column) {
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

void MainWindow::applyNetFilter(const QString& text) {
    if (netsTable_ == nullptr) return;
    const QString needle = text.trimmed();
    int visible = 0;
    for (int row = 0; row < netsTable_->rowCount(); ++row) {
        const QTableWidgetItem* item = netsTable_->item(row, 0);
        const bool match = needle.isEmpty() ||
                           (item != nullptr && item->text().contains(needle, Qt::CaseInsensitive));
        netsTable_->setRowHidden(row, !match);
        if (match) ++visible;
    }
    if (netsInfo_ != nullptr && hasDocument_) {
        const int total = static_cast<int>(document_.netSummaries.size());
        if (total == 0) {
            netsInfo_->setText(tr("no nets"));
        } else {
            netsInfo_->setText(needle.isEmpty() ? tr("%1 nets").arg(total)
                                                : tr("%1 of %2").arg(visible).arg(total));
        }
    }
}

void MainWindow::highlightNetRow(const QString& net) {
    if (netsTable_ == nullptr) return;
    for (int row = 0; row < netsTable_->rowCount(); ++row) {
        const QTableWidgetItem* item = netsTable_->item(row, 0);
        if (item == nullptr) continue;
        if (item->data(Qt::UserRole).toString() == net) {
            QSignalBlocker blocker(netsTable_);
            netsTable_->selectRow(row);
            netsTable_->scrollToItem(item);
            return;
        }
    }
}

void MainWindow::selectNet(const QString& net) {
    if (boardView_ != nullptr) boardView_->setHighlightNet(net);
    if (net.isEmpty()) {
        if (netsTable_ != nullptr) {
            QSignalBlocker blocker(netsTable_);
            netsTable_->clearSelection();
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
    if (logView_ == nullptr) return;
    logView_->appendPlainText(message);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/windowState"), saveState());
    settings.setValue(QStringLiteral("ui/lastDirectory"), lastDirectory_);
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
    openDocument(urls.first().toLocalFile());
    event->acceptProposedAction();
}
