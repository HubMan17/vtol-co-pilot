import logging
import math
import time
from collections import deque
from typing import List, Optional, Tuple

from src.mavlink.telemetry import LatLon, TelemetryState
from src.navigation.calculations import (
    meters_to_lat_offset, meters_to_lon_offset
)

logger = logging.getLogger("autopilot")


class DeadReckoningEngine:
    # Kalman parameters
    _KALMAN_Q: float = 0.005        # process noise (m^2/s^3) — how fast drift can change
    _KALMAN_R: float = 2.0          # measurement noise (m^2/s^2) — operator click accuracy
    _KALMAN_P_INIT: float = 10.0    # initial covariance — "we know nothing"
    _KALMAN_P_MIN: float = 0.01     # min covariance — prevent overconfidence
    _KALMAN_P_MAX: float = 100.0    # max covariance — prevent divergence
    _MAX_DRIFT_SPEED: float = 10.0  # max drift magnitude (m/s)
    _OUTLIER_THRESHOLD: float = 5.991  # chi-squared 95% for 2 DOF
    _MIN_FIX_INTERVAL: float = 5.0  # min seconds between Kalman updates
    _GPS_RETURN_DECAY: float = 0.98  # drift decay per cycle when GPS returns

    def __init__(self, max_track_points: int = 1000):
        self._position: Optional[LatLon] = None
        self._track_history: deque[LatLon] = deque(maxlen=max_track_points)
        self._max_track_points = max_track_points
        self._last_update_time = 0.0
        self._initialized = False

        # Wind freezing
        self._frozen_wind_speed: float = 0.0
        self._frozen_wind_direction: float = 0.0
        self._wind_frozen: bool = False
        self._manual_wind_active: bool = False

        # Kalman drift correction
        self._drift_vector_n: float = 0.0
        self._drift_vector_e: float = 0.0
        self._kalman_p_n: float = self._KALMAN_P_INIT
        self._kalman_p_e: float = self._KALMAN_P_INIT
        self._last_fix_time: float = 0.0
        self._gps_was_lost: bool = False

        # Last calculated velocity (for GPS_INPUT — avoids feedback loop)
        self._last_vn: float = 0.0  # velocity North (m/s)
        self._last_ve: float = 0.0  # velocity East (m/s)

        # Position blending for smooth GPS_INPUT transitions on pilot corrections
        # Offset decays from (old_pos - new_pos) to zero over blend duration
        self._blend_offset_n: float = 0.0   # North offset in meters (decaying)
        self._blend_offset_e: float = 0.0   # East offset in meters (decaying)
        self._blend_start_time: float = 0.0
        self._blend_duration: float = 0.0
        self._MAX_BLEND_SPEED: float = 2.0   # m/s max implied correction velocity
        self._MIN_BLEND_DURATION: float = 5.0  # seconds minimum blend time

    def update(self, telemetry: TelemetryState, force_dr: bool = False) -> Optional[LatLon]:
        """Update DR position.
        force_dr=True: pilot forced GPS off (spoof protection) — don't snap to GPS.
        """
        current_time = time.time()
        gps_ok = telemetry.gps_fix >= 3 and not force_dr

        # --- Initialization ---
        if not self._initialized or self._position is None:
            if telemetry.position and not force_dr:
                self._set_position_internal(telemetry.position.lat, telemetry.position.lon)
            if gps_ok:
                self._frozen_wind_speed = telemetry.wind_speed
                self._frozen_wind_direction = telemetry.wind_direction
                self._wind_frozen = True
            return self._position

        dt = current_time - self._last_update_time
        if dt <= 0 or dt > 5.0:
            self._last_update_time = current_time
            return self._position

        # --- GPS snap: GPS OK → snap DR to GPS position ---
        if gps_ok and telemetry.position:
            self._position = LatLon(telemetry.position.lat, telemetry.position.lon)
            self._last_update_time = current_time
            # GPS OK — use actual NED velocity from GLOBAL_POSITION_INT
            # NOT heading-derived: heading != track in crosswind (crab angle)
            self._last_vn = telemetry.velocity_n
            self._last_ve = telemetry.velocity_e
            if not self._manual_wind_active:
                self._frozen_wind_speed = telemetry.wind_speed
                self._frozen_wind_direction = telemetry.wind_direction
                self._wind_frozen = True

            # GPS return: decay drift vector + grow P
            if self._gps_was_lost:
                # Time-based decay: ~60s to reach zero (tau ≈ 12s → exp(-60/12) ≈ 0.007)
                decay = self._GPS_RETURN_DECAY ** (dt * 10.0)  # normalize to 10 Hz
                self._drift_vector_n *= decay
                self._drift_vector_e *= decay
                self._kalman_p_n = min(self._kalman_p_n + 0.01 * dt, self._KALMAN_P_MAX)
                self._kalman_p_e = min(self._kalman_p_e + 0.01 * dt, self._KALMAN_P_MAX)
                drift_mag = math.sqrt(self._drift_vector_n ** 2 + self._drift_vector_e ** 2)
                if drift_mag < 0.01:
                    self._gps_was_lost = False
            return self._position

        # --- GPS lost: propagate DR ---
        self._gps_was_lost = True

        # Kalman predict: P grows with time
        self._kalman_p_n = min(self._kalman_p_n + self._KALMAN_Q * dt, self._KALMAN_P_MAX)
        self._kalman_p_e = min(self._kalman_p_e + self._KALMAN_Q * dt, self._KALMAN_P_MAX)

        # Wind source selection
        if self._manual_wind_active:
            wind_speed = telemetry.wind_speed
            wind_direction = telemetry.wind_direction
        elif self._wind_frozen:
            wind_speed = self._frozen_wind_speed
            wind_direction = self._frozen_wind_direction
        else:
            wind_speed = 0.0
            wind_direction = 0.0

        # Velocity calculation: air + wind + drift correction
        # ATTITUDE.yaw (float radians) is far more precise than VFR_HUD.heading (int degrees)
        heading_rad = telemetry.yaw
        if heading_rad < 0:
            heading_rad += 2 * math.pi
        wind_from_rad = math.radians(wind_direction + 180)

        airspeed = telemetry.airspeed
        if airspeed < 5.0:
            airspeed = max(telemetry.groundspeed, 15.0)

        vx_air = airspeed * math.sin(heading_rad)
        vy_air = airspeed * math.cos(heading_rad)

        vx_wind = wind_speed * math.sin(wind_from_rad)
        vy_wind = wind_speed * math.cos(wind_from_rad)

        vx = vx_air + vx_wind + self._drift_vector_e
        vy = vy_air + vy_wind + self._drift_vector_n

        # Store velocity for GPS_INPUT (avoids groundspeed feedback loop)
        self._last_ve = vx  # East component
        self._last_vn = vy  # North component

        # Position integration
        delta_x = vx * dt
        delta_y = vy * dt

        new_lat = self._position.lat + meters_to_lat_offset(delta_y)
        new_lon = self._position.lon + meters_to_lon_offset(delta_x, self._position.lat)

        self._position = LatLon(new_lat, new_lon)
        self._last_update_time = current_time

        # Track uses blended position (what GPS_INPUT actually sends)
        blended = self.get_position()
        if blended:
            self._track_history.append(blended)

        return self._position

    def set_position(self, lat: float, lon: float):
        """Set DR position manually. If previous fix exists — run Kalman update."""
        now = time.time()

        if self._position is not None and self._last_fix_time > 0:
            dt = now - self._last_fix_time
            if dt > self._MIN_FIX_INTERVAL:
                # Position error in meters
                error_n_m = (lat - self._position.lat) / meters_to_lat_offset(1.0)
                error_e_m = (lon - self._position.lon) / meters_to_lon_offset(1.0, lat)

                # Observed drift velocity (m/s)
                z_n = error_n_m / dt
                z_e = error_e_m / dt

                # Outlier detection (Mahalanobis distance for diagonal covariance)
                s_n = self._kalman_p_n + self._KALMAN_R
                s_e = self._kalman_p_e + self._KALMAN_R
                innovation_n = z_n - self._drift_vector_n
                innovation_e = z_e - self._drift_vector_e
                mahalanobis_sq = (innovation_n ** 2) / s_n + (innovation_e ** 2) / s_e

                if mahalanobis_sq <= self._OUTLIER_THRESHOLD:
                    # Valid fix — Kalman update
                    k_n = self._kalman_p_n / s_n
                    k_e = self._kalman_p_e / s_e

                    self._drift_vector_n += k_n * innovation_n
                    self._drift_vector_e += k_e * innovation_e

                    # Joseph form for numerical stability
                    self._kalman_p_n = (1 - k_n) ** 2 * self._kalman_p_n + k_n ** 2 * self._KALMAN_R
                    self._kalman_p_e = (1 - k_e) ** 2 * self._kalman_p_e + k_e ** 2 * self._KALMAN_R

                    # Enforce bounds
                    self._kalman_p_n = max(self._KALMAN_P_MIN, min(self._kalman_p_n, self._KALMAN_P_MAX))
                    self._kalman_p_e = max(self._KALMAN_P_MIN, min(self._kalman_p_e, self._KALMAN_P_MAX))

                    # Sanity clamp on drift magnitude
                    drift_mag = math.sqrt(self._drift_vector_n ** 2 + self._drift_vector_e ** 2)
                    if drift_mag > self._MAX_DRIFT_SPEED:
                        scale = self._MAX_DRIFT_SPEED / drift_mag
                        self._drift_vector_n *= scale
                        self._drift_vector_e *= scale

                    logger.info(
                        f"KALMAN UPDATE: K=[{k_n:.3f},{k_e:.3f}] "
                        f"drift=[{self._drift_vector_n:.3f},{self._drift_vector_e:.3f}] m/s "
                        f"P=[{self._kalman_p_n:.3f},{self._kalman_p_e:.3f}] "
                        f"error=[{error_n_m:.1f},{error_e_m:.1f}]m dt={dt:.0f}s"
                    )
                else:
                    logger.warning(
                        f"KALMAN OUTLIER REJECTED: mahalanobis^2={mahalanobis_sq:.1f} "
                        f"> threshold={self._OUTLIER_THRESHOLD:.1f} "
                        f"innovation=[{innovation_n:.3f},{innovation_e:.3f}] m/s"
                    )
                    # Grow P as safety — maybe we were wrong to reject
                    self._kalman_p_n = min(self._kalman_p_n * 1.5, self._KALMAN_P_MAX)
                    self._kalman_p_e = min(self._kalman_p_e * 1.5, self._KALMAN_P_MAX)

        # Smooth blending: calculate offset so GPS_INPUT doesn't jump
        if self._position is not None:
            old_lat = self._position.lat
            old_lon = self._position.lon
            offset_n = (old_lat - lat) / meters_to_lat_offset(1.0)
            offset_e = (old_lon - lon) / meters_to_lon_offset(1.0, lat)
            distance = math.sqrt(offset_n ** 2 + offset_e ** 2)
            if distance > 5.0:  # only blend if correction > 5m
                self._blend_offset_n = offset_n
                self._blend_offset_e = offset_e
                self._blend_duration = max(self._MIN_BLEND_DURATION,
                                           distance / self._MAX_BLEND_SPEED)
                self._blend_start_time = now
                logger.info(
                    f"BLEND START: offset=[{offset_n:.1f},{offset_e:.1f}]m "
                    f"distance={distance:.1f}m duration={self._blend_duration:.1f}s"
                )
            else:
                # Small correction — no blend needed
                self._blend_offset_n = 0.0
                self._blend_offset_e = 0.0
                self._blend_duration = 0.0

        # Teleport internal position (always, even if outlier)
        self._position = LatLon(lat, lon)
        self._last_fix_time = now
        self._initialized = True
        self._last_update_time = now
        self._track_history = deque([LatLon(lat, lon)], maxlen=self._max_track_points)

    def _set_position_internal(self, lat: float, lon: float):
        """Internal position init — no Kalman update."""
        self._position = LatLon(lat, lon)
        self._initialized = True
        self._last_update_time = time.time()
        self._track_history = deque([LatLon(lat, lon)], maxlen=self._max_track_points)

    def set_manual_wind_active(self, active: bool):
        self._manual_wind_active = active

    def get_velocity(self) -> Tuple[float, float]:
        """Return last DR velocity (vn, ve) in m/s NED.
        Use this for GPS_INPUT instead of telemetry.groundspeed to avoid feedback loop."""
        return (self._last_vn, self._last_ve)

    def get_frozen_wind(self) -> Tuple[float, float]:
        """Return frozen wind (speed, direction) for display when GPS is lost."""
        return (self._frozen_wind_speed, self._frozen_wind_direction)

    def get_drift_vector(self) -> Tuple[float, float]:
        return (self._drift_vector_n, self._drift_vector_e)

    def get_drift_confidence(self) -> float:
        avg_p = (self._kalman_p_n + self._kalman_p_e) / 2.0
        return 1.0 / (1.0 + avg_p)

    def get_kalman_state(self) -> dict:
        """Full Kalman state for debugging/logging."""
        return {
            'drift_n': self._drift_vector_n,
            'drift_e': self._drift_vector_e,
            'p_n': self._kalman_p_n,
            'p_e': self._kalman_p_e,
            'confidence': self.get_drift_confidence(),
            'frozen_wind': (self._frozen_wind_speed, self._frozen_wind_direction),
            'wind_frozen': self._wind_frozen,
            'gps_was_lost': self._gps_was_lost,
        }

    def get_position(self) -> Optional[LatLon]:
        if self._position is None:
            return None
        # Apply decaying blend offset (smooth GPS_INPUT after pilot correction)
        if self._blend_duration > 0.0:
            elapsed = time.time() - self._blend_start_time
            if elapsed >= self._blend_duration:
                # Blend complete — clear offset
                self._blend_offset_n = 0.0
                self._blend_offset_e = 0.0
                self._blend_duration = 0.0
            else:
                # Smoothstep: t²(3-2t) for smooth velocity profile
                t = elapsed / self._blend_duration
                smooth = t * t * (3.0 - 2.0 * t)
                # Remaining offset fraction = 1 - smooth (decays from 1 to 0)
                frac = 1.0 - smooth
                offset_n = self._blend_offset_n * frac
                offset_e = self._blend_offset_e * frac
                blended_lat = self._position.lat + meters_to_lat_offset(offset_n)
                blended_lon = self._position.lon + meters_to_lon_offset(
                    offset_e, self._position.lat)
                return LatLon(blended_lat, blended_lon)
        return self._position

    def get_track(self) -> List[LatLon]:
        return list(self._track_history)

    def clear_track(self):
        if self._position:
            self._track_history = deque([LatLon(self._position.lat, self._position.lon)], maxlen=self._max_track_points)
        else:
            self._track_history = deque(maxlen=self._max_track_points)

    def reset(self):
        self._position = None
        self._track_history = deque(maxlen=self._max_track_points)
        self._initialized = False
        self._last_update_time = 0.0
        self._frozen_wind_speed = 0.0
        self._frozen_wind_direction = 0.0
        self._wind_frozen = False
        self._drift_vector_n = 0.0
        self._drift_vector_e = 0.0
        self._kalman_p_n = self._KALMAN_P_INIT
        self._kalman_p_e = self._KALMAN_P_INIT
        self._last_fix_time = 0.0
        self._gps_was_lost = False
        self._last_vn = 0.0
        self._last_ve = 0.0
        self._blend_offset_n = 0.0
        self._blend_offset_e = 0.0
        self._blend_start_time = 0.0
        self._blend_duration = 0.0

    def is_initialized(self) -> bool:
        return self._initialized
