import socket
import threading
import time
import logging
from typing import Callable, Optional, List
from pymavlink import mavutil

from src.mavlink.telemetry import TelemetryParser, TelemetryState
from src.core.events import EventBus, Event

logger = logging.getLogger(__name__)


class MAVLinkProxy:
    def __init__(self, sitl_host: str = "127.0.0.1", sitl_port: int = 5762,
                 proxy_port: int = 14550, system_id: int = 255, component_id: int = 0):
        self.sitl_host = sitl_host
        self.sitl_port = sitl_port
        self.proxy_port = proxy_port
        self.system_id = system_id
        self.component_id = component_id

        self._sitl_conn: Optional[mavutil.mavlink_connection] = None
        self._proxy_socket: Optional[socket.socket] = None
        self._clients: List[socket.socket] = []

        self._running = False
        self._sitl_thread: Optional[threading.Thread] = None
        self._proxy_thread: Optional[threading.Thread] = None

        self._telemetry = TelemetryParser()
        self._event_bus = EventBus()
        self._message_callbacks: List[Callable] = []

        self._connected = False
        self._lock = threading.Lock()

    def connect(self) -> bool:
        try:
            conn_str = f"tcp:{self.sitl_host}:{self.sitl_port}"
            self._sitl_conn = mavutil.mavlink_connection(
                conn_str,
                source_system=self.system_id,
                source_component=self.component_id
            )

            self._sitl_conn.wait_heartbeat(timeout=10)
            self._connected = True
            self._event_bus.emit(Event.CONNECTION_RESTORED)
            return True
        except Exception as e:
            print(f"Connection failed: {e}")
            self._connected = False
            return False

    def start(self):
        if self._running:
            return

        self._running = True

        self._sitl_thread = threading.Thread(target=self._sitl_loop, daemon=True)
        self._sitl_thread.start()

        self._proxy_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._proxy_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._proxy_socket.bind(("0.0.0.0", self.proxy_port))
        self._proxy_socket.listen(5)

        self._proxy_thread = threading.Thread(target=self._proxy_accept_loop, daemon=True)
        self._proxy_thread.start()

    def stop(self):
        self._running = False

        if self._sitl_conn:
            self._sitl_conn.close()
            self._sitl_conn = None

        if self._proxy_socket:
            self._proxy_socket.close()
            self._proxy_socket = None

        for client in self._clients:
            try:
                client.close()
            except:
                pass
        self._clients.clear()

        self._connected = False

    def _sitl_loop(self):
        while self._running and self._sitl_conn:
            try:
                msg = self._sitl_conn.recv_match(blocking=True, timeout=0.1)
                if msg is None:
                    continue

                if msg.get_type() == "BAD_DATA":
                    continue

                self._telemetry.parse_message(msg)
                self._event_bus.emit(Event.TELEMETRY_UPDATE, self._telemetry.get_state())

                for callback in self._message_callbacks:
                    callback(msg)

                raw_bytes = msg.get_msgbuf()
                self._broadcast_to_clients(raw_bytes)

            except Exception as e:
                if self._running:
                    print(f"SITL loop error: {e}")
                    self._event_bus.emit(Event.CONNECTION_LOST)
                    time.sleep(1)

    def _proxy_accept_loop(self):
        while self._running and self._proxy_socket:
            try:
                self._proxy_socket.settimeout(1.0)
                client, addr = self._proxy_socket.accept()
                print(f"Client connected: {addr}")
                with self._lock:
                    self._clients.append(client)
                threading.Thread(target=self._client_handler, args=(client,), daemon=True).start()
            except socket.timeout:
                continue
            except Exception as e:
                if self._running:
                    print(f"Accept error: {e}")

    def _client_handler(self, client: socket.socket):
        try:
            while self._running:
                data = client.recv(1024)
                if not data:
                    break
                if self._sitl_conn:
                    self._sitl_conn.write(data)
        except:
            pass
        finally:
            with self._lock:
                if client in self._clients:
                    self._clients.remove(client)
            try:
                client.close()
            except:
                pass

    def _broadcast_to_clients(self, data: bytes):
        with self._lock:
            dead_clients = []
            for client in self._clients:
                try:
                    client.sendall(data)
                except:
                    dead_clients.append(client)
            for client in dead_clients:
                self._clients.remove(client)
                try:
                    client.close()
                except:
                    pass

    def send_rc_override(self, channels: dict):
        if not self._sitl_conn:
            return

        # 65535 = don't touch this channel (leave as-is)
        # Only override the channels we explicitly pass in
        ch = [65535] * 8
        for i, val in channels.items():
            if 1 <= i <= 8:
                ch[i - 1] = val

        self._sitl_conn.mav.rc_channels_override_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            ch[0], ch[1], ch[2], ch[3], ch[4], ch[5], ch[6], ch[7]
        )
        self._event_bus.emit(Event.RC_OVERRIDE_SENT, channels)

    def release_rc_override(self):
        """Release only control channels (1-3), leave aux channels untouched"""
        if not self._sitl_conn:
            return
        # 0 = release channel back to RC input
        # 65535 = don't touch (leave aux channels like CH7 ArmDisarm alone)
        self._sitl_conn.mav.rc_channels_override_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            0, 0, 0, 65535, 65535, 65535, 65535, 65535
        )

    def set_mode(self, mode_name: str):
        """Set ArduPilot flight mode (e.g., 'CRUISE', 'MANUAL', 'AUTO')"""
        if not self._sitl_conn:
            logger.warning(f"SET MODE FAILED: No SITL connection, mode={mode_name}")
            return

        # ArduPilot plane mode numbers
        mode_mapping = {
            'MANUAL': 0,
            'CIRCLE': 1,
            'STABILIZE': 2,
            'TRAINING': 3,
            'ACRO': 4,
            'FLY_BY_WIRE_A': 5,
            'FBWA': 5,
            'FLY_BY_WIRE_B': 6,
            'FBWB': 6,
            'CRUISE': 7,
            'AUTOTUNE': 8,
            'AUTO': 10,
            'RTL': 11,
            'LOITER': 12,
            'GUIDED': 15,
        }

        if mode_name not in mode_mapping:
            logger.error(f"SET MODE FAILED: Unknown mode={mode_name}")
            print(f"Unknown mode: {mode_name}")
            return

        mode_id = mode_mapping[mode_name]
        self._sitl_conn.set_mode(mode_id)
        logger.info(f"SET MODE: {mode_name} (id={mode_id})")

    def send_guided_target(self, lat: float, lon: float, alt: float):
        """Send a target position for ArduPlane GUIDED mode.
        Uses MISSION_ITEM_INT with current=2, which is the standard way
        to set a GUIDED target in ArduPlane (like 'fly to here' in Mission Planner).
        """
        if not self._sitl_conn:
            return
        self._sitl_conn.mav.mission_item_int_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            0,                                              # seq
            mavutil.mavlink.MAV_FRAME_GLOBAL_RELATIVE_ALT,  # frame
            mavutil.mavlink.MAV_CMD_NAV_WAYPOINT,           # command
            2,                                              # current = 2 means GUIDED target
            0,                                              # autocontinue
            0, 0, 0, 0,                                     # params 1-4 (unused for NAV_WAYPOINT guided)
            int(lat * 1e7),                                 # lat (degE7)
            int(lon * 1e7),                                 # lon (degE7)
            alt                                             # alt (meters, relative)
        )

    def send_speed(self, airspeed_ms: float):
        """Send MAV_CMD_DO_CHANGE_SPEED — works in GUIDED/AUTO. NOT reliable in LOITER (use set_cruise_airspeed)."""
        if not self._sitl_conn:
            return
        self._sitl_conn.mav.command_long_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            mavutil.mavlink.MAV_CMD_DO_CHANGE_SPEED,
            0,
            0,               # param1: 0=airspeed
            airspeed_ms,     # param2: speed (m/s)
            -1,              # param3: throttle (-1=no change)
            0, 0, 0, 0
        )

    def send_guided_change_altitude(self, altitude_m: float, rate_ms: float = 0):
        """Send MAV_CMD_GUIDED_CHANGE_ALTITUDE (43001) via COMMAND_INT.
        Changes altitude in GUIDED mode WITHOUT resetting navigation path interpolation.
        This is the preferred way to change altitude — it overrides waypoint-based altitude
        with a direct target that TECS tracks independently.
        altitude_m: target altitude (meters, relative to home)
        rate_ms: climb/descent rate (m/s), 0 = maximum rate
        """
        if not self._sitl_conn:
            return
        self._sitl_conn.mav.command_int_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            mavutil.mavlink.MAV_FRAME_GLOBAL_RELATIVE_ALT,  # frame 6
            43001,           # MAV_CMD_GUIDED_CHANGE_ALTITUDE
            0, 0,            # current, autocontinue
            0,               # param1: empty
            0,               # param2: empty
            rate_ms,         # param3: rate (m/s), 0=max
            0,               # param4: empty
            0, 0,            # x, y: not used
            altitude_m       # z: target altitude (meters, relative)
        )
        logger.info(f"GUIDED_CHANGE_ALT: target={altitude_m:.1f}m rate={rate_ms:.1f}m/s")

    def set_cruise_airspeed(self, speed_ms: float):
        """Set cruise airspeed parameter for LOITER/CRUISE modes.
        Sets both TRIM_ARSPD_CM (ArduPlane <=4.4, cm/s) and AIRSPEED_CRUISE (4.5+, m/s)
        so that one of them will work regardless of firmware version.
        """
        self.set_param('TRIM_ARSPD_CM', speed_ms * 100.0)
        self.set_param('AIRSPEED_CRUISE', speed_ms)

    def send_reposition(self, lat: float, lon: float, alt: float):
        """Change target position/altitude in LOITER via SET_POSITION_TARGET_GLOBAL_INT."""
        if not self._sitl_conn:
            return
        # type_mask: use position only, ignore velocity/acceleration/yaw
        type_mask = (
            0x0008 | 0x0010 | 0x0020 |  # ignore vx, vy, vz
            0x0040 | 0x0080 | 0x0100 |  # ignore ax, ay, az
            0x0400 | 0x0800             # ignore yaw, yaw_rate
        )
        self._sitl_conn.mav.set_position_target_global_int_send(
            0,                                              # time_boot_ms
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            mavutil.mavlink.MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
            type_mask,
            int(lat * 1e7),          # lat (degE7)
            int(lon * 1e7),          # lon (degE7)
            alt,                     # alt (meters, relative)
            0, 0, 0,                 # velocity (ignored)
            0, 0, 0,                 # acceleration (ignored)
            0, 0                     # yaw, yaw_rate (ignored)
        )
        logger.info(f"POSITION_TARGET: lat={lat:.6f} lon={lon:.6f} alt={alt:.1f}")

    # L1 controller orbit radius compensation: ArduPlane's L1 navigation
    # tracks outside the commanded radius. This factor reduces the sent radius
    # so the actual orbit matches the requested radius.
    ORBIT_RADIUS_COMPENSATION = 0.85

    def send_loiter_unlim(self, lat: float, lon: float, alt: float, radius: float,
                          ccw: bool = False):
        """Orbit at specified center via MAV_CMD_DO_REPOSITION (COMMAND_INT).
        Enters GUIDED mode and orbits the point with the given radius.
        ArduPlane's GUIDED with radius is functionally identical to LOITER.
        radius: orbit radius in meters (always positive).
        ccw: True for counter-clockwise, False for clockwise.
        """
        if not self._sitl_conn:
            return
        compensated = abs(radius) * self.ORBIT_RADIUS_COMPENSATION
        self._sitl_conn.mav.command_int_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            mavutil.mavlink.MAV_FRAME_GLOBAL_RELATIVE_ALT,  # frame
            mavutil.mavlink.MAV_CMD_DO_REPOSITION,           # command 192
            0, 0,                                            # current, autocontinue
            -1,                                              # param1: ground speed (-1 = no change)
            1,                                               # param2: MAV_DO_REPOSITION_FLAGS_CHANGE_MODE
            compensated,                                     # param3: loiter radius (compensated)
            1 if ccw else 0,                                 # param4: direction (0=CW, 1=CCW)
            int(lat * 1e7),                                  # x: lat (degE7)
            int(lon * 1e7),                                  # y: lon (degE7)
            alt                                              # z: alt (meters, relative)
        )
        direction_str = "CCW" if ccw else "CW"
        logger.info(f"DO_REPOSITION: lat={lat:.6f} lon={lon:.6f} alt={alt:.1f} "
                     f"radius={radius:.0f}(cmd={compensated:.0f}) {direction_str}")

    def send_position_reset(self, lat: float, lon: float, accuracy: float = 5.0):
        """Send custom COMMAND_INT (43210) to force EKF position reset in firmware.
        Uses forcePositionReset() added to AP_NavEKF3.
        accuracy: position accuracy in meters (sets EKF covariance).
        """
        if not self._sitl_conn:
            return
        self._sitl_conn.mav.command_int_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            mavutil.mavlink.MAV_FRAME_GLOBAL_RELATIVE_ALT,  # frame
            43210,           # custom command: forcePositionReset
            0, 0,            # current, autocontinue
            accuracy,        # param1: position accuracy (meters)
            0,               # param2: empty
            0,               # param3: empty
            0,               # param4: empty
            int(lat * 1e7),  # x: lat (degE7)
            int(lon * 1e7),  # y: lon (degE7)
            0                # z: alt (not used)
        )
        logger.info(f"POSITION_RESET: lat={lat:.6f} lon={lon:.6f} acc={accuracy:.1f}")

    def send_wind_override(self, direction_deg: int, speed_ms: int, accuracy: float = 2.0):
        """Send custom COMMAND_INT (43211) to force EKF wind state in firmware.
        direction_deg: wind FROM direction in degrees (meteorological convention).
        speed_ms: wind speed in m/s.
        accuracy: wind accuracy in m/s (sets EKF covariance).
        """
        if not self._sitl_conn:
            return
        import math
        dir_rad = math.radians(direction_deg)
        wind_n = -speed_ms * math.cos(dir_rad)
        wind_e = -speed_ms * math.sin(dir_rad)
        self._sitl_conn.mav.command_int_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            0,               # frame (unused)
            43211,           # custom command: forceWindReset
            0, 0,            # current, autocontinue
            wind_n,          # param1: windN (m/s)
            wind_e,          # param2: windE (m/s)
            accuracy,        # param3: wind accuracy (m/s)
            0,               # param4: empty
            0, 0, 0          # x, y, z: not used
        )
        logger.info(f"WIND_OVERRIDE: dir={direction_deg}° speed={speed_ms}m/s "
                     f"(N={wind_n:.1f} E={wind_e:.1f}) acc={accuracy:.1f}")

    def on_message(self, callback: Callable):
        self._message_callbacks.append(callback)

    def get_telemetry(self) -> TelemetryState:
        return self._telemetry.get_state()

    def is_connected(self) -> bool:
        return self._connected

    def set_param(self, name: str, value: float, param_type: int = None):
        """Set an ArduPilot parameter via MAVLink"""
        if not self._sitl_conn:
            return
        if param_type is None:
            param_type = mavutil.mavlink.MAV_PARAM_TYPE_REAL32
        self._sitl_conn.mav.param_set_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            name.encode('utf-8'),
            value,
            param_type
        )
        logger.info(f"SET PARAM: {name} = {value}")

    def request_data_streams(self, rate: int = 4):
        if not self._sitl_conn:
            return

        self._sitl_conn.mav.request_data_stream_send(
            self._sitl_conn.target_system,
            self._sitl_conn.target_component,
            mavutil.mavlink.MAV_DATA_STREAM_ALL,
            rate, 1
        )
