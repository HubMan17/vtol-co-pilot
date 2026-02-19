#include "MapLibreAdapter.h"
#include "MapBackend.h"
#include "models/MapModels.h"

#include <QMapLibre/Map>
#include <QMapLibre/Types>
#include <QJsonDocument>
#include <QImage>
#include <QPainter>
#include <QAbstractItemModel>
#include <cmath>
#include <spdlog/spdlog.h>

namespace vtol {

// ═══════════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════════

MapLibreAdapter::MapLibreAdapter(QMapLibre::Map* map, MapBackend* backend, QObject* parent)
    : QObject(parent), m_map(map), m_backend(backend)
{
    addAllSources();
    addAllLayers();
    addAircraftImage();
    connectSignals();

    // Initial data push
    updateAircraftSource();
    updateTrackSource();
    updateHomeSource();
    updateWaypointSources();
    updateZoneSources();
    updateSettlementSources();
    updateConflictSources();
    updateAvoidanceSource();
    updateDirectPathSource();
    updateDrawingSource();
    updateEditingSources();

    SPDLOG_INFO("[MapLibreAdapter] initialized with {} sources", 17);
}

// ═══════════════════════════════════════════════════════════
//  GeoJSON helpers
// ═══════════════════════════════════════════════════════════

QJsonArray MapLibreAdapter::coord(double lat, double lon)
{
    // GeoJSON is [longitude, latitude]
    return QJsonArray{lon, lat};
}

QJsonObject MapLibreAdapter::makePointFeature(double lat, double lon, const QJsonObject& props)
{
    QJsonObject geom;
    geom["type"] = "Point";
    geom["coordinates"] = coord(lat, lon);

    QJsonObject f;
    f["type"] = "Feature";
    f["geometry"] = geom;
    f["properties"] = props;
    return f;
}

QJsonObject MapLibreAdapter::makeLineFeature(const QJsonArray& coords, const QJsonObject& props)
{
    QJsonObject geom;
    geom["type"] = "LineString";
    geom["coordinates"] = coords;

    QJsonObject f;
    f["type"] = "Feature";
    f["geometry"] = geom;
    f["properties"] = props;
    return f;
}

QJsonObject MapLibreAdapter::makePolygonFeature(const QJsonArray& ring, const QJsonObject& props)
{
    QJsonObject geom;
    geom["type"] = "Polygon";
    QJsonArray rings;
    rings.append(ring);
    geom["coordinates"] = rings;

    QJsonObject f;
    f["type"] = "Feature";
    f["geometry"] = geom;
    f["properties"] = props;
    return f;
}

QJsonObject MapLibreAdapter::emptyFeatureCollection()
{
    QJsonObject fc;
    fc["type"] = "FeatureCollection";
    QJsonArray features;
    fc["features"] = features;
    return fc;
}

void MapLibreAdapter::setSourceGeoJson(const QString& sourceId, const QJsonObject& geojson)
{
    int featureCount = geojson["features"].toArray().size();
    // Log non-aircraft updates (aircraft is too frequent at 10Hz)
    if (featureCount > 0 && sourceId != "aircraft" && sourceId != "track" && sourceId != "nav-line") {
        SPDLOG_INFO("[MapLibreAdapter] updateSource '{}': {} features",
                     sourceId.toStdString(), featureCount);
    }
    // MapLibre Native Qt requires QByteArray for GeoJSON data
    // (conversion_p.hpp toGeoJSON: "JSON data must be in QByteArray")
    QByteArray jsonBytes = QJsonDocument(geojson).toJson(QJsonDocument::Compact);
    QVariantMap params;
    params["data"] = jsonBytes;
    if (m_map->sourceExists(sourceId)) {
        m_map->updateSource(sourceId, params);
    } else {
        SPDLOG_WARN("[MapLibreAdapter] Source '{}' does not exist!", sourceId.toStdString());
    }
}

// ═══════════════════════════════════════════════════════════
//  Source creation
// ═══════════════════════════════════════════════════════════

void MapLibreAdapter::addAllSources()
{
    auto addGeoJsonSource = [this](const QString& id) {
        QVariantMap params;
        params["type"] = "geojson";
        QByteArray emptyData = QJsonDocument(emptyFeatureCollection()).toJson(QJsonDocument::Compact);
        params["data"] = emptyData;
        m_map->addSource(id, params);
    };

    addGeoJsonSource("aircraft");
    addGeoJsonSource("home");
    addGeoJsonSource("track");
    addGeoJsonSource("route");
    addGeoJsonSource("waypoints");
    addGeoJsonSource("zones");
    addGeoJsonSource("conflicts");
    addGeoJsonSource("conflict-pts");
    addGeoJsonSource("avoidance");
    addGeoJsonSource("direct-path");
    addGeoJsonSource("drawing");
    addGeoJsonSource("draw-vertices");
    addGeoJsonSource("edit-vertices");
    addGeoJsonSource("edit-midpoints");
    addGeoJsonSource("nav-line");
    addGeoJsonSource("stl-polys");
    addGeoJsonSource("stl-circles");
}

// ═══════════════════════════════════════════════════════════
//  Layer creation (back-to-front order)
// ═══════════════════════════════════════════════════════════

void MapLibreAdapter::addAllLayers()
{
    auto addFill = [this](const QString& id, const QString& source,
                          const QString& color, double opacity) {
        QVariantMap params;
        params["id"] = id;
        params["type"] = "fill";
        params["source"] = source;
        m_map->addLayer(id, params);
        m_map->setPaintProperty(id, "fill-color", color);
        m_map->setPaintProperty(id, "fill-opacity", opacity);
    };

    auto addLine = [this](const QString& id, const QString& source,
                          const QString& color, double width,
                          const QVariantList& dasharray = {}) {
        QVariantMap params;
        params["id"] = id;
        params["type"] = "line";
        params["source"] = source;
        m_map->addLayer(id, params);
        m_map->setPaintProperty(id, "line-color", color);
        m_map->setPaintProperty(id, "line-width", width);
        if (!dasharray.isEmpty())
            m_map->setPaintProperty(id, "line-dasharray", dasharray);
    };

    auto addCircle = [this](const QString& id, const QString& source,
                            const QString& color, double radius,
                            const QString& strokeColor = {},
                            double strokeWidth = 0.0) {
        QVariantMap params;
        params["id"] = id;
        params["type"] = "circle";
        params["source"] = source;
        m_map->addLayer(id, params);
        m_map->setPaintProperty(id, "circle-color", color);
        m_map->setPaintProperty(id, "circle-radius", radius);
        if (!strokeColor.isEmpty()) {
            m_map->setPaintProperty(id, "circle-stroke-color", strokeColor);
            m_map->setPaintProperty(id, "circle-stroke-width", strokeWidth);
        }
    };

    auto addSymbol = [this](const QString& id, const QString& source) {
        QVariantMap params;
        params["id"] = id;
        params["type"] = "symbol";
        params["source"] = source;
        m_map->addLayer(id, params);
    };

    // --- Settlement polygons ---
    addFill("stl-poly-fill", "stl-polys", "#FFA500", 0.15);
    addLine("stl-poly-border", "stl-polys", "#FFA500", 1.5, {4.0, 3.0});

    // --- Settlement circles ---
    addCircle("stl-circle", "stl-circles", "#FFA500", 4.0, "#FFA500", 1.0);

    // --- Zones ---
    addFill("zone-fill", "zones", "#FF4444", 0.15);
    addLine("zone-border", "zones", "#FF4444", 2.0, {6.0, 4.0});

    // --- Track ---
    addLine("track-line", "track", "#00E676", 2.0);

    // --- Nav line (aircraft → target WP) ---
    addLine("nav-line", "nav-line", "#00E676", 1.5, {6.0, 4.0});

    // --- Route ---
    addLine("route-line", "route", "#00E676", 2.0);

    // --- Conflicts ---
    addLine("conflict-line", "conflicts", "#FF4444", 3.0);

    // --- Direct path ---
    addLine("direct-line", "direct-path", "#888888", 1.0, {4.0, 4.0});

    // --- Avoidance ---
    addLine("avoidance-line", "avoidance", "#00FF88", 2.0);

    // --- Conflict points ---
    addCircle("conflict-pt", "conflict-pts", "#FF4444", 8.0, "#FFFFFF", 2.0);

    // --- Waypoints (numbered icon — no text-field, avoids glyphs dependency) ---
    addSymbol("wp-icon", "waypoints");
    // Data-driven icon: "wp-1", "wp-2", … based on feature property "icon"
    m_map->setLayoutProperty("wp-icon", "icon-image", QVariantList{"get", "icon"});
    m_map->setLayoutProperty("wp-icon", "icon-size", 1.0);
    m_map->setLayoutProperty("wp-icon", "icon-allow-overlap", true);

    // --- Drawing ---
    addLine("draw-line", "drawing", "#FFAA00", 2.0);
    addCircle("draw-vertex", "draw-vertices", "#FFAA00", 5.0, "#FFFFFF", 1.5);

    // --- Editing ---
    addCircle("edit-vertex", "edit-vertices", "#00AAFF", 6.0, "#FFFFFF", 2.0);
    addCircle("edit-midpoint", "edit-midpoints", "#00AAFF", 4.0, "#FFFFFF", 1.0);

    // --- Home ---
    addSymbol("home-icon", "home");
    m_map->setLayoutProperty("home-icon", "icon-image", "home-marker");
    m_map->setLayoutProperty("home-icon", "icon-size", 1.0);
    m_map->setLayoutProperty("home-icon", "icon-allow-overlap", true);

    // --- Aircraft ---
    addSymbol("aircraft-icon", "aircraft");
    m_map->setLayoutProperty("aircraft-icon", "icon-image", "aircraft-marker");
    m_map->setLayoutProperty("aircraft-icon", "icon-size", 1.0);
    m_map->setLayoutProperty("aircraft-icon", "icon-rotate", QVariantList{"get", "heading"});
    m_map->setLayoutProperty("aircraft-icon", "icon-rotation-alignment", "map");
    m_map->setLayoutProperty("aircraft-icon", "icon-allow-overlap", true);
}

// ═══════════════════════════════════════════════════════════
//  Aircraft marker image
// ═══════════════════════════════════════════════════════════

void MapLibreAdapter::addAircraftImage()
{
    // Aircraft top-down silhouette (48x48, pointing up = north)
    QImage img(48, 48, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);

    const double cx = 24.0, cy = 24.0;

    // Shadow offset
    p.save();
    p.translate(1.5, 1.5);
    p.setBrush(QColor(0, 0, 0, 60));
    p.setPen(Qt::NoPen);
    // Fuselage
    QPolygonF fShadow;
    fShadow << QPointF(cx, 4) << QPointF(cx + 3, 14) << QPointF(cx + 2.5, 38)
            << QPointF(cx, 42) << QPointF(cx - 2.5, 38) << QPointF(cx - 3, 14);
    p.drawPolygon(fShadow);
    // Wings
    QPolygonF wShadow;
    wShadow << QPointF(cx, 16) << QPointF(cx + 17, 26) << QPointF(cx + 16, 28)
            << QPointF(cx, 22) << QPointF(cx - 16, 28) << QPointF(cx - 17, 26);
    p.drawPolygon(wShadow);
    p.restore();

    // Fuselage
    QPolygonF fuselage;
    fuselage << QPointF(cx, 4) << QPointF(cx + 3, 14) << QPointF(cx + 2.5, 38)
             << QPointF(cx, 42) << QPointF(cx - 2.5, 38) << QPointF(cx - 3, 14);
    p.setBrush(QColor("#00E676"));
    p.setPen(QPen(Qt::white, 1.2));
    p.drawPolygon(fuselage);

    // Main wings
    QPolygonF wings;
    wings << QPointF(cx, 16) << QPointF(cx + 17, 26) << QPointF(cx + 16, 28)
          << QPointF(cx, 22) << QPointF(cx - 16, 28) << QPointF(cx - 17, 26);
    p.drawPolygon(wings);

    // Tail wings
    QPolygonF tail;
    tail << QPointF(cx, 35) << QPointF(cx + 8, 40) << QPointF(cx + 7, 42)
         << QPointF(cx, 38) << QPointF(cx - 7, 42) << QPointF(cx - 8, 40);
    p.drawPolygon(tail);

    p.end();

    m_map->addImage("aircraft-marker", img);

    // Waypoint marker (numbered circle with pin)
    QImage wpImg(28, 28, QImage::Format_ARGB32_Premultiplied);
    wpImg.fill(Qt::transparent);
    QPainter wp(&wpImg);
    wp.setRenderHint(QPainter::Antialiasing);

    // Drop shadow
    wp.setBrush(QColor(0, 0, 0, 50));
    wp.setPen(Qt::NoPen);
    wp.drawEllipse(QPointF(14.5, 14.5), 11.5, 11.5);

    // Main circle
    wp.setBrush(QColor("#00C853"));
    wp.setPen(QPen(Qt::white, 2.0));
    wp.drawEllipse(QPointF(14, 14), 11, 11);
    wp.end();

    m_map->addImage("wp-marker", wpImg);

    // Pre-render numbered waypoint markers: wp-1 … wp-30
    for (int n = 1; n <= 30; ++n) {
        QImage numImg(28, 28, QImage::Format_ARGB32_Premultiplied);
        numImg.fill(Qt::transparent);
        QPainter np(&numImg);
        np.setRenderHint(QPainter::Antialiasing);
        // Shadow
        np.setBrush(QColor(0, 0, 0, 50));
        np.setPen(Qt::NoPen);
        np.drawEllipse(QPointF(14.5, 14.5), 11.5, 11.5);
        // Circle
        np.setBrush(QColor("#00C853"));
        np.setPen(QPen(Qt::white, 2.0));
        np.drawEllipse(QPointF(14, 14), 11, 11);
        // Number
        np.setPen(Qt::white);
        QFont nf("Arial", n < 10 ? 11 : 9, QFont::Bold);
        np.setFont(nf);
        np.drawText(QRectF(0, 0, 28, 28), Qt::AlignCenter, QString::number(n));
        np.end();
        m_map->addImage(QStringLiteral("wp-%1").arg(n), numImg);
    }

    // Home marker
    QImage homeImg(28, 28, QImage::Format_ARGB32_Premultiplied);
    homeImg.fill(Qt::transparent);
    QPainter hp(&homeImg);
    hp.setRenderHint(QPainter::Antialiasing);
    hp.setBrush(QColor(0, 0, 0, 50));
    hp.setPen(Qt::NoPen);
    hp.drawEllipse(QPointF(14.5, 14.5), 11.5, 11.5);
    hp.setBrush(QColor("#FFD700"));
    hp.setPen(QPen(Qt::white, 2.0));
    hp.drawEllipse(QPointF(14, 14), 11, 11);
    // H letter
    hp.setPen(Qt::NoPen);
    hp.setBrush(Qt::white);
    QFont hf("Arial", 11, QFont::Bold);
    hp.setFont(hf);
    hp.drawText(QRectF(0, 0, 28, 28), Qt::AlignCenter, "H");
    hp.end();

    m_map->addImage("home-marker", homeImg);
}

// ═══════════════════════════════════════════════════════════
//  Signal connections
// ═══════════════════════════════════════════════════════════

void MapLibreAdapter::connectSignals()
{
    // Aircraft
    connect(m_backend, &MapBackend::aircraftPositionChanged, this, &MapLibreAdapter::updateAircraftSource);
    connect(m_backend, &MapBackend::aircraftHeadingChanged, this, &MapLibreAdapter::updateAircraftSource);

    // Track
    connect(m_backend, &MapBackend::trackPathChanged, this, &MapLibreAdapter::updateTrackSource);
    connect(m_backend, &MapBackend::trackCoordinateAdded, this, &MapLibreAdapter::appendTrackCoord);

    // Home
    connect(m_backend, &MapBackend::homePositionChanged, this, &MapLibreAdapter::updateHomeSource);

    // Avoidance / direct path
    connect(m_backend, &MapBackend::avoidancePathChanged, this, &MapLibreAdapter::updateAvoidanceSource);
    connect(m_backend, &MapBackend::plannedDirectPathChanged, this, &MapLibreAdapter::updateDirectPathSource);

    // Drawing
    connect(m_backend, &MapBackend::drawingPathChanged, this, &MapLibreAdapter::updateDrawingSource);

    // Visibility toggles
    connect(m_backend, &MapBackend::showTrackChanged, this, &MapLibreAdapter::onShowTrackChanged);
    connect(m_backend, &MapBackend::showWaypointsChanged, this, &MapLibreAdapter::onShowWaypointsChanged);
    connect(m_backend, &MapBackend::showZonesChanged, this, &MapLibreAdapter::onShowZonesChanged);
    connect(m_backend, &MapBackend::showSettlementsChanged, this, &MapLibreAdapter::onShowSettlementsChanged);

    // Models: connect dataChanged / rowsInserted / rowsRemoved → full rebuild
    auto connectModel = [](QAbstractItemModel* model, auto* receiver, auto slot) {
        QObject::connect(model, &QAbstractItemModel::dataChanged, receiver,
                         [=](auto&&...) { (receiver->*slot)(); });
        QObject::connect(model, &QAbstractItemModel::rowsInserted, receiver,
                         [=](auto&&...) { (receiver->*slot)(); });
        QObject::connect(model, &QAbstractItemModel::rowsRemoved, receiver,
                         [=](auto&&...) { (receiver->*slot)(); });
        QObject::connect(model, &QAbstractItemModel::modelReset, receiver, slot);
    };

    connectModel(qobject_cast<QAbstractItemModel*>(m_backend->waypointModel()),
                 this, &MapLibreAdapter::updateWaypointSources);
    connectModel(qobject_cast<QAbstractItemModel*>(m_backend->routeSegmentModel()),
                 this, &MapLibreAdapter::updateWaypointSources);
    connectModel(qobject_cast<QAbstractItemModel*>(m_backend->zoneModel()),
                 this, &MapLibreAdapter::updateZoneSources);
    connectModel(qobject_cast<QAbstractItemModel*>(m_backend->settlementPolyModel()),
                 this, &MapLibreAdapter::updateSettlementSources);
    connectModel(qobject_cast<QAbstractItemModel*>(m_backend->settlementCircleModel()),
                 this, &MapLibreAdapter::updateSettlementSources);
    connectModel(qobject_cast<QAbstractItemModel*>(m_backend->conflictModel()),
                 this, &MapLibreAdapter::updateConflictSources);
    connectModel(qobject_cast<QAbstractItemModel*>(m_backend->conflictPointModel()),
                 this, &MapLibreAdapter::updateConflictSources);
    connectModel(qobject_cast<QAbstractItemModel*>(m_backend->editingVertexModel()),
                 this, &MapLibreAdapter::updateEditingSources);
    connectModel(qobject_cast<QAbstractItemModel*>(m_backend->editingMidpointModel()),
                 this, &MapLibreAdapter::updateEditingSources);
}

// ═══════════════════════════════════════════════════════════
//  Source update implementations
// ═══════════════════════════════════════════════════════════

void MapLibreAdapter::updateAircraftSource()
{
    QJsonObject fc = emptyFeatureCollection();
    double acLat = 0, acLon = 0;
    bool acVis = m_backend->aircraftVisible();
    if (acVis) {
        auto pos = m_backend->aircraftPosition();
        acLat = pos.latitude();
        acLon = pos.longitude();
        QJsonObject props;
        props["heading"] = m_backend->aircraftHeading();
        QJsonArray features;
        features.append(makePointFeature(acLat, acLon, props));
        fc["features"] = features;
    }
    setSourceGeoJson("aircraft", fc);

    // Nav-line: aircraft → first active (or first) waypoint
    QJsonObject navFc = emptyFeatureCollection();
    if (acVis) {
        auto* wpModel = qobject_cast<WaypointListModel*>(m_backend->waypointModel());
        if (wpModel && !wpModel->items().isEmpty()) {
            const auto& items = wpModel->items();
            // Find active WP, fallback to first
            int idx = 0;
            for (int i = 0; i < items.size(); ++i) {
                if (items[i].state == "active") { idx = i; break; }
            }
            QJsonArray coords;
            coords.append(coord(acLat, acLon));
            coords.append(coord(items[idx].lat, items[idx].lon));
            QJsonArray features;
            features.append(makeLineFeature(coords));
            navFc["features"] = features;
        }
    }
    setSourceGeoJson("nav-line", navFc);
}

void MapLibreAdapter::updateTrackSource()
{
    // Full rebuild of track coordinates cache
    m_trackCoords = QJsonArray();
    const auto& path = m_backend->trackPath();
    for (const auto& v : path) {
        auto c = v.value<QGeoCoordinate>();
        m_trackCoords.append(coord(c.latitude(), c.longitude()));
    }

    QJsonObject fc = emptyFeatureCollection();
    if (m_trackCoords.size() >= 2) {
        QJsonArray features;
        features.append(makeLineFeature(m_trackCoords));
        fc["features"] = features;
    }
    setSourceGeoJson("track", fc);
}

void MapLibreAdapter::appendTrackCoord(double lat, double lon)
{
    m_trackCoords.append(coord(lat, lon));

    QJsonObject fc = emptyFeatureCollection();
    if (m_trackCoords.size() >= 2) {
        QJsonArray features;
        features.append(makeLineFeature(m_trackCoords));
        fc["features"] = features;
    }
    setSourceGeoJson("track", fc);
}

void MapLibreAdapter::updateHomeSource()
{
    QJsonObject fc = emptyFeatureCollection();
    if (m_backend->homeVisible()) {
        auto pos = m_backend->homePosition();
        QJsonArray features;
        features.append(makePointFeature(pos.latitude(), pos.longitude()));
        fc["features"] = features;
    }
    setSourceGeoJson("home", fc);
}

void MapLibreAdapter::updateAvoidanceSource()
{
    QJsonObject fc = emptyFeatureCollection();
    const auto& path = m_backend->avoidancePath();
    if (path.size() >= 2) {
        QJsonArray coords;
        for (const auto& v : path) {
            auto c = v.value<QGeoCoordinate>();
            coords.append(coord(c.latitude(), c.longitude()));
        }
        QJsonArray features;
        features.append(makeLineFeature(coords));
        fc["features"] = features;
    }
    setSourceGeoJson("avoidance", fc);
}

void MapLibreAdapter::updateDirectPathSource()
{
    QJsonObject fc = emptyFeatureCollection();
    const auto& path = m_backend->plannedDirectPath();
    if (path.size() >= 2) {
        QJsonArray coords;
        for (const auto& v : path) {
            auto c = v.value<QGeoCoordinate>();
            coords.append(coord(c.latitude(), c.longitude()));
        }
        QJsonArray features;
        features.append(makeLineFeature(coords));
        fc["features"] = features;
    }
    setSourceGeoJson("direct-path", fc);
}

void MapLibreAdapter::updateDrawingSource()
{
    const auto& path = m_backend->drawingPath();
    auto* vertexModel = qobject_cast<SimpleVertexModel*>(m_backend->drawingVertexModel());

    // Line
    QJsonObject lineFc = emptyFeatureCollection();
    if (path.size() >= 2) {
        QJsonArray coords;
        for (const auto& v : path) {
            auto c = v.value<QGeoCoordinate>();
            coords.append(coord(c.latitude(), c.longitude()));
        }
        QJsonArray features;
        features.append(makeLineFeature(coords));
        lineFc["features"] = features;
    }
    setSourceGeoJson("drawing", lineFc);

    // Vertices
    QJsonObject vertFc = emptyFeatureCollection();
    if (vertexModel) {
        auto pts = vertexModel->getPoints();
        QJsonArray features;
        for (int i = 0; i < pts.size(); ++i) {
            QJsonObject props;
            props["index"] = i;
            features.append(makePointFeature(pts[i].x(), pts[i].y(), props));
        }
        vertFc["features"] = features;
    }
    setSourceGeoJson("draw-vertices", vertFc);
}

void MapLibreAdapter::updateWaypointSources()
{
    auto* wpModel = qobject_cast<WaypointListModel*>(m_backend->waypointModel());
    auto* routeModel = qobject_cast<RouteSegmentModel*>(m_backend->routeSegmentModel());
    SPDLOG_INFO("[MapLibreAdapter] updateWaypointSources: wp={} route={}",
                 wpModel ? wpModel->items().size() : -1,
                 routeModel ? routeModel->items().size() : -1);

    // Waypoints as points
    QJsonObject wpFc = emptyFeatureCollection();
    if (wpModel) {
        const auto& items = wpModel->items();
        QJsonArray features;
        for (int i = 0; i < items.size(); ++i) {
            QJsonObject props;
            int num = i + 1;
            props["index"] = QString::number(num);
            props["icon"] = QStringLiteral("wp-%1").arg(qMin(num, 30));
            props["state"] = items[i].state;
            features.append(makePointFeature(items[i].lat, items[i].lon, props));
        }
        wpFc["features"] = features;
    }
    setSourceGeoJson("waypoints", wpFc);

    // Route segments as lines
    QJsonObject routeFc = emptyFeatureCollection();
    if (routeModel) {
        const auto& items = routeModel->items();
        QJsonArray features;
        for (const auto& seg : items) {
            QJsonArray coords;
            coords.append(coord(seg.fromLat, seg.fromLon));
            coords.append(coord(seg.toLat, seg.toLon));
            QJsonObject props;
            props["state"] = seg.state;
            features.append(makeLineFeature(coords, props));
        }
        routeFc["features"] = features;
    }
    setSourceGeoJson("route", routeFc);
}

void MapLibreAdapter::updateZoneSources()
{
    auto* model = qobject_cast<ZoneListModel*>(m_backend->zoneModel());
    QJsonObject fc = emptyFeatureCollection();
    if (model) {
        const auto& items = model->items();
        QJsonArray features;
        for (const auto& zone : items) {
            QJsonArray ring;
            for (const auto& pt : zone.points) {
                auto list = pt.toList();
                if (list.size() >= 2)
                    ring.append(coord(list[0].toDouble(), list[1].toDouble()));
            }
            // Close the ring
            if (ring.size() >= 3) {
                ring.append(ring[0]);
                QJsonObject props;
                props["id"] = zone.id;
                props["name"] = zone.name;
                features.append(makePolygonFeature(ring, props));
            }
        }
        fc["features"] = features;
    }
    setSourceGeoJson("zones", fc);
}

void MapLibreAdapter::updateSettlementSources()
{
    auto* polyModel = qobject_cast<SettlementPolyModel*>(m_backend->settlementPolyModel());
    auto* circleModel = qobject_cast<SettlementCircleModel*>(m_backend->settlementCircleModel());

    // Polygons
    QJsonObject polyFc = emptyFeatureCollection();
    if (polyModel) {
        const auto& items = polyModel->items();
        QJsonArray features;
        for (const auto& poly : items) {
            QJsonArray ring;
            for (const auto& pt : poly) {
                auto list = pt.toList();
                if (list.size() >= 2)
                    ring.append(coord(list[0].toDouble(), list[1].toDouble()));
            }
            if (ring.size() >= 3) {
                ring.append(ring[0]);
                features.append(makePolygonFeature(ring));
            }
        }
        polyFc["features"] = features;
    }
    setSourceGeoJson("stl-polys", polyFc);

    // Circles (as points — MapLibre renders them with circle layer)
    QJsonObject circleFc = emptyFeatureCollection();
    if (circleModel) {
        const auto& items = circleModel->items();
        QJsonArray features;
        for (const auto& item : items) {
            QJsonObject props;
            props["radius"] = item.radius;
            features.append(makePointFeature(item.lat, item.lon, props));
        }
        circleFc["features"] = features;
    }
    setSourceGeoJson("stl-circles", circleFc);
}

void MapLibreAdapter::updateConflictSources()
{
    auto* segModel = qobject_cast<ConflictSegmentModel*>(m_backend->conflictModel());
    auto* ptModel = qobject_cast<ConflictPointModel*>(m_backend->conflictPointModel());

    // Conflict segments
    QJsonObject segFc = emptyFeatureCollection();
    if (segModel) {
        const auto& items = segModel->items();
        QJsonArray features;
        for (const auto& seg : items) {
            QJsonArray coords;
            coords.append(coord(seg.fromLat, seg.fromLon));
            coords.append(coord(seg.toLat, seg.toLon));
            features.append(makeLineFeature(coords));
        }
        segFc["features"] = features;
    }
    setSourceGeoJson("conflicts", segFc);

    // Conflict points
    QJsonObject ptFc = emptyFeatureCollection();
    if (ptModel) {
        const auto& items = ptModel->items();
        QJsonArray features;
        for (const auto& item : items) {
            QJsonObject props;
            props["reason"] = item.reason;
            features.append(makePointFeature(item.lat, item.lon, props));
        }
        ptFc["features"] = features;
    }
    setSourceGeoJson("conflict-pts", ptFc);
}

void MapLibreAdapter::updateEditingSources()
{
    auto* vertexModel = qobject_cast<SimpleVertexModel*>(m_backend->editingVertexModel());
    auto* midModel = qobject_cast<SimpleVertexModel*>(m_backend->editingMidpointModel());

    // Editing vertices
    QJsonObject vertFc = emptyFeatureCollection();
    if (vertexModel) {
        auto pts = vertexModel->getPoints();
        QJsonArray features;
        for (int i = 0; i < pts.size(); ++i) {
            QJsonObject props;
            props["index"] = i;
            features.append(makePointFeature(pts[i].x(), pts[i].y(), props));
        }
        vertFc["features"] = features;
    }
    setSourceGeoJson("edit-vertices", vertFc);

    // Midpoints
    QJsonObject midFc = emptyFeatureCollection();
    if (midModel) {
        auto pts = midModel->getPoints();
        QJsonArray features;
        for (int i = 0; i < pts.size(); ++i) {
            QJsonObject props;
            props["midIndex"] = i;
            features.append(makePointFeature(pts[i].x(), pts[i].y(), props));
        }
        midFc["features"] = features;
    }
    setSourceGeoJson("edit-midpoints", midFc);
}

// ═══════════════════════════════════════════════════════════
//  Visibility
// ═══════════════════════════════════════════════════════════

void MapLibreAdapter::setLayerVisibility(const QString& prefix, bool visible)
{
    QString vis = visible ? "visible" : "none";
    auto layerIds = m_map->layerIds();
    for (const auto& id : layerIds) {
        if (id.startsWith(prefix))
            m_map->setLayoutProperty(id, "visibility", vis);
    }
}

void MapLibreAdapter::onShowTrackChanged()
{
    setLayerVisibility("track-", m_backend->showTrack());
}

void MapLibreAdapter::onShowWaypointsChanged()
{
    bool v = m_backend->showWaypoints();
    setLayerVisibility("wp-", v);
    setLayerVisibility("route-", v);
}

void MapLibreAdapter::onShowZonesChanged()
{
    setLayerVisibility("zone-", m_backend->showZones());
}

void MapLibreAdapter::onShowSettlementsChanged()
{
    setLayerVisibility("stl-", m_backend->showSettlements());
}

} // namespace vtol
