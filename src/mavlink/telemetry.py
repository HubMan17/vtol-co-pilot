from dataclasses import dataclass, field
from typing import Optional, Tuple
import time
import math


@dataclass
class LatLon:
    lat: float
    lon: float

    def to_tuple(self) -> Tuple[float, float]:
        return (self.lat, self.lon)


@dataclass
class TelemetryState:
    timestamp: float = 0.0
    airspeed: float = 0.0
    groundspeed: float = 0.0
    heading: float = 0.0
    altitude: float = 0.0
    altitude_agl: float = 0.0
    climb_rate: float = 0.0
    roll: float = 0.0
    pitch: float = 0.0
    yaw: float = 0.0
    wind_speed: float = 0.0
    wind_direction: float = 0.0
    position: Optional[LatLon] = None
    gps_fix: int = 0
    satellites: int = 0
    battery_voltage: float = 0.0
    battery_current: float = 0.0
    rc_channels: list = field(default_factory=lambda: [0] * 8)
    armed: bool = False
    mode: str = ""


class TelemetryParser:
    def __init__(self):
        self.state = TelemetryState()
        self._last_update = 0.0

    def parse_message(self, msg) -> bool:
        msg_type = msg.get_type()
        updated = False

        if msg_type == "ATTITUDE":
            self.state.roll = msg.roll
            self.state.pitch = msg.pitch
            self.state.yaw = msg.yaw
            updated = True

        elif msg_type == "VFR_HUD":
            self.state.airspeed = msg.airspeed
            self.state.groundspeed = msg.groundspeed
            self.state.heading = msg.heading
            self.state.altitude = msg.alt
            self.state.climb_rate = msg.climb
            updated = True

        elif msg_type == "GLOBAL_POSITION_INT":
            self.state.position = LatLon(
                lat=msg.lat / 1e7,
                lon=msg.lon / 1e7
            )
            self.state.altitude_agl = msg.relative_alt / 1000.0
            updated = True

        elif msg_type == "WIND":
            self.state.wind_direction = msg.direction
            self.state.wind_speed = msg.speed
            updated = True

        elif msg_type == "RC_CHANNELS":
            self.state.rc_channels = [
                msg.chan1_raw, msg.chan2_raw, msg.chan3_raw, msg.chan4_raw,
                msg.chan5_raw, msg.chan6_raw, msg.chan7_raw, msg.chan8_raw
            ]
            updated = True

        elif msg_type == "SYS_STATUS":
            self.state.battery_voltage = msg.voltage_battery / 1000.0
            self.state.battery_current = msg.current_battery / 100.0
            updated = True

        elif msg_type == "GPS_RAW_INT":
            self.state.gps_fix = msg.fix_type
            self.state.satellites = msg.satellites_visible
            updated = True

        elif msg_type == "HEARTBEAT":
            self.state.armed = (msg.base_mode & 128) != 0
            mode_mapping = {
                0: "MANUAL", 1: "CIRCLE", 2: "STABILIZE", 3: "TRAINING",
                4: "ACRO", 5: "FBWA", 6: "FBWB", 7: "CRUISE",
                8: "AUTOTUNE", 10: "AUTO", 11: "RTL", 12: "LOITER",
                14: "LAND", 15: "GUIDED", 17: "QSTABILIZE", 18: "QHOVER",
                19: "QLOITER", 20: "QLAND", 21: "QRTL"
            }
            self.state.mode = mode_mapping.get(msg.custom_mode, f"MODE_{msg.custom_mode}")
            updated = True

        if updated:
            self.state.timestamp = time.time()
            self._last_update = self.state.timestamp

        return updated

    def get_state(self) -> TelemetryState:
        return self.state

    def is_stale(self, timeout: float = 3.0) -> bool:
        return (time.time() - self._last_update) > timeout
