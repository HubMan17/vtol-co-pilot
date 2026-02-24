#include "Config.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>

namespace vtol {

// --- PidCoeffs JSON (must be in vtol:: for ADL) ---
void to_json(nlohmann::json& j, const PidCoeffs& c) {
    j = {{"p", c.p}, {"i", c.i}, {"d", c.d}};
}

void from_json(const nlohmann::json& j, PidCoeffs& c) {
    j.at("p").get_to(c.p);
    j.at("i").get_to(c.i);
    j.at("d").get_to(c.d);
}

// ── Load ────────────────────────────────────────────────────────────

AppConfig loadConfig(const std::string& path)
{
    namespace fs = std::filesystem;
    AppConfig cfg;

    if (!fs::exists(path)) {
        SPDLOG_WARN("Config file not found: {}, using defaults", path);
        return cfg;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        SPDLOG_ERROR("Failed to open config file: {}", path);
        return cfg;
    }

    nlohmann::json data;
    try {
        file >> data;
    } catch (const nlohmann::json::exception& e) {
        SPDLOG_ERROR("JSON parse error in {}: {}", path, e.what());
        return cfg;
    }

    SPDLOG_INFO("Loading config from {}", path);

    // --- mavlink ---
    if (data.contains("mavlink")) {
        auto& m = data["mavlink"];
        if (m.contains("sitl_host"))    m["sitl_host"].get_to(cfg.mavlink.sitl_host);
        if (m.contains("sitl_port"))    m["sitl_port"].get_to(cfg.mavlink.sitl_port);
        if (m.contains("proxy_port"))   m["proxy_port"].get_to(cfg.mavlink.proxy_port);
        if (m.contains("system_id"))    m["system_id"].get_to(cfg.mavlink.system_id);
        if (m.contains("component_id")) m["component_id"].get_to(cfg.mavlink.component_id);
    }

    // --- autopilot ---
    if (data.contains("autopilot")) {
        auto& a = data["autopilot"];
        if (a.contains("heading_pid"))     a["heading_pid"].get_to(cfg.autopilot.heading_pid);
        if (a.contains("altitude_pid"))    a["altitude_pid"].get_to(cfg.autopilot.altitude_pid);
        if (a.contains("speed_pid"))       a["speed_pid"].get_to(cfg.autopilot.speed_pid);
        if (a.contains("bank_limit"))      a["bank_limit"].get_to(cfg.autopilot.bank_limit);
        if (a.contains("roll_limit_deg"))  a["roll_limit_deg"].get_to(cfg.autopilot.roll_limit_deg);
        if (a.contains("rc_roll_max"))     a["rc_roll_max"].get_to(cfg.autopilot.rc_roll_max);
        if (a.contains("target_airspeed")) a["target_airspeed"].get_to(cfg.autopilot.target_airspeed);
        if (a.contains("guided_yaw_rate")) a["guided_yaw_rate"].get_to(cfg.autopilot.guided_yaw_rate);
        if (a.contains("stick_threshold")) a["stick_threshold"].get_to(cfg.autopilot.stick_threshold);
        if (a.contains("timeout_ms"))      a["timeout_ms"].get_to(cfg.autopilot.timeout_ms);
        if (a.contains("altitude_tolerance")) a["altitude_tolerance"].get_to(cfg.autopilot.altitude_tolerance);

        // Migration: pitch_limit → pitch_limit_up / pitch_limit_down
        if (a.contains("pitch_limit") && !a.contains("pitch_limit_up")) {
            double old = a["pitch_limit"].get<double>();
            cfg.autopilot.pitch_limit_up = old;
            cfg.autopilot.pitch_limit_down = old;
            SPDLOG_INFO("Migrated pitch_limit {} → pitch_limit_up/down", old);
        } else {
            if (a.contains("pitch_limit_up"))   a["pitch_limit_up"].get_to(cfg.autopilot.pitch_limit_up);
            if (a.contains("pitch_limit_down")) a["pitch_limit_down"].get_to(cfg.autopilot.pitch_limit_down);
        }
    }

    // --- navigation ---
    if (data.contains("navigation")) {
        auto& n = data["navigation"];
        if (n.contains("waypoint_radius")) n["waypoint_radius"].get_to(cfg.navigation.waypoint_radius);
        // drift_coefficient removed — ignore from old configs
    }

    // --- gui ---
    if (data.contains("gui")) {
        auto& g = data["gui"];
        if (g.contains("map_center") && g["map_center"].is_array() && g["map_center"].size() == 2) {
            cfg.gui.map_center[0] = g["map_center"][0].get<double>();
            cfg.gui.map_center[1] = g["map_center"][1].get<double>();
        }
        if (g.contains("map_zoom"))          g["map_zoom"].get_to(cfg.gui.map_zoom);
        if (g.contains("track_length"))     g["track_length"].get_to(cfg.gui.track_length);
        if (g.contains("show_track"))       g["show_track"].get_to(cfg.gui.show_track);
        if (g.contains("show_waypoints"))   g["show_waypoints"].get_to(cfg.gui.show_waypoints);
        if (g.contains("show_zones"))       g["show_zones"].get_to(cfg.gui.show_zones);
        if (g.contains("show_settlements")) g["show_settlements"].get_to(cfg.gui.show_settlements);
    }

    // --- zone_avoidance ---
    if (data.contains("zone_avoidance")) {
        auto& z = data["zone_avoidance"];
        if (z.contains("settlement_mode"))         z["settlement_mode"].get_to(cfg.zone_avoidance.settlement_mode);
        if (z.contains("settlement_min_altitude")) z["settlement_min_altitude"].get_to(cfg.zone_avoidance.settlement_min_altitude);
        if (z.contains("settlement_buffer"))       z["settlement_buffer"].get_to(cfg.zone_avoidance.settlement_buffer);
        if (z.contains("nofly_mode"))              z["nofly_mode"].get_to(cfg.zone_avoidance.nofly_mode);
        if (z.contains("nofly_buffer"))            z["nofly_buffer"].get_to(cfg.zone_avoidance.nofly_buffer);
    }

    // --- home ---
    if (data.contains("home")) {
        auto& h = data["home"];
        if (h.contains("auto_from_drone"))  h["auto_from_drone"].get_to(cfg.home.auto_from_drone);
        if (h.contains("overwrite_mode"))   h["overwrite_mode"].get_to(cfg.home.overwrite_mode);
        if (h.contains("notify_auto_set"))  h["notify_auto_set"].get_to(cfg.home.notify_auto_set);
        if (h.contains("notify_no_home"))   h["notify_no_home"].get_to(cfg.home.notify_no_home);
    }

    // --- notifications ---
    if (data.contains("notifications")) {
        auto& n = data["notifications"];
        if (n.contains("info_duration_sec"))     n["info_duration_sec"].get_to(cfg.notifications.info_duration_sec);
        if (n.contains("warning_duration_sec"))  n["warning_duration_sec"].get_to(cfg.notifications.warning_duration_sec);
        if (n.contains("critical_duration_sec")) n["critical_duration_sec"].get_to(cfg.notifications.critical_duration_sec);
        if (n.contains("interrupt_curtail_pct")) n["interrupt_curtail_pct"].get_to(cfg.notifications.interrupt_curtail_pct);
        SPDLOG_DEBUG("Notification timings: info={}s, warning={}s, critical={}s, curtail={}%",
            cfg.notifications.info_duration_sec, cfg.notifications.warning_duration_sec,
            cfg.notifications.critical_duration_sec, cfg.notifications.interrupt_curtail_pct);
    }

    // --- system ---
    if (data.contains("system")) {
        auto& s = data["system"];
        if (s.contains("v_max"))                s["v_max"].get_to(cfg.system.v_max);
        if (s.contains("v_operational_zero"))    s["v_operational_zero"].get_to(cfg.system.v_operational_zero);
        if (s.contains("v_absolute_zero"))       s["v_absolute_zero"].get_to(cfg.system.v_absolute_zero);
        if (s.contains("v_critical"))            s["v_critical"].get_to(cfg.system.v_critical);
        if (s.contains("hysteresis_margin"))     s["hysteresis_margin"].get_to(cfg.system.hysteresis_margin);
        if (s.contains("rolling_avg_samples"))   s["rolling_avg_samples"].get_to(cfg.system.rolling_avg_samples);
        if (s.contains("amperage_warning"))      s["amperage_warning"].get_to(cfg.system.amperage_warning);
        if (s.contains("amperage_debounce_sec")) s["amperage_debounce_sec"].get_to(cfg.system.amperage_debounce_sec);
        if (s.contains("action_on_limit"))       s["action_on_limit"].get_to(cfg.system.action_on_limit);
        if (s.contains("action_on_critical"))    s["action_on_critical"].get_to(cfg.system.action_on_critical);
        if (s.contains("batt50_max_notifications"))   s["batt50_max_notifications"].get_to(cfg.system.batt50_max_notifications);
        if (s.contains("batt50_timeout_sec"))         s["batt50_timeout_sec"].get_to(cfg.system.batt50_timeout_sec);
        if (s.contains("no_home_max_warnings"))       s["no_home_max_warnings"].get_to(cfg.system.no_home_max_warnings);
        if (s.contains("home_orbit_action"))          s["home_orbit_action"].get_to(cfg.system.home_orbit_action);
        if (s.contains("home_decision_reminder_sec")) s["home_decision_reminder_sec"].get_to(cfg.system.home_decision_reminder_sec);
        SPDLOG_INFO("[Config::load] system: v_max={:.1f}, v_op_zero={:.1f}, v_crit={:.1f}, hysteresis={:.1f}",
            cfg.system.v_max, cfg.system.v_operational_zero,
            cfg.system.v_critical, cfg.system.hysteresis_margin);
    }

    SPDLOG_DEBUG("Config loaded: SITL={}:{}, proxy_port={}, track_length={}",
        cfg.mavlink.sitl_host, cfg.mavlink.sitl_port, cfg.mavlink.proxy_port, cfg.gui.track_length);

    return cfg;
}

// ── Save ────────────────────────────────────────────────────────────

void saveConfig(const AppConfig& cfg, const std::string& path)
{
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(path).parent_path());

    nlohmann::json data;

    data["mavlink"] = {
        {"sitl_host",    cfg.mavlink.sitl_host},
        {"sitl_port",    cfg.mavlink.sitl_port},
        {"proxy_port",   cfg.mavlink.proxy_port},
        {"system_id",    cfg.mavlink.system_id},
        {"component_id", cfg.mavlink.component_id}
    };

    data["autopilot"] = {
        {"heading_pid",       cfg.autopilot.heading_pid},
        {"altitude_pid",      cfg.autopilot.altitude_pid},
        {"speed_pid",         cfg.autopilot.speed_pid},
        {"bank_limit",        cfg.autopilot.bank_limit},
        {"roll_limit_deg",    cfg.autopilot.roll_limit_deg},
        {"rc_roll_max",       cfg.autopilot.rc_roll_max},
        {"pitch_limit_up",    cfg.autopilot.pitch_limit_up},
        {"pitch_limit_down",  cfg.autopilot.pitch_limit_down},
        {"target_airspeed",   cfg.autopilot.target_airspeed},
        {"guided_yaw_rate",   cfg.autopilot.guided_yaw_rate},
        {"stick_threshold",   cfg.autopilot.stick_threshold},
        {"timeout_ms",        cfg.autopilot.timeout_ms},
        {"altitude_tolerance", cfg.autopilot.altitude_tolerance}
    };

    data["navigation"] = {
        {"waypoint_radius", cfg.navigation.waypoint_radius}
    };

    data["gui"] = {
        {"map_center",       {cfg.gui.map_center[0], cfg.gui.map_center[1]}},
        {"map_zoom",         cfg.gui.map_zoom},
        {"track_length",     cfg.gui.track_length},
        {"show_track",       cfg.gui.show_track},
        {"show_waypoints",   cfg.gui.show_waypoints},
        {"show_zones",       cfg.gui.show_zones},
        {"show_settlements", cfg.gui.show_settlements}
    };

    data["zone_avoidance"] = {
        {"settlement_mode",         cfg.zone_avoidance.settlement_mode},
        {"settlement_min_altitude", cfg.zone_avoidance.settlement_min_altitude},
        {"settlement_buffer",       cfg.zone_avoidance.settlement_buffer},
        {"nofly_mode",              cfg.zone_avoidance.nofly_mode},
        {"nofly_buffer",            cfg.zone_avoidance.nofly_buffer}
    };

    data["home"] = {
        {"auto_from_drone",  cfg.home.auto_from_drone},
        {"overwrite_mode",   cfg.home.overwrite_mode},
        {"notify_auto_set",  cfg.home.notify_auto_set},
        {"notify_no_home",   cfg.home.notify_no_home}
    };

    data["notifications"] = {
        {"info_duration_sec",     cfg.notifications.info_duration_sec},
        {"warning_duration_sec",  cfg.notifications.warning_duration_sec},
        {"critical_duration_sec", cfg.notifications.critical_duration_sec},
        {"interrupt_curtail_pct", cfg.notifications.interrupt_curtail_pct}
    };

    data["system"] = {
        {"v_max",                cfg.system.v_max},
        {"v_operational_zero",   cfg.system.v_operational_zero},
        {"v_absolute_zero",      cfg.system.v_absolute_zero},
        {"v_critical",           cfg.system.v_critical},
        {"hysteresis_margin",    cfg.system.hysteresis_margin},
        {"rolling_avg_samples",  cfg.system.rolling_avg_samples},
        {"amperage_warning",     cfg.system.amperage_warning},
        {"amperage_debounce_sec", cfg.system.amperage_debounce_sec},
        {"action_on_limit",      cfg.system.action_on_limit},
        {"action_on_critical",   cfg.system.action_on_critical},
        {"batt50_max_notifications",   cfg.system.batt50_max_notifications},
        {"batt50_timeout_sec",         cfg.system.batt50_timeout_sec},
        {"no_home_max_warnings",       cfg.system.no_home_max_warnings},
        {"home_orbit_action",          cfg.system.home_orbit_action},
        {"home_decision_reminder_sec", cfg.system.home_decision_reminder_sec}
    };

    std::ofstream file(path);
    if (!file.is_open()) {
        SPDLOG_ERROR("Failed to write config to {}", path);
        return;
    }

    file << data.dump(2);
    SPDLOG_INFO("Config saved to {}", path);
}

} // namespace vtol
