#include "AutopilotManager.h"
#include "mavlink/MavlinkConnection.h"
#include "navigation/RoutePlanner.h"
#include "navigation/ZoneChecker.h"
#include "navigation/PathPlanner.h"
#include "navigation/Calculations.h"

#include <spdlog/spdlog.h>
#include <QtConcurrent/QtConcurrent>
#include <QElapsedTimer>
#include <cmath>
#include <chrono>

namespace vtol {

static double nowSec() {
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

AutopilotManager::AutopilotManager(MavlinkConnection& connection,
                                     const AutopilotConfig& config,
                                     QObject* parent)
    : QObject(parent)
    , m_connection(connection)
    , m_config(config)
    , m_speedCtrl(20.0)
{
    connect(&m_connection, &MavlinkConnection::connectionLost, this, [this]() {
        disengage("Соединение потеряно");
    });
}

// ─────────────────────── setters ───────────────────────

void AutopilotManager::setHomePosition(const std::optional<LatLon>& pos)
{
    m_homePosition = pos;
    if (pos) {
        SPDLOG_INFO("HOME POSITION SET: lat={:.6f}, lon={:.6f}", pos->lat, pos->lon);
        // Reset avoidance so it re-computes for new home position
        if (m_returningHome) {
            SPDLOG_INFO("HOME CHANGED DURING RTH — resetting avoidance");
            m_avoidanceWaypoints.clear();
            m_avoidanceWpIdx = 0;
            m_avoidanceForWpId = -1;
            m_avoidanceGaveUp = {};
            m_avoidanceComputing = false;
            m_avoidancePending.hasResult = false;
            m_avoidanceSuppressedWpId = -1;
            m_avoidanceNotifiedWpId = -1;
        }
    } else {
        SPDLOG_INFO("HOME POSITION CLEARED");
    }
}

void AutopilotManager::resetZoneAvoidance()
{
    m_avoidanceWaypoints.clear();
    m_avoidanceWpIdx = 0;
    m_avoidanceForWpId = -1;
    m_avoidanceGaveUp = {};
    m_avoidanceComputing = false;
    m_avoidancePending.hasResult = false;
    m_avoidanceLastCheckTime = 0.0;
    m_avoidanceSuppressedWpId = -1;
    m_avoidanceNotifiedWpId = -1;
    SPDLOG_INFO("AVOIDANCE: state reset — will recompute on next update");
}

void AutopilotManager::suppressAvoidanceRecheck()
{
    m_avoidanceSuppressedWpId = m_avoidanceForWpId;
    SPDLOG_INFO("AVOIDANCE: user suppressed rechecks for WP{}", m_avoidanceSuppressedWpId);
}

// ─────────────────────── engage / disengage ───────────────────────

bool AutopilotManager::engageNav()
{
    if (!m_connection.isConnected()) return false;
    if (!m_routePlanner) return false;

    Route* route = m_routePlanner->getRoute();
    if (!route || route->waypoints.empty()) return false;

    m_headingCtrl.reset();
    m_altitudeCtrl.reset();
    m_speedCtrl.reset();

    auto* tel = m_connection.telemetry();

    Waypoint* wp = m_routePlanner->activeWaypoint();
    if (!wp) {
        m_routePlanner->setActiveWaypoint(static_cast<int>(route->waypoints.size()) - 1);
        wp = m_routePlanner->activeWaypoint();
        if (!wp) return false;
    }

    if (wp->climb_enroute) {
        m_altitudeCtrl.setTargetAltitude(wp->altitude);
    } else {
        m_altitudeCtrl.setTargetAltitude(tel->altitudeAgl());
    }

    m_stickOverrideCount = 0;
    m_throttleBaseline = tel->rcChannels()[CH_THROTTLE];
    m_activeWaypointId = wp->id;

    if (m_savedAirspeedCruise.has_value()) {
        m_connection.setCruiseAirspeed(m_savedAirspeedCruise.value());
        m_savedAirspeedCruise.reset();
    }
    m_connection.setParamInt("THROTTLE_NUDGE", 1);

    m_isOrbiting = false;
    m_orbitTurnsCompleted = 0;
    m_orbitHeadingAccumulated = 0.0;
    m_orbitRepositionSent = false;
    m_waitingForAltitude = false;
    m_returningHome = false;
    m_loiterAltTransition = false;
    m_operationalWp.reset();
    m_operationalActive = false;
    m_operationalStandalone = false;
    m_disengageReason.clear();
    m_guidedSendTime = 0.0;
    m_avoidanceWaypoints.clear();
    m_avoidanceWpIdx = 0;
    m_avoidanceForWpId = -1;
    m_avoidanceGaveUp = {};
    m_avoidanceComputing = false;
    m_avoidancePending.hasResult = false;
    m_avoidanceSuppressedWpId = -1;
    m_avoidanceNotifiedWpId = -1;

    double now = nowSec();
    m_mode = AutopilotMode::NAV;
    m_lastUpdateTime = now;
    m_engageTime = now;

    // Force re-enter GUIDED to clear any DO_REPOSITION orbit radius
    m_connection.setMode("FBWA");
    m_connection.setMode("GUIDED");
    m_connection.sendSpeed(m_speedCtrl.target());

    double targetAlt = m_altitudeCtrl.target();
    LatLon pos = tel->position();
    if (pos.isValid()) {
        double initBearing = nav::bearingTo(pos.lat, pos.lon, wp->lat, wp->lon);
        auto [tgtLat, tgtLon] = nav::projectPoint(pos.lat, pos.lon, initBearing, 2000.0);
        m_connection.sendGuidedTarget(tgtLat, tgtLon, targetAlt);
    }
    if (wp->climb_enroute) {
        m_connection.sendGuidedChangeAltitude(targetAlt);
    }

    SPDLOG_INFO("ENGAGE NAV: waypoint={}/{}", wp->id, route->waypoints.size());
    emit engaged("NAV");
    return true;
}

bool AutopilotManager::engageHome()
{
    if (!m_connection.isConnected()) return false;
    if (!m_homePosition) return false;

    m_headingCtrl.reset();
    m_altitudeCtrl.reset();
    m_speedCtrl.reset();

    auto* tel = m_connection.telemetry();
    m_altitudeCtrl.setTargetAltitude(tel->altitudeAgl());
    m_stickOverrideCount = 0;
    m_throttleBaseline = tel->rcChannels()[CH_THROTTLE];

    if (m_routePlanner) {
        auto* wp = m_routePlanner->activeWaypoint();
        m_activeWaypointId = wp ? wp->id : -1;
    } else {
        m_activeWaypointId = -1;
    }

    if (m_savedAirspeedCruise.has_value()) {
        m_connection.setCruiseAirspeed(m_savedAirspeedCruise.value());
        m_savedAirspeedCruise.reset();
    }
    m_connection.setParamInt("THROTTLE_NUDGE", 1);

    m_isOrbiting = false;
    m_orbitTurnsCompleted = 0;
    m_orbitHeadingAccumulated = 0.0;
    m_orbitRepositionSent = false;
    m_waitingForAltitude = false;
    m_returningHome = true;
    m_loiterAltTransition = false;
    m_homeOrbitNotified = false;
    m_disengageReason.clear();
    m_guidedSendTime = 0.0;

    double now = nowSec();
    m_mode = AutopilotMode::NAV;
    m_lastUpdateTime = now;
    m_engageTime = now;

    m_connection.setMode("FBWA");
    m_connection.setMode("GUIDED");
    m_connection.sendSpeed(m_speedCtrl.target());

    LatLon pos = tel->position();
    if (pos.isValid()) {
        m_orbitCcw = chooseOrbitDirectionCcw(pos, m_homePosition->lat, m_homePosition->lon, tel->heading());
        double initBearing = nav::bearingTo(pos.lat, pos.lon, m_homePosition->lat, m_homePosition->lon);
        auto [tgtLat, tgtLon] = nav::projectPoint(pos.lat, pos.lon, initBearing, 2000.0);
        m_connection.sendGuidedTarget(tgtLat, tgtLon, tel->altitudeAgl());
    }

    SPDLOG_INFO("ENGAGE HOME: lat={:.6f} lon={:.6f} alt={:.1f}m",
                m_homePosition->lat, m_homePosition->lon, tel->altitudeAgl());
    emit engaged("HOME");
    return true;
}

// ─────────────────────── operational waypoint ───────────────────────

const OperationalWaypoint* AutopilotManager::operationalWaypoint() const
{
    return m_operationalWp.has_value() ? &m_operationalWp.value() : nullptr;
}

bool AutopilotManager::engageOperational(const Waypoint& wp, OperationalMode mode)
{
    if (!m_connection.isConnected()) return false;

    bool wasManual = (m_mode != AutopilotMode::NAV);

    if (wasManual) {
        // Auto-engage: initialize controllers and enter NAV/GUIDED
        SPDLOG_INFO("[AutopilotManager::engageOperational] auto-engage from MANUAL (standalone)");
        m_headingCtrl.reset();
        m_altitudeCtrl.reset();
        m_speedCtrl.reset();

        auto* tel = m_connection.telemetry();
        m_stickOverrideCount = 0;
        m_throttleBaseline = tel->rcChannels()[CH_THROTTLE];
        m_activeWaypointId = -1;

        if (m_savedAirspeedCruise.has_value()) {
            m_connection.setCruiseAirspeed(m_savedAirspeedCruise.value());
            m_savedAirspeedCruise.reset();
        }
        m_connection.setParamInt("THROTTLE_NUDGE", 1);

        m_isOrbiting = false;
        m_returningHome = false;
        m_homeOrbitNotified = false;
        m_loiterAltTransition = false;
        m_disengageReason.clear();

        double now = nowSec();
        m_mode = AutopilotMode::NAV;
        m_lastUpdateTime = now;
        m_engageTime = now;
        m_operationalStandalone = true;

        m_connection.setMode("FBWA");
        m_connection.setMode("GUIDED");
        m_connection.sendSpeed(m_speedCtrl.target());

        emit engaged("NAV");
    } else {
        m_operationalStandalone = false;
    }

    // Exit current orbit if any
    if (m_isOrbiting) {
        exitOrbit();
    }

    Waypoint opWpCopy = wp;
    opWpCopy.id = OPERATIONAL_WP_ID;
    m_operationalWp = OperationalWaypoint{opWpCopy, mode};
    m_operationalActive = true;

    // Reset avoidance for the new target
    m_avoidanceWaypoints.clear();
    m_avoidanceWpIdx = 0;
    m_avoidanceForWpId = -1;
    m_avoidanceGaveUp = {};
    m_avoidanceComputing = false;
    m_avoidancePending.hasResult = false;
    m_avoidanceSuppressedWpId = -1;
    m_avoidanceNotifiedWpId = -1;
    m_avoidanceLastCheckTime = 0.0;

    // Reset orbit state
    m_orbitTurnsCompleted = 0;
    m_orbitHeadingAccumulated = 0.0;
    m_orbitRepositionSent = false;
    m_waitingForAltitude = false;
    m_loiterAltTransition = false;
    m_tangentApproachWpId = -1;
    m_guidedSendTime = 0.0;

    // Altitude
    auto* tel = m_connection.telemetry();
    if (wp.climb_enroute) {
        m_altitudeCtrl.setTargetAltitude(wp.altitude);
    } else {
        m_altitudeCtrl.setTargetAltitude(tel->altitudeAgl());
    }

    // If was returning home, stop RTH
    m_returningHome = false;
    m_homeOrbitNotified = false;

    if (!wasManual) {
        // Re-enter GUIDED to clear DO_REPOSITION (already done for auto-engage above)
        m_connection.setMode("FBWA");
        m_connection.setMode("GUIDED");
    }

    // Initial vector towards operational WP
    LatLon pos = tel->position();
    if (pos.isValid()) {
        double initBearing = nav::bearingTo(pos.lat, pos.lon, wp.lat, wp.lon);
        auto [tgtLat, tgtLon] = nav::projectPoint(pos.lat, pos.lon, initBearing, GUIDED_PROJECTION_DISTANCE);
        m_connection.sendGuidedTarget(tgtLat, tgtLon, m_altitudeCtrl.target());
    }
    if (wp.climb_enroute) {
        m_connection.sendGuidedChangeAltitude(m_altitudeCtrl.target());
    }

    SPDLOG_INFO("[AutopilotManager::engageOperational] mode={}, lat={:.6f}, lon={:.6f}, alt={:.0f}, action={}, standalone={}",
                m_operationalWp->modeName(), wp.lat, wp.lon, wp.altitude, wp.action, m_operationalStandalone);
    emit operationalEngaged();
    return true;
}

void AutopilotManager::cancelOperational()
{
    if (!m_operationalActive) return;

    bool standalone = m_operationalStandalone;
    SPDLOG_INFO("[AutopilotManager::cancelOperational] standalone={}", standalone);

    if (m_isOrbiting) {
        exitOrbit();
    }

    m_operationalWp.reset();
    m_operationalActive = false;
    m_operationalStandalone = false;

    // Reset orbit/avoidance state
    m_orbitTurnsCompleted = 0;
    m_orbitHeadingAccumulated = 0.0;
    m_orbitRepositionSent = false;
    m_waitingForAltitude = false;
    m_loiterAltTransition = false;
    m_tangentApproachWpId = -1;
    m_guidedSendTime = 0.0;

    m_avoidanceWaypoints.clear();
    m_avoidanceWpIdx = 0;
    m_avoidanceForWpId = -1;
    m_avoidanceGaveUp = {};
    m_avoidanceComputing = false;
    m_avoidancePending.hasResult = false;
    m_avoidanceSuppressedWpId = -1;
    m_avoidanceNotifiedWpId = -1;
    m_avoidanceLastCheckTime = 0.0;

    emit operationalCancelled();

    // Standalone: no route to resume → disengage completely
    if (standalone || !m_routePlanner || !m_routePlanner->getRoute()
        || m_routePlanner->getRoute()->waypoints.empty()) {
        SPDLOG_INFO("[AutopilotManager::cancelOperational] No route — disengage");
        disengage("");
        return;
    }

    // Re-enter GUIDED and redirect to current route waypoint
    m_connection.setMode("FBWA");
    m_connection.setMode("GUIDED");

    Waypoint* wp = m_routePlanner->activeWaypoint();
    if (wp) {
        m_activeWaypointId = wp->id;
        if (wp->climb_enroute) {
            m_altitudeCtrl.setTargetAltitude(wp->altitude);
        }
        auto* tel = m_connection.telemetry();
        LatLon pos = tel->position();
        if (pos.isValid()) {
            double bearing = nav::bearingTo(pos.lat, pos.lon, wp->lat, wp->lon);
            auto [tgtLat, tgtLon] = nav::projectPoint(pos.lat, pos.lon, bearing, GUIDED_PROJECTION_DISTANCE);
            m_connection.sendGuidedTarget(tgtLat, tgtLon, m_altitudeCtrl.target());
        }
        SPDLOG_INFO("[AutopilotManager::cancelOperational] Redirected to WP{}", wp->id);
    }
}

void AutopilotManager::onOperationalReached()
{
    if (!m_operationalWp) return;

    SPDLOG_INFO("[AutopilotManager::onOperationalReached] mode={}", m_operationalWp->modeName());
    emit operationalReached();

    if (m_operationalWp->mode == OperationalMode::VIA_POINT) {
        // Auto-resume route
        cancelOperational();
    }
    // GOTO: stay active, operator must press "Resume route"
}

void AutopilotManager::activateRtl()
{
    SPDLOG_INFO("ACTIVATING RTL: switching to ArduPilot RTL mode");

    m_mode = AutopilotMode::MANUAL;
    m_isOrbiting = false;
    m_waitingForAltitude = false;
    m_returningHome = false;
    m_loiterAltTransition = false;
    m_homeOrbitNotified = false;
    m_tangentApproachWpId = -1;

    if (m_savedAirspeedCruise.has_value()) {
        m_connection.setCruiseAirspeed(m_savedAirspeedCruise.value());
        SPDLOG_INFO("RESTORED AIRSPEED_CRUISE={:.1f}", m_savedAirspeedCruise.value());
        m_savedAirspeedCruise.reset();
    }
    m_connection.setParamInt("THROTTLE_NUDGE", 1);

    m_connection.setMode("RTL");

    m_headingCtrl.reset();
    m_altitudeCtrl.reset();
    m_speedCtrl.reset();

    SPDLOG_INFO("RTL ACTIVATED");
    emit disengaged("NAV", QStringLiteral("RTL активирован"));
}

void AutopilotManager::disengage(const QString& reason)
{
    if (m_mode == AutopilotMode::MANUAL) return;

    SPDLOG_INFO("DISENGAGE: reason='{}', prev_mode={}",
                reason.toStdString(), m_mode == AutopilotMode::NAV ? "NAV" : "MANUAL");

    m_disengageReason = reason;
    QString prevMode = m_mode == AutopilotMode::NAV ? "NAV" : "MANUAL";
    m_mode = AutopilotMode::MANUAL;

    m_connection.setMode("CRUISE");
    m_headingCtrl.reset();
    m_altitudeCtrl.reset();
    m_speedCtrl.reset();

    m_isOrbiting = false;
    m_waitingForAltitude = false;
    m_returningHome = false;
    m_loiterAltTransition = false;
    m_homeOrbitNotified = false;
    m_tangentApproachWpId = -1;
    m_operationalWp.reset();
    m_operationalActive = false;
    m_operationalStandalone = false;

    if (m_savedAirspeedCruise.has_value()) {
        m_connection.setCruiseAirspeed(m_savedAirspeedCruise.value());
        SPDLOG_INFO("RESTORED AIRSPEED_CRUISE={:.1f}", m_savedAirspeedCruise.value());
        m_savedAirspeedCruise.reset();
    }
    m_connection.setParamInt("THROTTLE_NUDGE", 1);

    emit disengaged(prevMode, reason);
}

// ─────────────────────── main update loop ───────────────────────

void AutopilotManager::update()
{
    if (m_mode == AutopilotMode::MANUAL) return;

    if (!m_connection.isConnected()) {
        disengage("Соединение потеряно");
        return;
    }

    double currentTime = nowSec();
    auto* tel = m_connection.telemetry();

    double timeSinceEngage = (m_engageTime > 0) ? (currentTime - m_engageTime) : 0;
    if (timeSinceEngage > 2.0) {
        if (checkStickOverride(tel->rcChannels())) {
            disengage("Пилот взял управление");
            return;
        }
    }

    double timeoutSec = m_config.timeout_ms / 1000.0;
    if (m_lastUpdateTime > 0 && (currentTime - m_lastUpdateTime) > timeoutSec) {
        disengage("Таймаут обновления");
        return;
    }

    if (m_mode != AutopilotMode::NAV) {
        m_lastUpdateTime = currentTime;
        return;
    }

    if (!m_routePlanner) {
        disengage("Маршрут не задан");
        return;
    }

    LatLon position = tel->position();
    if (!position.isValid()) {
        m_lastUpdateTime = currentTime;
        return;
    }

    // Mode check (skip first 2s — mode takes time to propagate via HEARTBEAT)
    if (timeSinceEngage > 2.0 && !tel->mode().isEmpty()) {
        if (tel->mode() != "GUIDED") {
            SPDLOG_WARN("MODE CHECK FAILED: telemetry.mode='{}' expected GUIDED", tel->mode().toStdString());
            disengage(QString("Режим изменён: %1").arg(tel->mode()));
            return;
        }
    }

    // Detect active waypoint change (user changed via GUI)
    Waypoint* wpCheck = m_routePlanner->activeWaypoint();
    if (wpCheck && wpCheck->id != m_activeWaypointId) {
        // Cancel operational waypoint if user picks a different route WP
        if (m_operationalActive) {
            SPDLOG_INFO("REDIRECT: WP{} -> WP{} (was operational, cancelling)",
                        m_activeWaypointId, wpCheck->id);
            m_operationalWp.reset();
            m_operationalActive = false;
            emit operationalCancelled();
        }
        if (m_isOrbiting || m_returningHome) {
            SPDLOG_INFO("REDIRECT: WP{} -> WP{} (was {})", m_activeWaypointId, wpCheck->id,
                        m_returningHome ? "returning home" : "orbiting");
            exitOrbit();
            m_returningHome = false;
            m_waitingForAltitude = false;
            if (wpCheck->climb_enroute) {
                m_altitudeCtrl.setTargetAltitude(wpCheck->altitude);
            } else {
                m_altitudeCtrl.setTargetAltitude(tel->altitudeAgl());
            }
        }
        m_activeWaypointId = wpCheck->id;
        m_avoidanceWaypoints.clear();
        m_avoidanceWpIdx = 0;
        m_avoidanceForWpId = -1;
        m_avoidanceGaveUp = {};
        m_avoidanceComputing = false;
        m_avoidancePending.hasResult = false;
        m_avoidanceSuppressedWpId = -1;
        m_avoidanceNotifiedWpId = -1;
    }

    double targetBearing = tel->heading();  // fallback

    // ─── Return-to-home mode ───
    if (m_returningHome && m_homePosition) {
        double distHome = nav::haversineDistance(position.lat, position.lon,
                                                 m_homePosition->lat, m_homePosition->lon);

        if (distHome <= 160.0 || m_isOrbiting) {
            if (!m_isOrbiting) {
                // Start orbit at home — descend to 50m
                m_isOrbiting = true;
                m_orbitTurnsCompleted = 0;
                m_orbitHeadingAccumulated = 0.0;
                m_orbitLastHeading = tel->heading();
                if (!m_savedAirspeedCruise.has_value()) {
                    m_savedAirspeedCruise = tel->airspeed() > 0 ? tel->airspeed() : 25.0;
                }
                m_connection.setCruiseAirspeed(m_speedCtrl.target());
                m_connection.sendSpeed(m_speedCtrl.target());
                m_connection.setParamInt("THROTTLE_NUDGE", 0);
                m_connection.setParamInt("WP_LOITER_RAD", 150);
                m_connection.sendLoiterUnlim(m_homePosition->lat, m_homePosition->lon, 50.0, 150.0, m_orbitCcw);
                m_orbitRepositionSent = true;
                m_altitudeCtrl.setTargetAltitude(50.0);
                m_connection.sendGuidedChangeAltitude(50.0);
                SPDLOG_INFO("HOME REACHED -> ORBITING + DESCENDING: current={:.1f}m -> 50m {}",
                            tel->altitudeAgl(), m_orbitCcw ? "CCW" : "CW");
            }

            updateOrbitProgress(tel->heading());
            m_altitudeCtrl.update(tel->altitudeAgl());
            m_speedCtrl.update(tel->airspeed());

            if (!m_orbitRepositionSent) {
                m_connection.sendLoiterUnlim(m_homePosition->lat, m_homePosition->lon, 50.0, 150.0, m_orbitCcw);
                m_orbitRepositionSent = true;
            }

            // Check if home orbit is established: altitude reached + at least 1 full turn
            if (!m_homeOrbitNotified) {
                bool altOk = std::abs(tel->altitudeAgl() - 50.0) <= 5.0;
                bool turnsOk = m_orbitTurnsCompleted >= 1;
                SPDLOG_DEBUG("HOME ORBIT CHECK: alt={:.1f} alt_ok={}, turns={} turns_ok={}, notified={}",
                             tel->altitudeAgl(), altOk, m_orbitTurnsCompleted, turnsOk, m_homeOrbitNotified);
                if (altOk && turnsOk) {
                    m_homeOrbitNotified = true;
                    SPDLOG_INFO("HOME ORBIT ESTABLISHED: alt={:.1f}m, turns={}",
                                tel->altitudeAgl(), m_orbitTurnsCompleted);
                    emit homeOrbitEstablished();
                }
            }
        } else {
            // Navigate to home — check avoidance first
            bool avoidanceUsed = false;
            if (m_zoneChecker && m_pathPlanner) {
                Waypoint homeWp;
                homeWp.id = HOME_WP_ID;
                homeWp.lat = m_homePosition->lat;
                homeWp.lon = m_homePosition->lon;
                homeWp.altitude = tel->altitudeAgl();
                auto avoidTarget = getAvoidanceTarget(position, homeWp, tel->altitudeAgl());
                if (avoidTarget) {
                    auto [avLat, avLon] = *avoidTarget;
                    targetBearing = nav::bearingTo(position.lat, position.lon, avLat, avLon);
                    avoidanceUsed = true;
                    SPDLOG_DEBUG("HOME avoidance: bearing={:.0f} to ({:.5f},{:.5f}), {} remaining",
                                 targetBearing, avLat, avLon,
                                 m_avoidanceWaypoints.size() - m_avoidanceWpIdx);
                }
            }

            if (!avoidanceUsed) {
                // Direct navigation to home (tangent approach near home)
                if (distHome <= 300.0) {
                    double bearingFromHome = nav::bearingTo(m_homePosition->lat, m_homePosition->lon,
                                                            position.lat, position.lon);
                    double angleDeg = std::acos(std::min(150.0 / distHome, 1.0)) * 180.0 / M_PI;
                    double tangentBearing;
                    if (m_orbitCcw) {
                        tangentBearing = std::fmod(bearingFromHome - angleDeg + 360.0, 360.0);
                    } else {
                        tangentBearing = std::fmod(bearingFromHome + angleDeg, 360.0);
                    }
                    auto [tgtLat, tgtLon] = nav::projectPoint(m_homePosition->lat, m_homePosition->lon,
                                                                tangentBearing, 150.0);
                    targetBearing = nav::bearingTo(position.lat, position.lon, tgtLat, tgtLon);
                } else {
                    targetBearing = nav::bearingTo(position.lat, position.lon,
                                                    m_homePosition->lat, m_homePosition->lon);
                }
            }
            m_headingCtrl.setTargetHeading(targetBearing);
            m_headingCtrl.update(tel->heading());
            m_altitudeCtrl.update(tel->altitudeAgl());
            m_speedCtrl.update(tel->airspeed());
            sendGuidedCommands(targetBearing, position);
        }

        m_lastUpdateTime = nowSec();
        // Diagnostic logging
        m_logCounter++;
        if (m_logCounter >= 20) {
            m_logCounter = 0;
            SPDLOG_INFO("GUIDED: hdg={:.0f}->tgt={:.0f} err={:.1f} | alt={:.1f} tgt={:.1f} err={:.1f} | "
                        "spd={:.1f} tgt={:.1f} | mode={} gps={} armed={}",
                        tel->heading(), targetBearing, m_headingCtrl.error(),
                        tel->altitudeAgl(), m_altitudeCtrl.target(), m_altitudeCtrl.error(),
                        tel->airspeed(), m_speedCtrl.target(),
                        tel->mode().toStdString(), tel->gpsFix(), tel->armed());
        }
        return;
    }

    // ─── Operational waypoint navigation ───
    if (m_operationalActive && m_operationalWp) {
        auto& opWp = m_operationalWp->waypoint;
        double distToOp = nav::haversineDistance(position.lat, position.lon, opWp.lat, opWp.lon);

        if (m_isOrbiting) {
            // Orbiting over operational WP
            updateOrbitProgress(tel->heading());
            m_altitudeCtrl.update(tel->altitudeAgl());
            m_speedCtrl.update(tel->airspeed());
            targetBearing = tel->heading();

            if (!m_orbitRepositionSent) {
                double radius = opWp.orbit_radius > 0 ? opWp.orbit_radius : 150.0;
                m_connection.sendLoiterUnlim(opWp.lat, opWp.lon, opWp.altitude, radius, m_orbitCcw);
                m_orbitRepositionSent = true;
            }

            // ALTITUDE: auto-advance when target altitude reached
            if (opWp.action == "ALTITUDE" && !m_orbitAdvanceHandled) {
                if (m_altitudeCtrl.isOnAltitude(5.0)) {
                    m_orbitAdvanceHandled = true;
                    SPDLOG_INFO("[Operational] ALTITUDE REACHED: alt={:.1f}m", tel->altitudeAgl());
                    onOperationalReached();
                }
            }

            // ORBIT_TURNS: auto-advance when turns complete
            if (opWp.action == "ORBIT_TURNS" && m_orbitTurnsCompleted >= opWp.orbit_turns && !m_orbitAdvanceHandled) {
                m_orbitAdvanceHandled = true;
                SPDLOG_INFO("[Operational] ORBIT_TURNS complete: {}/{}", m_orbitTurnsCompleted, opWp.orbit_turns);
                onOperationalReached();
            }

            // ORBIT_INFINITE and GOTO with FLYTHROUGH — stay orbiting, operator decides
        } else {
            // Flying to operational WP
            bool hasOrbit = (opWp.action == "ORBIT_TURNS" || opWp.action == "ORBIT_INFINITE" || opWp.action == "ALTITUDE");
            double orbitRadius = opWp.orbit_radius > 0 ? opWp.orbit_radius : 150.0;
            bool tangentOrbitEntry = hasOrbit && distToOp <= orbitRadius;

            bool reached = distToOp <= opWp.radius;
            if (reached || tangentOrbitEntry) {
                // Operational WP reached
                if (!opWp.climb_enroute) {
                    m_altitudeCtrl.setTargetAltitude(opWp.altitude);
                    m_altitudeCtrl.update(tel->altitudeAgl());
                }

                if (hasOrbit) {
                    startOrbit(opWp, tel->heading());
                    if (!m_altitudeCtrl.isOnAltitude(5.0)) {
                        m_connection.sendGuidedChangeAltitude(opWp.altitude);
                    }
                    SPDLOG_INFO("[Operational] START ORBIT: action={}", opWp.action);
                    targetBearing = tel->heading();
                } else {
                    // FLYTHROUGH — operational reached immediately
                    SPDLOG_INFO("[Operational] FLYTHROUGH reached: dist={:.0f}m", distToOp);
                    onOperationalReached();
                }
            } else {
                // Flying towards operational WP — direct, no zone avoidance
                if (hasOrbit && distToOp <= orbitRadius * 2) {
                    // Tangent approach for orbit entry
                    if (m_tangentApproachWpId != opWp.id) {
                        m_orbitCcw = chooseOrbitDirectionCcw(position, opWp.lat, opWp.lon, tel->heading());
                        m_tangentApproachWpId = opWp.id;
                    }
                    double bearingFromOp = nav::bearingTo(opWp.lat, opWp.lon, position.lat, position.lon);
                    double angleDeg = std::acos(std::min(orbitRadius / distToOp, 1.0)) * 180.0 / M_PI;
                    double tangentBearing;
                    if (m_orbitCcw) {
                        tangentBearing = std::fmod(bearingFromOp - angleDeg + 360.0, 360.0);
                    } else {
                        tangentBearing = std::fmod(bearingFromOp + angleDeg, 360.0);
                    }
                    auto [tgtLat, tgtLon] = nav::projectPoint(opWp.lat, opWp.lon, tangentBearing, orbitRadius);
                    targetBearing = nav::bearingTo(position.lat, position.lon, tgtLat, tgtLon);
                } else {
                    targetBearing = nav::bearingTo(position.lat, position.lon, opWp.lat, opWp.lon);
                }

                m_headingCtrl.setTargetHeading(targetBearing);
                m_headingCtrl.update(tel->heading());
                m_altitudeCtrl.update(tel->altitudeAgl());
                m_speedCtrl.update(tel->airspeed());
                sendGuidedCommands(targetBearing, position);
            }
        }

        // Diagnostic logging
        m_logCounter++;
        if (m_logCounter >= 20) {
            m_logCounter = 0;
            SPDLOG_INFO("[Operational] dist={:.0f}m mode={} orbiting={} | hdg={:.0f} alt={:.1f} spd={:.1f}",
                        nav::haversineDistance(position.lat, position.lon, opWp.lat, opWp.lon),
                        m_operationalWp->modeName(), m_isOrbiting,
                        tel->heading(), tel->altitudeAgl(), tel->airspeed());
        }

        m_lastUpdateTime = nowSec();
        return;
    }

    // ─── Normal waypoint navigation ───
    Waypoint* wp = m_routePlanner->activeWaypoint();
    if (!wp) {
        disengage("Нет активной точки");
        return;
    }

    double distToWp = nav::haversineDistance(position.lat, position.lon, wp->lat, wp->lon);

    if (m_isOrbiting) {
        // Orbit mode
        updateOrbitProgress(tel->heading());
        m_altitudeCtrl.update(tel->altitudeAgl());
        m_speedCtrl.update(tel->airspeed());
        targetBearing = tel->heading();

        if (!m_orbitRepositionSent) {
            double radius = wp->orbit_radius > 0 ? wp->orbit_radius : 150.0;
            m_connection.sendLoiterUnlim(wp->lat, wp->lon, wp->altitude, radius, m_orbitCcw);
            m_orbitRepositionSent = true;
        }

        // ALTITUDE: auto-advance when target altitude reached
        if (wp->action == "ALTITUDE" && !m_orbitAdvanceHandled) {
            if (m_altitudeCtrl.isOnAltitude(5.0)) {
                m_orbitAdvanceHandled = true;
                SPDLOG_INFO("ALTITUDE REACHED at WP{}: alt={:.1f}m target={}m",
                            wp->id, tel->altitudeAgl(), wp->altitude);
                finishOrbitAndAdvance(*wp);
            }
        }

        // ORBIT_TURNS: auto-advance when turns complete
        if (wp->action == "ORBIT_TURNS" && m_orbitTurnsCompleted >= wp->orbit_turns && !m_orbitAdvanceHandled) {
            m_orbitAdvanceHandled = true;
            finishOrbitAndAdvance(*wp);
        }

    } else {
        // Flying to waypoint
        bool hasOrbit = (wp->action == "ORBIT_TURNS" || wp->action == "ORBIT_INFINITE" || wp->action == "ALTITUDE");
        double orbitRadius = wp->orbit_radius > 0 ? wp->orbit_radius : 150.0;
        bool tangentOrbitEntry = hasOrbit && distToWp <= orbitRadius;

        if (m_routePlanner->isWaypointReached(position) || tangentOrbitEntry) {
            // Waypoint reached
            if (!wp->climb_enroute && !m_waitingForAltitude) {
                m_altitudeCtrl.setTargetAltitude(wp->altitude);
                m_altitudeCtrl.update(tel->altitudeAgl());

                if (hasOrbit) {
                    startOrbit(*wp, tel->heading());
                    if (!m_altitudeCtrl.isOnAltitude(5.0)) {
                        m_connection.sendGuidedChangeAltitude(wp->altitude);
                        SPDLOG_INFO("ORBIT + ALT CHANGE at WP{}: target={}m", wp->id, wp->altitude);
                    } else {
                        SPDLOG_INFO("START ORBIT at WP{} (altitude OK)", wp->id);
                    }
                    targetBearing = tel->heading();
                } else {
                    m_waitingForAltitude = true;
                    m_guidedSendTime = 0.0;
                    m_connection.sendGuidedChangeAltitude(wp->altitude);
                    SPDLOG_INFO("WAITING FOR ALTITUDE: target={}m at WP{}", wp->altitude, wp->id);
                }
            }

            // Check if altitude reached (FLYTHROUGH only)
            if (m_waitingForAltitude) {
                m_altitudeCtrl.update(tel->altitudeAgl());
                if (m_altitudeCtrl.isOnAltitude(5.0)) {
                    SPDLOG_INFO("ALTITUDE REACHED at WP{}", wp->id);
                    m_waitingForAltitude = false;
                } else {
                    double now = nowSec();
                    if (now - m_guidedSendTime >= GUIDED_RESEND_INTERVAL) {
                        double targetAlt = m_altitudeCtrl.target();
                        auto [tLat, tLon] = nav::projectPoint(position.lat, position.lon, tel->heading(), 200.0);
                        m_connection.sendGuidedTarget(tLat, tLon, targetAlt);
                        m_connection.sendSpeed(m_speedCtrl.target());
                        m_guidedSendTime = now;
                    }
                    targetBearing = tel->heading();
                }
            }

            if (!m_waitingForAltitude) {
                if (hasOrbit) {
                    if (!m_isOrbiting) {
                        startOrbit(*wp, tel->heading());
                        SPDLOG_INFO("START ORBIT at WP{} (climb_enroute)", wp->id);
                    }
                    targetBearing = tel->heading();
                } else {
                    // FLYTHROUGH: advance to next waypoint
                    Waypoint oldWp = *wp;
                    int oldIdx = m_routePlanner->activeWaypointIndex();
                    m_routePlanner->nextWaypoint();
                    int newIdx = m_routePlanner->activeWaypointIndex();
                    Waypoint* newWp = m_routePlanner->activeWaypoint();

                    if (newIdx != oldIdx && newWp) {
                        m_activeWaypointId = newWp->id;
                        m_avoidanceWaypoints.clear();
                        m_avoidanceWpIdx = 0;
                        m_avoidanceForWpId = -1;
                        m_avoidanceGaveUp = {};
                        m_avoidanceComputing = false;
                        m_avoidancePending.hasResult = false;
                        m_avoidanceSuppressedWpId = -1;
                        m_avoidanceNotifiedWpId = -1;
                        if (newWp->climb_enroute) {
                            m_altitudeCtrl.setTargetAltitude(newWp->altitude);
                            m_waitingForAltitude = false;
                            SPDLOG_INFO("CLIMB ENROUTE to WP{}: target={}m", newWp->id, newWp->altitude);
                        } else {
                            m_altitudeCtrl.setTargetAltitude(tel->altitudeAgl());
                            m_waitingForAltitude = false;
                            SPDLOG_INFO("MAINTAIN ALTITUDE to WP{}: current={:.1f}m, will adjust to {}m on arrival",
                                        newWp->id, tel->altitudeAgl(), newWp->altitude);
                        }
                        emit waypointReached(oldWp.id, newWp->id);
                    } else {
                        // Last waypoint reached (FLYTHROUGH)
                        SPDLOG_INFO("LAST WAYPOINT REACHED: WP{} (FLYTHROUGH)", oldWp.id);
                        handleRouteCompletion();
                        m_lastUpdateTime = nowSec();
                        return;
                    }

                    wp = m_routePlanner->activeWaypoint();
                    if (!wp) {
                        disengage("Нет активной точки");
                        return;
                    }

                    targetBearing = nav::bearingTo(position.lat, position.lon, wp->lat, wp->lon);
                    m_headingCtrl.setTargetHeading(targetBearing);
                    m_headingCtrl.update(tel->heading());
                    m_altitudeCtrl.update(tel->altitudeAgl());
                    m_speedCtrl.update(tel->airspeed());
                    sendGuidedCommands(targetBearing, position);
                }
            }
        } else {
            // Flying to waypoint — zone avoidance + tangent approach
            double avoidAlt = std::min(tel->altitudeAgl(), wp->altitude);
            auto avoidTarget = getAvoidanceTarget(position, *wp, avoidAlt);

            if (avoidTarget) {
                auto [avLat, avLon] = *avoidTarget;
                targetBearing = nav::bearingTo(position.lat, position.lon, avLat, avLon);
                SPDLOG_INFO("AVOIDANCE ACTIVE: bearing={:.0f} to ({:.5f},{:.5f}) instead of WP{}",
                            targetBearing, avLat, avLon, wp->id);
            } else if (hasOrbit && distToWp <= orbitRadius * 2) {
                // Tangent approach
                if (m_tangentApproachWpId != wp->id) {
                    m_orbitCcw = chooseOrbitDirectionCcw(position, wp->lat, wp->lon, tel->heading());
                    m_tangentApproachWpId = wp->id;
                }
                double bearingFromWp = nav::bearingTo(wp->lat, wp->lon, position.lat, position.lon);
                double angleDeg = std::acos(std::min(orbitRadius / distToWp, 1.0)) * 180.0 / M_PI;
                double tangentBearing;
                if (m_orbitCcw) {
                    tangentBearing = std::fmod(bearingFromWp - angleDeg + 360.0, 360.0);
                } else {
                    tangentBearing = std::fmod(bearingFromWp + angleDeg, 360.0);
                }
                auto [tgtLat, tgtLon] = nav::projectPoint(wp->lat, wp->lon, tangentBearing, orbitRadius);
                targetBearing = nav::bearingTo(position.lat, position.lon, tgtLat, tgtLon);
            } else {
                targetBearing = nav::bearingTo(position.lat, position.lon, wp->lat, wp->lon);
            }

            m_headingCtrl.setTargetHeading(targetBearing);
            m_headingCtrl.update(tel->heading());
            m_altitudeCtrl.update(tel->altitudeAgl());
            m_speedCtrl.update(tel->airspeed());
            sendGuidedCommands(targetBearing, position);
        }
    }

    // Diagnostic logging every ~2 seconds
    m_logCounter++;
    if (m_logCounter >= 20) {
        m_logCounter = 0;
        SPDLOG_INFO("GUIDED: hdg={:.0f}->tgt={:.0f} err={:.1f} | alt={:.1f} tgt={:.1f} err={:.1f} | "
                    "spd={:.1f} tgt={:.1f} | mode={} gps={} armed={}",
                    tel->heading(), targetBearing, m_headingCtrl.error(),
                    tel->altitudeAgl(), m_altitudeCtrl.target(), m_altitudeCtrl.error(),
                    tel->airspeed(), m_speedCtrl.target(),
                    tel->mode().toStdString(), tel->gpsFix(), tel->armed());
    }

    // Use fresh wall time — not the stale currentTime captured at the start.
    // Blocking MAVSDK calls (setParam) can stall update() for seconds;
    // using the stale value would cause false timeout on the next tick.
    m_lastUpdateTime = nowSec();
}

// ─────────────────────── GUIDED commands ───────────────────────

void AutopilotManager::sendGuidedCommands(double targetBearing, const LatLon& position)
{
    double targetAlt = m_altitudeCtrl.target();
    double altError = std::abs(m_altitudeCtrl.error());
    double now = nowSec();

    double projDistance;
    if (altError > 5.0) {
        // Throttle sends for TECS altitude convergence
        if (now - m_guidedSendTime < GUIDED_RESEND_INTERVAL) return;
        projDistance = std::clamp(altError * 5.0, 200.0, 500.0);
    } else {
        projDistance = 2000.0;
    }

    auto [tgtLat, tgtLon] = nav::projectPoint(position.lat, position.lon, targetBearing, projDistance);
    m_connection.sendGuidedTarget(tgtLat, tgtLon, targetAlt);
    m_connection.sendSpeed(m_speedCtrl.target());
    m_guidedSendTime = now;
}

// ─────────────────────── stick override ───────────────────────

bool AutopilotManager::checkStickOverride(const std::array<int, 8>& rc)
{
    if (rc.size() < 4) return false;

    int threshold = m_config.stick_threshold;
    int roll = rc[CH_ROLL];
    int pitch = rc[CH_PITCH];
    int throttle = rc[CH_THROTTLE];
    int yaw = rc[CH_YAW];

    // Validate PWM range — disconnected receiver sends 0 or 65535
    for (int ch : {roll, pitch, throttle, yaw}) {
        if (ch < 800 || ch > 2200) return false;
    }

    int rollDiff = std::abs(roll - PWM_CENTER);
    int pitchDiff = std::abs(pitch - PWM_CENTER);
    int yawDiff = std::abs(yaw - PWM_CENTER);
    int throttleDiff = std::abs(throttle - m_throttleBaseline);

    bool overrideDetected = rollDiff > threshold || pitchDiff > threshold || yawDiff > threshold;

    // Throttle: skip during LOITER (THROTTLE_NUDGE=0 means ArduPilot ignores it)
    if (!m_isOrbiting) {
        overrideDetected = overrideDetected || throttleDiff > threshold;
    }

    if (overrideDetected) {
        m_stickOverrideCount++;
        SPDLOG_DEBUG("RC: roll={}({}) pitch={}({}) yaw={}({}) thr={}({}) count={}/{}",
                     roll, rollDiff, pitch, pitchDiff, yaw, yawDiff,
                     throttle, throttleDiff, m_stickOverrideCount, STICK_OVERRIDE_THRESHOLD);
        if (m_stickOverrideCount >= STICK_OVERRIDE_THRESHOLD) {
            SPDLOG_INFO("STICK OVERRIDE CONFIRMED: roll={} pitch={} yaw={} thr={} (baseline={}), count={}",
                        roll, pitch, yaw, throttle, m_throttleBaseline, m_stickOverrideCount);
            return true;
        }
    } else {
        if (m_stickOverrideCount > 0) {
            SPDLOG_DEBUG("RC: roll={} pitch={} yaw={} thr={} - reset count from {}",
                         roll, pitch, yaw, throttle, m_stickOverrideCount);
        }
        m_stickOverrideCount = 0;
    }
    return false;
}

// ─────────────────────── orbit management ───────────────────────

void AutopilotManager::exitOrbit()
{
    if (!m_isOrbiting) return;

    m_isOrbiting = false;
    m_orbitTurnsCompleted = 0;
    m_orbitHeadingAccumulated = 0.0;
    m_loiterAltTransition = false;

    if (m_savedAirspeedCruise.has_value()) {
        m_connection.setCruiseAirspeed(m_savedAirspeedCruise.value());
        SPDLOG_INFO("RESTORED AIRSPEED_CRUISE={:.1f}", m_savedAirspeedCruise.value());
        m_savedAirspeedCruise.reset();
    }
    m_connection.setParamInt("THROTTLE_NUDGE", 1);

    // Force re-enter GUIDED to clear DO_REPOSITION orbit radius
    m_connection.setMode("FBWA");
    m_connection.setMode("GUIDED");
    m_orbitRepositionSent = false;
    SPDLOG_INFO("EXITED ORBIT -> GUIDED");
}

void AutopilotManager::startOrbit(Waypoint& wp, double currentHeading)
{
    m_isOrbiting = true;
    m_orbitTurnsCompleted = 0;
    m_orbitLastHeading = currentHeading;
    m_orbitHeadingAccumulated = 0.0;
    m_orbitAdvanceHandled = false;

    double radius = wp.orbit_radius > 0 ? wp.orbit_radius : 150.0;

    auto* tel = m_connection.telemetry();

    // Choose orbit direction — skip if already pre-chosen by tangent approach
    if (m_tangentApproachWpId == wp.id) {
        m_tangentApproachWpId = -1;  // consumed
    } else {
        LatLon pos = tel->position();
        if (pos.isValid()) {
            m_orbitCcw = chooseOrbitDirectionCcw(pos, wp.lat, wp.lon, currentHeading);
        } else {
            m_orbitCcw = false;
        }
    }

    double targetSpeed = m_speedCtrl.target();
    if (!m_savedAirspeedCruise.has_value()) {
        m_savedAirspeedCruise = tel->airspeed() > 0 ? tel->airspeed() : 25.0;
        SPDLOG_INFO("SAVED AIRSPEED_CRUISE={:.1f} for restore", m_savedAirspeedCruise.value());
    }
    m_connection.setCruiseAirspeed(targetSpeed);
    m_connection.sendSpeed(targetSpeed);
    m_connection.setParamInt("THROTTLE_NUDGE", 0);
    m_connection.setParamInt("WP_LOITER_RAD", static_cast<int>(radius));
    m_connection.sendLoiterUnlim(wp.lat, wp.lon, wp.altitude, radius, m_orbitCcw);
    m_orbitRepositionSent = true;
    SPDLOG_INFO("START ORBIT (DO_REPOSITION) at WP{}: radius={}, speed={}, {}",
                wp.id, radius, targetSpeed, m_orbitCcw ? "CCW" : "CW");
}

bool AutopilotManager::chooseOrbitDirectionCcw(const LatLon& position,
                                                 double centerLat, double centerLon,
                                                 double heading)
{
    double bearingToCenter = nav::bearingTo(position.lat, position.lon, centerLat, centerLon);
    double relative = std::fmod(bearingToCenter - heading + 360.0, 360.0);
    bool ccw = relative >= 180.0;
    SPDLOG_INFO("ORBIT DIRECTION: bearing_to_center={:.0f} heading={:.0f} relative={:.0f} -> {}",
                bearingToCenter, heading, relative, ccw ? "CCW" : "CW");
    return ccw;
}

void AutopilotManager::updateOrbitProgress(double currentHeading)
{
    double headingDiff = currentHeading - m_orbitLastHeading;
    if (headingDiff > 180) headingDiff -= 360;
    else if (headingDiff < -180) headingDiff += 360;

    m_orbitHeadingAccumulated += headingDiff;
    m_orbitLastHeading = currentHeading;

    int newTurns = static_cast<int>(std::abs(m_orbitHeadingAccumulated) / 360.0);
    if (newTurns > m_orbitTurnsCompleted) {
        m_orbitTurnsCompleted = newTurns;
        SPDLOG_INFO("ORBIT TURN {} completed", m_orbitTurnsCompleted);
    }
}

void AutopilotManager::finishOrbitAndAdvance(Waypoint& oldWp)
{
    int oldIdx = m_routePlanner->activeWaypointIndex();
    m_routePlanner->nextWaypoint();
    int newIdx = m_routePlanner->activeWaypointIndex();
    Waypoint* newWp = m_routePlanner->activeWaypoint();

    if (newIdx != oldIdx && newWp) {
        SPDLOG_INFO("ORBIT COMPLETE: {} turns at WP{} -> advancing to WP{}",
                    m_orbitTurnsCompleted, oldWp.id, newWp->id);
        exitOrbit();
        m_activeWaypointId = newWp->id;
        m_waitingForAltitude = false;

        auto* tel = m_connection.telemetry();
        if (newWp->climb_enroute) {
            m_altitudeCtrl.setTargetAltitude(newWp->altitude);
        } else {
            m_altitudeCtrl.setTargetAltitude(tel->altitudeAgl());
        }

        emit waypointReached(oldWp.id, newWp->id);
    } else {
        // Last waypoint — revert next_waypoint() clamping
        m_routePlanner->setActiveWaypoint(oldIdx);

        if (m_homePosition) {
            SPDLOG_INFO("ORBIT COMPLETE: {} turns at last WP{} -> returning home",
                        m_orbitTurnsCompleted, oldWp.id);
            exitOrbit();
            handleRouteCompletion();
        } else {
            SPDLOG_INFO("ORBIT COMPLETE: {} turns at last WP{}, no home — continuing orbit",
                        m_orbitTurnsCompleted, oldWp.id);
        }
    }
}

// ─────────────────────── route completion ───────────────────────

void AutopilotManager::handleRouteCompletion()
{
    if (m_homePosition) {
        SPDLOG_INFO("ROUTE COMPLETE -> RETURNING HOME");
        m_returningHome = true;
        m_waitingForAltitude = false;
        m_guidedSendTime = 0.0;

        auto* tel = m_connection.telemetry();
        m_altitudeCtrl.setTargetAltitude(tel->altitudeAgl());

        LatLon pos = tel->position();
        if (pos.isValid()) {
            m_orbitCcw = chooseOrbitDirectionCcw(pos, m_homePosition->lat, m_homePosition->lon, tel->heading());
            double initBearing = nav::bearingTo(pos.lat, pos.lon, m_homePosition->lat, m_homePosition->lon);
            auto [tgtLat, tgtLon] = nav::projectPoint(pos.lat, pos.lon, initBearing, 2000.0);
            m_connection.sendGuidedTarget(tgtLat, tgtLon, tel->altitudeAgl());
        }
    } else {
        SPDLOG_INFO("ROUTE COMPLETE -> ORBITING INDEFINITELY (no home position)");
        if (m_routePlanner) {
            int wpCount = m_routePlanner->waypointCount();
            if (wpCount > 0) {
                m_routePlanner->setActiveWaypoint(wpCount - 1);
                Waypoint* wp = m_routePlanner->activeWaypoint();
                if (wp && !m_isOrbiting) {
                    startOrbit(*wp, m_connection.telemetry()->heading());
                    SPDLOG_INFO("INFINITE ORBIT started at WP{}", wp->id);
                }
            }
        }
    }
}

// ─────────────────────── zone avoidance ───────────────────────

std::optional<std::tuple<double, double>> AutopilotManager::getAvoidanceTarget(
    const LatLon& position, const Waypoint& wp, double altitude)
{
    if (!m_zoneChecker || !m_pathPlanner) return std::nullopt;

    // Check for completed background computation
    if (m_avoidancePending.hasResult) {
        auto path = std::move(m_avoidancePending.path);
        int forWpId = m_avoidancePending.wpId;
        bool isNull = m_avoidancePending.isNull;
        m_avoidancePending.hasResult = false;
        m_avoidanceComputing = false;

        if (forWpId == m_avoidanceForWpId) {
            if (!isNull && !path.empty()) {
                m_avoidanceWaypoints = std::move(path);
                SPDLOG_WARN("AVOIDANCE: {} intermediate points to WP{}",
                            m_avoidanceWaypoints.size(), forWpId);
            } else if (isNull) {
                m_avoidanceGaveUp = {forWpId, position.lat, position.lon};
                SPDLOG_WARN("AVOIDANCE: no path found to WP{}, flying direct (will retry after {:.0f}m)",
                            forWpId, AVOIDANCE_RETRY_DISTANCE);
                if (m_avoidanceNotifiedWpId != forWpId) {
                    m_avoidanceNotifiedWpId = forWpId;
                    emit avoidanceFailed(QString("Обход не найден — полёт напрямую"));
                }
            }
        }
    }

    // Clear suppression if WP changed
    if (m_avoidanceSuppressedWpId != -1 && m_avoidanceSuppressedWpId != wp.id) {
        SPDLOG_INFO("AVOIDANCE: suppression cleared (WP changed from {} to {})",
                     m_avoidanceSuppressedWpId, wp.id);
        m_avoidanceSuppressedWpId = -1;
    }

    // Already tried and no path found — retry after moving far enough or periodic recheck
    double now = nowSec();
    bool needsRecheck = (now - m_avoidanceLastCheckTime >= AVOIDANCE_RECHECK_INTERVAL)
                        && !m_avoidanceComputing.load()
                        && m_avoidanceSuppressedWpId != wp.id;

    if (m_avoidanceGaveUp.wpId == wp.id && !needsRecheck) {
        double movedDist = nav::haversineDistance(position.lat, position.lon,
                                                   m_avoidanceGaveUp.lat, m_avoidanceGaveUp.lon);
        if (movedDist < AVOIDANCE_RETRY_DISTANCE) return std::nullopt;
        SPDLOG_INFO("AVOIDANCE: retrying for WP{} — moved {:.0f}m from gaveUp position", wp.id, movedDist);
        m_avoidanceGaveUp = {};
        m_avoidanceForWpId = -1;  // force re-computation
    }

    // Compute avoidance if not yet done for this wp, or periodic recheck
    if (m_avoidanceForWpId != wp.id || needsRecheck) {
        const char* reason = (m_avoidanceForWpId != wp.id) ? "new WP" : "periodic recheck";
        m_avoidanceForWpId = wp.id;
        m_avoidanceComputing = false;
        m_avoidanceGaveUp = {};
        m_avoidanceLastCheckTime = now;

        bool intersects = m_zoneChecker->segmentIntersectsObstacles(
            position.lat, position.lon, wp.lat, wp.lon, altitude);
        SPDLOG_WARN("AVOIDANCE: {} WP{}, alt={:.0f}, pos=({:.5f},{:.5f})->({:.5f},{:.5f}), intersects={}",
                    reason, wp.id, altitude, position.lat, position.lon, wp.lat, wp.lon, intersects);

        if (intersects) {
            // Start background computation — clear old waypoints
            m_avoidanceWaypoints.clear();
            m_avoidanceWpIdx = 0;
            m_avoidanceComputing = true;
            startAvoidanceComputation(position.lat, position.lon, wp.lat, wp.lon, altitude, wp.id);
            return std::nullopt;
        } else {
            // Path is now clear — remove stale avoidance waypoints
            if (!m_avoidanceWaypoints.empty()) {
                SPDLOG_WARN("AVOIDANCE: path now CLEAR to WP{} — dropping old avoidance", wp.id);
            }
            m_avoidanceWaypoints.clear();
            m_avoidanceWpIdx = 0;
        }
    }

    if (m_avoidanceComputing.load()) return std::nullopt;
    if (m_avoidanceWaypoints.empty()) return std::nullopt;

    // Navigate through avoidance waypoints
    auto [tLat, tLon] = m_avoidanceWaypoints[m_avoidanceWpIdx];
    double dist = nav::haversineDistance(position.lat, position.lon, tLat, tLon);

    if (dist <= 150.0) {
        m_avoidanceWpIdx++;
        if (m_avoidanceWpIdx >= static_cast<int>(m_avoidanceWaypoints.size())) {
            m_avoidanceWaypoints.clear();
            m_avoidanceWpIdx = 0;
            SPDLOG_INFO("AVOIDANCE: all points passed for WP{}, flying direct", wp.id);
            return std::nullopt;
        }
        return m_avoidanceWaypoints[m_avoidanceWpIdx];
    }

    return m_avoidanceWaypoints[m_avoidanceWpIdx];
}

void AutopilotManager::startAvoidanceComputation(double startLat, double startLon,
                                                    double endLat, double endLon,
                                                    double altitude, int wpId)
{
    // Capture raw pointer — PathPlanner is long-lived, owned externally
    PathPlanner* planner = m_pathPlanner;
    QtConcurrent::run([this, planner, startLat, startLon, endLat, endLon, altitude, wpId]() {
        std::vector<std::tuple<double, double>> resultPath;
        bool isNull = false;
        try {
            auto result = planner->planPath(startLat, startLon, endLat, endLon, altitude);
            if (!result.has_value()) {
                isNull = true;
            } else {
                for (const auto& pt : *result)
                    resultPath.emplace_back(pt.lat, pt.lon);
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("AVOIDANCE computation error: {}", e.what());
            isNull = true;
        }

        // Post back to main thread
        QMetaObject::invokeMethod(this, [this, path = std::move(resultPath), wpId, isNull]() mutable {
            m_avoidancePending.path = std::move(path);
            m_avoidancePending.wpId = wpId;
            m_avoidancePending.isNull = isNull;
            m_avoidancePending.hasResult = true;
        }, Qt::QueuedConnection);
    });
}

// ─────────────────────── live adjustments ───────────────────────

void AutopilotManager::setTargetAltitude(double altitude)
{
    m_altitudeCtrl.setTargetAltitude(altitude);
    m_guidedSendTime = 0.0;  // force immediate resend
    if (m_routePlanner) {
        Waypoint* wp = m_routePlanner->activeWaypoint();
        if (wp) {
            wp->altitude = altitude;
            SPDLOG_INFO("TARGET ALTITUDE SET: {}m for WP{}", altitude, wp->id);
        }
    }
    if (m_mode != AutopilotMode::MANUAL) {
        m_connection.sendGuidedChangeAltitude(altitude);
    }
}

void AutopilotManager::setOrbitRadius(double radius)
{
    if (m_routePlanner) {
        Waypoint* wp = m_routePlanner->activeWaypoint();
        if (wp) {
            wp->orbit_radius = radius;
            if (m_isOrbiting) {
                m_connection.setParamInt("WP_LOITER_RAD", static_cast<int>(radius));
                m_orbitRepositionSent = false;
            }
            SPDLOG_INFO("ORBIT RADIUS SET: {}m for WP{}", radius, wp->id);
        }
    }
}

void AutopilotManager::setTargetAirspeed(double speed)
{
    m_speedCtrl.setTargetSpeed(speed);
    if (m_mode != AutopilotMode::MANUAL) {
        m_connection.sendSpeed(speed);
        if (m_isOrbiting) {
            m_connection.setCruiseAirspeed(speed);
        }
    }
    SPDLOG_INFO("TARGET AIRSPEED SET: {} m/s", speed);
}

// ─────────────────────── status ───────────────────────

AutopilotStatus AutopilotManager::status() const
{
    AutopilotStatus s;
    s.mode = m_mode == AutopilotMode::NAV ? "NAV" : "MANUAL";
    s.targetHeading = m_headingCtrl.target();
    s.headingError = m_headingCtrl.error();
    s.targetAltitude = m_altitudeCtrl.target();
    s.altitudeError = m_altitudeCtrl.error();
    s.targetAirspeed = m_speedCtrl.target();
    s.airspeedError = m_speedCtrl.error();
    s.lastUpdate = m_lastUpdateTime;
    s.disengageReason = m_disengageReason;
    s.isOrbiting = m_isOrbiting;
    s.orbitTurnsCompleted = m_orbitTurnsCompleted;
    s.returningHome = m_returningHome;
    s.operationalActive = m_operationalActive;
    if (m_operationalWp) {
        s.operationalMode = QString::fromLatin1(m_operationalWp->modeName());
    }

    auto altError = m_altitudeCtrl.error();
    QString vertAction;
    if (std::abs(altError) > 5.0) {
        vertAction = altError > 0 ? " (набор)" : " (снижение)";
    }

    if (m_operationalActive && m_operationalWp) {
        const auto& opWp = m_operationalWp->waypoint;
        if (m_isOrbiting) {
            s.orbitRadius = opWp.orbit_radius;
            if (opWp.action == "ORBIT_TURNS") {
                s.action = QString("OP_ORBIT_%1/%2%3").arg(m_orbitTurnsCompleted).arg(opWp.orbit_turns).arg(vertAction);
            } else if (opWp.action == "ORBIT_INFINITE") {
                s.action = QString("OP_ORBIT_INF%1").arg(vertAction);
            } else if (opWp.action == "ALTITUDE") {
                s.action = QString("OP_ALTITUDE_ORBIT%1").arg(vertAction);
            } else {
                s.action = "OP_ORBITING";
            }
        } else {
            s.action = QString("OP_%1%2").arg(
                m_operationalWp->mode == OperationalMode::GOTO ? "GOTO" : "VIA",
                vertAction);
            s.orbitRadius = opWp.orbit_radius;
        }
    } else if (m_returningHome && m_homePosition) {
        if (m_isOrbiting) {
            s.action = QString("ВОЗВРАТ_ДОМОЙ_ОРБИТА%1").arg(vertAction);
        } else {
            s.action = QString("ВОЗВРАТ_ДОМОЙ%1").arg(vertAction);
        }
        s.orbitRadius = 150.0;
    } else if (m_mode == AutopilotMode::NAV && m_routePlanner) {
        const Waypoint* wp = m_routePlanner->activeWaypoint();
        if (wp) {
            if (m_isOrbiting) {
                s.orbitRadius = wp->orbit_radius;
                if (wp->action == "ORBIT_TURNS") {
                    s.action = QString("ORBIT_%1/%2%3").arg(m_orbitTurnsCompleted).arg(wp->orbit_turns).arg(vertAction);
                } else if (wp->action == "ORBIT_INFINITE") {
                    s.action = QString("ORBIT_INF%1").arg(vertAction);
                } else if (wp->action == "ALTITUDE") {
                    s.action = QString("ALTITUDE_ORBIT%1").arg(vertAction);
                } else {
                    s.action = "ORBITING";
                }
            } else if (m_waitingForAltitude) {
                s.action = QString("ОЖИДАНИЕ_ВЫСОТЫ%1").arg(vertAction);
                s.orbitRadius = wp->orbit_radius;
            } else {
                s.action = QString("TO_WAYPOINT%1").arg(vertAction);
                s.orbitRadius = wp->orbit_radius;
            }
        }
    }

    return s;
}

std::vector<std::tuple<double, double>> AutopilotManager::remainingAvoidanceWaypoints() const
{
    if (m_avoidanceWaypoints.empty()) return {};
    return std::vector<std::tuple<double, double>>(
        m_avoidanceWaypoints.begin() + m_avoidanceWpIdx, m_avoidanceWaypoints.end());
}

} // namespace vtol
