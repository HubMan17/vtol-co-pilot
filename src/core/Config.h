#pragma once

#include "Types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <array>

namespace vtol {

struct MavlinkConfig {
    std::string sitl_host = "127.0.0.1";
    int sitl_port = 5762;
    int proxy_port = 14550;
    int system_id = 255;
    int component_id = 0;
};

struct AutopilotConfig {
    PidCoeffs heading_pid  = {1.5, 0.08, 0.15};
    PidCoeffs altitude_pid = {0.8, 0.05, 1.0};
    PidCoeffs speed_pid    = {50.0, 10.0, 5.0};
    double bank_limit = 27.0;
    double roll_limit_deg = 30.0;
    int    rc_roll_max = 1886;
    double pitch_limit_up = 12.0;
    double pitch_limit_down = 15.0;
    double target_airspeed = 20.0;
    double guided_yaw_rate = 25.0;
    int    stick_threshold = 50;
    int    timeout_ms = 3000;
    double altitude_tolerance = 3.0;
};

struct ZoneAvoidanceConfig {
    std::string settlement_mode = "disabled";       // "disabled" | "always" | "below_altitude"
    double settlement_min_altitude = 200.0;
    double settlement_buffer = 500.0;
    std::string nofly_mode = "always";              // "disabled" | "always" | "below_altitude"
    double nofly_buffer = 200.0;
};

struct NavigationConfig {
    double waypoint_radius = 150.0;
};

struct GuiConfig {
    std::array<double, 2> map_center = {55.751, 37.618};
    int map_zoom = 10;
    int track_length = 9999;
    bool show_track = true;
    bool show_waypoints = true;
    bool show_zones = true;
    bool show_settlements = true;
};

struct AppConfig {
    MavlinkConfig mavlink;
    AutopilotConfig autopilot;
    NavigationConfig navigation;
    GuiConfig gui;
    ZoneAvoidanceConfig zone_avoidance;
};

// Load config from JSON file. Returns default config if file doesn't exist.
AppConfig loadConfig(const std::string& path = "config/settings.json");

// Save config to JSON file.
void saveConfig(const AppConfig& config, const std::string& path = "config/settings.json");

} // namespace vtol
