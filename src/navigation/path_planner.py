import heapq
import logging
import time
from typing import Dict, List, Optional, Set, Tuple

import numpy as np

from src.navigation.zone_checker import ZoneChecker
from src.navigation.calculations import (
    haversine_distance, polygon_buffer, segment_intersects_polygon,
    is_visible, is_visible_vg,
    meters_to_lat_offset, meters_to_lon_offset,
    batch_segments_intersect, batch_point_in_polygon, batch_haversine,
)

logger = logging.getLogger(__name__)

_MAX_OBSTACLE_VERTICES = 100


class PathPlanner:
    def __init__(self, zone_checker: ZoneChecker):
        self._zone_checker = zone_checker

    def plan_path(self, start_lat: float, start_lon: float,
                  end_lat: float, end_lon: float,
                  altitude: float) -> Optional[List[Tuple[float, float]]]:
        """Compute avoidance path around obstacles.
        Returns:
            [] — no avoidance needed (direct path is clear)
            [(lat, lon), ...] — intermediate waypoints (excluding start/end)
            None — no path found
        """
        # Build bbox around start-end with margin for buffer zones
        margin = 5000.0  # 5 km margin
        min_lat = min(start_lat, end_lat) - meters_to_lat_offset(margin)
        max_lat = max(start_lat, end_lat) + meters_to_lat_offset(margin)
        mid_lat = (start_lat + end_lat) / 2
        min_lon = min(start_lon, end_lon) - meters_to_lon_offset(margin, mid_lat)
        max_lon = max(start_lon, end_lon) + meters_to_lon_offset(margin, mid_lat)
        bbox = (min_lat, max_lat, min_lon, max_lon)

        buffered = self._zone_checker.get_buffered_obstacles(altitude, bbox=bbox)
        if not buffered:
            return []

        # Check if direct path is clear
        direct_clear = True
        for poly in buffered:
            if segment_intersects_polygon(start_lat, start_lon, end_lat, end_lon, poly):
                direct_clear = False
                break
        if direct_clear:
            return []

        # Cap total vertices to prevent visibility graph explosion
        total_verts = sum(len(p) for p in buffered)
        if total_verts > _MAX_OBSTACLE_VERTICES:
            buffered = self._prune_obstacles(buffered, start_lat, start_lon,
                                             end_lat, end_lon)

        # Build visibility graph
        t_start = time.perf_counter()
        path = self._build_and_solve(start_lat, start_lon, end_lat, end_lon, buffered)
        total_ms = (time.perf_counter() - t_start) * 1000

        if path is None:
            logger.warning("[PATHFIND] No path found (%.6f,%.6f)→(%.6f,%.6f) in %.1fms",
                           start_lat, start_lon, end_lat, end_lon, total_ms)
            return None

        result = path[1:-1] if len(path) > 2 else []
        logger.info("[PATHFIND] Path found: %d intermediate points, %.1fms total",
                    len(result), total_ms)
        return result

    def _prune_obstacles(self, obstacles: List[List[List[float]]],
                         start_lat: float, start_lon: float,
                         end_lat: float, end_lon: float) -> List[List[List[float]]]:
        """Keep blockers first, then closest obstacles, within vertex budget."""
        mid_lat = (start_lat + end_lat) / 2
        mid_lon = (start_lon + end_lon) / 2

        # Obstacles that intersect the direct segment are mandatory.
        blocking = []
        non_blocking = []
        for poly in obstacles:
            if segment_intersects_polygon(start_lat, start_lon, end_lat, end_lon, poly):
                blocking.append(poly)
            else:
                non_blocking.append(poly)

        # Sort optional obstacles by distance from path midpoint.
        scored = []
        for poly in non_blocking:
            cx = sum(v[0] for v in poly) / len(poly)
            cy = sum(v[1] for v in poly) / len(poly)
            d = haversine_distance(mid_lat, mid_lon, cx, cy)
            scored.append((d, poly))
        scored.sort(key=lambda x: x[0])

        result = []
        vert_count = 0
        for poly in blocking:
            if vert_count + len(poly) > _MAX_OBSTACLE_VERTICES and result:
                break
            result.append(poly)
            vert_count += len(poly)
        for _, poly in scored:
            if vert_count + len(poly) > _MAX_OBSTACLE_VERTICES:
                break
            result.append(poly)
            vert_count += len(poly)

        logger.warning("Pruned obstacles: %d→%d polygons, %d→%d vertices",
                        len(obstacles), len(result),
                        sum(len(p) for p in obstacles), vert_count)
        return result

    def _build_and_solve(self, start_lat: float, start_lon: float,
                         end_lat: float, end_lon: float,
                         obstacles: List[List[List[float]]]) -> Optional[List[Tuple[float, float]]]:
        """Build visibility graph (numpy-vectorized) and find shortest path."""
        t0 = time.perf_counter()

        # Collect all vertices from obstacle polygons
        nodes_list: List[Tuple[float, float]] = [(start_lat, start_lon), (end_lat, end_lon)]
        start_idx = 0
        end_idx = 1

        path_dist = haversine_distance(start_lat, start_lon, end_lat, end_lon)
        mid_lat = (start_lat + end_lat) / 2
        mid_lon = (start_lon + end_lon) / 2
        max_range = path_dist * 2.0

        for poly in obstacles:
            for vertex in poly:
                vdist = haversine_distance(mid_lat, mid_lon, vertex[0], vertex[1])
                if vdist <= max_range:
                    nodes_list.append((vertex[0], vertex[1]))

        if len(nodes_list) <= 2:
            return None

        nodes = np.array(nodes_list, dtype=np.float64)  # (V, 2)
        V = len(nodes)

        # Collect all obstacle edges into one array
        all_edges = []
        for poly in obstacles:
            n = len(poly)
            for i in range(n):
                j = (i + 1) % n
                all_edges.append([poly[i][0], poly[i][1], poly[j][0], poly[j][1]])

        if not all_edges:
            return None

        edges = np.array(all_edges, dtype=np.float64)  # (E, 4)

        # Generate all V*(V-1)/2 node pairs
        pi, pj = np.triu_indices(V, k=1)
        segments = np.column_stack([
            nodes[pi, 0], nodes[pi, 1],
            nodes[pj, 0], nodes[pj, 1]
        ])  # (P, 4)

        # ── Batch edge-crossing check ──
        crosses = batch_segments_intersect(segments, edges)  # (P, E)
        any_crossing = crosses.any(axis=1)  # (P,)

        # ── Interior check for non-crossing pairs ──
        # (catches segments along polygon boundary that go through interior)
        blocked = any_crossing.copy()
        no_cross_idx = np.where(~any_crossing)[0]

        if len(no_cross_idx) > 0:
            check_segs = segments[no_cross_idx]

            mid_lats = (check_segs[:, 0] + check_segs[:, 2]) / 2
            mid_lons = (check_segs[:, 1] + check_segs[:, 3]) / 2

            dlat = check_segs[:, 2] - check_segs[:, 0]
            dlon = check_segs[:, 3] - check_segs[:, 1]
            norm = np.sqrt(dlat ** 2 + dlon ** 2)
            eps = 1e-7
            safe_norm = np.where(norm > 1e-15, norm, 1.0)
            perp_lat = -dlon / safe_norm * eps
            perp_lon = dlat / safe_norm * eps

            pts1 = np.column_stack([mid_lats + perp_lat, mid_lons + perp_lon])
            pts2 = np.column_stack([mid_lats - perp_lat, mid_lons - perp_lon])

            for poly in obstacles:
                poly_arr = np.array(poly, dtype=np.float64)
                in1 = batch_point_in_polygon(pts1, poly_arr)
                in2 = batch_point_in_polygon(pts2, poly_arr)
                both = in1 & in2
                if both.any():
                    blocked[no_cross_idx[both]] = True

        # ── Build adjacency from visible pairs ──
        visible_mask = ~blocked
        vis_pi = pi[visible_mask]
        vis_pj = pj[visible_mask]

        vis_segs = segments[visible_mask]
        dists = batch_haversine(
            vis_segs[:, 0], vis_segs[:, 1],
            vis_segs[:, 2], vis_segs[:, 3]
        )

        adj: Dict[int, List[Tuple[int, float]]] = {i: [] for i in range(V)}
        for k in range(len(vis_pi)):
            i_idx, j_idx, d = int(vis_pi[k]), int(vis_pj[k]), float(dists[k])
            adj[i_idx].append((j_idx, d))
            adj[j_idx].append((i_idx, d))

        elapsed_ms = (time.perf_counter() - t0) * 1000
        logger.info("[PATHFIND] Visibility graph: V=%d, edges=%d, pairs=%d, visible=%d, %.1fms",
                    V, len(edges), len(pi), int(visible_mask.sum()), elapsed_ms)

        # Dijkstra
        path_indices = self._dijkstra(adj, start_idx, end_idx, V)
        if path_indices is None:
            return None

        path = [(float(nodes[i, 0]), float(nodes[i, 1])) for i in path_indices]

        # Simplify: remove intermediate nodes with direct visibility
        path = self._simplify_path(path, obstacles)
        return path

    def _dijkstra(self, adj: Dict[int, List[Tuple[int, float]]],
                  start: int, end: int, n: int) -> Optional[List[int]]:
        """Shortest path via Dijkstra. Returns list of node indices or None."""
        dist = [float('inf')] * n
        prev = [-1] * n
        dist[start] = 0.0
        visited: Set[int] = set()

        heap = [(0.0, start)]
        while heap:
            d, u = heapq.heappop(heap)
            if u in visited:
                continue
            visited.add(u)
            if u == end:
                break
            for v, w in adj[u]:
                nd = d + w
                if nd < dist[v]:
                    dist[v] = nd
                    prev[v] = u
                    heapq.heappush(heap, (nd, v))

        if dist[end] == float('inf'):
            return None

        # Reconstruct path
        path = []
        cur = end
        while cur != -1:
            path.append(cur)
            cur = prev[cur]
        path.reverse()
        return path

    def _simplify_path(self, path: List[Tuple[float, float]],
                       obstacles: List[List[List[float]]]) -> List[Tuple[float, float]]:
        """Remove redundant intermediate points if direct visibility exists."""
        if len(path) <= 2:
            return path

        simplified = [path[0]]
        i = 0
        while i < len(path) - 1:
            # Try to skip as many points as possible
            farthest = i + 1
            for j in range(len(path) - 1, i + 1, -1):
                if is_visible_vg(path[i][0], path[i][1], path[j][0], path[j][1], obstacles):
                    farthest = j
                    break
            simplified.append(path[farthest])
            i = farthest

        return simplified
