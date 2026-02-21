#include "ZoneManager.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace vtol {

namespace {
std::string nowIso() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}
} // anonymous

ZoneManager::ZoneManager(const std::string& path)
    : m_path(path)
{}

bool ZoneManager::load()
{
    if (!std::filesystem::exists(m_path)) {
        SPDLOG_WARN("Zones file not found: {}", m_path);
        return false;
    }

    try {
        std::ifstream file(m_path);
        nlohmann::json data;
        file >> data;

        m_zones.clear();
        for (const auto& z : data.value("zones", nlohmann::json::array())) {
            NoFlyZone zone;
            zone.id = z.at("id").get<std::string>();
            for (const auto& pt : z.at("points")) {
                zone.points.push_back({pt[0].get<double>(), pt[1].get<double>()});
            }
            zone.name = z.value("name", "");
            zone.description = z.value("description", "");
            if (z.contains("altitude") && !z["altitude"].is_null())
                zone.altitude = z["altitude"].get<double>();
            zone.color = z.value("color", "#1E293B");
            zone.created = z.value("created", "");
            zone.modified = z.value("modified", "");
            if (z.contains("avoid_mode") && !z["avoid_mode"].is_null())
                zone.avoid_mode = z["avoid_mode"].get<std::string>();
            if (z.contains("buffer") && !z["buffer"].is_null())
                zone.buffer = z["buffer"].get<double>();

            m_zones.push_back(std::move(zone));
        }

        SPDLOG_INFO("Loaded {} zones from {}", m_zones.size(), m_path);
        return true;

    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to load zones: {}", e.what());
        return false;
    }
}

bool ZoneManager::save()
{
    try {
        namespace fs = std::filesystem;
        fs::create_directories(fs::path(m_path).parent_path());

        nlohmann::json data;
        auto& zones = data["zones"] = nlohmann::json::array();

        for (const auto& z : m_zones) {
            nlohmann::json zj;
            zj["id"] = z.id;
            zj["points"] = nlohmann::json::array();
            for (const auto& pt : z.points)
                zj["points"].push_back({pt.lat, pt.lon});
            zj["name"] = z.name;
            zj["description"] = z.description;
            zj["altitude"] = z.altitude.has_value() ? nlohmann::json(z.altitude.value()) : nlohmann::json(nullptr);
            zj["color"] = z.color;
            zj["created"] = z.created;
            zj["modified"] = z.modified;
            zj["avoid_mode"] = z.avoid_mode.has_value() ? nlohmann::json(z.avoid_mode.value()) : nlohmann::json(nullptr);
            zj["buffer"] = z.buffer.has_value() ? nlohmann::json(z.buffer.value()) : nlohmann::json(nullptr);
            zones.push_back(std::move(zj));
        }

        std::ofstream file(m_path);
        file << data.dump(2);
        SPDLOG_INFO("Saved {} zones to {}", m_zones.size(), m_path);
        return true;

    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to save zones: {}", e.what());
        return false;
    }
}

std::string ZoneManager::addZone(const Polygon& points, const std::string& name,
                                   const std::string& description, std::optional<double> altitude)
{
    NoFlyZone zone;
    zone.id = generateUuid();
    zone.points = points;
    zone.name = name;
    zone.description = description;
    zone.altitude = altitude;
    zone.created = nowIso();
    zone.modified = zone.created;

    m_zones.push_back(std::move(zone));
    save();
    return m_zones.back().id;
}

bool ZoneManager::removeZone(const std::string& zoneId)
{
    auto it = std::find_if(m_zones.begin(), m_zones.end(),
        [&](const NoFlyZone& z) { return z.id == zoneId; });
    if (it == m_zones.end()) return false;
    m_zones.erase(it);
    save();
    return true;
}

void ZoneManager::clearAll()
{
    auto count = m_zones.size();
    m_zones.clear();
    save();
    SPDLOG_INFO("[ZoneManager] Cleared all {} zones", count);
}

bool ZoneManager::updateZone(const std::string& zoneId, const std::map<std::string, nlohmann::json>& fields)
{
    auto* zone = getZone(zoneId);
    if (!zone) return false;

    for (const auto& [key, val] : fields) {
        if (key == "name") zone->name = val.get<std::string>();
        else if (key == "description") zone->description = val.get<std::string>();
        else if (key == "color") zone->color = val.get<std::string>();
        else if (key == "altitude") zone->altitude = val.is_null() ? std::nullopt : std::optional(val.get<double>());
        else if (key == "avoid_mode") zone->avoid_mode = val.is_null() ? std::nullopt : std::optional(val.get<std::string>());
        else if (key == "buffer") zone->buffer = val.is_null() ? std::nullopt : std::optional(val.get<double>());
    }
    zone->modified = nowIso();
    save();
    return true;
}

bool ZoneManager::updateZonePoints(const std::string& zoneId, const Polygon& points)
{
    auto* zone = getZone(zoneId);
    if (!zone) return false;
    zone->points = points;
    zone->modified = nowIso();
    save();
    return true;
}

NoFlyZone* ZoneManager::getZone(const std::string& zoneId)
{
    auto it = std::find_if(m_zones.begin(), m_zones.end(),
        [&](const NoFlyZone& z) { return z.id == zoneId; });
    return it != m_zones.end() ? &(*it) : nullptr;
}

const NoFlyZone* ZoneManager::getZone(const std::string& zoneId) const
{
    auto it = std::find_if(m_zones.begin(), m_zones.end(),
        [&](const NoFlyZone& z) { return z.id == zoneId; });
    return it != m_zones.end() ? &(*it) : nullptr;
}

std::string ZoneManager::generateUuid()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    auto r = [&]() { return dist(gen); };
    std::ostringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(8) << r() << '-'
       << std::setw(4) << (r() & 0xFFFF) << '-'
       << std::setw(4) << ((r() & 0x0FFF) | 0x4000) << '-'
       << std::setw(4) << ((r() & 0x3FFF) | 0x8000) << '-'
       << std::setw(8) << r() << std::setw(4) << (r() & 0xFFFF);
    return ss.str();
}

} // namespace vtol
