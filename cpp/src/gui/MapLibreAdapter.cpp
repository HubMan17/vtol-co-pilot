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
    m_cullDebounce = new QTimer(this);
    m_cullDebounce->setSingleShot(true);
    m_cullDebounce->setInterval(150);
    connect(m_cullDebounce, &QTimer::timeout, this, &MapLibreAdapter::applyViewportCulling);

    addAllSources();
    addAllLayers();
    addAircraftImage();
    connectSignals();

    // Verify zone source/layers were created
    SPDLOG_INFO("[MapLibreAdapter] sourceExists('zones')={}, layerExists('zone-fill')={}, layerExists('zone-border')={}",
                 m_map->sourceExists("zones"), m_map->layerExists("zone-fill"), m_map->layerExists("zone-border"));
    SPDLOG_INFO("[MapLibreAdapter] sourceExists('stl-polys')={}, layerExists('stl-poly-fill')={}",
                 m_map->sourceExists("stl-polys"), m_map->layerExists("stl-poly-fill"));

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

    // Apply current layer visibility from backend (signals fired before adapter existed)
    onShowTrackChanged();
    onShowWaypointsChanged();
    onShowZonesChanged();
    onShowSettlementsChanged();

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

    // --- Settlement polygons (red) ---
    addFill("stl-poly-fill", "stl-polys", "#CC0000", 0.18);
    addLine("stl-poly-border", "stl-polys", "#CC0000", 2.0);

    // --- Settlement circles (red) ---
    addCircle("stl-circle", "stl-circles", "#CC0000", 6.0, "#CC0000", 1.0);

    // --- Zones (dark fill, dark red-black border) ---
    addFill("zone-fill", "zones", "#000000", 0.60);
    addLine("zone-border", "zones", "#882020", 2.5);

    // Zone: zoom-dependent visibility (off at zoom ≤11, visible 12-14, fade out 14-15.5)
    // Using 11.99 ensures zones are invisible when UI shows zoom "11" (int truncation)
    m_map->setPaintProperty("zone-fill", "fill-opacity",
        QVariantList{"interpolate", QVariantList{"linear"}, QVariantList{"zoom"},
            11.99, 0.0,  12.0, 0.60,  14.0, 0.60,  15.5, 0.0});
    m_map->setPaintProperty("zone-border", "line-opacity",
        QVariantList{"interpolate", QVariantList{"linear"}, QVariantList{"zoom"},
            11.99, 0.0,  12.0, 1.0,  14.0, 1.0,  15.5, 0.0});
    m_map->setPaintProperty("zone-border", "line-width",
        QVariantList{"interpolate", QVariantList{"linear"}, QVariantList{"zoom"},
            12, 1.5,  13, 2.0,  14, 2.5});

    // --- Zone labels (QPainter-rendered name icons at polygon centroids) ---
    // Uses same "zones" source as fill/border — MapLibre places symbols at polygon centroid
    addSymbol("zone-label-icon", "zones");
    m_map->setLayoutProperty("zone-label-icon", "icon-image", QVariantList{"get", "icon"});
    m_map->setLayoutProperty("zone-label-icon", "icon-allow-overlap", true);
    // Zoom-dependent size: small at z12, grows to full at z14
    m_map->setLayoutProperty("zone-label-icon", "icon-size",
        QVariantList{"interpolate", QVariantList{"linear"}, QVariantList{"zoom"},
            12.0, 0.55,  13.0, 0.75,  14.0, 1.0});
    m_map->setPaintProperty("zone-label-icon", "icon-opacity",
        QVariantList{"interpolate", QVariantList{"linear"}, QVariantList{"zoom"},
            11.99, 0.0,  12.0, 1.0,  14.0, 1.0,  15.5, 0.0});

    // Settlement: zoom-dependent visibility (off at zoom ≤11, visible 12-14, fade out 14-15.5)
    m_map->setPaintProperty("stl-poly-fill", "fill-opacity",
        QVariantList{"interpolate", QVariantList{"linear"}, QVariantList{"zoom"},
            11.99, 0.0,  12.0, 0.18,  14.0, 0.18,  15.5, 0.0});
    m_map->setPaintProperty("stl-poly-border", "line-opacity",
        QVariantList{"interpolate", QVariantList{"linear"}, QVariantList{"zoom"},
            11.99, 0.0,  12.0, 1.0,  14.0, 1.0,  15.5, 0.0});
    m_map->setPaintProperty("stl-poly-border", "line-width",
        QVariantList{"interpolate", QVariantList{"linear"}, QVariantList{"zoom"},
            12, 1.0,  13, 1.5,  14, 2.0});
    m_map->setPaintProperty("stl-circle", "circle-opacity",
        QVariantList{"interpolate", QVariantList{"linear"}, QVariantList{"zoom"},
            11.99, 0.0,  12.0, 1.0,  14.0, 1.0,  15.5, 0.0});
    m_map->setPaintProperty("stl-circle", "circle-stroke-opacity",
        QVariantList{"interpolate", QVariantList{"linear"}, QVariantList{"zoom"},
            11.99, 0.0,  12.0, 1.0,  14.0, 1.0,  15.5, 0.0});

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

    // --- Conflict points (warning icon) ---
    addSymbol("conflict-pt", "conflict-pts");
    m_map->setLayoutProperty("conflict-pt", "icon-image", "warning-marker");
    m_map->setLayoutProperty("conflict-pt", "icon-size", 1.0);
    m_map->setLayoutProperty("conflict-pt", "icon-allow-overlap", true);

    // --- Waypoints (numbered icon — no text-field, avoids glyphs dependency) ---
    addSymbol("wp-icon", "waypoints");
    // Data-driven icon: "wp-1", "wp-2", … based on feature property "icon"
    m_map->setLayoutProperty("wp-icon", "icon-image", QVariantList{"get", "icon"});
    m_map->setLayoutProperty("wp-icon", "icon-size", 1.0);
    m_map->setLayoutProperty("wp-icon", "icon-allow-overlap", true);

    // --- Drawing (yellow line + vertices, matching Python) ---
    addLine("draw-line", "drawing", "#FFFF00", 2.0);
    addCircle("draw-vertex", "draw-vertices", "#FFFFFF", 5.0, "#000000", 1.0);
    // First vertex is bigger and yellow (data-driven by index property)
    m_map->setPaintProperty("draw-vertex", "circle-radius",
        QVariantList{"case", QVariantList{"==", QVariantList{"get", "index"}, 0}, 8.0, 5.0});
    m_map->setPaintProperty("draw-vertex", "circle-color",
        QVariantList{"case", QVariantList{"==", QVariantList{"get", "index"}, 0},
                     QString("#FFFF00"), QString("#FFFFFF")});

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

    // Home marker (red house icon)
    QImage homeImg(28, 28, QImage::Format_ARGB32_Premultiplied);
    homeImg.fill(Qt::transparent);
    QPainter hp(&homeImg);
    hp.setRenderHint(QPainter::Antialiasing);

    // Shadow
    hp.setBrush(QColor(0, 0, 0, 50));
    hp.setPen(Qt::NoPen);
    QPolygonF roofShadow;
    roofShadow << QPointF(14.5, 3.5) << QPointF(25.5, 14.5) << QPointF(3.5, 14.5);
    hp.drawPolygon(roofShadow);
    hp.drawRect(QRectF(6.5, 14.5, 16, 11));

    // House body
    hp.setBrush(QColor("#D32F2F"));
    hp.setPen(QPen(Qt::white, 1.5));
    hp.drawRect(QRectF(6, 14, 16, 11));

    // Roof
    QPolygonF roof;
    roof << QPointF(14, 3) << QPointF(25, 14) << QPointF(3, 14);
    hp.setBrush(QColor("#B71C1C"));
    hp.drawPolygon(roof);

    // Door
    hp.setBrush(Qt::white);
    hp.setPen(Qt::NoPen);
    hp.drawRect(QRectF(11.5, 18, 5, 7));

    hp.end();

    m_map->addImage("home-marker", homeImg);

    // Warning marker (yellow triangle with !)
    QImage warnImg(28, 28, QImage::Format_ARGB32_Premultiplied);
    warnImg.fill(Qt::transparent);
    QPainter wp2(&warnImg);
    wp2.setRenderHint(QPainter::Antialiasing);

    // Shadow
    wp2.setBrush(QColor(0, 0, 0, 50));
    wp2.setPen(Qt::NoPen);
    QPolygonF triShadow;
    triShadow << QPointF(14.5, 2.5) << QPointF(26.5, 25.5) << QPointF(2.5, 25.5);
    wp2.drawPolygon(triShadow);

    // Yellow triangle
    QPolygonF tri;
    tri << QPointF(14, 2) << QPointF(26, 25) << QPointF(2, 25);
    wp2.setBrush(QColor("#FFAB00"));
    wp2.setPen(QPen(QColor("#E65100"), 1.5));
    wp2.drawPolygon(tri);

    // "!" text
    wp2.setPen(QColor("#E65100"));
    QFont wf("Arial", 14, QFont::ExtraBold);
    wp2.setFont(wf);
    wp2.drawText(QRectF(0, 4, 28, 24), Qt::AlignCenter, "!");
    wp2.end();

    m_map->addImage("warning-marker", warnImg);
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
    connect(m_backend, &MapBackend::homePositionChanged, this, &MapLibreAdapter::updateWaypointSources);
    connect(m_backend, &MapBackend::returningHomeChanged, this, &MapLibreAdapter::updateAircraftSource);

    // Avoidance / direct path
    connect(m_backend, &MapBackend::avoidancePathChanged, this, &MapLibreAdapter::updateAvoidanceSource);
    connect(m_backend, &MapBackend::plannedDirectPathChanged, this, &MapLibreAdapter::updateDirectPathSource);

    // Drawing
    connect(m_backend, &MapBackend::drawingPathChanged, this, &MapLibreAdapter::updateDrawingSource);

    // Zones (direct signal — bypasses model signal chain for reliability)
    connect(m_backend, &MapBackend::zonesChanged, this, &MapLibreAdapter::updateZoneSources);

    // Viewport culling — recompute render area on camera change
    connect(m_backend, &MapBackend::boundsChanged,
            this, &MapLibreAdapter::onViewportChanged);

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
    // Settlement models → trigger viewport culling (reads from unlimited cache, not model)
    auto triggerSettlementCull = [this](auto&&...) { m_cullDebounce->start(); };
    auto* stlPolyModel = qobject_cast<QAbstractItemModel*>(m_backend->settlementPolyModel());
    auto* stlCircleModel = qobject_cast<QAbstractItemModel*>(m_backend->settlementCircleModel());
    connect(stlPolyModel, &QAbstractItemModel::rowsInserted, this, triggerSettlementCull);
    connect(stlPolyModel, &QAbstractItemModel::modelReset, this, triggerSettlementCull);
    connect(stlCircleModel, &QAbstractItemModel::rowsInserted, this, triggerSettlementCull);
    connect(stlCircleModel, &QAbstractItemModel::modelReset, this, triggerSettlementCull);
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

    // Nav-line: aircraft → target (active WP or home when returning)
    QJsonObject navFc = emptyFeatureCollection();
    if (acVis) {
        bool navLineSet = false;

        // Priority: if returning home → nav-line to home
        if (m_backend->returningHome() && m_backend->homeVisible()) {
            auto home = m_backend->homePosition();
            QJsonArray coords;
            coords.append(coord(acLat, acLon));
            coords.append(coord(home.latitude(), home.longitude()));
            QJsonArray features;
            features.append(makeLineFeature(coords));
            navFc["features"] = features;
            navLineSet = true;
        }

        // Otherwise: nav-line to active waypoint
        if (!navLineSet) {
            auto* wpModel = qobject_cast<WaypointListModel*>(m_backend->waypointModel());
            if (wpModel && !wpModel->items().isEmpty()) {
                const auto& items = wpModel->items();
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
    {
        QJsonArray features;
        if (routeModel) {
            const auto& items = routeModel->items();
            for (const auto& seg : items) {
                QJsonArray coords;
                coords.append(coord(seg.fromLat, seg.fromLon));
                coords.append(coord(seg.toLat, seg.toLon));
                QJsonObject props;
                props["state"] = seg.state;
                features.append(makeLineFeature(coords, props));
            }
        }

        // Last WP → home segment
        if (m_backend->homeVisible() && wpModel && !wpModel->items().isEmpty()) {
            const auto& lastWp = wpModel->items().back();
            auto home = m_backend->homePosition();
            QJsonArray coords;
            coords.append(coord(lastWp.lat, lastWp.lon));
            coords.append(coord(home.latitude(), home.longitude()));
            QJsonObject props;
            props["state"] = QStringLiteral("home");
            features.append(makeLineFeature(coords, props));
        }

        routeFc["features"] = features;
    }
    setSourceGeoJson("route", routeFc);
}

void MapLibreAdapter::updateZoneSources()
{
    // Skip GeoJSON rebuild when zoom is outside visibility range (opacity=0)
    int zoom = m_backend->currentZoom();
    if (zoom < 11 || zoom > 16) {
        setSourceGeoJson("zones", emptyFeatureCollection());
        return;
    }

    auto* model = qobject_cast<ZoneListModel*>(m_backend->zoneModel());
    QJsonObject fc = emptyFeatureCollection();
    QJsonArray features;

    int totalZones = 0, visibleZones = 0;
    if (model) {
        const auto& items = model->items();
        totalZones = static_cast<int>(items.size());
        for (const auto& zone : items) {
            QJsonArray ring;
            double minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
            for (const auto& pt : zone.points) {
                auto list = pt.toList();
                if (list.size() >= 2) {
                    double lat = list[0].toDouble();
                    double lon = list[1].toDouble();
                    ring.append(coord(lat, lon));
                    minLat = std::min(minLat, lat); maxLat = std::max(maxLat, lat);
                    minLon = std::min(minLon, lon); maxLon = std::max(maxLon, lon);
                }
            }
            // Close the ring
            if (ring.size() >= 3) {
                // Viewport culling: skip zones outside render area
                if (!bboxIntersectsRenderArea(minLat, minLon, maxLat, maxLon))
                    continue;

                ring.append(ring[0]);
                QJsonObject props;
                props["id"] = zone.id;
                props["name"] = zone.name;

                // Render zone name as icon image (symbol layer reads from same source)
                if (!zone.name.isEmpty()) {
                    QString imageId = QStringLiteral("zone-name-%1").arg(zone.id);
                    QFont font("Arial", 9, QFont::Bold);
                    QFontMetrics fm(font);
                    int padding = 4;
                    int w = fm.horizontalAdvance(zone.name) + padding * 2;
                    int h = fm.height() + padding * 2;
                    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
                    img.fill(Qt::transparent);
                    QPainter p(&img);
                    p.setRenderHint(QPainter::Antialiasing);
                    p.setBrush(QColor(0, 0, 0, 160));
                    p.setPen(Qt::NoPen);
                    p.drawRoundedRect(0, 0, w, h, 4, 4);
                    p.setPen(Qt::white);
                    p.setFont(font);
                    p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, zone.name);
                    p.end();
                    m_map->addImage(imageId, img);
                    props["icon"] = imageId;
                }

                features.append(makePolygonFeature(ring, props));
                ++visibleZones;
            }
        }
    }

    fc["features"] = features;
    SPDLOG_INFO("[MapLibreAdapter] viewport culling: {}/{} zones visible", visibleZones, totalZones);

    setSourceGeoJson("zones", fc);
}

void MapLibreAdapter::updateSettlementSources()
{
    // Skip GeoJSON rebuild when zoom is outside visibility range (opacity=0)
    int zoom = m_backend->currentZoom();
    if (zoom < 11 || zoom > 16) {
        setSourceGeoJson("stl-polys", emptyFeatureCollection());
        setSourceGeoJson("stl-circles", emptyFeatureCollection());
        return;
    }

    // Read from unlimited cache (survives LRU eviction in SettlementPolyModel)
    const auto& cache = m_backend->settlementPolyCache();
    int totalPolys = cache.size(), visiblePolys = 0;

    QJsonObject polyFc = emptyFeatureCollection();
    QJsonArray features;
    for (const auto& item : cache) {
        if (!bboxIntersectsRenderArea(item.minLat, item.minLon, item.maxLat, item.maxLon))
            continue;
        QJsonArray ring;
        for (const auto& pt : item.coords) {
            auto list = pt.toList();
            if (list.size() >= 2)
                ring.append(coord(list[0].toDouble(), list[1].toDouble()));
        }
        if (ring.size() >= 3) {
            ring.append(ring[0]);
            features.append(makePolygonFeature(ring));
            ++visiblePolys;
        }
    }
    polyFc["features"] = features;
    setSourceGeoJson("stl-polys", polyFc);

    // stl-circles: empty (nodes are converted to polygons in addSettlementFeatures)
    setSourceGeoJson("stl-circles", emptyFeatureCollection());

    SPDLOG_INFO("[MapLibreAdapter] viewport culling: {}/{} stl-polys visible",
                 visiblePolys, totalPolys);
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
//  Viewport culling
// ═══════════════════════════════════════════════════════════

bool MapLibreAdapter::bboxIntersectsRenderArea(double minLat, double minLon,
                                                double maxLat, double maxLon) const
{
    if (!m_hasRenderArea) return true;  // before first bounds — show everything
    return !(maxLat < m_renderSouth || minLat > m_renderNorth ||
             maxLon < m_renderWest  || minLon > m_renderEast);
}

void MapLibreAdapter::onViewportChanged(double south, double west, double north, double east)
{
    double dLat = (north - south) * RENDER_AREA_MARGIN;
    double dLon = (east - west) * RENDER_AREA_MARGIN;
    m_renderSouth = south + dLat;
    m_renderWest  = west + dLon;
    m_renderNorth = north - dLat;
    m_renderEast  = east - dLon;
    m_hasRenderArea = true;
    m_cullDebounce->start();
}

void MapLibreAdapter::applyViewportCulling()
{
    updateZoneSources();
    updateSettlementSources();
    m_backend->setRenderArea(m_renderSouth, m_renderWest, m_renderNorth, m_renderEast);
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
