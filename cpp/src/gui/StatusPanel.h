#pragma once

#include <QWidget>
#include <QLabel>
#include <QSpinBox>
#include <QPushButton>
#include <QFrame>
#include <QMap>
#include <QString>
#include "autopilot/AutopilotManager.h"

namespace vtol {

class TelemetryState;

/// Compact metric cell: small label + large colored value
class MetricCell : public QWidget {
    Q_OBJECT
public:
    MetricCell(const QString& label, const QString& unit = "",
               const QString& color = "#FFFFFF", QWidget* parent = nullptr);
    void setValue(double value, int decimals = 1);
    void setText(const QString& text);
    void setColor(const QString& color);
    QLabel* valueLabel() const { return m_valueLabel; }
private:
    QString m_unit;
    QString m_color;
    QLabel* m_nameLabel;
    QLabel* m_valueLabel;
};

class StatusPanel : public QWidget {
    Q_OBJECT
public:
    explicit StatusPanel(QWidget* parent = nullptr);

    void updateTelemetry(const TelemetryState& state);
    void updateNavigation(int waypointIdx, int totalWaypoints,
                           double distance, double etaSeconds, double xtk);
    void updateAutopilot(const QString& mode, const AutopilotStatus& status);

signals:
    void orbitRadiusChanged(int radius);
    void targetAltitudeChanged(int altitude);
    void targetAirspeedChanged(int speed);
    void windOverrideRequested(int directionDeg, int speedMs);

private:
    void setupUi();
    static QFrame* metricRow(std::initializer_list<QWidget*> cells);
    static QString formatAction(const QString& action);

    // Telemetry cells
    MetricCell* m_airspeed;
    MetricCell* m_groundspeed;
    MetricCell* m_altitudeAgl;
    QLabel* m_windValue;
    MetricCell* m_battery;
    MetricCell* m_gps;

    // Navigation cells
    MetricCell* m_waypoint;
    MetricCell* m_distance;
    MetricCell* m_eta;
    MetricCell* m_xtk;

    // Autopilot
    QLabel* m_apMode;
    QLabel* m_apAction;
    QLabel* m_apTarget;
    QLabel* m_apError;
    QLabel* m_apAltError;

    // Controls
    QSpinBox* m_spinTargetAlt;
    QSpinBox* m_spinOrbitRadius;
    QSpinBox* m_spinTargetSpeed;
    QPushButton* m_btnSetAlt;
    QPushButton* m_btnSetRadius;
    QPushButton* m_btnSetSpeed;

    // Wind popup
    QFrame* m_windPopup;
    QPushButton* m_btnWindDropdown;
    QSpinBox* m_spinWindDir;
    QSpinBox* m_spinWindSpeed;
    QPushButton* m_btnSetWind;

    // Manual override flags
    bool m_manualAltOverride = false;
    bool m_manualRadiusOverride = false;
    bool m_manualSpeedOverride = false;

    // Cached mode to avoid setStyleSheet every tick
    QString m_lastApMode;
    bool m_lastOrbitState = false;
};

} // namespace vtol
