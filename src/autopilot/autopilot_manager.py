import time
import logging
import math
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
        logging.StreamHandler(),
    ]
)
logger = logging.getLogger(__name__)
from src.mavlink.telemetry import LatLon
from src.core.config import AutopilotConfig
from src.core.events import EventBus, Event
from src.autopilot.heading_controller import HeadingController
from src.autopilot.altitude_controller import AltitudeController
from src.autopilot.speed_controller import SpeedController
from src.navigation.calculations import bearing_to, haversine_distance, project_point

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
    GUIDED_PROJECTION_DISTANCE = 2000.0  # meters ahead for GUIDED target projection

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
        self._engage_time = 0.0
        self._disengage_reason = ""
        self._active_waypoint_id = -1
        self._stick_override_count = 0
        self._stick_override_threshold_count = 3
        self._throttle_baseline = 0  # PWM газа при engage

        self._is_orbiting = False
        self._orbit_turns_completed = 0
        self._orbit_last_heading = 0.0
        self._orbit_heading_accumulated = 0.0
        self._waiting_for_altitude = False
        self._home_position: Optional[LatLon] = None
        self._returning_home = False
        self._saved_airspeed_cruise: Optional[float] = None  # original AIRSPEED_CRUISE to restore
        self._loiter_alt_transition = False  # GUIDED altitude transition during LOITER orbit
        self._orbit_advance_handled = False  # prevent repeated _finish_orbit_and_advance calls
        self._orbit_reposition_sent = False  # DO_REPOSITION sent for current orbit
        self._orbit_ccw = False  # orbit direction: False=CW, True=CCW
        self._tangent_approach_wp_id = -1  # waypoint ID for which orbit direction was pre-chosen
        self._use_dr = False  # pilot toggled "Использовать счисление"

        # Throttle GUIDED target sends: ArduPlane resets altitude path interpolation
        # (prev_WP_loc = current_loc) on every mission_item_int(current=2).
        # Sending every 100ms means altitude progress is always ~0%.
        # Sending every 2s with short projection gives real altitude convergence.
        self._guided_send_time = 0.0
        self._GUIDED_RESEND_INTERVAL = 2.0  # seconds between GUIDED target resends

        self._log_counter = 0
        self._gps_input_counter = 0
        self._GPS_INPUT_EVERY_N = 2  # every 2 update cycles = ~200ms at 100ms timer

        self._event_bus.subscribe(Event.CONNECTION_LOST, self._on_connection_lost)

    def set_route_planner(self, route_planner: 'RoutePlanner'):
        self._route_planner = route_planner

    def set_home_position(self, position: Optional[LatLon]):
        """Set home position for return-to-home after route completion"""
        self._home_position = position
        if position:
            logger.info(f"HOME POSITION SET: lat={position.lat:.6f}, lon={position.lon:.6f}")
        else:
            logger.info("HOME POSITION CLEARED")

    def set_use_dr(self, enabled: bool):
        if enabled != self._use_dr:
            self._use_dr = enabled
            logger.info(f"USE DR: {'ON' if enabled else 'OFF'}")

    def engage_nav(self) -> bool:
        if not self._proxy.is_connected():
            return False

        if not self._route_planner:
            return False

        route = self._route_planner.get_route()
        if not route or not route.waypoints:
            return False

        self._heading_controller.reset()
        self._altitude_controller.reset()
        self._speed_controller.reset()

        telemetry = self._proxy.get_telemetry()

        wp = self._route_planner.get_active_waypoint()
        if not wp:
            # Route might be "complete" (index past end) — reset to last waypoint
            self._route_planner.set_active_waypoint(len(route.waypoints) - 1)
            wp = self._route_planner.get_active_waypoint()
            if not wp:
                return False

        if wp.climb_enroute:
            self._altitude_controller.set_target_altitude(wp.altitude)
        else:
            self._altitude_controller.set_target_altitude(telemetry.altitude_agl)
        self._stick_override_count = 0
        self._throttle_baseline = telemetry.rc_channels[self.CH_THROTTLE]
        self._active_waypoint_id = wp.id
        # Restore cruise airspeed if re-engaging from orbit
        if self._saved_airspeed_cruise is not None:
            self._proxy.set_cruise_airspeed(self._saved_airspeed_cruise)
            self._saved_airspeed_cruise = None
        self._proxy.set_param('THROTTLE_NUDGE', 1)

        self._is_orbiting = False
        self._orbit_turns_completed = 0
        self._orbit_heading_accumulated = 0.0
        self._orbit_reposition_sent = False
        self._waiting_for_altitude = False
        self._returning_home = False
        self._loiter_alt_transition = False
        self._disengage_reason = ""
        self._guided_send_time = 0.0  # force immediate send on engage

        now = time.time()
        self._mode = AutopilotMode.NAV
        self._last_update_time = now
        self._engage_time = now

        # Force re-enter GUIDED to clear any DO_REPOSITION orbit radius
        self._proxy.set_mode('FBWA')
        self._proxy.set_mode('GUIDED')
        self._proxy.send_speed(self._speed_controller.get_target_speed())
        # Send initial target towards waypoint
        target_alt = self._altitude_controller.get_target_altitude()
        if telemetry.position:
            init_bearing = bearing_to(telemetry.position.lat, telemetry.position.lon, wp.lat, wp.lon)
            tgt_lat, tgt_lon = project_point(telemetry.position.lat, telemetry.position.lon, init_bearing, 2000.0)
            self._proxy.send_guided_target(tgt_lat, tgt_lon, target_alt)
        # Direct altitude command — doesn't reset path interpolation
        if wp.climb_enroute:
            self._proxy.send_guided_change_altitude(target_alt)

        logger.info(f"ENGAGE NAV: waypoint={wp.id}/{len(route.waypoints)}")

        self._event_bus.emit(Event.AUTOPILOT_ENGAGE, {
            'mode': 'NAV',
            'waypoint': wp.id,
            'total': len(route.waypoints)
        })

        return True

    def engage_home(self) -> bool:
        """Engage return-to-home: fly to home at current altitude, orbit and descend to 50m."""
        if not self._proxy.is_connected():
            return False
        if not self._home_position:
            return False

        self._heading_controller.reset()
        self._altitude_controller.reset()
        self._speed_controller.reset()

        telemetry = self._proxy.get_telemetry()

        # Keep current altitude during flight to home
        self._altitude_controller.set_target_altitude(telemetry.altitude_agl)
        self._stick_override_count = 0
        self._throttle_baseline = telemetry.rc_channels[self.CH_THROTTLE]
        # Sync active_waypoint_id with current route to prevent false redirect detection
        if self._route_planner:
            wp = self._route_planner.get_active_waypoint()
            self._active_waypoint_id = wp.id if wp else -1
        else:
            self._active_waypoint_id = -1

        # Restore cruise airspeed if re-engaging from orbit
        if self._saved_airspeed_cruise is not None:
            self._proxy.set_cruise_airspeed(self._saved_airspeed_cruise)
            self._saved_airspeed_cruise = None
        self._proxy.set_param('THROTTLE_NUDGE', 1)

        self._is_orbiting = False
        self._orbit_turns_completed = 0
        self._orbit_heading_accumulated = 0.0
        self._orbit_reposition_sent = False
        self._waiting_for_altitude = False
        self._returning_home = True
        self._loiter_alt_transition = False
        self._disengage_reason = ""
        self._guided_send_time = 0.0

        now = time.time()
        self._mode = AutopilotMode.NAV
        self._last_update_time = now
        self._engage_time = now

        # Force re-enter GUIDED to clear any DO_REPOSITION orbit radius
        self._proxy.set_mode('FBWA')
        self._proxy.set_mode('GUIDED')
        self._proxy.send_speed(self._speed_controller.get_target_speed())

        # Pre-choose orbit direction for smooth tangent approach
        if telemetry.position:
            self._orbit_ccw = self._choose_orbit_direction_ccw(
                telemetry.position, self._home_position.lat, self._home_position.lon, telemetry.heading
            )

        # Send initial target towards home
        if telemetry.position:
            init_bearing = bearing_to(telemetry.position.lat, telemetry.position.lon,
                                       self._home_position.lat, self._home_position.lon)
            tgt_lat, tgt_lon = project_point(telemetry.position.lat, telemetry.position.lon,
                                              init_bearing, 2000.0)
            self._proxy.send_guided_target(tgt_lat, tgt_lon, telemetry.altitude_agl)

        logger.info(f"ENGAGE HOME: lat={self._home_position.lat:.6f} lon={self._home_position.lon:.6f} "
                     f"alt={telemetry.altitude_agl:.1f}m")

        self._event_bus.emit(Event.AUTOPILOT_ENGAGE, {'mode': 'HOME'})
        return True

    def disengage(self, reason: str = ""):
        if self._mode == AutopilotMode.MANUAL:
            return

        logger.info(f"DISENGAGE: reason='{reason}', prev_mode={self._mode.name}")

        self._disengage_reason = reason
        prev_mode = self._mode
        self._mode = AutopilotMode.MANUAL

        self._proxy.set_mode('CRUISE')

        self._heading_controller.reset()
        self._altitude_controller.reset()
        self._speed_controller.reset()

        self._is_orbiting = False
        self._waiting_for_altitude = False
        self._returning_home = False
        self._loiter_alt_transition = False
        self._tangent_approach_wp_id = -1

        # Restore cruise airspeed and THROTTLE_NUDGE if we modified them for LOITER
        if self._saved_airspeed_cruise is not None:
            self._proxy.set_cruise_airspeed(self._saved_airspeed_cruise)
            logger.info(f"RESTORED AIRSPEED_CRUISE={self._saved_airspeed_cruise:.1f}")
            self._saved_airspeed_cruise = None
        self._proxy.set_param('THROTTLE_NUDGE', 1)

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

        time_since_engage = current_time - self._engage_time if self._engage_time > 0 else 0
        if time_since_engage > 2.0:
            if self.check_stick_override(telemetry.rc_channels):
                self.disengage("Пилот взял управление")
                return False

        timeout_sec = self._config.timeout_ms / 1000.0
        if self._last_update_time > 0 and (current_time - self._last_update_time) > timeout_sec:
            self.disengage("Таймаут обновления")
            return False

        if self._mode == AutopilotMode.NAV:
            if not self._route_planner:
                self.disengage("Маршрут не задан")
                return False

            position = override_position if override_position else telemetry.position
            if not position:
                self._last_update_time = current_time
                return True

            # GPS monitoring and GPS_INPUT injection
            gps_ok = telemetry.gps_fix >= 3

            # DR or GPS loss → need heading-based GUIDED orbit instead of DO_REPOSITION orbit
            need_guided_orbit = not gps_ok

            # If ArduPilot exited our mode unexpectedly
            # Skip check for first 2 seconds after engage (mode takes time to update via HEARTBEAT)
            expected_modes = {'GUIDED'}
            time_since_engage = current_time - self._engage_time if self._engage_time > 0 else 0
            if time_since_engage > 2.0 and telemetry.mode and telemetry.mode not in expected_modes:
                logger.warning(f"MODE CHECK FAILED: telemetry.mode='{telemetry.mode}' not in {expected_modes}")
                self.disengage(f"Режим изменён: {telemetry.mode}")
                return False

            # GPS_INPUT: warmup when GPS OK, primary source when GPS lost
            self._send_gps_input_if_needed(position, telemetry)

            # Detect if user changed the active waypoint (via GUI spinner, prev/next, or new waypoint added)
            wp_check = self._route_planner.get_active_waypoint()
            if wp_check and wp_check.id != self._active_waypoint_id:
                if self._is_orbiting or self._returning_home:
                    logger.info(f"REDIRECT: WP{self._active_waypoint_id} -> WP{wp_check.id} "
                                f"(was {'returning home' if self._returning_home else 'orbiting'})")
                    self._exit_orbit()
                    self._returning_home = False
                    self._waiting_for_altitude = False
                    if wp_check.climb_enroute:
                        self._altitude_controller.set_target_altitude(wp_check.altitude)
                    else:
                        self._altitude_controller.set_target_altitude(telemetry.altitude_agl)
                self._active_waypoint_id = wp_check.id

            # Handle return-to-home mode
            if self._returning_home and self._home_position:
                distance_to_home = haversine_distance(position.lat, position.lon,
                                                      self._home_position.lat, self._home_position.lon)

                # Start orbit when aircraft reaches orbit radius — smooth entry from edge
                if distance_to_home <= 150.0 or self._is_orbiting:
                    if not self._is_orbiting:
                        # Start orbiting at home — enter orbit, descend to 50m via DO_REPOSITION
                        self._is_orbiting = True
                        self._orbit_turns_completed = 0
                        self._orbit_heading_accumulated = 0.0
                        self._orbit_last_heading = telemetry.heading
                        # Orbit direction already chosen in engage_home() for tangent approach
                        # Enter orbit via DO_REPOSITION with target alt=50m
                        # ArduPlane descends while orbiting — no need for alt transition
                        if gps_ok:
                            if self._saved_airspeed_cruise is None:
                                self._saved_airspeed_cruise = telemetry.airspeed if telemetry.airspeed > 0 else 25.0
                            self._proxy.set_cruise_airspeed(self._speed_controller.get_target_speed())
                            self._proxy.send_speed(self._speed_controller.get_target_speed())
                            self._proxy.set_param('THROTTLE_NUDGE', 0)
                            self._proxy.set_param('WP_LOITER_RAD', 150.0)
                            self._proxy.send_loiter_unlim(self._home_position.lat, self._home_position.lon, 50.0, 150.0, ccw=self._orbit_ccw)
                            self._orbit_reposition_sent = True
                        else:
                            self._orbit_reposition_sent = False

                        self._altitude_controller.set_target_altitude(50.0)
                        self._proxy.send_guided_change_altitude(50.0)
                        logger.info(f"HOME REACHED -> ORBITING + DESCENDING: current={telemetry.altitude_agl:.1f}m -> 50m "
                                     f"{'CCW' if self._orbit_ccw else 'CW'}")

                    # Orbit: DO_REPOSITION handles orbit + altitude, we track progress
                    self._update_orbit_progress(telemetry.heading)
                    self._altitude_controller.update(telemetry.altitude_agl)
                    self._speed_controller.update(telemetry.airspeed)

                    if need_guided_orbit:
                        # DR active or GPS lost — orbit via GUIDED heading targets around home
                        self._orbit_reposition_sent = False
                        now = time.time()
                        if now - self._guided_send_time >= self._GUIDED_RESEND_INTERVAL:
                            orbit_heading = self._calculate_orbit_heading_for_point(
                                position, self._home_position, 150.0, telemetry.heading
                            )
                            tgt_lat, tgt_lon = project_point(
                                position.lat, position.lon, orbit_heading, 300.0
                            )
                            target_alt = self._altitude_controller.get_target_altitude()
                            self._proxy.send_guided_target(tgt_lat, tgt_lon, target_alt)
                            self._proxy.send_speed(self._speed_controller.get_target_speed())
                            self._guided_send_time = now
                            logger.debug(f"GUIDED ORBIT HOME (DR): hdg={orbit_heading:.1f} alt={target_alt:.1f}")
                    else:
                        # GPS OK, no DR — orbit via DO_REPOSITION (GUIDED with radius)
                        if not self._orbit_reposition_sent:
                            self._proxy.send_loiter_unlim(self._home_position.lat, self._home_position.lon, 50.0, 150.0, ccw=self._orbit_ccw)
                            self._orbit_reposition_sent = True
                else:
                    # Navigate to home
                    if distance_to_home <= 300.0:
                        # Tangent approach: aim for geometric tangent point on orbit circle
                        # Angle = arccos(R/D) — the angle at circle center between the line to aircraft
                        # and the line to the tangent point. At D=300m, R=150m → 60°. At D=150m → 0°.
                        bearing_from_home = bearing_to(self._home_position.lat, self._home_position.lon,
                                                       position.lat, position.lon)
                        angle_deg = math.degrees(math.acos(min(150.0 / distance_to_home, 1.0)))
                        if self._orbit_ccw:
                            tangent_bearing = (bearing_from_home - angle_deg) % 360
                        else:
                            tangent_bearing = (bearing_from_home + angle_deg) % 360
                        tgt_lat, tgt_lon = project_point(self._home_position.lat, self._home_position.lon,
                                                          tangent_bearing, 150.0)
                        target_bearing = bearing_to(position.lat, position.lon, tgt_lat, tgt_lon)
                    else:
                        target_bearing = bearing_to(position.lat, position.lon,
                                                   self._home_position.lat, self._home_position.lon)
                    self._heading_controller.set_target_heading(target_bearing)
                    self._heading_controller.update(telemetry.heading)
                    self._altitude_controller.update(telemetry.altitude_agl)
                    self._speed_controller.update(telemetry.airspeed)
                    self._send_guided_commands(target_bearing, position)

                self._last_update_time = current_time
                return True

            wp = self._route_planner.get_active_waypoint()
            if not wp:
                self.disengage("Нет активной точки")
                return False

            distance_to_wp = haversine_distance(position.lat, position.lon, wp.lat, wp.lon)

            if self._is_orbiting:
                # Orbit mode: LOITER handles everything, we just track progress
                self._update_orbit_progress(telemetry.heading)
                self._altitude_controller.update(telemetry.altitude_agl)
                self._speed_controller.update(telemetry.airspeed)
                target_bearing = telemetry.heading  # for logging only

                if need_guided_orbit:
                    # DR active or GPS lost — orbit via GUIDED heading targets around orbit center
                    self._orbit_reposition_sent = False
                    now = time.time()
                    if now - self._guided_send_time >= self._GUIDED_RESEND_INTERVAL:
                        orbit_heading = self._calculate_orbit_heading(position, wp, telemetry.heading)
                        tgt_lat, tgt_lon = project_point(
                            position.lat, position.lon, orbit_heading, 300.0
                        )
                        target_alt = self._altitude_controller.get_target_altitude()
                        self._proxy.send_guided_target(tgt_lat, tgt_lon, target_alt)
                        self._proxy.send_speed(self._speed_controller.get_target_speed())
                        self._guided_send_time = now
                        logger.debug(f"GUIDED ORBIT (DR): hdg={orbit_heading:.1f} alt={target_alt:.1f}")
                else:
                    # GPS OK, no DR — orbit via DO_REPOSITION (GUIDED with radius)
                    if not self._orbit_reposition_sent:
                        radius = getattr(wp, 'orbit_radius', 150.0)
                        if radius <= 0:
                            radius = 150.0
                        self._proxy.send_loiter_unlim(wp.lat, wp.lon, wp.altitude, radius, ccw=self._orbit_ccw)
                        self._orbit_reposition_sent = True

                # ALTITUDE: auto-advance when target altitude reached during orbit
                if wp.action == "ALTITUDE" and not self._orbit_advance_handled:
                    if self._altitude_controller.is_on_altitude(tolerance=5.0):
                        self._orbit_advance_handled = True
                        logger.info(f"ALTITUDE REACHED at WP{wp.id}: alt={telemetry.altitude_agl:.1f}m target={wp.altitude}m")
                        self._finish_orbit_and_advance(wp)

                # ORBIT_TURNS: auto-advance when turns complete (if there's somewhere to go)
                if wp.action == "ORBIT_TURNS" and self._orbit_turns_completed >= wp.orbit_turns and not self._orbit_advance_handled:
                    self._orbit_advance_handled = True
                    self._finish_orbit_and_advance(wp)

            else:
                if self._route_planner.is_waypoint_reached(position):
                    # Set altitude target on arrival if not climbing enroute
                    if not wp.climb_enroute and not self._waiting_for_altitude:
                        self._altitude_controller.set_target_altitude(wp.altitude)
                        self._altitude_controller.update(telemetry.altitude_agl)  # refresh error before checking

                        if wp.action in ("ORBIT_TURNS", "ORBIT_INFINITE", "ALTITUDE"):
                            # Orbit waypoints: start DO_REPOSITION orbit immediately
                            # Altitude handled by send_guided_change_altitude (COMMAND_INT) — TECS tracks independently
                            self._start_orbit(wp, telemetry.heading)
                            if not self._altitude_controller.is_on_altitude(tolerance=5.0):
                                self._proxy.send_guided_change_altitude(wp.altitude)
                                logger.info(f"ORBIT + ALT CHANGE at WP{wp.id}: target={wp.altitude}m")
                            else:
                                logger.info(f"START ORBIT at WP{wp.id} (altitude OK)")
                            target_bearing = telemetry.heading
                        else:
                            self._waiting_for_altitude = True
                            self._guided_send_time = 0.0  # force immediate send
                            # Direct altitude command — doesn't reset path interpolation
                            self._proxy.send_guided_change_altitude(wp.altitude)
                            logger.info(f"WAITING FOR ALTITUDE: target={wp.altitude}m at WP{wp.id}")

                    # Check if we need to wait for altitude before advancing (FLYTHROUGH only)
                    if self._waiting_for_altitude:
                        self._altitude_controller.update(telemetry.altitude_agl)
                        if self._altitude_controller.is_on_altitude(tolerance=5.0):
                            logger.info(f"ALTITUDE REACHED at WP{wp.id}")
                            self._waiting_for_altitude = False
                        else:
                            # Throttle sends: ArduPlane resets altitude path on every mission_item_int.
                            # Send every 2s so aircraft makes real progress along the path.
                            now = time.time()
                            if now - self._guided_send_time >= self._GUIDED_RESEND_INTERVAL:
                                target_alt = self._altitude_controller.get_target_altitude()
                                tgt_lat, tgt_lon = project_point(
                                    position.lat, position.lon, telemetry.heading, 200.0
                                )
                                self._proxy.send_guided_target(tgt_lat, tgt_lon, target_alt)
                                self._proxy.send_speed(self._speed_controller.get_target_speed())
                                self._guided_send_time = now
                            target_bearing = telemetry.heading

                    if not self._waiting_for_altitude:
                        if wp.action in ("ORBIT_TURNS", "ORBIT_INFINITE", "ALTITUDE"):
                            if not self._is_orbiting:
                                # climb_enroute=True case: altitude already OK, just start orbit
                                self._start_orbit(wp, telemetry.heading)
                                logger.info(f"START ORBIT at WP{wp.id} (climb_enroute)")
                            target_bearing = telemetry.heading
                        else:
                            old_wp = wp
                            old_idx = self._route_planner.get_active_waypoint_index()
                            self._route_planner.next_waypoint()
                            new_idx = self._route_planner.get_active_waypoint_index()
                            new_wp = self._route_planner.get_active_waypoint()

                            if new_idx != old_idx and new_wp:
                                self._active_waypoint_id = new_wp.id
                                if new_wp.climb_enroute:
                                    self._altitude_controller.set_target_altitude(new_wp.altitude)
                                    self._waiting_for_altitude = False
                                    logger.info(f"CLIMB ENROUTE to WP{new_wp.id}: target={new_wp.altitude}m")
                                else:
                                    self._altitude_controller.set_target_altitude(telemetry.altitude_agl)
                                    self._waiting_for_altitude = False
                                    logger.info(f"MAINTAIN ALTITUDE to WP{new_wp.id}: current={telemetry.altitude_agl}m, will adjust to {new_wp.altitude}m on arrival")
                                self._event_bus.emit(Event.WAYPOINT_REACHED, {
                                    'reached': old_wp.id if old_wp else 0,
                                    'next': new_wp.id
                                })
                            else:
                                # Last waypoint reached (FLYTHROUGH) — route complete
                                logger.info(f"LAST WAYPOINT REACHED: WP{old_wp.id} (FLYTHROUGH)")
                                self._handle_route_completion()
                                self._last_update_time = current_time
                                return True

                            wp = self._route_planner.get_active_waypoint()
                            if not wp:
                                self.disengage("Нет активной точки")
                                return False

                            target_bearing = bearing_to(position.lat, position.lon, wp.lat, wp.lon)
                            self._heading_controller.set_target_heading(target_bearing)
                            self._heading_controller.update(telemetry.heading)
                            self._altitude_controller.update(telemetry.altitude_agl)
                            self._speed_controller.update(telemetry.airspeed)
                            self._send_guided_commands(target_bearing, position)
                else:
                    # Flying to waypoint — send GUIDED target
                    has_orbit = wp.action in ("ORBIT_TURNS", "ORBIT_INFINITE", "ALTITUDE")
                    orbit_radius = getattr(wp, 'orbit_radius', 150.0)
                    if orbit_radius <= 0:
                        orbit_radius = 150.0

                    if has_orbit and distance_to_wp <= orbit_radius * 2:
                        # Tangent approach: curve into orbit circle instead of flying head-on
                        if self._tangent_approach_wp_id != wp.id:
                            self._orbit_ccw = self._choose_orbit_direction_ccw(
                                position, wp.lat, wp.lon, telemetry.heading
                            )
                            self._tangent_approach_wp_id = wp.id
                        bearing_from_wp = bearing_to(wp.lat, wp.lon, position.lat, position.lon)
                        angle_deg = math.degrees(math.acos(min(orbit_radius / distance_to_wp, 1.0)))
                        if self._orbit_ccw:
                            tangent_bearing = (bearing_from_wp - angle_deg) % 360
                        else:
                            tangent_bearing = (bearing_from_wp + angle_deg) % 360
                        tgt_lat, tgt_lon = project_point(wp.lat, wp.lon, tangent_bearing, orbit_radius)
                        target_bearing = bearing_to(position.lat, position.lon, tgt_lat, tgt_lon)
                    else:
                        target_bearing = bearing_to(position.lat, position.lon, wp.lat, wp.lon)

                    self._heading_controller.set_target_heading(target_bearing)
                    self._heading_controller.update(telemetry.heading)
                    self._altitude_controller.update(telemetry.altitude_agl)
                    self._speed_controller.update(telemetry.airspeed)
                    self._send_guided_commands(target_bearing, position)

            # Diagnostic logging every ~2 seconds
            self._log_counter += 1
            if self._log_counter >= 20:
                self._log_counter = 0
                logger.info(
                    f"GUIDED: "
                    f"hdg={telemetry.heading:.0f}->tgt={target_bearing:.0f} err={self._heading_controller.get_current_error():.1f} | "
                    f"alt={telemetry.altitude_agl:.1f} tgt={self._altitude_controller.get_target_altitude():.1f} err={self._altitude_controller.get_current_error():.1f} | "
                    f"spd={telemetry.airspeed:.1f} tgt={self._speed_controller.get_target_speed():.1f} | "
                    f"mode={telemetry.mode} gps={telemetry.gps_fix} armed={telemetry.armed}"
                )

        self._last_update_time = current_time
        return True

    def _send_guided_commands(self, target_bearing: float, position: LatLon):
        """Send GUIDED target with throttling for altitude convergence.
        ArduPlane resets altitude path interpolation (prev_WP = current_loc) on every
        mission_item_int(current=2). Sending every 100ms means ~0% path progress per cycle.
        Throttle to every 2s with short projection so ArduPlane makes real altitude progress.
        When nearly level, send every cycle with long projection for heading responsiveness.
        """
        target_alt = self._altitude_controller.get_target_altitude()
        alt_error = abs(self._altitude_controller.get_current_error())
        now = time.time()

        if alt_error > 5.0:
            # Altitude change needed — throttle sends for TECS altitude convergence
            if now - self._guided_send_time < self._GUIDED_RESEND_INTERVAL:
                return  # skip — let ArduPlane make progress on altitude path
            proj_distance = max(200.0, min(500.0, alt_error * 5.0))
        else:
            # Nearly level — send every cycle for heading responsiveness
            proj_distance = 2000.0

        tgt_lat, tgt_lon = project_point(position.lat, position.lon, target_bearing, proj_distance)
        self._proxy.send_guided_target(tgt_lat, tgt_lon, target_alt)
        self._proxy.send_speed(self._speed_controller.get_target_speed())
        self._guided_send_time = now

    def _send_gps_input_if_needed(self, position: LatLon, telemetry):
        """Send GPS_INPUT: warmup when GPS OK (low priority), primary when GPS lost."""
        if not position:
            return

        self._gps_input_counter += 1
        if self._gps_input_counter < self._GPS_INPUT_EVERY_N:
            return
        self._gps_input_counter = 0

        # Velocity from heading + groundspeed (CRITICAL: never send zero velocity)
        hdg_rad = math.radians(telemetry.heading)
        vn = telemetry.groundspeed * math.cos(hdg_rad)
        ve = telemetry.groundspeed * math.sin(hdg_rad)
        vd = -telemetry.climb_rate  # NED: down is positive

        if telemetry.gps_fix >= 3:
            # GPS alive — warmup: high horiz_accuracy = low priority for EKF
            accuracy = 50.0
        else:
            # GPS lost — DR becomes primary source
            accuracy = 10.0
            logger.debug(f"GPS_INPUT (DR): lat={position.lat:.6f} lon={position.lon:.6f} acc={accuracy}")

        self._proxy.send_gps_input(
            lat=position.lat, lon=position.lon,
            alt=telemetry.altitude_agl,
            vn=vn, ve=ve, vd=vd,
            heading=telemetry.heading,
            horiz_accuracy=accuracy
        )

    def check_stick_override(self, rc_channels: list) -> bool:
        if len(rc_channels) < 4:
            logger.warning(f"RC channels too short: {len(rc_channels)}, data: {rc_channels}")
            return False

        threshold = self._config.stick_threshold
        roll = rc_channels[self.CH_ROLL]
        pitch = rc_channels[self.CH_PITCH]
        throttle = rc_channels[self.CH_THROTTLE]
        yaw = rc_channels[self.CH_YAW]

        # Validate PWM range — disconnected receiver sends 0 or 65535
        for ch_val in (roll, pitch, throttle, yaw):
            if ch_val < 800 or ch_val > 2200:
                return False

        roll_diff = abs(roll - self.PWM_CENTER)
        pitch_diff = abs(pitch - self.PWM_CENTER)
        yaw_diff = abs(yaw - self.PWM_CENTER)
        throttle_diff = abs(throttle - self._throttle_baseline)

        override_detected = roll_diff > threshold or pitch_diff > threshold or yaw_diff > threshold

        # Throttle: skip during LOITER (THROTTLE_NUDGE=0 means ArduPilot ignores it, but PWM still changes)
        if not self._is_orbiting:
            override_detected = override_detected or throttle_diff > threshold

        if override_detected:
            self._stick_override_count += 1
            logger.debug(
                f"RC: roll={roll}({roll_diff}) pitch={pitch}({pitch_diff}) "
                f"yaw={yaw}({yaw_diff}) thr={throttle}({throttle_diff}) "
                f"count={self._stick_override_count}/{self._stick_override_threshold_count}"
            )

            if self._stick_override_count >= self._stick_override_threshold_count:
                logger.info(
                    f"STICK OVERRIDE CONFIRMED: roll={roll} pitch={pitch} "
                    f"yaw={yaw} thr={throttle} (baseline={self._throttle_baseline}), "
                    f"count={self._stick_override_count}"
                )
                return True
        else:
            if self._stick_override_count > 0:
                logger.debug(f"RC: roll={roll} pitch={pitch} yaw={yaw} thr={throttle} - reset count from {self._stick_override_count}")
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
            'returning_home': self._returning_home,
        }

        if self._returning_home and self._home_position:
            alt_error = self._altitude_controller.get_current_error()
            vertical_action = ''
            if abs(alt_error) > 5.0:
                if alt_error > 0:
                    vertical_action = ' (набор)'
                else:
                    vertical_action = ' (снижение)'

            if self._is_orbiting:
                status['action'] = f'ВОЗВРАТ_ДОМОЙ_ОРБИТА{vertical_action}'
            else:
                status['action'] = f'ВОЗВРАТ_ДОМОЙ{vertical_action}'
            status['orbit_radius'] = 150.0

        elif self._mode == AutopilotMode.NAV and self._route_planner:
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
                    elif wp.action == 'ALTITUDE':
                        status['action'] = f'ALTITUDE_ORBIT{vertical_action}'
                elif self._waiting_for_altitude:
                    status['action'] = f'ОЖИДАНИЕ_ВЫСОТЫ{vertical_action}'
                    status['orbit_radius'] = wp.orbit_radius
                else:
                    status['action'] = f'TO_WAYPOINT{vertical_action}'
                    status['orbit_radius'] = wp.orbit_radius

        return status

    def _exit_orbit(self):
        """Exit orbit/LOITER mode and prepare for GUIDED navigation."""
        if not self._is_orbiting:
            return

        self._is_orbiting = False
        self._orbit_turns_completed = 0
        self._orbit_heading_accumulated = 0.0
        self._loiter_alt_transition = False

        # Restore cruise airspeed and THROTTLE_NUDGE
        if self._saved_airspeed_cruise is not None:
            self._proxy.set_cruise_airspeed(self._saved_airspeed_cruise)
            logger.info(f"RESTORED AIRSPEED_CRUISE={self._saved_airspeed_cruise:.1f}")
            self._saved_airspeed_cruise = None
        self._proxy.set_param('THROTTLE_NUDGE', 1)

        # Force re-enter GUIDED to clear DO_REPOSITION orbit radius
        self._proxy.set_mode('FBWA')
        self._proxy.set_mode('GUIDED')
        self._orbit_reposition_sent = False
        logger.info("EXITED ORBIT -> GUIDED")

    def _start_orbit(self, wp, current_heading: float):
        self._is_orbiting = True
        self._orbit_turns_completed = 0
        self._orbit_last_heading = current_heading
        self._orbit_heading_accumulated = 0.0
        self._orbit_advance_handled = False

        radius = getattr(wp, 'orbit_radius', 150.0)
        if radius <= 0:
            radius = 150.0

        telemetry = self._proxy.get_telemetry()

        # Choose orbit direction — skip if already pre-chosen by tangent approach
        if self._tangent_approach_wp_id == wp.id:
            self._tangent_approach_wp_id = -1  # consumed
        elif telemetry.position:
            self._orbit_ccw = self._choose_orbit_direction_ccw(
                telemetry.position, wp.lat, wp.lon, current_heading
            )
        else:
            self._orbit_ccw = False

        # Set AIRSPEED_CRUISE so LOITER uses our target speed (LOITER reads this param directly)
        target_speed = self._speed_controller.get_target_speed()
        if self._saved_airspeed_cruise is None:
            # Save current cruise speed to restore later (approximate from current airspeed)
            self._saved_airspeed_cruise = telemetry.airspeed if telemetry.airspeed > 0 else 25.0
            logger.info(f"SAVED AIRSPEED_CRUISE={self._saved_airspeed_cruise:.1f} for restore")
        self._proxy.set_cruise_airspeed(target_speed)
        if telemetry.gps_fix >= 3:
            # DO_REPOSITION: enters GUIDED with orbit center + radius (like LOITER but exact center)
            self._proxy.send_speed(target_speed)
            self._proxy.set_param('THROTTLE_NUDGE', 0)
            self._proxy.set_param('WP_LOITER_RAD', radius)
            self._proxy.send_loiter_unlim(wp.lat, wp.lon, wp.altitude, radius, ccw=self._orbit_ccw)
            self._orbit_reposition_sent = True
            logger.info(f"START ORBIT (DO_REPOSITION) at WP{wp.id}: radius={radius}, speed={target_speed}, "
                         f"{'CCW' if self._orbit_ccw else 'CW'}")
        else:
            # No GPS — orbit via GUIDED heading commands
            self._orbit_reposition_sent = False
            logger.warning(f"START ORBIT (GUIDED fallback) at WP{wp.id}: radius={radius} — NO GPS")

    def _calculate_orbit_heading_for_point(self, position: LatLon, center: LatLon,
                                              orbit_radius: float, current_heading: float) -> float:
        """Calculate orbit tangent heading for arbitrary center point and radius.
        Respects self._orbit_ccw: CW = bearing+90, CCW = bearing-90."""
        bearing_to_center = bearing_to(position.lat, position.lon, center.lat, center.lon)
        distance_to_center = haversine_distance(position.lat, position.lon, center.lat, center.lon)

        # Radius error: positive = too far from center, negative = too close
        radius_error = distance_to_center - orbit_radius

        # Correction angle via atan — naturally limits to ~±90°
        correction = math.degrees(math.atan2(radius_error, orbit_radius))

        # CW: tangent = bearing + 90, correction inward when too far
        # CCW: tangent = bearing - 90, correction inward when too far
        if self._orbit_ccw:
            orbit_heading = (bearing_to_center - 90 + correction) % 360
        else:
            orbit_heading = (bearing_to_center + 90 - correction) % 360
        return orbit_heading

    def _calculate_orbit_heading(self, position: LatLon, wp, current_heading: float) -> float:
        orbit_radius = getattr(wp, 'orbit_radius', 150.0)
        if orbit_radius <= 0:
            orbit_radius = 150.0
        center = LatLon(wp.lat, wp.lon)
        return self._calculate_orbit_heading_for_point(position, center, orbit_radius, current_heading)

    def _choose_orbit_direction_ccw(self, position: LatLon, center_lat: float,
                                     center_lon: float, heading: float) -> bool:
        """Choose most efficient orbit direction based on approach angle.
        Returns True for CCW, False for CW.
        If the orbit center is to the right of the aircraft → CW is efficient.
        If the orbit center is to the left → CCW is efficient.
        """
        bearing_to_center = bearing_to(position.lat, position.lon, center_lat, center_lon)
        # Relative bearing: 0-180 = center is to the right, 180-360 = center is to the left
        relative = (bearing_to_center - heading + 360) % 360
        ccw = relative >= 180  # center to the left → turn left (CCW)
        direction_str = "CCW" if ccw else "CW"
        logger.info(f"ORBIT DIRECTION: bearing_to_center={bearing_to_center:.0f} heading={heading:.0f} "
                     f"relative={relative:.0f} -> {direction_str}")
        return ccw

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
        """After ORBIT_TURNS complete: advance if possible, otherwise keep orbiting."""
        old_idx = self._route_planner.get_active_waypoint_index()
        self._route_planner.next_waypoint()
        new_idx = self._route_planner.get_active_waypoint_index()
        new_wp = self._route_planner.get_active_waypoint()

        if new_idx != old_idx and new_wp:
            # There's a next waypoint — exit orbit and navigate to it
            logger.info(f"ORBIT COMPLETE: {self._orbit_turns_completed} turns at WP{old_wp.id} -> advancing to WP{new_wp.id}")
            self._exit_orbit()
            self._active_waypoint_id = new_wp.id
            self._waiting_for_altitude = False

            telemetry = self._proxy.get_telemetry()
            if new_wp.climb_enroute:
                self._altitude_controller.set_target_altitude(new_wp.altitude)
            else:
                self._altitude_controller.set_target_altitude(telemetry.altitude_agl)

            self._event_bus.emit(Event.WAYPOINT_REACHED, {
                'reached': old_wp.id if old_wp else 0,
                'next': new_wp.id
            })
        else:
            # Last waypoint — revert next_waypoint() clamping
            self._route_planner.set_active_waypoint(old_idx)

            if self._home_position:
                # Home exists — exit orbit and return home
                logger.info(f"ORBIT COMPLETE: {self._orbit_turns_completed} turns at last WP{old_wp.id} -> returning home")
                self._exit_orbit()
                self._handle_route_completion()
            else:
                # No home, no more waypoints — keep orbiting
                logger.info(f"ORBIT COMPLETE: {self._orbit_turns_completed} turns at last WP{old_wp.id}, no home — continuing orbit")

    def set_target_altitude(self, altitude: float):
        self._altitude_controller.set_target_altitude(altitude)
        self._guided_send_time = 0.0  # force immediate resend with new altitude
        if self._route_planner:
            wp = self._route_planner.get_active_waypoint()
            if wp:
                wp.altitude = altitude
                logger.info(f"TARGET ALTITUDE SET: {altitude}m for WP{wp.id}")

        # Send GUIDED_CHANGE_ALTITUDE — overrides path interpolation directly
        if self._mode != AutopilotMode.MANUAL:
            self._proxy.send_guided_change_altitude(altitude)

        # send_guided_change_altitude (COMMAND_INT) handles altitude independently of orbit
        # DO_REPOSITION orbit continues uninterrupted — no need for GUIDED alt transition

    def set_orbit_radius(self, radius: float):
        if self._route_planner:
            wp = self._route_planner.get_active_waypoint()
            if wp:
                wp.orbit_radius = radius
                if self._is_orbiting:
                    self._proxy.set_param('WP_LOITER_RAD', radius)
                    # Force re-send DO_REPOSITION with new radius on next cycle
                    self._orbit_reposition_sent = False
                logger.info(f"ORBIT RADIUS SET: {radius}m for WP{wp.id}")

    def set_target_airspeed(self, speed: float):
        self._speed_controller.set_target_speed(speed)
        if self._mode != AutopilotMode.MANUAL:
            self._proxy.send_speed(speed)
            if self._is_orbiting:
                self._proxy.set_cruise_airspeed(speed)
        logger.info(f"TARGET AIRSPEED SET: {speed} m/s")

    def _handle_route_completion(self):
        """Handle route completion: return home or orbit indefinitely"""
        if self._home_position:
            logger.info("ROUTE COMPLETE -> RETURNING HOME")
            self._returning_home = True
        else:
            logger.info("ROUTE COMPLETE -> ORBITING INDEFINITELY (no home position)")
            if self._route_planner:
                wp_count = self._route_planner.get_waypoint_count()
                if wp_count > 0:
                    self._route_planner.set_active_waypoint(wp_count - 1)
                    wp = self._route_planner.get_active_waypoint()
                    if wp and not self._is_orbiting:
                        self._start_orbit(wp, self._proxy.get_telemetry().heading)
                        logger.info(f"INFINITE ORBIT started at WP{wp.id}")

    def _on_connection_lost(self, data):
        self.disengage("Соединение потеряно")
