#include "FlightMapCanvas.h"
#include "MapBackend.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QHoverEvent>
#include <QNetworkReply>
#include <QGeoCoordinate>
#include <cmath>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace vtol {

// ═══════════════════════════════════════════════════════════
//  Constructor
// ═══════════════════════════════════════════════════════════

FlightMapCanvas::FlightMapCanvas(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setOpaquePainting(true);
    setRenderTarget(QQuickPaintedItem::Image);  // raster engine — faster for 2D
    setAntialiasing(false);  // AA per-call where needed, not global

    // Repaint throttle ~60 FPS
    m_repaintTimer.setInterval(16);
    m_repaintTimer.setSingleShot(true);
    connect(&m_repaintTimer, &QTimer::timeout, this, [this] {
        m_repaintScheduled = false;
        updateVisibleTiles();
        update();
    });

    // Follow mode
    m_followTimer.setInterval(200);
    m_followTimer.setSingleShot(true);
    connect(&m_followTimer, &QTimer::timeout, this, [this] {
        if (!m_backend || !m_backend->followAircraft()) return;
        auto pos = m_backend->aircraftPosition();
        if (!pos.isValid()) return;
        m_centerLat = pos.latitude();
        m_centerLon = pos.longitude();
        m_tileLayerDirty = true;
        scheduleRepaint();
    });

    // Bounds debounce
    m_boundsTimer.setInterval(300);
    m_boundsTimer.setSingleShot(true);
    connect(&m_boundsTimer, &QTimer::timeout, this, &FlightMapCanvas::notifyBounds);

    // Click timer for single/double click disambiguation
    m_clickTimer.setInterval(200);
    m_clickTimer.setSingleShot(true);
    connect(&m_clickTimer, &QTimer::timeout, this, [this] {
        if (!m_clickPending || !m_backend) return;
        m_clickPending = false;
        double lat, lon;
        screenToGeo(m_clickPos.x(), m_clickPos.y(), lat, lon);
        m_backend->onMapClick(lat, lon);
    });
}

// ═══════════════════════════════════════════════════════════
//  Backend property
// ═══════════════════════════════════════════════════════════

QObject* FlightMapCanvas::backendObj() const { return m_backend; }

void FlightMapCanvas::setBackendObj(QObject* obj)
{
    auto* b = qobject_cast<MapBackend*>(obj);
    if (m_backend == b) return;
    m_backend = b;
    if (m_backend) {
        SPDLOG_INFO("[FlightMapCanvas] Backend set, tileUrl='{}'",
                    m_backend->tileServerUrl().toStdString());
        connectBackendSignals();
    }
    emit backendChanged();
    m_tileLayerDirty = true;
    scheduleRepaint();
}

void FlightMapCanvas::connectBackendSignals()
{
    auto repaint = [this] { scheduleRepaint(); };

    // Aircraft — repaint overlays only (tile layer unchanged)
    connect(m_backend, &MapBackend::aircraftPositionChanged, this, [this] {
        if (m_backend->followAircraft()) m_followTimer.start();
        scheduleRepaint();
    });
    connect(m_backend, &MapBackend::aircraftHeadingChanged, this, repaint);
    connect(m_backend, &MapBackend::aircraftVisibleChanged, this, repaint);

    // Home
    connect(m_backend, &MapBackend::homePositionChanged, this, repaint);
    connect(m_backend, &MapBackend::homeVisibleChanged, this, repaint);

    // Track
    connect(m_backend, &MapBackend::trackCoordinateAdded, this, repaint);
    connect(m_backend, &MapBackend::trackPathChanged, this, repaint);

    // WP / route
    connect(m_backend, &MapBackend::activeWpPositionChanged, this, repaint);

    // Layer visibility
    connect(m_backend, &MapBackend::showTrackChanged, this, repaint);
    connect(m_backend, &MapBackend::showWaypointsChanged, this, repaint);
    connect(m_backend, &MapBackend::showZonesChanged, this, repaint);
    connect(m_backend, &MapBackend::showSettlementsChanged, this, repaint);

    // Avoidance
    connect(m_backend, &MapBackend::avoidancePathChanged, this, repaint);
    connect(m_backend, &MapBackend::plannedDirectPathChanged, this, repaint);

    // Drawing
    connect(m_backend, &MapBackend::drawingModeChanged, this, repaint);
    connect(m_backend, &MapBackend::drawingPathChanged, this, repaint);

    // Map control from backend
    connect(m_backend, &MapBackend::mapCenterRequested, this, [this](double lat, double lon) {
        m_centerLat = lat;
        m_centerLon = lon;
        m_tileLayerDirty = true;
        scheduleRepaint();
        m_boundsTimer.start();
    });
    connect(m_backend, &MapBackend::zoomRequested, this, [this](int level) {
        m_zoom = std::clamp(level, MIN_ZOOM, MAX_ZOOM);
        m_tileLayerDirty = true;
        scheduleRepaint();
        m_boundsTimer.start();
    });

    // Tile server URL changes → clear and refetch
    connect(m_backend, &MapBackend::tileServerUrlChanged, this, [this] {
        m_tileCache.clear();
        m_pendingTiles.clear();
        m_tileLayerDirty = true;
        scheduleRepaint();
    });

    // Model changes
    auto connectModel = [this, repaint](QObject* model) {
        auto* m = qobject_cast<QAbstractItemModel*>(model);
        if (!m) return;
        connect(m, &QAbstractItemModel::rowsInserted, this, repaint);
        connect(m, &QAbstractItemModel::rowsRemoved, this, repaint);
        connect(m, &QAbstractItemModel::modelReset, this, repaint);
        connect(m, &QAbstractItemModel::dataChanged, this, repaint);
    };
    connectModel(m_backend->waypointModel());
    connectModel(m_backend->routeSegmentModel());
    connectModel(m_backend->zoneModel());
    connectModel(m_backend->settlementPolyModel());
    connectModel(m_backend->settlementCircleModel());
    connectModel(m_backend->conflictModel());
    connectModel(m_backend->conflictPointModel());
    connectModel(m_backend->drawingVertexModel());
    connectModel(m_backend->editingVertexModel());
    connectModel(m_backend->editingMidpointModel());
    connectModel(m_backend->obstacleWarningModel());

    // Initial bounds
    QTimer::singleShot(0, this, [this] { notifyBounds(); });
}

// ═══════════════════════════════════════════════════════════
//  Precomputed paint constants
// ═══════════════════════════════════════════════════════════

void FlightMapCanvas::updatePaintConstants()
{
    m_pScale = static_cast<double>(1 << m_zoom);
    m_pTotalSize = TILE_SIZE * m_pScale;
    m_pCwx = geoToWorldX(m_centerLon);
    m_pCwy = geoToWorldY(m_centerLat);
    m_pHalfW = width() / 2.0;
    m_pHalfH = height() / 2.0;
}

void FlightMapCanvas::screenToGeo(double sx, double sy, double& lat, double& lon) const
{
    double wx = sx - m_pHalfW + m_pCwx;
    double wy = sy - m_pHalfH + m_pCwy;
    lon = wx / m_pTotalSize * 360.0 - 180.0;
    double n = M_PI - 2.0 * M_PI * wy / m_pTotalSize;
    lat = 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

double FlightMapCanvas::metersPerPixel() const
{
    double latRad = m_centerLat * (M_PI / 180.0);
    return 156543.03392 * std::cos(latRad) / m_pScale;
}

// ═══════════════════════════════════════════════════════════
//  Tile layer cache
// ═══════════════════════════════════════════════════════════

void FlightMapCanvas::compositeTileLayer()
{
    int w = static_cast<int>(width());
    int h = static_cast<int>(height());
    if (w < 1 || h < 1) return;

    // Check if cache is still valid
    if (!m_tileLayerDirty &&
        m_tlCenterLat == m_centerLat && m_tlCenterLon == m_centerLon &&
        m_tlZoom == m_zoom && m_tlWidth == w && m_tlHeight == h &&
        m_tlTileCount == m_tileCache.size()) {
        return;  // cache hit — skip recomposition
    }

    // Recomposite
    if (m_tileLayer.width() != w || m_tileLayer.height() != h)
        m_tileLayer = QImage(w, h, QImage::Format_RGB32);

    m_tileLayer.fill(QColor("#0B0F1A"));

    QPainter tp(&m_tileLayer);
    int maxTile = static_cast<int>(m_pScale);
    double hw = w / 2.0 + TILE_SIZE;
    double hh = h / 2.0 + TILE_SIZE;

    int txMin = std::max(0, static_cast<int>(std::floor((m_pCwx - hw) / TILE_SIZE)));
    int txMax = std::min(maxTile - 1, static_cast<int>(std::floor((m_pCwx + hw) / TILE_SIZE)));
    int tyMin = std::max(0, static_cast<int>(std::floor((m_pCwy - hh) / TILE_SIZE)));
    int tyMax = std::min(maxTile - 1, static_cast<int>(std::floor((m_pCwy + hh) / TILE_SIZE)));

    for (int ty = tyMin; ty <= tyMax; ++ty) {
        for (int tx = txMin; tx <= txMax; ++tx) {
            auto it = m_tileCache.find(TileKey{m_zoom, tx, ty});
            if (it != m_tileCache.end()) {
                int sx = static_cast<int>(tx * TILE_SIZE - m_pCwx + m_pHalfW);
                int sy = static_cast<int>(ty * TILE_SIZE - m_pCwy + m_pHalfH);
                tp.drawImage(sx, sy, it.value());
            }
        }
    }
    tp.end();

    m_tlCenterLat = m_centerLat;
    m_tlCenterLon = m_centerLon;
    m_tlZoom = m_zoom;
    m_tlWidth = w;
    m_tlHeight = h;
    m_tlTileCount = m_tileCache.size();
    m_tileLayerDirty = false;
}

void FlightMapCanvas::paintTilesFromCache(QPainter* p)
{
    compositeTileLayer();
    if (!m_tileLayer.isNull())
        p->drawImage(0, 0, m_tileLayer);
}

// ═══════════════════════════════════════════════════════════
//  Tiles — fetch
// ═══════════════════════════════════════════════════════════

void FlightMapCanvas::updateVisibleTiles()
{
    if (!m_backend || m_backend->tileServerUrl().isEmpty()) return;
    if (width() < 1 || height() < 1) return;

    // Use current paint constants
    updatePaintConstants();

    int maxTile = static_cast<int>(m_pScale);
    double hw = width() / 2.0 + TILE_SIZE;
    double hh = height() / 2.0 + TILE_SIZE;

    int txMin = std::max(0, static_cast<int>(std::floor((m_pCwx - hw) / TILE_SIZE)));
    int txMax = std::min(maxTile - 1, static_cast<int>(std::floor((m_pCwx + hw) / TILE_SIZE)));
    int tyMin = std::max(0, static_cast<int>(std::floor((m_pCwy - hh) / TILE_SIZE)));
    int tyMax = std::min(maxTile - 1, static_cast<int>(std::floor((m_pCwy + hh) / TILE_SIZE)));

    for (int ty = tyMin; ty <= tyMax; ++ty) {
        for (int tx = txMin; tx <= txMax; ++tx) {
            TileKey key{m_zoom, tx, ty};
            if (!m_tileCache.contains(key) && !m_pendingTiles.contains(key))
                requestTile(key);
        }
    }
}

void FlightMapCanvas::requestTile(const TileKey& key)
{
    if (m_activeFetches >= MAX_CONCURRENT_FETCHES) return;
    if (!m_backend) return;

    QString base = m_backend->tileServerUrl();
    while (base.endsWith('/')) base.chop(1);
    QString url = QString("%1/%2/%3/%4.png").arg(base).arg(key.z).arg(key.x).arg(key.y);

    m_pendingTiles.insert(key);
    m_activeFetches++;

    auto* reply = m_nam.get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, key, reply] {
        reply->deleteLater();
        m_activeFetches--;
        m_pendingTiles.remove(key);

        if (reply->error() == QNetworkReply::NoError) {
            QByteArray data = reply->readAll();
            QImage img;
            if (img.loadFromData(data)) {
                // Convert to Format_RGB32 for fastest blitting
                if (img.format() != QImage::Format_RGB32)
                    img = img.convertToFormat(QImage::Format_RGB32);
                if (m_tileCache.size() >= MAX_CACHED_TILES) evictTiles();
                m_tileCache.insert(key, std::move(img));
                m_tileLayerDirty = true;
                scheduleRepaint();
            }
        }
        updateVisibleTiles();
    });
}

void FlightMapCanvas::evictTiles()
{
    QList<TileKey> keys = m_tileCache.keys();
    int target = MAX_CACHED_TILES * 3 / 4;
    std::sort(keys.begin(), keys.end(), [this](const TileKey& a, const TileKey& b) {
        return std::abs(a.z - m_zoom) > std::abs(b.z - m_zoom);
    });
    while (m_tileCache.size() > target && !keys.isEmpty())
        m_tileCache.remove(keys.takeFirst());
}

// ═══════════════════════════════════════════════════════════
//  Scheduling & Bounds
// ═══════════════════════════════════════════════════════════

void FlightMapCanvas::scheduleRepaint()
{
    if (!m_repaintScheduled) {
        m_repaintScheduled = true;
        m_repaintTimer.start();
    }
}

void FlightMapCanvas::notifyBounds()
{
    if (!m_backend || width() < 1 || height() < 1) return;
    updatePaintConstants();
    double nwLat, nwLon, seLat, seLon;
    screenToGeo(0, 0, nwLat, nwLon);
    screenToGeo(width(), height(), seLat, seLon);
    m_backend->onBoundsChanged(
        std::min(nwLat, seLat), std::min(nwLon, seLon),
        std::max(nwLat, seLat), std::max(nwLon, seLon));
    m_backend->onZoomChanged(m_zoom);
}

// ═══════════════════════════════════════════════════════════
//  Mouse handling
// ═══════════════════════════════════════════════════════════

void FlightMapCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_editDragVertex = hitTestEditingVertex(event->position().x(), event->position().y());
        if (m_editDragVertex >= 0) { event->accept(); return; }
        m_dragging = true;
        m_dragStart = event->position();
        m_dragStartLat = m_centerLat;
        m_dragStartLon = m_centerLon;
        if (m_backend) m_backend->setFollowAircraft(false);
        event->accept();
    } else if (event->button() == Qt::RightButton) {
        if (!m_backend) return;
        int vtx = hitTestEditingVertex(event->position().x(), event->position().y());
        if (vtx >= 0) { m_backend->deleteEditingVertex(vtx); event->accept(); return; }
        double lat, lon;
        screenToGeo(event->position().x(), event->position().y(), lat, lon);
        m_backend->onContextMenu(lat, lon,
                                 static_cast<int>(event->position().x()),
                                 static_cast<int>(event->position().y()));
        event->accept();
    }
}

void FlightMapCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (m_editDragVertex >= 0 && m_backend) {
        double lat, lon;
        screenToGeo(event->position().x(), event->position().y(), lat, lon);
        m_backend->moveEditingVertex(m_editDragVertex, lat, lon);
        event->accept();
        return;
    }

    if (!m_dragging) return;
    double dx = event->position().x() - m_dragStart.x();
    double dy = event->position().y() - m_dragStart.y();

    // Recompute constants for drag start position
    double startCwx = (m_dragStartLon + 180.0) / 360.0 * m_pTotalSize;
    double startLatRad = m_dragStartLat * (M_PI / 180.0);
    double startCwy = (1.0 - std::log(std::tan(startLatRad) + 1.0 / std::cos(startLatRad)) / M_PI) * 0.5 * m_pTotalSize;

    double newCwx = startCwx - dx;
    double newCwy = startCwy - dy;

    m_centerLon = newCwx / m_pTotalSize * 360.0 - 180.0;
    double n = M_PI - 2.0 * M_PI * newCwy / m_pTotalSize;
    m_centerLat = 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
    m_centerLat = std::clamp(m_centerLat, -85.0, 85.0);
    m_centerLon = std::fmod(m_centerLon + 540.0, 360.0) - 180.0;

    m_tileLayerDirty = true;
    scheduleRepaint();
    m_boundsTimer.start();
    event->accept();
}

void FlightMapCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_editDragVertex >= 0) { m_editDragVertex = -1; event->accept(); return; }
        if (m_dragging) {
            double dist = QPointF(event->position() - m_dragStart).manhattanLength();
            m_dragging = false;
            if (dist < 5.0 && m_backend &&
                (m_backend->drawingMode() || m_backend->leftClickMode())) {
                m_clickPending = true;
                m_clickPos = event->position();
                m_clickTimer.start();
            }
        }
        event->accept();
    }
}

void FlightMapCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_backend) {
        m_clickTimer.stop();
        m_clickPending = false;
        double lat, lon;
        screenToGeo(event->position().x(), event->position().y(), lat, lon);
        m_backend->onMapDoubleClick(lat, lon);
        event->accept();
    }
}

void FlightMapCanvas::wheelEvent(QWheelEvent* event)
{
    int dy = event->angleDelta().y();
    if (dy == 0) return;

    int steps = (dy > 0) ? 1 : -1;
    int newZoom = std::clamp(m_zoom + steps, MIN_ZOOM, MAX_ZOOM);
    if (newZoom == m_zoom) return;

    // Zoom toward cursor
    auto cursorPos = event->position();
    double geoLat, geoLon;
    screenToGeo(cursorPos.x(), cursorPos.y(), geoLat, geoLon);

    m_zoom = newZoom;
    updatePaintConstants();  // recalc with new zoom

    double newWx = geoToWorldX(geoLon);
    double newWy = geoToWorldY(geoLat);
    double targetCwx = newWx - cursorPos.x() + m_pHalfW;
    double targetCwy = newWy - cursorPos.y() + m_pHalfH;

    m_centerLon = targetCwx / m_pTotalSize * 360.0 - 180.0;
    double n2 = M_PI - 2.0 * M_PI * targetCwy / m_pTotalSize;
    m_centerLat = 180.0 / M_PI * std::atan(0.5 * (std::exp(n2) - std::exp(-n2)));
    m_centerLat = std::clamp(m_centerLat, -85.0, 85.0);
    m_centerLon = std::fmod(m_centerLon + 540.0, 360.0) - 180.0;

    m_tileLayerDirty = true;
    scheduleRepaint();
    m_boundsTimer.start();
    event->accept();
}

void FlightMapCanvas::hoverMoveEvent(QHoverEvent* event)
{
    m_mouseScreenPos = event->position();

    if (!m_backend) return;
    double lat, lon;
    screenToGeo(event->position().x(), event->position().y(), lat, lon);
    m_backend->onMouseMove(lat, lon);

    // Hit-test obstacle warnings for hover tooltip
    int prevHovered = m_hoveredWarning;
    m_hoveredWarning = -1;
    if (m_zoom >= 10 && m_zoom <= 16) {
        auto* model = static_cast<ObstacleWarningModel*>(m_backend->obstacleWarningModel());
        const auto& items = model->items();
        double bestDist2 = 18.0 * 18.0;  // 18px hit radius
        for (int i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            // Visibility check: zones need showZones, settlements need showSettlements
            if (item.source == "zone" && !m_backend->showZones()) continue;
            if (item.source == "settlement" && !m_backend->showSettlements()) continue;
            QPointF sp = geoToScreen(item.lat, item.lon);
            double dx = sp.x() - m_mouseScreenPos.x();
            double dy = sp.y() - m_mouseScreenPos.y();
            double d2 = dx * dx + dy * dy;
            if (d2 < bestDist2) {
                bestDist2 = d2;
                m_hoveredWarning = i;
            }
        }
    }
    if (m_hoveredWarning != prevHovered)
        scheduleRepaint();
}

void FlightMapCanvas::hoverLeaveEvent(QHoverEvent* /*event*/)
{
    if (m_hoveredWarning >= 0) {
        m_hoveredWarning = -1;
        scheduleRepaint();
    }
}

// ═══════════════════════════════════════════════════════════
//  Editing vertex hit-test
// ═══════════════════════════════════════════════════════════

int FlightMapCanvas::hitTestEditingVertex(double sx, double sy, double radius) const
{
    if (!m_backend) return -1;
    auto* model = qobject_cast<SimpleVertexModel*>(m_backend->editingVertexModel());
    if (!model) return -1;
    auto pts = model->getPoints();
    double r2 = radius * radius;
    for (int i = 0; i < pts.size(); ++i) {
        QPointF sp = geoToScreen(pts[i].x(), pts[i].y());
        double dx = sp.x() - sx, dy = sp.y() - sy;
        if (dx * dx + dy * dy <= r2)
            return i;
    }
    return -1;
}

double FlightMapCanvas::zoomAlpha() const
{
    if (m_zoom <= 14) return 0.4;
    if (m_zoom <= 15) return 0.32;
    if (m_zoom <= 16) return 0.2;
    return 0.12;
}

// ═══════════════════════════════════════════════════════════
//  paint() — master render
// ═══════════════════════════════════════════════════════════

void FlightMapCanvas::paint(QPainter* p)
{
    // 0. Precompute frame constants (used by ALL geoToScreen calls)
    updatePaintConstants();

    // 1. Tiles (cached composite — single blit)
    paintTilesFromCache(p);

    if (!m_backend) return;

    // Enable AA only for overlays
    p->setRenderHint(QPainter::Antialiasing, true);

    // 2-4. Settlements
    if (m_backend->showSettlements() && m_zoom >= 12) {
        paintSettlementPolygons(p);
        paintSettlementCircles(p);
        paintSettlementBorders(p);
    }

    // 5-6. Zones
    if (m_backend->showZones() && m_zoom >= 14) {
        paintZoneFills(p);
        paintZoneBorders(p);
    }

    // 7. Track
    if (m_backend->showTrack())
        paintTrack(p);

    // 8. Route segments
    if (m_backend->showWaypoints())
        paintRouteSegments(p);

    // 9. Active WP line
    if (m_backend->showWaypoints() && m_backend->aircraftVisible())
        paintActiveWpLine(p);

    // 10. Conflicts + avoidance
    paintConflictSegments(p);
    paintPlannedDirectPath(p);
    paintAvoidancePath(p);

    // 11. Obstacle warnings (⚠)
    paintObstacleWarnings(p);

    // 12. Conflict points
    paintConflictPoints(p);

    // 13. Waypoints
    if (m_backend->showWaypoints())
        paintWaypoints(p);

    // 14. Drawing + editing
    paintDrawingOverlay(p);
    paintEditingVertices(p);

    // 15. Home
    paintHome(p);

    // 16. Aircraft
    paintAircraft(p);
}

// ═══════════════════════════════════════════════════════════
//  Paint layers
// ═══════════════════════════════════════════════════════════

void FlightMapCanvas::paintSettlementPolygons(QPainter* p)
{
    auto* model = static_cast<SettlementPolyModel*>(m_backend->settlementPolyModel());
    const auto& items = model->items();
    if (items.isEmpty()) return;

    double alpha = zoomAlpha();
    QColor fill(0xEF, 0x44, 0x44);
    fill.setAlphaF(alpha);
    p->setPen(Qt::NoPen);
    p->setBrush(fill);

    for (const auto& coordList : items) {
        QPolygonF poly;
        poly.reserve(coordList.size());
        for (const auto& c : coordList) {
            auto pair = c.toList();
            if (pair.size() >= 2)
                poly.append(geoToScreen(pair[0].toDouble(), pair[1].toDouble()));
        }
        if (poly.size() >= 3)
            p->drawPolygon(poly);
    }
}

void FlightMapCanvas::paintSettlementCircles(QPainter* p)
{
    auto* model = static_cast<SettlementCircleModel*>(m_backend->settlementCircleModel());
    const auto& items = model->items();
    if (items.isEmpty()) return;

    double alpha = zoomAlpha();
    QColor fill(0xEF, 0x44, 0x44);
    fill.setAlphaF(alpha);
    QColor border(0xEF, 0x44, 0x44);
    border.setAlphaF(std::min(alpha * 2.0, 1.0));

    double mpp = metersPerPixel();
    p->setPen(QPen(border, 2));
    p->setBrush(fill);

    for (const auto& item : items) {
        double radiusPx = item.radius / mpp;
        if (radiusPx < 2.0) continue;
        p->drawEllipse(geoToScreen(item.lat, item.lon), radiusPx, radiusPx);
    }
}

void FlightMapCanvas::paintSettlementBorders(QPainter* p)
{
    auto* model = static_cast<SettlementPolyModel*>(m_backend->settlementPolyModel());
    const auto& items = model->items();
    if (items.isEmpty()) return;

    double alpha = (m_zoom <= 14) ? 1.0 : (m_zoom <= 15) ? 0.8 : (m_zoom <= 16) ? 0.5 : 0.3;
    QColor col(0xEF, 0x44, 0x44);
    col.setAlphaF(alpha);
    QPen pen(col, 2, Qt::DashLine);
    pen.setDashPattern({10, 8});
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);

    for (const auto& coordList : items) {
        QPolygonF poly;
        poly.reserve(coordList.size() + 1);
        for (const auto& c : coordList) {
            auto pair = c.toList();
            if (pair.size() >= 2)
                poly.append(geoToScreen(pair[0].toDouble(), pair[1].toDouble()));
        }
        if (poly.size() >= 3) {
            poly.append(poly.first());
            p->drawPolyline(poly);
        }
    }
}

void FlightMapCanvas::paintZoneFills(QPainter* p)
{
    auto* model = static_cast<ZoneListModel*>(m_backend->zoneModel());
    const auto& items = model->items();
    if (items.isEmpty()) return;

    double alpha = (m_zoom < 15) ? 0.28 : 0.48;
    QColor fill(0x1A, 0x05, 0x05);
    fill.setAlphaF(alpha);
    p->setPen(Qt::NoPen);
    p->setBrush(fill);

    for (const auto& item : items) {
        QPolygonF poly;
        poly.reserve(item.points.size());
        for (const auto& pt : item.points) {
            auto pair = pt.toList();
            if (pair.size() >= 2)
                poly.append(geoToScreen(pair[0].toDouble(), pair[1].toDouble()));
        }
        if (poly.size() >= 3)
            p->drawPolygon(poly);
    }
}

void FlightMapCanvas::paintZoneBorders(QPainter* p)
{
    auto* model = static_cast<ZoneListModel*>(m_backend->zoneModel());
    const auto& items = model->items();
    if (items.isEmpty()) return;

    double alpha = (m_zoom < 15) ? 0.45 : 0.7;
    QColor col(0x8B, 0x1A, 0x1A);
    col.setAlphaF(alpha);
    QPen pen(col, 3, Qt::DashLine);
    pen.setDashPattern({8, 6});
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);

    for (const auto& item : items) {
        QPolygonF poly;
        poly.reserve(item.points.size() + 1);
        for (const auto& pt : item.points) {
            auto pair = pt.toList();
            if (pair.size() >= 2)
                poly.append(geoToScreen(pair[0].toDouble(), pair[1].toDouble()));
        }
        if (poly.size() >= 3) {
            poly.append(poly.first());
            p->drawPolyline(poly);
        }
    }
}

void FlightMapCanvas::paintTrack(QPainter* p)
{
    const auto& trackPath = m_backend->trackPath();
    if (trackPath.size() < 2) return;

    p->setPen(QPen(QColor("#22C55E"), 2));
    p->setBrush(Qt::NoBrush);

    QPolygonF poly;
    poly.reserve(trackPath.size());
    for (const auto& v : trackPath) {
        auto coord = v.value<QGeoCoordinate>();
        if (coord.isValid())
            poly.append(geoToScreen(coord.latitude(), coord.longitude()));
    }
    if (poly.size() >= 2)
        p->drawPolyline(poly);
}

void FlightMapCanvas::paintRouteSegments(QPainter* p)
{
    auto* model = static_cast<RouteSegmentModel*>(m_backend->routeSegmentModel());
    const auto& items = model->items();
    if (items.isEmpty()) return;

    for (const auto& seg : items) {
        QColor col;
        int w;
        if (seg.state == "past")       { col = QColor(255, 255, 255, 85);  w = 2; }
        else if (seg.state == "active") { col = QColor(255, 255, 255, 255); w = 3; }
        else                            { col = QColor(255, 255, 255, 170); w = 2; }
        p->setPen(QPen(col, w));
        p->drawLine(geoToScreen(seg.fromLat, seg.fromLon),
                     geoToScreen(seg.toLat, seg.toLon));
    }
}

void FlightMapCanvas::paintActiveWpLine(QPainter* p)
{
    auto awp = m_backend->activeWpPosition();
    auto ap = m_backend->aircraftPosition();
    if (!awp.isValid() || !ap.isValid()) return;

    QColor col(0xF5, 0x9E, 0x0B);
    col.setAlphaF(0.93);
    p->setPen(QPen(col, 3));
    p->drawLine(geoToScreen(ap.latitude(), ap.longitude()),
                geoToScreen(awp.latitude(), awp.longitude()));
}

void FlightMapCanvas::paintConflictSegments(QPainter* p)
{
    auto* model = static_cast<ConflictSegmentModel*>(m_backend->conflictModel());
    const auto& items = model->items();
    if (items.isEmpty()) return;

    p->setPen(QPen(QColor("#EF4444"), 3));
    for (const auto& seg : items)
        p->drawLine(geoToScreen(seg.fromLat, seg.fromLon),
                     geoToScreen(seg.toLat, seg.toLon));
}

void FlightMapCanvas::paintPlannedDirectPath(QPainter* p)
{
    const auto& path = m_backend->plannedDirectPath();
    if (path.size() < 2) return;

    QColor col(0xFD, 0xE0, 0x47);
    col.setAlphaF(0.95);
    p->setPen(QPen(col, 4));
    p->setBrush(Qt::NoBrush);

    QPolygonF poly;
    for (const auto& v : path) {
        auto coord = v.value<QGeoCoordinate>();
        if (coord.isValid())
            poly.append(geoToScreen(coord.latitude(), coord.longitude()));
    }
    if (poly.size() >= 2)
        p->drawPolyline(poly);
}

void FlightMapCanvas::paintAvoidancePath(QPainter* p)
{
    const auto& path = m_backend->avoidancePath();
    if (path.size() < 2) return;

    p->setPen(QPen(QColor("#F59E0B"), 3));
    p->setBrush(Qt::NoBrush);

    QPolygonF poly;
    for (const auto& v : path) {
        auto coord = v.value<QGeoCoordinate>();
        if (coord.isValid())
            poly.append(geoToScreen(coord.latitude(), coord.longitude()));
    }
    if (poly.size() >= 2)
        p->drawPolyline(poly);
}

void FlightMapCanvas::paintObstacleWarnings(QPainter* p)
{
    if (m_zoom < 10 || m_zoom > 16) return;

    auto* model = static_cast<ObstacleWarningModel*>(m_backend->obstacleWarningModel());
    const auto& items = model->items();
    if (items.isEmpty()) return;

    auto aircraftPos = m_backend->aircraftPosition();
    bool hasAircraft = m_backend->aircraftVisible() && aircraftPos.isValid();

    QFont iconFont;
    iconFont.setPixelSize(14);
    iconFont.setBold(true);

    constexpr double BADGE_SIZE = 24.0;
    constexpr double HALF = BADGE_SIZE / 2.0;

    for (int i = 0; i < items.size(); ++i) {
        const auto& item = items[i];

        // Per-source visibility
        if (item.source == "zone" && !m_backend->showZones()) continue;
        if (item.source == "settlement" && !m_backend->showSettlements()) continue;

        // Proximity fade: 1.0 at >5km, 0.0 at <1km
        double opacity = 1.0;
        if (hasAircraft) {
            QGeoCoordinate itemPos(item.lat, item.lon);
            double dist = aircraftPos.distanceTo(itemPos);
            opacity = std::clamp((dist - 1000.0) / 4000.0, 0.0, 1.0);
        }
        if (opacity < 0.02) continue;

        QPointF sp = geoToScreen(item.lat, item.lon);

        // Cull off-screen (with margin)
        if (sp.x() < -HALF || sp.x() > width() + HALF ||
            sp.y() < -HALF || sp.y() > height() + HALF)
            continue;

        p->setOpacity(opacity);

        // Badge background
        QRectF badgeRect(sp.x() - HALF, sp.y() - HALF, BADGE_SIZE, BADGE_SIZE);
        bool isZone = (item.source == "zone");

        QColor fill = isZone ? QColor(0xFE, 0xF3, 0xC7) : QColor(0xFE, 0xE2, 0xE2);
        QColor border = isZone ? QColor(0xF5, 0x9E, 0x0B) : QColor(0xEF, 0x44, 0x44);
        QColor textCol = isZone ? QColor(0x92, 0x40, 0x0E) : QColor(0xB9, 0x1C, 0x1C);

        p->setPen(QPen(border, 2));
        p->setBrush(fill);
        p->drawRoundedRect(badgeRect, 4, 4);

        // ⚠ icon text
        p->setFont(iconFont);
        p->setPen(textCol);
        p->drawText(badgeRect, Qt::AlignCenter, QString::fromUtf8("\u26A0"));
    }

    p->setOpacity(1.0);

    // Draw hover tooltip
    if (m_hoveredWarning >= 0 && m_hoveredWarning < items.size()) {
        const auto& item = items[m_hoveredWarning];
        QPointF sp = geoToScreen(item.lat, item.lon);

        QFont tipFont;
        tipFont.setPixelSize(11);
        QFontMetrics fm(tipFont);

        QStringList lines = item.tooltip.split('\n');
        int maxWidth = 0;
        int totalHeight = 0;
        for (const auto& line : lines) {
            int w = fm.horizontalAdvance(line);
            maxWidth = std::max(maxWidth, w);
            totalHeight += fm.height();
        }

        int padX = 8, padY = 6;
        int tipW = std::min(maxWidth + padX * 2, 320);
        int tipH = totalHeight + padY * 2;

        QRectF tipRect(sp.x() - tipW / 2.0, sp.y() - HALF - 8 - tipH, tipW, tipH);

        // Clamp to viewport
        if (tipRect.left() < 4) tipRect.moveLeft(4);
        if (tipRect.right() > width() - 4) tipRect.moveRight(width() - 4);
        if (tipRect.top() < 4) tipRect.moveTop(sp.y() + HALF + 8);  // flip below

        p->setPen(QPen(QColor(0x33, 0x41, 0x55), 1));
        p->setBrush(QColor(0x11, 0x18, 0x27, 0xDD));
        p->drawRoundedRect(tipRect, 6, 6);

        p->setFont(tipFont);
        p->setPen(QColor(0xF8, 0xFA, 0xFC));
        QRectF textRect = tipRect.adjusted(padX, padY, -padX, -padY);
        p->drawText(textRect, Qt::AlignHCenter | Qt::TextWordWrap, item.tooltip);
    }
}

void FlightMapCanvas::paintConflictPoints(QPainter* p)
{
    auto* model = static_cast<ConflictPointModel*>(m_backend->conflictPointModel());
    const auto& items = model->items();
    if (items.isEmpty()) return;

    QFont font;
    font.setPixelSize(15);
    font.setBold(true);

    for (const auto& item : items) {
        QPointF sp = geoToScreen(item.lat, item.lon);
        p->setPen(QPen(QColor("#EF4444"), 2));
        p->setBrush(QColor("#FEE2E2"));
        p->drawEllipse(sp, 12, 12);
        p->setPen(QColor("#B91C1C"));
        p->setFont(font);
        p->drawText(QRectF(sp.x() - 12, sp.y() - 12, 24, 24), Qt::AlignCenter, "!");
    }
}

void FlightMapCanvas::paintWaypoints(QPainter* p)
{
    auto* model = static_cast<WaypointListModel*>(m_backend->waypointModel());
    const auto& items = model->items();
    if (items.isEmpty()) return;

    QFont font;
    font.setPixelSize(12);
    font.setBold(true);
    p->setFont(font);

    for (int i = 0; i < items.size(); ++i) {
        const auto& wp = items[i];
        QPointF sp = geoToScreen(wp.lat, wp.lon);

        QColor fill, textCol;
        bool hasBorder = false;
        if (wp.state == "past")        { fill = QColor(0x88, 0x88, 0x88, 85); textCol = QColor("#AAAAAA"); }
        else if (wp.state == "active") { fill = QColor("#22C55E"); textCol = QColor("#000000"); hasBorder = true; }
        else                           { fill = QColor(255, 255, 255, 170); textCol = QColor("#000000"); }

        p->setPen(hasBorder ? QPen(Qt::white, 2) : QPen(Qt::NoPen));
        p->setBrush(fill);
        p->drawEllipse(sp, 14, 14);
        p->setPen(textCol);
        p->drawText(QRectF(sp.x() - 14, sp.y() - 14, 28, 28), Qt::AlignCenter, QString::number(i + 1));
    }
}

void FlightMapCanvas::paintDrawingOverlay(QPainter* p)
{
    if (!m_backend->drawingMode()) return;

    const auto& path = m_backend->drawingPath();
    if (path.size() >= 2) {
        p->setPen(QPen(QColor("#FFFF00"), 2));
        p->setBrush(Qt::NoBrush);
        QPolygonF poly;
        for (const auto& v : path) {
            auto coord = v.value<QGeoCoordinate>();
            if (coord.isValid())
                poly.append(geoToScreen(coord.latitude(), coord.longitude()));
        }
        if (poly.size() >= 2)
            p->drawPolyline(poly);
    }

    auto* vtxModel = qobject_cast<SimpleVertexModel*>(m_backend->drawingVertexModel());
    if (!vtxModel) return;
    auto pts = vtxModel->getPoints();
    for (int i = 0; i < pts.size(); ++i) {
        QPointF sp = geoToScreen(pts[i].x(), pts[i].y());
        double r = (i == 0) ? 8.0 : 5.0;
        p->setPen(QPen(Qt::black, 1));
        p->setBrush((i == 0) ? QColor("#FFFF00") : QColor("#FFFFFF"));
        p->drawEllipse(sp, r, r);
    }
}

void FlightMapCanvas::paintEditingVertices(QPainter* p)
{
    auto* vtxModel = qobject_cast<SimpleVertexModel*>(m_backend->editingVertexModel());
    if (!vtxModel) return;
    auto pts = vtxModel->getPoints();
    if (pts.isEmpty()) return;

    if (pts.size() >= 2) {
        p->setPen(QPen(QColor("#EF4444"), 2));
        p->setBrush(Qt::NoBrush);
        QPolygonF poly;
        for (const auto& pt : pts)
            poly.append(geoToScreen(pt.x(), pt.y()));
        poly.append(poly.first());
        p->drawPolyline(poly);
    }

    for (const auto& pt : pts) {
        p->setPen(QPen(QColor("#EF4444"), 2));
        p->setBrush(Qt::white);
        p->drawEllipse(geoToScreen(pt.x(), pt.y()), 8, 8);
    }

    auto* midModel = qobject_cast<SimpleVertexModel*>(m_backend->editingMidpointModel());
    if (!midModel) return;
    auto mids = midModel->getPoints();
    p->setPen(QPen(Qt::white, 1));
    p->setBrush(QColor("#888888"));
    for (const auto& pt : mids)
        p->drawEllipse(geoToScreen(pt.x(), pt.y()), 5, 5);
}

void FlightMapCanvas::paintHome(QPainter* p)
{
    if (!m_backend->homeVisible()) return;
    auto pos = m_backend->homePosition();
    if (!pos.isValid()) return;

    QPointF sp = geoToScreen(pos.latitude(), pos.longitude());
    QColor border(0x25, 0x63, 0xEB);
    p->setPen(QPen(border, 2));
    p->setBrush(QColor(0x25, 0x63, 0xEB, 51));
    p->drawEllipse(sp, 16, 16);

    QFont font;
    font.setPixelSize(14);
    font.setBold(true);
    p->setPen(border);
    p->setFont(font);
    p->drawText(QRectF(sp.x() - 16, sp.y() - 16, 32, 32), Qt::AlignCenter, "H");
}

void FlightMapCanvas::paintAircraft(QPainter* p)
{
    if (!m_backend->aircraftVisible()) return;
    auto pos = m_backend->aircraftPosition();
    if (!pos.isValid()) return;

    QPointF sp = geoToScreen(pos.latitude(), pos.longitude());
    double heading = m_backend->aircraftHeading();

    p->setPen(QPen(QColor(0x22, 0xC5, 0x5E), 1.5));
    p->setBrush(QColor(0x22, 0xC5, 0x5E, 51));
    p->drawEllipse(sp, 20, 20);

    p->save();
    p->translate(sp);
    p->rotate(heading);

    QPainterPath arrow;
    arrow.moveTo(0, -10);
    arrow.lineTo(-6, 8);
    arrow.lineTo(0, 4);
    arrow.lineTo(6, 8);
    arrow.closeSubpath();

    p->setPen(QPen(QColor("#22C55E"), 1));
    p->setBrush(QColor("#22C55E"));
    p->drawPath(arrow);
    p->restore();
}

} // namespace vtol
