#pragma once

#include <QObject>
#include <QGeoCoordinate>
#include <QVariantList>
#include <QString>
#include <QVector>
#include <QPointF>
#include <QTimer>

#include "models/MapModels.h"

namespace vtol {

/// Pre-computed settlement polygon for unlimited cache (render from render area)
struct SettlementCacheItem {
    QVariantList coords;   // [[lat,lon], ...]
    double minLat, minLon, maxLat, maxLon;  // pre-computed AABB
};

class MapBackend : public QObject {
    Q_OBJECT

    // ── Aircraft ──
    Q_PROPERTY(QGeoCoordinate aircraftPosition READ aircraftPosition NOTIFY aircraftPositionChanged)
    Q_PROPERTY(double aircraftHeading READ aircraftHeading NOTIFY aircraftHeadingChanged)
    Q_PROPERTY(bool aircraftVisible READ aircraftVisible NOTIFY aircraftVisibleChanged)

    // ── Home ──
    Q_PROPERTY(QGeoCoordinate homePosition READ homePosition NOTIFY homePositionChanged)
    Q_PROPERTY(bool homeVisible READ homeVisible NOTIFY homeVisibleChanged)
    Q_PROPERTY(bool returningHome READ returningHome NOTIFY returningHomeChanged)

    // ── Track ──
    Q_PROPERTY(QVariantList trackPath READ trackPath NOTIFY trackPathChanged)

    // ── Active waypoint ──
    Q_PROPERTY(QGeoCoordinate activeWpPosition READ activeWpPosition NOTIFY activeWpPositionChanged)

    // ── Settings ──
    Q_PROPERTY(bool followAircraft READ followAircraft WRITE setFollowAircraft NOTIFY followAircraftChanged)
    Q_PROPERTY(QString tileServerUrl READ tileServerUrl NOTIFY tileServerUrlChanged)

    // ── Layer visibility ──
    Q_PROPERTY(bool showTrack READ showTrack WRITE setShowTrack NOTIFY showTrackChanged)
    Q_PROPERTY(bool showWaypoints READ showWaypoints WRITE setShowWaypoints NOTIFY showWaypointsChanged)
    Q_PROPERTY(bool showZones READ showZones WRITE setShowZones NOTIFY showZonesChanged)
    Q_PROPERTY(bool showSettlements READ showSettlements WRITE setShowSettlements NOTIFY showSettlementsChanged)
    Q_PROPERTY(bool showLayerPanel READ showLayerPanel NOTIFY showLayerPanelChanged)
    Q_PROPERTY(QString fpsText READ fpsText NOTIFY fpsTextChanged)

    // ── Drawing ──
    Q_PROPERTY(bool drawingMode READ drawingMode NOTIFY drawingModeChanged)
    Q_PROPERTY(QVariantList drawingPath READ drawingPath NOTIFY drawingPathChanged)
    Q_PROPERTY(bool leftClickMode READ leftClickMode WRITE setLeftClickMode NOTIFY leftClickModeChanged)

    // ── Avoidance ──
    Q_PROPERTY(QVariantList avoidancePath READ avoidancePath NOTIFY avoidancePathChanged)
    Q_PROPERTY(QVariantList plannedDirectPath READ plannedDirectPath NOTIFY plannedDirectPathChanged)

    // ── Models (read-only constants for QML) ──
    Q_PROPERTY(QObject* waypointModel READ waypointModel CONSTANT)
    Q_PROPERTY(QObject* routeSegmentModel READ routeSegmentModel CONSTANT)
    Q_PROPERTY(QObject* zoneModel READ zoneModel CONSTANT)
    Q_PROPERTY(QObject* zoneBorderModel READ zoneBorderModel CONSTANT)
    Q_PROPERTY(QObject* settlementPolyModel READ settlementPolyModel CONSTANT)
    Q_PROPERTY(QObject* settlementCircleModel READ settlementCircleModel CONSTANT)
    Q_PROPERTY(QObject* settlementBorderModel READ settlementBorderModel CONSTANT)
    Q_PROPERTY(QObject* conflictModel READ conflictModel CONSTANT)
    Q_PROPERTY(QObject* conflictPointModel READ conflictPointModel CONSTANT)
    Q_PROPERTY(QObject* drawingVertexModel READ drawingVertexModel CONSTANT)
    Q_PROPERTY(QObject* editingVertexModel READ editingVertexModel CONSTANT)
    Q_PROPERTY(QObject* editingMidpointModel READ editingMidpointModel CONSTANT)
    Q_PROPERTY(QObject* obstacleWarningModel READ obstacleWarningModel CONSTANT)

public:
    explicit MapBackend(QObject* parent = nullptr);

    // ── Property getters ──
    QGeoCoordinate aircraftPosition() const { return m_aircraftPos; }
    double aircraftHeading() const { return m_aircraftHeading; }
    bool aircraftVisible() const { return m_aircraftVisible; }
    QGeoCoordinate homePosition() const { return m_homePos; }
    bool homeVisible() const { return m_homeVisible; }
    bool returningHome() const { return m_returningHome; }
    QVariantList trackPath() const { return m_trackPath; }
    QGeoCoordinate activeWpPosition() const { return m_activeWpPos; }
    int currentZoom() const { return m_currentZoom; }
    bool followAircraft() const { return m_followAircraft; }
    QString tileServerUrl() const { return m_tileServerUrl; }
    bool showTrack() const { return m_showTrack; }
    bool showWaypoints() const { return m_showWaypoints; }
    bool showZones() const { return m_showZones; }
    bool showSettlements() const { return m_showSettlements; }
    bool showLayerPanel() const { return m_showLayerPanel; }
    QString fpsText() const { return m_fpsText; }
    bool drawingMode() const { return m_drawingMode; }
    QVariantList drawingPath() const { return m_drawingPath; }
    bool leftClickMode() const { return m_leftClickMode; }
    QVariantList avoidancePath() const { return m_avoidancePath; }
    QVariantList plannedDirectPath() const { return m_plannedDirectPath; }

    // ── Model getters ──
    QObject* waypointModel() { return &m_waypointModel; }
    QObject* routeSegmentModel() { return &m_routeSegmentModel; }
    QObject* zoneModel() { return &m_zoneModel; }
    QObject* zoneBorderModel() { return &m_zoneBorderModel; }
    QObject* settlementPolyModel() { return &m_settlementPolyModel; }
    QObject* settlementCircleModel() { return &m_settlementCircleModel; }
    QObject* settlementBorderModel() { return &m_settlementBorderModel; }
    QObject* conflictModel() { return &m_conflictModel; }
    QObject* conflictPointModel() { return &m_conflictPointModel; }
    QObject* drawingVertexModel() { return &m_drawingVertexModel; }
    QObject* editingVertexModel() { return &m_editingVertexModel; }
    QObject* editingMidpointModel() { return &m_editingMidpointModel; }
    QObject* obstacleWarningModel() { return &m_obstacleWarningModel; }

    // ── Python API equivalents (called by MainWindow / other C++ code) ──
    void updateAircraft(double lat, double lon, double heading);
    void setAircraftPosition(double lat, double lon);
    void setTrackMaxLength(int length);
    void clearTrack();
    void setHome(double lat, double lon);
    void setReturningHome(bool v);
    void setWaypoints(const QVector<QVariantMap>& waypoints, int activeIdx = 0);
    void updateActiveWaypoint(int index);
    void setFollowMode(bool enabled);
    void setZoom(int level);
    void centerOn(double lat, double lon);
    void setTileServer(const QString& url);
    void setFpsText(const QString& text);
    void tickFps(double elapsedMs);

    // Zones
    void addZone(const QString& id, const QVariantList& points, const QString& name = "");
    void removeZone(const QString& id);
    void loadAllZones(const QVariantList& zones);

    // Settlements
    void addSettlementFeatures(const QVariantList& features);
    void clearSettlements();

    // Settlement unlimited cache (for MapLibreAdapter rendering)
    const QVector<SettlementCacheItem>& settlementPolyCache() const { return m_stlPolyCache; }

    // Render area relay (called by MapLibreAdapter after viewport culling)
    void setRenderArea(double south, double west, double north, double east);

    // Conflicts / avoidance
    void setRouteConflicts(const QVariantList& conflicts);
    void clearRouteConflicts();
    void setAvoidancePath(const QVariantList& points);
    void setPlannedDirectPath(const QVariantList& points);
    void setConflictPoints(const QVariantList& points);

    // Layer visibility
    void setLayerVisibility(const QString& layerName, bool visible);

    // Drawing mode
    void startDrawing();
    void cancelDrawing();

    // Editing mode
    void enableZoneEditing(const QString& zoneId);
    void disableZoneEditing();

    // State queries
    bool isEditing() const { return !m_editingZoneId.isEmpty(); }
    bool isPointInZone(double lat, double lon) const { return !hitTestZone(lat, lon).isEmpty(); }

    // Property setters
    void setFollowAircraft(bool v);
    void setShowTrack(bool v);
    void setShowWaypoints(bool v);
    void setShowZones(bool v);
    void setShowSettlements(bool v);
    void setLeftClickMode(bool v);

public slots:
    // ── QML slots ──
    Q_INVOKABLE void onMapClick(double lat, double lon);
    Q_INVOKABLE void onMapDoubleClick(double lat, double lon);
    Q_INVOKABLE void onContextMenu(double lat, double lon, int sx, int sy);
    Q_INVOKABLE void onBoundsChanged(double south, double west, double north, double east);
    Q_INVOKABLE void onMouseMove(double lat, double lon);
    Q_INVOKABLE void onZoomChanged(int zoom);
    Q_INVOKABLE void moveEditingVertex(int idx, double lat, double lon);
    Q_INVOKABLE void deleteEditingVertex(int idx);
    Q_INVOKABLE void insertEditingVertex(int midIdx);
    Q_INVOKABLE void cancelDrawingSlot();
    Q_INVOKABLE QVariantList settlementBorderPolygons() const;
    Q_INVOKABLE QVariantList zoneBorderPolygons() const;

signals:
    // Property change notifications
    void aircraftPositionChanged();
    void aircraftHeadingChanged();
    void aircraftVisibleChanged();
    void homePositionChanged();
    void homeVisibleChanged();
    void returningHomeChanged();
    void trackPathChanged();
    void trackCoordinateAdded(double lat, double lon);
    void activeWpPositionChanged();
    void followAircraftChanged();
    void tileServerUrlChanged();
    void showTrackChanged();
    void showWaypointsChanged();
    void showZonesChanged();
    void showSettlementsChanged();
    void showLayerPanelChanged();
    void fpsTextChanged();
    void drawingModeChanged();
    void drawingPathChanged();
    void leftClickModeChanged();
    void avoidancePathChanged();
    void plannedDirectPathChanged();
    void zonesChanged();
    void renderAreaChanged(double south, double west, double north, double east);

    // QML → C++ communication
    void mapClicked(double lat, double lon);
    void mapDoubleClicked(double lat, double lon);
    void contextMenuRequested(double lat, double lon, int sx, int sy);
    void boundsChanged(double south, double west, double north, double east);
    void mouseMoved(double lat, double lon);
    void zoomChanged(int zoom);

    // Map control (C++ → QML)
    void mapCenterRequested(double lat, double lon);
    void zoomRequested(int zoom);

    // Drawing/editing results
    void drawingFinished(const QString& pointsJson);
    void drawingCancelled();
    void zoneContextMenuRequested(const QString& zoneId, int sx, int sy);
    void zoneDoubleClicked(const QString& zoneId);
    void zoneVerticesUpdated(const QString& zoneId, const QVariantList& points);

private:
    QVariantList finishDrawing();
    void addDrawingVertex(double lat, double lon);
    QString hitTestZone(double lat, double lon) const;
    static bool pointInPolygon(double lat, double lon, const QVariantList& polygon);
    void rebuildMidpoints(const QVector<QPointF>& pts);

    // Aircraft
    QGeoCoordinate m_aircraftPos;
    double m_aircraftHeading = 0.0;
    bool m_aircraftVisible = false;

    // Home
    QGeoCoordinate m_homePos;
    bool m_homeVisible = false;
    bool m_returningHome = false;

    // Track
    QVariantList m_trackPath;
    QVector<QPointF> m_trackPoints;  // raw lat/lon for decimation
    double m_lastTrackLat = 0.0;
    double m_lastTrackLon = 0.0;
    int m_trackMaxLength = 9999;

    // Active WP
    QGeoCoordinate m_activeWpPos;

    // Settings
    bool m_followAircraft = false;
    QString m_tileServerUrl;

    // Layers
    bool m_showTrack = true;
    bool m_showWaypoints = true;
    bool m_showZones = true;
    bool m_showSettlements = true;
    bool m_showLayerPanel = true;

    // FPS
    QString m_fpsText;
    int m_frameCount = 0;
    double m_fpsTimestamp = 0.0;
    double m_fpsUpdateMs = 0.0;
    int m_fpsDrops = 0;

    // Drawing
    bool m_drawingMode = false;
    QVariantList m_drawingPath;
    bool m_leftClickMode = false;

    // Avoidance
    QVariantList m_avoidancePath;
    QVariantList m_plannedDirectPath;

    // Viewport bounds (for settlement filtering)
    double m_viewSouth = 0, m_viewWest = 0, m_viewNorth = 0, m_viewEast = 0;
    int m_currentZoom = 10;

    // Settlement unload timer
    QTimer* m_settlementUnloadTimer = nullptr;

    // Settlement unlimited cache (survives LRU eviction in model)
    QVector<SettlementCacheItem> m_stlPolyCache;
    QSet<QString> m_stlPolyCacheSigs;  // dedup: "p_{lat}_{lon}_{count}"

    // Editing
    QString m_editingZoneId;

    // Current waypoints (for conflict resolution)
    QVector<QVariantMap> m_currentWaypoints;

    // Models
    WaypointListModel m_waypointModel{this};
    RouteSegmentModel m_routeSegmentModel{this};
    ZoneListModel m_zoneModel{this};
    DashBorderModel m_zoneBorderModel{0.0004, 0.0003, 0, this};
    SettlementPolyModel m_settlementPolyModel{this};
    SettlementCircleModel m_settlementCircleModel{this};
    DashBorderModel m_settlementBorderModel{0.0006, 0.0004, 2000, this};
    ConflictSegmentModel m_conflictModel{this};
    ConflictPointModel m_conflictPointModel{this};
    SimpleVertexModel m_drawingVertexModel{this};
    SimpleVertexModel m_editingVertexModel{this};
    SimpleVertexModel m_editingMidpointModel{this};
    ObstacleWarningModel m_obstacleWarningModel{this};

    // Zone details for obstacle warning tooltip generation
    QVariantList m_zoneDetails;  // raw zone maps with altitude/mode/buffer
    void rebuildObstacleWarnings();
    static std::pair<double, double> bboxCenter(const QVariantList& coords);
};

} // namespace vtol
