// BoardView.h — 2D PCB viewer widget (paints pcbv::BoardRenderList).
#pragma once

#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QWidget>

#include "core/BoardRender.h"
#include "core/Export.h"
#include "core/Model.h"

class QKeyEvent;
class QPainter;

class BoardView : public QWidget {
    Q_OBJECT

public:
    explicit BoardView(QWidget* parent = nullptr);

    void setDocument(const pcbv::Document& document);
    void clear();
    void setRenderOptions(const pcbv::BoardRenderOptions& options);
    pcbv::BoardRenderOptions renderOptions() const { return options_; }

    /// Highlights one component (index into document.components), -1 = none.
    void setSelectedComponent(int index);

    /// Highlights every primitive of one net ("" clears the highlight).
    void setHighlightNet(const QString& net);
    QString highlightNet() const { return highlightNet_; }

    /// Measure tool: click two points (snapping to pads, vias and track ends); the
    /// distance in mm is drawn on the board and reported through measureChanged().
    void setMeasureMode(bool on);
    bool measureMode() const { return measureMode_; }
    void clearMeasurement();

    /// Frames one component (index into document.components).
    void zoomToComponent(int index);

    /// Simple 3D preview: the board extruded to 1.6 mm and tilted. Drag rotates,
    /// the wheel zooms, a double click resets the camera.
    void setThreeD(bool on);
    bool threeD() const { return threeD_; }

    /// Draws the board for the printer: `devicePerMm` device units per millimetre,
    /// top-left corner at `origin`. Returns the size of the drawing in device units.
    QSizeF renderForPrint(QPainter& painter, const QPointF& origin, double devicePerMm) const;

    bool hasGeometry() const { return !list_.empty(); }
    double zoom() const { return zoom_; }
    QPointF pan() const { return pan_; }
    int selectedComponent() const { return selected_; }

    /// Restores a saved camera (used when switching between open documents).
    void setView(double zoom, const QPointF& pan);

public slots:
    void fitToWindow();
    void zoomIn();
    void zoomOut();

signals:
    /// Emitted when the user clicks a component on the board.
    void componentClicked(int index);
    /// Emitted when the user clicks a track, pad or via of a net.
    void netClicked(const QString& net);
    /// Emitted whenever the measure tool has something to say ("" clears the status).
    void measureChanged(const QString& text);
    /// Emitted when the view itself changes mode (Esc leaves the 3D view).
    void threeDChanged(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuild();
    void resetView();
    double fitScale() const;
    QPointF boardToWidget(double x, double y) const;
    QPointF widgetToBoard(const QPointF& point) const;
    QPointF milToWidget(double milX, double milY) const;
    QPointF widgetToMil(const QPointF& point) const;
    double viewMargin() const;
    int componentAt(const QPointF& widgetPos) const;
    int primitiveAt(const QPointF& widgetPos) const;
    QString netAt(const QPointF& widgetPos) const;
    void clampPan();
    void paintMeasure(QPainter& painter) const;
    void paintThreeD(QPainter& painter);
    void resetThreeDCamera();
    void drawPrimitives(QPainter& painter, double scale, bool forPrint) const;
    bool snapToFeature(const QPointF& milPoint, QPointF& snapped, QString& label) const;
    double snapToleranceMil() const;
    QString measureText() const;
    void applyMeasurement();
    QPointF milToBoardPoint(double milX, double milY) const;

    pcbv::Document document_;
    pcbv::BoardRenderOptions options_;
    pcbv::BoardRenderList list_;

    bool hasDocument_ = false;
    bool dirty_ = true;
    int selected_ = -1;
    QString highlightNet_;
    QString hoverNet_;

    double zoom_ = 1.0;      // 1.0 = fit
    QPointF pan_;            // device pixels
    bool panning_ = false;
    QPointF panStart_;
    QPointF panOrigin_;
    QPointF cursor_;
    bool cursorInside_ = false;

    bool measureMode_ = false;
    bool hasMeasureStart_ = false;   ///< first click placed, waiting for the second
    QPointF measureStartMil_;
    bool hasMeasureEnd_ = false;     ///< a finished measurement is on screen
    QPointF measureEndMil_;
    QPointF measureCursorMil_;       ///< snapped cursor, for the rubber band
    QString measureSnapLabel_;

    bool threeD_ = false;
    double yawDeg_ = -35.0;        ///< rotation around the board normal
    double elevationDeg_ = 52.0;   ///< 90 = straight down on the board
    double threeDZoom_ = 1.0;
    QPointF rotateStart_;
    bool rotating_ = false;
};
