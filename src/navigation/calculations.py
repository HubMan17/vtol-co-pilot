import math
from typing import List, Tuple

import numpy as np

EARTH_RADIUS = 6371000


def haversine_distance(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    lat1_rad = math.radians(lat1)
    lat2_rad = math.radians(lat2)
    delta_lat = math.radians(lat2 - lat1)
    delta_lon = math.radians(lon2 - lon1)

    a = math.sin(delta_lat / 2) ** 2 + \
        math.cos(lat1_rad) * math.cos(lat2_rad) * math.sin(delta_lon / 2) ** 2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))

    return EARTH_RADIUS * c


def bearing_to(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    lat1_rad = math.radians(lat1)
    lat2_rad = math.radians(lat2)
    delta_lon = math.radians(lon2 - lon1)

    x = math.sin(delta_lon) * math.cos(lat2_rad)
    y = math.cos(lat1_rad) * math.sin(lat2_rad) - \
        math.sin(lat1_rad) * math.cos(lat2_rad) * math.cos(delta_lon)

    bearing = math.atan2(x, y)
    return (math.degrees(bearing) + 360) % 360


def cross_track_distance(pos_lat: float, pos_lon: float,
                         wp1_lat: float, wp1_lon: float,
                         wp2_lat: float, wp2_lon: float) -> float:
    d13 = haversine_distance(wp1_lat, wp1_lon, pos_lat, pos_lon) / EARTH_RADIUS
    brng13 = math.radians(bearing_to(wp1_lat, wp1_lon, pos_lat, pos_lon))
    brng12 = math.radians(bearing_to(wp1_lat, wp1_lon, wp2_lat, wp2_lon))

    xtd = math.asin(math.sin(d13) * math.sin(brng13 - brng12))
    return xtd * EARTH_RADIUS


def along_track_distance(pos_lat: float, pos_lon: float,
                         wp1_lat: float, wp1_lon: float,
                         wp2_lat: float, wp2_lon: float) -> float:
    d13 = haversine_distance(wp1_lat, wp1_lon, pos_lat, pos_lon) / EARTH_RADIUS
    xtd = cross_track_distance(pos_lat, pos_lon, wp1_lat, wp1_lon, wp2_lat, wp2_lon) / EARTH_RADIUS

    atd = math.acos(math.cos(d13) / math.cos(xtd))
    return atd * EARTH_RADIUS


def eta_seconds(distance_m: float, groundspeed_ms: float) -> float:
    if groundspeed_ms <= 0:
        return float('inf')
    return distance_m / groundspeed_ms


def meters_to_lat_offset(meters: float) -> float:
    return meters / 111320.0


def meters_to_lon_offset(meters: float, latitude: float) -> float:
    return meters / (111320.0 * math.cos(math.radians(latitude)))


def normalize_heading(heading: float) -> float:
    return heading % 360


def heading_difference(h1: float, h2: float) -> float:
    diff = h2 - h1
    while diff > 180:
        diff -= 360
    while diff < -180:
        diff += 360
    return diff


def project_point(lat: float, lon: float, bearing_deg: float, distance_m: float) -> Tuple[float, float]:
    """Project a point at given bearing and distance from current position.
    Returns (lat, lon) in degrees."""
    d = distance_m / EARTH_RADIUS
    brg = math.radians(bearing_deg)
    lat1 = math.radians(lat)
    lon1 = math.radians(lon)

    lat2 = math.asin(math.sin(lat1) * math.cos(d) + math.cos(lat1) * math.sin(d) * math.cos(brg))
    lon2 = lon1 + math.atan2(math.sin(brg) * math.sin(d) * math.cos(lat1),
                              math.cos(d) - math.sin(lat1) * math.sin(lat2))

    return math.degrees(lat2), math.degrees(lon2)


# ──────────────────── Zone avoidance geometry ────────────────────


def point_in_polygon(lat: float, lon: float, polygon: List[List[float]]) -> bool:
    """Ray casting algorithm to check if point is inside polygon.
    polygon: [[lat, lon], ...] — closed ring (first != last is OK)."""
    n = len(polygon)
    if n < 3:
        return False
    inside = False
    j = n - 1
    for i in range(n):
        yi, xi = polygon[i][0], polygon[i][1]
        yj, xj = polygon[j][0], polygon[j][1]
        if ((yi > lat) != (yj > lat)) and (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi):
            inside = not inside
        j = i
    return inside


def circle_to_polygon(center_lat: float, center_lon: float,
                      radius_m: float, n_points: int = 16) -> List[List[float]]:
    """Approximate a circle as a polygon with n_points vertices."""
    points = []
    for i in range(n_points):
        bearing = 360.0 * i / n_points
        plat, plon = project_point(center_lat, center_lon, bearing, radius_m)
        points.append([plat, plon])
    return points


def _segments_intersect_2d(ax1: float, ay1: float, ax2: float, ay2: float,
                           bx1: float, by1: float, bx2: float, by2: float) -> bool:
    """Check if segment (a1-a2) intersects segment (b1-b2) using cross products.
    Coordinates are flat (lat, lon treated as planar — valid for small areas)."""
    dx_a = ax2 - ax1
    dy_a = ay2 - ay1
    dx_b = bx2 - bx1
    dy_b = by2 - by1

    denom = dx_a * dy_b - dy_a * dx_b
    if abs(denom) < 1e-15:
        return False  # parallel

    t = ((bx1 - ax1) * dy_b - (by1 - ay1) * dx_b) / denom
    u = ((bx1 - ax1) * dy_a - (by1 - ay1) * dx_a) / denom

    return 0.0 < t < 1.0 and 0.0 < u < 1.0


def segment_intersects_polygon(p1_lat: float, p1_lon: float,
                               p2_lat: float, p2_lon: float,
                               polygon: List[List[float]]) -> bool:
    """Check if segment p1-p2 intersects any edge of polygon or passes through it."""
    # If either endpoint is inside the polygon, the segment intersects
    if point_in_polygon(p1_lat, p1_lon, polygon) or point_in_polygon(p2_lat, p2_lon, polygon):
        return True
    n = len(polygon)
    if n < 3:
        return False
    for i in range(n):
        j = (i + 1) % n
        if _segments_intersect_2d(
            p1_lat, p1_lon, p2_lat, p2_lon,
            polygon[i][0], polygon[i][1], polygon[j][0], polygon[j][1]
        ):
            return True
    return False


def is_visible(p1_lat: float, p1_lon: float, p2_lat: float, p2_lon: float,
               obstacles: List[List[List[float]]]) -> bool:
    """Check if two points can see each other (no obstacle polygon blocks the line of sight)."""
    for poly in obstacles:
        if segment_intersects_polygon(p1_lat, p1_lon, p2_lat, p2_lon, poly):
            return False
    return True


def is_visible_vg(p1_lat: float, p1_lon: float, p2_lat: float, p2_lon: float,
                  obstacles: List[List[List[float]]]) -> bool:
    """Visibility check for visibility graph — handles polygon vertex endpoints.
    Unlike is_visible, does NOT check endpoint containment (vertices lie on
    polygon boundaries and would be incorrectly classified as 'inside').
    Checks edge crossings + perpendicular interior test to distinguish
    polygon-edge segments from polygon-interior segments."""
    mid_lat = (p1_lat + p2_lat) / 2
    mid_lon = (p1_lon + p2_lon) / 2

    # Perpendicular offset for interior detection (~1cm)
    dlat = p2_lat - p1_lat
    dlon = p2_lon - p1_lon
    norm = math.sqrt(dlat * dlat + dlon * dlon)
    eps = 1e-7
    if norm > 1e-15:
        perp_lat = -dlon / norm * eps
        perp_lon = dlat / norm * eps
    else:
        perp_lat, perp_lon = eps, 0.0

    cl1, cn1 = mid_lat + perp_lat, mid_lon + perp_lon
    cl2, cn2 = mid_lat - perp_lat, mid_lon - perp_lon

    for poly in obstacles:
        n = len(poly)
        if n < 3:
            continue
        # Check edge crossings (strict inequality — ignores shared endpoints)
        for i in range(n):
            j = (i + 1) % n
            if _segments_intersect_2d(
                p1_lat, p1_lon, p2_lat, p2_lon,
                poly[i][0], poly[i][1], poly[j][0], poly[j][1]
            ):
                return False
        # Both perpendicular sides inside → segment cuts through polygon interior
        # (a segment along a polygon edge has only ONE side inside)
        if point_in_polygon(cl1, cn1, poly) and point_in_polygon(cl2, cn2, poly):
            return False
    return True


def polygon_buffer(polygon: List[List[float]], buffer_m: float) -> List[List[float]]:
    """Expand polygon outward by buffer_m meters from boundary.
    Each edge is offset outward along its normal, vertices placed at
    intersections of adjacent offset edges (miter join).
    Uses flat-earth approximation for speed (accurate within ~10km)."""
    if not polygon or buffer_m <= 0:
        return list(polygon) if polygon else []

    n = len(polygon)
    if n < 3:
        return list(polygon)

    cx = sum(p[0] for p in polygon) / n
    cy = sum(p[1] for p in polygon) / n
    lat_scale = 111320.0
    lon_scale = 111320.0 * math.cos(math.radians(cx))

    # Convert to flat meters
    flat = [((p[0] - cx) * lat_scale, (p[1] - cy) * lon_scale) for p in polygon]

    # Signed area to determine winding (CCW → positive)
    area2 = 0.0
    for i in range(n):
        j = (i + 1) % n
        area2 += flat[i][0] * flat[j][1] - flat[j][0] * flat[i][1]
    sign = 1.0 if area2 > 0 else -1.0

    result = []
    for i in range(n):
        prev_i = (i - 1) % n
        next_i = (i + 1) % n

        # Incoming edge: prev → i
        dx1 = flat[i][0] - flat[prev_i][0]
        dy1 = flat[i][1] - flat[prev_i][1]
        L1 = math.sqrt(dx1 * dx1 + dy1 * dy1)

        # Outgoing edge: i → next
        dx2 = flat[next_i][0] - flat[i][0]
        dy2 = flat[next_i][1] - flat[i][1]
        L2 = math.sqrt(dx2 * dx2 + dy2 * dy2)

        if L1 < 0.01 or L2 < 0.01:
            result.append([polygon[i][0], polygon[i][1]])
            continue

        # Outward normals (for CCW: (dy, -dx); flip sign for CW)
        nx1, ny1 = sign * dy1 / L1, sign * (-dx1) / L1
        nx2, ny2 = sign * dy2 / L2, sign * (-dx2) / L2

        # Bisector direction
        bx = nx1 + nx2
        by = ny1 + ny2
        blen = math.sqrt(bx * bx + by * by)
        if blen < 1e-10:
            bx, by = nx1, ny1
        else:
            bx /= blen
            by /= blen

        # Offset along bisector: buffer / cos(half_angle_between_normals)
        cos_half = nx1 * bx + ny1 * by
        cos_half = max(cos_half, 0.25)  # cap at ~75° to prevent spikes
        offset = buffer_m / cos_half

        new_x = flat[i][0] + bx * offset
        new_y = flat[i][1] + by * offset

        new_lat = cx + new_x / lat_scale
        new_lon = cy + new_y / lon_scale
        result.append([new_lat, new_lon])

    return result


# ──────────────────── Batch (numpy) geometry ────────────────────


def batch_segments_intersect(segs_a: np.ndarray, segs_b: np.ndarray) -> np.ndarray:
    """Vectorized intersection test between two sets of line segments.

    Args:
        segs_a: (P, 4) array — [lat1, lon1, lat2, lon2] per row
        segs_b: (E, 4) array — [lat1, lon1, lat2, lon2] per row

    Returns:
        (P, E) bool array — result[i,j] is True when seg_a[i] crosses seg_b[j].
    """
    ax1, ay1 = segs_a[:, 0], segs_a[:, 1]
    ax2, ay2 = segs_a[:, 2], segs_a[:, 3]
    bx1, by1 = segs_b[:, 0], segs_b[:, 1]
    bx2, by2 = segs_b[:, 2], segs_b[:, 3]

    dx_a = ax2 - ax1  # (P,)
    dy_a = ay2 - ay1
    dx_b = bx2 - bx1  # (E,)
    dy_b = by2 - by1

    # (P,1) × (1,E) → (P,E)
    denom = dx_a[:, None] * dy_b[None, :] - dy_a[:, None] * dx_b[None, :]

    dbx = bx1[None, :] - ax1[:, None]  # (P, E)
    dby = by1[None, :] - ay1[:, None]

    safe_denom = np.where(np.abs(denom) < 1e-15, 1.0, denom)
    t = (dbx * dy_b[None, :] - dby * dx_b[None, :]) / safe_denom
    u = (dbx * dy_a[:, None] - dby * dx_a[:, None]) / safe_denom

    return (np.abs(denom) >= 1e-15) & (t > 0) & (t < 1) & (u > 0) & (u < 1)


def batch_point_in_polygon(points: np.ndarray, polygon: np.ndarray) -> np.ndarray:
    """Ray-casting for M points against one polygon (vectorized over points).

    Args:
        points: (M, 2) array — [lat, lon] per row
        polygon: (N, 2) array — [lat, lon] per vertex

    Returns:
        (M,) bool array — True if point is inside polygon.
    """
    n = polygon.shape[0]
    if n < 3:
        return np.zeros(points.shape[0], dtype=bool)

    plat = points[:, 0]
    plon = points[:, 1]
    inside = np.zeros(len(plat), dtype=bool)

    j = n - 1
    for i in range(n):
        yi, xi = polygon[i, 0], polygon[i, 1]
        yj, xj = polygon[j, 0], polygon[j, 1]

        dy = yj - yi
        if abs(dy) < 1e-15:
            j = i
            continue

        cond = (yi > plat) != (yj > plat)
        x_intersect = (xj - xi) * (plat - yi) / dy + xi
        inside ^= cond & (plon < x_intersect)

        j = i

    return inside


def batch_haversine(lat1: np.ndarray, lon1: np.ndarray,
                    lat2: np.ndarray, lon2: np.ndarray) -> np.ndarray:
    """Vectorized haversine distance in meters."""
    lat1_r = np.radians(lat1)
    lat2_r = np.radians(lat2)
    dlat = np.radians(lat2 - lat1)
    dlon = np.radians(lon2 - lon1)
    a = np.sin(dlat / 2) ** 2 + np.cos(lat1_r) * np.cos(lat2_r) * np.sin(dlon / 2) ** 2
    c = 2 * np.arctan2(np.sqrt(a), np.sqrt(1 - a))
    return EARTH_RADIUS * c
