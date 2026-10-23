// BoardView.h — 2D PCB viewer widget (paints pcbv::BoardRenderList).
#pragma once

#include <QPointF>
#include <QString>
#include <QWidget>

#include "core/BoardRender.h"
#include "core/Export.h"
#include "core/Model.h"

class QKeyEvent;

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

    bool hasGeometry() const { return !list_.empty(); }
    double zoom() const { return zoom_; }

public slots:
    void fitToWindow();
    void zoomIn();
    void zoomOut();

signals:
    /// Emitted when the user clicks a component on the board.
    void componentClicked(int index);
    /// Emitted when the user clicks a track, pad or via of a net.
    void netClicked(const QString& net);

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
};
