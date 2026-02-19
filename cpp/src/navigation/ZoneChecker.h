#pragma once

#include "core/Types.h"
#include "core/Config.h"
#include "ZoneManager.h"
#include "Calculations.h"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace vtol {

struct CachedSettlement {
    Polygon polygon;
    std::string type;       // "city", "town", "village", "hamlet"
    std::string name;
    double minLat, maxLat, minLon, maxLon;  // precomputed bbox
};

struct RestrictionResult {
    bool restricted = false;
    std::string reason;
};

class ZoneChecker {
public:
    ZoneChecker(const ZoneManager& zoneManager, const ZoneAvoidanceConfig& config);

    void updateConfig(const ZoneAvoidanceConfig& config) { m_config = config; }

    // Primary check: is this point restricted?
    RestrictionResult isPointRestricted(double lat, double lon, double altitude) const;

    // Line-of-sight: does segment cross any obstacle?
    bool segmentIntersectsObstacles(double lat1, double lon1,
                                     double lat2, double lon2, double altitude) const;

    // Get active obstacle polygons (for pathfinding / visualization)
    std::vector<Polygon> getActiveObstacles(double altitude,
                                             double minLat, double maxLat,
                                             double minLon, double maxLon) const;

    // Get buffered obstacle polygons (for avoidance)
    std::vector<Polygon> getBufferedObstacles(double altitude,
                                               double minLat, double maxLat,
                                               double minLon, double maxLon) const;

    // Find intersection points of a segment with obstacles
    struct IntersectionPoint {
        double lat, lon;
        std::string reason;
    };
    std::vector<IntersectionPoint> findIntersectionPoints(
        double lat1, double lon1, double lat2, double lon2, double altitude) const;

    // Incremental settlement loading (from SettlementLoader)
    void addSettlementFeatures(const std::vector<CachedSettlement>& features);
    void setSettlementCacheDir(const std::string& dir);

    static const std::map<std::string, double> SETTLEMENT_FALLBACK_RADII;

private:
    std::vector<CachedSettlement> settlementsInBbox(double minLat, double maxLat,
                                                     double minLon, double maxLon) const;

    bool isNoFlyActive(const NoFlyZone& zone, double altitude) const;
    bool isSettlementActive(double altitude) const;

    const ZoneManager& m_zoneManager;
    ZoneAvoidanceConfig m_config;
    std::vector<CachedSettlement> m_settlements;
};

} // namespace vtol
