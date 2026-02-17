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
