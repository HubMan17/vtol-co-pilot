import time

import pytest
from pathlib import Path

from src.core.config import ZoneAvoidanceConfig
from src.navigation.zone_manager import ZoneManager, NoFlyZone
from src.navigation.zone_checker import ZoneChecker
from src.navigation.path_planner import PathPlanner


def _make_planner(zones=None, nofly_buffer=50.0) -> PathPlanner:
    config = ZoneAvoidanceConfig(
        nofly_mode='always',
        nofly_buffer=nofly_buffer,
        settlement_mode='disabled',
    )
    zm = ZoneManager(Path('__nonexistent_zones__.json'))
    if zones:
        for z in zones:
            zm._zones[z.id] = z
    checker = ZoneChecker(zm, config)
    return PathPlanner(checker)


BLOCKING_ZONE = NoFlyZone(
    id='z1',
    points=[[55.49, 37.3], [55.49, 37.7], [55.51, 37.7], [55.51, 37.3]],
    name='BlockingZone',
)


class TestPlanPath:
    def test_no_obstacles_direct(self):
        planner = _make_planner()
        result = planner.plan_path(55.0, 37.0, 55.0, 38.0, 100)
        assert result == []

    def test_single_obstacle_avoidance(self):
        planner = _make_planner(zones=[BLOCKING_ZONE])
        result = planner.plan_path(55.5, 37.0, 55.5, 38.0, 100)
        assert result is not None
        assert len(result) > 0
        # Intermediate points should not be inside the zone
        for lat, lon in result:
            assert not (55.49 <= lat <= 55.51 and 37.3 <= lon <= 37.7), \
                f"Avoidance point ({lat}, {lon}) is inside the zone"

    def test_clear_path_returns_empty(self):
        planner = _make_planner(zones=[BLOCKING_ZONE])
        # Path that doesn't cross the zone
        result = planner.plan_path(56.0, 37.0, 56.0, 38.0, 100)
        assert result == []

    def test_multiple_obstacles(self):
        zone2 = NoFlyZone(
            id='z2',
            points=[[55.49, 38.3], [55.49, 38.7], [55.51, 38.7], [55.51, 38.3]],
            name='BlockingZone2',
        )
        planner = _make_planner(zones=[BLOCKING_ZONE, zone2])
        result = planner.plan_path(55.5, 37.0, 55.5, 39.0, 100)
        assert result is not None
        assert len(result) >= 2  # should have at least 2 intermediate points

    def test_path_simplification(self):
        planner = _make_planner(zones=[BLOCKING_ZONE])
        result = planner.plan_path(55.5, 37.0, 55.5, 38.0, 100)
        if result:
            # Simplified path should have minimal points
            assert len(result) <= 4  # shouldn't need more than 4 intermediate points

    def test_dense_obstacles_performance(self):
        """Many obstacles with ~80 total vertices — must complete within 500ms."""
        zones = []
        # Grid of small zones along the path
        for i in range(8):
            lon_base = 37.1 + i * 0.1
            zone = NoFlyZone(
                id=f'perf_{i}',
                points=[
                    [55.49, lon_base],
                    [55.49, lon_base + 0.05],
                    [55.51, lon_base + 0.05],
                    [55.51, lon_base],
                ],
                name=f'PerfZone{i}',
            )
            zones.append(zone)

        planner = _make_planner(zones=zones, nofly_buffer=100.0)

        t0 = time.perf_counter()
        result = planner.plan_path(55.5, 37.0, 55.5, 38.0, 100)
        elapsed_ms = (time.perf_counter() - t0) * 1000

        assert result is not None, "Should find a path around obstacles"
        assert len(result) > 0, "Should have intermediate points"
        assert elapsed_ms < 500, f"Path planning took {elapsed_ms:.0f}ms, expected <500ms"

    def test_avoidance_points_not_inside_obstacles(self):
        """All intermediate avoidance points must be outside buffered obstacles."""
        planner = _make_planner(zones=[BLOCKING_ZONE], nofly_buffer=100.0)
        result = planner.plan_path(55.5, 37.0, 55.5, 38.0, 100)
        assert result is not None

        from src.navigation.calculations import point_in_polygon, polygon_buffer
        buffered = polygon_buffer(BLOCKING_ZONE.points, 100.0)
        for lat, lon in result:
            assert not point_in_polygon(lat, lon, buffered), \
                f"Point ({lat:.6f}, {lon:.6f}) is inside buffered obstacle"

    def test_prune_keeps_direct_blockers(self, monkeypatch):
        """Pruning must keep at least one obstacle that blocks direct segment."""
        import src.navigation.path_planner as pp
        from src.navigation.calculations import segment_intersects_polygon

        monkeypatch.setattr(pp, "_MAX_OBSTACLE_VERTICES", 20)
        planner = _make_planner()

        # 10 small optional obstacles near midpoint, but away from the direct line.
        optional = []
        for i in range(10):
            lat = 55.60 + i * 0.001
            optional.append([
                [lat, 37.20],
                [lat, 37.205],
                [lat + 0.001, 37.205],
                [lat + 0.001, 37.20],
            ])

        # One blocking obstacle crossing direct segment y=55.5 from lon 37.0 to 38.0.
        blocker = [[55.49, 37.45], [55.49, 37.55], [55.51, 37.55], [55.51, 37.45]]
        obstacles = optional + [blocker]

        pruned = planner._prune_obstacles(obstacles, 55.5, 37.0, 55.5, 38.0)
        assert any(
            segment_intersects_polygon(55.5, 37.0, 55.5, 38.0, poly) for poly in pruned
        ), "Direct-blocking obstacle was dropped by pruning"


class TestCorridorFilter:
    """Tests for _filter_by_corridor — reduce obstacles to those near direct path."""

    def test_corridor_filter_reduces_obstacles(self):
        """Obstacles far from direct path should be filtered out."""
        planner = _make_planner()

        # Direct path goes east at lat=55.5
        # 3 obstacles near the path (within 5km corridor)
        near = [
            [[55.50, 37.3], [55.50, 37.35], [55.51, 37.35], [55.51, 37.3]],
            [[55.49, 37.5], [55.49, 37.55], [55.50, 37.55], [55.50, 37.5]],
            [[55.50, 37.7], [55.50, 37.75], [55.51, 37.75], [55.51, 37.7]],
        ]
        # 7 obstacles far from the path (>10km away)
        far = []
        for i in range(7):
            lat = 55.7 + i * 0.05  # at least 20km north of path
            far.append([
                [lat, 37.3], [lat, 37.35], [lat + 0.01, 37.35], [lat + 0.01, 37.3]
            ])

        all_obstacles = near + far
        filtered = planner._filter_by_corridor(all_obstacles, 55.5, 37.0, 55.5, 38.0)
        assert len(filtered) == 3, f"Expected 3 near obstacles, got {len(filtered)}"

    def test_corridor_filter_keeps_blocking(self):
        """Obstacles that block the direct path must survive corridor filter."""
        planner = _make_planner()
        blocker = [[55.49, 37.45], [55.49, 37.55], [55.51, 37.55], [55.51, 37.45]]
        result = planner._filter_by_corridor([blocker], 55.5, 37.0, 55.5, 38.0)
        assert len(result) == 1

    def test_corridor_filter_empty_input(self):
        planner = _make_planner()
        result = planner._filter_by_corridor([], 55.5, 37.0, 55.5, 38.0)
        assert result == []


class TestAltitudeProfile:
    """Tests for plan_path with end_altitude (climb_enroute)."""

    def test_altitude_profile_skips_high_zones(self):
        """Zone at 500m altitude, climb from 100→1000m: zone should be skipped
        because aircraft is above zone altitude for most of the path."""
        zone = NoFlyZone(
            id='alt_zone',
            points=[[55.49, 37.6], [55.49, 37.7], [55.51, 37.7], [55.51, 37.6]],
            name='AltZone500',
            altitude=500.0,
            avoid_mode='below_altitude',
        )
        planner = _make_planner(zones=[zone])
        # Zone centroid is at lon ~37.65, about 70% along a lon 37.0→38.0 path
        # At 70%, altitude = 100 + 0.7*900 = 730m > 500m → zone should be inactive
        result = planner.plan_path(55.5, 37.0, 55.5, 38.0, 100.0, end_altitude=1000.0)
        assert result == [], "Path should be clear — aircraft is above zone at that point"

    def test_altitude_profile_keeps_low_zones(self):
        """Zone at 500m altitude, climb from 100→300m: zone remains active
        because aircraft never reaches zone altitude."""
        zone = NoFlyZone(
            id='alt_zone',
            points=[[55.49, 37.4], [55.49, 37.6], [55.51, 37.6], [55.51, 37.4]],
            name='AltZone500',
            altitude=500.0,
            avoid_mode='below_altitude',
        )
        planner = _make_planner(zones=[zone])
        result = planner.plan_path(55.5, 37.0, 55.5, 38.0, 100.0, end_altitude=300.0)
        assert result is not None
        assert len(result) > 0, "Zone should block path — aircraft stays below 500m"

    def test_always_mode_ignores_altitude_profile(self):
        """Zone with mode='always' blocks path regardless of altitude profile."""
        zone = NoFlyZone(
            id='always_zone',
            points=[[55.49, 37.4], [55.49, 37.6], [55.51, 37.6], [55.51, 37.4]],
            name='AlwaysZone',
        )
        planner = _make_planner(zones=[zone])
        result = planner.plan_path(55.5, 37.0, 55.5, 38.0, 100.0, end_altitude=5000.0)
        assert result is not None
        assert len(result) > 0, "Always-mode zone should block even at high altitude"

    def test_long_distance_path(self):
        """25 km path through several zones — must complete in <1 second."""
        zones = []
        # 5 zones along a 25km path
        for i in range(5):
            lon_base = 37.05 + i * 0.06
            zones.append(NoFlyZone(
                id=f'long_{i}',
                points=[
                    [55.49, lon_base],
                    [55.49, lon_base + 0.02],
                    [55.51, lon_base + 0.02],
                    [55.51, lon_base],
                ],
                name=f'LongZone{i}',
            ))

        planner = _make_planner(zones=zones, nofly_buffer=100.0)
        t0 = time.perf_counter()
        result = planner.plan_path(55.5, 37.0, 55.5, 37.35, 100)
        elapsed_ms = (time.perf_counter() - t0) * 1000

        assert result is not None, "Should find a path"
        assert elapsed_ms < 1000, f"Took {elapsed_ms:.0f}ms, expected <1000ms"
