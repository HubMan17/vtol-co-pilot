#pragma once

#include "core/Types.h"
#include "ZoneChecker.h"
#include <vector>
#include <optional>

namespace vtol {

class PathPlanner {
public:
    explicit PathPlanner(const ZoneChecker& zoneChecker);

    /// Compute avoidance path around obstacles.
    /// Returns: empty — direct path is clear
    ///          points — intermediate waypoints (excluding start/end)
    ///          nullopt — no path found
    std::optional<std::vector<LatLon>> planPath(
        double startLat, double startLon,
        double endLat, double endLon,
        double altitude);

    static constexpr int MAX_SETTLEMENT_VERTICES = 150;

private:
    using PolyList = std::vector<Polygon>;

    PolyList pruneObstacles(const PolyList& obstacles,
                             double startLat, double startLon,
                             double endLat, double endLon);

    std::optional<std::vector<LatLon>> buildAndSolve(
        double startLat, double startLon,
        double endLat, double endLon,
        const PolyList& obstacles);

    std::optional<std::vector<int>> dijkstra(
        const std::vector<std::vector<std::pair<int, double>>>& adj,
        int start, int end, int n);

    std::vector<LatLon> simplifyPath(const std::vector<LatLon>& path,
                                      const PolyList& obstacles);

    const ZoneChecker& m_zoneChecker;
};

} // namespace vtol
