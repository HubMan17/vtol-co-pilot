#include "MavlinkConnection.h"
#include <spdlog/spdlog.h>
#include <QMetaObject>
#include <cmath>
#include <chrono>

namespace vtol {

// ArduPlane mode string → custom_mode number
const std::map<std::string, int> MavlinkConnection::s_modeMapping = {
    {"MANUAL", 0},    {"CIRCLE", 1},     {"STABILIZE", 2},  {"TRAINING", 3},
    {"ACRO", 4},      {"FBWA", 5},       {"FLY_BY_WIRE_A", 5},
    {"FBWB", 6},      {"FLY_BY_WIRE_B", 6},
    {"CRUISE", 7},    {"AUTOTUNE", 8},   {"AUTO", 10},
    {"RTL", 11},      {"LOITER", 12},    {"GUIDED", 15},
};

// custom_mode number → display string
const std::map<int, QString> MavlinkConnection::s_modeNames = {
    {0, "MANUAL"},   {1, "CIRCLE"},    {2, "STABILIZE"},  {3, "TRAINING"},
    {4, "ACRO"},     {5, "FBWA"},      {6, "FBWB"},       {7, "CRUISE"},
    {8, "AUTOTUNE"}, {10, "AUTO"},     {11, "RTL"},       {12, "LOITER"},
    {14, "LAND"},    {15, "GUIDED"},   {17, "QSTABILIZE"},{18, "QHOVER"},
    {19, "QLOITER"}, {20, "QLAND"},   {21, "QRTL"},
};

MavlinkConnection::MavlinkConnection(const MavlinkConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
{
    SPDLOG_DEBUG("MavlinkConnection created: {}:{}", config.sitl_host, config.sitl_port);
}

MavlinkConnection::~MavlinkConnection()
{
    disconnect();
}

bool MavlinkConnection::connectToSitl()
{
    SPDLOG_INFO("Connecting to SITL at {}:{}...", m_config.sitl_host, m_config.sitl_port);

    mavsdk::Mavsdk::Configuration mavsdk_config{
        mavsdk::Mavsdk::Configuration{mavsdk::ComponentType::GroundStation}};
    m_mavsdk = std::make_unique<mavsdk::Mavsdk>(mavsdk_config);

    auto connStr = std::format("tcp://{}:{}", m_config.sitl_host, m_config.sitl_port);
    auto result = m_mavsdk->add_any_connection(connStr);
    if (result != mavsdk::ConnectionResult::Success) {
        SPDLOG_ERROR("Connection failed: {}", static_cast<int>(result));
        return false;
    }

    SPDLOG_INFO("Waiting for system discovery...");
    // Wait for system with timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        auto systems = m_mavsdk->systems();
        if (!systems.empty()) {
            m_system = systems[0];
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!m_system) {
        SPDLOG_ERROR("No system discovered within timeout");
        return false;
    }

    SPDLOG_INFO("System discovered, id={}", m_system->get_system_id());

    // Initialize plugins
    m_telPlugin = std::make_unique<mavsdk::Telemetry>(m_system);
    m_action = std::make_unique<mavsdk::Action>(m_system);
    m_param = std::make_unique<mavsdk::Param>(m_system);
    m_passthrough = std::make_unique<mavsdk::MavlinkPassthrough>(m_system);

    setupTelemetrySubscriptions();

    m_connected = true;
    emit connectionRestored();
    SPDLOG_INFO("Connected to SITL successfully");
    return true;
}

void MavlinkConnection::disconnect()
{
    if (!m_connected) return;

    SPDLOG_INFO("Disconnecting from SITL...");
    m_connected = false;

    m_telPlugin.reset();
    m_action.reset();
    m_param.reset();
    m_passthrough.reset();
    m_system.reset();
    m_mavsdk.reset();
}

void MavlinkConnection::setupTelemetrySubscriptions()
{
    if (!m_telPlugin) return;

    m_telPlugin->set_rate_position(4.0);
    m_telPlugin->set_rate_attitude_euler(10.0);
    m_telPlugin->set_rate_velocity_ned(4.0);
    m_telPlugin->set_rate_battery(1.0);
    m_telPlugin->set_rate_gps_info(2.0);
    m_telPlugin->set_rate_rc_status(4.0);

    // Position
    m_telPlugin->subscribe_position([this](mavsdk::Telemetry::Position pos) {
        QMetaObject::invokeMethod(this, [this, pos]() {
            m_telemetry.setPosition(LatLon{pos.latitude_deg, pos.longitude_deg});
            m_telemetry.setAltitude(pos.relative_altitude_m);
            m_telemetry.setAltitudeAgl(pos.relative_altitude_m);
            emit telemetryUpdated();
        }, Qt::QueuedConnection);
    });

    // Attitude (Euler)
    m_telPlugin->subscribe_attitude_euler([this](mavsdk::Telemetry::EulerAngle euler) {
        QMetaObject::invokeMethod(this, [this, euler]() {
            m_telemetry.setRoll(euler.roll_deg);
            m_telemetry.setPitch(euler.pitch_deg);
            m_telemetry.setYaw(euler.yaw_deg);
        }, Qt::QueuedConnection);
    });

    // Velocity NED → groundspeed + climb rate
    m_telPlugin->subscribe_velocity_ned([this](mavsdk::Telemetry::VelocityNed vel) {
        QMetaObject::invokeMethod(this, [this, vel]() {
            double gs = std::sqrt(vel.north_m_s * vel.north_m_s + vel.east_m_s * vel.east_m_s);
            m_telemetry.setGroundspeed(gs);
            m_telemetry.setClimbRate(-vel.down_m_s);
        }, Qt::QueuedConnection);
    });

    // Battery
    m_telPlugin->subscribe_battery([this](mavsdk::Telemetry::Battery bat) {
        QMetaObject::invokeMethod(this, [this, bat]() {
            m_telemetry.setBatteryVoltage(bat.voltage_v);
            // MAVSDK battery doesn't have current directly — will use passthrough for SYS_STATUS
        }, Qt::QueuedConnection);
    });

    // GPS
    m_telPlugin->subscribe_gps_info([this](mavsdk::Telemetry::GpsInfo gps) {
        QMetaObject::invokeMethod(this, [this, gps]() {
            m_telemetry.setGpsFix(static_cast<int>(gps.fix_type));
            m_telemetry.setSatellites(gps.num_satellites);
        }, Qt::QueuedConnection);
    });

    // Armed state
    m_telPlugin->subscribe_armed([this](bool armed) {
        QMetaObject::invokeMethod(this, [this, armed]() {
            m_telemetry.setArmed(armed);
        }, Qt::QueuedConnection);
    });

    // Flight mode
    m_telPlugin->subscribe_flight_mode([this](mavsdk::Telemetry::FlightMode fm) {
        QMetaObject::invokeMethod(this, [this, fm]() {
            // MAVSDK flight mode enum — map to our string names
            QString modeName = "UNKNOWN";
            switch (fm) {
                case mavsdk::Telemetry::FlightMode::Manual:    modeName = "MANUAL"; break;
                case mavsdk::Telemetry::FlightMode::Stabilized: modeName = "STABILIZE"; break;
                case mavsdk::Telemetry::FlightMode::Mission:   modeName = "AUTO"; break;
                case mavsdk::Telemetry::FlightMode::ReturnToLaunch: modeName = "RTL"; break;
                case mavsdk::Telemetry::FlightMode::Hold:      modeName = "LOITER"; break;
                case mavsdk::Telemetry::FlightMode::Offboard:  modeName = "GUIDED"; break;
                default: modeName = QString("MODE_%1").arg(static_cast<int>(fm)); break;
            }
            m_telemetry.setMode(modeName);
        }, Qt::QueuedConnection);
    });

    // Raw MAVLink for VFR_HUD, WIND, RC, SYS_STATUS, HEARTBEAT
    if (m_passthrough) {
        m_passthrough->subscribe_message(
            MAVLINK_MSG_ID_VFR_HUD,
            [this](const mavlink_message_t& msg) {
                mavlink_vfr_hud_t hud;
                mavlink_msg_vfr_hud_decode(&msg, &hud);
                QMetaObject::invokeMethod(this, [this, hud]() {
                    m_telemetry.setAirspeed(hud.airspeed);
                    m_telemetry.setGroundspeed(hud.groundspeed);
                    m_telemetry.setHeading(hud.heading);
                    m_telemetry.setAltitude(hud.alt);
                    m_telemetry.setClimbRate(hud.climb);
                    emit telemetryUpdated();
                }, Qt::QueuedConnection);
            }
        );

        m_passthrough->subscribe_message(
            MAVLINK_MSG_ID_WIND,
            [this](const mavlink_message_t& msg) {
                mavlink_wind_t wind;
                mavlink_msg_wind_decode(&msg, &wind);
                QMetaObject::invokeMethod(this, [this, wind]() {
                    m_telemetry.setWindDirection(wind.direction);
                    m_telemetry.setWindSpeed(wind.speed);
                }, Qt::QueuedConnection);
            }
        );

        m_passthrough->subscribe_message(
            MAVLINK_MSG_ID_RC_CHANNELS,
            [this](const mavlink_message_t& msg) {
                mavlink_rc_channels_t rc;
                mavlink_msg_rc_channels_decode(&msg, &rc);
                QMetaObject::invokeMethod(this, [this, rc]() {
                    m_telemetry.setRcChannels({
                        rc.chan1_raw, rc.chan2_raw, rc.chan3_raw, rc.chan4_raw,
                        rc.chan5_raw, rc.chan6_raw, rc.chan7_raw, rc.chan8_raw
                    });
                }, Qt::QueuedConnection);
            }
        );

        m_passthrough->subscribe_message(
            MAVLINK_MSG_ID_SYS_STATUS,
            [this](const mavlink_message_t& msg) {
                mavlink_sys_status_t sys;
                mavlink_msg_sys_status_decode(&msg, &sys);
                QMetaObject::invokeMethod(this, [this, sys]() {
                    m_telemetry.setBatteryVoltage(sys.voltage_battery / 1000.0);
                    m_telemetry.setBatteryCurrent(sys.current_battery / 100.0);
                }, Qt::QueuedConnection);
            }
        );

        m_passthrough->subscribe_message(
            MAVLINK_MSG_ID_HEARTBEAT,
            [this](const mavlink_message_t& msg) {
                mavlink_heartbeat_t hb;
                mavlink_msg_heartbeat_decode(&msg, &hb);
                if (hb.type == MAV_TYPE_GCS) return; // Ignore GCS heartbeats
                QMetaObject::invokeMethod(this, [this, hb]() {
                    m_telemetry.setArmed((hb.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0);
                    auto it = s_modeNames.find(static_cast<int>(hb.custom_mode));
                    if (it != s_modeNames.end()) {
                        m_telemetry.setMode(it->second);
                    } else {
                        m_telemetry.setMode(QString("MODE_%1").arg(hb.custom_mode));
                    }
                }, Qt::QueuedConnection);
            }
        );

        // Forward all raw messages
        m_passthrough->subscribe_message(0, [this](const mavlink_message_t& msg) {
            emit rawMessage(msg);
        });
    }
}

// ── Commands ────────────────────────────────────────────────────────

void MavlinkConnection::sendCommandInt(uint16_t command, uint8_t frame,
                                         float p1, float p2, float p3, float p4,
                                         int32_t x, int32_t y, float z)
{
    if (!m_passthrough) return;

    mavlink_message_t msg;
    mavlink_msg_command_int_pack(
        m_config.system_id, m_config.component_id, &msg,
        m_system->get_system_id(), MAV_COMP_ID_AUTOPILOT1,
        frame, command, 0, 0,
        p1, p2, p3, p4, x, y, z
    );
    m_passthrough->send_message(msg);
}

void MavlinkConnection::sendCommandLong(uint16_t command,
                                          float p1, float p2, float p3, float p4,
                                          float p5, float p6, float p7)
{
    if (!m_passthrough) return;

    mavlink_message_t msg;
    mavlink_msg_command_long_pack(
        m_config.system_id, m_config.component_id, &msg,
        m_system->get_system_id(), MAV_COMP_ID_AUTOPILOT1,
        command, 0,
        p1, p2, p3, p4, p5, p6, p7
    );
    m_passthrough->send_message(msg);
}

void MavlinkConnection::sendGuidedTarget(double lat, double lon, double alt)
{
    if (!m_passthrough) return;

    mavlink_message_t msg;
    mavlink_msg_mission_item_int_pack(
        m_config.system_id, m_config.component_id, &msg,
        m_system->get_system_id(), MAV_COMP_ID_AUTOPILOT1,
        0,                                              // seq
        MAV_FRAME_GLOBAL_RELATIVE_ALT,                 // frame
        MAV_CMD_NAV_WAYPOINT,                          // command
        2,                                              // current = 2 (GUIDED target)
        0,                                              // autocontinue
        0, 0, 0, 0,                                     // params 1-4
        static_cast<int32_t>(lat * 1e7),               // lat degE7
        static_cast<int32_t>(lon * 1e7),               // lon degE7
        static_cast<float>(alt),                        // alt meters relative
        MAV_MISSION_TYPE_MISSION
    );
    m_passthrough->send_message(msg);
}

void MavlinkConnection::sendGuidedChangeAltitude(double altitudeM, double rateMs)
{
    sendCommandInt(
        43001,                          // MAV_CMD_GUIDED_CHANGE_ALTITUDE
        MAV_FRAME_GLOBAL_RELATIVE_ALT,
        0, 0,                           // p1, p2 empty
        static_cast<float>(rateMs),     // p3: rate (m/s), 0 = max
        0,                              // p4 empty
        0, 0,                           // x, y not used
        static_cast<float>(altitudeM)   // z: target altitude
    );
    SPDLOG_INFO("GUIDED_CHANGE_ALT: target={:.1f}m rate={:.1f}m/s", altitudeM, rateMs);
}

void MavlinkConnection::sendSpeed(double airspeedMs)
{
    sendCommandLong(
        MAV_CMD_DO_CHANGE_SPEED,
        0,                              // p1: 0 = airspeed
        static_cast<float>(airspeedMs), // p2: speed (m/s)
        -1,                             // p3: throttle (-1 = no change)
        0, 0, 0, 0
    );
}

void MavlinkConnection::sendLoiterUnlim(double lat, double lon, double alt,
                                          double radius, bool ccw)
{
    double compensated = std::abs(radius) * ORBIT_RADIUS_COMPENSATION;
    sendCommandInt(
        MAV_CMD_DO_REPOSITION,
        MAV_FRAME_GLOBAL_RELATIVE_ALT,
        -1,                                         // p1: ground speed (-1 = no change)
        1,                                          // p2: MAV_DO_REPOSITION_FLAGS_CHANGE_MODE
        static_cast<float>(compensated),            // p3: loiter radius (compensated)
        ccw ? 1.0f : 0.0f,                         // p4: direction (0=CW, 1=CCW)
        static_cast<int32_t>(lat * 1e7),            // x
        static_cast<int32_t>(lon * 1e7),            // y
        static_cast<float>(alt)                     // z
    );
    SPDLOG_INFO("DO_REPOSITION: lat={:.6f} lon={:.6f} alt={:.1f} radius={:.0f}(cmd={:.0f}) {}",
        lat, lon, alt, radius, compensated, ccw ? "CCW" : "CW");
}

void MavlinkConnection::setMode(const std::string& modeName)
{
    auto it = s_modeMapping.find(modeName);
    if (it == s_modeMapping.end()) {
        SPDLOG_ERROR("SET MODE FAILED: Unknown mode={}", modeName);
        return;
    }

    if (!m_passthrough) return;

    mavlink_message_t msg;
    mavlink_msg_set_mode_pack(
        m_config.system_id, m_config.component_id, &msg,
        m_system->get_system_id(),
        MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
        it->second
    );
    m_passthrough->send_message(msg);
    SPDLOG_INFO("SET MODE: {} (id={})", modeName, it->second);
}

void MavlinkConnection::setParam(const std::string& name, float value)
{
    if (!m_param) return;

    auto result = m_param->set_param_float(name, value);
    if (result != mavsdk::Param::Result::Success) {
        SPDLOG_ERROR("SET PARAM FAILED: {} = {} (result={})", name, value, static_cast<int>(result));
        return;
    }
    SPDLOG_INFO("SET PARAM: {} = {}", name, value);
}

void MavlinkConnection::sendRcOverride(const std::map<int, int>& channels)
{
    if (!m_passthrough) return;

    // 65535 = don't touch this channel
    uint16_t ch[8] = {65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535};
    for (const auto& [idx, val] : channels) {
        if (idx >= 1 && idx <= 8) {
            ch[idx - 1] = static_cast<uint16_t>(val);
        }
    }

    mavlink_message_t msg;
    mavlink_msg_rc_channels_override_pack(
        m_config.system_id, m_config.component_id, &msg,
        m_system->get_system_id(), MAV_COMP_ID_AUTOPILOT1,
        ch[0], ch[1], ch[2], ch[3], ch[4], ch[5], ch[6], ch[7],
        65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535
    );
    m_passthrough->send_message(msg);
}

void MavlinkConnection::releaseRcOverride()
{
    if (!m_passthrough) return;

    // 0 = release, 65535 = don't touch
    mavlink_message_t msg;
    mavlink_msg_rc_channels_override_pack(
        m_config.system_id, m_config.component_id, &msg,
        m_system->get_system_id(), MAV_COMP_ID_AUTOPILOT1,
        0, 0, 0, 65535, 65535, 65535, 65535, 65535,
        65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535
    );
    m_passthrough->send_message(msg);
}

void MavlinkConnection::sendPositionReset(double lat, double lon, double accuracy)
{
    sendCommandInt(
        43210,  // custom: forcePositionReset
        MAV_FRAME_GLOBAL_RELATIVE_ALT,
        static_cast<float>(accuracy), 0, 0, 0,
        static_cast<int32_t>(lat * 1e7),
        static_cast<int32_t>(lon * 1e7),
        0
    );
    SPDLOG_INFO("POSITION_RESET: lat={:.6f} lon={:.6f} acc={:.1f}", lat, lon, accuracy);
}

void MavlinkConnection::sendWindOverride(int directionDeg, int speedMs, double accuracy)
{
    double dirRad = directionDeg * M_PI / 180.0;
    float windN = static_cast<float>(-speedMs * std::cos(dirRad));
    float windE = static_cast<float>(-speedMs * std::sin(dirRad));

    sendCommandInt(
        43211,  // custom: forceWindReset
        0,      // frame unused
        windN, windE,
        static_cast<float>(accuracy), 0,
        0, 0, 0
    );
    SPDLOG_INFO("WIND_OVERRIDE: dir={}° speed={}m/s (N={:.1f} E={:.1f}) acc={:.1f}",
        directionDeg, speedMs, windN, windE, accuracy);
}

void MavlinkConnection::setCruiseAirspeed(double speedMs)
{
    setParam("TRIM_ARSPD_CM", static_cast<float>(speedMs * 100.0));
    setParam("AIRSPEED_CRUISE", static_cast<float>(speedMs));
}

void MavlinkConnection::requestDataStreams(int rate)
{
    if (!m_passthrough) return;

    mavlink_message_t msg;
    mavlink_msg_request_data_stream_pack(
        m_config.system_id, m_config.component_id, &msg,
        m_system->get_system_id(), MAV_COMP_ID_AUTOPILOT1,
        MAV_DATA_STREAM_ALL,
        rate, 1
    );
    m_passthrough->send_message(msg);
    SPDLOG_DEBUG("Requested data streams at {} Hz", rate);
}

void MavlinkConnection::sendReposition(double lat, double lon, double alt)
{
    if (!m_passthrough) return;

    // type_mask: use position only, ignore velocity/acceleration/yaw
    uint16_t typeMask = 0x0008 | 0x0010 | 0x0020 |  // ignore vx, vy, vz
                        0x0040 | 0x0080 | 0x0100 |  // ignore ax, ay, az
                        0x0400 | 0x0800;             // ignore yaw, yaw_rate

    mavlink_message_t msg;
    mavlink_msg_set_position_target_global_int_pack(
        m_config.system_id, m_config.component_id, &msg,
        0,  // time_boot_ms
        m_system->get_system_id(), MAV_COMP_ID_AUTOPILOT1,
        MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
        typeMask,
        static_cast<int32_t>(lat * 1e7),
        static_cast<int32_t>(lon * 1e7),
        static_cast<float>(alt),
        0, 0, 0,  // velocity
        0, 0, 0,  // acceleration
        0, 0      // yaw, yaw_rate
    );
    m_passthrough->send_message(msg);
    SPDLOG_INFO("POSITION_TARGET: lat={:.6f} lon={:.6f} alt={:.1f}", lat, lon, alt);
}

} // namespace vtol
