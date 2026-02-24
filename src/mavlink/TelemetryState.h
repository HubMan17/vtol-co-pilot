#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <array>
#include "core/Types.h"

namespace vtol {

class TelemetryState : public QObject {
    Q_OBJECT

    // Flight data
    Q_PROPERTY(double timestamp      READ timestamp      NOTIFY timestampChanged)
    Q_PROPERTY(double airspeed       READ airspeed       NOTIFY airspeedChanged)
    Q_PROPERTY(double groundspeed    READ groundspeed    NOTIFY groundspeedChanged)
    Q_PROPERTY(double heading        READ heading        NOTIFY headingChanged)
    Q_PROPERTY(double altitude       READ altitude       NOTIFY altitudeChanged)
    Q_PROPERTY(double altitudeAgl    READ altitudeAgl    NOTIFY altitudeAglChanged)
    Q_PROPERTY(double climbRate      READ climbRate      NOTIFY climbRateChanged)

    // Attitude
    Q_PROPERTY(double roll  READ roll  NOTIFY rollChanged)
    Q_PROPERTY(double pitch READ pitch NOTIFY pitchChanged)
    Q_PROPERTY(double yaw   READ yaw   NOTIFY yawChanged)

    // Wind
    Q_PROPERTY(double windSpeed     READ windSpeed     NOTIFY windChanged)
    Q_PROPERTY(double windDirection READ windDirection NOTIFY windChanged)

    // Position
    Q_PROPERTY(double lat READ lat NOTIFY positionChanged)
    Q_PROPERTY(double lon READ lon NOTIFY positionChanged)

    // GPS
    Q_PROPERTY(int gpsFix     READ gpsFix     NOTIFY gpsChanged)
    Q_PROPERTY(int satellites READ satellites NOTIFY gpsChanged)

    // Battery
    Q_PROPERTY(double batteryVoltage READ batteryVoltage NOTIFY batteryChanged)
    Q_PROPERTY(double batteryCurrent READ batteryCurrent NOTIFY batteryChanged)

    // State
    Q_PROPERTY(bool    armed READ armed NOTIFY armedChanged)
    Q_PROPERTY(QString mode  READ mode  NOTIFY modeChanged)

public:
    explicit TelemetryState(QObject* parent = nullptr) : QObject(parent) {}

    // --- Getters ---
    double timestamp()      const { return m_timestamp; }
    double airspeed()       const { return m_airspeed; }
    double groundspeed()    const { return m_groundspeed; }
    double heading()        const { return m_heading; }
    double altitude()       const { return m_altitude; }
    double altitudeAgl()    const { return m_altitudeAgl; }
    double climbRate()      const { return m_climbRate; }
    double roll()           const { return m_roll; }
    double pitch()          const { return m_pitch; }
    double yaw()            const { return m_yaw; }
    double windSpeed()      const { return m_windSpeed; }
    double windDirection()  const { return m_windDirection; }
    double lat()            const { return m_position.lat; }
    double lon()            const { return m_position.lon; }
    int    gpsFix()         const { return m_gpsFix; }
    int    satellites()     const { return m_satellites; }
    double batteryVoltage() const { return m_batteryVoltage; }
    double batteryCurrent() const { return m_batteryCurrent; }
    bool   armed()          const { return m_armed; }
    QString mode()          const { return m_mode; }

    LatLon position() const { return m_position; }
    const std::array<int, 8>& rcChannels()    const { return m_rcChannels; }
    const std::array<int, 8>& rcChannelsRaw() const { return m_rcChannelsRaw; }
    const std::array<uint16_t, 8>& servoOutput() const { return m_servoOutput; }

    // --- Setters (called from MavlinkConnection) ---
    void setTimestamp(double v)      { if (m_timestamp != v) { m_timestamp = v; emit timestampChanged(); } }
    void setAirspeed(double v)       { if (m_airspeed != v) { m_airspeed = v; emit airspeedChanged(); } }
    void setGroundspeed(double v)    { if (m_groundspeed != v) { m_groundspeed = v; emit groundspeedChanged(); } }
    void setHeading(double v)        { if (m_heading != v) { m_heading = v; emit headingChanged(); } }
    void setAltitude(double v)       { if (m_altitude != v) { m_altitude = v; emit altitudeChanged(); } }
    void setAltitudeAgl(double v)    { if (m_altitudeAgl != v) { m_altitudeAgl = v; emit altitudeAglChanged(); } }
    void setClimbRate(double v)      { if (m_climbRate != v) { m_climbRate = v; emit climbRateChanged(); } }
    void setRoll(double v)           { if (m_roll != v) { m_roll = v; emit rollChanged(); } }
    void setPitch(double v)          { if (m_pitch != v) { m_pitch = v; emit pitchChanged(); } }
    void setYaw(double v)            { if (m_yaw != v) { m_yaw = v; emit yawChanged(); } }
    void setWindSpeed(double v)      { m_windSpeed = v; emit windChanged(); }
    void setWindDirection(double v)  { m_windDirection = v; emit windChanged(); }
    void setPosition(const LatLon& p) { m_position = p; emit positionChanged(); }
    void setGpsFix(int v)            { if (m_gpsFix != v) { m_gpsFix = v; emit gpsChanged(); } }
    void setSatellites(int v)        { if (m_satellites != v) { m_satellites = v; emit gpsChanged(); } }
    void setBatteryVoltage(double v) { m_batteryVoltage = v; emit batteryChanged(); }
    void setBatteryCurrent(double v) { m_batteryCurrent = v; emit batteryChanged(); }
    void setArmed(bool v)            { if (m_armed != v) { m_armed = v; emit armedChanged(); } }
    void setMode(const QString& v)   { if (m_mode != v) { m_mode = v; emit modeChanged(); } }
    void setRcChannels(const std::array<int, 8>& ch)    { m_rcChannels = ch; emit rcChannelsChanged(); }
    void setRcChannelsRaw(const std::array<int, 8>& ch) { m_rcChannelsRaw = ch; emit rcChannelsChanged(); }
    void setServoOutput(const std::array<uint16_t, 8>& s) { m_servoOutput = s; emit servoOutputChanged(); }

signals:
    void timestampChanged();
    void airspeedChanged();
    void groundspeedChanged();
    void headingChanged();
    void altitudeChanged();
    void altitudeAglChanged();
    void climbRateChanged();
    void rollChanged();
    void pitchChanged();
    void yawChanged();
    void windChanged();
    void positionChanged();
    void gpsChanged();
    void batteryChanged();
    void armedChanged();
    void modeChanged();
    void rcChannelsChanged();
    void servoOutputChanged();

private:
    double  m_timestamp = 0.0;
    double  m_airspeed = 0.0;
    double  m_groundspeed = 0.0;
    double  m_heading = 0.0;
    double  m_altitude = 0.0;
    double  m_altitudeAgl = 0.0;
    double  m_climbRate = 0.0;
    double  m_roll = 0.0;
    double  m_pitch = 0.0;
    double  m_yaw = 0.0;
    double  m_windSpeed = 0.0;
    double  m_windDirection = 0.0;
    LatLon  m_position;
    int     m_gpsFix = 0;
    int     m_satellites = 0;
    double  m_batteryVoltage = 0.0;
    double  m_batteryCurrent = 0.0;
    bool    m_armed = false;
    QString m_mode;
    std::array<int, 8> m_rcChannels = {1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500};
    std::array<int, 8> m_rcChannelsRaw = {1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500};
    std::array<uint16_t, 8> m_servoOutput = {};
};

} // namespace vtol
