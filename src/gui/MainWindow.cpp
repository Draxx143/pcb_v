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
#include <QPlainTextEdit>
#include <QStringList>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>

#include "core/AltiumParser.h"
#include "core/Export.h"

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

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("PCB Viewer — Altium .PcbDoc / .SchDoc component reader");
    resize(1500, 900);
    setAcceptDrops(true);
    setDockNestingEnabled(true);

    applyTheme();
    createComponentTable();
    createDockPanels();
    createActions();
    createMenus();

    statusBar()->showMessage(
        tr("Open an Altium .PcbDoc or .SchDoc file (Ctrl+O), or drag & drop it here."));

    detailsView_->setHtml(
        "<div style='color:#9aa0a6;font-family:sans-serif;padding:18px'>"
        "<h2 style='color:#e6e6e6;margin-top:0'>No document loaded</h2>"
        "<p>Open an Altium <b>.PcbDoc</b> (PCB) or <b>.SchDoc</b> (schematic) file.</p>"
        "<p>Every component is listed with its designator, value/comment, footprint, "
        "manufacturer, part number, pins/pads and the complete raw parameter set.</p>"
        "</div>");
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

void MainWindow::applyTheme() {
    qApp->setStyle("Fusion");
    qApp->setStyleSheet(R"(
QMainWindow, QDialog { background: #1b1e24; }
QWidget { color: #e3e6ea; font-size: 13px; }
QMenuBar { background: #23262d; border-bottom: 1px solid #33383f; }
QMenuBar::item { padding: 5px 10px; background: transparent; }
QMenuBar::item:selected { background: #2f353e; border-radius: 4px; }
QMenu { background: #23262d; border: 1px solid #33383f; padding: 4px; }
QMenu::item { padding: 6px 22px; border-radius: 4px; }
QMenu::item:selected { background: #2a5d9f; }
QMenu::separator { height: 1px; background: #33383f; margin: 4px 8px; }
QDockWidget { color: #cfd8dc; titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title { background: #23262d; padding: 6px 10px; border: 1px solid #2c313a; }
QTableWidget { background: #171a20; alternate-background-color: #1b1f26; gridline-color: #262b34;
               selection-background-color: #2a5d9f; selection-color: #ffffff; border: 1px solid #2c313a; }
QHeaderView::section { background: #242a33; color: #cfd8dc; padding: 6px; border: 0;
                       border-right: 1px solid #2c313a; border-bottom: 1px solid #2c313a; }
QLineEdit, QPlainTextEdit, QTextBrowser, QTreeWidget { background: #171a20; border: 1px solid #2c313a;
                       border-radius: 4px; selection-background-color: #2a5d9f; }
QLineEdit { padding: 6px 8px; }
QTreeWidget::item { padding: 3px 2px; }
QTreeWidget::item:selected { background: #2a5d9f; }
QStatusBar { background: #23262d; color: #9aa0a6; }
QSplitter::handle { background: #2c313a; }
QScrollBar:vertical { background: #171a20; width: 12px; margin: 0; }
QScrollBar::handle:vertical { background: #3a414b; border-radius: 6px; min-height: 24px; }
QScrollBar:horizontal { background: #171a20; height: 12px; }
QScrollBar::handle:horizontal { background: #3a414b; border-radius: 6px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QToolTip { background: #23262d; color: #e3e6ea; border: 1px solid #3a414b; padding: 4px; }
)");
}

// ---------------------------------------------------------------------------
// Actions / menus
// ---------------------------------------------------------------------------

void MainWindow::createActions() {
    openAction_ = new QAction(tr("&Open PCB / Schematic…"), this);
    openAction_->setShortcut(QKeySequence::Open);
    openAction_->setStatusTip(tr("Open an Altium .PcbDoc or .SchDoc document"));
    connect(openAction_, &QAction::triggered, this, &MainWindow::openFileDialog);

    csvAction_ = new QAction(tr("Export BOM as &CSV…"), this);
    csvAction_->setShortcut(QKeySequence("Ctrl+Shift+C"));
    connect(csvAction_, &QAction::triggered, this, &MainWindow::exportCsv);

    jsonAction_ = new QAction(tr("Export as &JSON…"), this);
    connect(jsonAction_, &QAction::triggered, this, &MainWindow::exportJson);

    htmlAction_ = new QAction(tr("Export &HTML report…"), this);
    connect(htmlAction_, &QAction::triggered, this, &MainWindow::exportHtml);

    copyAction_ = new QAction(tr("&Copy component table"), this);
    copyAction_->setShortcut(QKeySequence::Copy);
    connect(copyAction_, &QAction::triggered, this, &MainWindow::copyBomToClipboard);

    summaryAction_ = new QAction(tr("Document s&ummary"), this);
    connect(summaryAction_, &QAction::triggered, this, &MainWindow::showDocumentSummary);

    exitAction_ = new QAction(tr("E&xit"), this);
    exitAction_->setShortcut(QKeySequence::Quit);
    connect(exitAction_, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::createMenus() {
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(openAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(csvAction_);
    fileMenu->addAction(jsonAction_);
    fileMenu->addAction(htmlAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(copyAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(exitAction_);

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    if (structureDock_) viewMenu->addAction(structureDock_->toggleViewAction());
    if (detailsDock_) viewMenu->addAction(detailsDock_->toggleViewAction());
    if (logDock_) viewMenu->addAction(logDock_->toggleViewAction());
    viewMenu->addSeparator();
    viewMenu->addAction(summaryAction_);

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction* aboutAction = helpMenu->addAction(tr("&About"));
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, tr("About PCB Viewer"),
                           tr("<h3>PCB Viewer</h3>"
                              "<p>Altium .PcbDoc / .SchDoc reader — step 1: component list.</p>"
                              "<p>Reads the OLE2 compound container and Altium's binary record format "
                              "directly: no Altium installation and no proprietary library required.</p>"
                              "<p>Coordinates are shown in mm (1 mil = 0.0254 mm).</p>"));
    });
    QAction* shortcutsAction = helpMenu->addAction(tr("&Keyboard shortcuts"));
    connect(shortcutsAction, &QAction::triggered, this, [this]() {
        QMessageBox::information(this, tr("Keyboard shortcuts"),
                                 tr("<b>Ctrl+O</b> — open document<br>"
                                    "<b>Ctrl+Shift+C</b> — export CSV<br>"
                                    "<b>Ctrl+C</b> — copy component table<br>"
                                    "<b>Ctrl+F</b> — jump to the filter field<br>"
                                    "<b>F5</b> — show document summary"));
    });

    QAction* findAction = new QAction(this);
    findAction->setShortcut(QKeySequence::Find);
    addAction(findAction);
    connect(findAction, &QAction::triggered, this, [this]() {
        if (filterEdit_) {
            filterEdit_->setFocus();
            filterEdit_->selectAll();
        }
    });

    QAction* refreshAction = new QAction(this);
    refreshAction->setShortcut(QKeySequence(Qt::Key_F5));
    addAction(refreshAction);
    connect(refreshAction, &QAction::triggered, this, &MainWindow::showDocumentSummary);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::createComponentTable() {
    QWidget* central = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    QHBoxLayout* filterRow = new QHBoxLayout();
    QLabel* filterLabel = new QLabel(tr("Filter:"), central);
    filterEdit_ = new QLineEdit(central);
    filterEdit_->setPlaceholderText(
        tr("Type a designator, value, footprint, manufacturer … (case-insensitive)"));
    filterEdit_->setClearButtonEnabled(true);
    tableInfoLabel_ = new QLabel(tr("no document"), central);
    tableInfoLabel_->setStyleSheet("color:#9aa0a6;");
    filterRow->addWidget(filterLabel);
    filterRow->addWidget(filterEdit_, 1);
    filterRow->addWidget(tableInfoLabel_);
    layout->addLayout(filterRow);

    table_ = new QTableWidget(0, 10, central);
    table_->setHorizontalHeaderLabels({tr("#"), tr("Designator"), tr("Comment / Value"),
                                       tr("Footprint"), tr("Manufacturer"), tr("MPN"),
                                       tr("Pins"), tr("Pads"), tr("Layer"), tr("Description")});
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
    table_->setColumnWidth(0, 46);
    table_->setColumnWidth(1, 110);
    table_->setColumnWidth(2, 170);
    table_->setColumnWidth(3, 240);
    table_->setColumnWidth(4, 170);
    table_->setColumnWidth(5, 170);
    table_->setColumnWidth(6, 60);
    table_->setColumnWidth(7, 60);
    table_->setColumnWidth(8, 90);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &MainWindow::showSelectedComponent);
    connect(filterEdit_, &QLineEdit::textChanged, this, &MainWindow::applyFilter);

    layout->addWidget(table_, 1);
    setCentralWidget(central);
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

    logDock_ = new QDockWidget(tr("Parser log"), this);
    logDock_->setObjectName("logDock");
    logDock_->setWidget(logView_);
    addDockWidget(Qt::BottomDockWidgetArea, logDock_);
    resizeDocks({logDock_}, {150}, Qt::Vertical);
}

// ---------------------------------------------------------------------------
// Document loading
// ---------------------------------------------------------------------------

void MainWindow::openFileDialog() {
    const QString startDir =
        hasDocument_ ? QString::fromStdString(document_.path).section('/', 0, -2) : QString();
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
        QMessageBox::critical(this, tr("Cannot read document"),
                              tr("%1\n\n%2").arg(info.fileName(), qs(outcome.error)));
        log(tr("ERROR  %1 — %2").arg(info.fileName(), qs(outcome.error)));
        return;
    }
    setDocument(outcome.document);
}

void MainWindow::setDocument(const pcbv::Document& document) {
    document_ = document;
    hasDocument_ = true;

    setWindowTitle(QString("%1 — PCB Viewer").arg(qs(document_.fileName)));
    populateComponents();
    populateStructure();
    showDocumentSummary();

    log(tr("Opened %1  (%2)").arg(qs(document_.fileName), qs(pcbv::docTypeName(document_.type))));
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

    statusBar()->showMessage(tr("%1 · %2 components · %3")
                                 .arg(qs(document_.fileName))
                                 .arg(document_.components.size())
                                 .arg(qs(document_.formatVersion)));
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

        auto* indexItem = new NumericItem(QString::number(row + 1), row + 1);
        indexItem->setData(Qt::UserRole + 1, row); // component index
        table_->setItem(row, 0, indexItem);
        table_->setItem(row, 1, new QTableWidgetItem(qs(c.designator)));
        table_->setItem(row, 2, new QTableWidgetItem(qs(c.comment)));
        table_->setItem(row, 3, new QTableWidgetItem(qs(c.footprint)));
        table_->setItem(row, 4, new QTableWidgetItem(qs(c.manufacturer)));
        table_->setItem(row, 5, new QTableWidgetItem(qs(c.partNumber)));

        const int pins = c.pinCount > 0 ? c.pinCount : -1;
        const int pads = c.padCount > 0 ? c.padCount : -1;
        table_->setItem(row, 6, new NumericItem(c.pinCount > 0 ? QString::number(c.pinCount) : QString(), pins));
        table_->setItem(row, 7, new NumericItem(c.padCount > 0 ? QString::number(c.padCount) : QString(), pads));
        table_->setItem(row, 8, new QTableWidgetItem(qs(c.layer)));
        table_->setItem(row, 9, new QTableWidgetItem(qs(c.description)));

        QString tooltip = tr("<b>%1</b><br>%2").arg(qs(c.designator), qs(c.description));
        if (!c.supplierPartNumber.empty())
            tooltip += tr("<br>Supplier P/N: %1").arg(qs(c.supplierPartNumber));
        if (!c.datasheet.empty()) tooltip += tr("<br>%1").arg(qs(c.datasheet));
        for (int col = 0; col < 10; ++col) {
            if (QTableWidgetItem* item = table_->item(row, col)) item->setToolTip(tooltip);
        }
    }

    table_->setSortingEnabled(true);

    tableInfoLabel_->setText(tr("%1 components · %2")
                                 .arg(document_.components.size())
                                 .arg(pcb ? tr("PCB") : tr("schematic")));
    applyFilter(filterEdit_ ? filterEdit_->text() : QString());
}

void MainWindow::populateStructure() {
    structureTree_->clear();

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
    structureTree_->resizeColumnToContents(1);
}

// ---------------------------------------------------------------------------
// Selection / details
// ---------------------------------------------------------------------------

void MainWindow::applyFilter(const QString& text) {
    if (table_ == nullptr) return;
    const QString needle = text.trimmed();
    int visible = 0;
    for (int row = 0; row < table_->rowCount(); ++row) {
        bool match = needle.isEmpty();
        if (!match) {
            for (int col = 1; col < table_->columnCount(); ++col) {
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
    const QTableWidgetItem* first = table_->item(row, 0);
    if (first == nullptr) return;
    const int index = first->data(Qt::UserRole + 1).toInt();
    if (index < 0 || index >= static_cast<int>(document_.components.size())) return;
    detailsView_->setHtml(
        componentDetailsHtml(document_.components[static_cast<std::size_t>(index)]));
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
    row(tr("Format"), qs(d.formatVersion).toHtmlEscaped());
    if (!d.boardVersion.empty()) row(tr("Board format"), qs(d.boardVersion));
    if (!d.lastSaved.empty()) row(tr("Last saved"), qs(d.lastSaved));
    row(tr("File size"), formatSize(d.fileSize));
    row(tr("Root entries"), QString::number(d.structure.size()));
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
    QString text;
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (table_->isRowHidden(row)) continue;
        QStringList fields;
        for (int col = 1; col < table_->columnCount(); ++col) {
            const QTableWidgetItem* item = table_->item(row, col);
            fields << (item != nullptr ? item->text() : QString());
        }
        text += fields.join('\t') + '\n';
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
