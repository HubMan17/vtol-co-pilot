#pragma once

#include <QObject>
#include <memory>
#include <string>
#include <map>
#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/param/param.h>
#include <mavsdk/plugins/mavlink_passthrough/mavlink_passthrough.h>

#include "TelemetryState.h"
#include "core/Config.h"

namespace vtol {

class MavlinkConnection : public QObject {
    Q_OBJECT
public:
    explicit MavlinkConnection(const MavlinkConfig& config, QObject* parent = nullptr);
    ~MavlinkConnection() override;

    bool connectToSitl();
    void disconnect();

    [[nodiscard]] bool isConnected() const { return m_connected; }
    [[nodiscard]] TelemetryState* telemetry() { return &m_telemetry; }
    [[nodiscard]] const TelemetryState* telemetry() const { return &m_telemetry; }

    // --- Commands ---
    void sendGuidedTarget(double lat, double lon, double alt);
    void sendGuidedChangeAltitude(double altitudeM, double rateMs = 0.0);
    void sendSpeed(double airspeedMs);
    void sendLoiterUnlim(double lat, double lon, double alt, double radius, bool ccw = false);
    void setMode(const std::string& modeName);
    void setParam(const std::string& name, float value);
    void sendRcOverride(const std::map<int, int>& channels);
    void releaseRcOverride();
    void sendPositionReset(double lat, double lon, double accuracy = 5.0);
    void sendWindOverride(int directionDeg, int speedMs, double accuracy = 2.0);
    void setCruiseAirspeed(double speedMs);
    void requestDataStreams(int rate = 4);
    void sendReposition(double lat, double lon, double alt);

    static constexpr double ORBIT_RADIUS_COMPENSATION = 0.85;

signals:
    void telemetryUpdated();
    void connectionLost();
    void connectionRestored();
    void rawMessage(const mavlink_message_t& msg);

private:
    void setupTelemetrySubscriptions();
    void sendCommandInt(uint16_t command, uint8_t frame,
                        float p1, float p2, float p3, float p4,
                        int32_t x, int32_t y, float z);
    void sendCommandLong(uint16_t command,
                         float p1, float p2, float p3, float p4,
                         float p5, float p6, float p7);

    MavlinkConfig m_config;
    TelemetryState m_telemetry;
    bool m_connected = false;

    std::unique_ptr<mavsdk::Mavsdk> m_mavsdk;
    std::shared_ptr<mavsdk::System> m_system;
    std::unique_ptr<mavsdk::Telemetry> m_telPlugin;
    std::unique_ptr<mavsdk::Action> m_action;
    std::unique_ptr<mavsdk::Param> m_param;
    std::unique_ptr<mavsdk::MavlinkPassthrough> m_passthrough;

    // Mode mapping (ArduPlane)
    static const std::map<std::string, int> s_modeMapping;
    static const std::map<int, QString> s_modeNames;
};

} // namespace vtol
