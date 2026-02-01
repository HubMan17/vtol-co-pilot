import math
import time
from typing import List, Optional
from dataclasses import dataclass

from src.mavlink.telemetry import LatLon, TelemetryState
from src.navigation.calculations import (
    haversine_distance, meters_to_lat_offset, meters_to_lon_offset
)


@dataclass
class DRState:
    position: Optional[LatLon] = None
    groundspeed: float = 0.0
    track: float = 0.0


class DeadReckoningEngine:
    def __init__(self, drift_coefficient: float = 1.0, max_track_points: int = 1000):
        self._position: Optional[LatLon] = None
        self._track_history: List[LatLon] = []
        self._drift_coefficient = drift_coefficient
        self._max_track_points = max_track_points
        self._last_update_time = 0.0
        self._initialized = False
        self._last_gps_position: Optional[LatLon] = None
        self._dr_distance_traveled = 0.0
        self._gps_distance_traveled = 0.0

    def update(self, telemetry: TelemetryState) -> Optional[LatLon]:
        current_time = time.time()

        if not self._initialized or self._position is None:
            if telemetry.position:
                self.set_position(telemetry.position.lat, telemetry.position.lon)
            return self._position

        dt = current_time - self._last_update_time
        if dt <= 0 or dt > 5.0:
            self._last_update_time = current_time
            return self._position

        heading_rad = math.radians(telemetry.heading)
        wind_from_rad = math.radians(telemetry.wind_direction + 180)

        vx_air = telemetry.airspeed * math.sin(heading_rad)
        vy_air = telemetry.airspeed * math.cos(heading_rad)

        vx_wind = telemetry.wind_speed * math.sin(wind_from_rad)
        vy_wind = telemetry.wind_speed * math.cos(wind_from_rad)

        vx = (vx_air + vx_wind) * self._drift_coefficient
        vy = (vy_air + vy_wind) * self._drift_coefficient

        delta_x = vx * dt
        delta_y = vy * dt

        distance_moved = math.sqrt(delta_x ** 2 + delta_y ** 2)
        self._dr_distance_traveled += distance_moved

        new_lat = self._position.lat + meters_to_lat_offset(delta_y)
        new_lon = self._position.lon + meters_to_lon_offset(delta_x, self._position.lat)

        self._position = LatLon(new_lat, new_lon)
        self._last_update_time = current_time

        self._track_history.append(LatLon(new_lat, new_lon))
        if len(self._track_history) > self._max_track_points:
            self._track_history.pop(0)

        if telemetry.position and telemetry.gps_fix >= 3:
            self._learn_drift(telemetry.position)

        return self._position

    def set_position(self, lat: float, lon: float):
        self._position = LatLon(lat, lon)
        self._initialized = True
        self._last_update_time = time.time()
        self._track_history = [LatLon(lat, lon)]
        self._dr_distance_traveled = 0.0
        self._gps_distance_traveled = 0.0
        self._last_gps_position = None

    def get_position(self) -> Optional[LatLon]:
        return self._position

    def get_track(self) -> List[LatLon]:
        return self._track_history.copy()

    def clear_track(self):
        if self._position:
            self._track_history = [LatLon(self._position.lat, self._position.lon)]
        else:
            self._track_history = []

    def reset(self):
        self._position = None
        self._track_history = []
        self._initialized = False
        self._last_update_time = 0.0
        self._dr_distance_traveled = 0.0
        self._gps_distance_traveled = 0.0
        self._last_gps_position = None

    def set_drift_coefficient(self, coefficient: float):
        self._drift_coefficient = max(0.5, min(1.5, coefficient))

    def get_drift_coefficient(self) -> float:
        return self._drift_coefficient

    def _learn_drift(self, gps_position: LatLon):
        if self._last_gps_position is None:
            self._last_gps_position = gps_position
            return

        gps_distance = haversine_distance(
            self._last_gps_position.lat, self._last_gps_position.lon,
            gps_position.lat, gps_position.lon
        )

        self._gps_distance_traveled += gps_distance
        self._last_gps_position = gps_position

        if self._dr_distance_traveled > 100 and self._gps_distance_traveled > 100:
            ratio = self._gps_distance_traveled / self._dr_distance_traveled
            alpha = 0.1
            new_coefficient = self._drift_coefficient * (1 - alpha) + ratio * alpha
            self.set_drift_coefficient(new_coefficient)

            self._dr_distance_traveled = 0.0
            self._gps_distance_traveled = 0.0

    def is_initialized(self) -> bool:
        return self._initialized
