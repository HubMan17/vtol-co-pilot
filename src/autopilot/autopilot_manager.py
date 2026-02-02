import time
from enum import Enum, auto
from typing import Optional, TYPE_CHECKING

from src.mavlink.proxy import MAVLinkProxy
from src.mavlink.telemetry import LatLon
from src.core.config import AutopilotConfig
from src.core.events import EventBus, Event
from src.autopilot.heading_controller import HeadingController
from src.navigation.calculations import bearing_to

if TYPE_CHECKING:
    from src.navigation.route_planner import RoutePlanner


class AutopilotMode(Enum):
    MANUAL = auto()
    HEADING_HOLD = auto()
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
        self._route_planner: Optional['RoutePlanner'] = None

        self._last_update_time = 0.0
        self._disengage_reason = ""
        self._active_waypoint_id = -1

        self._event_bus.subscribe(Event.CONNECTION_LOST, self._on_connection_lost)

    def set_route_planner(self, route_planner: 'RoutePlanner'):
        self._route_planner = route_planner

    def engage_heading_hold(self, target_heading: float = None) -> bool:
        if not self._proxy.is_connected():
            return False

        telemetry = self._proxy.get_telemetry()

        if target_heading is None:
            target_heading = telemetry.heading

        self._heading_controller.set_target_heading(target_heading)
        self._heading_controller.reset()

        self._mode = AutopilotMode.HEADING_HOLD
        self._last_update_time = time.time()

        self._event_bus.emit(Event.AUTOPILOT_ENGAGE, {
            'mode': 'HEADING_HOLD',
            'target': target_heading
        })

        return True

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
        self._active_waypoint_id = wp.id

        self._mode = AutopilotMode.NAV
        self._last_update_time = time.time()

        self._event_bus.emit(Event.AUTOPILOT_ENGAGE, {
            'mode': 'NAV',
            'waypoint': wp.id,
            'total': len(route.waypoints)
        })

        return True

    def disengage(self, reason: str = ""):
        if self._mode == AutopilotMode.MANUAL:
            return

        self._disengage_reason = reason
        prev_mode = self._mode
        self._mode = AutopilotMode.MANUAL

        self._proxy.release_rc_override()

        self._heading_controller.reset()

        self._event_bus.emit(Event.AUTOPILOT_DISENGAGE, {
            'previous_mode': prev_mode.name,
            'reason': reason
        })

    def get_mode(self) -> AutopilotMode:
        return self._mode

    def is_engaged(self) -> bool:
        return self._mode != AutopilotMode.MANUAL

    def update(self) -> bool:
        if self._mode == AutopilotMode.MANUAL:
            return False

        if not self._proxy.is_connected():
            self.disengage("Соединение потеряно")
            return False

        current_time = time.time()
        telemetry = self._proxy.get_telemetry()

        if self.check_stick_override(telemetry.rc_channels):
            self.disengage("Пилот взял управление")
            return False

        timeout_sec = self._config.timeout_ms / 1000.0
        if self._last_update_time > 0 and (current_time - self._last_update_time) > timeout_sec:
            self.disengage("Таймаут обновления")
            return False

        dt = current_time - self._last_update_time if self._last_update_time > 0 else 0.05

        if self._mode == AutopilotMode.HEADING_HOLD:
            roll_pwm = self._heading_controller.update(telemetry.heading, dt)

            self._proxy.send_rc_override({
                1: roll_pwm,
                2: 0,
                3: 0,
                4: 0
            })

        elif self._mode == AutopilotMode.NAV:
            if not self._route_planner:
                self.disengage("Маршрут не задан")
                return False

            position = telemetry.position
            if not position:
                self._last_update_time = current_time
                return True

            if self._route_planner.is_waypoint_reached(position):
                old_wp = self._route_planner.get_active_waypoint()
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
                    return False

            wp = self._route_planner.get_active_waypoint()
            if not wp:
                self.disengage("Нет активной точки")
                return False

            target_bearing = bearing_to(position.lat, position.lon, wp.lat, wp.lon)
            self._heading_controller.set_target_heading(target_bearing)

            roll_pwm = self._heading_controller.update(telemetry.heading, dt)

            self._proxy.send_rc_override({
                1: roll_pwm,
                2: 0,
                3: 0,
                4: 0
            })

        self._last_update_time = current_time
        return True

    def check_stick_override(self, rc_channels: list) -> bool:
        if len(rc_channels) < 4:
            return False

        threshold = self._config.stick_threshold

        if abs(rc_channels[self.CH_ROLL] - self.PWM_CENTER) > threshold:
            return True

        if abs(rc_channels[self.CH_YAW] - self.PWM_CENTER) > threshold:
            return True

        return False

    def get_target_heading(self) -> float:
        return self._heading_controller.get_target_heading()

    def get_heading_error(self) -> float:
        return self._heading_controller.get_current_error()

    def get_status(self) -> dict:
        return {
            'mode': self._mode.name,
            'target_heading': self._heading_controller.get_target_heading(),
            'heading_error': self._heading_controller.get_current_error(),
            'last_update': self._last_update_time,
            'disengage_reason': self._disengage_reason
        }

    def _on_connection_lost(self, data):
        self.disengage("Соединение потеряно")
