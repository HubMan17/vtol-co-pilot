"""
Settlement boundary loader — fetches from Overpass API with tile caching.
"""
import json
import math
import os
import threading
import time
import urllib.request
import urllib.parse

from PyQt5.QtCore import QObject, pyqtSignal, QThread


_TILE_GRID = 0.1  # ~10 km tile grid
_CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'cache', 'settlements_v5')
_DP_TOLERANCE = 0.0005  # ~55 m - Douglas-Peucker simplification

_OVERPASS_ENDPOINTS = [
    'https://overpass-api.de/api/interpreter',
    'https://overpass.kumi.systems/api/interpreter',
]


def _close(a, b, eps=1e-6):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


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
        polys = []
        nodes = []
        poly_bboxes = []

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

        # Filter out nodes that already have a polygon
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
        ring = list(segments[0])
        remaining = segments[1:]
        max_iter = len(remaining) * 2
        i = 0
        while remaining and i < max_iter:
            i += 1
            matched = False
            for idx, seg in enumerate(remaining):
                if _close(ring[-1], seg[0]):
                    ring.extend(seg[1:])
                    remaining.pop(idx)
                    matched = True
                    break
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
