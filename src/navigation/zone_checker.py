import json
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from src.core.config import ZoneAvoidanceConfig
from src.navigation.zone_manager import ZoneManager
from src.navigation.calculations import (
    point_in_polygon, polygon_buffer, segment_intersects_polygon,
    circle_to_polygon, haversine_distance, meters_to_lat_offset, meters_to_lon_offset,
    along_track_fraction, cross_track_distance,
)

logger = logging.getLogger(__name__)

_SETTLEMENT_FALLBACK_RADII: Dict[str, float] = {
    'city': 5000.0,
    'town': 2000.0,
    'village': 700.0,
    'hamlet': 300.0,
}


@dataclass
class ObstaclePolygon:
    points: List[List[float]]   # [[lat, lon], ...]
    buffer: float               # meters
    source: str                 # "nofly" | "settlement"
    name: str
    min_altitude: Optional[float]  # None = restricted at all altitudes


@dataclass
class _CachedSettlement:
    """Settlement feature with precomputed bounding box for spatial filtering."""
    feat: dict
    min_lat: float
    max_lat: float
    min_lon: float
    max_lon: float


class ZoneChecker:
    def __init__(self, zone_manager: ZoneManager, config: ZoneAvoidanceConfig):
        self._zone_manager = zone_manager
        self._config = config
        self._settlements: List[_CachedSettlement] = []
        self._cache_dir: Optional[Path] = None

    def set_config(self, config: ZoneAvoidanceConfig):
        self._config = config

    def set_settlement_cache_dir(self, cache_dir: Path):
        self._cache_dir = cache_dir
        self._load_settlement_cache()

    def _load_settlement_cache(self):
        self._settlements = []
        if not self._cache_dir or not self._cache_dir.exists():
            return
        try:
            for f in self._cache_dir.glob('*.json'):
                with open(f, 'r', encoding='utf-8') as fh:
                    features = json.load(fh)
                for feat in features:
                    cached = self._make_cached_settlement(feat)
                    if cached:
                        self._settlements.append(cached)
        except Exception as e:
            logger.warning("Failed to load settlement cache: %s", e)

    def _make_cached_settlement(self, feat: dict) -> Optional[_CachedSettlement]:
        """Precompute bounding box for fast spatial filtering."""
        if feat.get('F') == 'p' and 'c' in feat:
            coords = feat['c']
            if len(coords) < 3:
                return None
            lats = [c[0] for c in coords]
            lons = [c[1] for c in coords]
            return _CachedSettlement(feat, min(lats), max(lats), min(lons), max(lons))
        elif feat.get('F') == 'n':
            lat = feat.get('lat')
            lon = feat.get('lon')
            if lat is None or lon is None:
                return None
            stype = feat.get('t', 'hamlet')
            radius = _SETTLEMENT_FALLBACK_RADII.get(stype, 500.0)
            dlat = meters_to_lat_offset(radius)
            dlon = meters_to_lon_offset(radius, lat)
            return _CachedSettlement(feat, lat - dlat, lat + dlat, lon - dlon, lon + dlon)
        return None

    def reload_settlements(self):
        self._load_settlement_cache()

    def add_settlement_features(self, features: list):
        """Incrementally add new settlement features (from freshly fetched tiles)."""
        added = 0
        for feat in features:
            cached = self._make_cached_settlement(feat)
            if cached:
                self._settlements.append(cached)
                added += 1
        if added:
            logger.debug("[ZoneChecker] Added %d settlements incrementally (total: %d)",
                         added, len(self._settlements))

    def _get_settlement_polygon(self, feat: dict) -> Optional[List[List[float]]]:
        """Extract polygon from settlement feature."""
        if feat.get('F') == 'p' and 'c' in feat:
            return feat['c']
        elif feat.get('F') == 'n':
            lat = feat.get('lat')
            lon = feat.get('lon')
            stype = feat.get('t', 'hamlet')
            if lat is not None and lon is not None:
                radius = _SETTLEMENT_FALLBACK_RADII.get(stype, 500.0)
                return circle_to_polygon(lat, lon, radius, 16)
        return None

    def _settlements_in_bbox(self, min_lat: float, max_lat: float,
                             min_lon: float, max_lon: float) -> List[_CachedSettlement]:
        """Fast bbox filter — returns only settlements that overlap the query box."""
        result = []
        for s in self._settlements:
            if s.max_lat < min_lat or s.min_lat > max_lat:
                continue
            if s.max_lon < min_lon or s.min_lon > max_lon:
                continue
            result.append(s)
        return result

    def is_point_restricted(self, lat: float, lon: float,
                            altitude: float) -> Tuple[bool, Optional[str]]:
        """Check if point is in a restricted area.
        Returns (restricted, reason_string)."""
        # Check nofly zones
        for zone in self._zone_manager.get_all_zones():
            mode = zone.avoid_mode if zone.avoid_mode else self._config.nofly_mode
            if mode == "disabled":
                continue

            buf = zone.buffer if zone.buffer is not None else self._config.nofly_buffer

            if mode == "below_altitude" and zone.altitude is not None:
                if altitude >= zone.altitude:
                    continue

            poly = zone.points
            if buf > 0:
                poly = polygon_buffer(poly, buf)
            if point_in_polygon(lat, lon, poly):
                reason = f"Запретная зона: {zone.name or zone.id}"
                if mode == "below_altitude" and zone.altitude:
                    reason += f" (ниже {zone.altitude:.0f} м)"
                return True, reason

        # Check settlements — bbox filter first
        if self._config.settlement_mode != "disabled":
            if self._config.settlement_mode == "below_altitude":
                if altitude >= self._config.settlement_min_altitude:
                    return False, None

            # Expand search box by buffer
            buf = self._config.settlement_buffer
            dlat = meters_to_lat_offset(buf) if buf > 0 else 0.01
            dlon = meters_to_lon_offset(buf, lat) if buf > 0 else 0.01
            nearby = self._settlements_in_bbox(lat - dlat, lat + dlat, lon - dlon, lon + dlon)

            for cached in nearby:
                poly = self._get_settlement_polygon(cached.feat)
                if poly is None:
                    continue
                buf_poly = poly
                if self._config.settlement_buffer > 0:
                    buf_poly = polygon_buffer(poly, self._config.settlement_buffer)
                if point_in_polygon(lat, lon, buf_poly):
                    stype = cached.feat.get('t', 'населённый пункт')
                    reason = f"Населённый пункт ({stype})"
                    if self._config.settlement_mode == "below_altitude":
                        reason += f" (ниже {self._config.settlement_min_altitude:.0f} м)"
                    return True, reason

        return False, None

    def segment_intersects_obstacles(self, lat1: float, lon1: float,
                                     lat2: float, lon2: float,
                                     altitude: float) -> bool:
        """Check if segment crosses any active obstacle.
        Uses bbox prefilter for settlements."""
        # NoFly zones (few, check all)
        for zone in self._zone_manager.get_all_zones():
            mode = zone.avoid_mode if zone.avoid_mode else self._config.nofly_mode
            if mode == "disabled":
                continue
            if mode == "below_altitude" and zone.altitude is not None:
                if altitude >= zone.altitude:
                    continue
            buf = zone.buffer if zone.buffer is not None else self._config.nofly_buffer
            poly = zone.points
            if buf > 0:
                poly = polygon_buffer(poly, buf)
            if segment_intersects_polygon(lat1, lon1, lat2, lon2, poly):
                return True

        # Settlements — bbox filter
        if self._config.settlement_mode != "disabled":
            if self._config.settlement_mode == "below_altitude":
                if altitude >= self._config.settlement_min_altitude:
                    return False

            buf = self._config.settlement_buffer
            min_lat = min(lat1, lat2) - meters_to_lat_offset(buf + 1000)
            max_lat = max(lat1, lat2) + meters_to_lat_offset(buf + 1000)
            mid_lat = (lat1 + lat2) / 2
            min_lon = min(lon1, lon2) - meters_to_lon_offset(buf + 1000, mid_lat)
            max_lon = max(lon1, lon2) + meters_to_lon_offset(buf + 1000, mid_lat)

            nearby = self._settlements_in_bbox(min_lat, max_lat, min_lon, max_lon)
            for cached in nearby:
                poly = self._get_settlement_polygon(cached.feat)
                if poly is None:
                    continue
                if self._config.settlement_buffer > 0:
                    poly = polygon_buffer(poly, self._config.settlement_buffer)
                if segment_intersects_polygon(lat1, lon1, lat2, lon2, poly):
                    return True

        return False

    def get_active_obstacles(self, altitude: float,
                             bbox: Optional[Tuple[float, float, float, float]] = None
                             ) -> List[ObstaclePolygon]:
        """Collect active obstacle polygons for given altitude.
        bbox: optional (min_lat, max_lat, min_lon, max_lon) for spatial filtering."""
        obstacles = []

        # NoFly zones (always all — typically few)
        for zone in self._zone_manager.get_all_zones():
            mode = zone.avoid_mode if zone.avoid_mode else self._config.nofly_mode
            if mode == "disabled":
                continue
            if mode == "below_altitude" and zone.altitude is not None:
                if altitude >= zone.altitude:
                    continue

            buf = zone.buffer if zone.buffer is not None else self._config.nofly_buffer
            obstacles.append(ObstaclePolygon(
                points=zone.points,
                buffer=buf,
                source="nofly",
                name=zone.name or zone.id,
                min_altitude=zone.altitude if mode == "below_altitude" else None,
            ))

        # Settlements — spatial filter required
        if self._config.settlement_mode != "disabled":
            if self._config.settlement_mode == "below_altitude":
                if altitude >= self._config.settlement_min_altitude:
                    return obstacles

            if bbox:
                nearby = self._settlements_in_bbox(*bbox)
            else:
                nearby = self._settlements  # no bbox = all (legacy fallback)

            min_alt = self._config.settlement_min_altitude if self._config.settlement_mode == "below_altitude" else None
            for cached in nearby:
                poly = self._get_settlement_polygon(cached.feat)
                if poly is None:
                    continue
                stype = cached.feat.get('t', 'settlement')
                obstacles.append(ObstaclePolygon(
                    points=poly,
                    buffer=self._config.settlement_buffer,
                    source="settlement",
                    name=stype,
                    min_altitude=min_alt,
                ))

        return obstacles

    def get_buffered_obstacles(self, altitude: float,
                               bbox: Optional[Tuple[float, float, float, float]] = None
                               ) -> List[List[List[float]]]:
        """Get buffered polygons for visibility graph / intersection checks."""
        result = []
        for obs in self.get_active_obstacles(altitude, bbox=bbox):
            if obs.buffer > 0:
                result.append(polygon_buffer(obs.points, obs.buffer))
            else:
                result.append(obs.points)
        return result

    def segment_intersects_obstacles_climb(self, lat1: float, lon1: float,
                                            lat2: float, lon2: float,
                                            start_alt: float, end_alt: float) -> bool:
        """Check segment intersection with altitude interpolation for climb_enroute.
        For below_altitude zones, interpolate altitude along the segment and skip
        zones where the aircraft is above the zone limit at that point."""
        # NoFly zones
        for zone in self._zone_manager.get_all_zones():
            mode = zone.avoid_mode if zone.avoid_mode else self._config.nofly_mode
            if mode == "disabled":
                continue

            buf = zone.buffer if zone.buffer is not None else self._config.nofly_buffer
            poly = zone.points
            if buf > 0:
                poly = polygon_buffer(poly, buf)

            if mode == "below_altitude" and zone.altitude is not None:
                # Interpolate altitude at zone centroid position along segment
                cx = sum(v[0] for v in poly) / len(poly)
                cy = sum(v[1] for v in poly) / len(poly)
                frac = along_track_fraction(cx, cy, lat1, lon1, lat2, lon2)
                alt_at_zone = start_alt + (end_alt - start_alt) * frac
                if alt_at_zone >= zone.altitude:
                    continue

            if segment_intersects_polygon(lat1, lon1, lat2, lon2, poly):
                return True

        # Settlements
        if self._config.settlement_mode != "disabled":
            # For climb: check with minimum altitude (most conservative for settlements)
            min_alt = min(start_alt, end_alt)
            if self._config.settlement_mode == "below_altitude":
                if min_alt >= self._config.settlement_min_altitude:
                    return False

            buf = self._config.settlement_buffer
            min_lat = min(lat1, lat2) - meters_to_lat_offset(buf + 1000)
            max_lat = max(lat1, lat2) + meters_to_lat_offset(buf + 1000)
            mid_lat = (lat1 + lat2) / 2
            min_lon = min(lon1, lon2) - meters_to_lon_offset(buf + 1000, mid_lat)
            max_lon = max(lon1, lon2) + meters_to_lon_offset(buf + 1000, mid_lat)

            nearby = self._settlements_in_bbox(min_lat, max_lat, min_lon, max_lon)
            for cached in nearby:
                poly = self._get_settlement_polygon(cached.feat)
                if poly is None:
                    continue
                if self._config.settlement_buffer > 0:
                    poly = polygon_buffer(poly, self._config.settlement_buffer)

                if self._config.settlement_mode == "below_altitude":
                    cx = sum(v[0] for v in poly) / len(poly)
                    cy = sum(v[1] for v in poly) / len(poly)
                    frac = along_track_fraction(cx, cy, lat1, lon1, lat2, lon2)
                    alt_at_settlement = start_alt + (end_alt - start_alt) * frac
                    if alt_at_settlement >= self._config.settlement_min_altitude:
                        continue

                if segment_intersects_polygon(lat1, lon1, lat2, lon2, poly):
                    return True

        return False

    def get_buffered_obstacles_climb(self, start_alt: float, end_alt: float,
                                      bbox: Optional[Tuple[float, float, float, float]] = None,
                                      start_lat: float = 0, start_lon: float = 0,
                                      end_lat: float = 0, end_lon: float = 0
                                      ) -> List[List[List[float]]]:
        """Get buffered obstacles with altitude profile — skip zones the aircraft
        will fly over at that segment position."""
        result = []

        for zone in self._zone_manager.get_all_zones():
            mode = zone.avoid_mode if zone.avoid_mode else self._config.nofly_mode
            if mode == "disabled":
                continue

            if mode == "below_altitude" and zone.altitude is not None:
                cx = sum(v[0] for v in zone.points) / len(zone.points)
                cy = sum(v[1] for v in zone.points) / len(zone.points)
                frac = along_track_fraction(cx, cy, start_lat, start_lon, end_lat, end_lon)
                alt_at_zone = start_alt + (end_alt - start_alt) * frac
                if alt_at_zone >= zone.altitude:
                    continue

            buf = zone.buffer if zone.buffer is not None else self._config.nofly_buffer
            if buf > 0:
                result.append(polygon_buffer(zone.points, buf))
            else:
                result.append(zone.points)

        # Settlements
        if self._config.settlement_mode != "disabled":
            min_alt = min(start_alt, end_alt)
            if self._config.settlement_mode == "below_altitude":
                if min_alt >= self._config.settlement_min_altitude:
                    return result

            if bbox:
                nearby = self._settlements_in_bbox(*bbox)
            else:
                nearby = self._settlements

            for cached in nearby:
                poly = self._get_settlement_polygon(cached.feat)
                if poly is None:
                    continue

                if self._config.settlement_mode == "below_altitude":
                    cx = sum(v[0] for v in poly) / len(poly)
                    cy = sum(v[1] for v in poly) / len(poly)
                    frac = along_track_fraction(cx, cy, start_lat, start_lon, end_lat, end_lon)
                    alt_at_settlement = start_alt + (end_alt - start_alt) * frac
                    if alt_at_settlement >= self._config.settlement_min_altitude:
                        continue

                if self._config.settlement_buffer > 0:
                    result.append(polygon_buffer(poly, self._config.settlement_buffer))
                else:
                    result.append(poly)

        return result
