// BoardView.cpp — paints the framework independent display list produced by
// pcbv::buildBoardRender(). Zoom with the wheel, pan by dragging, double click to fit.
#include "BoardView.h"

#include <QFont>
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
    dirty_ = true;
    rebuild();
    resetView();
    update();
}

void BoardView::clear() {
    document_ = pcbv::Document();
    hasDocument_ = false;
    selected_ = -1;
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
    if (panning_) {
        pan_ = panOrigin_ + (event->pos() - panStart_);
        clampPan();
    }
    update();
}

void BoardView::mouseReleaseEvent(QMouseEvent* event) {
    if (panning_) {
        panning_ = false;
        unsetCursor();
        event->accept();
    }
}

void BoardView::mouseDoubleClickEvent(QMouseEvent* event) {
    fitToWindow();
    event->accept();
}

void BoardView::leaveEvent(QEvent* event) {
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

    const double scale = fitScale() * zoom_;
    painter.save();
    painter.translate(width() / 2.0 + pan_.x(), height() / 2.0 + pan_.y());
    painter.scale(scale, scale);
    painter.translate(-list_.width / 2.0, -list_.height / 2.0);

    for (const auto& primitive : list_.primitives) {
        switch (primitive.kind) {
            case pcbv::RenderPrimitive::Kind::Polygon: {
                if (primitive.points.size() < 3) break;
                QPolygonF polygon;
                for (const auto& point : primitive.points) polygon << QPointF(point.x, point.y);
                if (primitive.filled) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(toColor(primitive.color, primitive.opacity));
                    painter.drawPolygon(polygon);
                }
                if (primitive.strokeWidth > 0.0) {
                    QPen pen(toColor(primitive.color));
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
                QPen pen(toColor(primitive.color));
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
                    painter.setBrush(toColor(primitive.color, primitive.opacity));
                    painter.drawEllipse(box);
                }
                if (primitive.strokeWidth > 0.0) {
                    QPen pen(toColor(primitive.color));
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
                    painter.setPen(QPen(toColor(0x10141a), primitive.strokeWidth));
                    painter.setBrush(toColor(primitive.color));
                    painter.drawPath(path);
                } else {
                    painter.setPen(toColor(primitive.color));
                    painter.drawText(box, Qt::AlignHCenter | Qt::AlignVCenter,
                                     QString::fromStdString(primitive.text));
                }
                painter.restore();
                break;
            }
        }
    }
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

    // --- status overlay ------------------------------------------------------
    if (cursorInside_ && hasDocument_ && document_.bounds.valid) {
        const QPointF mil = widgetToMil(cursor_);
        const QString info = QStringLiteral("X %1 mm   Y %2 mm   ·   zoom %3%")
                                 .arg(mil.x() * 0.0254, 0, 'f', 2)
                                 .arg(mil.y() * 0.0254, 0, 'f', 2)
                                 .arg(zoom_ * 100.0, 0, 'f', 0);
        painter.setPen(QColor(0x9a, 0xa0, 0xa6));
        painter.drawText(rect().adjusted(10, 0, -10, -6), Qt::AlignLeft | Qt::AlignBottom, info);
    }
}
