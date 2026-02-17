"""
QML Map Backend — QObject bridge between Python logic and QML Map.

All data flows through properties (Python→QML) and signals (QML→Python).
"""
import json
import logging
import math
from typing import List, Optional

from PyQt5.QtCore import (
    QObject, pyqtProperty, pyqtSignal, pyqtSlot,
    QAbstractListModel, QModelIndex, Qt, QVariant
)
from PyQt5.QtPositioning import QGeoCoordinate

logger = logging.getLogger(__name__)


# ════════════════════════════════════════════════════════════════
#  List Models
# ════════════════════════════════════════════════════════════════

class WaypointListModel(QAbstractListModel):
    """Waypoint model for QML MapItemView."""
    LatRole = Qt.UserRole + 1
    LonRole = Qt.UserRole + 2
    AltRole = Qt.UserRole + 3
    ActionRole = Qt.UserRole + 4
    WpIndexRole = Qt.UserRole + 5
    WpStateRole = Qt.UserRole + 6

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = []

    def roleNames(self):
        return {
            self.LatRole: b'lat',
            self.LonRole: b'lon',
            self.AltRole: b'altitude',
            self.ActionRole: b'action',
            self.WpIndexRole: b'wpIndex',
            self.WpStateRole: b'wpState',
        }

    def rowCount(self, parent=QModelIndex()):
        return len(self._items)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or index.row() >= len(self._items):
            return QVariant()
        item = self._items[index.row()]
        if role == self.LatRole:
            return item['lat']
        if role == self.LonRole:
            return item['lon']
        if role == self.AltRole:
            return item.get('altitude', 0)
        if role == self.ActionRole:
            return item.get('action', '')
        if role == self.WpIndexRole:
            return index.row()
        if role == self.WpStateRole:
            return item.get('_state', 'future')
        return QVariant()

    def set_waypoints(self, waypoints: list, active_idx: int = 0):
        self.beginResetModel()
        self._items = []
        for i, wp in enumerate(waypoints):
            wp_copy = dict(wp)
            if i < active_idx:
                wp_copy['_state'] = 'past'
            elif i == active_idx:
                wp_copy['_state'] = 'active'
            else:
                wp_copy['_state'] = 'future'
            self._items.append(wp_copy)
        self.endResetModel()

    def update_active(self, active_idx: int):
        for i, item in enumerate(self._items):
            old_state = item['_state']
            if i < active_idx:
                new_state = 'past'
            elif i == active_idx:
                new_state = 'active'
            else:
                new_state = 'future'
            if old_state != new_state:
                item['_state'] = new_state
                idx = self.index(i)
                self.dataChanged.emit(idx, idx, [self.WpStateRole])


class RouteSegmentModel(QAbstractListModel):
    """Route segment model — one row per segment between waypoints."""
    FromLatRole = Qt.UserRole + 1
    FromLonRole = Qt.UserRole + 2
    ToLatRole = Qt.UserRole + 3
    ToLonRole = Qt.UserRole + 4
    SegStateRole = Qt.UserRole + 5

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = []

    def roleNames(self):
        return {
            self.FromLatRole: b'fromLat',
            self.FromLonRole: b'fromLon',
            self.ToLatRole: b'toLat',
            self.ToLonRole: b'toLon',
            self.SegStateRole: b'segState',
        }

    def rowCount(self, parent=QModelIndex()):
        return len(self._items)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or index.row() >= len(self._items):
            return QVariant()
        item = self._items[index.row()]
        if role == self.FromLatRole:
            return item['from_lat']
        if role == self.FromLonRole:
            return item['from_lon']
        if role == self.ToLatRole:
            return item['to_lat']
        if role == self.ToLonRole:
            return item['to_lon']
        if role == self.SegStateRole:
            return item['state']
        return QVariant()

    def set_segments(self, waypoints: list, active_idx: int = 0):
        self.beginResetModel()
        self._items = []
        for i in range(len(waypoints) - 1):
            wp1 = waypoints[i]
            wp2 = waypoints[i + 1]
            if i < active_idx:
                state = 'past'
            elif i == active_idx:
                state = 'active'
            else:
                state = 'future'
            self._items.append({
                'from_lat': wp1['lat'], 'from_lon': wp1['lon'],
                'to_lat': wp2['lat'], 'to_lon': wp2['lon'],
                'state': state,
            })
        self.endResetModel()


class ZoneListModel(QAbstractListModel):
    """Restricted zone model for QML."""
    ZoneIdRole = Qt.UserRole + 1
    ZoneCoordsRole = Qt.UserRole + 2
    ZoneNameRole = Qt.UserRole + 3

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = []

    def roleNames(self):
        return {
            self.ZoneIdRole: b'zoneId',
            self.ZoneCoordsRole: b'zoneCoords',
            self.ZoneNameRole: b'zoneName',
        }

    def rowCount(self, parent=QModelIndex()):
        return len(self._items)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or index.row() >= len(self._items):
            return QVariant()
        item = self._items[index.row()]
        if role == self.ZoneIdRole:
            return item['id']
        if role == self.ZoneCoordsRole:
            return item['points']
        if role == self.ZoneNameRole:
            return item.get('name', '')
        return QVariant()

    def add_zone(self, zone_id: str, points: list, name: str = ""):
        row = self.rowCount()
        self.beginInsertRows(QModelIndex(), row, row)
        self._items.append({'id': zone_id, 'points': points, 'name': name})
        self.endInsertRows()

    def remove_zone(self, zone_id: str):
        for i, item in enumerate(self._items):
            if item['id'] == zone_id:
                self.beginRemoveRows(QModelIndex(), i, i)
                self._items.pop(i)
                self.endRemoveRows()
                return

    def update_zone_points(self, zone_id: str, points: list):
        for i, item in enumerate(self._items):
            if item['id'] == zone_id:
                item['points'] = points
                idx = self.index(i)
                self.dataChanged.emit(idx, idx, [self.ZoneCoordsRole])
                return

    def clear(self):
        if self._items:
            self.beginResetModel()
            self._items = []
            self.endResetModel()


class ZoneBorderModel(QAbstractListModel):
    """Dashed border segments for restricted zones.

    Generates short polyline segments (dashes) along zone polygon edges.
    QML MapPolygon doesn't support dashed borders natively, so we render
    each dash as a separate 2-point MapPolyline.
    """
    FromLatRole = Qt.UserRole + 1
    FromLonRole = Qt.UserRole + 2
    ToLatRole = Qt.UserRole + 3
    ToLonRole = Qt.UserRole + 4

    DASH_DEG = 0.0004   # ~44m at mid-latitudes
    GAP_DEG = 0.0003    # ~33m

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = []  # list of (from_lat, from_lon, to_lat, to_lon)

    def roleNames(self):
        return {
            self.FromLatRole: b'fromLat',
            self.FromLonRole: b'fromLon',
            self.ToLatRole: b'toLat',
            self.ToLonRole: b'toLon',
        }

    def rowCount(self, parent=QModelIndex()):
        return len(self._items)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or index.row() >= len(self._items):
            return QVariant()
        item = self._items[index.row()]
        if role == self.FromLatRole:
            return item[0]
        if role == self.FromLonRole:
            return item[1]
        if role == self.ToLatRole:
            return item[2]
        if role == self.ToLonRole:
            return item[3]
        return QVariant()

    def rebuild(self, zones: list):
        """Recompute dash segments from zone polygon list."""
        self.beginResetModel()
        self._items = []
        step = self.DASH_DEG + self.GAP_DEG
        for zone in zones:
            pts = zone.get('points', [])
            n = len(pts)
            if n < 3:
                continue
            for i in range(n):
                j = (i + 1) % n
                lat1, lon1 = pts[i][0], pts[i][1]
                lat2, lon2 = pts[j][0], pts[j][1]
                dlat = lat2 - lat1
                dlon = lon2 - lon1
                edge_len = (dlat * dlat + dlon * dlon) ** 0.5
                if edge_len < 1e-9:
                    continue
                t = 0.0
                while t < edge_len:
                    t_end = min(t + self.DASH_DEG, edge_len)
                    f1 = t / edge_len
                    f2 = t_end / edge_len
                    self._items.append((
                        lat1 + f1 * dlat, lon1 + f1 * dlon,
                        lat1 + f2 * dlat, lon1 + f2 * dlon,
                    ))
                    t += step
        self.endResetModel()


class SettlementPolyModel(QAbstractListModel):
    """Settlement polygon boundaries (ways/relations)."""
    CoordsRole = Qt.UserRole + 1

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = []
        self._sigs = set()

    def roleNames(self):
        return {self.CoordsRole: b'coords'}

    def rowCount(self, parent=QModelIndex()):
        return len(self._items)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or index.row() >= len(self._items):
            return QVariant()
        if role == self.CoordsRole:
            return self._items[index.row()]
        return QVariant()

    MAX_ITEMS = 80

    def add_polys(self, polys: list):
        new_items = []
        for f in polys:
            coords = f.get('c', [])
            if not coords:
                continue
            sig = f"p_{coords[0][0]:.4f}_{coords[0][1]:.4f}_{len(coords)}"
            if sig not in self._sigs:
                self._sigs.add(sig)
                new_items.append(coords)
        if not new_items:
            return
        # Evict oldest if over capacity
        overflow = len(self._items) + len(new_items) - self.MAX_ITEMS
        if overflow > 0:
            self.beginRemoveRows(QModelIndex(), 0, overflow - 1)
            self._items = self._items[overflow:]
            self.endRemoveRows()
        start = len(self._items)
        self.beginInsertRows(QModelIndex(), start, start + len(new_items) - 1)
        self._items.extend(new_items)
        self.endInsertRows()


class SettlementBorderModel(QAbstractListModel):
    """Dashed border segments for settlement polygons.

    Same approach as ZoneBorderModel: generates short polyline segments (dashes)
    along polygon edges since QML MapPolygon doesn't support dashed borders.
    Uses larger dash/gap to keep segment count manageable with many polygons.
    """
    FromLatRole = Qt.UserRole + 1
    FromLonRole = Qt.UserRole + 2
    ToLatRole = Qt.UserRole + 3
    ToLonRole = Qt.UserRole + 4

    DASH_DEG = 0.0006   # ~67m at mid-latitudes
    GAP_DEG = 0.0004    # ~44m

    MAX_SEGMENTS = 2000

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = []  # list of (from_lat, from_lon, to_lat, to_lon)

    def roleNames(self):
        return {
            self.FromLatRole: b'fromLat',
            self.FromLonRole: b'fromLon',
            self.ToLatRole: b'toLat',
            self.ToLonRole: b'toLon',
        }

    def rowCount(self, parent=QModelIndex()):
        return len(self._items)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or index.row() >= len(self._items):
            return QVariant()
        item = self._items[index.row()]
        if role == self.FromLatRole:
            return item[0]
        if role == self.FromLonRole:
            return item[1]
        if role == self.ToLatRole:
            return item[2]
        if role == self.ToLonRole:
            return item[3]
        return QVariant()

    def rebuild(self, poly_coords_list: list):
        """Recompute dash segments from settlement polygon coordinate lists."""
        self.beginResetModel()
        self._items = []
        step = self.DASH_DEG + self.GAP_DEG
        for coords in poly_coords_list:
            if len(coords) < 3:
                continue
            n = len(coords)
            for i in range(n):
                j = (i + 1) % n
                lat1, lon1 = coords[i][0], coords[i][1]
                lat2, lon2 = coords[j][0], coords[j][1]
                dlat = lat2 - lat1
                dlon = lon2 - lon1
                edge_len = (dlat * dlat + dlon * dlon) ** 0.5
                if edge_len < 1e-9:
                    continue
                t = 0.0
                while t < edge_len:
                    t_end = min(t + self.DASH_DEG, edge_len)
                    f1 = t / edge_len
                    f2 = t_end / edge_len
                    self._items.append((
                        lat1 + f1 * dlat, lon1 + f1 * dlon,
                        lat1 + f2 * dlat, lon1 + f2 * dlon,
                    ))
                    if len(self._items) >= self.MAX_SEGMENTS:
                        self.endResetModel()
                        return
                    t += step
        self.endResetModel()


class SettlementCircleModel(QAbstractListModel):
    """Settlement node fallback (circles)."""
    LatRole = Qt.UserRole + 1
    LonRole = Qt.UserRole + 2
    RadiusRole = Qt.UserRole + 3

    _PLACE_RADIUS = {'city': 5000, 'town': 2000, 'village': 800, 'hamlet': 400}

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = []
        self._sigs = set()

    def roleNames(self):
        return {
            self.LatRole: b'lat',
            self.LonRole: b'lon',
            self.RadiusRole: b'radius',
        }

    def rowCount(self, parent=QModelIndex()):
        return len(self._items)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or index.row() >= len(self._items):
            return QVariant()
        item = self._items[index.row()]
        if role == self.LatRole:
            return item[0]
        if role == self.LonRole:
            return item[1]
        if role == self.RadiusRole:
            return item[2]
        return QVariant()

    MAX_ITEMS = 40

    def add_circles(self, nodes: list):
        new_items = []
        for f in nodes:
            lat = f.get('lat', 0.0)
            lon = f.get('lon', 0.0)
            sig = f"n_{lat:.5f}_{lon:.5f}"
            if sig not in self._sigs:
                self._sigs.add(sig)
                r = self._PLACE_RADIUS.get(f.get('t', ''), 500)
                new_items.append((lat, lon, r))
        if not new_items:
            return
        # Evict oldest if over capacity
        overflow = len(self._items) + len(new_items) - self.MAX_ITEMS
        if overflow > 0:
            self.beginRemoveRows(QModelIndex(), 0, overflow - 1)
            self._items = self._items[overflow:]
            self.endRemoveRows()
        start = len(self._items)
        self.beginInsertRows(QModelIndex(), start, start + len(new_items) - 1)
        self._items.extend(new_items)
        self.endInsertRows()


class ConflictSegmentModel(QAbstractListModel):
    """Conflict segments for route display."""
    FromLatRole = Qt.UserRole + 1
    FromLonRole = Qt.UserRole + 2
    ToLatRole = Qt.UserRole + 3
    ToLonRole = Qt.UserRole + 4

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = []

    def roleNames(self):
        return {
            self.FromLatRole: b'fromLat',
            self.FromLonRole: b'fromLon',
            self.ToLatRole: b'toLat',
            self.ToLonRole: b'toLon',
        }

    def rowCount(self, parent=QModelIndex()):
        return len(self._items)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or index.row() >= len(self._items):
            return QVariant()
        item = self._items[index.row()]
        if role == self.FromLatRole:
            return item['from_lat']
        if role == self.FromLonRole:
            return item['from_lon']
        if role == self.ToLatRole:
            return item['to_lat']
        if role == self.ToLonRole:
            return item['to_lon']
        return QVariant()

    def set_conflicts(self, conflicts: list, waypoints: list):
        self.beginResetModel()
        self._items = []
        for c in conflicts:
            fi = c['from_idx']
            ti = c['to_idx']
            if fi < len(waypoints) and ti < len(waypoints):
                self._items.append({
                    'from_lat': waypoints[fi]['lat'],
                    'from_lon': waypoints[fi]['lon'],
                    'to_lat': waypoints[ti]['lat'],
                    'to_lon': waypoints[ti]['lon'],
                })
        self.endResetModel()

    def clear(self):
        if self._items:
            self.beginResetModel()
            self._items = []
            self.endResetModel()


class ConflictPointModel(QAbstractListModel):
    """Warning points for route conflicts."""
    LatRole = Qt.UserRole + 1
    LonRole = Qt.UserRole + 2
    ReasonRole = Qt.UserRole + 3

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = []

    def roleNames(self):
        return {
            self.LatRole: b'lat',
            self.LonRole: b'lon',
            self.ReasonRole: b'reason',
        }

    def rowCount(self, parent=QModelIndex()):
        return len(self._items)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or index.row() >= len(self._items):
            return QVariant()
        item = self._items[index.row()]
        if role == self.LatRole:
            return item['lat']
        if role == self.LonRole:
            return item['lon']
        if role == self.ReasonRole:
            return item['reason']
        return QVariant()

    def set_conflicts(self, conflicts: list, waypoints: list):
        self.beginResetModel()
        self._items = []
        for c in conflicts:
            fi = c.get('from_idx', -1)
            ti = c.get('to_idx', -1)
            if fi < 0 or ti < 0:
                continue
            if fi >= len(waypoints) or ti >= len(waypoints):
                continue
            wp1 = waypoints[fi]
            wp2 = waypoints[ti]
            self._items.append({
                'lat': (wp1['lat'] + wp2['lat']) / 2.0,
                'lon': (wp1['lon'] + wp2['lon']) / 2.0,
                'reason': c.get('reason', 'Конфликт маршрута'),
            })
        self.endResetModel()

    def set_points(self, points: list):
        self.beginResetModel()
        self._items = list(points)
        self.endResetModel()

    def clear(self):
        if self._items:
            self.beginResetModel()
            self._items = []
            self.endResetModel()


class SimpleVertexModel(QAbstractListModel):
    """Vertex model for drawing / editing overlays."""
    LatRole = Qt.UserRole + 1
    LonRole = Qt.UserRole + 2
    VertexIndexRole = Qt.UserRole + 3
    MidpointIndexRole = Qt.UserRole + 4

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = []

    def roleNames(self):
        return {
            self.LatRole: b'lat',
            self.LonRole: b'lon',
            self.VertexIndexRole: b'vertexIndex',
            self.MidpointIndexRole: b'midpointIndex',
        }

    def rowCount(self, parent=QModelIndex()):
        return len(self._items)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or index.row() >= len(self._items):
            return QVariant()
        item = self._items[index.row()]
        if role == self.LatRole:
            return item[0]
        if role == self.LonRole:
            return item[1]
        if role == self.VertexIndexRole:
            return index.row()
        if role == self.MidpointIndexRole:
            return index.row()
        return QVariant()

    def set_points(self, points: list):
        """points: list of [lat, lon]"""
        self.beginResetModel()
        self._items = list(points)
        self.endResetModel()

    def add_point(self, lat: float, lon: float):
        row = len(self._items)
        self.beginInsertRows(QModelIndex(), row, row)
        self._items.append([lat, lon])
        self.endInsertRows()

    def update_point(self, idx: int, lat: float, lon: float):
        if 0 <= idx < len(self._items):
            self._items[idx] = [lat, lon]
            midx = self.index(idx)
            self.dataChanged.emit(midx, midx, [self.LatRole, self.LonRole])

    def remove_point(self, idx: int):
        if 0 <= idx < len(self._items):
            self.beginRemoveRows(QModelIndex(), idx, idx)
            self._items.pop(idx)
            self.endRemoveRows()

    def clear(self):
        if self._items:
            self.beginResetModel()
            self._items = []
            self.endResetModel()

    def get_points(self):
        return [list(p) for p in self._items]


# ════════════════════════════════════════════════════════════════
#  QML Map Backend
# ════════════════════════════════════════════════════════════════

class QmlMapBackend(QObject):
    """
    Central QObject that bridges Python ↔ QML.

    Python sets properties → QML auto-updates via binding.
    QML calls slots → Python emits signals to the widget.
    """

    # ── Signals: property change notifications ──
    aircraftPositionChanged = pyqtSignal()
    aircraftHeadingChanged = pyqtSignal()
    aircraftVisibleChanged = pyqtSignal()
    homePositionChanged = pyqtSignal()
    homeVisibleChanged = pyqtSignal()
    trackPathChanged = pyqtSignal()
    trackCoordinateAdded = pyqtSignal(float, float)
    activeWpPositionChanged = pyqtSignal()
    followAircraftChanged = pyqtSignal()
    tileServerUrlChanged = pyqtSignal()

    showTrackChanged = pyqtSignal()
    showWaypointsChanged = pyqtSignal()
    showZonesChanged = pyqtSignal()
    showSettlementsChanged = pyqtSignal()
    showLayerPanelChanged = pyqtSignal()
    fpsTextChanged = pyqtSignal()

    drawingModeChanged = pyqtSignal()
    drawingPathChanged = pyqtSignal()
    leftClickModeChanged = pyqtSignal()
    avoidancePathChanged = pyqtSignal()
    plannedDirectPathChanged = pyqtSignal()

    # ── Signals: QML → Python communication ──
    mapClicked = pyqtSignal(float, float)
    mapDoubleClicked = pyqtSignal(float, float)
    contextMenuRequested = pyqtSignal(float, float, int, int)
    boundsChanged = pyqtSignal(float, float, float, float)
    mouseMoved = pyqtSignal(float, float)
    zoomChanged = pyqtSignal(int)

    # Map control signals (Python → QML)
    mapCenterRequested = pyqtSignal(float, float)
    zoomRequested = pyqtSignal(int)

    def __init__(self, parent=None):
        super().__init__(parent)

        # Aircraft state
        self._aircraftPosition = QGeoCoordinate()
        self._aircraftHeading = 0.0
        self._aircraftVisible = False

        # Home
        self._homePosition = QGeoCoordinate()
        self._homeVisible = False

        # Track
        self._trackPath = []
        self._trackPoints = []  # raw [lat,lon] list for decimation
        self._lastTrackLat = 0.0
        self._lastTrackLon = 0.0
        self._track_max_length = 9999  # настраивается через set_track_max_length

        # Active waypoint
        self._activeWpPosition = QGeoCoordinate()

        # Follow mode
        self._followAircraft = False

        # Tile server — local proxy → Google Hybrid (satellite + roads)
        from src.gui.tile_proxy import start_tile_proxy
        proxy_port = start_tile_proxy()
        self._tileServerUrl = f"http://127.0.0.1:{proxy_port}/"

        # Layer visibility
        self._showTrack = True
        self._showWaypoints = True
        self._showZones = True
        self._showSettlements = True
        self._showLayerPanel = True

        # FPS counter
        self._fpsText = ""
        self._frameCount = 0
        self._fpsTimestamp = 0.0
        self._fpsUpdateMs = 0.0
        self._fpsDrops = 0

        # Drawing mode
        self._drawingMode = False
        self._drawingPath = []

        # Left-click mode (position set, home set — blocks map pan)
        self._leftClickMode = False

        # Avoidance path
        self._avoidancePath = []
        self._plannedDirectPath = []

        # Editing mode
        self._editingZoneId = ""

        # Models
        self._waypointModel = WaypointListModel(self)
        self._routeSegmentModel = RouteSegmentModel(self)
        self._zoneModel = ZoneListModel(self)
        self._zoneBorderModel = ZoneBorderModel(self)
        self._settlementPolyModel = SettlementPolyModel(self)
        self._settlementCircleModel = SettlementCircleModel(self)
        self._conflictModel = ConflictSegmentModel(self)
        self._conflictPointModel = ConflictPointModel(self)
        self._settlementBorderModel = SettlementBorderModel(self)
        self._drawingVertexModel = SimpleVertexModel(self)
        self._editingVertexModel = SimpleVertexModel(self)
        self._editingMidpointModel = SimpleVertexModel(self)

        # Store waypoints for conflict resolution
        self._currentWaypoints = []

    # ════════════════════ Properties ════════════════════

    @pyqtProperty(QGeoCoordinate, notify=aircraftPositionChanged)
    def aircraftPosition(self):
        return self._aircraftPosition

    @pyqtProperty(float, notify=aircraftHeadingChanged)
    def aircraftHeading(self):
        return self._aircraftHeading

    @pyqtProperty(bool, notify=aircraftVisibleChanged)
    def aircraftVisible(self):
        return self._aircraftVisible

    @pyqtProperty(QGeoCoordinate, notify=homePositionChanged)
    def homePosition(self):
        return self._homePosition

    @pyqtProperty(bool, notify=homeVisibleChanged)
    def homeVisible(self):
        return self._homeVisible

    @pyqtProperty('QVariantList', notify=trackPathChanged)
    def trackPath(self):
        return self._trackPath

    @pyqtProperty(QGeoCoordinate, notify=activeWpPositionChanged)
    def activeWpPosition(self):
        return self._activeWpPosition

    @pyqtProperty(bool, notify=followAircraftChanged)
    def followAircraft(self):
        return self._followAircraft

    @followAircraft.setter
    def followAircraft(self, val):
        if self._followAircraft != val:
            self._followAircraft = val
            self.followAircraftChanged.emit()

    @pyqtProperty(str, notify=tileServerUrlChanged)
    def tileServerUrl(self):
        return self._tileServerUrl

    @pyqtProperty(bool, notify=showTrackChanged)
    def showTrack(self):
        return self._showTrack

    @showTrack.setter
    def showTrack(self, val):
        if self._showTrack != val:
            self._showTrack = val
            self.showTrackChanged.emit()

    @pyqtProperty(bool, notify=showWaypointsChanged)
    def showWaypoints(self):
        return self._showWaypoints

    @showWaypoints.setter
    def showWaypoints(self, val):
        if self._showWaypoints != val:
            self._showWaypoints = val
            self.showWaypointsChanged.emit()

    @pyqtProperty(bool, notify=showZonesChanged)
    def showZones(self):
        return self._showZones

    @showZones.setter
    def showZones(self, val):
        if self._showZones != val:
            self._showZones = val
            self.showZonesChanged.emit()

    @pyqtProperty(bool, notify=showSettlementsChanged)
    def showSettlements(self):
        return self._showSettlements

    @showSettlements.setter
    def showSettlements(self, val):
        if self._showSettlements != val:
            self._showSettlements = val
            self.showSettlementsChanged.emit()

    @pyqtProperty(bool, notify=showLayerPanelChanged)
    def showLayerPanel(self):
        return self._showLayerPanel

    @pyqtProperty(str, notify=fpsTextChanged)
    def fpsText(self):
        return self._fpsText

    @pyqtProperty(bool, notify=drawingModeChanged)
    def drawingMode(self):
        return self._drawingMode

    @pyqtProperty('QVariantList', notify=drawingPathChanged)
    def drawingPath(self):
        return self._drawingPath

    @pyqtProperty(bool, notify=leftClickModeChanged)
    def leftClickMode(self):
        return self._leftClickMode

    @leftClickMode.setter
    def leftClickMode(self, val):
        if self._leftClickMode != val:
            self._leftClickMode = val
            self.leftClickModeChanged.emit()

    # ── Models (read-only properties for QML) ──

    @pyqtProperty(QObject, constant=True)
    def waypointModel(self):
        return self._waypointModel

    @pyqtProperty(QObject, constant=True)
    def routeSegmentModel(self):
        return self._routeSegmentModel

    @pyqtProperty(QObject, constant=True)
    def zoneModel(self):
        return self._zoneModel

    @pyqtProperty(QObject, constant=True)
    def zoneBorderModel(self):
        return self._zoneBorderModel

    @pyqtProperty(QObject, constant=True)
    def settlementPolyModel(self):
        return self._settlementPolyModel

    @pyqtProperty(QObject, constant=True)
    def settlementCircleModel(self):
        return self._settlementCircleModel

    @pyqtProperty(QObject, constant=True)
    def settlementBorderModel(self):
        return self._settlementBorderModel

    @pyqtProperty(QObject, constant=True)
    def conflictModel(self):
        return self._conflictModel

    @pyqtProperty(QObject, constant=True)
    def conflictPointModel(self):
        return self._conflictPointModel

    @pyqtProperty(QObject, constant=True)
    def drawingVertexModel(self):
        return self._drawingVertexModel

    @pyqtProperty(QObject, constant=True)
    def editingVertexModel(self):
        return self._editingVertexModel

    @pyqtProperty(QObject, constant=True)
    def editingMidpointModel(self):
        return self._editingMidpointModel

    @pyqtProperty('QVariantList', notify=avoidancePathChanged)
    def avoidancePath(self):
        return self._avoidancePath

    @pyqtProperty('QVariantList', notify=plannedDirectPathChanged)
    def plannedDirectPath(self):
        return self._plannedDirectPath

    # ════════════════════ Python API (called by QmlMapWidget) ════════════════════

    def update_aircraft(self, lat: float, lon: float, heading: float):
        pos = QGeoCoordinate(lat, lon)
        changed = False

        if self._aircraftPosition != pos:
            self._aircraftPosition = pos
            self.aircraftPositionChanged.emit()
            changed = True

            # Track: distance-based throttle + incremental append
            dlat = lat - self._lastTrackLat
            dlon = lon - self._lastTrackLon
            if dlat * dlat + dlon * dlon > 2e-10:  # ~1.5m
                self._lastTrackLat = lat
                self._lastTrackLon = lon
                self._trackPoints.append([lat, lon])
                self._trackPath.append(QGeoCoordinate(lat, lon))
                max_len = self._track_max_length
                if len(self._trackPoints) > max_len:
                    trim = max(max_len // 10, 50)  # удаляем 10% за раз
                    self._trackPoints = self._trackPoints[trim:]
                    self._trackPath = self._trackPath[trim:]
                    self.trackPathChanged.emit()  # full rebuild after trim
                else:
                    self.trackCoordinateAdded.emit(lat, lon)  # incremental

        if self._aircraftHeading != heading:
            self._aircraftHeading = heading
            self.aircraftHeadingChanged.emit()
            changed = True

        if not self._aircraftVisible:
            self._aircraftVisible = True
            self.aircraftVisibleChanged.emit()

    def set_aircraft_position(self, lat: float, lon: float):
        self._aircraftPosition = QGeoCoordinate(lat, lon)
        self.aircraftPositionChanged.emit()
        if not self._aircraftVisible:
            self._aircraftVisible = True
            self.aircraftVisibleChanged.emit()

    def set_track_max_length(self, length: int):
        self._track_max_length = max(100, length)

    def clear_track(self):
        self._trackPoints.clear()
        self._trackPath = []
        self._lastTrackLat = 0.0
        self._lastTrackLon = 0.0
        self.trackPathChanged.emit()

    def set_home(self, lat: float, lon: float):
        self._homePosition = QGeoCoordinate(lat, lon)
        self._homeVisible = True
        self.homePositionChanged.emit()
        self.homeVisibleChanged.emit()

    def set_waypoints(self, waypoints: list, active_idx: int = 0):
        self._currentWaypoints = waypoints
        self._waypointModel.set_waypoints(waypoints, active_idx)
        self._routeSegmentModel.set_segments(waypoints, active_idx)
        if waypoints and 0 <= active_idx < len(waypoints):
            wp = waypoints[active_idx]
            self._activeWpPosition = QGeoCoordinate(wp['lat'], wp['lon'])
        else:
            self._activeWpPosition = QGeoCoordinate()
        self.activeWpPositionChanged.emit()

    def update_active_waypoint(self, index: int):
        self._waypointModel.update_active(index)
        wps = self._currentWaypoints
        if wps and 0 <= index < len(wps):
            self._activeWpPosition = QGeoCoordinate(wps[index]['lat'], wps[index]['lon'])
        else:
            self._activeWpPosition = QGeoCoordinate()
        self.activeWpPositionChanged.emit()

    def set_follow_mode(self, enabled: bool):
        self.followAircraft = enabled

    def center_on(self, lat: float, lon: float):
        self.mapCenterRequested.emit(lat, lon)

    def set_tile_server(self, url: str):
        if self._tileServerUrl != url:
            self._tileServerUrl = url
            self.tileServerUrlChanged.emit()

    def set_fps_text(self, text: str):
        if self._fpsText != text:
            self._fpsText = text
            self.fpsTextChanged.emit()

    def tick_fps(self, elapsed_ms: float):
        """Called from update_display every 100ms. Tracks update metrics."""
        import time
        self._frameCount += 1
        self._fpsUpdateMs = elapsed_ms
        if elapsed_ms > 16:
            self._fpsDrops += 1

        now = time.monotonic()
        if self._fpsTimestamp == 0.0:
            self._fpsTimestamp = now
            return
        dt = now - self._fpsTimestamp
        if dt >= 1.0:
            stl = len(self._settlementPolyModel._items) + len(self._settlementCircleModel._items)
            text = f"upd: {self._fpsUpdateMs:.1f}ms | stl: {stl}"
            self._frameCount = 0
            self._fpsDrops = 0
            self._fpsTimestamp = now
            self.set_fps_text(text)

    # ── Zones ──

    def add_zone(self, zone_id: str, points: list, name: str = ""):
        self._zoneModel.add_zone(zone_id, points, name)
        self._zoneBorderModel.rebuild(self._zoneModel._items)

    def remove_zone(self, zone_id: str):
        self._zoneModel.remove_zone(zone_id)
        self._zoneBorderModel.rebuild(self._zoneModel._items)

    def load_all_zones(self, zones: list):
        self._zoneModel.clear()
        for z in zones:
            self._zoneModel.add_zone(z['id'], z['points'], z.get('name', ''))
        self._zoneBorderModel.rebuild(self._zoneModel._items)

    # ── Settlements ──

    def add_settlement_features(self, features: list):
        polys = [f for f in features if f.get('F') == 'p']
        nodes = [f for f in features if f.get('F') == 'n']
        if polys:
            self._settlementPolyModel.add_polys(polys)
            # Rebuild dashed borders from all polygon coords
            self._settlementBorderModel.rebuild(self._settlementPolyModel._items)
        if nodes:
            self._settlementCircleModel.add_circles(nodes)

    # ── Conflicts / Avoidance ──

    def set_route_conflicts(self, conflicts: list):
        self._conflictModel.set_conflicts(conflicts, self._currentWaypoints)
        self._conflictPointModel.set_conflicts(conflicts, self._currentWaypoints)

    def clear_route_conflicts(self):
        self._conflictModel.clear()
        self._conflictPointModel.clear()
        self.set_planned_direct_path([])
        self.set_avoidance_path([])

    def set_avoidance_path(self, points: list):
        self._avoidancePath = [QGeoCoordinate(p['lat'], p['lon']) for p in points]
        # Manual notify since we can't use proper notify on avoidancePath
        self.avoidancePathChanged.emit()

    def set_planned_direct_path(self, points: list):
        self._plannedDirectPath = [QGeoCoordinate(p['lat'], p['lon']) for p in points]
        self.plannedDirectPathChanged.emit()

    def set_conflict_points(self, points: list):
        self._conflictPointModel.set_points(points)

    # ── Layer visibility ──

    def set_layer_visibility(self, layer_name: str, visible: bool):
        mapping = {
            'track': 'showTrack',
            'waypoints': 'showWaypoints',
            'restricted': 'showZones',
            'settlements': 'showSettlements',
        }
        prop = mapping.get(layer_name)
        if prop:
            setattr(self, prop, visible)

    # ── Drawing mode ──

    def start_drawing(self):
        self._drawingMode = True
        self._drawingPath = []
        self._drawingVertexModel.clear()
        self.drawingModeChanged.emit()
        self.drawingPathChanged.emit()

    def cancel_drawing(self):
        self._drawingMode = False
        self._drawingPath = []
        self._drawingVertexModel.clear()
        self.drawingModeChanged.emit()
        self.drawingPathChanged.emit()

    def _finish_drawing(self):
        points = self._drawingVertexModel.get_points()
        self._drawingMode = False
        self._drawingPath = []
        self._drawingVertexModel.clear()
        self.drawingModeChanged.emit()
        self.drawingPathChanged.emit()
        return points

    # ── Editing mode ──

    def enable_zone_editing(self, zone_id: str):
        self._editingZoneId = zone_id
        # Find zone points
        for item in self._zoneModel._items:
            if item['id'] == zone_id:
                pts = item['points']
                self._editingVertexModel.set_points(pts)
                self._rebuild_midpoints(pts)
                return

    def disable_zone_editing(self):
        self._editingZoneId = ""
        self._editingVertexModel.clear()
        self._editingMidpointModel.clear()
        self._zoneBorderModel.rebuild(self._zoneModel._items)

    def _rebuild_midpoints(self, pts):
        mids = []
        for i in range(len(pts)):
            j = (i + 1) % len(pts)
            mids.append([
                (pts[i][0] + pts[j][0]) / 2,
                (pts[i][1] + pts[j][1]) / 2,
            ])
        self._editingMidpointModel.set_points(mids)

    # ════════════════════ QML Slots (called from QML) ════════════════════

    @pyqtSlot(float, float)
    def onMapClick(self, lat, lon):
        if self._drawingMode:
            self._add_drawing_vertex(lat, lon)
        else:
            self.mapClicked.emit(lat, lon)

    @pyqtSlot(float, float)
    def onMapDoubleClick(self, lat, lon):
        if self._drawingMode:
            points = self._finish_drawing()
            if len(points) >= 3:
                self.drawingFinished.emit(json.dumps(points))
            else:
                self.drawingCancelled.emit()
        else:
            zone_id = self._hit_test_zone(lat, lon)
            if zone_id:
                self.zoneDoubleClicked.emit(zone_id)
            else:
                self.mapDoubleClicked.emit(lat, lon)

    @pyqtSlot(float, float, int, int)
    def onContextMenu(self, lat, lon, sx, sy):
        if self._drawingMode:
            return
        zone_id = self._hit_test_zone(lat, lon)
        if zone_id:
            self.zoneContextMenuRequested.emit(zone_id, sx, sy)
        else:
            self.contextMenuRequested.emit(lat, lon, sx, sy)

    @pyqtSlot(float, float, float, float)
    def onBoundsChanged(self, south, west, north, east):
        if math.isnan(south) or math.isnan(west) or math.isnan(north) or math.isnan(east):
            return
        self.boundsChanged.emit(south, west, north, east)

    @pyqtSlot(float, float)
    def onMouseMove(self, lat, lon):
        self.mouseMoved.emit(lat, lon)

    @pyqtSlot(int)
    def onZoomChanged(self, zoom):
        self.zoomChanged.emit(zoom)

    @pyqtSlot(int, float, float)
    def moveEditingVertex(self, idx, lat, lon):
        self._editingVertexModel.update_point(idx, lat, lon)
        pts = self._editingVertexModel.get_points()
        self._rebuild_midpoints(pts)
        if self._editingZoneId:
            self._zoneModel.update_zone_points(self._editingZoneId, pts)
            self.zoneVerticesUpdated.emit(self._editingZoneId, pts)

    @pyqtSlot(int)
    def deleteEditingVertex(self, idx):
        if len(self._editingVertexModel._items) <= 3:
            return
        self._editingVertexModel.remove_point(idx)
        pts = self._editingVertexModel.get_points()
        self._rebuild_midpoints(pts)
        if self._editingZoneId:
            self._zoneModel.update_zone_points(self._editingZoneId, pts)
            self.zoneVerticesUpdated.emit(self._editingZoneId, pts)

    @pyqtSlot(int)
    def insertEditingVertex(self, midIdx):
        mids = self._editingMidpointModel.get_points()
        if 0 <= midIdx < len(mids):
            lat, lon = mids[midIdx]
            insert_at = midIdx + 1
            pts = self._editingVertexModel.get_points()
            pts.insert(insert_at, [lat, lon])
            self._editingVertexModel.set_points(pts)
            self._rebuild_midpoints(pts)
            if self._editingZoneId:
                self._zoneModel.update_zone_points(self._editingZoneId, pts)
                self.zoneVerticesUpdated.emit(self._editingZoneId, pts)

    @pyqtSlot()
    def cancelDrawing(self):
        """QML-callable slot for Escape key cancellation."""
        if self._drawingMode:
            self.cancel_drawing()
            self.drawingCancelled.emit()

    # ── Zone hit-testing ──

    def _hit_test_zone(self, lat, lon):
        """Check if point is inside any visible zone polygon. Returns zone_id or None."""
        if not self._showZones:
            return None
        for item in self._zoneModel._items:
            if self._point_in_polygon(lat, lon, item['points']):
                return item['id']
        return None

    @staticmethod
    def _point_in_polygon(lat, lon, polygon):
        """Ray-casting point-in-polygon test."""
        n = len(polygon)
        if n < 3:
            return False
        inside = False
        j = n - 1
        for i in range(n):
            lat_i, lon_i = polygon[i][0], polygon[i][1]
            lat_j, lon_j = polygon[j][0], polygon[j][1]
            if ((lat_i > lat) != (lat_j > lat)) and \
               (lon < (lon_j - lon_i) * (lat - lat_i) / (lat_j - lat_i) + lon_i):
                inside = not inside
            j = i
        return inside

    # ── Drawing helpers ──

    def _add_drawing_vertex(self, lat, lon):
        pts = self._drawingVertexModel.get_points()
        # Check snap to first point
        if len(pts) >= 3:
            d = ((lat - pts[0][0]) ** 2 + (lon - pts[0][1]) ** 2) ** 0.5
            if d < 0.0002:  # ~20m snap threshold
                points = self._finish_drawing()
                self.drawingFinished.emit(json.dumps(points))
                return

        self._drawingVertexModel.add_point(lat, lon)
        # Update preview polyline
        pts = self._drawingVertexModel.get_points()
        self._drawingPath = [QGeoCoordinate(p[0], p[1]) for p in pts]
        self.drawingPathChanged.emit()

    # ── Signals for zone drawing/editing results ──
    drawingFinished = pyqtSignal(str)  # JSON points
    drawingCancelled = pyqtSignal()
    zoneContextMenuRequested = pyqtSignal(str, int, int)
    zoneDoubleClicked = pyqtSignal(str)
    zoneEditingFinished = pyqtSignal()
    zoneVerticesUpdated = pyqtSignal(str, list)
