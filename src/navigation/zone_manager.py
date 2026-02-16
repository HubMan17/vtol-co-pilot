import json
import logging
import uuid
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional

logger = logging.getLogger(__name__)

_DEFAULT_ZONES_PATH = Path(__file__).parent.parent.parent / 'data' / 'zones.json'


@dataclass
class NoFlyZone:
    id: str
    points: List[List[float]]  # [[lat, lon], ...]
    name: str = ""
    description: str = ""
    altitude: Optional[float] = None  # max altitude in meters, None = unlimited
    color: str = "#1E293B"
    created: str = ""
    modified: str = ""
    # Per-zone avoidance overrides (None = use global config defaults)
    avoid_mode: Optional[str] = None   # "disabled" | "always" | "below_altitude"
    buffer: Optional[float] = None     # meters distance from boundary

    def __post_init__(self):
        if not self.created:
            self.created = datetime.now().isoformat()
        if not self.modified:
            self.modified = self.created


class ZoneManager:
    def __init__(self, path: Optional[Path] = None):
        self._path = path or _DEFAULT_ZONES_PATH
        self._zones: Dict[str, NoFlyZone] = {}
        self._load()

    def _load(self):
        if not self._path.exists():
            return
        try:
            with open(self._path, 'r', encoding='utf-8') as f:
                data = json.load(f)
            for z in data.get('zones', []):
                zone = NoFlyZone(
                    id=z['id'],
                    points=z['points'],
                    name=z.get('name', ''),
                    description=z.get('description', ''),
                    altitude=z.get('altitude'),
                    color=z.get('color', '#EF4444'),
                    created=z.get('created', ''),
                    modified=z.get('modified', ''),
                    avoid_mode=z.get('avoid_mode'),
                    buffer=z.get('buffer'),
                )
                self._zones[zone.id] = zone
        except Exception as e:
            logger.error("Failed to load zones from %s: %s", self._path, e)

    def _save(self):
        try:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            data = {
                'zones': [
                    {
                        'id': z.id,
                        'points': z.points,
                        'name': z.name,
                        'description': z.description,
                        'altitude': z.altitude,
                        'color': z.color,
                        'created': z.created,
                        'modified': z.modified,
                        'avoid_mode': z.avoid_mode,
                        'buffer': z.buffer,
                    }
                    for z in self._zones.values()
                ]
            }
            with open(self._path, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2, ensure_ascii=False)
        except Exception as e:
            logger.error("Failed to save zones to %s: %s", self._path, e)

    def add_zone(self, points: List[List[float]], name: str = "",
                 description: str = "", altitude: Optional[float] = None) -> NoFlyZone:
        zone = NoFlyZone(
            id=str(uuid.uuid4()),
            points=points,
            name=name,
            description=description,
            altitude=altitude,
        )
        self._zones[zone.id] = zone
        self._save()
        return zone

    def remove_zone(self, zone_id: str) -> bool:
        if zone_id not in self._zones:
            return False
        del self._zones[zone_id]
        self._save()
        return True

    def update_zone(self, zone_id: str, **kwargs) -> Optional[NoFlyZone]:
        zone = self._zones.get(zone_id)
        if not zone:
            return None
        for key in ('name', 'description', 'altitude', 'color', 'avoid_mode', 'buffer'):
            if key in kwargs:
                setattr(zone, key, kwargs[key])
        zone.modified = datetime.now().isoformat()
        self._save()
        return zone

    def update_zone_points(self, zone_id: str, points: List[List[float]]) -> Optional[NoFlyZone]:
        zone = self._zones.get(zone_id)
        if not zone:
            return None
        zone.points = points
        zone.modified = datetime.now().isoformat()
        self._save()
        return zone

    def get_zone(self, zone_id: str) -> Optional[NoFlyZone]:
        return self._zones.get(zone_id)

    def get_all_zones(self) -> List[NoFlyZone]:
        return list(self._zones.values())
