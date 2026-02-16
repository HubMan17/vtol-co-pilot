import pytest
import numpy as np
from src.navigation.calculations import (
    point_in_polygon, circle_to_polygon, segment_intersects_polygon,
    _segments_intersect_2d, is_visible, is_visible_vg, polygon_buffer,
    haversine_distance,
    batch_segments_intersect, batch_point_in_polygon, batch_haversine,
)


# ──── point_in_polygon ────

class TestPointInPolygon:
    SQUARE = [[0, 0], [0, 1], [1, 1], [1, 0]]

    def test_inside(self):
        assert point_in_polygon(0.5, 0.5, self.SQUARE) is True

    def test_outside(self):
        assert point_in_polygon(2.0, 2.0, self.SQUARE) is False

    def test_outside_near(self):
        assert point_in_polygon(-0.1, 0.5, self.SQUARE) is False

    def test_concave_polygon(self):
        # L-shaped polygon
        poly = [[0, 0], [0, 2], [1, 2], [1, 1], [2, 1], [2, 0]]
        assert point_in_polygon(0.5, 0.5, poly) is True  # inside
        assert point_in_polygon(1.5, 1.5, poly) is False  # outside the concave notch

    def test_degenerate_polygon(self):
        assert point_in_polygon(0, 0, [[0, 0], [1, 1]]) is False  # < 3 points


# ──── segments_intersect ────

class TestSegmentsIntersect:
    def test_crossing(self):
        assert _segments_intersect_2d(0, 0, 1, 1, 0, 1, 1, 0) is True

    def test_parallel(self):
        assert _segments_intersect_2d(0, 0, 1, 0, 0, 1, 1, 1) is False

    def test_non_crossing(self):
        assert _segments_intersect_2d(0, 0, 0.4, 0.4, 0.6, 0.6, 1, 1) is False

    def test_t_shape(self):
        # Segment ends exactly at intersection — strict inequality means no intersection
        assert _segments_intersect_2d(0, 0.5, 1, 0.5, 0.5, 0, 0.5, 0.5) is False


# ──── segment_intersects_polygon ────

class TestSegmentIntersectsPolygon:
    SQUARE = [[0, 0], [0, 1], [1, 1], [1, 0]]

    def test_crosses(self):
        assert segment_intersects_polygon(-0.5, 0.5, 1.5, 0.5, self.SQUARE) is True

    def test_avoids(self):
        assert segment_intersects_polygon(-1, -1, -0.5, -0.5, self.SQUARE) is False

    def test_both_endpoints_inside(self):
        assert segment_intersects_polygon(0.2, 0.2, 0.8, 0.8, self.SQUARE) is True

    def test_one_endpoint_inside(self):
        assert segment_intersects_polygon(0.5, 0.5, 2.0, 2.0, self.SQUARE) is True


# ──── is_visible ────

class TestIsVisible:
    SQUARE = [[0, 0], [0, 1], [1, 1], [1, 0]]

    def test_visible_no_obstacles(self):
        assert is_visible(0, 0, 1, 1, []) is True

    def test_blocked_by_obstacle(self):
        assert is_visible(-0.5, 0.5, 1.5, 0.5, [self.SQUARE]) is False

    def test_visible_around_obstacle(self):
        assert is_visible(-1, -1, -0.5, -0.5, [self.SQUARE]) is True


# ──── is_visible_vg ────

class TestIsVisibleVg:
    """Tests for visibility graph variant that handles polygon vertex endpoints."""
    SQUARE = [[0, 0], [0, 1], [1, 1], [1, 0]]

    def test_visible_no_obstacles(self):
        assert is_visible_vg(0, 0, 1, 1, []) is True

    def test_blocked_through_obstacle(self):
        assert is_visible_vg(-0.5, 0.5, 1.5, 0.5, [self.SQUARE]) is False

    def test_visible_around_obstacle(self):
        assert is_visible_vg(-1, -1, -0.5, -0.5, [self.SQUARE]) is True

    def test_polygon_vertex_to_external_point(self):
        """Vertex of polygon should be visible to an external point
        if the line doesn't cross the polygon."""
        # Vertex (0,0) to external point (-1, -1) — doesn't cross
        assert is_visible_vg(0, 0, -1, -1, [self.SQUARE]) is True

    def test_polygon_vertex_to_vertex_along_edge(self):
        """Adjacent vertices should be visible (segment is the edge)."""
        assert is_visible_vg(0, 0, 0, 1, [self.SQUARE]) is True

    def test_polygon_vertex_to_vertex_diagonal_blocked(self):
        """Diagonal through concave polygon interior should be blocked."""
        # L-shaped polygon: diagonal from (0,0) to (1,1) passes through interior
        L_SHAPE = [[0, 0], [0, 2], [1, 2], [1, 1], [2, 1], [2, 0]]
        # Midpoint (0.5, 0.5) is inside the L-shape
        assert is_visible_vg(0, 0, 1, 1, [L_SHAPE]) is False


# ──── polygon_buffer ────

class TestPolygonBuffer:
    def test_expands_polygon(self):
        square = [[55.0, 37.0], [55.0, 37.01], [55.01, 37.01], [55.01, 37.0]]
        buffered = polygon_buffer(square, 100)
        assert len(buffered) == 4
        # Each vertex should be farther from centroid than original
        cx = sum(p[0] for p in square) / 4
        cy = sum(p[1] for p in square) / 4
        for orig, buf in zip(square, buffered):
            orig_dist = haversine_distance(cx, cy, orig[0], orig[1])
            buf_dist = haversine_distance(cx, cy, buf[0], buf[1])
            assert buf_dist > orig_dist

    def test_zero_buffer(self):
        square = [[0, 0], [0, 1], [1, 1], [1, 0]]
        result = polygon_buffer(square, 0)
        assert result == square

    def test_empty_polygon(self):
        assert polygon_buffer([], 100) == []


# ──── circle_to_polygon ────

class TestCircleToPolygon:
    def test_correct_point_count(self):
        poly = circle_to_polygon(55.75, 37.62, 1000, 16)
        assert len(poly) == 16

    def test_radius_approximately_correct(self):
        center_lat, center_lon = 55.75, 37.62
        radius = 500.0
        poly = circle_to_polygon(center_lat, center_lon, radius, 32)
        for pt in poly:
            dist = haversine_distance(center_lat, center_lon, pt[0], pt[1])
            assert abs(dist - radius) < 5.0  # within 5m tolerance

    def test_8_points(self):
        poly = circle_to_polygon(0, 0, 100, 8)
        assert len(poly) == 8


# ──── batch_segments_intersect ────

class TestBatchSegmentsIntersect:
    def test_crossing_pair(self):
        a = np.array([[0, 0, 1, 1]], dtype=np.float64)
        b = np.array([[0, 1, 1, 0]], dtype=np.float64)
        result = batch_segments_intersect(a, b)
        assert result.shape == (1, 1)
        assert result[0, 0] is np.True_

    def test_parallel_no_crossing(self):
        a = np.array([[0, 0, 1, 0]], dtype=np.float64)
        b = np.array([[0, 1, 1, 1]], dtype=np.float64)
        result = batch_segments_intersect(a, b)
        assert result[0, 0] is np.False_

    def test_non_overlapping(self):
        a = np.array([[0, 0, 0.4, 0.4]], dtype=np.float64)
        b = np.array([[0.6, 0.6, 1, 1]], dtype=np.float64)
        result = batch_segments_intersect(a, b)
        assert result[0, 0] is np.False_

    def test_matches_scalar_version(self):
        """Batch result must exactly match scalar _segments_intersect_2d."""
        cases = [
            (0, 0, 1, 1, 0, 1, 1, 0),    # crossing
            (0, 0, 1, 0, 0, 1, 1, 1),    # parallel
            (0, 0, 0.4, 0.4, 0.6, 0.6, 1, 1),  # non-crossing
            (0, 0.5, 1, 0.5, 0.5, 0, 0.5, 0.5),  # T-shape endpoint
        ]
        for ax1, ay1, ax2, ay2, bx1, by1, bx2, by2 in cases:
            scalar = _segments_intersect_2d(ax1, ay1, ax2, ay2, bx1, by1, bx2, by2)
            a = np.array([[ax1, ay1, ax2, ay2]], dtype=np.float64)
            b = np.array([[bx1, by1, bx2, by2]], dtype=np.float64)
            batch = bool(batch_segments_intersect(a, b)[0, 0])
            assert batch == scalar, \
                f"Mismatch for ({ax1},{ay1})-({ax2},{ay2}) vs ({bx1},{by1})-({bx2},{by2}): " \
                f"scalar={scalar}, batch={batch}"

    def test_multiple_pairs_and_edges(self):
        """P=3 segments × E=2 edges → (3,2) result matrix."""
        segs = np.array([
            [0, 0, 1, 1],      # diagonal
            [0, 0, 0.3, 0],    # short horizontal
            [-1, 0.5, 2, 0.5], # long horizontal crossing both
        ], dtype=np.float64)
        edges = np.array([
            [0, 1, 1, 0],     # anti-diagonal
            [0.5, 0, 0.5, 1], # vertical
        ], dtype=np.float64)
        result = batch_segments_intersect(segs, edges)
        assert result.shape == (3, 2)
        assert result[0, 0] is np.True_   # diagonal × anti-diagonal
        assert result[0, 1] is np.True_   # diagonal × vertical
        assert result[1, 0] is np.False_  # short horiz × anti-diagonal
        assert result[1, 1] is np.False_  # short horiz × vertical
        assert result[2, 0] is np.True_   # long horiz × anti-diagonal
        assert result[2, 1] is np.True_   # long horiz × vertical


# ──── batch_point_in_polygon ────

class TestBatchPointInPolygon:
    SQUARE = np.array([[0, 0], [0, 1], [1, 1], [1, 0]], dtype=np.float64)

    def test_inside_and_outside(self):
        pts = np.array([[0.5, 0.5], [2.0, 2.0]], dtype=np.float64)
        result = batch_point_in_polygon(pts, self.SQUARE)
        assert result[0] is np.True_
        assert result[1] is np.False_

    def test_matches_scalar_version(self):
        """Batch must exactly match scalar point_in_polygon."""
        poly_list = [[0, 0], [0, 1], [1, 1], [1, 0]]
        test_points = [
            (0.5, 0.5),   # inside
            (2.0, 2.0),   # outside
            (-0.1, 0.5),  # outside near
            (0.5, 0.0001),  # near edge, inside
            (0.999, 0.999),  # near corner, inside
        ]
        for lat, lon in test_points:
            scalar = point_in_polygon(lat, lon, poly_list)
            pts = np.array([[lat, lon]], dtype=np.float64)
            batch = bool(batch_point_in_polygon(pts, self.SQUARE)[0])
            assert batch == scalar, \
                f"Mismatch at ({lat},{lon}): scalar={scalar}, batch={batch}"

    def test_concave_polygon(self):
        L = np.array([[0, 0], [0, 2], [1, 2], [1, 1], [2, 1], [2, 0]], dtype=np.float64)
        pts = np.array([[0.5, 0.5], [1.5, 1.5]], dtype=np.float64)
        result = batch_point_in_polygon(pts, L)
        assert result[0] is np.True_   # inside L
        assert result[1] is np.False_  # outside notch

    def test_degenerate_polygon(self):
        poly = np.array([[0, 0], [1, 1]], dtype=np.float64)
        pts = np.array([[0, 0]], dtype=np.float64)
        result = batch_point_in_polygon(pts, poly)
        assert result[0] is np.False_

    def test_many_points(self):
        """Stress test with 1000 random points."""
        rng = np.random.default_rng(42)
        pts = rng.uniform(-0.5, 1.5, size=(1000, 2))
        poly_list = [[0, 0], [0, 1], [1, 1], [1, 0]]
        batch_result = batch_point_in_polygon(pts.astype(np.float64), self.SQUARE)
        for i in range(len(pts)):
            scalar = point_in_polygon(pts[i, 0], pts[i, 1], poly_list)
            assert batch_result[i] == scalar, f"Mismatch at point {i}: {pts[i]}"


# ──── batch_haversine ────

class TestBatchHaversine:
    def test_zero_distance(self):
        d = batch_haversine(np.array([55.0]), np.array([37.0]),
                            np.array([55.0]), np.array([37.0]))
        assert d[0] == pytest.approx(0.0)

    def test_one_degree_lon_at_equator(self):
        d = batch_haversine(np.array([0.0]), np.array([0.0]),
                            np.array([0.0]), np.array([1.0]))
        assert d[0] == pytest.approx(111195, rel=0.01)

    def test_matches_scalar(self):
        pairs = [
            (55.0, 37.0, 55.5, 37.5),
            (0, 0, 0, 1),
            (59.9, 30.3, 60.0, 30.5),
        ]
        for lat1, lon1, lat2, lon2 in pairs:
            scalar = haversine_distance(lat1, lon1, lat2, lon2)
            batch = batch_haversine(
                np.array([lat1]), np.array([lon1]),
                np.array([lat2]), np.array([lon2])
            )[0]
            assert batch == pytest.approx(scalar, rel=1e-10), \
                f"Mismatch for ({lat1},{lon1})->({lat2},{lon2})"

    def test_vectorized_multiple(self):
        d = batch_haversine(
            np.array([0, 0, 55.0]),
            np.array([0, 0, 37.0]),
            np.array([0, 1, 55.5]),
            np.array([1, 0, 37.5]),
        )
        assert len(d) == 3
        assert d[0] == pytest.approx(d[1], rel=0.01)  # symmetric at equator
        assert d[2] > 0
