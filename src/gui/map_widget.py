import json
import math
import os
import threading
import time
import urllib.request
import urllib.parse

import qtawesome as qta
from PyQt5.QtWidgets import QWidget, QVBoxLayout, QMenu, QAction
from PyQt5.QtWebEngineWidgets import QWebEngineView
from PyQt5.QtWebChannel import QWebChannel
from PyQt5.QtCore import QObject, pyqtSlot, pyqtSignal, QThread
from PyQt5.QtGui import QCursor, QColor

from src.gui.theme import Colors


_TILE_GRID = 0.1  # ~10 km tile grid
_CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'cache', 'settlements_v5')
_DP_TOLERANCE = 0.0005  # ~55 m — Douglas-Peucker simplification

_OVERPASS_ENDPOINTS = [
    'https://overpass-api.de/api/interpreter',
    'https://overpass.kumi.systems/api/interpreter',
]


class _FetchWorker(QThread):
    """Batch-fetches settlements for all queued tiles in one Overpass request."""
    tile_loaded = pyqtSignal(str, list)

    def __init__(self, loader):
        super().__init__()
        self._loader = loader

    def run(self):
        while True:
            with self._loader._lock:
                if not self._loader._queue:
                    return
                tiles = list(self._loader._queue)
                self._loader._queue.clear()

            s = min(t[0] for t in tiles)
            w = min(t[1] for t in tiles)
            n = max(t[2] for t in tiles)
            e = max(t[3] for t in tiles)

            query = (
                f'[out:json][timeout:30];('
                f'way["place"~"^(city|town|village|hamlet)$"]({s},{w},{n},{e});'
                f'relation["place"~"^(city|town|village|hamlet)$"]({s},{w},{n},{e});'
                f'node["place"~"^(city|town|village|hamlet)$"]({s},{w},{n},{e});'
                f');out geom;'
            )

            last_err = None
            for attempt in range(len(_OVERPASS_ENDPOINTS)):
                try:
                    ep = _OVERPASS_ENDPOINTS[attempt]
                    post_data = urllib.parse.urlencode({'data': query}).encode()
                    req = urllib.request.Request(ep, data=post_data)
                    with urllib.request.urlopen(req, timeout=30) as resp:
                        raw = json.loads(resp.read())

                    elements = raw.get('elements', [])
                    all_features = SettlementLoader._process_elements(elements)
                    tile_map = self._distribute(all_features, tiles)

                    with self._loader._lock:
                        for key, features in tile_map.items():
                            self._loader._cache[key] = features
                            self._loader._emitted_keys.add(key)

                    for key, features in tile_map.items():
                        SettlementLoader._save_tile(key, features)
                        self.tile_loaded.emit(key, features)

                    last_err = None
                    break
                except Exception as exc:
                    last_err = exc
                    if attempt < len(_OVERPASS_ENDPOINTS) - 1:
                        time.sleep(2)

            if last_err:
                print(f"[FIX] Settlement batch failed: {last_err}")
                with self._loader._lock:
                    for t in tiles:
                        self._loader._loaded_keys.discard(SettlementLoader._key(t))

    @staticmethod
    def _distribute(features, tiles):
        """Assign features to overlapping tiles by bbox."""
        tile_map = {}
        for t in tiles:
            tile_map[SettlementLoader._key(t)] = []
        for f in features:
            if f['F'] == 'p':
                lats = [c[0] for c in f['c']]
                lons = [c[1] for c in f['c']]
                fb = (min(lats), min(lons), max(lats), max(lons))
            else:
                fb = (f['lat'], f['lon'], f['lat'], f['lon'])
            for t in tiles:
                if not (fb[2] < t[0] or fb[0] > t[2] or fb[3] < t[1] or fb[1] > t[3]):
                    tile_map[SettlementLoader._key(t)].append(f)
        return tile_map


class SettlementLoader(QObject):
    """Batch-fetches settlement boundaries with tile caching."""
    tile_loaded = pyqtSignal(str, list)

    def __init__(self):
        super().__init__()
        self._queue = []
        self._loaded_keys = set()
        self._emitted_keys = set()
        self._cache = {}
        self._lock = threading.Lock()
        os.makedirs(_CACHE_DIR, exist_ok=True)
        self._load_disk_cache()
        self._worker = _FetchWorker(self)
        self._worker.tile_loaded.connect(self.tile_loaded)
        self._worker.finished.connect(self._on_worker_done)

    def request(self, south, west, north, east):
        tiles = self._grid_tiles(south, west, north, east)
        added = False
        with self._lock:
            for t in tiles:
                key = self._key(t)
                if key not in self._loaded_keys:
                    self._loaded_keys.add(key)
                    self._queue.append(t)
                    added = True
        if added:
            self._kick_worker()

    def _kick_worker(self):
        if not self._worker.isRunning():
            self._worker.start()

    def _on_worker_done(self):
        with self._lock:
            has_work = bool(self._queue)
        if has_work:
            self._worker.start()

    @staticmethod
    def _process_elements(elements):
        """Extract polygons from ways/relations, fall back to nodes for the rest."""
        polys = []  # {F:'p', c:[[lat,lon],...], t:'village'}
        nodes = []  # {F:'n', lat, lon, t}
        poly_bboxes = []  # (min_lat, min_lon, max_lat, max_lon)

        for el in elements:
            place = el.get('tags', {}).get('place', '')
            if not place:
                continue

            if el['type'] == 'way' and 'geometry' in el:
                coords = [[p['lat'], p['lon']] for p in el['geometry']]
                if len(coords) < 3:
                    continue
                lats = [c[0] for c in coords]
                lons = [c[1] for c in coords]
                span = max(max(lats) - min(lats), max(lons) - min(lons))
                tol = _DP_TOLERANCE
                if span > 0.5:
                    tol = 0.005
                elif span > 0.2:
                    tol = 0.002
                elif span > 0.1:
                    tol = 0.001
                simplified = SettlementLoader._simplify_dp(coords, tol)
                polys.append({'F': 'p', 'c': simplified, 't': place})
                poly_bboxes.append((min(lats), min(lons), max(lats), max(lons)))

            elif el['type'] == 'relation' and 'members' in el:
                ring = SettlementLoader._merge_relation(el['members'])
                if ring and len(ring) >= 3:
                    lats = [c[0] for c in ring]
                    lons = [c[1] for c in ring]
                    span = max(max(lats) - min(lats), max(lons) - min(lons))
                    tol = _DP_TOLERANCE
                    if span > 0.5:
                        tol = 0.005
                    elif span > 0.2:
                        tol = 0.002
                    elif span > 0.1:
                        tol = 0.001
                    simplified = SettlementLoader._simplify_dp(ring, tol)
                    polys.append({'F': 'p', 'c': simplified, 't': place})
                    poly_bboxes.append((min(lats), min(lons), max(lats), max(lons)))

            elif el['type'] == 'node':
                nodes.append({'F': 'n', 'lat': el['lat'], 'lon': el['lon'], 't': place})

        # Filter out nodes that already have a polygon (bbox check)
        result = list(polys)
        for nd in nodes:
            covered = False
            for (mn_la, mn_lo, mx_la, mx_lo) in poly_bboxes:
                if mn_la <= nd['lat'] <= mx_la and mn_lo <= nd['lon'] <= mx_lo:
                    covered = True
                    break
            if not covered:
                result.append(nd)
        return result

    @staticmethod
    def _merge_relation(members):
        """Merge outer way geometries of a relation into a single ring."""
        segments = []
        for m in members:
            if m.get('type') != 'way':
                continue
            role = m.get('role', 'outer')
            if role not in ('outer', ''):
                continue
            geom = m.get('geometry')
            if not geom:
                continue
            seg = [[p['lat'], p['lon']] for p in geom]
            if seg:
                segments.append(seg)
        if not segments:
            return []
        # Try to chain segments end-to-end
        ring = list(segments[0])
        remaining = segments[1:]
        max_iter = len(remaining) * 2
        i = 0
        while remaining and i < max_iter:
            i += 1
            matched = False
            for idx, seg in enumerate(remaining):
                # ring end -> seg start
                if _close(ring[-1], seg[0]):
                    ring.extend(seg[1:])
                    remaining.pop(idx)
                    matched = True
                    break
                # ring end -> seg end (reversed)
                if _close(ring[-1], seg[-1]):
                    ring.extend(reversed(seg[:-1]))
                    remaining.pop(idx)
                    matched = True
                    break
            if not matched:
                break
        return ring

    @staticmethod
    def _simplify_dp(coords, tolerance):
        """Douglas-Peucker polyline simplification."""
        if len(coords) <= 4:
            return [[round(c[0], 5), round(c[1], 5)] for c in coords]

        def _perp_dist(p, a, b):
            dx, dy = b[0] - a[0], b[1] - a[1]
            if dx == 0 and dy == 0:
                return math.hypot(p[0] - a[0], p[1] - a[1])
            t = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / (dx * dx + dy * dy)
            t = max(0, min(1, t))
            proj = [a[0] + t * dx, a[1] + t * dy]
            return math.hypot(p[0] - proj[0], p[1] - proj[1])

        def _dp(pts, tol):
            if len(pts) <= 2:
                return pts
            max_d, max_i = 0, 0
            for i in range(1, len(pts) - 1):
                d = _perp_dist(pts[i], pts[0], pts[-1])
                if d > max_d:
                    max_d, max_i = d, i
            if max_d > tol:
                left = _dp(pts[:max_i + 1], tol)
                right = _dp(pts[max_i:], tol)
                return left[:-1] + right
            return [pts[0], pts[-1]]

        result = _dp(coords, tolerance)
        return [[round(c[0], 5), round(c[1], 5)] for c in result]

    @staticmethod
    def _key(tile):
        return f"{tile[0]},{tile[1]}"

    @staticmethod
    def _grid_tiles(south, west, north, east):
        g = _TILE_GRID
        tiles = []
        s = math.floor(south / g) * g
        while s < north:
            w_cur = math.floor(west / g) * g
            while w_cur < east:
                tiles.append((
                    round(s, 4), round(w_cur, 4),
                    round(s + g, 4), round(w_cur + g, 4),
                ))
                w_cur += g
            s += g
        return tiles

    def _load_disk_cache(self):
        try:
            for fname in os.listdir(_CACHE_DIR):
                if not fname.endswith('.json'):
                    continue
                key = fname[:-5]
                path = os.path.join(_CACHE_DIR, fname)
                with open(path, 'r', encoding='utf-8') as f:
                    self._cache[key] = json.load(f)
                self._loaded_keys.add(key)
        except Exception as e:
            print(f"Settlement cache load error: {e}")

    @staticmethod
    def _save_tile(key, features):
        try:
            path = os.path.join(_CACHE_DIR, key + '.json')
            with open(path, 'w', encoding='utf-8') as f:
                json.dump(features, f, ensure_ascii=False, separators=(',', ':'))
        except Exception as e:
            print(f"Settlement cache save error: {e}")

    def preload_cached_tiles(self, south, west, north, east):
        tiles = self._grid_tiles(south, west, north, east)
        for t in tiles:
            key = self._key(t)
            if key in self._cache and self._cache[key] and key not in self._emitted_keys:
                self._emitted_keys.add(key)
                self.tile_loaded.emit(key, self._cache[key])


def _close(a, b, eps=1e-6):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


class MapBridge(QObject):
    position_clicked = pyqtSignal(float, float)
    context_menu_requested = pyqtSignal(float, float, int, int)
    bounds_changed = pyqtSignal(float, float, float, float)
    mouse_moved = pyqtSignal(float, float)
    zoom_changed = pyqtSignal(int)
    zone_drawing_finished = pyqtSignal(str)
    zone_drawing_cancelled = pyqtSignal()
    zone_double_clicked = pyqtSignal(str)
    zone_context_menu_requested = pyqtSignal(str, int, int)
    zone_editing_finished = pyqtSignal()
    zone_vertices_updated = pyqtSignal(str, str)

    @pyqtSlot(float, float)
    def onMapClick(self, lat, lon):
        self.position_clicked.emit(lat, lon)

    @pyqtSlot(float, float, int, int)
    def onContextMenu(self, lat, lon, screen_x, screen_y):
        self.context_menu_requested.emit(lat, lon, screen_x, screen_y)

    @pyqtSlot(float, float, float, float)
    def onBoundsChanged(self, south, west, north, east):
        self.bounds_changed.emit(south, west, north, east)

    @pyqtSlot(float, float)
    def onMouseMove(self, lat, lon):
        self.mouse_moved.emit(lat, lon)

    @pyqtSlot(int)
    def onZoomChanged(self, zoom):
        self.zoom_changed.emit(zoom)

    @pyqtSlot(str)
    def onZoneDrawingFinished(self, points_json):
        self.zone_drawing_finished.emit(points_json)

    @pyqtSlot()
    def onZoneDrawingCancelled(self):
        self.zone_drawing_cancelled.emit()

    @pyqtSlot(str)
    def onZoneDoubleClicked(self, zone_id):
        self.zone_double_clicked.emit(zone_id)

    @pyqtSlot(str, int, int)
    def onZoneContextMenu(self, zone_id, screen_x, screen_y):
        self.zone_context_menu_requested.emit(zone_id, screen_x, screen_y)

    @pyqtSlot()
    def onZoneEditingFinished(self):
        self.zone_editing_finished.emit()

    @pyqtSlot(str, str)
    def onZoneVerticesUpdated(self, zone_id, points_json):
        self.zone_vertices_updated.emit(zone_id, points_json)


class MapWidget(QWidget):
    set_position_requested = pyqtSignal(float, float)
    add_waypoint_requested = pyqtSignal(float, float)
    set_home_requested = pyqtSignal(float, float)
    center_map_requested = pyqtSignal(float, float)
    zone_drawing_finished = pyqtSignal(list)
    zone_drawing_cancelled = pyqtSignal()
    zone_double_clicked = pyqtSignal(str)
    zone_context_menu_requested = pyqtSignal(str, int, int)
    zone_editing_finished = pyqtSignal()
    zone_vertices_updated = pyqtSignal(str, list)
    draw_zone_requested = pyqtSignal()
    page_loaded = pyqtSignal()

    def __init__(self, center: tuple = (59.939, 30.315), zoom: int = 14):
        super().__init__()
        self.center = center
        self.zoom = zoom
        self._context_lat = 0.0
        self._context_lon = 0.0

        self._settlement_loader = SettlementLoader()
        self._settlement_loader.tile_loaded.connect(self._on_tile_loaded)

        self._setup_ui()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self.web_view = QWebEngineView()
        self.web_view.page().setBackgroundColor(QColor(Colors.BG_APP))

        self.bridge = MapBridge()
        self.channel = QWebChannel()
        self.channel.registerObject("bridge", self.bridge)
        self.web_view.page().setWebChannel(self.channel)

        self.bridge.context_menu_requested.connect(self._show_context_menu)
        self.bridge.bounds_changed.connect(self._on_bounds_changed)
        self.bridge.zone_drawing_finished.connect(self._on_zone_drawing_finished)
        self.bridge.zone_drawing_cancelled.connect(self.zone_drawing_cancelled.emit)
        self.bridge.zone_double_clicked.connect(self.zone_double_clicked.emit)
        self.bridge.zone_context_menu_requested.connect(self.zone_context_menu_requested.emit)
        self.bridge.zone_editing_finished.connect(self.zone_editing_finished.emit)
        self.bridge.zone_vertices_updated.connect(self._on_zone_vertices_updated)

        self.mouse_moved = self.bridge.mouse_moved
        self.zoom_changed = self.bridge.zoom_changed

        html = self._generate_html()
        self.web_view.setHtml(html)
        self.web_view.loadFinished.connect(self._on_page_loaded)

        layout.addWidget(self.web_view)

    def _on_page_loaded(self, ok):
        if ok:
            self.page_loaded.emit()

    def _on_bounds_changed(self, south, west, north, east):
        # Instantly show cached tiles + queue missing for parallel fetch
        self._settlement_loader.preload_cached_tiles(south, west, north, east)
        self._settlement_loader.request(south, west, north, east)

    def _on_tile_loaded(self, key, features):
        if features:
            data_json = json.dumps(features)
            self.web_view.page().runJavaScript(
                f"addSettlements('{key}', {data_json});"
            )

    def _show_context_menu(self, lat: float, lon: float, screen_x: int, screen_y: int):
        self._context_lat = lat
        self._context_lon = lon

        menu = QMenu(self)
        menu.setStyleSheet(f"""
            QMenu {{
                background-color: {Colors.BG_TOOLTIP};
                color: {Colors.TEXT_PRIMARY};
                border: 1px solid {Colors.BORDER};
                border-radius: 8px;
                padding: 4px;
            }}
            QMenu::item {{
                padding: 8px 16px;
                border-radius: 4px;
            }}
            QMenu::item:selected {{
                background-color: {Colors.BG_INPUT};
            }}
            QMenu::separator {{
                height: 1px;
                background: {Colors.BORDER};
                margin: 4px 8px;
            }}
        """)

        action_set_pos = QAction("Установить позицию здесь", self)
        action_set_pos.setIcon(qta.icon("mdi.crosshairs-gps", color=Colors.TEXT_SECONDARY))
        action_set_pos.triggered.connect(self._on_set_position)
        menu.addAction(action_set_pos)

        action_add_wp = QAction("Добавить точку маршрута", self)
        action_add_wp.setIcon(qta.icon("mdi.map-marker-plus", color=Colors.TEXT_SECONDARY))
        action_add_wp.triggered.connect(self._on_add_waypoint)
        menu.addAction(action_add_wp)

        action_set_home = QAction("Установить дом", self)
        action_set_home.setIcon(qta.icon("mdi.home-map-marker", color=Colors.TEXT_SECONDARY))
        action_set_home.triggered.connect(self._on_set_home)
        menu.addAction(action_set_home)

        menu.addSeparator()

        action_center = QAction("Центрировать карту", self)
        action_center.setIcon(qta.icon("mdi.crosshairs", color=Colors.TEXT_SECONDARY))
        action_center.triggered.connect(self._on_center_map)
        menu.addAction(action_center)

        action_clear_track = QAction("Очистить трек", self)
        action_clear_track.setIcon(qta.icon("mdi.eraser", color=Colors.TEXT_SECONDARY))
        action_clear_track.triggered.connect(self._on_clear_track)
        menu.addAction(action_clear_track)

        menu.addSeparator()

        action_draw_zone = QAction("Нарисовать запретную зону", self)
        action_draw_zone.setIcon(qta.icon("mdi.shield-alert-outline", color=Colors.TEXT_SECONDARY))
        action_draw_zone.triggered.connect(self._on_draw_zone)
        menu.addAction(action_draw_zone)

        menu.popup(QCursor.pos())

    def _on_set_position(self):
        self.set_position_requested.emit(self._context_lat, self._context_lon)

    def _on_add_waypoint(self):
        self.add_waypoint_requested.emit(self._context_lat, self._context_lon)

    def _on_set_home(self):
        self.set_home_requested.emit(self._context_lat, self._context_lon)

    def _on_center_map(self):
        self.center_on(self._context_lat, self._context_lon)

    def _on_clear_track(self):
        self.clear_track()

    def _on_draw_zone(self):
        self.draw_zone_requested.emit()

    def _generate_html(self) -> str:
        return f'''
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
    <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
    <script src="qrc:///qtwebchannel/qwebchannel.js"></script>
    <style>
        html, body {{ margin: 0; padding: 0; background: {Colors.BG_APP}; overflow: hidden; }}
        #map {{ width: 100%; height: 100vh; }}
        .leaflet-container {{ background: {Colors.BG_APP} !important; }}
        .leaflet-tile-pane {{ opacity: 1; }}
        .leaflet-popup-content-wrapper {{
            background: {Colors.BG_CARD} !important;
            color: {Colors.TEXT_PRIMARY} !important;
            border: 1px solid {Colors.BORDER} !important;
            border-radius: 8px !important;
            box-shadow: 0 4px 16px rgba(0,0,0,0.4) !important;
        }}
        .leaflet-popup-tip {{ background: {Colors.BG_CARD} !important; }}
        .leaflet-popup-close-button {{ color: {Colors.TEXT_TERTIARY} !important; }}
        .aircraft-icon {{
            width: 32px;
            height: 32px;
            margin-left: -16px;
            margin-top: -16px;
        }}
        .waypoint-pin {{
            filter: drop-shadow(0 2px 4px rgba(0,0,0,0.5));
        }}
        .waypoint-pin .pin-body {{
            transition: transform 0.15s ease;
        }}
        .waypoint-pin:hover .pin-body {{
            transform: scale(1.1);
        }}
        .wp-number {{
            font-family: 'Segoe UI', Inter, sans-serif;
            font-size: 11px;
            font-weight: bold;
            fill: #fff;
        }}
        .leaflet-tooltip {{
            background: {Colors.BG_TOOLTIP};
            border: 1px solid {Colors.BORDER};
            border-radius: 8px;
            padding: 6px 10px;
            font-family: 'Segoe UI', Inter, sans-serif;
            font-size: 11px;
            color: {Colors.TEXT_PRIMARY};
            box-shadow: 0 4px 12px rgba(0,0,0,0.3);
        }}
        .leaflet-tooltip-top:before {{
            border-top-color: {Colors.BG_TOOLTIP};
        }}
        .leaflet-control-zoom a {{
            background-color: {Colors.BG_CARD} !important;
            color: {Colors.TEXT_PRIMARY} !important;
            border-color: {Colors.BORDER} !important;
        }}
        .leaflet-control-zoom a:hover {{
            background-color: {Colors.BG_TOOLTIP} !important;
        }}
        .leaflet-bar {{
            border: 1px solid {Colors.BORDER} !important;
            border-radius: 8px !important;
            overflow: hidden;
            box-shadow: 0 2px 8px rgba(0,0,0,0.3) !important;
        }}
        .leaflet-bar a {{
            border-bottom-color: {Colors.BORDER} !important;
        }}

        .map-layer-panel {{
            position: fixed;
            bottom: 16px;
            left: 16px;
            z-index: 1000;
            font-family: 'Segoe UI', Inter, sans-serif;
        }}
        .map-layer-toggle {{
            width: 34px;
            height: 34px;
            background: {Colors.BG_CARD};
            border: 1px solid {Colors.BORDER};
            border-radius: 8px;
            color: {Colors.TEXT_SECONDARY};
            font-size: 16px;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            box-shadow: 0 2px 8px rgba(0,0,0,0.3);
            backdrop-filter: blur(8px);
            -webkit-backdrop-filter: blur(8px);
            transition: background 0.15s;
        }}
        .map-layer-toggle:hover {{
            background: {Colors.BG_HOVER};
            color: {Colors.TEXT_PRIMARY};
        }}
        .map-layer-content {{
            display: none;
            background: {Colors.BG_CARD};
            border: 1px solid {Colors.BORDER};
            border-radius: 8px;
            padding: 8px 12px;
            margin-bottom: 6px;
            box-shadow: 0 4px 16px rgba(0,0,0,0.4);
            backdrop-filter: blur(12px);
            -webkit-backdrop-filter: blur(12px);
            min-width: 160px;
        }}
        .map-layer-content.open {{
            display: block;
        }}
        .map-layer-content label {{
            display: flex;
            align-items: center;
            gap: 8px;
            padding: 4px 0;
            color: {Colors.TEXT_SECONDARY};
            font-size: 12px;
            cursor: pointer;
            user-select: none;
            transition: color 0.15s;
        }}
        .map-layer-content label:hover {{
            color: {Colors.TEXT_PRIMARY};
        }}
        .map-layer-content input[type="checkbox"] {{
            width: 14px;
            height: 14px;
            accent-color: {Colors.PRIMARY};
            cursor: pointer;
        }}
    </style>
</head>
<body>
    <div id="map"></div>

    <div class="map-layer-panel">
        <div class="map-layer-content" id="layerContent">
            <label><input type="checkbox" id="chkTrack" checked onchange="toggleLayer('track')"> Трек</label>
            <label><input type="checkbox" id="chkWaypoints" checked onchange="toggleLayer('waypoints')"> Точки маршрута</label>
            <label><input type="checkbox" id="chkHud" checked onchange="toggleLayer('hud')"> HUD элементы</label>
            <label><input type="checkbox" id="chkRestricted" onchange="toggleLayer('restricted')"> Запретные зоны</label>
            <label><input type="checkbox" id="chkSettlements" onchange="toggleLayer('settlements')"> Нас. пункты</label>
        </div>
        <button class="map-layer-toggle" onclick="togglePanel()" title="Слои">
            <svg width="16" height="16" viewBox="0 0 16 16" fill="none">
                <path d="M8 1L1 5l7 4 7-4-7-4z" fill="currentColor" opacity="0.6"/>
                <path d="M1 8l7 4 7-4" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round" stroke-linecap="round"/>
                <path d="M1 11l7 4 7-4" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round" stroke-linecap="round"/>
            </svg>
        </button>
    </div>

    <script>
        var map = L.map('map', {{attributionControl: false}}).setView([{self.center[0]}, {self.center[1]}], {self.zoom});

        L.tileLayer('https://{{s}}.google.com/vt/lyrs=s,h&x={{x}}&y={{y}}&z={{z}}', {{
            maxZoom: 20,
            subdomains: ['mt0', 'mt1', 'mt2', 'mt3']
        }}).addTo(map);

        var aircraftMarker = null;
        var trackLine = null;
        var trackPoints = [];
        var waypointMarkers = [];
        var routeLines = [];
        var activeWaypointLine = null;
        var waypointData = [];
        var activeWaypointIdx = 0;
        var followAircraft = false;
        var lastHeading = 0;
        var lastAircraftPos = null;

        var showTrack = true;
        var showWaypoints = true;
        var showHud = true;
        var showRestrictedZones = false;
        var showSettlements = false;

        var restrictedZonesLayer = L.layerGroup();
        var restrictedZones = {{}};
        var drawingMode = false;
        var drawingPoints = [];
        var drawingMarkers = [];
        var drawingPreviewLine = null;
        var drawingMouseLine = null;
        var editingZoneId = null;
        var editingMarkers = [];

        /* ── Hatching pattern helper ── */
        function _createHatchPattern(bgColor, lineColor, spacing, lineWidth) {{
            var c = document.createElement('canvas');
            c.width = spacing; c.height = spacing;
            var ctx = c.getContext('2d');
            ctx.fillStyle = bgColor;
            ctx.fillRect(0, 0, spacing, spacing);
            ctx.strokeStyle = lineColor;
            ctx.lineWidth = lineWidth;
            ctx.beginPath();
            ctx.moveTo(-1, spacing + 1);
            ctx.lineTo(spacing + 1, -1);
            ctx.moveTo(-1 - spacing, 1);
            ctx.lineTo(1, -1 - spacing + 2);
            ctx.moveTo(spacing - 1, spacing * 2 + 1);
            ctx.lineTo(spacing * 2 + 1, spacing - 1);
            ctx.stroke();
            return c;
        }}

        var _settlementHatch = _createHatchPattern(
            'rgba(239,68,68,0.22)', 'rgba(200,40,40,0.45)', 10, 1.5
        );
        var _zoneHatch = _createHatchPattern(
            'rgba(180,30,30,0.18)', 'rgba(0,0,0,0.55)', 12, 1.5
        );

        /* Override L.Canvas to support fillPattern */
        var _origFillStroke = L.Canvas.prototype._fillStroke;
        L.Canvas.include({{
            _fillStroke: function(ctx, layer) {{
                var opt = layer.options;
                if (opt.fillPattern) {{
                    if (opt.fill) {{
                        ctx.globalAlpha = opt.fillOpacity != null ? opt.fillOpacity : 0.2;
                        ctx.fillStyle = ctx.createPattern(opt.fillPattern, 'repeat');
                        ctx.fill(opt.fillRule || 'evenodd');
                    }}
                    if (opt.stroke && opt.weight !== 0) {{
                        if (ctx.setLineDash) {{
                            ctx.setLineDash(layer.options && layer.options._dashArray || []);
                        }}
                        ctx.globalAlpha = opt.opacity != null ? opt.opacity : 1;
                        ctx.lineWidth = opt.weight;
                        ctx.strokeStyle = opt.color;
                        ctx.lineCap = opt.lineCap || 'round';
                        ctx.lineJoin = opt.lineJoin || 'round';
                        ctx.stroke();
                    }}
                }} else {{
                    _origFillStroke.call(this, ctx, layer);
                }}
            }}
        }});

        map.createPane('restricted');
        map.getPane('restricted').style.zIndex = 260;
        var restrictedRenderer = L.canvas({{ pane: 'restricted' }});

        map.createPane('settlements');
        map.getPane('settlements').style.zIndex = 250;
        var settlementLayer = L.layerGroup();
        var settlementTileLayers = {{}};
        var settlementTileData = {{}};

        function togglePanel() {{
            document.getElementById('layerContent').classList.toggle('open');
        }}

        var _settlementTimer = null;
        function _requestSettlements() {{
            if (!showSettlements || !bridge) return;
            if (_settlementTimer) clearTimeout(_settlementTimer);
            _settlementTimer = setTimeout(function() {{
                _updateOverlayOpacity();
                if (map.getZoom() <= 12) {{
                    for (var k in settlementTileLayers) {{ _removeTile(k); }}
                    return;
                }}
                _cullSettlementTiles();
                var b = map.getBounds().pad(0.3);
                bridge.onBoundsChanged(b.getSouth(), b.getWest(), b.getNorth(), b.getEast());
            }}, 150);
        }}

        function toggleLayer(name) {{
            switch (name) {{
                case 'track':
                    showTrack = document.getElementById('chkTrack').checked;
                    if (trackLine) {{
                        showTrack ? map.addLayer(trackLine) : map.removeLayer(trackLine);
                    }}
                    break;
                case 'waypoints':
                    showWaypoints = document.getElementById('chkWaypoints').checked;
                    waypointMarkers.forEach(function(m) {{
                        showWaypoints ? map.addLayer(m) : map.removeLayer(m);
                    }});
                    routeLines.forEach(function(l) {{
                        showWaypoints ? map.addLayer(l) : map.removeLayer(l);
                    }});
                    if (activeWaypointLine) {{
                        showWaypoints ? map.addLayer(activeWaypointLine) : map.removeLayer(activeWaypointLine);
                    }}
                    break;
                case 'hud':
                    showHud = document.getElementById('chkHud').checked;
                    if (homeMarker) {{
                        showHud ? map.addLayer(homeMarker) : map.removeLayer(homeMarker);
                    }}
                    break;
                case 'restricted':
                    showRestrictedZones = document.getElementById('chkRestricted').checked;
                    if (showRestrictedZones) {{
                        map.addLayer(restrictedZonesLayer);
                        _updateOverlayOpacity();
                    }} else {{
                        map.removeLayer(restrictedZonesLayer);
                    }}
                    break;
                case 'settlements':
                    showSettlements = document.getElementById('chkSettlements').checked;
                    if (showSettlements) {{
                        map.addLayer(settlementLayer);
                        _updateOverlayOpacity();
                        _requestSettlements();
                    }} else {{
                        map.removeLayer(settlementLayer);
                        for (var k in settlementTileLayers) {{ _removeTile(k); }}
                    }}
                    break;
            }}
        }}

        var settlementRenderer = L.canvas({{ pane: 'settlements' }});
        var _settlementStyle = {{
            fillPattern: _settlementHatch,
            fillOpacity: 1,
            color: 'rgba(239,68,68,0.75)',
            weight: 2,
            fill: true,
            interactive: false,
            renderer: settlementRenderer
        }};
        var _fallbackRadii = {{city: 5000, town: 2000, village: 700, hamlet: 300}};

        function _isTileVisible(tileKey) {{
            var p = tileKey.split(',');
            var ts = parseFloat(p[0]), tw = parseFloat(p[1]), g = 0.1;
            var b = map.getBounds();
            return !(ts + g < b.getSouth() || ts > b.getNorth() || tw + g < b.getWest() || tw > b.getEast());
        }}

        function _renderTile(tileKey, features) {{
            if (settlementTileLayers[tileKey]) return;
            var layers = [];
            features.forEach(function(f) {{
                var shape;
                if (f.F === 'p') {{
                    shape = L.polygon(f.c, _settlementStyle);
                }} else {{
                    var r = _fallbackRadii[f.t] || 700;
                    shape = L.circle([f.lat, f.lon], Object.assign({{radius: r}}, _settlementStyle));
                }}
                shape.addTo(settlementLayer);
                layers.push(shape);
            }});
            settlementTileLayers[tileKey] = layers;
        }}

        function _removeTile(tileKey) {{
            var layers = settlementTileLayers[tileKey];
            if (!layers) return;
            layers.forEach(function(l) {{ settlementLayer.removeLayer(l); }});
            delete settlementTileLayers[tileKey];
        }}

        function _cullSettlementTiles() {{
            var b = map.getBounds();
            var pad = 0.05, g = 0.1;
            var south = b.getSouth() - pad, north = b.getNorth() + pad;
            var west = b.getWest() - pad, east = b.getEast() + pad;
            for (var key in settlementTileLayers) {{
                var p = key.split(',');
                var ts = parseFloat(p[0]), tw = parseFloat(p[1]);
                if (ts + g < south || ts > north || tw + g < west || tw > east) _removeTile(key);
            }}
            for (var key in settlementTileData) {{
                if (settlementTileLayers[key]) continue;
                var p = key.split(',');
                var ts = parseFloat(p[0]), tw = parseFloat(p[1]);
                if (ts + g >= south && ts <= north && tw + g >= west && tw <= east) {{
                    _renderTile(key, settlementTileData[key]);
                }}
            }}
        }}

        function _updateOverlayOpacity() {{
            var z = map.getZoom();
            var opacity;
            if (z <= 12 || z >= 17) {{ opacity = 0; }}
            else if (z <= 14) {{ opacity = 1; }}
            else {{ opacity = (1 - (z - 14) / 3).toFixed(2); }}

            var sp = map.getPane('settlements');
            if (sp) sp.style.opacity = opacity;
            var rp = map.getPane('restricted');
            if (rp) rp.style.opacity = opacity;
        }}

        function addSettlements(tileKey, features) {{
            if (settlementTileData[tileKey]) return;
            settlementTileData[tileKey] = features;
            if (showSettlements && _isTileVisible(tileKey)) {{
                _renderTile(tileKey, features);
            }}
        }}

        /* ── Restricted Zones ── */

        var _zoneStyle = {{
            fillPattern: _zoneHatch,
            fillOpacity: 1,
            color: 'rgba(100,0,0,0.9)',
            weight: 2.5,
            dashArray: '10,6',
            fill: true,
            interactive: true,
            renderer: restrictedRenderer
        }};

        function addRestrictedZone(zoneId, points, name) {{
            if (restrictedZones[zoneId]) removeRestrictedZone(zoneId);
            var polygon = L.polygon(points, _zoneStyle);
            if (name) {{
                polygon.bindTooltip(name, {{ permanent: false, direction: 'center' }});
            }}
            polygon.on('contextmenu', function(e) {{
                L.DomEvent.stopPropagation(e);
                L.DomEvent.preventDefault(e);
                if (bridge && !drawingMode) {{
                    bridge.onZoneContextMenu(zoneId, e.originalEvent.screenX, e.originalEvent.screenY);
                }}
            }});
            polygon.on('dblclick', function(e) {{
                L.DomEvent.stopPropagation(e);
                L.DomEvent.preventDefault(e);
                if (bridge && !drawingMode) bridge.onZoneDoubleClicked(zoneId);
            }});
            polygon.on('click', function(e) {{
                L.DomEvent.stopPropagation(e);
            }});
            polygon.addTo(restrictedZonesLayer);
            restrictedZones[zoneId] = {{ polygon: polygon, name: name }};
        }}

        function removeRestrictedZone(zoneId) {{
            var z = restrictedZones[zoneId];
            if (!z) return;
            if (editingZoneId === zoneId) disableZoneEditing();
            restrictedZonesLayer.removeLayer(z.polygon);
            delete restrictedZones[zoneId];
        }}

        function updateRestrictedZone(zoneId, points) {{
            var z = restrictedZones[zoneId];
            if (!z) return;
            z.polygon.setLatLngs(points);
        }}

        function clearRestrictedZones() {{
            disableZoneEditing();
            for (var id in restrictedZones) {{
                restrictedZonesLayer.removeLayer(restrictedZones[id].polygon);
            }}
            restrictedZones = {{}};
        }}

        function highlightZone(zoneId) {{
            var z = restrictedZones[zoneId];
            if (!z) return;
            z.polygon.setStyle({{ weight: 4 }});
        }}

        function unhighlightZone(zoneId) {{
            var z = restrictedZones[zoneId];
            if (!z) return;
            z.polygon.setStyle({{ weight: 2.5 }});
        }}

        /* ── Drawing Mode ── */

        function startDrawing() {{
            if (drawingMode) return;
            drawingMode = true;
            drawingPoints = [];
            drawingMarkers = [];
            map.getContainer().style.cursor = 'crosshair';
            map.doubleClickZoom.disable();
        }}

        function cancelDrawing() {{
            if (!drawingMode) return;
            _cleanupDrawing();
            drawingMode = false;
            map.getContainer().style.cursor = '';
            map.doubleClickZoom.enable();
            if (bridge) bridge.onZoneDrawingCancelled();
        }}

        function _addDrawingVertex(latlng) {{
            // If 3+ points and click is near the first vertex — finish
            if (drawingPoints.length >= 3 && drawingMarkers.length > 0) {{
                var firstPx = map.latLngToContainerPoint(drawingMarkers[0].getLatLng());
                var clickPx = map.latLngToContainerPoint(latlng);
                var dist = firstPx.distanceTo(clickPx);
                if (dist < 20) {{
                    _finishDrawing();
                    return;
                }}
            }}

            drawingPoints.push([latlng.lat, latlng.lng]);
            var idx = drawingPoints.length - 1;
            var marker = L.circleMarker(latlng, {{
                radius: 5, color: '#fff', fillColor: '#DC2626',
                fillOpacity: 1, weight: 2
            }}).addTo(map);

            if (idx === 0) {{
                marker.setStyle({{ radius: 8 }});
            }}
            drawingMarkers.push(marker);

            if (drawingPreviewLine) {{
                drawingPreviewLine.setLatLngs(drawingPoints);
            }} else {{
                drawingPreviewLine = L.polyline(drawingPoints, {{
                    color: 'rgba(220,38,38,0.8)', weight: 2, dashArray: '6,6', opacity: 0.8
                }}).addTo(map);
            }}
        }}

        function _finishDrawing() {{
            if (drawingPoints.length < 3) return;
            var pts = drawingPoints.slice();
            _cleanupDrawing();
            drawingMode = false;
            map.getContainer().style.cursor = '';
            map.doubleClickZoom.enable();
            if (bridge) bridge.onZoneDrawingFinished(JSON.stringify(pts));
        }}

        function _cleanupDrawing() {{
            drawingMarkers.forEach(function(m) {{ map.removeLayer(m); }});
            drawingMarkers = [];
            drawingPoints = [];
            if (drawingPreviewLine) {{ map.removeLayer(drawingPreviewLine); drawingPreviewLine = null; }}
            if (drawingMouseLine) {{ map.removeLayer(drawingMouseLine); drawingMouseLine = null; }}
        }}

        /* ── Zone Vertex Editing ── */

        function _notifyVerticesUpdated(zoneId) {{
            var z = restrictedZones[zoneId];
            if (z && bridge) {{
                var pts = z.polygon.getLatLngs()[0].map(function(ll) {{
                    return [ll.lat, ll.lng];
                }});
                bridge.onZoneVerticesUpdated(zoneId, JSON.stringify(pts));
            }}
        }}

        function enableZoneEditing(zoneId) {{
            disableZoneEditing();
            var z = restrictedZones[zoneId];
            if (!z) return;
            editingZoneId = zoneId;
            z.polygon.setStyle({{ dashArray: '6,4' }});
            _refreshEditingMarkers(zoneId);
        }}

        function disableZoneEditing() {{
            if (!editingZoneId) return;
            editingMarkers.forEach(function(m) {{ map.removeLayer(m); }});
            editingMarkers = [];
            var z = restrictedZones[editingZoneId];
            if (z) z.polygon.setStyle({{ dashArray: '10,6' }});
            editingZoneId = null;
        }}

        function _refreshEditingMarkers(zoneId) {{
            editingMarkers.forEach(function(m) {{ map.removeLayer(m); }});
            editingMarkers = [];
            var z = restrictedZones[zoneId];
            if (!z) return;
            var latlngs = z.polygon.getLatLngs()[0];

            /* vertex markers */
            latlngs.forEach(function(ll, idx) {{
                var m = L.circleMarker(ll, {{
                    radius: 7, color: '#fff', fillColor: '{Colors.PRIMARY}',
                    fillOpacity: 1, weight: 2
                }}).addTo(map);
                m._vertexIdx = idx;
                m._isVertex = true;
                _makeVertexDraggable(m, zoneId);
                /* right-click to delete vertex */
                m.on('contextmenu', function(e) {{
                    L.DomEvent.stopPropagation(e);
                    L.DomEvent.preventDefault(e);
                    var cur = z.polygon.getLatLngs()[0];
                    if (cur.length > 3) {{
                        cur.splice(idx, 1);
                        z.polygon.setLatLngs([cur]);
                        _refreshEditingMarkers(zoneId);
                        _notifyVerticesUpdated(zoneId);
                    }}
                }});
                editingMarkers.push(m);
            }});

            /* midpoint markers */
            for (var i = 0; i < latlngs.length; i++) {{
                var next = (i + 1) % latlngs.length;
                var midLat = (latlngs[i].lat + latlngs[next].lat) / 2;
                var midLng = (latlngs[i].lng + latlngs[next].lng) / 2;
                var mid = L.circleMarker([midLat, midLng], {{
                    radius: 5, color: '#fff', fillColor: '#888',
                    fillOpacity: 0.7, weight: 1.5
                }}).addTo(map);
                mid._insertAfter = i;
                mid._isMidpoint = true;
                _makeMidpointInteractive(mid, zoneId);
                editingMarkers.push(mid);
            }}
        }}

        function _makeVertexDraggable(marker, zoneId) {{
            var dragging = false;
            marker.on('mousedown', function(e) {{
                if (e.originalEvent.button !== 0) return;
                L.DomEvent.stopPropagation(e);
                dragging = true;
                map.dragging.disable();
                map.on('mousemove', onMove);
                map.on('mouseup', onUp);
            }});
            function onMove(e) {{
                if (!dragging) return;
                marker.setLatLng(e.latlng);
                var z = restrictedZones[zoneId];
                if (z) {{
                    var latlngs = z.polygon.getLatLngs()[0];
                    latlngs[marker._vertexIdx] = e.latlng;
                    z.polygon.setLatLngs(latlngs);
                }}
            }}
            function onUp() {{
                if (!dragging) return;
                dragging = false;
                map.dragging.enable();
                map.off('mousemove', onMove);
                map.off('mouseup', onUp);
                _refreshEditingMarkers(zoneId);
                _notifyVerticesUpdated(zoneId);
            }}
        }}

        function _makeMidpointInteractive(marker, zoneId) {{
            var dragging = false, inserted = false, insertIdx = -1;
            marker.on('mousedown', function(e) {{
                if (e.originalEvent.button !== 0) return;
                L.DomEvent.stopPropagation(e);
                dragging = true;
                inserted = false;
                insertIdx = marker._insertAfter + 1;
                map.dragging.disable();
                map.on('mousemove', onMove);
                map.on('mouseup', onUp);
            }});
            function onMove(e) {{
                if (!dragging) return;
                var z = restrictedZones[zoneId];
                if (!z) return;
                if (!inserted) {{
                    var latlngs = z.polygon.getLatLngs()[0];
                    latlngs.splice(insertIdx, 0, e.latlng);
                    z.polygon.setLatLngs([latlngs]);
                    inserted = true;
                    marker.setStyle({{ radius: 7, fillColor: '{Colors.PRIMARY}', fillOpacity: 1 }});
                }}
                marker.setLatLng(e.latlng);
                var latlngs = z.polygon.getLatLngs()[0];
                latlngs[insertIdx] = e.latlng;
                z.polygon.setLatLngs([latlngs]);
            }}
            function onUp() {{
                dragging = false;
                map.dragging.enable();
                map.off('mousemove', onMove);
                map.off('mouseup', onUp);
                if (inserted) {{
                    _refreshEditingMarkers(zoneId);
                    _notifyVerticesUpdated(zoneId);
                }}
            }}
        }}

        /* ── Drawing event integration ── */

        document.addEventListener('keydown', function(e) {{
            if (e.key === 'Escape' && drawingMode) cancelDrawing();
            if (e.key === 'Escape' && editingZoneId) {{
                disableZoneEditing();
                if (bridge) bridge.onZoneEditingFinished();
            }}
        }});

        function createAircraftIcon(heading) {{
            return L.divIcon({{
                html: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32" style="transform: rotate(${{heading}}deg); filter: drop-shadow(0 2px 4px rgba(0,0,0,0.5));">
                    <path d="M16 2 L14 12 L4 14 L4 18 L14 16 L14 26 L10 28 L10 30 L16 28 L22 30 L22 28 L18 26 L18 16 L28 18 L28 14 L18 12 Z"
                          fill="{Colors.SUCCESS}" stroke="#fff" stroke-width="0.5" opacity="0.95"/>
                </svg>`,
                className: 'aircraft-icon',
                iconSize: [32, 32],
                iconAnchor: [16, 16]
            }});
        }}

        function updateAircraft(lat, lon, heading) {{
            lastAircraftPos = [lat, lon];

            if (!aircraftMarker) {{
                aircraftMarker = L.marker([lat, lon], {{
                    icon: createAircraftIcon(heading),
                    zIndexOffset: 1000
                }}).addTo(map);
            }} else {{
                aircraftMarker.setLatLng([lat, lon]);
                if (Math.abs(heading - lastHeading) > 1) {{
                    aircraftMarker.setIcon(createAircraftIcon(heading));
                    lastHeading = heading;
                }}
            }}

            trackPoints.push([lat, lon]);
            if (trackPoints.length > 1000) trackPoints.shift();

            if (trackLine) {{
                trackLine.setLatLngs(trackPoints);
            }} else {{
                trackLine = L.polyline(trackPoints, {{
                    color: '{Colors.SUCCESS}',
                    weight: 2,
                    opacity: 0.7
                }});
                if (showTrack) trackLine.addTo(map);
            }}

            updateActiveWaypointLine();

            if (followAircraft) {{
                map.panTo([lat, lon], {{animate: false}});
            }}
        }}

        function setFollowMode(enabled) {{
            followAircraft = enabled;
        }}

        function setAircraftPosition(lat, lon) {{
            if (aircraftMarker) {{
                aircraftMarker.setLatLng([lat, lon]);
            }}
            trackPoints = [[lat, lon]];
            if (trackLine) {{
                trackLine.setLatLngs(trackPoints);
            }}
            map.setView([lat, lon], map.getZoom());
        }}

        function clearTrack() {{
            trackPoints = [];
            if (trackLine) {{
                trackLine.setLatLngs([]);
            }}
        }}

        function formatTooltip(wp, index, isActive) {{
            var actionShort = {{
                'FLYTHROUGH': '',
                'ORBIT_TURNS': 'Круж. ' + wp.orbit_turns + 'x',
                'ORBIT_INFINITE': 'Круж. ∞',
                'ALTITUDE': '↕ Высота'
            }};

            var lines = [];
            lines.push('<b>#' + (index + 1) + '</b> ' + wp.altitude + 'м');

            var action = actionShort[wp.action] || '';
            if (action) {{
                lines.push(action + (wp.orbit_radius ? ' R' + wp.orbit_radius : ''));
            }}

            if (wp.climb_enroute) {{
                lines.push('↗ набор');
            }}

            return lines.join('<br>');
        }}

        function createWaypointIcon(number, isPast, isActive) {{
            var size = isActive ? 32 : 28;
            var fillColor, strokeColor, textColor;

            if (isPast) {{
                fillColor = '{Colors.TEXT_TERTIARY}';
                strokeColor = '#4b5563';
                textColor = '{Colors.TEXT_SECONDARY}';
            }} else if (isActive) {{
                fillColor = '{Colors.SUCCESS}';
                strokeColor = '#059669';
                textColor = '#ffffff';
            }} else {{
                fillColor = '{Colors.WARNING}';
                strokeColor = '#d97706';
                textColor = '#ffffff';
            }}

            var glowFilter = isActive ? '<filter id="glow"><feGaussianBlur stdDeviation="2" result="coloredBlur"/><feMerge><feMergeNode in="coloredBlur"/><feMergeNode in="SourceGraphic"/></feMerge></filter>' : '';
            var glowAttr = isActive ? 'filter="url(#glow)"' : '';

            var svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 28 36" width="${{size}}" height="${{size * 36/28}}" class="waypoint-pin">
                <defs>${{glowFilter}}</defs>
                <g class="pin-body" ${{glowAttr}}>
                    <path d="M14 0C6.3 0 0 6.3 0 14c0 10.5 14 22 14 22s14-11.5 14-22C28 6.3 21.7 0 14 0z"
                          fill="${{fillColor}}" stroke="${{strokeColor}}" stroke-width="1.5"/>
                    <circle cx="14" cy="13" r="9" fill="rgba(255,255,255,0.15)"/>
                </g>
                <text x="14" y="17" text-anchor="middle" class="wp-number" fill="${{textColor}}">${{number}}</text>
            </svg>`;

            return L.divIcon({{
                html: svg,
                className: '',
                iconSize: [size, size * 36/28],
                iconAnchor: [size/2, size * 36/28],
                popupAnchor: [0, -size * 36/28]
            }});
        }}

        function setWaypoints(waypoints, activeIdx) {{
            waypointMarkers.forEach(m => map.removeLayer(m));
            waypointMarkers = [];
            routeLines.forEach(l => map.removeLayer(l));
            routeLines = [];

            waypointData = waypoints;
            if (activeIdx !== undefined) activeWaypointIdx = activeIdx;

            waypoints.forEach((wp, i) => {{
                var isPast = i < activeWaypointIdx;
                var isActive = i === activeWaypointIdx;

                var icon = createWaypointIcon(i + 1, isPast, isActive);
                var marker = L.marker([wp.lat, wp.lon], {{
                    icon: icon,
                    zIndexOffset: isActive ? 100 : (isPast ? -100 : 0)
                }});
                if (showWaypoints) marker.addTo(map);

                marker.bindTooltip(formatTooltip(wp, i, isActive), {{
                    permanent: false,
                    direction: 'top',
                    offset: [0, -32],
                    className: ''
                }});

                waypointMarkers.push(marker);
            }});

            for (var i = 0; i < waypoints.length - 1; i++) {{
                var isPastSegment = i < activeWaypointIdx - 1;
                var isActiveSegment = i === activeWaypointIdx - 1;
                var isFutureSegment = i >= activeWaypointIdx;

                var color, weight, opacity, dashArray;

                if (isPastSegment) {{
                    color = '{Colors.TEXT_TERTIARY}';
                    weight = 2;
                    opacity = 0.4;
                    dashArray = null;
                }} else if (isActiveSegment) {{
                    color = '{Colors.PRIMARY_LIGHT}';
                    weight = 3;
                    opacity = 0.9;
                    dashArray = null;
                }} else {{
                    color = '{Colors.WARNING}';
                    weight = 2;
                    opacity = 0.7;
                    dashArray = '8, 8';
                }}

                var line = L.polyline([
                    [waypoints[i].lat, waypoints[i].lon],
                    [waypoints[i + 1].lat, waypoints[i + 1].lon]
                ], {{
                    color: color,
                    weight: weight,
                    opacity: opacity,
                    dashArray: dashArray
                }});
                if (showWaypoints) line.addTo(map);
                routeLines.push(line);
            }}

            updateActiveWaypointLine();
        }}

        function updateActiveWaypoint(idx) {{
            activeWaypointIdx = idx;
            if (waypointData.length > 0) {{
                setWaypoints(waypointData, idx);
            }}
        }}

        function updateActiveWaypointLine() {{
            if (activeWaypointLine) {{
                map.removeLayer(activeWaypointLine);
                activeWaypointLine = null;
            }}

            if (lastAircraftPos && waypointData.length > activeWaypointIdx) {{
                var wp = waypointData[activeWaypointIdx];
                activeWaypointLine = L.polyline([
                    lastAircraftPos,
                    [wp.lat, wp.lon]
                ], {{
                    color: '{Colors.PRIMARY}',
                    weight: 2,
                    opacity: 0.8,
                    dashArray: '4, 8'
                }});
                if (showWaypoints) activeWaypointLine.addTo(map);
            }}
        }}

        function addWaypoint(lat, lon, index, wpData) {{
            var icon = createWaypointIcon(index, false, false);
            var marker = L.marker([lat, lon], {{
                icon: icon
            }});
            if (showWaypoints) marker.addTo(map);

            var tooltip = wpData ? formatTooltip(wpData, index - 1, false) : 'Точка ' + index;
            marker.bindTooltip(tooltip, {{
                permanent: false,
                direction: 'top',
                offset: [0, -32]
            }});

            waypointMarkers.push(marker);

            if (wpData) {{
                waypointData.push(wpData);
            }}
        }}

        function centerOn(lat, lon) {{
            map.setView([lat, lon], map.getZoom());
        }}

        var homeMarker = null;

        function setHomeMarker(lat, lon) {{
            if (homeMarker) {{
                homeMarker.setLatLng([lat, lon]);
            }} else {{
                var homeIcon = L.divIcon({{
                    html: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24" style="filter: drop-shadow(0 2px 3px rgba(0,0,0,0.4));">
                        <path d="M12 3L4 9v12h5v-7h6v7h5V9l-8-6z" fill="{Colors.ERROR}" stroke="#fff" stroke-width="0.8"/>
                    </svg>`,
                    className: 'home-icon',
                    iconSize: [24, 24],
                    iconAnchor: [12, 24]
                }});
                homeMarker = L.marker([lat, lon], {{icon: homeIcon, zIndexOffset: 500}});
                if (showHud) homeMarker.addTo(map);
                homeMarker.bindTooltip('Дом', {{permanent: false, direction: 'top'}});
            }}
        }}

        map.on('move', function() {{
            _requestSettlements();
        }});
        map.on('zoomend', function() {{
            _requestSettlements();
            _updateOverlayOpacity();
        }});

        var bridge = null;
        new QWebChannel(qt.webChannelTransport, function(channel) {{
            bridge = channel.objects.bridge;
        }});

        map.on('click', function(e) {{
            if (drawingMode) {{
                _addDrawingVertex(e.latlng);
                return;
            }}
            if (editingZoneId) {{
                disableZoneEditing();
                if (bridge) bridge.onZoneEditingFinished();
                return;
            }}
            if (bridge) {{
                bridge.onMapClick(e.latlng.lat, e.latlng.lng);
            }}
        }});

        map.on('dblclick', function(e) {{
            if (drawingMode && drawingPoints.length >= 3) {{
                L.DomEvent.stopPropagation(e);
                L.DomEvent.preventDefault(e);
                _finishDrawing();
            }}
        }});

        map.on('contextmenu', function(e) {{
            if (bridge) {{
                bridge.onContextMenu(e.latlng.lat, e.latlng.lng, e.originalEvent.screenX, e.originalEvent.screenY);
            }}
        }});

        map.on('mousemove', function(e) {{
            if (drawingMode && drawingPoints.length > 0) {{
                var lastPt = drawingPoints[drawingPoints.length - 1];
                if (drawingMouseLine) {{
                    drawingMouseLine.setLatLngs([lastPt, [e.latlng.lat, e.latlng.lng]]);
                }} else {{
                    drawingMouseLine = L.polyline([lastPt, [e.latlng.lat, e.latlng.lng]], {{
                        color: 'rgba(220,38,38,0.6)', weight: 1.5, dashArray: '4,4', opacity: 0.6
                    }}).addTo(map);
                }}
            }}
            if (bridge) {{
                bridge.onMouseMove(e.latlng.lat, e.latlng.lng);
            }}
        }});

        map.on('zoomend', function() {{
            if (bridge) {{
                bridge.onZoomChanged(map.getZoom());
            }}
        }});
    </script>
</body>
</html>
'''

    def update_aircraft(self, lat: float, lon: float, heading: float):
        self.web_view.page().runJavaScript(f"updateAircraft({lat}, {lon}, {heading});")

    def set_aircraft_position(self, lat: float, lon: float):
        self.web_view.page().runJavaScript(f"setAircraftPosition({lat}, {lon});")

    def clear_track(self):
        self.web_view.page().runJavaScript("clearTrack();")

    def set_waypoints(self, waypoints: list, active_idx: int = 0):
        wp_json = json.dumps(waypoints)
        self.web_view.page().runJavaScript(f"setWaypoints({wp_json}, {active_idx});")

    def update_active_waypoint(self, index: int):
        self.web_view.page().runJavaScript(f"updateActiveWaypoint({index});")

    def add_waypoint(self, lat: float, lon: float, index: int, wp_data: dict = None):
        if wp_data:
            wp_json = json.dumps(wp_data)
            self.web_view.page().runJavaScript(f"addWaypoint({lat}, {lon}, {index}, {wp_json});")
        else:
            self.web_view.page().runJavaScript(f"addWaypoint({lat}, {lon}, {index}, null);")

    def center_on(self, lat: float, lon: float):
        self.web_view.page().runJavaScript(f"centerOn({lat}, {lon});")

    def set_follow_mode(self, enabled: bool):
        self.web_view.page().runJavaScript(f"setFollowMode({'true' if enabled else 'false'});")

    def set_home_marker(self, lat: float, lon: float):
        self.web_view.page().runJavaScript(f"setHomeMarker({lat}, {lon});")

    def set_layer_visibility(self, layer_name: str, visible: bool):
        checkbox_map = {
            'track': 'chkTrack',
            'waypoints': 'chkWaypoints',
            'hud': 'chkHud',
            'restricted': 'chkRestricted',
            'settlements': 'chkSettlements',
        }
        chk_id = checkbox_map.get(layer_name)
        if not chk_id:
            return
        js_val = 'true' if visible else 'false'
        self.web_view.page().runJavaScript(
            f"document.getElementById('{chk_id}').checked = {js_val}; toggleLayer('{layer_name}');"
        )

    # ── Restricted Zones ──

    def _on_zone_drawing_finished(self, points_json):
        try:
            points = json.loads(points_json)
            self.zone_drawing_finished.emit(points)
        except json.JSONDecodeError:
            pass

    def _on_zone_vertices_updated(self, zone_id, points_json):
        try:
            points = json.loads(points_json)
            self.zone_vertices_updated.emit(zone_id, points)
        except json.JSONDecodeError:
            pass

    def start_zone_drawing(self):
        self.web_view.page().runJavaScript("startDrawing();")

    def cancel_zone_drawing(self):
        self.web_view.page().runJavaScript("cancelDrawing();")

    def add_restricted_zone(self, zone_id: str, points: list, name: str = ""):
        pts_json = json.dumps(points)
        name_escaped = json.dumps(name)
        self.web_view.page().runJavaScript(
            f"addRestrictedZone({json.dumps(zone_id)}, {pts_json}, {name_escaped});"
        )

    def remove_restricted_zone(self, zone_id: str):
        self.web_view.page().runJavaScript(f"removeRestrictedZone({json.dumps(zone_id)});")

    def update_restricted_zone(self, zone_id: str, points: list):
        pts_json = json.dumps(points)
        self.web_view.page().runJavaScript(
            f"updateRestrictedZone({json.dumps(zone_id)}, {pts_json});"
        )

    def enable_zone_editing(self, zone_id: str):
        self.web_view.page().runJavaScript(f"enableZoneEditing({json.dumps(zone_id)});")

    def disable_zone_editing(self, zone_id: str = ""):
        self.web_view.page().runJavaScript("disableZoneEditing();")

    def highlight_zone(self, zone_id: str):
        self.web_view.page().runJavaScript(f"highlightZone({json.dumps(zone_id)});")

    def unhighlight_zone(self, zone_id: str):
        self.web_view.page().runJavaScript(f"unhighlightZone({json.dumps(zone_id)});")

    def load_all_zones(self, zones: list):
        for z in zones:
            self.add_restricted_zone(z['id'], z['points'], z.get('name', ''))
