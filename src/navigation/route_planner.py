import json
from pathlib import Path
from dataclasses import dataclass, field, asdict
from typing import List, Optional, Tuple
from datetime import datetime

from src.mavlink.telemetry import LatLon
from src.navigation.calculations import (
    haversine_distance, bearing_to, cross_track_distance,
    along_track_distance, eta_seconds
)


@dataclass
class Waypoint:
    id: int
    lat: float
    lon: float
    altitude: float
    radius: float = 50.0
    action: str = "FLYTHROUGH"
    orbit_radius: float = 100.0
    orbit_turns: int = 1
    target_altitude: float = 0.0

    def to_latlon(self) -> LatLon:
        return LatLon(self.lat, self.lon)

    def get_action_name(self) -> str:
        names = {
            "FLYTHROUGH": "Пролёт",
            "ORBIT_ALTITUDE": "Кружить до высоты",
            "ORBIT_TURNS": "Кружить N кругов",
            "ORBIT_INFINITE": "Кружить бесконечно",
        }
        return names.get(self.action, self.action)


@dataclass
class Route:
    name: str
    waypoints: List[Waypoint] = field(default_factory=list)
    created: str = ""
    modified: str = ""

    def __post_init__(self):
        if not self.created:
            self.created = datetime.now().isoformat()
        if not self.modified:
            self.modified = self.created


class RoutePlanner:
    def __init__(self):
        self._route: Optional[Route] = None
        self._active_waypoint_idx = 0

    def load_route(self, path: Path) -> Optional[Route]:
        try:
            with open(path, 'r', encoding='utf-8') as f:
                data = json.load(f)

            waypoints = []
            for wp_data in data.get('waypoints', []):
                wp = Waypoint(
                    id=wp_data.get('id', len(waypoints) + 1),
                    lat=wp_data['lat'],
                    lon=wp_data['lon'],
                    altitude=wp_data.get('altitude', wp_data.get('alt', 100)),
                    radius=wp_data.get('radius', 50.0),
                    action=wp_data.get('action', 'FLYTHROUGH'),
                    orbit_radius=wp_data.get('orbit_radius', 100.0),
                    orbit_turns=wp_data.get('orbit_turns', 1),
                    target_altitude=wp_data.get('target_altitude', 0.0)
                )
                waypoints.append(wp)

            self._route = Route(
                name=data.get('name', path.stem),
                waypoints=waypoints,
                created=data.get('created', ''),
                modified=data.get('modified', '')
            )
            self._active_waypoint_idx = 0
            return self._route

        except Exception as e:
            print(f"Failed to load route: {e}")
            return None

    def save_route(self, route: Route, path: Path) -> bool:
        try:
            route.modified = datetime.now().isoformat()

            data = {
                'name': route.name,
                'created': route.created,
                'modified': route.modified,
                'waypoints': [
                    {
                        'id': wp.id,
                        'lat': wp.lat,
                        'lon': wp.lon,
                        'altitude': wp.altitude,
                        'radius': wp.radius,
                        'action': wp.action,
                        'orbit_radius': wp.orbit_radius,
                        'orbit_turns': wp.orbit_turns,
                        'target_altitude': wp.target_altitude
                    }
                    for wp in route.waypoints
                ]
            }

            path.parent.mkdir(parents=True, exist_ok=True)
            with open(path, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2)

            return True
        except Exception as e:
            print(f"Failed to save route: {e}")
            return False

    def get_route(self) -> Optional[Route]:
        return self._route

    def clear_route(self):
        self._route = None
        self._active_waypoint_idx = 0

    def clear_waypoints(self):
        if self._route:
            self._route.waypoints = []
            self._active_waypoint_idx = 0

    def create_route(self, name: str) -> Route:
        self._route = Route(name=name, waypoints=[])
        self._active_waypoint_idx = 0
        return self._route

    def add_waypoint(self, lat: float, lon: float, altitude: float = 100.0,
                     radius: float = 50.0, action: str = "FLYTHROUGH",
                     orbit_radius: float = 100.0, orbit_turns: int = 1,
                     target_altitude: float = 0.0) -> Optional[Waypoint]:
        if not self._route:
            return None
        new_id = len(self._route.waypoints) + 1
        wp = Waypoint(
            id=new_id,
            lat=lat,
            lon=lon,
            altitude=altitude,
            radius=radius,
            action=action,
            orbit_radius=orbit_radius,
            orbit_turns=orbit_turns,
            target_altitude=target_altitude
        )
        self._route.waypoints.append(wp)
        return wp

    def remove_waypoint(self, index: int) -> bool:
        if not self._route or index < 0 or index >= len(self._route.waypoints):
            return False
        self._route.waypoints.pop(index)
        for i, wp in enumerate(self._route.waypoints):
            wp.id = i + 1
        if self._active_waypoint_idx >= len(self._route.waypoints):
            self._active_waypoint_idx = max(0, len(self._route.waypoints) - 1)
        return True

    def get_active_waypoint(self) -> Optional[Waypoint]:
        if not self._route or not self._route.waypoints:
            return None
        if self._active_waypoint_idx >= len(self._route.waypoints):
            return None
        return self._route.waypoints[self._active_waypoint_idx]

    def get_active_waypoint_index(self) -> int:
        return self._active_waypoint_idx

    def get_waypoint_count(self) -> int:
        if not self._route:
            return 0
        return len(self._route.waypoints)

    def next_waypoint(self) -> Optional[Waypoint]:
        if not self._route or not self._route.waypoints:
            return None
        if self._active_waypoint_idx < len(self._route.waypoints) - 1:
            self._active_waypoint_idx += 1
        return self.get_active_waypoint()

    def prev_waypoint(self) -> Optional[Waypoint]:
        if not self._route or not self._route.waypoints:
            return None
        if self._active_waypoint_idx > 0:
            self._active_waypoint_idx -= 1
        return self.get_active_waypoint()

    def set_active_waypoint(self, index: int):
        if self._route and 0 <= index < len(self._route.waypoints):
            self._active_waypoint_idx = index

    def is_waypoint_reached(self, position: LatLon) -> bool:
        wp = self.get_active_waypoint()
        if not wp:
            return False
        distance = haversine_distance(position.lat, position.lon, wp.lat, wp.lon)
        return distance <= wp.radius

    def distance_to_waypoint(self, position: LatLon) -> float:
        wp = self.get_active_waypoint()
        if not wp:
            return float('inf')
        return haversine_distance(position.lat, position.lon, wp.lat, wp.lon)

    def bearing_to_waypoint(self, position: LatLon) -> float:
        wp = self.get_active_waypoint()
        if not wp:
            return 0.0
        return bearing_to(position.lat, position.lon, wp.lat, wp.lon)

    def eta_to_waypoint(self, position: LatLon, groundspeed: float) -> float:
        distance = self.distance_to_waypoint(position)
        return eta_seconds(distance, groundspeed)

    def cross_track_error(self, position: LatLon) -> float:
        if not self._route or len(self._route.waypoints) < 2:
            return 0.0
        if self._active_waypoint_idx == 0:
            return 0.0

        wp1 = self._route.waypoints[self._active_waypoint_idx - 1]
        wp2 = self._route.waypoints[self._active_waypoint_idx]

        return cross_track_distance(
            position.lat, position.lon,
            wp1.lat, wp1.lon,
            wp2.lat, wp2.lon
        )

    def get_previous_waypoint(self) -> Optional[Waypoint]:
        if not self._route or self._active_waypoint_idx == 0:
            return None
        return self._route.waypoints[self._active_waypoint_idx - 1]

    def get_waypoints_for_display(self) -> List[dict]:
        if not self._route:
            return []
        return [
            {
                'lat': wp.lat,
                'lon': wp.lon,
                'id': wp.id,
                'altitude': wp.altitude,
                'action': wp.action,
                'action_name': wp.get_action_name(),
                'radius': wp.radius,
                'orbit_radius': wp.orbit_radius,
                'orbit_turns': wp.orbit_turns,
                'target_altitude': wp.target_altitude
            }
            for wp in self._route.waypoints
        ]

    def is_route_complete(self) -> bool:
        if not self._route:
            return True
        return self._active_waypoint_idx >= len(self._route.waypoints)
