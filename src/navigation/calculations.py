import math
from typing import Tuple

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
