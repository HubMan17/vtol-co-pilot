#include "PathPlanner.h"
#include "Calculations.h"
#include "BatchGeometry.h"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <queue>
#include <chrono>
#include <algorithm>
#include <set>

namespace vtol {

PathPlanner::PathPlanner(const ZoneChecker& zoneChecker)
    : m_zoneChecker(zoneChecker)
{}

std::optional<std::vector<LatLon>> PathPlanner::planPath(
    double startLat, double startLon,
    double endLat, double endLon,
    double altitude)
{
    constexpr double margin = 5000.0;  // 5 km
    double minLat = std::min(startLat, endLat) - nav::metersToLatOffset(margin);
    double maxLat = std::max(startLat, endLat) + nav::metersToLatOffset(margin);
    double midLat = (startLat + endLat) / 2;
    double minLon = std::min(startLon, endLon) - nav::metersToLonOffset(margin, midLat);
    double maxLon = std::max(startLon, endLon) + nav::metersToLonOffset(margin, midLat);

    // Get zones (always included, never pruned) and settlements (prunable)
    auto zones = m_zoneChecker.getBufferedZones(altitude);
    auto allObstacles = m_zoneChecker.getBufferedObstacles(altitude, minLat, maxLat, minLon, maxLon);
    if (allObstacles.empty()) return std::vector<LatLon>{};  // no obstacles

    // Direct path clear?
    bool directClear = true;
    for (const auto& poly : allObstacles) {
        if (nav::segmentIntersectsPolygon(startLat, startLon, endLat, endLon, poly)) {
            directClear = false;
            break;
        }
    }
    if (directClear) return std::vector<LatLon>{};

    // Build final obstacle list: zones always included + pruned settlements
    int zoneVerts = 0;
    for (const auto& z : zones) zoneVerts += static_cast<int>(z.size());

    // Separate settlements (obstacles that are not zones)
    PolyList settlements;
    for (size_t i = zones.size(); i < allObstacles.size(); ++i)
        settlements.push_back(allObstacles[i]);

    int settlementVerts = 0;
    for (const auto& s : settlements) settlementVerts += static_cast<int>(s.size());

    PolyList buffered;
    // Always add all zones first
    for (auto& z : zones) buffered.push_back(std::move(z));

    // Prune settlements only if they exceed the budget
    if (settlementVerts > MAX_SETTLEMENT_VERTICES) {
        auto pruned = pruneObstacles(settlements, startLat, startLon, endLat, endLon);
        for (auto& s : pruned) buffered.push_back(std::move(s));
    } else {
        for (auto& s : settlements) buffered.push_back(std::move(s));
    }

    int totalVerts = 0;
    for (const auto& p : buffered) totalVerts += static_cast<int>(p.size());
    SPDLOG_INFO("[PATHFIND] obstacles: {} zones ({} verts) + {} settlements → {} total ({} verts)",
                zones.size(), zoneVerts, buffered.size() - zones.size(), buffered.size(), totalVerts);

    auto t0 = std::chrono::steady_clock::now();
    auto path = buildAndSolve(startLat, startLon, endLat, endLon, buffered);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    if (elapsed > 500) {
        SPDLOG_WARN("[PATHFIND] Timeout: computation took {}ms, discarding result", elapsed);
        return std::nullopt;
    }

    if (!path) {
        SPDLOG_WARN("[PATHFIND] No path found ({:.6f},{:.6f})→({:.6f},{:.6f}) in {}ms",
            startLat, startLon, endLat, endLon, elapsed);
        return std::nullopt;
    }

    // Strip start/end
    std::vector<LatLon> result;
    if (path->size() > 2)
        result.assign(path->begin() + 1, path->end() - 1);

    SPDLOG_INFO("[PATHFIND] Path found: {} intermediate points, {}ms", result.size(), elapsed);
    return result;
}

PathPlanner::PolyList PathPlanner::pruneObstacles(
    const PolyList& obstacles,
    double startLat, double startLon,
    double endLat, double endLon)
{
    double midLat = (startLat + endLat) / 2;
    double midLon = (startLon + endLon) / 2;

    PolyList blocking, nonBlocking;
    for (const auto& poly : obstacles) {
        if (nav::segmentIntersectsPolygon(startLat, startLon, endLat, endLon, poly))
            blocking.push_back(poly);
        else
            nonBlocking.push_back(poly);
    }

    // Sort non-blocking by distance from midpoint
    std::vector<std::pair<double, const Polygon*>> scored;
    for (const auto& poly : nonBlocking) {
        double cx = 0, cy = 0;
        for (const auto& v : poly) { cx += v.lat; cy += v.lon; }
        cx /= poly.size(); cy /= poly.size();
        scored.push_back({nav::haversineDistance(midLat, midLon, cx, cy), &poly});
    }
    std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first < b.first; });

    PolyList result;
    int vertCount = 0;
    for (const auto& poly : blocking) {
        if (vertCount + static_cast<int>(poly.size()) > MAX_SETTLEMENT_VERTICES && !result.empty()) break;
        result.push_back(poly);
        vertCount += static_cast<int>(poly.size());
    }
    for (const auto& [d, poly] : scored) {
        if (vertCount + static_cast<int>(poly->size()) > MAX_SETTLEMENT_VERTICES) break;
        result.push_back(*poly);
        vertCount += static_cast<int>(poly->size());
    }

    SPDLOG_WARN("Pruned obstacles: {}→{} polygons, vertices capped at {}",
        obstacles.size(), result.size(), vertCount);
    return result;
}

std::optional<std::vector<LatLon>> PathPlanner::buildAndSolve(
    double startLat, double startLon,
    double endLat, double endLon,
    const PolyList& obstacles)
{
    // Collect nodes: start + end + obstacle vertices
    std::vector<LatLon> nodes;
    nodes.push_back({startLat, startLon});
    nodes.push_back({endLat, endLon});
    constexpr int startIdx = 0, endIdx = 1;

    double pathDist = nav::haversineDistance(startLat, startLon, endLat, endLon);
    double midLat = (startLat + endLat) / 2;
    double midLon = (startLon + endLon) / 2;
    double maxRange = pathDist * 2.0;

    for (const auto& poly : obstacles) {
        for (const auto& v : poly) {
            if (nav::haversineDistance(midLat, midLon, v.lat, v.lon) <= maxRange)
                nodes.push_back(v);
        }
    }

    int V = static_cast<int>(nodes.size());
    if (V <= 2) return std::nullopt;

    // Collect obstacle edges
    std::vector<std::array<double, 4>> allEdges;
    for (const auto& poly : obstacles) {
        int n = static_cast<int>(poly.size());
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            allEdges.push_back({poly[i].lat, poly[i].lon, poly[j].lat, poly[j].lon});
        }
    }
    if (allEdges.empty()) return std::nullopt;

    int E = static_cast<int>(allEdges.size());

    // Build Eigen matrices
    Eigen::MatrixXd edgeMat(E, 4);
    for (int i = 0; i < E; ++i) {
        edgeMat(i, 0) = allEdges[i][0]; edgeMat(i, 1) = allEdges[i][1];
        edgeMat(i, 2) = allEdges[i][2]; edgeMat(i, 3) = allEdges[i][3];
    }

    // Generate all V*(V-1)/2 pairs
    std::vector<int> pi, pj;
    for (int i = 0; i < V; ++i)
        for (int j = i + 1; j < V; ++j) {
            pi.push_back(i);
            pj.push_back(j);
        }
    int P = static_cast<int>(pi.size());

    Eigen::MatrixXd segMat(P, 4);
    for (int k = 0; k < P; ++k) {
        segMat(k, 0) = nodes[pi[k]].lat; segMat(k, 1) = nodes[pi[k]].lon;
        segMat(k, 2) = nodes[pj[k]].lat; segMat(k, 3) = nodes[pj[k]].lon;
    }

    // Batch edge-crossing
    auto crosses = nav::batchSegmentsIntersect(segMat, edgeMat);  // (P, E)
    std::vector<bool> blocked(P, false);
    for (int k = 0; k < P; ++k) {
        for (int e = 0; e < E; ++e) {
            if (crosses(k, e)) { blocked[k] = true; break; }
        }
    }

    // Interior check for non-crossing pairs
    std::vector<int> noCrossIdx;
    for (int k = 0; k < P; ++k) {
        if (!blocked[k]) noCrossIdx.push_back(k);
    }

    if (!noCrossIdx.empty()) {
        int M = static_cast<int>(noCrossIdx.size());
        Eigen::MatrixXd checkSegs(M, 4);
        for (int i = 0; i < M; ++i)
            checkSegs.row(i) = segMat.row(noCrossIdx[i]);

        Eigen::VectorXd midLats = (checkSegs.col(0) + checkSegs.col(2)) / 2;
        Eigen::VectorXd midLons = (checkSegs.col(1) + checkSegs.col(3)) / 2;
        Eigen::VectorXd dlat = checkSegs.col(2) - checkSegs.col(0);
        Eigen::VectorXd dlon = checkSegs.col(3) - checkSegs.col(1);
        Eigen::VectorXd norm = (dlat.array().square() + dlon.array().square()).sqrt();

        constexpr double eps = 1e-7;
        Eigen::VectorXd safeNorm = norm.array().max(1e-15);
        Eigen::VectorXd perpLat = -dlon.array() / safeNorm.array() * eps;
        Eigen::VectorXd perpLon = dlat.array() / safeNorm.array() * eps;

        Eigen::MatrixXd pts1(M, 2), pts2(M, 2);
        pts1.col(0) = midLats + perpLat; pts1.col(1) = midLons + perpLon;
        pts2.col(0) = midLats - perpLat; pts2.col(1) = midLons - perpLon;

        for (const auto& poly : obstacles) {
            int n = static_cast<int>(poly.size());
            Eigen::MatrixXd polyArr(n, 2);
            for (int i = 0; i < n; ++i) { polyArr(i, 0) = poly[i].lat; polyArr(i, 1) = poly[i].lon; }

            auto in1 = nav::batchPointInPolygon(pts1, polyArr);
            auto in2 = nav::batchPointInPolygon(pts2, polyArr);

            for (int i = 0; i < M; ++i) {
                if (in1(i) && in2(i))
                    blocked[noCrossIdx[i]] = true;
            }
        }
    }

    // Build adjacency
    std::vector<std::vector<std::pair<int, double>>> adj(V);
    for (int k = 0; k < P; ++k) {
        if (blocked[k]) continue;
        double d = nav::haversineDistance(
            nodes[pi[k]].lat, nodes[pi[k]].lon,
            nodes[pj[k]].lat, nodes[pj[k]].lon);
        adj[pi[k]].push_back({pj[k], d});
        adj[pj[k]].push_back({pi[k], d});
    }

    SPDLOG_INFO("[PATHFIND] Visibility graph: V={}, edges={}, pairs={}", V, E, P);

    auto pathIndices = dijkstra(adj, startIdx, endIdx, V);
    if (!pathIndices) return std::nullopt;

    std::vector<LatLon> path;
    for (int idx : *pathIndices)
        path.push_back(nodes[idx]);

    return simplifyPath(path, obstacles);
}

std::optional<std::vector<int>> PathPlanner::dijkstra(
    const std::vector<std::vector<std::pair<int, double>>>& adj,
    int start, int end, int n)
{
    std::vector<double> dist(n, std::numeric_limits<double>::infinity());
    std::vector<int> prev(n, -1);
    dist[start] = 0.0;
    std::set<int> visited;

    using PQEntry = std::pair<double, int>;
    std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<>> heap;
    heap.push({0.0, start});

    while (!heap.empty()) {
        auto [d, u] = heap.top(); heap.pop();
        if (visited.count(u)) continue;
        visited.insert(u);
        if (u == end) break;

        for (auto [v, w] : adj[u]) {
            double nd = d + w;
            if (nd < dist[v]) {
                dist[v] = nd;
                prev[v] = u;
                heap.push({nd, v});
            }
        }
    }

    if (dist[end] == std::numeric_limits<double>::infinity())
        return std::nullopt;

    std::vector<int> path;
    for (int cur = end; cur != -1; cur = prev[cur])
        path.push_back(cur);
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<LatLon> PathPlanner::simplifyPath(const std::vector<LatLon>& path,
                                                const PolyList& obstacles)
{
    if (path.size() <= 2) return path;

    // Convert obstacles to the format isVisibleVg expects
    std::vector<LatLon> simplified = {path[0]};
    int i = 0;
    while (i < static_cast<int>(path.size()) - 1) {
        int farthest = i + 1;
        for (int j = static_cast<int>(path.size()) - 1; j > i + 1; --j) {
            if (nav::isVisibleVg(path[i].lat, path[i].lon, path[j].lat, path[j].lon, obstacles)) {
                farthest = j;
                break;
            }
        }
        simplified.push_back(path[farthest]);
        i = farthest;
    }
    return simplified;
}

} // namespace vtol
