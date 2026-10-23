// BoardView.cpp — paints the framework independent display list produced by
// pcbv::buildBoardRender(). Zoom with the wheel, pan by dragging, double click to fit.
#include "BoardView.h"

#include <QFont>
#include <QKeyEvent>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPolygonF>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

QColor toColor(std::uint32_t rgb, double opacity = 1.0) {
    QColor color(static_cast<int>((rgb >> 16) & 0xFF), static_cast<int>((rgb >> 8) & 0xFF),
                 static_cast<int>(rgb & 0xFF));
    color.setAlphaF(std::clamp(opacity, 0.0, 1.0));
    return color;
}

/// Paper colours: the dark theme does not survive a white page, so the board body turns
/// light, drill holes stay light grey and near-white silkscreen prints dark.
QColor toPrintColor(std::uint32_t rgb) {
    if (rgb == 0x0b0f14) return QColor(0xf5, 0xf5, 0xf1);  // board body
    if (rgb == 0x10141a) return QColor(0xc9, 0xcd, 0xd2);  // holes, drills
    const int r = static_cast<int>((rgb >> 16) & 0xFF);
    const int g = static_cast<int>((rgb >> 8) & 0xFF);
    const int b = static_cast<int>(rgb & 0xFF);
    const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
    if (luma > 190.0) return QColor(0x33, 0x37, 0x3d);     // silkscreen on paper
    return QColor(r, g, b);
}

const char* kHint = "No PCB geometry in this document";

} // namespace

BoardView::BoardView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
}

void BoardView::setDocument(const pcbv::Document& document) {
    document_ = document;
    hasDocument_ = true;
    selected_ = -1;
    highlightNet_.clear();
    hoverNet_.clear();
    dirty_ = true;
    rebuild();
    resetView();
    update();
}

void BoardView::clear() {
    document_ = pcbv::Document();
    hasDocument_ = false;
    selected_ = -1;
    highlightNet_.clear();
    hoverNet_.clear();
    list_ = pcbv::BoardRenderList();
    zoom_ = 1.0;
    pan_ = QPointF();
    update();
}

void BoardView::setRenderOptions(const pcbv::BoardRenderOptions& options) {
    options_ = options;
    dirty_ = true;
    rebuild();
    update();
}

void BoardView::setSelectedComponent(int index) {
    if (index == selected_) return;
    selected_ = index;
    update();
}

void BoardView::setHighlightNet(const QString& net) {
    if (net == highlightNet_) return;
    highlightNet_ = net;
    update();
}

void BoardView::rebuild() {
    if (!dirty_) return;
    list_ = hasDocument_ ? pcbv::buildBoardRender(document_, options_) : pcbv::BoardRenderList();
    dirty_ = false;
}

double BoardView::fitScale() const {
    if (list_.width <= 0.0 || list_.height <= 0.0) return 1.0;
    const double margin = 12.0;
    const double sx = (width() - margin * 2.0) / list_.width;
    const double sy = (height() - margin * 2.0) / list_.height;
    return std::max(std::min(sx, sy), 0.01);
}

QPointF BoardView::boardToWidget(double x, double y) const {
    const double scale = fitScale() * zoom_;
    const double cx = width() / 2.0 + pan_.x();
    const double cy = height() / 2.0 + pan_.y();
    return QPointF(cx + (x - list_.width / 2.0) * scale,
                   cy + (y - list_.height / 2.0) * scale);
}

QPointF BoardView::widgetToBoard(const QPointF& point) const {
    const double scale = fitScale() * zoom_;
    if (scale <= 0.0) return QPointF();
    const double cx = width() / 2.0 + pan_.x();
    const double cy = height() / 2.0 + pan_.y();
    return QPointF((point.x() - cx) / scale + list_.width / 2.0,
                   (point.y() - cy) / scale + list_.height / 2.0);
}

void BoardView::setView(double zoom, const QPointF& pan) {
    zoom_ = std::clamp(zoom, 0.2, 80.0);
    pan_ = pan;
    clampPan();
    update();
}

void BoardView::clampPan() {
    const double scale = fitScale() * zoom_;
    const double halfW = list_.width * scale / 2.0;
    const double halfH = list_.height * scale / 2.0;
    const double limitX = std::max(halfW - width() / 2.0 + 40.0, 0.0);
    const double limitY = std::max(halfH - height() / 2.0 + 40.0, 0.0);
    pan_.setX(std::clamp(pan_.x(), -limitX, limitX));
    pan_.setY(std::clamp(pan_.y(), -limitY, limitY));
}

void BoardView::resetView() {
    zoom_ = 1.0;
    pan_ = QPointF();
}

void BoardView::fitToWindow() {
    resetView();
    update();
}

void BoardView::zoomIn() {
    zoom_ = std::min(zoom_ * 1.25, 60.0);
    clampPan();
    update();
}

void BoardView::zoomOut() {
    zoom_ = std::max(zoom_ / 1.25, 0.05);
    clampPan();
    update();
}

void BoardView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    clampPan();
}

void BoardView::wheelEvent(QWheelEvent* event) {
    if (threeD_) {
        const int steps = event->angleDelta().y();
        if (steps == 0) return;
        threeDZoom_ = std::clamp(threeDZoom_ * (steps > 0 ? 1.12 : 1.0 / 1.12), 0.3, 8.0);
        update();
        event->accept();
        return;
    }
    if (list_.empty()) return;
    const QPointF anchor = event->position();
    const QPointF before = widgetToBoard(anchor);
    const double factor = event->angleDelta().y() > 0 ? 1.18 : 1.0 / 1.18;
    zoom_ = std::clamp(zoom_ * factor, 0.05, 60.0);
    const QPointF after = widgetToBoard(anchor);
    // Keep the point under the cursor fixed while zooming.
    const double scale = fitScale() * zoom_;
    pan_ += QPointF((after.x() - before.x()) * scale, (after.y() - before.y()) * scale);
    clampPan();
    update();
    event->accept();
}

void BoardView::mousePressEvent(QMouseEvent* event) {
    if (threeD_ && event->button() == Qt::LeftButton) {
        rotating_ = true;
        rotateStart_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (measureMode_ && !document_.bounds.valid) {
        // Without a board outline there is nothing to measure against.
    } else if (measureMode_) {
        if (event->button() == Qt::LeftButton) {
            QPointF snapped;
            QString label;
            const QPointF mil = widgetToMil(event->pos());
            const QPointF point = snapToFeature(mil, snapped, label) ? snapped : mil;
            if (!hasMeasureStart_) {
                hasMeasureStart_ = true;
                hasMeasureEnd_ = false;
                measureStartMil_ = point;
                measureCursorMil_ = point;
            } else {
                measureEndMil_ = point;
                hasMeasureEnd_ = true;
                hasMeasureStart_ = false;
            }
            applyMeasurement();
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton) {
            clearMeasurement();
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ShiftModifier))) {
        panning_ = true;
        panStart_ = event->pos();
        panOrigin_ = pan_;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        // A click picks the net under the cursor (what a PCB engineer expects) and,
        // when the click lands on a pad, the component that owns it.
        const QString net = netAt(event->pos());
        if (!net.isEmpty()) {
            setHighlightNet(net);
            emit netClicked(net);
        } else {
            setHighlightNet(QString());
            emit netClicked(QString());
        }
        const int index = componentAt(event->pos());
        if (index >= 0) {
            setSelectedComponent(index);
            emit componentClicked(index);
        }
        panning_ = true;
        panStart_ = event->pos();
        panOrigin_ = pan_;
        event->accept();
    }
}

void BoardView::mouseMoveEvent(QMouseEvent* event) {
    cursor_ = event->pos();
    cursorInside_ = true;
    if (threeD_) {
        if (rotating_) {
            const QPointF delta = event->pos() - rotateStart_;
            rotateStart_ = event->pos();
            yawDeg_ += delta.x() * 0.45;
            elevationDeg_ = std::clamp(elevationDeg_ - delta.y() * 0.35, 12.0, 88.0);
            update();
        }
        return;
    }
    if (measureMode_) {
        QPointF snapped;
        QString label;
        const QPointF mil = widgetToMil(cursor_);
        measureCursorMil_ = snapToFeature(mil, snapped, label) ? snapped : mil;
        measureSnapLabel_ = label;
        if (hasMeasureStart_) applyMeasurement();
        update();
        return;
    }
    if (panning_) {
        pan_ = panOrigin_ + (event->pos() - panStart_);
        clampPan();
    } else {
        const QString net = netAt(cursor_);
        if (net != hoverNet_) {
            hoverNet_ = net;
            setCursor(net.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
        }
    }
    update();
}

void BoardView::mouseReleaseEvent(QMouseEvent* event) {
    if (rotating_) {
        rotating_ = false;
        unsetCursor();
        event->accept();
        return;
    }
    if (panning_) {
        panning_ = false;
        unsetCursor();
        event->accept();
    }
}

void BoardView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (threeD_) {
        resetThreeDCamera();
        event->accept();
        return;
    }
    fitToWindow();
    event->accept();
}

void BoardView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && threeD_) {
        // Esc returns to the flat view; the toolbar button follows through the signal.
        setThreeD(false);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && measureMode_ &&
        (hasMeasureStart_ || hasMeasureEnd_)) {
        clearMeasurement();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (!highlightNet_.isEmpty()) {
            setHighlightNet(QString());
            emit netClicked(QString());
            event->accept();
            return;
        }
        setSelectedComponent(-1);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void BoardView::leaveEvent(QEvent* event) {
    if (!hoverNet_.isEmpty()) {
        hoverNet_.clear();
        update();
    }
    cursorInside_ = false;
    update();
    QWidget::leaveEvent(event);
}

double BoardView::viewMargin() const {
    const double longest = std::max(document_.bounds.width(), document_.bounds.height());
    return longest * 0.03;
}

QPointF BoardView::milToWidget(double milX, double milY) const {
    if (!document_.bounds.valid) return QPointF();
    const double unitsPerMil = list_.unitsPerMil > 0.0 ? list_.unitsPerMil : 1.0;
    const double margin = viewMargin();
    return boardToWidget((milX - document_.bounds.minX + margin) * unitsPerMil,
                         (document_.bounds.maxY - milY + margin) * unitsPerMil);
}

QPointF BoardView::widgetToMil(const QPointF& point) const {
    if (!document_.bounds.valid) return QPointF();
    const double unitsPerMil = list_.unitsPerMil > 0.0 ? list_.unitsPerMil : 1.0;
    const double margin = viewMargin();
    const QPointF device = widgetToBoard(point);
    return QPointF(device.x() / unitsPerMil + document_.bounds.minX - margin,
                   document_.bounds.maxY - (device.y() / unitsPerMil - margin));
}

int BoardView::componentAt(const QPointF& widgetPos) const {
    if (!hasDocument_ || !document_.bounds.valid) return -1;
    int best = -1;
    double bestDistance = 1e18;
    for (std::size_t i = 0; i < document_.components.size(); ++i) {
        const pcbv::Component& component = document_.components[i];
        // A component is "hit" when the click lands inside its pad outline (or a
        // small circle around its origin when it has no pads, e.g. an ASCII export).
        double halfWidth = 22.0;
        double halfHeight = 22.0;
        if (!component.pads.empty()) {
            double minX = component.pads[0].x, maxX = minX;
            double minY = component.pads[0].y, maxY = minY;
            for (const auto& pad : component.pads) {
                minX = std::min(minX, pad.x - pad.width / 2.0);
                maxX = std::max(maxX, pad.x + pad.width / 2.0);
                minY = std::min(minY, pad.y - pad.height / 2.0);
                maxY = std::max(maxY, pad.y + pad.height / 2.0);
            }
            halfWidth = std::max((maxX - minX) / 2.0, 12.0);
            halfHeight = std::max((maxY - minY) / 2.0, 12.0);
        }
        const QPointF centre = milToWidget(component.x, component.y);
        const double tolerance = 6.0;  // device pixels of slack
        const double distance =
            std::max(std::fabs(widgetPos.x() - centre.x()) - halfWidth,
                     std::fabs(widgetPos.y() - centre.y()) - halfHeight);
        if (distance <= tolerance && distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

namespace {

/// Distance from a point to a segment, in the same units as the inputs.
double distanceToSegment(const QPointF& point, const QPointF& a, const QPointF& b) {
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 1e-12) return std::hypot(point.x() - a.x(), point.y() - a.y());
    double t = ((point.x() - a.x()) * dx + (point.y() - a.y()) * dy) / lengthSquared;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(point.x() - (a.x() + t * dx), point.y() - (a.y() + t * dy));
}

bool pointInPolygon(const QPointF& point, const std::vector<pcbv::Point>& polygon) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const double xi = polygon[i].x, yi = polygon[i].y;
        const double xj = polygon[j].x, yj = polygon[j].y;
        const bool crosses = ((yi > point.y()) != (yj > point.y())) &&
                             (point.x() < (xj - xi) * (point.y() - yi) / (yj - yi + 1e-12) + xi);
        if (crosses) inside = !inside;
    }
    return inside;
}

} // namespace

int BoardView::primitiveAt(const QPointF& widgetPos) const {
    if (list_.empty()) return -1;
    const double scale = fitScale() * zoom_;
    const QPointF device = widgetToBoard(widgetPos);
    const double tolerance = 6.0 / std::max(scale, 1e-6);  // ~6 screen pixels

    int best = -1;
    double bestScore = 1e18;
    for (std::size_t i = 0; i < list_.primitives.size(); ++i) {
        const pcbv::RenderPrimitive& primitive = list_.primitives[i];
        if (!primitive.selectable) continue;
        double distance = 1e18;
        switch (primitive.kind) {
            case pcbv::RenderPrimitive::Kind::Line:
                if (primitive.points.size() >= 2) {
                    distance = distanceToSegment(device,
                                                 QPointF(primitive.points[0].x, primitive.points[0].y),
                                                 QPointF(primitive.points[1].x, primitive.points[1].y));
                    distance -= primitive.strokeWidth / 2.0;
                }
                break;
            case pcbv::RenderPrimitive::Kind::Circle: {
                const double centreDistance =
                    std::hypot(device.x() - primitive.center.x, device.y() - primitive.center.y);
                if (primitive.filled) {
                    distance = centreDistance - primitive.radius;
                } else {
                    distance = std::fabs(centreDistance - primitive.radius) - primitive.strokeWidth / 2.0;
                }
                break;
            }
            case pcbv::RenderPrimitive::Kind::Polygon:
                if (primitive.points.size() >= 3 && pointInPolygon(device, primitive.points)) {
                    distance = 0.0;  // a pad is hit as soon as the cursor is inside it
                }
                break;
            case pcbv::RenderPrimitive::Kind::Text:
                break;
        }
        if (distance <= tolerance && distance < bestScore) {
            bestScore = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

QString BoardView::netAt(const QPointF& widgetPos) const {
    const int index = primitiveAt(widgetPos);
    if (index < 0) return QString();
    return QString::fromStdString(list_.primitives[static_cast<std::size_t>(index)].net);
}

void BoardView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    rebuild();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(rect(), QColor(0x10, 0x14, 0x1a));

    if (list_.empty()) {
        painter.setPen(QColor(0x8a, 0x92, 0x9e));
        QFont font = painter.font();
        font.setPointSizeF(11.0);
        painter.setFont(font);
        const QString message = hasDocument_
                                    ? tr("%1\n\nThis document has no PCB geometry "
                                         "(schematic sheets are listed in the Components tab).")
                                          .arg(kHint)
                                    : tr("Open an Altium .PcbDoc to see the board here.");
        painter.drawText(rect().adjusted(24, 24, -24, -24),
                         Qt::AlignCenter | Qt::TextWordWrap, message);
        return;
    }

    if (threeD_) {
        paintThreeD(painter);
        return;
    }

    const double scale = fitScale() * zoom_;
    painter.save();
    painter.translate(width() / 2.0 + pan_.x(), height() / 2.0 + pan_.y());
    painter.scale(scale, scale);
    painter.translate(-list_.width / 2.0, -list_.height / 2.0);

    drawPrimitives(painter, scale, false);
    painter.restore();

    // --- selection highlight -------------------------------------------------
    if (selected_ >= 0 && selected_ < static_cast<int>(document_.components.size())) {
        const pcbv::Component& component = document_.components[static_cast<std::size_t>(selected_)];
        double minX = component.x - 30.0, maxX = component.x + 30.0;
        double minY = component.y - 30.0, maxY = component.y + 30.0;
        if (!component.pads.empty()) {
            minX = maxX = component.pads[0].x;
            minY = maxY = component.pads[0].y;
            for (const auto& pad : component.pads) {
                minX = std::min(minX, pad.x - pad.width / 2.0);
                maxX = std::max(maxX, pad.x + pad.width / 2.0);
                minY = std::min(minY, pad.y - pad.height / 2.0);
                maxY = std::max(maxY, pad.y + pad.height / 2.0);
            }
        }
        const QPointF topLeft = milToWidget(minX, maxY);
        const QPointF bottomRight = milToWidget(maxX, minY);
        painter.save();
        QPen pen(QColor(0xf5, 0xd7, 0x6e));
        pen.setWidthF(1.6);
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const QRectF box = QRectF(topLeft, bottomRight).normalized().adjusted(-6, -6, 6, 6);
        painter.drawRoundedRect(box, 3, 3);

        QFont font = painter.font();
        font.setPixelSize(12);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(QColor(0x10, 0x14, 0x1a));
        const QString label = QString::fromStdString(component.designator);
        const QRectF textBox(box.left(), box.top() - 20.0, box.width(), 18.0);
        painter.fillRect(textBox, QColor(0xf5, 0xd7, 0x6e));
        painter.drawText(textBox, Qt::AlignCenter, label);
        painter.restore();
    }

    paintMeasure(painter);

    // --- board size badge ----------------------------------------------------
    if (document_.bounds.valid) {
        const QString size = QStringLiteral("Board %1 × %2 mm")
                                 .arg(document_.bounds.width() * 0.0254, 0, 'f', 2)
                                 .arg(document_.bounds.height() * 0.0254, 0, 'f', 2);
        QFont font = painter.font();
        font.setPixelSize(12);
        painter.setFont(font);
        painter.setPen(QColor(0x9a, 0xa0, 0xa6));
        painter.drawText(rect().adjusted(10, 8, -10, -24), Qt::AlignRight | Qt::AlignTop, size);
    }

    // --- status overlay ------------------------------------------------------
    if (cursorInside_ && hasDocument_ && document_.bounds.valid) {
        const QPointF mil = widgetToMil(cursor_);
        QString info = QStringLiteral("X %1 mm   Y %2 mm   ·   zoom %3%")
                           .arg(mil.x() * 0.0254, 0, 'f', 2)
                           .arg(mil.y() * 0.0254, 0, 'f', 2)
                           .arg(zoom_ * 100.0, 0, 'f', 0);
        if (!hoverNet_.isEmpty()) {
            info += QStringLiteral("   ·   net %1").arg(hoverNet_);
        } else if (!highlightNet_.isEmpty()) {
            info += QStringLiteral("   ·   highlighting %1 (click empty space to clear)")
                        .arg(highlightNet_);
        }
        painter.setPen(QColor(0x9a, 0xa0, 0xa6));
        painter.drawText(rect().adjusted(10, 0, -10, -6), Qt::AlignLeft | Qt::AlignBottom, info);
    }
}

// ---------------------------------------------------------------------------
// Measure tool
// ---------------------------------------------------------------------------

void BoardView::setMeasureMode(bool on) {
    if (measureMode_ == on) return;
    measureMode_ = on;
    if (!on) {
        hasMeasureStart_ = false;
        hasMeasureEnd_ = false;
        measureSnapLabel_.clear();
        unsetCursor();
        emit measureChanged(QString());
    } else {
        setCursor(Qt::CrossCursor);
    }
    update();
}

void BoardView::clearMeasurement() {
    hasMeasureStart_ = false;
    hasMeasureEnd_ = false;
    measureSnapLabel_.clear();
    emit measureChanged(QString());
    update();
}

double BoardView::snapToleranceMil() const {
    const double scale = fitScale() * zoom_;
    if (scale <= 0.0 || list_.unitsPerMil <= 0.0) return 0.0;
    // 11 screen pixels, expressed in mil.
    return 11.0 / (scale * list_.unitsPerMil);
}

bool BoardView::snapToFeature(const QPointF& milPoint, QPointF& snapped, QString& label) const {
    const double tolerance = snapToleranceMil();
    if (tolerance <= 0.0) return false;

    double best = tolerance;
    auto consider = [&](double x, double y, const std::string& text) {
        const double distance = std::hypot(x - milPoint.x(), y - milPoint.y());
        if (distance > best) return;
        best = distance;
        snapped = QPointF(x, y);
        label = QString::fromStdString(text);
    };

    // Pads first: they are the points a designer actually wants to measure between.
    for (const auto& pad : document_.geometry.pads) {
        consider(pad.x, pad.y, pad.designator.empty() ? pad.net : pad.net + " " + pad.designator);
    }
    for (const auto& via : document_.geometry.vias) consider(via.x, via.y, via.net);
    for (const auto& track : document_.geometry.tracks) {
        consider(track.x1, track.y1, track.net);
        consider(track.x2, track.y2, track.net);
    }
    return best < tolerance;
}

QString BoardView::measureText() const {
    if (!hasMeasureEnd_ && !hasMeasureStart_) return QString();
    const QPointF start = measureStartMil_;
    const QPointF end = hasMeasureEnd_ ? measureEndMil_ : measureCursorMil_;
    const double dx = end.x() - start.x();
    const double dy = end.y() - start.y();
    const double mm = std::hypot(dx, dy) * 0.0254;
    QString text = QStringLiteral("%1 mm   (ΔX %2 · ΔY %3)")
                       .arg(mm, 0, 'f', 3)
                       .arg(dx * 0.0254, 0, 'f', 2)
                       .arg(dy * 0.0254, 0, 'f', 2);
    if (!measureSnapLabel_.isEmpty()) text += QStringLiteral("   ·   snap %1").arg(measureSnapLabel_);
    return text;
}

void BoardView::applyMeasurement() {
    emit measureChanged(measureText());
}

void BoardView::paintMeasure(QPainter& painter) const {
    if (!measureMode_ || (!hasMeasureStart_ && !hasMeasureEnd_)) return;

    const QPointF start = milToWidget(measureStartMil_.x(), measureStartMil_.y());
    const QPointF endMil = hasMeasureEnd_ ? measureEndMil_ : measureCursorMil_;
    const QPointF end = milToWidget(endMil.x(), endMil.y());

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    // The line: bright, dashed, with a dot at both ends.
    QPen line(QColor(0xf5, 0xd7, 0x6e));
    line.setWidthF(1.5);
    line.setStyle(Qt::DashLine);
    painter.setPen(line);
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(start, end);

    painter.setPen(QPen(QColor(0x10, 0x14, 0x1a), 1.0));
    painter.setBrush(QColor(0xf5, 0xd7, 0x6e));
    painter.drawEllipse(start, 3.4, 3.4);
    painter.drawEllipse(end, 3.4, 3.4);

    // The label sits in the middle of the line, on a dark plate so it reads over copper.
    const QString text = measureText();
    if (!text.isEmpty()) {
        QFont font = painter.font();
        font.setPixelSize(12);
        font.setBold(true);
        painter.setFont(font);
        const QFontMetricsF metrics(font);
        const QPointF middle = (start + end) / 2.0;
        const double textWidth = metrics.horizontalAdvance(text);
        QRectF plate(middle.x() - textWidth / 2.0 - 7.0, middle.y() - 21.0,
                     textWidth + 14.0, 20.0);
        const QRectF bounds = rect().adjusted(2, 2, -2, -2);
        if (plate.left() < bounds.left()) plate.moveLeft(bounds.left());
        if (plate.right() > bounds.right()) plate.moveRight(bounds.right());
        if (plate.top() < bounds.top()) plate.moveTop(bounds.top());
        if (plate.bottom() > bounds.bottom()) plate.moveBottom(bounds.bottom());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x10, 0x14, 0x1a, 225));
        painter.drawRoundedRect(plate, 4, 4);
        painter.setPen(QColor(0xf5, 0xd7, 0x6e));
        painter.drawText(plate, Qt::AlignCenter, text);
    }

    // A small hint while the first click is still missing.
    if (!hasMeasureStart_ && !hasMeasureEnd_) {
        QFont font = painter.font();
        font.setPixelSize(11);
        font.setBold(false);
        painter.setFont(font);
        painter.setPen(QColor(0x9a, 0xa0, 0xa6));
        painter.drawText(rect().adjusted(12, 10, -12, -10), Qt::AlignLeft | Qt::AlignTop,
                         tr("Measure: click two points   ·   Esc to finish"));
    }
    painter.restore();
}

void BoardView::zoomToComponent(int index) {
    if (!hasDocument_ || index < 0 || index >= static_cast<int>(document_.components.size())) return;
    const pcbv::Component& component = document_.components[static_cast<std::size_t>(index)];

    double minX = component.x - 25.0;
    double maxX = component.x + 25.0;
    double minY = component.y - 25.0;
    double maxY = component.y + 25.0;
    if (!component.pads.empty()) {
        minX = maxX = component.pads.front().x;
        minY = maxY = component.pads.front().y;
        for (const auto& pad : component.pads) {
            minX = std::min(minX, pad.x - pad.width / 2.0);
            maxX = std::max(maxX, pad.x + pad.width / 2.0);
            minY = std::min(minY, pad.y - pad.height / 2.0);
            maxY = std::max(maxY, pad.y + pad.height / 2.0);
        }
    }

    const QPointF topLeft = milToBoardPoint(minX, maxY);
    const QPointF bottomRight = milToBoardPoint(maxX, minY);
    const QRectF box = QRectF(topLeft, bottomRight).normalized();
    const double fit = fitScale();
    if (fit <= 0.0) return;

    const double targetWidth = std::max(box.width(), 1.0);
    const double targetHeight = std::max(box.height(), 1.0);
    const double zoomX = (width() * 0.62) / (targetWidth * fit);
    const double zoomY = (height() * 0.62) / (targetHeight * fit);
    zoom_ = std::clamp(std::min(zoomX, zoomY), 1.0, 60.0);

    const double scale = fitScale() * zoom_;
    const QPointF centre = box.center();
    pan_ = QPointF(-(centre.x() - list_.width / 2.0) * scale,
                   -(centre.y() - list_.height / 2.0) * scale);
    clampPan();
    update();
}

QPointF BoardView::milToBoardPoint(double milX, double milY) const {
    const double unitsPerMil = list_.unitsPerMil > 0.0 ? list_.unitsPerMil : 1.0;
    const double margin = viewMargin();
    const double minX = document_.bounds.valid ? document_.bounds.minX : 0.0;
    const double maxY = document_.bounds.valid ? document_.bounds.maxY : 1.0;
    return QPointF((milX - minX + margin) * unitsPerMil, (maxY - milY + margin) * unitsPerMil);
}

QSizeF BoardView::renderForPrint(QPainter& painter, const QPointF& origin,
                                 double devicePerMm) const {
    if (list_.empty() || list_.unitsPerMil <= 0.0) return QSizeF();
    const double unitsPerMm = list_.unitsPerMil / 0.0254;  // list units per millimetre
    if (unitsPerMm <= 0.0) return QSizeF();
    const double scale = devicePerMm / unitsPerMm;        // device units per list unit

    painter.save();
    painter.translate(origin);
    painter.scale(scale, scale);
    drawPrimitives(painter, scale, true);
    painter.restore();
    return QSizeF(list_.width * scale, list_.height * scale);
}

void BoardView::drawPrimitives(QPainter& painter, double scale, bool forPrint) const {
    // On paper the dark theme is inverted: the board becomes a light plate, holes a light
    // grey and everything that is nearly white (silkscreen) turns dark so it stays visible.
    auto paint = [forPrint](std::uint32_t rgb, double opacity = 1.0) {
        if (!forPrint) return toColor(rgb, opacity);
        return toPrintColor(rgb);
    };

    const std::string highlighted = forPrint ? std::string() : highlightNet_.toStdString();
    for (const auto& primitive : list_.primitives) {
        if (forPrint && primitive.background) continue;
        // With a net highlighted everything else fades, but the board outline and
        // silkscreen stay readable as context.
        if (!highlighted.empty()) {
            if (primitive.net == highlighted) {
                painter.setOpacity(1.0);
            } else if (primitive.selectable) {
                painter.setOpacity(0.16);
            } else {
                painter.setOpacity(0.55);
            }
        } else {
            painter.setOpacity(1.0);
        }
        switch (primitive.kind) {
            case pcbv::RenderPrimitive::Kind::Polygon: {
                if (primitive.points.size() < 3) break;
                QPolygonF polygon;
                for (const auto& point : primitive.points) polygon << QPointF(point.x, point.y);
                if (primitive.filled) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(paint(primitive.color, primitive.opacity));
                    painter.drawPolygon(polygon);
                }
                if (primitive.strokeWidth > 0.0) {
                    QPen pen(paint(primitive.color));
                    pen.setWidthF(std::max(primitive.strokeWidth, 0.4 / scale));
                    pen.setJoinStyle(Qt::RoundJoin);
                    pen.setCapStyle(Qt::RoundCap);
                    if (primitive.dashed) pen.setStyle(Qt::DashLine);
                    painter.setPen(pen);
                    painter.setBrush(Qt::NoBrush);
                    // Arcs arrive as open polylines: drawPolygon would close them.
                    if (primitive.closed) {
                        painter.drawPolygon(polygon);
                    } else {
                        painter.drawPolyline(polygon);
                    }
                }
                break;
            }
            case pcbv::RenderPrimitive::Kind::Line: {
                if (primitive.points.size() < 2) break;
                QPen pen(paint(primitive.color));
                pen.setWidthF(std::max(primitive.strokeWidth, 0.5 / scale));
                pen.setCapStyle(Qt::RoundCap);
                if (primitive.dashed) pen.setStyle(Qt::DashLine);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                if (primitive.points.size() == 2) {
                    painter.drawLine(QPointF(primitive.points[0].x, primitive.points[0].y),
                                     QPointF(primitive.points[1].x, primitive.points[1].y));
                } else {
                    QPolygonF polyline;
                    for (const auto& point : primitive.points) polyline << QPointF(point.x, point.y);
                    painter.drawPolyline(polyline);
                }
                break;
            }
            case pcbv::RenderPrimitive::Kind::Circle: {
                if (primitive.radius <= 0.0) break;
                const QRectF box(primitive.center.x - primitive.radius,
                                 primitive.center.y - primitive.radius, primitive.radius * 2.0,
                                 primitive.radius * 2.0);
                if (primitive.filled) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(paint(primitive.color, primitive.opacity));
                    painter.drawEllipse(box);
                }
                if (primitive.strokeWidth > 0.0) {
                    QPen pen(paint(primitive.color));
                    pen.setWidthF(std::max(primitive.strokeWidth, 0.4 / scale));
                    painter.setPen(pen);
                    painter.setBrush(Qt::NoBrush);
                    painter.drawEllipse(box);
                }
                break;
            }
            case pcbv::RenderPrimitive::Kind::Text: {
                if (primitive.text.empty() || primitive.fontSize <= 0.0) break;
                painter.save();
                painter.translate(primitive.x, primitive.y);
                if (primitive.rotation != 0.0) painter.rotate(primitive.rotation);
                QFont font(QStringLiteral("Arial"));
                font.setPixelSize(std::max(static_cast<int>(std::lround(primitive.fontSize)), 1));
                painter.setFont(font);
                const QRectF box(-2000.0, -primitive.fontSize, 4000.0, primitive.fontSize * 2.0);
                if (primitive.strokeWidth > 0.0) {
                    QPainterPath path;
                    QFontMetricsF metrics(font);
                    const double textWidth = metrics.horizontalAdvance(QString::fromStdString(primitive.text));
                    path.addText(-textWidth / 2.0, 0.0, font, QString::fromStdString(primitive.text));
                    painter.setPen(QPen(paint(0x10141a), primitive.strokeWidth));
                    painter.setBrush(paint(primitive.color));
                    painter.drawPath(path);
                } else {
                    painter.setPen(paint(primitive.color));
                    painter.drawText(box, Qt::AlignHCenter | Qt::AlignVCenter,
                                     QString::fromStdString(primitive.text));
                }
                painter.restore();
                break;
            }
        }
    }
    painter.setOpacity(1.0);
}

// ---------------------------------------------------------------------------
// Simple 3D preview
// ---------------------------------------------------------------------------

namespace {

constexpr double kPi3D = 3.14159265358979323846;

/// Layers that live on the underside of the board.
bool isBottomLayer(int layer) {
    if (layer >= 17 && layer <= 32) return true;             // bottom half + bottom copper
    return layer == 34 || layer == 36 || layer == 38;        // bottom overlay / paste / solder
}

} // namespace

void BoardView::setThreeD(bool on) {
    if (threeD_ == on) return;
    threeD_ = on;
    if (on) {
        setMeasureMode(false);
        resetThreeDCamera();
        setCursor(Qt::OpenHandCursor);
        emit threeDChanged(true);
    } else {
        unsetCursor();
        emit threeDChanged(false);
    }
    update();
}

void BoardView::resetThreeDCamera() {
    yawDeg_ = -35.0;
    elevationDeg_ = 52.0;
    threeDZoom_ = 1.0;
    update();
}

void BoardView::paintThreeD(QPainter& painter) {
    if (list_.empty() || !document_.bounds.valid) return;

    struct Item {
        QPolygonF polygon;
        QColor color;
        bool filled = true;
        bool line = false;
        double width = 1.0;
        double depth = 0.0;
        QString text;
        QPointF textPos;
        double textSize = 0.0;
    };

    constexpr double kThicknessMm = 1.6;
    const double unitsPerMm = list_.unitsPerMil > 0.0 ? list_.unitsPerMil / 0.0254 : 1.0;

    // --- board outline in mm, centred on the origin -------------------------
    std::vector<QPointF> outline;
    if (document_.geometry.outline.size() >= 3) {
        for (const auto& point : document_.geometry.outline) {
            outline.push_back(QPointF(point.x * 0.0254, point.y * 0.0254));
        }
    } else {
        const pcbv::Bounds& bounds = document_.bounds;
        outline = {QPointF(bounds.minX * 0.0254, bounds.minY * 0.0254),
                   QPointF(bounds.maxX * 0.0254, bounds.minY * 0.0254),
                   QPointF(bounds.maxX * 0.0254, bounds.maxY * 0.0254),
                   QPointF(bounds.minX * 0.0254, bounds.maxY * 0.0254)};
    }
    QPointF centre(0.0, 0.0);
    for (const auto& point : outline) centre += point;
    centre /= static_cast<double>(outline.size());
    for (auto& point : outline) point -= centre;

    // --- camera --------------------------------------------------------------
    const double yaw = yawDeg_ * kPi3D / 180.0;
    const double elev = elevationDeg_ * kPi3D / 180.0;
    const double cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
    const double cosElev = std::cos(elev), sinElev = std::sin(elev);

    auto project = [&](const QPointF& mm, double z, double& depth) {
        const double x1 = mm.x() * cosYaw - mm.y() * sinYaw;
        const double y1 = mm.x() * sinYaw + mm.y() * cosYaw;
        depth = y1 * cosElev - z * sinElev;
        return QPointF(x1, -(y1 * sinElev + z * cosElev));
    };

    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (const auto& point : outline) {
        for (const double z : {0.0, kThicknessMm}) {
            double depth = 0.0;
            const QPointF projected = project(point, z, depth);
            minX = std::min(minX, projected.x());
            maxX = std::max(maxX, projected.x());
            minY = std::min(minY, projected.y());
            maxY = std::max(maxY, projected.y());
        }
    }
    const double spanX = std::max(maxX - minX, 1e-3);
    const double spanY = std::max(maxY - minY, 1e-3);
    const double scale =
        std::min(width() * 0.76 / spanX, height() * 0.76 / spanY) * threeDZoom_;
    const double originX = width() / 2.0 - (minX + maxX) / 2.0 * scale;
    const double originY = height() / 2.0 - (minY + maxY) / 2.0 * scale;
    auto toScreen = [&](const QPointF& point) {
        return QPointF(originX + point.x() * scale, originY + point.y() * scale);
    };

    // --- gathering -----------------------------------------------------------
    std::vector<Item> faceBottom, bottomItems, wallItems, faceTop, topItems;

    auto addFace = [&](std::vector<Item>& bucket, const std::vector<QPointF>& points, double z,
                       const QColor& color, bool filled, double width, double depthBias) {
        Item item;
        item.filled = filled;
        item.color = color;
        item.width = width;
        double depthSum = 0.0;
        for (const auto& point : points) {
            double depth = 0.0;
            item.polygon << toScreen(project(point, z, depth));
            depthSum += depth;
        }
        item.depth = depthSum / std::max<std::size_t>(1, points.size()) + depthBias;
        bucket.push_back(item);
    };

    addFace(faceBottom, outline, 0.0, QColor(0x08, 0x0c, 0x11), true, 0.0, 8.0);
    addFace(faceTop, outline, kThicknessMm, QColor(0x16, 0x20, 0x2b), true, 0.0, -8.0);

    // Side walls, shaded by how much they face the viewer.
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const QPointF& a = outline[i];
        const QPointF& b = outline[(i + 1) % outline.size()];
        const double edgeAngle = std::atan2(b.y() - a.y(), b.x() - a.x()) + kPi3D / 2.0;
        const double facing = std::fabs(std::cos(edgeAngle - yaw));
        const int shade = static_cast<int>(30.0 + 26.0 * facing);   // 30..56
        double depthA = 0.0;
        double depthB = 0.0;
        const QPointF aLow = toScreen(project(a, 0.0, depthA));
        const QPointF aHigh = toScreen(project(a, kThicknessMm, depthA));
        const QPointF bLow = toScreen(project(b, 0.0, depthB));
        const QPointF bHigh = toScreen(project(b, kThicknessMm, depthB));
        Item quad;
        quad.polygon << aLow << bLow << bHigh << aHigh;
        quad.color = QColor(shade, shade + 6, shade + 14);
        quad.depth = (depthA + depthB) / 2.0;
        wallItems.push_back(quad);
    }

    // Primitives: copper, silk, pads and designators, each on its own face.
    for (const auto& primitive : list_.primitives) {
        if (primitive.background) continue;
        const bool bottom = isBottomLayer(primitive.layer);
        const double z = bottom ? -0.02 : kThicknessMm + 0.02;
        std::vector<Item>& bucket = bottom ? bottomItems : topItems;
        const QColor color(static_cast<int>((primitive.color >> 16) & 0xFF),
                           static_cast<int>((primitive.color >> 8) & 0xFF),
                           static_cast<int>(primitive.color & 0xFF));

        switch (primitive.kind) {
            case pcbv::RenderPrimitive::Kind::Polygon: {
                if (primitive.points.size() < 2) break;
                std::vector<QPointF> mm;
                mm.reserve(primitive.points.size());
                for (const auto& point : primitive.points) {
                    mm.push_back(QPointF(point.x / unitsPerMm, point.y / unitsPerMm));
                }
                addFace(bucket, mm, z, color, primitive.filled,
                        std::max(primitive.strokeWidth * scale, 0.6), 0.0);
                break;
            }
            case pcbv::RenderPrimitive::Kind::Line: {
                if (primitive.points.size() < 2) break;
                double depthA = 0.0, depthB = 0.0;
                Item item;
                item.filled = false;
                item.line = true;
                item.color = color;
                item.width = std::max(primitive.strokeWidth * scale, 0.7);
                item.polygon << toScreen(project(QPointF(primitive.points[0].x / unitsPerMm,
                                                         primitive.points[0].y / unitsPerMm),
                                                 z, depthA));
                item.polygon << toScreen(project(QPointF(primitive.points[1].x / unitsPerMm,
                                                         primitive.points[1].y / unitsPerMm),
                                                 z, depthB));
                item.depth = (depthA + depthB) / 2.0;
                bucket.push_back(item);
                break;
            }
            case pcbv::RenderPrimitive::Kind::Circle: {
                if (primitive.radius <= 0.0) break;
                std::vector<QPointF> mm;
                const double radiusMm = primitive.radius / unitsPerMm;
                const QPointF centreMm(primitive.center.x / unitsPerMm,
                                       primitive.center.y / unitsPerMm);
                const int steps = 20;
                for (int i = 0; i < steps; ++i) {
                    const double angle = 2.0 * kPi3D * i / steps;
                    mm.push_back(centreMm + QPointF(std::cos(angle), std::sin(angle)) * radiusMm);
                }
                addFace(bucket, mm, z, color, primitive.filled, 0.0, 0.0);
                break;
            }
            case pcbv::RenderPrimitive::Kind::Text: {
                if (primitive.text.empty()) break;
                double depth = 0.0;
                Item item;
                item.filled = false;
                item.color = color;
                const QPointF anchor = toScreen(project(QPointF(primitive.x / unitsPerMm,
                                                                primitive.y / unitsPerMm),
                                                        z, depth));
                item.depth = depth;
                item.textPos = anchor;
                item.text = QString::fromStdString(primitive.text);
                item.textSize = std::clamp(primitive.fontSize / unitsPerMm * scale, 6.0, 16.0);
                bucket.push_back(item);
                break;
            }
        }
    }

    auto drawBucket = [&painter](std::vector<Item>& bucket) {
        std::sort(bucket.begin(), bucket.end(), [](const Item& a, const Item& b) {
            return a.depth > b.depth;  // far to near
        });
        for (const auto& item : bucket) {
            if (!item.text.isEmpty()) {
                QFont font = painter.font();
                font.setPixelSize(std::max(static_cast<int>(item.textSize), 6));
                font.setBold(true);
                painter.setFont(font);
                painter.setPen(item.color);
                painter.drawText(item.textPos + QPointF(0.0, -item.textSize * 0.4), item.text);
                continue;
            }
            if (item.line) {
                QPen pen(item.color);
                pen.setWidthF(item.width);
                pen.setCapStyle(Qt::RoundCap);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawPolyline(item.polygon);
                continue;
            }
            if (item.filled) {
                painter.setPen(item.width > 0.0 ? QPen(item.color, item.width) : QPen(Qt::NoPen));
                painter.setBrush(item.color);
                painter.drawPolygon(item.polygon);
            } else {
                QPen pen(item.color);
                pen.setWidthF(std::max(item.width, 0.6));
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawPolygon(item.polygon);
            }
        }
    };

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawBucket(faceBottom);
    drawBucket(bottomItems);
    drawBucket(wallItems);
    drawBucket(faceTop);
    drawBucket(topItems);

    // Hint line.
    QFont font = painter.font();
    font.setPixelSize(12);
    font.setBold(false);
    painter.setFont(font);
    painter.setPen(QColor(0x9a, 0xa0, 0xa6));
    painter.drawText(rect().adjusted(12, 8, -12, -10), Qt::AlignLeft | Qt::AlignBottom,
                     tr("3D · drag to rotate · wheel to zoom · double click or Esc for the flat view"));
    painter.restore();
}
