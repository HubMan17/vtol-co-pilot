import json
import math
import os
import threading
import time
import urllib.request
import urllib.parse

import qtawesome as qta
from PyQt5.QtWidgets import QWidget, QVBoxLayout, QMenu, QAction
from PyQt5.QtWebEngineWidgets import QWebEngineView, QWebEnginePage
from PyQt5.QtWebChannel import QWebChannel
from PyQt5.QtCore import QObject, pyqtSlot, pyqtSignal, QThread
from PyQt5.QtGui import QCursor, QColor

from src.gui.theme import Colors


_TILE_GRID = 0.1  # ~10 km tile grid
_CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'cache', 'settlements_v5')
_DP_TOLERANCE = 0.0005  # ~55 m - Douglas-Peucker simplification

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

        self._last_aircraft_ts = 0.0
        self._last_aircraft_lat = 0.0
        self._last_aircraft_lon = 0.0

        self._setup_ui()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self.web_view = QWebEngineView()
        # Route JS console.warn/console.log to Python logger
        self.web_view.page().javaScriptConsoleMessage = self._on_js_console
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

    @staticmethod
    def _on_js_console(level, message, line, source):
        import logging
        _logger = logging.getLogger('map_js')
        if '[PERF]' in message:
            _logger.warning(message)
        elif level == QWebEnginePage.WarningMessageLevel:
            _logger.warning(message)

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
        .wp-index-icon {{
            width: 24px;
            height: 32px;
            display: flex;
            align-items: center;
            justify-content: center;
            pointer-events: none;
            user-select: none;
        }}
        .wp-icon-wrap {{
            position: relative;
            width: 24px;
            height: 32px;
            display: block;
        }}
        .wp-icon-svg {{
            width: 24px;
            height: 32px;
            overflow: visible;
            filter: drop-shadow(0 1px 3px rgba(0,0,0,0.55));
        }}
        .wp-icon-num {{
            font-family: 'Segoe UI', Inter, sans-serif;
            font-size: 6px;
            font-weight: 800;
            fill: #ffffff;
            text-anchor: middle;
            dominant-baseline: middle;
            paint-order: stroke;
            stroke: rgba(0,0,0,0.8);
            stroke-width: 0.8;
        }}
        .aircraft-icon {{
            width: 30px;
            height: 30px;
            pointer-events: none;
            user-select: none;
        }}
        .aircraft-icon-wrap {{
            width: 30px;
            height: 30px;
            display: block;
            transform-origin: 15px 15px;
            filter: drop-shadow(0 1px 4px rgba(0,0,0,0.55));
        }}
        .aircraft-icon-svg {{
            width: 30px;
            height: 30px;
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

    <div id="perfOverlay" style="position:fixed;top:4px;right:4px;z-index:9999;background:rgba(0,0,0,0.75);color:#0f0;font:bold 11px monospace;padding:3px 6px;border-radius:4px;pointer-events:none;"></div>

    <script>
        /* РІвЂќР‚РІвЂќР‚ Performance profiler РІвЂќР‚РІвЂќР‚ */
        var _perfFrames = 0, _perfLastTs = performance.now(), _perfFps = 0;
        var _perfSlowFrames = 0, _perfUpdateMs = 0;
        var _perfEl = document.getElementById('perfOverlay');

        function _perfTick() {{
            _perfFrames++;
            var now = performance.now();
            if (now - _perfLastTs >= 1000) {{
                _perfFps = _perfFrames;
                _perfFrames = 0;
                _perfLastTs = now;
                _perfEl.textContent = 'FPS: ' + _perfFps + ' | upd: ' + _perfUpdateMs.toFixed(1) + 'ms | slow: ' + _perfSlowFrames;
                _perfSlowFrames = 0;
            }}
            requestAnimationFrame(_perfTick);
        }}
        requestAnimationFrame(_perfTick);

        var map = L.map('map', {{attributionControl: false, preferCanvas: true}}).setView([{self.center[0]}, {self.center[1]}], {self.zoom});

        L.tileLayer('https://{{s}}.google.com/vt/lyrs=s,h&hl=ru&x={{x}}&y={{y}}&z={{z}}', {{
            maxZoom: 20,
            subdomains: ['mt0', 'mt1', 'mt2', 'mt3']
        }}).addTo(map);

        var aircraftHalo = null;
        var aircraftBody = null;
        var trackPoints = [];
        var waypointMarkers = [];
        var waypointIndexMarkers = [];
        var routeLines = [];
        var waypointData = [];
        var activeWaypointIdx = 0;
        var followAircraft = false;
        var lastHeading = 0;
        var lastAircraftPos = null;

        /* РІвЂќР‚РІвЂќР‚ Separate Canvas renderer for track + active waypoint line РІвЂќР‚РІвЂќР‚ */
        /* Isolates these high-frequency layers from route SVG.            */
        /* L.canvas renderer lives inside the map pane РІвЂ вЂ™ moves via CSS     */
        /* transform during pan (GPU, zero JS redraw). Only redraws on     */
        /* actual data change or zoom.                                     */
        /* 3 isolated canvas renderers — updating one does NOT redraw others */
        var _aircraftRenderer = L.canvas({{ padding: 0.1 }});  /* halo only */
        var _trackRenderer    = L.canvas({{ padding: 0.3 }});  /* track + wp-line */
        var _routeRenderer    = L.canvas({{ padding: 0.3 }});  /* waypoints, routes, conflicts */
        var trackLine = L.polyline([], {{
            renderer: _trackRenderer,
            color: '{Colors.SUCCESS}',
            weight: 2,
            opacity: 0.7,
            interactive: false
        }}).addTo(map);
        var activeWaypointLine = L.polyline([], {{
            renderer: _trackRenderer,
            color: '{Colors.PRIMARY}',
            weight: 2,
            opacity: 0.8,
            dashArray: '4, 8',
            interactive: false
        }}).addTo(map);
        var _trackUpdateCounter = 0;
        var _wpLineCounter = 0;
        var _mapInteracting = false;
        var _trackDirty = false;
        var _wpLineDirty = false;

        map.on('movestart', function() {{ _mapInteracting = true; }});
        map.on('moveend', function() {{
            _mapInteracting = false;
            /* Flush deferred updates after drag/zoom ends */
            if (_trackDirty) {{
                _trackDirty = false;
                trackLine.setLatLngs(trackPoints);
            }}
            if (_wpLineDirty) {{
                _wpLineDirty = false;
                if (lastAircraftPos && waypointData.length > activeWaypointIdx) {{
                    var wp = waypointData[activeWaypointIdx];
                    activeWaypointLine.setLatLngs([lastAircraftPos, [wp.lat, wp.lon]]);
                }}
            }}
            /* keep aircraft shape in sync after interaction */
            if (lastAircraftPos && aircraftBody) {{
                aircraftBody.setLatLng(lastAircraftPos);
                _setAircraftHeading(lastHeading);
            }}
        }});

        var showTrack = true;
        var showWaypoints = true;
        var showHud = true;
        var showRestrictedZones = false;
        var showSettlements = false;

        var restrictedZones = {{}};
        var drawingMode = false;
        var drawingPoints = [];
        var drawingMarkers = [];
        var drawingPreviewLine = null;
        var drawingMouseLine = null;
        var editingZoneId = null;
        var editingMarkers = [];

        var conflictLines = [];
        var conflictMarkers = [];
        var avoidanceLine = null;

        map.createPane('restricted');
        map.getPane('restricted').style.zIndex = 260;

        map.createPane('settlements');
        map.getPane('settlements').style.zIndex = 250;

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
                if (map.getZoom() <= 12) return;
                var b = map.getBounds().pad(0.3);
                bridge.onBoundsChanged(b.getSouth(), b.getWest(), b.getNorth(), b.getEast());
            }}, 300);
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

        /* РІвЂќР‚РІвЂќР‚ Shared Overlay Tile Mixin РІвЂќР‚РІвЂќР‚ */
        var _OverlayTileMixin = {{
            _latLngToTilePixel: function(lat, lng, zoom, tileX, tileY) {{
                var n = Math.pow(2, zoom);
                var px = ((lng + 180) / 360) * n * 256 - tileX * 256;
                var latRad = lat * Math.PI / 180;
                var py = (1 - Math.log(Math.tan(latRad) + 1 / Math.cos(latRad)) / Math.PI) / 2 * n * 256 - tileY * 256;
                return [px, py];
            }},
            _tileBounds: function(coords) {{
                var n = Math.pow(2, coords.z);
                var west = coords.x / n * 360 - 180;
                var east = (coords.x + 1) / n * 360 - 180;
                var north = Math.atan(Math.sinh(Math.PI * (1 - 2 * coords.y / n))) * 180 / Math.PI;
                var south = Math.atan(Math.sinh(Math.PI * (1 - 2 * (coords.y + 1) / n))) * 180 / Math.PI;
                return {{ south: south, west: west, north: north, east: east }};
            }},
            _boundsOverlap: function(a, b) {{
                return !(a.south > b.north || a.north < b.south || a.west > b.east || a.east < b.west);
            }},
            _metersToPixels: function(lat, meters, zoom) {{
                return meters / (40075016.686 * Math.cos(lat * Math.PI / 180) / Math.pow(2, zoom + 8));
            }}
        }};

        /* РІвЂќР‚РІвЂќР‚ SettlementGridLayer РІвЂќР‚РІвЂќР‚ */
        var _fallbackRadii = {{city: 5000, town: 2000, village: 700, hamlet: 300}};

        var SettlementGridLayer = L.GridLayer.extend({{
            options: {{
                pane: 'settlements',
                tileSize: 256,
                updateWhenZooming: false,
                updateWhenIdle: true
            }},

            initialize: function(opts) {{
                L.GridLayer.prototype.initialize.call(this, opts);
                this._data = {{}};
                this._allFeatures = [];
                this._dirtyFlag = 0;
                this._bucketSize = 0.2;
                this._buckets = {{}};
                this._fidCounter = 1;
                this._featureSeen = {{}};
            }},

            setData: function(key, features) {{
                if (this._data[key]) return;
                this._data[key] = features;
                for (var i = 0; i < features.length; i++) {{
                    var f = features[i];
                    if (!f._fid) f._fid = this._fidCounter++;
                    if (f.F === 'p') {{
                        var lats = f.c.map(function(c) {{ return c[0]; }});
                        var lons = f.c.map(function(c) {{ return c[1]; }});
                        f._bbox = {{ south: Math.min.apply(null, lats), north: Math.max.apply(null, lats),
                                     west: Math.min.apply(null, lons), east: Math.max.apply(null, lons) }};
                        var c0 = f.c[0];
                        var c1 = f.c[f.c.length - 1];
                        f._sig = 'p:' + f.c.length + ':' + c0[0] + ',' + c0[1] + ':' + c1[0] + ',' + c1[1];
                    }} else {{
                        var r = (_fallbackRadii[f.t] || 700) / 111000;
                        f._bbox = {{ south: f.lat - r, north: f.lat + r, west: f.lon - r, east: f.lon + r }};
                        f._sig = 'n:' + f.lat + ',' + f.lon + ':' + (f.t || '');
                    }}
                    if (this._featureSeen[f._sig]) {{
                        continue;
                    }}
                    this._featureSeen[f._sig] = 1;
                    this._allFeatures.push(f);
                    this._indexFeature(f);
                }}
                this._scheduleRedraw();
            }},

            _indexFeature: function(f) {{
                var keys = this._bucketKeys(f._bbox);
                for (var i = 0; i < keys.length; i++) {{
                    var k = keys[i];
                    if (!this._buckets[k]) this._buckets[k] = [];
                    this._buckets[k].push(f);
                }}
            }},

            _bucketKeys: function(bbox) {{
                var bs = this._bucketSize;
                var s0 = Math.floor(bbox.south / bs);
                var s1 = Math.floor(bbox.north / bs);
                var w0 = Math.floor(bbox.west / bs);
                var w1 = Math.floor(bbox.east / bs);
                var out = [];
                for (var si = s0; si <= s1; si++) {{
                    for (var wi = w0; wi <= w1; wi++) {{
                        out.push(si + ':' + wi);
                    }}
                }}
                return out;
            }},

            _featuresForBounds: function(tb) {{
                var keys = this._bucketKeys(tb);
                var seen = {{}};
                var out = [];
                for (var i = 0; i < keys.length; i++) {{
                    var arr = this._buckets[keys[i]];
                    if (!arr) continue;
                    for (var j = 0; j < arr.length; j++) {{
                        var f = arr[j];
                        if (seen[f._fid]) continue;
                        seen[f._fid] = 1;
                        out.push(f);
                    }}
                }}
                return out;
            }},

            _scheduleRedraw: function() {{
                var self = this;
                if (self._redrawRaf) return;
                self._redrawRaf = requestAnimationFrame(function() {{
                    self._redrawRaf = null;
                    self._dirtyFlag++;
                    self.redraw();
                }});
            }},

            createTile: function(coords) {{
                var tile = document.createElement('canvas');
                var sz = this.getTileSize();
                tile.width = sz.x;
                tile.height = sz.y;
                this._drawTile(tile, coords);
                return tile;
            }},

            _drawTile: function(canvas, coords) {{
                if (_mapInteracting) return canvas;
                var ctx = canvas.getContext('2d');
                var tb = _OverlayTileMixin._tileBounds(coords);
                var features = this._featuresForBounds(tb);

                for (var i = 0; i < features.length; i++) {{
                    var f = features[i];
                    if (!_OverlayTileMixin._boundsOverlap(f._bbox, tb)) continue;

                    ctx.beginPath();
                    if (f.F === 'p') {{
                        var c = f.c;
                        for (var j = 0; j < c.length; j++) {{
                            var p = _OverlayTileMixin._latLngToTilePixel(c[j][0], c[j][1], coords.z, coords.x, coords.y);
                            if (j === 0) ctx.moveTo(p[0], p[1]);
                            else ctx.lineTo(p[0], p[1]);
                        }}
                        ctx.closePath();
                    }} else {{
                        var cp = _OverlayTileMixin._latLngToTilePixel(f.lat, f.lon, coords.z, coords.x, coords.y);
                        var rPx = _OverlayTileMixin._metersToPixels(f.lat, _fallbackRadii[f.t] || 700, coords.z);
                        ctx.arc(cp[0], cp[1], rPx, 0, Math.PI * 2);
                    }}

                    ctx.fillStyle = 'rgba(239,68,68,0.16)';
                    ctx.fill('evenodd');

                    /* stroke */
                    ctx.strokeStyle = 'rgba(239,68,68,0.9)';
                    ctx.lineWidth = 1.6;
                    ctx.stroke();
                }}
            }}
        }});

        var settlementGridLayer = new SettlementGridLayer();

        function addSettlements(tileKey, features) {{
            if (settlementTileData[tileKey]) return;
            settlementTileData[tileKey] = features;
            settlementGridLayer.setData(tileKey, features);
        }}

        function toggleLayer(name) {{
            switch (name) {{
                case 'track':
                    showTrack = document.getElementById('chkTrack').checked;
                    showTrack ? map.addLayer(trackLine) : map.removeLayer(trackLine);
                    break;
                case 'waypoints':
                    showWaypoints = document.getElementById('chkWaypoints').checked;
                    waypointMarkers.forEach(function(m) {{
                        showWaypoints ? map.addLayer(m) : map.removeLayer(m);
                    }});
                    waypointIndexMarkers.forEach(function(m) {{
                        showWaypoints ? map.addLayer(m) : map.removeLayer(m);
                    }});
                    routeLines.forEach(function(l) {{
                        showWaypoints ? map.addLayer(l) : map.removeLayer(l);
                    }});
                    showWaypoints ? map.addLayer(activeWaypointLine) : map.removeLayer(activeWaypointLine);
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
                        map.addLayer(restrictedGridLayer);
                        map.addLayer(restrictedHitLayer);
                        _updateOverlayOpacity();
                    }} else {{
                        map.removeLayer(restrictedGridLayer);
                        map.removeLayer(restrictedHitLayer);
                    }}
                    break;
                case 'settlements':
                    showSettlements = document.getElementById('chkSettlements').checked;
                    if (showSettlements) {{
                        map.addLayer(settlementGridLayer);
                        _updateOverlayOpacity();
                        _requestSettlements();
                    }} else {{
                        map.removeLayer(settlementGridLayer);
                    }}
                    break;
            }}
        }}

        /* РІвЂќР‚РІвЂќР‚ Restricted Zones РІР‚вЂќ GridLayer + Hit Layer РІвЂќР‚РІвЂќР‚ */

        var RestrictedGridLayer = L.GridLayer.extend({{
            options: {{
                pane: 'restricted',
                tileSize: 256,
                updateWhenZooming: false,
                updateWhenIdle: true
            }},

            initialize: function(opts) {{
                L.GridLayer.prototype.initialize.call(this, opts);
                this._highlightId = null;
                this._editingZoneId = null;
            }},

            createTile: function(coords) {{
                var tile = document.createElement('canvas');
                var sz = this.getTileSize();
                tile.width = sz.x;
                tile.height = sz.y;
                this._drawTile(tile, coords);
                return tile;
            }},

            _drawTile: function(canvas, coords) {{
                if (_mapInteracting) return canvas;
                var ctx = canvas.getContext('2d');
                var tb = _OverlayTileMixin._tileBounds(coords);

                for (var zoneId in restrictedZones) {{
                    if (this._editingZoneId === zoneId) continue;
                    var z = restrictedZones[zoneId];
                    if (!z.points || z.points.length < 3) continue;

                    /* bbox pre-filter */
                    if (!_OverlayTileMixin._boundsOverlap(z.bbox, tb)) continue;

                    ctx.beginPath();
                    for (var j = 0; j < z.points.length; j++) {{
                        var p = _OverlayTileMixin._latLngToTilePixel(z.points[j][0], z.points[j][1], coords.z, coords.x, coords.y);
                        if (j === 0) ctx.moveTo(p[0], p[1]);
                        else ctx.lineTo(p[0], p[1]);
                    }}
                    ctx.closePath();

                    ctx.fillStyle = 'rgba(220,38,38,0.30)';
                    ctx.fill('evenodd');

                    /* dashed stroke */
                    var isHighlighted = (this._highlightId === zoneId);
                    ctx.strokeStyle = 'rgba(120,0,0,0.95)';
                    ctx.lineWidth = isHighlighted ? 3.2 : 2.1;
                    ctx.setLineDash([]);
                    ctx.stroke();
                }}
            }},

            _scheduleRedraw: function() {{
                var self = this;
                if (self._redrawRaf) return;
                self._redrawRaf = requestAnimationFrame(function() {{
                    self._redrawRaf = null;
                    self.redraw();
                }});
            }}
        }});

        var restrictedGridLayer = new RestrictedGridLayer();
        var restrictedHitLayer = L.layerGroup();

        function _computePointsBBox(points) {{
            var minLat = Infinity, minLon = Infinity, maxLat = -Infinity, maxLon = -Infinity;
            for (var i = 0; i < points.length; i++) {{
                var p = points[i];
                if (p[0] < minLat) minLat = p[0];
                if (p[0] > maxLat) maxLat = p[0];
                if (p[1] < minLon) minLon = p[1];
                if (p[1] > maxLon) maxLon = p[1];
            }}
            return {{ south: minLat, north: maxLat, west: minLon, east: maxLon }};
        }}

        function _createHitPolygon(zoneId, points, name) {{
            var hitPoly = L.polygon(points, {{
                fillOpacity: 0, stroke: false, interactive: true
            }});
            if (name) {{
                hitPoly.bindTooltip(name, {{ permanent: false, direction: 'center' }});
            }}
            hitPoly.on('contextmenu', function(e) {{
                L.DomEvent.stopPropagation(e);
                L.DomEvent.preventDefault(e);
                if (bridge && !drawingMode) {{
                    bridge.onZoneContextMenu(zoneId, e.originalEvent.screenX, e.originalEvent.screenY);
                }}
            }});
            hitPoly.on('dblclick', function(e) {{
                L.DomEvent.stopPropagation(e);
                L.DomEvent.preventDefault(e);
                if (bridge && !drawingMode) bridge.onZoneDoubleClicked(zoneId);
            }});
            hitPoly.on('click', function(e) {{
                L.DomEvent.stopPropagation(e);
            }});
            hitPoly.addTo(restrictedHitLayer);
            return hitPoly;
        }}

        function addRestrictedZone(zoneId, points, name) {{
            if (restrictedZones[zoneId]) removeRestrictedZone(zoneId);
            var hitPoly = _createHitPolygon(zoneId, points, name);
            restrictedZones[zoneId] = {{
                points: points,
                bbox: _computePointsBBox(points),
                name: name,
                hitPoly: hitPoly
            }};
            restrictedGridLayer._scheduleRedraw();
        }}

        function removeRestrictedZone(zoneId) {{
            var z = restrictedZones[zoneId];
            if (!z) return;
            if (editingZoneId === zoneId) disableZoneEditing();
            if (z.hitPoly) restrictedHitLayer.removeLayer(z.hitPoly);
            delete restrictedZones[zoneId];
            restrictedGridLayer._scheduleRedraw();
        }}

        function updateRestrictedZone(zoneId, points) {{
            var z = restrictedZones[zoneId];
            if (!z) return;
            z.points = points;
            z.bbox = _computePointsBBox(points);
            if (z.hitPoly) z.hitPoly.setLatLngs(points);
            restrictedGridLayer._scheduleRedraw();
        }}

        function clearRestrictedZones() {{
            disableZoneEditing();
            for (var id in restrictedZones) {{
                if (restrictedZones[id].hitPoly) restrictedHitLayer.removeLayer(restrictedZones[id].hitPoly);
            }}
            restrictedZones = {{}};
            restrictedGridLayer._scheduleRedraw();
        }}

        function highlightZone(zoneId) {{
            restrictedGridLayer._highlightId = zoneId;
            restrictedGridLayer._scheduleRedraw();
        }}

        function unhighlightZone(zoneId) {{
            if (restrictedGridLayer._highlightId === zoneId) {{
                restrictedGridLayer._highlightId = null;
                restrictedGridLayer._scheduleRedraw();
            }}
        }}

        /* РІвЂќР‚РІвЂќР‚ Drawing Mode РІвЂќР‚РІвЂќР‚ */

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
            // If 3+ points and click is near the first vertex РІР‚вЂќ finish
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

        /* РІвЂќР‚РІвЂќР‚ Zone Vertex Editing РІвЂќР‚РІвЂќР‚ */

        var _editTempPolygon = null;

        function _notifyVerticesUpdated(zoneId) {{
            if (_editTempPolygon && bridge) {{
                var pts = _editTempPolygon.getLatLngs()[0].map(function(ll) {{
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
            /* hide zone in GridLayer, show temp polygon for editing */
            restrictedGridLayer._editingZoneId = zoneId;
            restrictedGridLayer._scheduleRedraw();
            _editTempPolygon = L.polygon(z.points, {{
                fillColor: 'rgba(220,38,38,0.30)',
                fillOpacity: 1,
                color: 'rgba(100,0,0,0.9)',
                weight: 2.5,
                dashArray: null,
                fill: true,
                interactive: false
            }}).addTo(map);
            _refreshEditingMarkers(zoneId);
        }}

        function disableZoneEditing() {{
            if (!editingZoneId) return;
            editingMarkers.forEach(function(m) {{ map.removeLayer(m); }});
            editingMarkers = [];
            /* extract final coords from temp polygon, update zone data */
            if (_editTempPolygon) {{
                var z = restrictedZones[editingZoneId];
                if (z) {{
                    var finalPts = _editTempPolygon.getLatLngs()[0].map(function(ll) {{
                        return [ll.lat, ll.lng];
                    }});
                    z.points = finalPts;
                    if (z.hitPoly) z.hitPoly.setLatLngs(finalPts);
                }}
                map.removeLayer(_editTempPolygon);
                _editTempPolygon = null;
            }}
            restrictedGridLayer._editingZoneId = null;
            restrictedGridLayer._scheduleRedraw();
            editingZoneId = null;
        }}

        function _refreshEditingMarkers(zoneId) {{
            editingMarkers.forEach(function(m) {{ map.removeLayer(m); }});
            editingMarkers = [];
            if (!_editTempPolygon) return;
            var latlngs = _editTempPolygon.getLatLngs()[0];

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
                    if (!_editTempPolygon) return;
                    var cur = _editTempPolygon.getLatLngs()[0];
                    if (cur.length > 3) {{
                        cur.splice(idx, 1);
                        _editTempPolygon.setLatLngs([cur]);
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
                if (_editTempPolygon) {{
                    var latlngs = _editTempPolygon.getLatLngs()[0];
                    latlngs[marker._vertexIdx] = e.latlng;
                    _editTempPolygon.setLatLngs(latlngs);
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
                if (!dragging || !_editTempPolygon) return;
                if (!inserted) {{
                    var latlngs = _editTempPolygon.getLatLngs()[0];
                    latlngs.splice(insertIdx, 0, e.latlng);
                    _editTempPolygon.setLatLngs([latlngs]);
                    inserted = true;
                    marker.setStyle({{ radius: 7, fillColor: '{Colors.PRIMARY}', fillOpacity: 1 }});
                }}
                marker.setLatLng(e.latlng);
                var latlngs = _editTempPolygon.getLatLngs()[0];
                latlngs[insertIdx] = e.latlng;
                _editTempPolygon.setLatLngs([latlngs]);
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

        /* РІвЂќР‚РІвЂќР‚ Drawing event integration РІвЂќР‚РІвЂќР‚ */

        document.addEventListener('keydown', function(e) {{
            if (e.key === 'Escape' && drawingMode) cancelDrawing();
            if (e.key === 'Escape' && editingZoneId) {{
                disableZoneEditing();
                if (bridge) bridge.onZoneEditingFinished();
            }}
        }});

        function _headingEndpoint(lat, lon, headingDeg, meters) {{
            var r = headingDeg * Math.PI / 180;
            var dLat = (meters * Math.cos(r)) / 111320;
            var dLon = (meters * Math.sin(r)) / (111320 * Math.cos(lat * Math.PI / 180));
            return [lat + dLat, lon + dLon];
        }}

        function _aircraftShape(lat, lon, headingDeg, sizeMeters) {{
            var h = headingDeg * Math.PI / 180;
            var c = Math.cos(h), s = Math.sin(h);
            var kLon = 1 / (111320 * Math.cos(lat * Math.PI / 180));
            function pt(fwd, right) {{
                var north = fwd * c - right * s;
                var east = fwd * s + right * c;
                return [lat + north / 111320, lon + east * kLon];
            }}
            var u = sizeMeters;
            return [
                pt( 1.20 * u,  0.00 * u),  // nose
                pt( 0.20 * u, -0.18 * u),  // left root
                pt(-0.05 * u, -0.82 * u),  // left wing tip
                pt(-0.24 * u, -0.20 * u),  // left rear root
                pt(-0.95 * u, -0.12 * u),  // tail left
                pt(-0.95 * u,  0.12 * u),  // tail right
                pt(-0.24 * u,  0.20 * u),  // right rear root
                pt(-0.05 * u,  0.82 * u),  // right wing tip
                pt( 0.20 * u,  0.18 * u)   // right root
            ];
        }}

        function _aircraftIconHtml() {{
            // Bootstrap Icons airplane-fill (MIT): https://icons.getbootstrap.com/icons/airplane-fill/
            return `<div class="aircraft-icon-wrap">
                <svg class="aircraft-icon-svg" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" aria-hidden="true">
                    <path d="M6.428 1.151C6.708.591 7.213 0 8 0s1.292.592 1.572 1.151C9.861 1.73 10 2.431 10 3v3.691l5.17 2.585a1.5 1.5 0 0 1 .83 1.342V12a.5.5 0 0 1-.582.493l-5.507-.918-.375 2.253 1.318 1.318A.5.5 0 0 1 10.5 16h-5a.5.5 0 0 1-.354-.854l1.319-1.318-.376-2.253-5.507.918A.5.5 0 0 1 0 12v-1.382a1.5 1.5 0 0 1 .83-1.342L6 6.691V3c0-.568.14-1.271.428-1.849" fill="#39FF14" stroke="#F3FFF3" stroke-width="0.55"/>
                </svg>
            </div>`;
        }}

        function _setAircraftHeading(headingDeg) {{
            if (!aircraftBody) return;
            var el = aircraftBody.getElement();
            if (!el) return;
            var wrap = el.querySelector('.aircraft-icon-wrap');
            if (wrap) wrap.style.transform = 'rotate(' + headingDeg.toFixed(1) + 'deg)';
        }}

        function _ensureAircraftLayers(lat, lon) {{
            if (!aircraftHalo) {{
                aircraftHalo = L.circleMarker([lat, lon], {{
                    renderer: _aircraftRenderer,
                    radius: 7,
                    color: '#E6FFE8',
                    weight: 1.5,
                    opacity: 1,
                    fillColor: '#63FF4A',
                    fillOpacity: 0.34,
                    interactive: false
                }}).addTo(map);
            }}
            if (!aircraftBody) {{
                aircraftBody = L.marker([lat, lon], {{
                    icon: L.divIcon({{
                        className: 'aircraft-icon',
                        html: _aircraftIconHtml(),
                        iconSize: [30, 30],
                        iconAnchor: [15, 15]
                    }}),
                    interactive: false,
                    keyboard: false
                }}).addTo(map);
                _setAircraftHeading(lastHeading);
            }}
        }}

        /* РІвЂќР‚РІвЂќР‚ rAF-batched aircraft update РІвЂќР‚РІвЂќР‚ */
        var _pendingAircraft = null;
        var _rafScheduled = false;

        function updateAircraft(lat, lon, heading) {{
            _pendingAircraft = [lat, lon, heading];
            if (!_rafScheduled) {{
                _rafScheduled = true;
                requestAnimationFrame(_flushAircraftUpdate);
            }}
        }}

        function _flushAircraftUpdate() {{
            var _t0 = performance.now();
            _rafScheduled = false;
            if (!_pendingAircraft) return;
            var lat = _pendingAircraft[0];
            var lon = _pendingAircraft[1];
            var heading = _pendingAircraft[2];
            _pendingAircraft = null;

            lastAircraftPos = [lat, lon];

            _ensureAircraftLayers(lat, lon);
            aircraftHalo.setLatLng([lat, lon]);
            if (Math.abs(heading - lastHeading) > 1) {{
                lastHeading = heading;
            }}
            aircraftBody.setLatLng([lat, lon]);
            _setAircraftHeading(lastHeading);

            /* track РІР‚вЂќ accumulate always, render only when map is idle */
            trackPoints.push([lat, lon]);
            if (trackPoints.length > 1200) trackPoints.splice(0, 200);
            if (_mapInteracting) {{
                _trackDirty = true;
            }} else {{
                _trackUpdateCounter++;
                if (_trackUpdateCounter >= 10) {{
                    _trackUpdateCounter = 0;
                    trackLine.setLatLngs(trackPoints);
                }}
            }}

            /* active waypoint line РІР‚вЂќ skip during map interaction */
            if (_mapInteracting) {{
                if (waypointData.length > activeWaypointIdx) _wpLineDirty = true;
            }} else {{
                _wpLineCounter++;
                if (_wpLineCounter >= 10) {{
                    _wpLineCounter = 0;
                    if (waypointData.length > activeWaypointIdx) {{
                        var wp = waypointData[activeWaypointIdx];
                        activeWaypointLine.setLatLngs([[lat, lon], [wp.lat, wp.lon]]);
                    }} else {{
                        activeWaypointLine.setLatLngs([]);
                    }}
                }}
            }}

            if (followAircraft && !_mapInteracting) {{
                var center = map.getCenter();
                var centerPx = map.latLngToContainerPoint(center);
                var posPx = map.latLngToContainerPoint([lat, lon]);
                var size = map.getSize();
                var threshold = Math.min(size.x, size.y) * 0.15;
                if (centerPx.distanceTo(posPx) > threshold) {{
                    map.panTo([lat, lon], {{animate: false}});
                }}
            }}

            var _dt = performance.now() - _t0;
            _perfUpdateMs = _dt;
            if (_dt > 8) _perfSlowFrames++;
        }}

        function setFollowMode(enabled) {{
            followAircraft = enabled;
        }}

        function setAircraftPosition(lat, lon) {{
            lastAircraftPos = [lat, lon];
            _ensureAircraftLayers(lat, lon);
            aircraftHalo.setLatLng([lat, lon]);
            aircraftBody.setLatLng([lat, lon]);
            _setAircraftHeading(lastHeading);
            trackPoints = [[lat, lon]];
            trackLine.setLatLngs(trackPoints);
            activeWaypointLine.setLatLngs([]);
            map.setView([lat, lon], map.getZoom());
        }}

        function clearTrack() {{
            trackPoints = [];
            trackLine.setLatLngs([]);
        }}

        function formatTooltip(wp, index, isActive) {{
            var actionShort = {{
                'FLYTHROUGH': '',
                'ORBIT_TURNS': 'Круг ' + wp.orbit_turns + 'x',
                'ORBIT_INFINITE': 'Круг ∞',
                'ALTITUDE': 'Высота'
            }};

            var lines = [];
            lines.push('<b>#' + (index + 1) + '</b> ' + wp.altitude + 'м');

            var action = actionShort[wp.action] || '';
            if (action) {{
                lines.push(action + (wp.orbit_radius ? ' R' + wp.orbit_radius : ''));
            }}

            if (wp.climb_enroute) {{
                lines.push('Набор');
            }}

            return lines.join('<br>');
        }}

        function _waypointVisual(index) {{
            // Keep an invisible-but-clickable canvas marker as a hit target.
            return {{ radius: 9, color: '#000000', weight: 0, opacity: 0, fillColor: '#000000', fillOpacity: 0 }};
        }}

        function _waypointIconVisual(index) {{
            var isPast = index < activeWaypointIdx;
            var isActive = index === activeWaypointIdx;
            if (isPast) {{
                return {{ fill: '#6B7280', stroke: '#D1D5DB' }};
            }}
            if (isActive) {{
                return {{ fill: '#39FF14', stroke: '#F3FFF3' }};
            }}
            return {{ fill: '#7CFF61', stroke: '#E6FFE8' }};
        }}

        function _waypointIconHtml(index) {{
            var v = _waypointIconVisual(index);
            var num = String(index + 1);
            // Bootstrap Icons geo-alt-fill (MIT): https://icons.getbootstrap.com/icons/geo-alt-fill/
            return `<span class="wp-icon-wrap">
                <svg class="wp-icon-svg" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" aria-hidden="true">
                    <path d="M8 16s6-5.686 6-10A6 6 0 0 0 2 6c0 4.314 6 10 6 10m0-7a3 3 0 1 1 0-6 3 3 0 0 1 0 6" fill="${{v.fill}}" stroke="${{v.stroke}}" stroke-width="0.65"/>
                    <text x="8" y="6.15" class="wp-icon-num">${{num}}</text>
                </svg>
            </span>`;
        }}

        function _updateWaypointIndexIcon(marker, index) {{
            marker.setIcon(L.divIcon({{
                className: 'wp-index-icon',
                html: _waypointIconHtml(index),
                iconSize: [24, 32],
                iconAnchor: [12, 32]
            }}));
        }}

        function _routeVisual(index) {{
            var isPast = index < activeWaypointIdx - 1;
            var isActive = index === activeWaypointIdx - 1;
            if (isPast) {{
                return {{ color: '{Colors.TEXT_TERTIARY}', weight: 2, opacity: 0.4, dashArray: null }};
            }}
            if (isActive) {{
                return {{ color: '#66FF45', weight: 3, opacity: 0.95, dashArray: null }};
            }}
            return {{ color: '#A3FF89', weight: 2, opacity: 0.8, dashArray: '8, 8' }};
        }}

        function _applyWaypointStyle(marker, index) {{
            var s = _waypointVisual(index);
            marker.setStyle(s);
            if (index === activeWaypointIdx) marker.bringToFront();
        }}

        function _applyRouteStyle(line, index) {{
            line.setStyle(_routeVisual(index));
        }}

        function setWaypoints(waypoints, activeIdx) {{
            var _swT0 = performance.now();
            waypointMarkers.forEach(m => map.removeLayer(m));
            waypointMarkers = [];
            waypointIndexMarkers.forEach(m => map.removeLayer(m));
            waypointIndexMarkers = [];
            routeLines.forEach(l => map.removeLayer(l));
            routeLines = [];

            waypointData = waypoints;
            if (activeIdx !== undefined) activeWaypointIdx = activeIdx;

            waypoints.forEach((wp, i) => {{
                var marker = L.circleMarker([wp.lat, wp.lon], {{
                    renderer: _routeRenderer,
                    interactive: true
                }});
                _applyWaypointStyle(marker, i);
                if (showWaypoints) marker.addTo(map);

                marker.bindTooltip(formatTooltip(wp, i, i === activeWaypointIdx), {{
                    permanent: false,
                    direction: 'top',
                    offset: [0, -10],
                    className: ''
                }});

                waypointMarkers.push(marker);

                var idxMarker = L.marker([wp.lat, wp.lon], {{
                    icon: L.divIcon({{
                        className: 'wp-index-icon',
                        html: _waypointIconHtml(i),
                        iconSize: [24, 32],
                        iconAnchor: [12, 32]
                    }}),
                    interactive: false,
                    keyboard: false
                }});
                if (showWaypoints) idxMarker.addTo(map);
                waypointIndexMarkers.push(idxMarker);
            }});

            for (var i = 0; i < waypoints.length - 1; i++) {{
                var line = L.polyline([
                    [waypoints[i].lat, waypoints[i].lon],
                    [waypoints[i + 1].lat, waypoints[i + 1].lon]
                ], {{
                    renderer: _routeRenderer,
                    interactive: false
                }});
                _applyRouteStyle(line, i);
                if (showWaypoints) line.addTo(map);
                routeLines.push(line);
            }}

            updateActiveWaypointLine();
            var _swDt = performance.now() - _swT0;
            if (_swDt > 5) console.warn('[PERF] setWaypoints: ' + _swDt.toFixed(1) + 'ms for ' + waypoints.length + ' wpts');
        }}

        function updateActiveWaypoint(idx) {{
            var prev = activeWaypointIdx;
            activeWaypointIdx = idx;
            if (!waypointData || waypointData.length === 0) return;
            if (prev === idx) return;

            for (var i = 0; i < waypointMarkers.length; i++) {{
                _applyWaypointStyle(waypointMarkers[i], i);
                waypointMarkers[i].setTooltipContent(formatTooltip(waypointData[i], i, i === activeWaypointIdx));
                _updateWaypointIndexIcon(waypointIndexMarkers[i], i);
            }}
            for (var j = 0; j < routeLines.length; j++) {{
                _applyRouteStyle(routeLines[j], j);
            }}
            updateActiveWaypointLine();
        }}

        function updateActiveWaypointLine() {{
            if (lastAircraftPos && waypointData.length > activeWaypointIdx) {{
                var wp = waypointData[activeWaypointIdx];
                activeWaypointLine.setLatLngs([lastAircraftPos, [wp.lat, wp.lon]]);
            }} else {{
                activeWaypointLine.setLatLngs([]);
            }}
        }}

        function setRouteConflicts(conflicts) {{
            // Clear existing conflict indicators
            conflictLines.forEach(l => map.removeLayer(l));
            conflictLines = [];
            conflictMarkers.forEach(m => map.removeLayer(m));
            conflictMarkers = [];

            if (!waypointData || waypointData.length < 2) return;

            conflicts.forEach(function(c) {{
                var fi = c.from_idx;
                var ti = c.to_idx;
                if (fi >= waypointData.length || ti >= waypointData.length) return;

                var wp1 = waypointData[fi];
                var wp2 = waypointData[ti];

                // Red dashed overlay on conflicting segment
                var line = L.polyline([
                    [wp1.lat, wp1.lon],
                    [wp2.lat, wp2.lon]
                ], {{
                    renderer: _routeRenderer,
                    interactive: false,
                    color: '#EF4444',
                    weight: 4,
                    opacity: 0.8,
                    dashArray: '6, 8'
                }});
                line.addTo(map);
                conflictLines.push(line);

                // Warning marker at segment midpoint (canvas circle)
                var midLat = (wp1.lat + wp2.lat) / 2;
                var midLon = (wp1.lon + wp2.lon) / 2;
                var marker = L.circleMarker([midLat, midLon], {{
                    renderer: _routeRenderer,
                    radius: 7,
                    color: '#fff',
                    weight: 2,
                    fillColor: '#EF4444',
                    fillOpacity: 0.95
                }});
                if (c.reason) marker.bindTooltip(c.reason, {{ direction: 'top', offset: [0, -14] }});
                marker.addTo(map);
                conflictMarkers.push(marker);
            }});
        }}

        function setAvoidancePath(points) {{
            if (avoidanceLine) {{
                map.removeLayer(avoidanceLine);
                avoidanceLine = null;
            }}
            if (!points || points.length < 2) return;

            var latlngs = points.map(function(p) {{ return [p.lat, p.lon]; }});
            avoidanceLine = L.polyline(latlngs, {{
                renderer: _routeRenderer,
                interactive: false,
                color: '#F97316',
                weight: 3,
                opacity: 0.85,
                dashArray: '8, 6'
            }});
            avoidanceLine.addTo(map);
        }}

        function clearRouteConflicts() {{
            conflictLines.forEach(l => map.removeLayer(l));
            conflictLines = [];
            conflictMarkers.forEach(m => map.removeLayer(m));
            conflictMarkers = [];
            if (avoidanceLine) {{
                map.removeLayer(avoidanceLine);
                avoidanceLine = null;
            }}
        }}

        function addWaypoint(lat, lon, index, wpData) {{
            var marker = L.circleMarker([lat, lon], {{
                renderer: _routeRenderer,
                interactive: true
            }});
            _applyWaypointStyle(marker, index - 1);
            if (showWaypoints) marker.addTo(map);

            var tooltip = wpData ? formatTooltip(wpData, index - 1, false) : 'Точка ' + index;
            marker.bindTooltip(tooltip, {{
                permanent: false,
                direction: 'top',
                offset: [0, -10]
            }});

            waypointMarkers.push(marker);
            var idxMarker = L.marker([lat, lon], {{
                icon: L.divIcon({{
                    className: 'wp-index-icon',
                    html: _waypointIconHtml(index - 1),
                    iconSize: [24, 32],
                    iconAnchor: [12, 32]
                }}),
                interactive: false,
                keyboard: false
            }});
            if (showWaypoints) idxMarker.addTo(map);
            waypointIndexMarkers.push(idxMarker);

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
                    html: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24">
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

        map.on('moveend', function() {{ _requestSettlements(); }});
        map.on('zoomend', function() {{ _requestSettlements(); _updateOverlayOpacity(); }});

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

        var _lastMouseBridgeTs = 0;
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
                var _now = performance.now();
                if (!_mapInteracting && (_now - _lastMouseBridgeTs) >= 40) {{
                    _lastMouseBridgeTs = _now;
                    bridge.onMouseMove(e.latlng.lat, e.latlng.lng);
                }}
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
        now = time.monotonic()
        dt = now - self._last_aircraft_ts
        dlat = abs(lat - self._last_aircraft_lat)
        dlon = abs(lon - self._last_aircraft_lon)
        # Skip IPC if <150ms and position barely moved (~1m)
        if dt < 0.15 and dlat < 1e-5 and dlon < 1e-5:
            return
        self._last_aircraft_ts = now
        self._last_aircraft_lat = lat
        self._last_aircraft_lon = lon
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

    def set_route_conflicts(self, conflicts: list):
        c_json = json.dumps(conflicts)
        self.web_view.page().runJavaScript(f"setRouteConflicts({c_json});")

    def set_avoidance_path(self, points: list):
        p_json = json.dumps(points)
        self.web_view.page().runJavaScript(f"setAvoidancePath({p_json});")

    def clear_route_conflicts(self):
        self.web_view.page().runJavaScript("clearRouteConflicts();")

    # РІвЂќР‚РІвЂќР‚ Restricted Zones РІвЂќР‚РІвЂќР‚

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
