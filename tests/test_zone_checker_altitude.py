"""Tests for zone_checker altitude profile (climb_enroute) methods."""
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


# Zone at lon 37.6-37.7 with altitude limit 500m
ALT_ZONE = NoFlyZone(
    id='alt1',
    points=[[55.49, 37.6], [55.49, 37.7], [55.51, 37.7], [55.51, 37.6]],
    name='AltZone',
    altitude=500.0,
    avoid_mode='below_altitude',
)

# Zone always active
ALWAYS_ZONE = NoFlyZone(
    id='always1',
    points=[[55.49, 37.6], [55.49, 37.7], [55.51, 37.7], [55.51, 37.6]],
    name='AlwaysZone',
)


class TestSegmentIntersectsObstaclesClimb:
    def test_climb_clears_zone(self):
        """Climb from 100→1000m, zone at 500m near end of path → zone cleared."""
        checker = _make_checker(zones=[ALT_ZONE])
        # Zone centroid at ~lon 37.65, path from 37.0 to 38.0
        # fraction ~0.65, altitude at zone = 100 + 0.65*900 = 685m > 500m
        result = checker.segment_intersects_obstacles_climb(
            55.5, 37.0, 55.5, 38.0, 100.0, 1000.0
        )
        assert result is False, "Aircraft is above zone — should not intersect"

    def test_climb_partial_intersection(self):
        """Climb from 100→400m, zone at 500m → aircraft still below zone."""
        checker = _make_checker(zones=[ALT_ZONE])
        result = checker.segment_intersects_obstacles_climb(
            55.5, 37.0, 55.5, 38.0, 100.0, 400.0
        )
        assert result is True, "Aircraft stays below zone altitude — should intersect"

    def test_always_mode_ignores_climb(self):
        """Mode='always' zones ignore altitude profile entirely."""
        checker = _make_checker(zones=[ALWAYS_ZONE])
        result = checker.segment_intersects_obstacles_climb(
            55.5, 37.0, 55.5, 38.0, 100.0, 10000.0
        )
        assert result is True, "Always-mode zone should intersect regardless of altitude"

    def test_descent_keeps_zone_active(self):
        """Descent from 1000→100m, zone at start of path → zone active."""
        # Zone at lon 37.6-37.7, path lon 37.0→38.0, zone fraction ~0.65
        # altitude at zone = 1000 + 0.65*(-900) = 415m < 500m → active
        checker = _make_checker(zones=[ALT_ZONE])
        result = checker.segment_intersects_obstacles_climb(
            55.5, 37.0, 55.5, 38.0, 1000.0, 100.0
        )
        assert result is True, "Descending below zone altitude — should intersect"

    def test_no_zones_no_intersection(self):
        checker = _make_checker()
        result = checker.segment_intersects_obstacles_climb(
            55.5, 37.0, 55.5, 38.0, 100.0, 1000.0
        )
        assert result is False


class TestGetBufferedObstaclesClimb:
    def test_climb_excludes_high_zone(self):
        """Zone at 500m should be excluded when aircraft climbs to 1000m at that position."""
        checker = _make_checker(zones=[ALT_ZONE])
        result = checker.get_buffered_obstacles_climb(
            100.0, 1000.0,
            start_lat=55.5, start_lon=37.0,
            end_lat=55.5, end_lon=38.0,
        )
        assert len(result) == 0, "Zone should be excluded — aircraft above altitude at zone position"

    def test_climb_includes_low_zone(self):
        """Zone at 500m stays when aircraft only climbs to 300m."""
        checker = _make_checker(zones=[ALT_ZONE])
        result = checker.get_buffered_obstacles_climb(
            100.0, 300.0,
            start_lat=55.5, start_lon=37.0,
            end_lat=55.5, end_lon=38.0,
        )
        assert len(result) == 1, "Zone should be included — aircraft below altitude"

    def test_always_mode_always_included(self):
        """Always-mode zone included regardless of altitude."""
        checker = _make_checker(zones=[ALWAYS_ZONE])
        result = checker.get_buffered_obstacles_climb(
            100.0, 10000.0,
            start_lat=55.5, start_lon=37.0,
            end_lat=55.5, end_lon=38.0,
        )
        assert len(result) == 1, "Always-mode zone should always be included"
