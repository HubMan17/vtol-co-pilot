import pytest
from pathlib import Path

from src.core.config import ZoneAvoidanceConfig
from src.navigation.zone_manager import ZoneManager, NoFlyZone
from src.navigation.zone_checker import ZoneChecker


def _make_checker(nofly_mode='always', nofly_buffer=0, settlement_mode='disabled',
                  zones=None) -> ZoneChecker:
    config = ZoneAvoidanceConfig(
        nofly_mode=nofly_mode,
        nofly_buffer=nofly_buffer,
        settlement_mode=settlement_mode,
        settlement_min_altitude=200.0,
        settlement_buffer=0,
    )
    zm = ZoneManager(Path('__nonexistent_zones__.json'))
    if zones:
        for z in zones:
            zm._zones[z.id] = z
    return ZoneChecker(zm, config)


ZONE_SQUARE = NoFlyZone(
    id='z1',
    points=[[55.0, 37.0], [55.0, 38.0], [56.0, 38.0], [56.0, 37.0]],
    name='TestZone',
    altitude=300.0,
)


class TestIsPointRestricted:
    def test_point_inside_nofly_zone(self):
        checker = _make_checker(zones=[ZONE_SQUARE])
        restricted, reason = checker.is_point_restricted(55.5, 37.5, 100)
        assert restricted is True
        assert 'TestZone' in reason

    def test_point_outside_nofly_zone(self):
        checker = _make_checker(zones=[ZONE_SQUARE])
        restricted, _ = checker.is_point_restricted(60.0, 40.0, 100)
        assert restricted is False

    def test_nofly_disabled(self):
        checker = _make_checker(nofly_mode='disabled', zones=[ZONE_SQUARE])
        restricted, _ = checker.is_point_restricted(55.5, 37.5, 100)
        assert restricted is False

    def test_below_altitude_restricted(self):
        zone = NoFlyZone(id='z2', points=ZONE_SQUARE.points, name='AltZone',
                         altitude=200.0, avoid_mode='below_altitude')
        checker = _make_checker(zones=[zone])
        # Below zone altitude → restricted
        restricted, _ = checker.is_point_restricted(55.5, 37.5, 100)
        assert restricted is True

    def test_above_altitude_allowed(self):
        zone = NoFlyZone(id='z2', points=ZONE_SQUARE.points, name='AltZone',
                         altitude=200.0, avoid_mode='below_altitude')
        checker = _make_checker(zones=[zone])
        # Above zone altitude → allowed
        restricted, _ = checker.is_point_restricted(55.5, 37.5, 250)
        assert restricted is False

    def test_per_zone_override_disabled(self):
        zone = NoFlyZone(id='z3', points=ZONE_SQUARE.points, name='OverrideZone',
                         avoid_mode='disabled')
        checker = _make_checker(nofly_mode='always', zones=[zone])
        # Global says always, but per-zone says disabled
        restricted, _ = checker.is_point_restricted(55.5, 37.5, 100)
        assert restricted is False

    def test_per_zone_buffer(self):
        zone = NoFlyZone(id='z4',
                         points=[[55.0, 37.0], [55.0, 37.01], [55.01, 37.01], [55.01, 37.0]],
                         name='SmallZone', buffer=2000.0)
        checker = _make_checker(nofly_buffer=0, zones=[zone])
        # Point outside zone but within 2km buffer (zone is ~1km, buffer adds 2km)
        restricted, _ = checker.is_point_restricted(55.005, 37.02, 100)
        assert restricted is True


class TestSegmentIntersectsObstacles:
    def test_segment_crosses_zone(self):
        checker = _make_checker(zones=[ZONE_SQUARE])
        assert checker.segment_intersects_obstacles(54.0, 37.5, 57.0, 37.5, 100) is True

    def test_segment_avoids_zone(self):
        checker = _make_checker(zones=[ZONE_SQUARE])
        assert checker.segment_intersects_obstacles(60.0, 40.0, 61.0, 41.0, 100) is False

    def test_no_zones(self):
        checker = _make_checker()
        assert checker.segment_intersects_obstacles(0, 0, 1, 1, 100) is False


class TestGetActiveObstacles:
    def test_always_mode_returns_zone(self):
        checker = _make_checker(zones=[ZONE_SQUARE])
        obstacles = checker.get_active_obstacles(100)
        assert len(obstacles) == 1
        assert obstacles[0].source == 'nofly'

    def test_disabled_returns_empty(self):
        checker = _make_checker(nofly_mode='disabled', zones=[ZONE_SQUARE])
        obstacles = checker.get_active_obstacles(100)
        assert len(obstacles) == 0

    def test_altitude_filter(self):
        zone = NoFlyZone(id='z5', points=ZONE_SQUARE.points, altitude=200.0,
                         avoid_mode='below_altitude')
        checker = _make_checker(zones=[zone])
        # Below altitude → zone is active
        assert len(checker.get_active_obstacles(100)) == 1
        # Above altitude → zone is not active
        assert len(checker.get_active_obstacles(300)) == 0
