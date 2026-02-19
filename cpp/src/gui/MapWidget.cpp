#include "MapWidget.h"
#include "MapLibreAdapter.h"
#include "MapLegend.h"

#include <QMapLibreWidgets/GLWidget>
#include <QMapLibre/Map>
#include <QMapLibre/Settings>
#include <QMapLibre/Types>

#include <QMouseEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QShowEvent>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <spdlog/spdlog.h>

namespace vtol {

MapWidget::MapWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // Follow mode: sync map center to every aircraft position update (no timer lag)
    connect(&m_backend, &MapBackend::aircraftPositionChanged, this, &MapWidget::onFollowTick);

    // Center request from backend
    connect(&m_backend, &MapBackend::mapCenterRequested, this, [this](double lat, double lon) {
        if (m_map) m_map->setCoordinate({lat, lon});
    });
}

void MapWidget::setTileServerUrl(const QString& url)
{
    m_tileUrl = url;
    m_backend.setTileServer(url);
}

void MapWidget::loadMap()
{
    SPDLOG_INFO("[MapWidget] loadMap() — creating GLWidget");

    // --- Settings ---
    QMapLibre::Settings settings(QMapLibre::Settings::NoProvider);
    settings.setContextMode(QMapLibre::Settings::SharedGLContext);

    QString cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                        + "/maplibre-cache.db";
    settings.setCacheDatabasePath(cachePath);
    settings.setCacheDatabaseMaximumSize(256 * 1024 * 1024);  // 256 MB

    // Default coordinate: Moscow
    settings.setDefaultCoordinate({55.75, 37.62});
    settings.setDefaultZoom(10.0);

    // --- Create GLWidget (Map* will be null until initializeGL) ---
    m_glWidget = new QMapLibre::GLWidget(settings);
    m_glWidget->setFocusPolicy(Qt::StrongFocus);
    layout()->addWidget(m_glWidget);

    // Connect mouse signals (GLWidget handles null m_map internally)
    connect(m_glWidget, &QMapLibre::GLWidget::onMousePressEvent,
            this, [this](QMapLibre::Coordinate c) {
                QPointF screenPos = m_glWidget->mapFromGlobal(QCursor::pos());
                onMousePress(c.first, c.second, screenPos, Qt::LeftButton);
            });
    connect(m_glWidget, &QMapLibre::GLWidget::onMouseReleaseEvent,
            this, [this](QMapLibre::Coordinate c) {
                QPointF screenPos = m_glWidget->mapFromGlobal(QCursor::pos());
                onMouseRelease(c.first, c.second, screenPos, Qt::LeftButton);
            });
    // NOTE: double-click handled ONLY in eventFilter (not via signal)
    // to prevent calling onMouseDoubleClick twice and to consume the event
    // when clicking on zones (preventing GLWidget zoom)
    connect(m_glWidget, &QMapLibre::GLWidget::onMouseMoveEvent,
            this, [this](QMapLibre::Coordinate c) {
                onMouseMove(c.first, c.second);
            });

    m_glWidget->installEventFilter(this);

    // FPS overlay label
    m_fpsLabel = new QLabel(this);
    m_fpsLabel->setStyleSheet(
        "background-color: rgba(0,0,0,160); color: #0f0; "
        "font-family: monospace; font-size: 11px; font-weight: bold; "
        "padding: 2px 6px; border-radius: 3px;");
    m_fpsLabel->setText("-- fps");
    m_fpsLabel->setFixedHeight(20);
    m_fpsLabel->adjustSize();
    m_fpsLabel->raise();

    // Legend overlay (bottom-left)
    m_legend = new MapLegend(&m_backend, this);
    m_legend->raise();

    SPDLOG_INFO("[MapWidget] GLWidget created, waiting for initializeGL...");
}

void MapWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    if (!m_mapSetupDone && m_glWidget) {
        // After show, initializeGL should have run — schedule setup on next event loop tick
        QTimer::singleShot(0, this, &MapWidget::setupMap);
    }
}

void MapWidget::setupMap()
{
    if (m_mapSetupDone) return;

    m_map = m_glWidget->map();
    if (!m_map) {
        SPDLOG_WARN("[MapWidget] Map still null after show, retrying...");
        QTimer::singleShot(50, this, &MapWidget::setupMap);
        return;
    }

    m_mapSetupDone = true;
    SPDLOG_INFO("[MapWidget] Map ptr obtained, setting up style");

    // --- Style JSON ---
    QString tileUrlTemplate = m_tileUrl;
    if (!tileUrlTemplate.isEmpty()) {
        if (!tileUrlTemplate.endsWith('/')) tileUrlTemplate += '/';
        tileUrlTemplate += "{z}/{x}/{y}.png";
    }

    QJsonObject style;
    style["version"] = 8;
    style["name"] = "VTOL Satellite";
    // NOTE: no "glyphs" — waypoint numbers baked into icon images via QPainter

    QJsonObject sources;
    if (!tileUrlTemplate.isEmpty()) {
        QJsonObject satellite;
        satellite["type"] = "raster";
        QJsonArray tiles;
        tiles.append(tileUrlTemplate);
        satellite["tiles"] = tiles;
        satellite["tileSize"] = 256;
        sources["satellite"] = satellite;
    }
    style["sources"] = sources;

    QJsonArray layers;

    QJsonObject bgLayer;
    bgLayer["id"] = "bg";
    bgLayer["type"] = "background";
    QJsonObject bgPaint;
    bgPaint["background-color"] = "#0B0F1A";
    bgLayer["paint"] = bgPaint;
    layers.append(bgLayer);

    if (!tileUrlTemplate.isEmpty()) {
        QJsonObject satLayer;
        satLayer["id"] = "satellite";
        satLayer["type"] = "raster";
        satLayer["source"] = "satellite";
        layers.append(satLayer);
    }

    style["layers"] = layers;

    // --- Connect signals BEFORE setting style ---
    connect(m_map, &QMapLibre::Map::mapChanged, this, [this](QMapLibre::Map::MapChange change) {
        onMapChanged(static_cast<int>(change));
    });

    connect(m_map, &QMapLibre::Map::mapChanged, this, [this](QMapLibre::Map::MapChange change) {
        if (change == QMapLibre::Map::MapChangeDidFinishLoadingStyle && !m_adapter) {
            m_adapter = new MapLibreAdapter(m_map, &m_backend, this);
            SPDLOG_INFO("[MapWidget] MapLibreAdapter created after style loaded");
        }
    });

    // --- FPS counting: count map renders ---
    m_fpsTimer.start();
    connect(m_map, &QMapLibre::Map::mapChanged, this, [this](QMapLibre::Map::MapChange change) {
        if (change == QMapLibre::Map::MapChangeDidFinishRenderingFrameFullyRendered ||
            change == QMapLibre::Map::MapChangeDidFinishRenderingFrame) {
            ++m_fpsFrameCount;
            qint64 elapsed = m_fpsTimer.elapsed();
            if (elapsed >= 1000) {
                m_fpsValue = m_fpsFrameCount * 1000.0 / elapsed;
                m_fpsFrameCount = 0;
                m_fpsTimer.restart();
                if (m_fpsLabel)
                    m_fpsLabel->setText(QStringLiteral("%1 fps").arg(m_fpsValue, 0, 'f', 1));
            }
        }
    });

    // --- Now set style (triggers mapChanged signals) ---
    QString styleJson = QJsonDocument(style).toJson(QJsonDocument::Compact);
    m_map->setStyleJson(styleJson);

    SPDLOG_INFO("[MapWidget] Style set");

    // Trigger initial bounds so settlement/zone loading kicks off
    QTimer::singleShot(500, this, &MapWidget::updateBounds);
}

void MapWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_fpsLabel) {
        // Position in top-right corner
        m_fpsLabel->adjustSize();
        m_fpsLabel->move(width() - m_fpsLabel->width() - 8, 8);
    }
    if (m_legend) {
        // Position in bottom-left corner
        m_legend->adjustSize();
        m_legend->move(8, height() - m_legend->height() - 8);
        m_legend->raise();
    }
}

bool MapWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (obj != m_glWidget) return QWidget::eventFilter(obj, event);

    // ── Escape: exit editing / cancel drawing ──
    if (event->type() == QEvent::KeyPress) {
        auto* e = static_cast<QKeyEvent*>(event);
        if (e->key() == Qt::Key_Escape) {
            if (m_backend.isEditing()) {
                m_backend.disableZoneEditing();
                return true;
            }
            if (m_backend.drawingMode()) {
                m_backend.cancelDrawingSlot();
                return true;
            }
        }
    }

    // ── Vertex drag: intercept BEFORE GLWidget to prevent map panning ──
    if (event->type() == QEvent::MouseButtonPress) {
        auto* e = static_cast<QMouseEvent*>(event);
        if (e->button() == Qt::LeftButton && m_map && m_backend.isEditing()) {
            int vertIdx = hitTestEditVertex(e->position());
            if (vertIdx >= 0) {
                m_draggingVertexIdx = vertIdx;
                return true;  // consume — no pan
            }
            int midIdx = hitTestEditMidpoint(e->position());
            if (midIdx >= 0) {
                m_backend.insertEditingVertex(midIdx);
                m_draggingVertexIdx = midIdx + 1;
                return true;
            }
            // Click outside vertex/midpoint: check if inside editing zone
            auto coord = m_map->coordinateForPixel(e->position());
            if (!m_backend.isPointInZone(coord.first, coord.second)) {
                m_backend.disableZoneEditing();
                // fall through — let GLWidget handle the click normally
            }
        }
    }

    if (event->type() == QEvent::MouseMove) {
        // Vertex dragging
        if (m_draggingVertexIdx >= 0 && m_map) {
            auto* e = static_cast<QMouseEvent*>(event);
            auto coord = m_map->coordinateForPixel(e->position());
            m_backend.moveEditingVertex(m_draggingVertexIdx, coord.first, coord.second);
            return true;  // consume — no pan
        }
        // Block right-button drag to disable map rotation
        auto* e = static_cast<QMouseEvent*>(event);
        if (e->buttons() & Qt::RightButton)
            return true;
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        if (m_draggingVertexIdx >= 0) {
            m_draggingVertexIdx = -1;
            return true;
        }
    }

    // ── Context menu / vertex delete ──
    if (event->type() == QEvent::ContextMenu) {
        auto* e = static_cast<QContextMenuEvent*>(event);
        if (m_map) {
            // In editing mode: right-click on vertex → delete it
            if (m_backend.isEditing()) {
                int vertIdx = hitTestEditVertex(e->pos().toPointF());
                if (vertIdx >= 0) {
                    m_backend.deleteEditingVertex(vertIdx);
                    return true;
                }
            }
            auto c = m_map->coordinateForPixel(e->pos().toPointF());
            m_backend.onContextMenu(c.first, c.second,
                                    e->globalPos().x(), e->globalPos().y());
        }
        return true;
    }

    // ── Double-click (single handler — not via signal, to avoid duplicates) ──
    if (event->type() == QEvent::MouseButtonDblClick) {
        auto* e = static_cast<QMouseEvent*>(event);
        if (m_map && e->button() == Qt::LeftButton) {
            auto coord = m_map->coordinateForPixel(e->position());
            // Check if we'll handle this (zone hit or drawing mode)
            bool consumed = m_backend.drawingMode() ||
                            m_backend.isPointInZone(coord.first, coord.second);
            onMouseDoubleClick(coord.first, coord.second);
            if (consumed) return true;  // prevent GLWidget zoom on zones
        }
        return false;  // let GLWidget zoom on empty area
    }

    return QWidget::eventFilter(obj, event);
}

void MapWidget::onMapChanged(int change)
{
    if (change == QMapLibre::Map::MapChangeRegionDidChange ||
        change == QMapLibre::Map::MapChangeRegionDidChangeAnimated) {
        updateBounds();
    }
}

void MapWidget::updateBounds()
{
    if (!m_map) return;
    QSize sz = m_glWidget->size();
    auto topLeft = m_map->coordinateForPixel({0, 0});
    auto bottomRight = m_map->coordinateForPixel({static_cast<double>(sz.width()),
                                                   static_cast<double>(sz.height())});
    double north = topLeft.first;
    double west = topLeft.second;
    double south = bottomRight.first;
    double east = bottomRight.second;
    m_backend.onBoundsChanged(south, west, north, east);
    m_backend.onZoomChanged(static_cast<int>(m_map->zoom()));
}

void MapWidget::onFollowTick()
{
    if (!m_map || !m_backend.followAircraft() || !m_backend.aircraftVisible()) return;
    auto pos = m_backend.aircraftPosition();
    m_map->setCoordinate({pos.latitude(), pos.longitude()});
}

void MapWidget::onMousePress(double lat, double lon, const QPointF& screenPos, Qt::MouseButton /*button*/)
{
    m_pressScreenPos = screenPos;
    m_dragging = false;
    Q_UNUSED(lat); Q_UNUSED(lon);
}

void MapWidget::onMouseRelease(double lat, double lon, const QPointF& screenPos, Qt::MouseButton /*button*/)
{
    if (!m_dragging) {
        double dx = screenPos.x() - m_pressScreenPos.x();
        double dy = screenPos.y() - m_pressScreenPos.y();
        double dist = std::sqrt(dx * dx + dy * dy);
        if (dist < CLICK_THRESHOLD) {
            m_backend.onMapClick(lat, lon);
        }
    }
}

void MapWidget::onMouseDoubleClick(double lat, double lon)
{
    m_backend.onMapDoubleClick(lat, lon);
}

void MapWidget::onMouseMove(double lat, double lon)
{
    // Detect drag
    if (!m_dragging) {
        QPointF curPos = m_glWidget->mapFromGlobal(QCursor::pos());
        double dx = curPos.x() - m_pressScreenPos.x();
        double dy = curPos.y() - m_pressScreenPos.y();
        if (std::sqrt(dx * dx + dy * dy) > CLICK_THRESHOLD) {
            m_dragging = true;
        }
    }
    m_backend.onMouseMove(lat, lon);
}

int MapWidget::hitTestEditVertex(const QPointF& screenPos)
{
    auto* model = qobject_cast<SimpleVertexModel*>(m_backend.editingVertexModel());
    if (!model || !m_map) return -1;
    auto pts = model->getPoints();
    constexpr double THR_SQ = VERTEX_HIT_THRESHOLD * VERTEX_HIT_THRESHOLD;
    for (int i = 0; i < pts.size(); ++i) {
        QPointF px = m_map->pixelForCoordinate({pts[i].x(), pts[i].y()});
        double dx = screenPos.x() - px.x();
        double dy = screenPos.y() - px.y();
        if (dx * dx + dy * dy < THR_SQ) return i;
    }
    return -1;
}

int MapWidget::hitTestEditMidpoint(const QPointF& screenPos)
{
    auto* model = qobject_cast<SimpleVertexModel*>(m_backend.editingMidpointModel());
    if (!model || !m_map) return -1;
    auto pts = model->getPoints();
    constexpr double THR_SQ = VERTEX_HIT_THRESHOLD * VERTEX_HIT_THRESHOLD;
    for (int i = 0; i < pts.size(); ++i) {
        QPointF px = m_map->pixelForCoordinate({pts[i].x(), pts[i].y()});
        double dx = screenPos.x() - px.x();
        double dy = screenPos.y() - px.y();
        if (dx * dx + dy * dy < THR_SQ) return i;
    }
    return -1;
}

} // namespace vtol
