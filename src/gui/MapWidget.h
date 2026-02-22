#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QElapsedTimer>
#include "MapBackend.h"

namespace QMapLibre { class GLWidget; class Map; }

namespace vtol {

class MapLibreAdapter;
class MapLegend;

class MapWidget : public QWidget {
    Q_OBJECT
public:
    explicit MapWidget(QWidget* parent = nullptr);

    MapBackend* backend() { return &m_backend; }

    /// Set tile server URL. Must be called BEFORE loadMap().
    void setTileServerUrl(const QString& url);

    /// Initialize MapLibre GLWidget. Call after setTileServerUrl().
    void loadMap();

    /// Start drag-to-move mode for the given waypoint index (0-based).
    /// Called from MainWindow after context menu "Переместить".
    void startWaypointDrag(int wpIndex);

    /// Activate waypoint placement mode: next left-click on the map places a new waypoint.
    /// Emits waypointPlacementRequested(lat, lon) when user clicks.
    /// Pressing Escape cancels.
    void startWaypointPlacement();

    /// Cancel placement mode without placing.
    void cancelWaypointPlacement();

signals:
    void waypointMoved(int wpIndex, double lat, double lon);
    void waypointPlacementRequested(double lat, double lon);
    void waypointPlacementCancelled();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void onMapChanged(int change);
    void updateBounds();
    void onFollowTick();

    // Click vs drag detection
    void onMousePress(double lat, double lon, const QPointF& screenPos, Qt::MouseButton button);
    void onMouseRelease(double lat, double lon, const QPointF& screenPos, Qt::MouseButton button);
    void onMouseDoubleClick(double lat, double lon);
    void onMouseMove(double lat, double lon);
    void setupMap();  // called once Map* is ready (after initializeGL)

    MapBackend m_backend{this};
    QMapLibre::GLWidget* m_glWidget = nullptr;
    QMapLibre::Map* m_map = nullptr;
    MapLibreAdapter* m_adapter = nullptr;
    MapLegend* m_legend = nullptr;
    QLabel* m_fpsLabel = nullptr;
    QString m_tileUrl;
    bool m_mapSetupDone = false;

    // Map FPS measurement
    QElapsedTimer m_fpsTimer;
    int m_fpsFrameCount = 0;
    double m_fpsValue = 0.0;

    // Click detection
    QPointF m_pressScreenPos;
    bool m_dragging = false;
    static constexpr double CLICK_THRESHOLD = 5.0;  // pixels

    // Vertex drag (zone editing)
    int m_draggingVertexIdx = -1;
    static constexpr double VERTEX_HIT_THRESHOLD = 15.0;  // pixels
    int hitTestEditVertex(const QPointF& screenPos);
    int hitTestEditMidpoint(const QPointF& screenPos);

    // Waypoint hit-test (pixel-based, uses pixelForCoordinate)
    int hitTestWaypoint(const QPointF& screenPos, double thresholdPx = 18.0) const;

    // Hover tooltip state
    QTimer* m_hoverTimer = nullptr;
    int m_hoveredWpIndex = -1;
    QPointF m_lastMouseScreenPos;

    // Waypoint drag-to-move state
    int m_draggingWpIndex = -1;
    QGeoCoordinate m_dragOriginalPos;  // for Escape cancellation

    // Waypoint placement mode state
    bool m_placementMode = false;
};

} // namespace vtol
