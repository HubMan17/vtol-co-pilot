#pragma once

#include "core/Types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <map>

namespace vtol {

struct NoFlyZone {
    std::string id;
    Polygon points;
    std::string name;
    std::string description;
    std::optional<double> altitude;      // max altitude — nullptr means any
    std::string color = "#1E293B";
    std::string created;
    std::string modified;
    std::optional<std::string> avoid_mode;  // per-zone override: "disabled"/"always"/"below_altitude"
    std::optional<double> buffer;           // per-zone buffer override (meters)
};

class ZoneManager {
public:
    explicit ZoneManager(const std::string& path = "data/zones.json");

    bool load();
    bool save();

    std::string addZone(const Polygon& points, const std::string& name,
                         const std::string& description = "", std::optional<double> altitude = std::nullopt);
    bool removeZone(const std::string& zoneId);
    void clearAll();
    bool updateZone(const std::string& zoneId, const std::map<std::string, nlohmann::json>& fields);
    bool updateZonePoints(const std::string& zoneId, const Polygon& points);

    NoFlyZone* getZone(const std::string& zoneId);
    const NoFlyZone* getZone(const std::string& zoneId) const;
    const std::vector<NoFlyZone>& getAllZones() const { return m_zones; }

private:
    static std::string generateUuid();

    std::string m_path;
    std::vector<NoFlyZone> m_zones;
};

} // namespace vtol
