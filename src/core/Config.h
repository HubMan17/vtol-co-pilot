#pragma once

#include "Types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <array>

namespace vtol {

struct MavlinkConfig {
    std::string protocol = "tcp";   // "tcp" for SITL, "udp" for real hardware
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

struct HomeConfig {
    bool auto_from_drone = true;           // Получать home с дрона
    std::string overwrite_mode = "ask";    // "force" | "keep" | "notify" | "ask"
    bool notify_auto_set = true;           // Уведомлять при авто-установке
    bool notify_no_home = true;            // Уведомлять если дом не задан при арме
};

struct NotificationConfig {
    int info_duration_sec     = 5;
    int warning_duration_sec  = 7;
    int critical_duration_sec = 10;
    int interrupt_curtail_pct = 50;  // % оставшегося времени при прерывании низкоприоритетного
};

struct SystemConfig {
    // Battery voltage thresholds
    double v_max              = 50.2;    // Fully charged
    double v_operational_zero = 40.5;    // Operational zero — time to land
    double v_absolute_zero    = 35.0;    // Real zero
    double v_critical         = 38.0;    // Critical — risk of thrust loss

    // Hysteresis
    double hysteresis_margin  = 1.0;     // Volts above threshold to reset
    int    rolling_avg_samples = 5;      // Smoothing window size

    // Amperage
    double amperage_warning     = 20.0;  // Amps threshold for warning
    int    amperage_debounce_sec = 30;   // Minimum interval between warnings

    // Actions: "notify" | "suggest_rth" | "auto_rtl"
    std::string action_on_limit    = "notify";
    // Actions: "notify" | "auto_rtl"
    std::string action_on_critical = "notify";

    // 50% decision flow
    int batt50_max_notifications    = 3;       // Notifications before auto-action
    int batt50_timeout_sec          = 15;      // Interval between notifications (sec)
    int no_home_max_warnings        = 3;       // "No home" warnings before orbit

    // Home orbit behavior: "wait" | "auto_rtl" | "auto_rtl_gps"
    std::string home_orbit_action   = "wait";
    int home_decision_reminder_sec  = 30;      // Reminder interval while orbiting (sec)
};

struct MeshConfig {
    bool enabled = false;
    std::string ground_modem_ip = "192.168.1.100";
    std::string air_modem_ip = "192.168.1.101";
    int poll_interval_ms = 500;
    int sliding_window_size = 10;
    int display_duration_sec = 4;
    double min_reliable_distance = 200.0;  // ниже — bearing ненадёжен
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
    HomeConfig home;
    NotificationConfig notifications;
    SystemConfig system;
    MeshConfig mesh;
};

// Load config from JSON file. Returns default config if file doesn't exist.
AppConfig loadConfig(const std::string& path = "config/settings.json");

// Save config to JSON file.
void saveConfig(const AppConfig& config, const std::string& path = "config/settings.json");

} // namespace vtol
