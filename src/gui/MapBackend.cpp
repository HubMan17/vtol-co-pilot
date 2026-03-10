#include "MapBackend.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

namespace vtol {

MapBackend::MapBackend(QObject* parent)
    : QObject(parent)
{
}

// ═══════════════════════ Aircraft ═══════════════════════

void MapBackend::updateAircraft(double lat, double lon, double heading)
{
    QGeoCoordinate pos(lat, lon);
    bool changed = false;

    if (m_aircraftPos != pos) {
        m_aircraftPos = pos;
        emit aircraftPositionChanged();
        changed = true;

        // Track: distance-based throttle + incremental append
        double dlat = lat - m_lastTrackLat;
        double dlon = lon - m_lastTrackLon;
        if (dlat * dlat + dlon * dlon > 2e-10) {  // ~1.5m
            m_lastTrackLat = lat;
            m_lastTrackLon = lon;
            m_trackPoints.append(QPointF(lat, lon));
            m_trackPath.append(QVariant::fromValue(QGeoCoordinate(lat, lon)));

            if (m_trackPoints.size() > m_trackMaxLength) {
                int trim = std::max(m_trackMaxLength / 10, 50);
                m_trackPoints.remove(0, trim);
                m_trackPath = m_trackPath.mid(trim);
                emit trackPathChanged();
            } else {
                emit trackCoordinateAdded(lat, lon);
            }
        }
    }

    if (m_aircraftHeading != heading) {
        m_aircraftHeading = heading;
        emit aircraftHeadingChanged();
        changed = true;
    }

    if (!m_aircraftVisible) {
        m_aircraftVisible = true;
        emit aircraftVisibleChanged();
    }
}

void MapBackend::setAircraftPosition(double lat, double lon)
{
    m_aircraftPos = QGeoCoordinate(lat, lon);
    emit aircraftPositionChanged();
    if (!m_aircraftVisible) {
        m_aircraftVisible = true;
        emit aircraftVisibleChanged();
    }
}

void MapBackend::setTrackMaxLength(int length)
{
    m_trackMaxLength = std::max(100, length);
}

void MapBackend::clearTrack()
{
    m_trackPoints.clear();
    m_trackPath.clear();
    m_lastTrackLat = 0.0;
    m_lastTrackLon = 0.0;
    emit trackPathChanged();
}

// ═══════════════════════ Home ═══════════════════════

void MapBackend::setHome(double lat, double lon)
{
    m_homePos = QGeoCoordinate(lat, lon);
    m_homeVisible = true;
    emit homePositionChanged();
    emit homeVisibleChanged();
}

void MapBackend::setReturningHome(bool v)
{
    if (m_returningHome != v) {
        m_returningHome = v;
        SPDLOG_INFO("[MapBackend] returningHome = {}", v);
        emit returningHomeChanged();
    }
}

// ═══════════════════════ Mesh Points ═══════════════════════

void MapBackend::addMeshPoint(double lat, double lon, int durationMs)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_meshPoints.append({lat, lon, now + durationMs});
    emit meshPointsChanged();

    if (!m_meshCleanupTimer) {
        m_meshCleanupTimer = new QTimer(this);
        m_meshCleanupTimer->setInterval(500);
        connect(m_meshCleanupTimer, &QTimer::timeout, this, &MapBackend::cleanupMeshPoints);
    }
    if (!m_meshCleanupTimer->isActive())
        m_meshCleanupTimer->start();
}

void MapBackend::cleanupMeshPoints()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    int before = m_meshPoints.size();
    m_meshPoints.erase(
        std::remove_if(m_meshPoints.begin(), m_meshPoints.end(),
                        [now](const MeshPoint& p) { return now >= p.expireMs; }),
        m_meshPoints.end());

    if (m_meshPoints.size() != before)
        emit meshPointsChanged();

    if (m_meshPoints.isEmpty())
        m_meshCleanupTimer->stop();
}

// ═══════════════════════ Operational Waypoint ═══════════════════════

void MapBackend::setOperationalWp(double lat, double lon, const QString& mode)
{
    m_operationalWpPos = QGeoCoordinate(lat, lon);
    m_operationalWpVisible = true;
    m_operationalWpMode = mode;
    SPDLOG_INFO("[MapBackend::setOperationalWp] lat={:.6f}, lon={:.6f}, mode={}", lat, lon, mode.toStdString());
    emit operationalWpChanged();
}

void MapBackend::clearOperationalWp()
{
    if (!m_operationalWpVisible) return;
    m_operationalWpVisible = false;
    m_operationalWpMode.clear();
    SPDLOG_INFO("[MapBackend::clearOperationalWp]");
    emit operationalWpChanged();
}

// ═══════════════════════ Waypoints ═══════════════════════

MapBackend::WpInfo MapBackend::waypointAt(int index) const
{
    if (index < 0 || index >= m_currentWaypoints.size()) return {};
    const auto& m = m_currentWaypoints[index];
    WpInfo info;
    info.lat        = m.value("lat").toDouble();
    info.lon        = m.value("lon").toDouble();
    info.altitude   = m.value("altitude").toDouble();
    info.action     = m.value("action").toString();
    info.orbitTurns = m.value("orbitTurns", 1).toInt();
    info.valid      = true;
    return info;
}

void MapBackend::setWaypoints(const QVector<QVariantMap>& waypoints, int activeIdx)
{
    m_currentWaypoints = waypoints;
    m_waypointModel.setWaypoints(waypoints, activeIdx);
    m_routeSegmentModel.setSegments(waypoints, activeIdx);
    if (!waypoints.isEmpty() && activeIdx >= 0 && activeIdx < waypoints.size()) {
        const auto& wp = waypoints[activeIdx];
        m_activeWpPos = QGeoCoordinate(wp["lat"].toDouble(), wp["lon"].toDouble());
    } else {
        m_activeWpPos = QGeoCoordinate();
    }
    emit activeWpPositionChanged();
    emit waypointsChanged();
}

void MapBackend::previewWaypointPosition(int index, double lat, double lon)
{
    if (index < 0 || index >= m_currentWaypoints.size()) return;
    m_currentWaypoints[index]["lat"] = lat;
    m_currentWaypoints[index]["lon"] = lon;
    // Update model in-place so MapLibreAdapter rebuilds GeoJSON correctly
    m_waypointModel.updateItemPosition(index, lat, lon);
    emit waypointsChanged();
}

void MapBackend::updateActiveWaypoint(int index)
{
    m_waypointModel.updateActive(index);
    if (!m_currentWaypoints.isEmpty() && index >= 0 && index < m_currentWaypoints.size()) {
        const auto& wp = m_currentWaypoints[index];
        m_activeWpPos = QGeoCoordinate(wp["lat"].toDouble(), wp["lon"].toDouble());
    } else {
        m_activeWpPos = QGeoCoordinate();
    }
    emit activeWpPositionChanged();
}

// ═══════════════════════ Settings ═══════════════════════

void MapBackend::setFollowMode(bool enabled) { setFollowAircraft(enabled); }
void MapBackend::setZoom(int level) { emit zoomRequested(level); }
void MapBackend::centerOn(double lat, double lon) { emit mapCenterRequested(lat, lon); }
void MapBackend::setTileServer(const QString& url) {
    if (m_tileServerUrl != url) { m_tileServerUrl = url; emit tileServerUrlChanged(); }
}

void MapBackend::setFollowAircraft(bool v) {
    if (m_followAircraft != v) { m_followAircraft = v; emit followAircraftChanged(); }
}
void MapBackend::setShowTrack(bool v) {
    if (m_showTrack != v) { m_showTrack = v; emit showTrackChanged(); }
}
void MapBackend::setShowWaypoints(bool v) {
    if (m_showWaypoints != v) { m_showWaypoints = v; emit showWaypointsChanged(); }
}
void MapBackend::setShowZones(bool v) {
    if (m_showZones != v) { m_showZones = v; emit showZonesChanged(); }
}
void MapBackend::setShowSettlements(bool v) {
    if (m_showSettlements != v) {
        m_showSettlements = v;
        emit showSettlementsChanged();
        if (!v) {
            // Start delayed unload timer (30 sec)
            if (!m_settlementUnloadTimer) {
                m_settlementUnloadTimer = new QTimer(this);
                m_settlementUnloadTimer->setSingleShot(true);
                connect(m_settlementUnloadTimer, &QTimer::timeout, this, [this] {
                    if (!m_showSettlements) {
                        clearSettlements();
                        SPDLOG_INFO("[MapBackend] Settlements unloaded (hidden for 30s)");
                    }
                });
            }
            m_settlementUnloadTimer->start(30000);
        } else {
            // Cancel unload if re-enabled
            if (m_settlementUnloadTimer)
                m_settlementUnloadTimer->stop();
        }
    }
}
void MapBackend::setLeftClickMode(bool v) {
    if (m_leftClickMode != v) { m_leftClickMode = v; emit leftClickModeChanged(); }
}

// ═══════════════════════ FPS ═══════════════════════

void MapBackend::setFpsText(const QString& text) {
    if (m_fpsText != text) { m_fpsText = text; emit fpsTextChanged(); }
}

void MapBackend::tickFps(double elapsedMs)
{
    m_frameCount++;
    m_fpsUpdateMs = elapsedMs;
    if (elapsedMs > 5.0) m_fpsDrops++;  // count ticks >5ms as heavy

    using clock = std::chrono::steady_clock;
    static auto epoch = clock::now();
    double now = std::chrono::duration<double>(clock::now() - epoch).count();

    if (m_fpsTimestamp == 0.0) { m_fpsTimestamp = now; return; }
    double dt = now - m_fpsTimestamp;
    if (dt >= 1.0) {
        int stl = m_settlementPolyModel.items().size() + m_settlementCircleModel.itemCount();
        int wps = m_waypointModel.rowCount();
        // upd=loop time, drops=ticks >5ms, stl=settlement items, wps=waypoints
        QString text = QString("upd:%1ms drp:%2 stl:%3 wps:%4")
                           .arg(m_fpsUpdateMs, 0, 'f', 1)
                           .arg(m_fpsDrops)
                           .arg(stl)
                           .arg(wps);
        SPDLOG_DEBUG("PERF tickFps: {}", text.toStdString());
        m_frameCount = 0;
        m_fpsDrops = 0;
        m_fpsTimestamp = now;
        setFpsText(text);
    }
}

// ═══════════════════════ Zones ═══════════════════════

void MapBackend::addZone(const QString& id, const QVariantList& points, const QString& name)
{
    SPDLOG_INFO("[MapBackend] addZone id={} name={} pts={}", id.toStdString(), name.toStdString(), points.size());
    m_zoneModel.addZone(id, points, name);
    emit zonesChanged();
}

void MapBackend::removeZone(const QString& id)
{
    SPDLOG_INFO("[MapBackend] removeZone id={}, model size before={}", id.toStdString(), m_zoneModel.rowCount());
    m_zoneModel.removeZone(id);
    SPDLOG_INFO("[MapBackend] removeZone model size after={}", m_zoneModel.rowCount());

    // Remove from zone details cache (used by rebuildObstacleWarnings)
    for (int i = 0; i < m_zoneDetails.size(); ++i) {
        if (m_zoneDetails[i].toMap().value("id").toString() == id) {
            m_zoneDetails.removeAt(i);
            break;
        }
    }
    rebuildObstacleWarnings();

    emit zonesChanged();
}

void MapBackend::loadAllZones(const QVariantList& zones)
{
    m_zoneModel.clear();
    for (const auto& z : zones) {
        auto map = z.toMap();
        m_zoneModel.addZone(map["id"].toString(), map["points"].toList(), map.value("name", "").toString());
    }
    m_zoneDetails = zones;
    rebuildObstacleWarnings();
    SPDLOG_INFO("[MapBackend] loadAllZones: {} zones loaded", zones.size());
    emit zonesChanged();
}

// ═══════════════════════ Settlements ═══════════════════════

void MapBackend::addSettlementFeatures(const QVariantList& features)
{
    QVariantList polys;
    for (const auto& f : features) {
        auto map = f.toMap();
        if (map["F"].toString() == "p") {
            polys.append(f);
        } else if (map["F"].toString() == "n") {
            // Convert node settlement to circle polygon
            double lat = map["lat"].toDouble();
            double lon = map["lon"].toDouble();
            QString type = map.value("t", "").toString();
            double radius = 500.0;
            if (type == "city") radius = 5000.0;
            else if (type == "town") radius = 2000.0;
            else if (type == "village") radius = 800.0;
            else if (type == "hamlet") radius = 400.0;

            constexpr int N = 16;
            constexpr double DEG_TO_M = 111320.0;
            double cosLat = std::cos(lat * M_PI / 180.0);
            QVariantList coords;
            for (int i = 0; i < N; ++i) {
                double angle = 2.0 * M_PI * i / N;
                double dLat = radius * std::cos(angle) / DEG_TO_M;
                double dLon = radius * std::sin(angle) / (DEG_TO_M * cosLat);
                coords.append(QVariant(QVariantList{lat + dLat, lon + dLon}));
            }
            QVariantMap polyFeat;
            polyFeat["F"] = "p";
            polyFeat["c"] = coords;
            polyFeat["n"] = map.value("n", "");
            polyFeat["t"] = type;
            polys.append(polyFeat);
        }
    }
    if (!polys.isEmpty()) {
        m_settlementPolyModel.addPolys(polys);
    }

    // Generate obstacle warnings for settlements
    {
        static const QHash<QString, QString> typeText = {
            {"city", QString::fromUtf8("город")},
            {"town", QString::fromUtf8("посёлок")},
            {"village", QString::fromUtf8("село")},
            {"hamlet", QString::fromUtf8("деревня")},
        };
        QVector<ObstacleWarningModel::Item> warnings;
        for (const auto& f : polys) {
            auto map = f.toMap();
            auto coords = map["c"].toList();
            if (coords.size() < 3) continue;
            auto [lat, lon] = bboxCenter(coords);
            QString placeType = map.value("t", "village").toString();
            QString tt = typeText.value(placeType, placeType);
            warnings.append({lat, lon,
                QString::fromUtf8("Населённый пункт (%1)").arg(tt),
                QStringLiteral("settlement")});
        }
        if (!warnings.isEmpty()) {
            SPDLOG_INFO("[MapBackend] Adding {} settlement warnings (total model: {})",
                        warnings.size(), m_obstacleWarningModel.items().size() + warnings.size());
            m_obstacleWarningModel.appendItems(warnings);
        }
    }

    // Add to unlimited cache (for MapLibreAdapter rendering — survives LRU eviction)
    for (const auto& f : polys) {
        auto map = f.toMap();
        auto coords = map["c"].toList();
        if (coords.size() < 3) continue;

        auto first = coords[0].toList();
        if (first.size() < 2) continue;
        QString sig = QString("p_%1_%2_%3")
            .arg(first[0].toDouble(), 0, 'f', 4)
            .arg(first[1].toDouble(), 0, 'f', 4)
            .arg(coords.size());

        if (m_stlPolyCacheSigs.contains(sig)) continue;
        m_stlPolyCacheSigs.insert(sig);

        // Pre-compute bbox
        double minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
        for (const auto& pt : coords) {
            auto list = pt.toList();
            if (list.size() >= 2) {
                double lat = list[0].toDouble(), lon = list[1].toDouble();
                minLat = std::min(minLat, lat); maxLat = std::max(maxLat, lat);
                minLon = std::min(minLon, lon); maxLon = std::max(maxLon, lon);
            }
        }

        m_stlPolyCache.append({coords, minLat, minLon, maxLat, maxLon});
    }
}

void MapBackend::clearSettlements()
{
    m_settlementPolyModel.clear();
    m_settlementCircleModel.clear();
    m_stlPolyCache.clear();
    m_stlPolyCacheSigs.clear();
}

void MapBackend::setRenderArea(double s, double w, double n, double e)
{
    emit renderAreaChanged(s, w, n, e);
}

// ═══════════════════════ Conflicts / Avoidance ═══════════════════════

void MapBackend::setRouteConflicts(const QVariantList& conflicts)
{
    m_conflictModel.setConflicts(conflicts, m_currentWaypoints);
    m_conflictPointModel.setConflicts(conflicts, m_currentWaypoints);
}

void MapBackend::clearRouteConflicts()
{
    m_conflictModel.clear();
    m_conflictPointModel.clear();
    setPlannedDirectPath({});
    setAvoidancePath({});
}

void MapBackend::setAvoidancePath(const QVariantList& points)
{
    m_avoidancePath.clear();
    for (const auto& p : points) {
        auto map = p.toMap();
        m_avoidancePath.append(QVariant::fromValue(
            QGeoCoordinate(map["lat"].toDouble(), map["lon"].toDouble())));
    }
    emit avoidancePathChanged();
}

void MapBackend::setPlannedDirectPath(const QVariantList& points)
{
    m_plannedDirectPath.clear();
    for (const auto& p : points) {
        auto map = p.toMap();
        m_plannedDirectPath.append(QVariant::fromValue(
            QGeoCoordinate(map["lat"].toDouble(), map["lon"].toDouble())));
    }
    emit plannedDirectPathChanged();
}

void MapBackend::setConflictPoints(const QVariantList& points)
{
    m_conflictPointModel.setPoints(points);
}

// ═══════════════════════ Layer visibility ═══════════════════════

void MapBackend::setLayerVisibility(const QString& layerName, bool visible)
{
    if (layerName == "track") setShowTrack(visible);
    else if (layerName == "waypoints") setShowWaypoints(visible);
    else if (layerName == "restricted") setShowZones(visible);
    else if (layerName == "settlements") setShowSettlements(visible);
}

// ═══════════════════════ Drawing mode ═══════════════════════

void MapBackend::startDrawing()
{
    m_drawingMode = true;
    m_drawingPath.clear();
    m_drawingVertexModel.clear();
    emit drawingModeChanged();
    emit drawingPathChanged();
}

void MapBackend::cancelDrawing()
{
    m_drawingMode = false;
    m_drawingPath.clear();
    m_drawingVertexModel.clear();
    emit drawingModeChanged();
    emit drawingPathChanged();
}

QVariantList MapBackend::finishDrawing()
{
    auto pts = m_drawingVertexModel.getPoints();
    SPDLOG_INFO("[MapBackend] finishDrawing: {} raw points from model", pts.size());
    m_drawingMode = false;
    m_drawingPath.clear();
    m_drawingVertexModel.clear();
    emit drawingModeChanged();
    emit drawingPathChanged();

    QVariantList result;
    for (const auto& p : pts) {
        result.append(QVariant::fromValue(QVariantList{p.x(), p.y()}));
        SPDLOG_INFO("[MapBackend]   point: x(lat)={}, y(lon)={}", p.x(), p.y());
    }
    SPDLOG_INFO("[MapBackend] finishDrawing: returning {} QVariantList items", result.size());
    return result;
}

void MapBackend::addDrawingVertex(double lat, double lon)
{
    auto pts = m_drawingVertexModel.getPoints();
    // Snap to first point to close polygon (zoom-dependent threshold ~20px)
    if (pts.size() >= 3) {
        // ~20 pixels in degrees: 20 * 360 / (256 * 2^zoom)
        double snapDeg = 7200.0 / (256.0 * std::pow(2.0, m_currentZoom));
        snapDeg = std::clamp(snapDeg, 0.00005, 0.05);  // 5m..5km bounds
        double d = std::sqrt(std::pow(lat - pts[0].x(), 2) + std::pow(lon - pts[0].y(), 2));
        if (d < snapDeg) {
            SPDLOG_INFO("[MapBackend] SNAP: closing polygon (d={} < snap={})", d, snapDeg);
            auto result = finishDrawing();
            QJsonArray arr;
            for (const auto& p : result) {
                auto list = p.toList();
                SPDLOG_INFO("[MapBackend] snap: p.toList() size={}", list.size());
                if (list.size() >= 2) {
                    QJsonArray pt;
                    pt.append(list[0].toDouble());
                    pt.append(list[1].toDouble());
                    arr.append(pt);
                }
            }
            // Defer signal to avoid crash: finishDrawing() triggers MapLibre source
            // updates synchronously, and the modal dialog in MainWindow would block
            // the event loop mid-chain causing re-entrant issues
            QString json = QJsonDocument(arr).toJson(QJsonDocument::Compact);
            SPDLOG_INFO("[MapBackend] snap: emitting drawingFinished json='{}' (len={})",
                         json.toStdString(), json.size());
            QTimer::singleShot(0, this, [this, json]() {
                emit drawingFinished(json);
            });
            return;
        }
    }

    m_drawingVertexModel.addPoint(lat, lon);
    pts = m_drawingVertexModel.getPoints();
    m_drawingPath.clear();
    for (const auto& p : pts)
        m_drawingPath.append(QVariant::fromValue(QGeoCoordinate(p.x(), p.y())));
    emit drawingPathChanged();
}

// ═══════════════════════ Editing mode ═══════════════════════

void MapBackend::enableZoneEditing(const QString& zoneId)
{
    m_editingZoneId = zoneId;
    for (const auto& item : m_zoneModel.items()) {
        if (item.id == zoneId) {
            QVector<QPointF> pts;
            for (const auto& pt : item.points) {
                auto list = pt.toList();
                if (list.size() >= 2)
                    pts.append(QPointF(list[0].toDouble(), list[1].toDouble()));
            }
            m_editingVertexModel.setPoints(pts);
            rebuildMidpoints(pts);
            return;
        }
    }
}

void MapBackend::disableZoneEditing()
{
    m_editingZoneId.clear();
    m_editingVertexModel.clear();
    m_editingMidpointModel.clear();
}

void MapBackend::rebuildMidpoints(const QVector<QPointF>& pts)
{
    QVector<QPointF> mids;
    for (int i = 0; i < pts.size(); ++i) {
        int j = (i + 1) % pts.size();
        mids.append(QPointF(
            (pts[i].x() + pts[j].x()) / 2,
            (pts[i].y() + pts[j].y()) / 2));
    }
    m_editingMidpointModel.setPoints(mids);
}

// ═══════════════════════ QML Slots ═══════════════════════

void MapBackend::onMapClick(double lat, double lon)
{
    if (m_drawingMode)
        addDrawingVertex(lat, lon);
    else
        emit mapClicked(lat, lon);
}

void MapBackend::onMapDoubleClick(double lat, double lon)
{
    if (m_drawingMode) {
        SPDLOG_INFO("[MapBackend] DOUBLE-CLICK finish drawing");
        auto result = finishDrawing();
        if (result.size() >= 3) {
            QJsonArray arr;
            for (const auto& p : result) {
                auto list = p.toList();
                SPDLOG_INFO("[MapBackend] dblclick: p.toList() size={}", list.size());
                if (list.size() >= 2) {
                    QJsonArray pt;
                    pt.append(list[0].toDouble());
                    pt.append(list[1].toDouble());
                    arr.append(pt);
                }
            }
            // Defer to avoid modal dialog mid-signal-chain crash
            QString json = QJsonDocument(arr).toJson(QJsonDocument::Compact);
            SPDLOG_INFO("[MapBackend] dblclick: emitting drawingFinished json='{}' (len={})",
                         json.toStdString(), json.size());
            QTimer::singleShot(0, this, [this, json]() {
                emit drawingFinished(json);
            });
        } else {
            emit drawingCancelled();
        }
    } else {
        QString zoneId = hitTestZone(lat, lon);
        if (!zoneId.isEmpty())
            emit zoneDoubleClicked(zoneId);
        else
            emit mapDoubleClicked(lat, lon);
    }
}

void MapBackend::onContextMenu(double lat, double lon, int sx, int sy)
{
    if (m_drawingMode) return;
    QString zoneId = hitTestZone(lat, lon);
    if (!zoneId.isEmpty())
        emit zoneContextMenuRequested(zoneId, sx, sy);
    else
        emit contextMenuRequested(lat, lon, sx, sy);
}

void MapBackend::onBoundsChanged(double south, double west, double north, double east)
{
    if (std::isnan(south) || std::isnan(west) || std::isnan(north) || std::isnan(east)) return;
    m_viewSouth = south;
    m_viewWest = west;
    m_viewNorth = north;
    m_viewEast = east;
    emit boundsChanged(south, west, north, east);
}

void MapBackend::onMouseMove(double lat, double lon) { emit mouseMoved(lat, lon); }
void MapBackend::onZoomChanged(int zoom) { m_currentZoom = zoom; emit zoomChanged(zoom); }

void MapBackend::moveEditingVertex(int idx, double lat, double lon)
{
    m_editingVertexModel.updatePoint(idx, lat, lon);
    auto pts = m_editingVertexModel.getPoints();
    rebuildMidpoints(pts);
    if (!m_editingZoneId.isEmpty()) {
        QVariantList ptsList;
        for (const auto& p : pts) ptsList.append(QVariant::fromValue(QVariantList{p.x(), p.y()}));
        m_zoneModel.updateZonePoints(m_editingZoneId, ptsList);
        emit zoneVerticesUpdated(m_editingZoneId, ptsList);
    }
}

void MapBackend::deleteEditingVertex(int idx)
{
    if (m_editingVertexModel.getPoints().size() <= 3) return;
    m_editingVertexModel.removePoint(idx);
    auto pts = m_editingVertexModel.getPoints();
    rebuildMidpoints(pts);
    if (!m_editingZoneId.isEmpty()) {
        QVariantList ptsList;
        for (const auto& p : pts) ptsList.append(QVariant::fromValue(QVariantList{p.x(), p.y()}));
        m_zoneModel.updateZonePoints(m_editingZoneId, ptsList);
        emit zoneVerticesUpdated(m_editingZoneId, ptsList);
    }
}

void MapBackend::insertEditingVertex(int midIdx)
{
    auto mids = m_editingMidpointModel.getPoints();
    if (midIdx < 0 || midIdx >= mids.size()) return;
    double lat = mids[midIdx].x();
    double lon = mids[midIdx].y();
    auto pts = m_editingVertexModel.getPoints();
    pts.insert(midIdx + 1, QPointF(lat, lon));
    m_editingVertexModel.setPoints(pts);
    rebuildMidpoints(pts);
    if (!m_editingZoneId.isEmpty()) {
        QVariantList ptsList;
        for (const auto& p : pts) ptsList.append(QVariant::fromValue(QVariantList{p.x(), p.y()}));
        m_zoneModel.updateZonePoints(m_editingZoneId, ptsList);
        emit zoneVerticesUpdated(m_editingZoneId, ptsList);
    }
}

void MapBackend::cancelDrawingSlot()
{
    if (m_drawingMode) {
        cancelDrawing();
        emit drawingCancelled();
    }
}

// ═══════════════════════ Canvas data ═══════════════════════

QVariantList MapBackend::settlementBorderPolygons() const {
    QVariantList result;
    for (const auto& poly : m_settlementPolyModel.items())
        result.append(QVariant(poly));
    return result;
}

QVariantList MapBackend::zoneBorderPolygons() const {
    QVariantList result;
    for (const auto& item : m_zoneModel.items())
        result.append(QVariant(item.points));
    return result;
}

// ═══════════════════════ Obstacle warnings ═══════════════════════

std::pair<double, double> MapBackend::bboxCenter(const QVariantList& coords)
{
    double minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
    for (const auto& pt : coords) {
        auto pair = pt.toList();
        if (pair.size() >= 2) {
            double lat = pair[0].toDouble(), lon = pair[1].toDouble();
            minLat = std::min(minLat, lat); maxLat = std::max(maxLat, lat);
            minLon = std::min(minLon, lon); maxLon = std::max(maxLon, lon);
        }
    }
    return {(minLat + maxLat) / 2.0, (minLon + maxLon) / 2.0};
}

void MapBackend::rebuildObstacleWarnings()
{
    static const QHash<QString, QString> modeText = {
        {"always", QString::fromUtf8("Всегда")},
        {"below_altitude", QString::fromUtf8("Ниже высоты")},
        {"disabled", QString::fromUtf8("Отключено")},
    };

    QVector<ObstacleWarningModel::Item> zoneWarnings;
    for (const auto& z : m_zoneDetails) {
        auto map = z.toMap();
        auto pts = map["points"].toList();
        if (pts.size() < 3) continue;
        auto [lat, lon] = bboxCenter(pts);
        QString name = map.value("name", "").toString();
        if (name.isEmpty()) name = QString::fromUtf8("Без названия");
        QString mode = map.value("avoid_mode", "always").toString();
        QString mt = modeText.value(mode, mode);

        QStringList lines;
        lines << QString::fromUtf8("\u26A0 Запретная зона: %1").arg(name);
        lines << QString::fromUtf8("Режим: %1").arg(mt);
        if (map.contains("altitude") && map["altitude"].isValid()) {
            lines << QString::fromUtf8("Высота: до %1 м").arg(static_cast<int>(map["altitude"].toDouble()));
        }
        if (map.contains("buffer") && map["buffer"].isValid()) {
            lines << QString::fromUtf8("Буфер: %1 м").arg(static_cast<int>(map["buffer"].toDouble()));
        }
        zoneWarnings.append({lat, lon, lines.join('\n'), QStringLiteral("zone")});
        SPDLOG_INFO("[MapBackend] Zone warning '{}' at ({:.6f}, {:.6f}), pts={}",
                    name.toStdString(), lat, lon, pts.size());
    }

    // Preserve existing settlement warnings
    QVector<ObstacleWarningModel::Item> all = std::move(zoneWarnings);
    for (const auto& item : m_obstacleWarningModel.items()) {
        if (item.source == "settlement")
            all.append(item);
    }
    m_obstacleWarningModel.setItems(std::move(all));
}

// ═══════════════════════ Zone hit-testing ═══════════════════════

QString MapBackend::hitTestZone(double lat, double lon) const
{
    if (!m_showZones) return {};
    for (const auto& item : m_zoneModel.items()) {
        if (pointInPolygon(lat, lon, item.points))
            return item.id;
    }
    return {};
}

bool MapBackend::pointInPolygon(double lat, double lon, const QVariantList& polygon)
{
    int n = polygon.size();
    if (n < 3) return false;
    bool inside = false;
    int j = n - 1;
    for (int i = 0; i < n; ++i) {
        auto pi = polygon[i].toList();
        auto pj = polygon[j].toList();
        if (pi.size() < 2 || pj.size() < 2) { j = i; continue; }
        double lati = pi[0].toDouble(), loni = pi[1].toDouble();
        double latj = pj[0].toDouble(), lonj = pj[1].toDouble();
        if (((lati > lat) != (latj > lat)) &&
            (lon < (lonj - loni) * (lat - lati) / (latj - lati) + loni)) {
            inside = !inside;
        }
        j = i;
    }
    return inside;
}

} // namespace vtol
