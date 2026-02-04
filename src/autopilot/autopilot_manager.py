import time
import logging
from pathlib import Path
from enum import Enum, auto
from typing import Optional, TYPE_CHECKING

from src.mavlink.proxy import MAVLinkProxy

log_dir = Path(__file__).parent.parent.parent / "logs"
log_dir.mkdir(exist_ok=True)
logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s.%(msecs)03d %(levelname)s: %(message)s',
    datefmt='%H:%M:%S',
    handlers=[
        logging.FileHandler(log_dir / "autopilot.log", mode='w', encoding='utf-8'),
    ]
)
logger = logging.getLogger(__name__)
from src.mavlink.telemetry import LatLon
from src.core.config import AutopilotConfig
from src.core.events import EventBus, Event
from src.autopilot.heading_controller import HeadingController
from src.autopilot.altitude_controller import AltitudeController
from src.autopilot.speed_controller import SpeedController
from src.navigation.calculations import bearing_to, haversine_distance
import math

if TYPE_CHECKING:
    from src.navigation.route_planner import RoutePlanner


class AutopilotMode(Enum):
    MANUAL = auto()
    NAV = auto()


class AutopilotManager:
    CH_ROLL = 0
    CH_PITCH = 1
    CH_THROTTLE = 2
    CH_YAW = 3

    PWM_CENTER = 1500

    def __init__(self, proxy: MAVLinkProxy, config: AutopilotConfig):
        self._proxy = proxy
        self._config = config
        self._event_bus = EventBus()

        self._mode = AutopilotMode.MANUAL
        self._heading_controller = HeadingController(config)
        self._altitude_controller = AltitudeController(config)
        self._speed_controller = SpeedController(config)
        self._route_planner: Optional['RoutePlanner'] = None

        self._last_update_time = 0.0
        self._disengage_reason = ""
        self._active_waypoint_id = -1
        self._stick_override_count = 0
        self._stick_override_threshold_count = 3

        self._is_orbiting = False
        self._orbit_turns_completed = 0
        self._orbit_last_heading = 0.0
        self._orbit_heading_accumulated = 0.0

        self._event_bus.subscribe(Event.CONNECTION_LOST, self._on_connection_lost)

    def set_route_planner(self, route_planner: 'RoutePlanner'):
        self._route_planner = route_planner

    def engage_nav(self) -> bool:
        if not self._proxy.is_connected():
            return False

        if not self._route_planner:
            return False

        route = self._route_planner.get_route()
        if not route or not route.waypoints:
            return False

        wp = self._route_planner.get_active_waypoint()
        if not wp:
            return False

        self._heading_controller.reset()
        self._altitude_controller.reset()
        self._speed_controller.reset()

        telemetry = self._proxy.get_telemetry()
        self._speed_controller.initialize_from_current(telemetry.airspeed)

        if wp.climb_enroute:
            self._altitude_controller.set_target_altitude(wp.altitude)
        else:
            self._altitude_controller.set_target_altitude(telemetry.altitude_agl)
        self._stick_override_count = 0
        self._active_waypoint_id = wp.id
        self._is_orbiting = False
        self._orbit_turns_completed = 0
        self._orbit_heading_accumulated = 0.0

        self._mode = AutopilotMode.NAV
        self._last_update_time = time.time()

        logger.info(f"ENGAGE NAV: waypoint={wp.id}/{len(route.waypoints)}")

        self._event_bus.emit(Event.AUTOPILOT_ENGAGE, {
            'mode': 'NAV',
            'waypoint': wp.id,
            'total': len(route.waypoints)
        })

        return True

    def disengage(self, reason: str = ""):
        if self._mode == AutopilotMode.MANUAL:
            return

        logger.info(f"DISENGAGE: reason='{reason}', prev_mode={self._mode.name}")

        self._disengage_reason = reason
        prev_mode = self._mode
        self._mode = AutopilotMode.MANUAL

        self._proxy.release_rc_override()

        self._heading_controller.reset()
        self._altitude_controller.reset()
        self._speed_controller.reset()

        self._event_bus.emit(Event.AUTOPILOT_DISENGAGE, {
            'previous_mode': prev_mode.name,
            'reason': reason
        })

    def get_mode(self) -> AutopilotMode:
        return self._mode

    def is_engaged(self) -> bool:
        return self._mode != AutopilotMode.MANUAL

    def update(self, override_position: Optional[LatLon] = None) -> bool:
        if self._mode == AutopilotMode.MANUAL:
            return False

        if not self._proxy.is_connected():
            self.disengage("Соединение потеряно")
            return False

        current_time = time.time()
        telemetry = self._proxy.get_telemetry()

        # TODO: временно отключено для тестирования в SITL
        # if self.check_stick_override(telemetry.rc_channels_raw):
        #     self.disengage("Пилот взял управление")
        #     return False

        timeout_sec = self._config.timeout_ms / 1000.0
        if self._last_update_time > 0 and (current_time - self._last_update_time) > timeout_sec:
            self.disengage("Таймаут обновления")
            return False

        dt = current_time - self._last_update_time if self._last_update_time > 0 else 0.05

        if self._mode == AutopilotMode.NAV:
            if not self._route_planner:
                self.disengage("Маршрут не задан")
                return False

            position = override_position if override_position else telemetry.position
            if not position:
                self._last_update_time = current_time
                return True

            wp = self._route_planner.get_active_waypoint()
            if not wp:
                self.disengage("Нет активной точки")
                return False

            distance_to_wp = haversine_distance(position.lat, position.lon, wp.lat, wp.lon)

            if self._is_orbiting:
                target_bearing = self._calculate_orbit_heading(position, wp, telemetry.heading)
                self._update_orbit_progress(telemetry.heading)

                if wp.action == "ORBIT_TURNS" and self._orbit_turns_completed >= wp.orbit_turns:
                    logger.info(f"ORBIT COMPLETE: {self._orbit_turns_completed} turns at WP{wp.id}")
                    self._finish_orbit_and_advance(wp)

            else:
                if self._route_planner.is_waypoint_reached(position):
                    if wp.action in ("ORBIT_TURNS", "ORBIT_INFINITE"):
                        self._start_orbit(wp, telemetry.heading)
                        if not wp.climb_enroute:
                            self._altitude_controller.set_target_altitude(wp.altitude)
                        target_bearing = self._calculate_orbit_heading(position, wp, telemetry.heading)
                    else:
                        old_wp = wp
                        self._route_planner.next_waypoint()
                        new_wp = self._route_planner.get_active_waypoint()

                        if new_wp and new_wp.id != self._active_waypoint_id:
                            self._active_waypoint_id = new_wp.id
                            if new_wp.climb_enroute:
                                self._altitude_controller.set_target_altitude(new_wp.altitude)
                            else:
                                self._altitude_controller.set_target_altitude(telemetry.altitude_agl)
                            self._event_bus.emit(Event.WAYPOINT_REACHED, {
                                'reached': old_wp.id if old_wp else 0,
                                'next': new_wp.id
                            })

                        if self._route_planner.is_route_complete():
                            self.disengage("Маршрут завершён")
                            return False

                        wp = self._route_planner.get_active_waypoint()
                        if not wp:
                            self.disengage("Нет активной точки")
                            return False

                        target_bearing = bearing_to(position.lat, position.lon, wp.lat, wp.lon)
                else:
                    target_bearing = bearing_to(position.lat, position.lon, wp.lat, wp.lon)

            self._heading_controller.set_target_heading(target_bearing)
            roll_pwm = self._heading_controller.update(telemetry.heading, dt)
            pitch_pwm = self._altitude_controller.update(telemetry.altitude_agl, dt)
            throttle_pwm = self._speed_controller.update(telemetry.airspeed, dt)

            self._proxy.send_rc_override({
                1: roll_pwm,
                2: pitch_pwm,
                3: throttle_pwm,
                4: 0
            })

        self._last_update_time = current_time
        return True

    def check_stick_override(self, rc_channels: list) -> bool:
        if len(rc_channels) < 4:
            logger.warning(f"RC channels too short: {len(rc_channels)}, data: {rc_channels}")
            return False

        threshold = self._config.stick_threshold
        roll = rc_channels[self.CH_ROLL]
        yaw = rc_channels[self.CH_YAW]
        roll_diff = abs(roll - self.PWM_CENTER)
        yaw_diff = abs(yaw - self.PWM_CENTER)

        override_detected = roll_diff > threshold or yaw_diff > threshold

        if override_detected:
            self._stick_override_count += 1
            logger.debug(f"RC RAW: roll={roll} (diff={roll_diff}), yaw={yaw} (diff={yaw_diff}), count={self._stick_override_count}/{self._stick_override_threshold_count}")

            if self._stick_override_count >= self._stick_override_threshold_count:
                logger.info(f"STICK OVERRIDE CONFIRMED: roll={roll}, yaw={yaw}, count={self._stick_override_count}")
                return True
        else:
            if self._stick_override_count > 0:
                logger.debug(f"RC RAW: roll={roll}, yaw={yaw} - reset count from {self._stick_override_count}")
            self._stick_override_count = 0

        return False

    def get_target_heading(self) -> float:
        return self._heading_controller.get_target_heading()

    def get_heading_error(self) -> float:
        return self._heading_controller.get_current_error()

    def get_status(self) -> dict:
        status = {
            'mode': self._mode.name,
            'target_heading': self._heading_controller.get_target_heading(),
            'heading_error': self._heading_controller.get_current_error(),
            'target_altitude': self._altitude_controller.get_target_altitude(),
            'altitude_error': self._altitude_controller.get_current_error(),
            'target_airspeed': self._speed_controller.get_target_speed(),
            'airspeed_error': self._speed_controller.get_current_error(),
            'last_update': self._last_update_time,
            'disengage_reason': self._disengage_reason,
            'action': 'IDLE',
            'is_orbiting': self._is_orbiting,
            'orbit_turns_completed': self._orbit_turns_completed,
            'orbit_radius': 0.0,
        }

        if self._mode == AutopilotMode.NAV and self._route_planner:
            wp = self._route_planner.get_active_waypoint()
            if wp:
                alt_error = self._altitude_controller.get_current_error()
                vertical_action = ''
                if abs(alt_error) > 5.0:
                    if alt_error > 0:
                        vertical_action = ' (набор)'
                    else:
                        vertical_action = ' (снижение)'

                if self._is_orbiting:
                    status['action'] = 'ORBITING'
                    status['orbit_radius'] = wp.orbit_radius
                    if wp.action == 'ORBIT_TURNS':
                        status['action'] = f'ORBIT_{self._orbit_turns_completed}/{wp.orbit_turns}{vertical_action}'
                    elif wp.action == 'ORBIT_INFINITE':
                        status['action'] = f'ORBIT_INF{vertical_action}'
                else:
                    status['action'] = f'TO_WAYPOINT{vertical_action}'
                    status['orbit_radius'] = wp.orbit_radius

        return status

    def _start_orbit(self, wp, current_heading: float):
        self._is_orbiting = True
        self._orbit_turns_completed = 0
        self._orbit_last_heading = current_heading
        self._orbit_heading_accumulated = 0.0
        logger.info(f"START ORBIT at WP{wp.id}: action={wp.action}, radius={wp.orbit_radius}")

    def _calculate_orbit_heading(self, position: LatLon, wp, current_heading: float) -> float:
        bearing_to_wp = bearing_to(position.lat, position.lon, wp.lat, wp.lon)
        orbit_heading = (bearing_to_wp + 90) % 360
        return orbit_heading

    def _update_orbit_progress(self, current_heading: float):
        heading_diff = current_heading - self._orbit_last_heading

        if heading_diff > 180:
            heading_diff -= 360
        elif heading_diff < -180:
            heading_diff += 360

        self._orbit_heading_accumulated += heading_diff
        self._orbit_last_heading = current_heading

        new_turns = int(abs(self._orbit_heading_accumulated) / 360)
        if new_turns > self._orbit_turns_completed:
            self._orbit_turns_completed = new_turns
            logger.info(f"ORBIT TURN {self._orbit_turns_completed} completed")

    def _finish_orbit_and_advance(self, old_wp):
        self._is_orbiting = False
        self._orbit_turns_completed = 0
        self._orbit_heading_accumulated = 0.0

        self._route_planner.next_waypoint()
        new_wp = self._route_planner.get_active_waypoint()

        if new_wp and new_wp.id != self._active_waypoint_id:
            self._active_waypoint_id = new_wp.id
            self._event_bus.emit(Event.WAYPOINT_REACHED, {
                'reached': old_wp.id if old_wp else 0,
                'next': new_wp.id
            })

        if self._route_planner.is_route_complete():
            self.disengage("Маршрут завершён")

    def set_target_altitude(self, altitude: float):
        self._altitude_controller.set_target_altitude(altitude)
        if self._route_planner:
            wp = self._route_planner.get_active_waypoint()
            if wp:
                wp.altitude = altitude
                logger.info(f"TARGET ALTITUDE SET: {altitude}m for WP{wp.id}")

    def set_orbit_radius(self, radius: float):
        if self._route_planner:
            wp = self._route_planner.get_active_waypoint()
            if wp:
                wp.orbit_radius = radius
                logger.info(f"ORBIT RADIUS SET: {radius}m for WP{wp.id}")

    def set_target_airspeed(self, speed: float):
        self._speed_controller.set_target_speed(speed)
        logger.info(f"TARGET AIRSPEED SET: {speed} m/s")

    def _on_connection_lost(self, data):
        self.disengage("Соединение потеряно")
