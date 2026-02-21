#pragma once

#include <QQuickPaintedItem>
#include <QTimer>
#include <QImage>
#include <QHash>
#include <QNetworkAccessManager>
#include <QSet>
#include <QPointF>
#include <cmath>

namespace vtol {

class MapBackend;

struct TileKey {
    int z, x, y;
    bool operator==(const TileKey& o) const { return z == o.z && x == o.x && y == o.y; }
};

inline size_t qHash(const TileKey& k, size_t seed = 0) {
    return ::qHash(k.z, seed) ^ ::qHash(k.x * 131071 + k.y, seed);
}

class FlightMapCanvas : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QObject* backend READ backendObj WRITE setBackendObj NOTIFY backendChanged)

public:
    explicit FlightMapCanvas(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    QObject* backendObj() const;
    void setBackendObj(QObject* obj);

signals:
    void backendChanged();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;

private:
    // ── Mercator projection ──
    static constexpr int TILE_SIZE = 256;
    static constexpr int MIN_ZOOM = 2;
    static constexpr int MAX_ZOOM = 20;

    // Precomputed per-frame paint constants (set once in paint())
    double m_pScale = 1.0;  // 2^zoom (cached)
    double m_pTotalSize = 256.0; // TILE_SIZE * 2^zoom
    double m_pCwx = 0;     // center world X
    double m_pCwy = 0;     // center world Y
    double m_pHalfW = 0;   // width() / 2
    double m_pHalfH = 0;   // height() / 2

    void updatePaintConstants();

    // Fast inline projection using precomputed constants
    inline double geoToWorldX(double lon) const {
        return (lon + 180.0) / 360.0 * m_pTotalSize;
    }
    inline double geoToWorldY(double lat) const {
        double latRad = lat * (M_PI / 180.0);
        return (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) * (1.0 / M_PI)) * 0.5 * m_pTotalSize;
    }
    inline QPointF geoToScreen(double lat, double lon) const {
        return {geoToWorldX(lon) - m_pCwx + m_pHalfW,
                geoToWorldY(lat) - m_pCwy + m_pHalfH};
    }
    void screenToGeo(double sx, double sy, double& lat, double& lon) const;

    // ── Map state ──
    double m_centerLat = 55.7558;
    double m_centerLon = 37.6173;
    int m_zoom = 12;

    // ── Tiles ──
    QHash<TileKey, QImage> m_tileCache;
    QSet<TileKey> m_pendingTiles;
    QNetworkAccessManager m_nam;
    int m_activeFetches = 0;

    void updateVisibleTiles();
    void requestTile(const TileKey& key);
    void evictTiles();

    static constexpr int MAX_CACHED_TILES = 512;
    static constexpr int MAX_CONCURRENT_FETCHES = 12;

    // ── Tile layer cache ──
    QImage m_tileLayer;             // pre-composited tile layer
    double m_tlCenterLat = 0;       // viewport state when composed
    double m_tlCenterLon = 0;
    int m_tlZoom = -1;
    int m_tlWidth = 0;
    int m_tlHeight = 0;
    int m_tlTileCount = 0;          // tile count at composition time
    bool m_tileLayerDirty = true;

    void compositeTileLayer();
    void paintTilesFromCache(QPainter* p);

    // ── Backend ──
    MapBackend* m_backend = nullptr;
    void connectBackendSignals();

    // ── Mouse ──
    bool m_dragging = false;
    QPointF m_dragStart;
    double m_dragStartLat = 0;
    double m_dragStartLon = 0;
    bool m_clickPending = false;
    QPointF m_clickPos;
    QTimer m_clickTimer;

    int m_editDragVertex = -1;

    // Obstacle warning hover
    int m_hoveredWarning = -1;
    QPointF m_mouseScreenPos;

    // ── Timers ──
    QTimer m_repaintTimer;  // 16ms throttle (~60 FPS)
    QTimer m_followTimer;   // 200ms follow mode
    QTimer m_boundsTimer;   // 300ms bounds debounce

    bool m_repaintScheduled = false;

    void scheduleRepaint();
    void notifyBounds();

    // ── Paint layers ──
    void paintSettlementPolygons(QPainter* p);
    void paintSettlementCircles(QPainter* p);
    void paintSettlementBorders(QPainter* p);
    void paintZoneFills(QPainter* p);
    void paintZoneBorders(QPainter* p);
    void paintTrack(QPainter* p);
    void paintRouteSegments(QPainter* p);
    void paintActiveWpLine(QPainter* p);
    void paintConflictSegments(QPainter* p);
    void paintPlannedDirectPath(QPainter* p);
    void paintAvoidancePath(QPainter* p);
    void paintObstacleWarnings(QPainter* p);
    void paintConflictPoints(QPainter* p);
    void paintWaypoints(QPainter* p);
    void paintDrawingOverlay(QPainter* p);
    void paintEditingVertices(QPainter* p);
    void paintHome(QPainter* p);
    void paintAircraft(QPainter* p);

    // ── Helpers ──
    double metersPerPixel() const;
    double zoomAlpha() const;
    int hitTestEditingVertex(double sx, double sy, double radius = 12.0) const;
};

} // namespace vtol
