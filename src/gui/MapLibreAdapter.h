#pragma once

#include <QObject>
#include <QVariantMap>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>

namespace QMapLibre { class Map; }

namespace vtol {

class MapBackend;

/// Bridge between MapBackend data and MapLibre map sources/layers.
/// Listens to MapBackend signals and updates GeoJSON sources on the map.
class MapLibreAdapter : public QObject {
    Q_OBJECT
public:
    explicit MapLibreAdapter(QMapLibre::Map* map, MapBackend* backend, QObject* parent = nullptr);

private slots:
    // --- Source updates (used as slots via SLOT() macro for model signals) ---
    void updateAircraftSource();
    void updateTrackSource();
    void appendTrackCoord(double lat, double lon);
    void updateAvoidanceSource();
    void updateDirectPathSource();
    void updateDrawingSource();
    void updateHomeSource();
    void updateOperationalWpSource();
    void updateWaypointSources();
    void updateZoneSources();
    void updateSettlementSources();
    void updateConflictSources();
    void updateEditingSources();
    void updateMeshSource();

    // --- Viewport culling ---
    void onViewportChanged(double south, double west, double north, double east);
    void applyViewportCulling();

    // --- Visibility ---
    void onShowTrackChanged();
    void onShowWaypointsChanged();
    void onShowZonesChanged();
    void onShowSettlementsChanged();

private:
    void setLayerVisibility(const QString& prefix, bool visible);

    // --- Setup ---
    void addAllSources();
    void addAllLayers();
    void addAircraftImage();
    void connectSignals();

    // --- Helpers ---
    void setSourceGeoJson(const QString& sourceId, const QJsonObject& geojson);
    bool bboxIntersectsRenderArea(double minLat, double minLon, double maxLat, double maxLon) const;
    static QJsonObject makePointFeature(double lat, double lon, const QJsonObject& props = {});
    static QJsonObject makeLineFeature(const QJsonArray& coords, const QJsonObject& props = {});
    static QJsonObject makePolygonFeature(const QJsonArray& ring, const QJsonObject& props = {});
    static QJsonObject emptyFeatureCollection();
    static QJsonArray coord(double lat, double lon);

    QMapLibre::Map* m_map;
    MapBackend* m_backend;

    // Track: incremental append cache
    QJsonArray m_trackCoords;

    // Viewport culling — render area (shrunk viewport)
    double m_renderSouth = 0, m_renderWest = 0, m_renderNorth = 0, m_renderEast = 0;
    bool m_hasRenderArea = false;
    static constexpr double RENDER_AREA_MARGIN = 0.05;  // 5% inset from each edge
    QTimer* m_cullDebounce = nullptr;
};

} // namespace vtol
