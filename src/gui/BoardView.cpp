// BoardView.cpp — paints the framework independent display list produced by
// pcbv::buildBoardRender(). Zoom with the wheel, pan by dragging, double click to fit.
#include "BoardView.h"

#include <QFont>
#include <QImage>
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
    threeDDirty_ = true;
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
    threeDDirty_ = true;
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
        threeDDirty_ = true;
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
        threeDQuick_ = true;   // fast frames while the camera moves
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
            // Below the horizon the board turns over, so the bottom copper is reachable.
            elevationDeg_ = std::clamp(elevationDeg_ - delta.y() * 0.35, -78.0, 88.0);
            threeDDirty_ = true;
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
    if (threeD_) {
        rotating_ = false;
        threeDQuick_ = false;
        threeDDirty_ = true;   // one crisp frame once the drag is over
        unsetCursor();
        setCursor(Qt::OpenHandCursor);
        update();
        return;
    }

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

void BoardView::setGridVisible(bool on) {
    if (showGrid_ == on) return;
    showGrid_ = on;
    update();
}

void BoardView::setTheme(pcbv::BoardTheme theme) {
    if (pcbv::currentTheme() == theme) return;
    pcbv::setBoardTheme(theme);
    dirty_ = true;        // the display list stores colours, so it has to be rebuilt
    threeDDirty_ = true;  // and so does the software rendered preview
    update();
}

/// Dot grid in board space every 100 mil (2.54 mm), the pitch every CAD tool starts with.
void BoardView::paintGrid(QPainter& painter, double scale) const {
    if (!showGrid_ || list_.empty()) return;
    const double pitch = 100.0 * list_.unitsPerMil;      // 2.54 mm in device units
    if (pitch <= 0.0) return;
    if (pitch * scale < 9.0) return;                     // too dense to be anything but noise
    const double radius = std::max(0.75 / scale, pitch * 0.010);
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(toColor(pcbv::themeColours().grid, 0.55));
    for (double y = 0.0; y <= list_.height + 0.5; y += pitch) {
        for (double x = 0.0; x <= list_.width + 0.5; x += pitch) {
            painter.drawEllipse(QPointF(x, y), radius, radius);
        }
    }

    // The origin of the board: a small cross with a ring, the way every editor marks (0, 0).
    const QPointF origin(0.0, list_.height);
    QPen originPen(toColor(pcbv::themeColours().copperMid, 0.9));
    originPen.setWidthF(1.4 / scale);
    painter.setPen(originPen);
    painter.setBrush(Qt::NoBrush);
    const double arm = pitch * 0.5;
    painter.drawLine(QPointF(origin.x() - arm, origin.y()), QPointF(origin.x() + arm, origin.y()));
    painter.drawLine(QPointF(origin.x(), origin.y() - arm), QPointF(origin.x(), origin.y() + arm));
    painter.drawEllipse(origin, arm * 0.34, arm * 0.34);
    painter.restore();
}

/// A full-window crosshair under the pointer: two hairlines with a gap around the cursor,
/// the way a measurement cursor looks in a CAD tool.
void BoardView::paintCrosshair(QPainter& painter) const {
    if (!cursorInside_ || list_.empty() || threeD_) return;
    painter.save();
    QPen pen(toColor(pcbv::themeColours().grid));
    pen.setWidthF(1.0);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    const double gap = 14.0;
    const QPointF point = cursor_;
    painter.drawLine(QPointF(0.0, point.y()), QPointF(point.x() - gap, point.y()));
    painter.drawLine(QPointF(point.x() + gap, point.y()), QPointF(width(), point.y()));
    painter.drawLine(QPointF(point.x(), 0.0), QPointF(point.x(), point.y() - gap));
    painter.drawLine(QPointF(point.x(), point.y() + gap), QPointF(point.x(), height()));
    painter.restore();
}

void BoardView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    rebuild();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(rect(), toColor(pcbv::themeColours().canvas));

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
    paintGrid(painter, scale);
    painter.restore();
    paintCrosshair(painter);

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
                if (primitive.strokeWidth > 0.0) {
                    QPainterPath path;
                    QFontMetricsF metrics(font);
                    const double textWidth = metrics.horizontalAdvance(QString::fromStdString(primitive.text));
                    const double offset = primitive.textAnchor == 1 ? 0.0
                                          : primitive.textAnchor == 2 ? -textWidth
                                                                      : -textWidth / 2.0;
                    path.addText(offset, 0.0, font, QString::fromStdString(primitive.text));
                    painter.setPen(QPen(paint(0x10141a), primitive.strokeWidth));
                    painter.setBrush(paint(primitive.color));
                    painter.drawPath(path);
                } else {
                    painter.setPen(paint(primitive.color));
                    const int alignment = primitive.textAnchor == 1
                                              ? (Qt::AlignLeft | Qt::AlignVCenter)
                                              : primitive.textAnchor == 2
                                                    ? (Qt::AlignRight | Qt::AlignVCenter)
                                                    : (Qt::AlignHCenter | Qt::AlignVCenter);
                    const QRectF box(primitive.textAnchor == 1 ? 0.0
                                                                 : primitive.textAnchor == 2
                                                                       ? -4000.0
                                                                       : -2000.0,
                                     -primitive.fontSize, 4000.0, primitive.fontSize * 2.0);
                    painter.drawText(box, alignment, QString::fromStdString(primitive.text));
                }
                painter.restore();
                break;
            }
        }
    }
    painter.setOpacity(1.0);
}

// ---------------------------------------------------------------------------
// 3D preview (rendered by pcbv::render3D, see core/ThreeD.h)
// ---------------------------------------------------------------------------

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
    yawDeg_ = 35.0;
    elevationDeg_ = 52.0;
    threeDZoom_ = 1.0;
    threeDDirty_ = true;
    update();
}

bool BoardView::saveThreeD(const QString& path, int width, int height) {
    if (list_.empty()) return false;
    pcbv::ThreeDView view;
    view.yawDeg = yawDeg_;
    view.elevationDeg = elevationDeg_;
    view.zoom = threeDZoom_;
    view.width = std::max(320, width);
    view.height = std::max(240, height);
    view.supersample = 2;
    view.labels = false;
    const pcbv::ThreeDImage rendered = pcbv::render3D(document_, options_, view);
    if (!rendered.valid()) return false;

    QImage frame(rendered.width, rendered.height, QImage::Format_RGB32);
    for (int y = 0; y < rendered.height; ++y) {
        auto* scanLine = reinterpret_cast<QRgb*>(frame.scanLine(y));
        const std::uint32_t* source =
            rendered.pixels.data() + static_cast<std::size_t>(y) * rendered.width;
        for (int x = 0; x < rendered.width; ++x) scanLine[x] = 0xFF000000u | source[x];
    }
    {
        QPainter painter(&frame);
        QFont font(QStringLiteral("Arial"));
        font.setPixelSize(std::max(10, rendered.height / 55));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(QColor(0xf2, 0xf5, 0xf8));
        for (const pcbv::ThreeDLabel& label : rendered.labels) {
            painter.drawText(QPointF(label.x + 4.0, label.y - 4.0),
                             QString::fromStdString(label.text));
        }
    }
    return frame.save(path);
}

void BoardView::paintThreeD(QPainter& painter) {
    if (list_.empty() || !document_.bounds.valid) return;

    // The frame is rendered once and reused: only a camera change (or a resize) makes it
    // stale. While the user drags the board the renderer runs at a lower quality so the
    // rotation stays smooth.
    const QSize target = size();
    if (threeDDirty_ || threeDSize_ != target) {
        pcbv::ThreeDView view;
        view.yawDeg = yawDeg_;
        view.elevationDeg = elevationDeg_;
        view.zoom = threeDZoom_;
        const double ratio = devicePixelRatioF();
        view.width = std::max(1, static_cast<int>(std::lround(width() * ratio)));
        view.height = std::max(1, static_cast<int>(std::lround(height() * ratio)));
        // Supersampling costs four times the pixels; skip it for big windows and while
        // the camera is moving.
        const long long area = static_cast<long long>(view.width) * view.height;
        view.supersample = (threeDQuick_ || area > 2200000) ? 1 : 2;
        // The renderer reports where the designators land; the widget draws them itself so
        // the text stays crisp at any zoom and follows the system font.
        view.labels = document_.components.size() <= 200;
        threeDImage_ = pcbv::render3D(document_, options_, view);
        threeDSize_ = target;
        threeDDirty_ = false;
    }

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (threeDImage_.valid()) {
        QImage frame(threeDImage_.width, threeDImage_.height, QImage::Format_RGB32);
        for (int y = 0; y < threeDImage_.height; ++y) {
            auto* scanLine = reinterpret_cast<QRgb*>(frame.scanLine(y));
            const std::uint32_t* source =
                threeDImage_.pixels.data() + static_cast<std::size_t>(y) * threeDImage_.width;
            for (int x = 0; x < threeDImage_.width; ++x) scanLine[x] = 0xFF000000u | source[x];
        }
        painter.drawImage(rect(), frame);

        // Designators sit on top of the image, drawn with the widget's own font.
        if (document_.components.size() <= 200) {
            QFont labelFont = painter.font();
            labelFont.setPixelSize(std::max(8, static_cast<int>(std::lround(11.0 * devicePixelRatioF()))));
            labelFont.setBold(true);
            painter.setFont(labelFont);
            painter.setPen(QColor(0xf5, 0xd7, 0x6e));
            const double sx = static_cast<double>(width()) / std::max(1, threeDImage_.width);
            const double sy = static_cast<double>(height()) / std::max(1, threeDImage_.height);
            for (const pcbv::ThreeDLabel& label : threeDImage_.labels) {
                painter.drawText(QPointF(label.x * sx, label.y * sy),
                                 QString::fromStdString(label.text));
            }
        }
    }

    // Hint line + a legend for what the colours mean.
    QFont font = painter.font();
    font.setPixelSize(12);
    font.setBold(false);
    painter.setFont(font);
    painter.setPen(QColor(0x9a, 0xa0, 0xa6));
    painter.drawText(rect().adjusted(12, 8, -12, -10), Qt::AlignLeft | Qt::AlignBottom,
                     tr("3D · drag to rotate · wheel to zoom · double click or Esc for the flat view"));

    struct Swatch {
        QColor color;
        QString text;
    };
    const Swatch swatches[] = {
        {QColor(0x16, 0x68, 0x4a), tr("solder mask")},
        {QColor(0x2f, 0x8f, 0x63), tr("top copper")},
        {QColor(0xd8, 0xb2, 0x6a), tr("pads / holes")},
        {QColor(0x24, 0x27, 0x2c), tr("parts")},
        {QColor(0xe8, 0xed, 0xf2), tr("silkscreen")},
    };
    int legendX = 12;
    const int legendY = height() - 34;
    for (const Swatch& swatch : swatches) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(swatch.color);
        painter.drawRoundedRect(QRectF(legendX, legendY, 10, 10), 2, 2);
        painter.setPen(QColor(0x8f, 0x97, 0xa3));
        const int textWidth = painter.fontMetrics().horizontalAdvance(swatch.text);
        painter.drawText(QPointF(legendX + 15, legendY + 9), swatch.text);
        legendX += 15 + textWidth + 16;
    }
    painter.restore();
}
