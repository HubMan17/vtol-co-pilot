"""Tests for SettlementLoader._process_elements and _FetchWorker._distribute."""

from src.gui.map_widget import SettlementLoader, _FetchWorker, _DP_TOLERANCE


class TestProcessElements:
    """Settlement boundary processing: ways, relations, nodes, adaptive DP."""

    def test_small_way_kept_as_polygon(self):
        """Small way polygon → kept as polygon with default DP tolerance."""
        elements = [{
            'type': 'way',
            'tags': {'place': 'village'},
            'geometry': [
                {'lat': 59.90, 'lon': 30.30},
                {'lat': 59.90, 'lon': 30.35},
                {'lat': 59.95, 'lon': 30.35},
                {'lat': 59.95, 'lon': 30.30},
            ]
        }]
        result = SettlementLoader._process_elements(elements)
        assert len(result) == 1
        assert result[0]['F'] == 'p'
        assert result[0]['t'] == 'village'
        assert len(result[0]['c']) >= 3

    def test_large_way_kept_as_polygon_not_circle(self):
        """Large way polygon (span >1°) → kept as polygon, not converted to circle."""
        elements = [{
            'type': 'way',
            'tags': {'place': 'city'},
            'geometry': [
                {'lat': -35.0, 'lon': 148.0},
                {'lat': -35.0, 'lon': 149.0},
                {'lat': -34.0, 'lon': 149.0},
                {'lat': -34.0, 'lon': 148.0},
            ]
        }]
        result = SettlementLoader._process_elements(elements)
        assert len(result) == 1
        assert result[0]['F'] == 'p'  # polygon, NOT circle node
        assert result[0]['t'] == 'city'

    def test_large_relation_kept_as_polygon(self):
        """Large relation → kept as polygon with adaptive DP."""
        elements = [{
            'type': 'relation',
            'tags': {'place': 'town'},
            'members': [{
                'type': 'way',
                'role': 'outer',
                'geometry': [
                    {'lat': 50.0, 'lon': 30.0},
                    {'lat': 50.0, 'lon': 30.5},
                    {'lat': 50.3, 'lon': 30.5},
                    {'lat': 50.3, 'lon': 30.0},
                    {'lat': 50.0, 'lon': 30.0},
                ]
            }]
        }]
        result = SettlementLoader._process_elements(elements)
        assert len(result) == 1
        assert result[0]['F'] == 'p'
        assert result[0]['t'] == 'town'

    def test_node_kept_as_node(self):
        """Standalone node without polygon coverage → kept."""
        elements = [{
            'type': 'node',
            'tags': {'place': 'hamlet'},
            'lat': 55.0,
            'lon': 37.0,
        }]
        result = SettlementLoader._process_elements(elements)
        assert len(result) == 1
        assert result[0]['F'] == 'n'
        assert result[0]['t'] == 'hamlet'

    def test_node_filtered_when_covered_by_polygon(self):
        """Node inside polygon bbox → filtered out (no duplicates)."""
        elements = [
            {
                'type': 'way',
                'tags': {'place': 'village'},
                'geometry': [
                    {'lat': 59.90, 'lon': 30.30},
                    {'lat': 59.90, 'lon': 30.35},
                    {'lat': 59.95, 'lon': 30.35},
                    {'lat': 59.95, 'lon': 30.30},
                ]
            },
            {
                'type': 'node',
                'tags': {'place': 'village'},
                'lat': 59.92,
                'lon': 30.32,
            }
        ]
        result = SettlementLoader._process_elements(elements)
        assert len(result) == 1
        assert result[0]['F'] == 'p'

    def test_node_not_filtered_when_outside_polygon(self):
        """Node outside polygon bbox → kept."""
        elements = [
            {
                'type': 'way',
                'tags': {'place': 'village'},
                'geometry': [
                    {'lat': 59.90, 'lon': 30.30},
                    {'lat': 59.90, 'lon': 30.35},
                    {'lat': 59.95, 'lon': 30.35},
                    {'lat': 59.95, 'lon': 30.30},
                ]
            },
            {
                'type': 'node',
                'tags': {'place': 'hamlet'},
                'lat': 60.5,
                'lon': 31.0,
            }
        ]
        result = SettlementLoader._process_elements(elements)
        assert len(result) == 2

    def test_way_with_less_than_3_points_skipped(self):
        """Way with < 3 points → skipped."""
        elements = [{
            'type': 'way',
            'tags': {'place': 'village'},
            'geometry': [
                {'lat': 59.90, 'lon': 30.30},
                {'lat': 59.95, 'lon': 30.35},
            ]
        }]
        result = SettlementLoader._process_elements(elements)
        assert len(result) == 0

    def test_element_without_place_tag_skipped(self):
        """Element without place tag → skipped."""
        elements = [{
            'type': 'way',
            'tags': {'name': 'Some road'},
            'geometry': [
                {'lat': 59.90, 'lon': 30.30},
                {'lat': 59.90, 'lon': 30.35},
                {'lat': 59.95, 'lon': 30.35},
            ]
        }]
        result = SettlementLoader._process_elements(elements)
        assert len(result) == 0

    def test_adaptive_dp_more_aggressive_for_larger_spans(self):
        """Larger polygons get more aggressive simplification."""
        # Small polygon (span < 0.1) — uses default tolerance
        small_coords = [
            {'lat': 50.0, 'lon': 30.0},
            {'lat': 50.0, 'lon': 30.05},
            {'lat': 50.01, 'lon': 30.03},
            {'lat': 50.02, 'lon': 30.05},
            {'lat': 50.03, 'lon': 30.04},
            {'lat': 50.04, 'lon': 30.05},
            {'lat': 50.05, 'lon': 30.05},
            {'lat': 50.05, 'lon': 30.0},
            {'lat': 50.0, 'lon': 30.0},
        ]
        # Large polygon (span > 0.5) — uses tol=0.005
        large_coords = [
            {'lat': 50.0, 'lon': 30.0},
            {'lat': 50.0, 'lon': 30.6},
            {'lat': 50.1, 'lon': 30.35},
            {'lat': 50.2, 'lon': 30.6},
            {'lat': 50.3, 'lon': 30.45},
            {'lat': 50.4, 'lon': 30.6},
            {'lat': 50.5, 'lon': 30.55},
            {'lat': 50.6, 'lon': 30.6},
            {'lat': 50.6, 'lon': 30.0},
            {'lat': 50.0, 'lon': 30.0},
        ]
        elements = [
            {'type': 'way', 'tags': {'place': 'village'}, 'geometry': small_coords},
            {'type': 'way', 'tags': {'place': 'city'}, 'geometry': large_coords},
        ]
        result = SettlementLoader._process_elements(elements)
        assert len(result) == 2
        small_poly = [r for r in result if r['t'] == 'village'][0]
        large_poly = [r for r in result if r['t'] == 'city'][0]
        # Both are polygons
        assert small_poly['F'] == 'p'
        assert large_poly['F'] == 'p'
        # Large should have fewer or equal points (more aggressive simplification)
        assert len(large_poly['c']) <= len(large_coords)


class TestDistribute:
    """_FetchWorker._distribute — feature to tile assignment."""

    def test_node_assigned_to_correct_tile(self):
        """Node at (55.05, 37.05) → tile (55.0, 37.0, 55.1, 37.1)."""
        features = [{'F': 'n', 'lat': 55.05, 'lon': 37.05, 't': 'village'}]
        tiles = [
            (55.0, 37.0, 55.1, 37.1),
            (55.1, 37.0, 55.2, 37.1),
        ]
        result = _FetchWorker._distribute(features, tiles)
        assert len(result['55.0,37.0']) == 1
        assert len(result['55.1,37.0']) == 0

    def test_polygon_assigned_to_overlapping_tiles(self):
        """Polygon spanning two tiles → assigned to both."""
        features = [{
            'F': 'p',
            'c': [[55.05, 37.05], [55.05, 37.15], [55.15, 37.15], [55.15, 37.05]],
            't': 'town',
        }]
        tiles = [
            (55.0, 37.0, 55.1, 37.1),
            (55.1, 37.1, 55.2, 37.2),
        ]
        result = _FetchWorker._distribute(features, tiles)
        assert len(result['55.0,37.0']) == 1
        assert len(result['55.1,37.1']) == 1

    def test_feature_outside_all_tiles_not_assigned(self):
        """Feature outside all tiles → not assigned anywhere."""
        features = [{'F': 'n', 'lat': 60.0, 'lon': 40.0, 't': 'hamlet'}]
        tiles = [(55.0, 37.0, 55.1, 37.1)]
        result = _FetchWorker._distribute(features, tiles)
        assert len(result['55.0,37.0']) == 0

    def test_empty_features(self):
        """Empty features list → all tiles empty."""
        tiles = [(55.0, 37.0, 55.1, 37.1)]
        result = _FetchWorker._distribute([], tiles)
        assert len(result['55.0,37.0']) == 0
