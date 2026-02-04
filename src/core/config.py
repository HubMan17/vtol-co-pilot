import json
from pathlib import Path
from dataclasses import dataclass, field
from typing import Dict, Any


@dataclass
class MAVLinkConfig:
    sitl_host: str = "127.0.0.1"
    sitl_port: int = 5762
    proxy_port: int = 14550
    system_id: int = 255
    component_id: int = 0


@dataclass
class AutopilotConfig:
    heading_pid: Dict[str, float] = field(default_factory=lambda: {"p": 0.7, "i": 0.08, "d": 0.15})
    altitude_pid: Dict[str, float] = field(default_factory=lambda: {"p": 0.4, "i": 0.02, "d": 0.8})
    speed_pid: Dict[str, float] = field(default_factory=lambda: {"p": 50.0, "i": 10.0, "d": 5.0})
    bank_limit: float = 25.0
    pitch_limit_up: float = 12.0
    pitch_limit_down: float = 15.0
    target_airspeed: float = 20.0
    stick_threshold: int = 50
    timeout_ms: int = 3000


@dataclass
class NavigationConfig:
    waypoint_radius: float = 50.0
    drift_coefficient: float = 1.0


@dataclass
class GUIConfig:
    map_center: tuple = (59.939, 30.315)
    map_zoom: int = 14


@dataclass
class AppConfig:
    mavlink: MAVLinkConfig = field(default_factory=MAVLinkConfig)
    autopilot: AutopilotConfig = field(default_factory=AutopilotConfig)
    navigation: NavigationConfig = field(default_factory=NavigationConfig)
    gui: GUIConfig = field(default_factory=GUIConfig)


def load_config(path: Path = None) -> AppConfig:
    if path is None:
        path = Path(__file__).parent.parent.parent / "config" / "settings.json"

    if not path.exists():
        return AppConfig()

    with open(path, "r") as f:
        data = json.load(f)

    config = AppConfig()

    if "mavlink" in data:
        config.mavlink = MAVLinkConfig(**data["mavlink"])
    if "autopilot" in data:
        ap_data = data["autopilot"].copy()
        if "pitch_limit" in ap_data:
            old_limit = ap_data.pop("pitch_limit")
            ap_data.setdefault("pitch_limit_up", old_limit)
            ap_data.setdefault("pitch_limit_down", old_limit)
        config.autopilot = AutopilotConfig(**ap_data)
    if "navigation" in data:
        config.navigation = NavigationConfig(**data["navigation"])
    if "gui" in data:
        config.gui = GUIConfig(**data["gui"])

    return config


def save_config(config: AppConfig, path: Path = None):
    if path is None:
        path = Path(__file__).parent.parent.parent / "config" / "settings.json"

    path.parent.mkdir(parents=True, exist_ok=True)

    data = {
        "mavlink": {
            "sitl_host": config.mavlink.sitl_host,
            "sitl_port": config.mavlink.sitl_port,
            "proxy_port": config.mavlink.proxy_port,
            "system_id": config.mavlink.system_id,
            "component_id": config.mavlink.component_id,
        },
        "autopilot": {
            "heading_pid": config.autopilot.heading_pid,
            "altitude_pid": config.autopilot.altitude_pid,
            "speed_pid": config.autopilot.speed_pid,
            "bank_limit": config.autopilot.bank_limit,
            "pitch_limit_up": config.autopilot.pitch_limit_up,
            "pitch_limit_down": config.autopilot.pitch_limit_down,
            "target_airspeed": config.autopilot.target_airspeed,
            "stick_threshold": config.autopilot.stick_threshold,
            "timeout_ms": config.autopilot.timeout_ms,
        },
        "navigation": {
            "waypoint_radius": config.navigation.waypoint_radius,
            "drift_coefficient": config.navigation.drift_coefficient,
        },
        "gui": {
            "map_center": list(config.gui.map_center),
            "map_zoom": config.gui.map_zoom,
        }
    }

    with open(path, "w") as f:
        json.dump(data, f, indent=2)
