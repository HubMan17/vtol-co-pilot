#pragma once

#include <QDialog>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>

#include "core/Config.h"
#include "navigation/ZoneManager.h"

namespace vtol {

class ZoneChecker;

// ─── WaypointDialog ─────────────────────────────────────────────────────
class WaypointDialog : public QDialog {
    Q_OBJECT
public:
    struct Result {
        double lat = 0;
        double lon = 0;
        int altitude = 100;
        QString action = "FLYTHROUGH";
        int radius = 150;
        bool climbEnroute = false;
        int orbitRadius = 150;
        int orbitTurns = 1;
    };

    explicit WaypointDialog(double lat, double lon,
                            const ZoneChecker* zoneChecker = nullptr,
                            QWidget* parent = nullptr);

    Result result() const;

private:
    void setupUi();
    void onTypeChanged(int index);
    void checkZoneRestriction();

    double m_lat;
    double m_lon;
    const ZoneChecker* m_zoneChecker;

    QSpinBox*   m_spinAltitude;
    QCheckBox*  m_chkClimbEnroute;
    QComboBox*  m_comboType;
    QSpinBox*   m_spinRadius;
    QGroupBox*  m_orbitGroup;
    QSpinBox*   m_spinOrbitRadius;
    QSpinBox*   m_spinOrbitTurns;
    QLabel*     m_lblZoneWarning;
    QPushButton* m_btnOk;
};

// ─── ZonePropertiesDialog ───────────────────────────────────────────────
class ZonePropertiesDialog : public QDialog {
    Q_OBJECT
public:
    static constexpr int DELETE_REQUESTED = 1001;

    struct Result {
        QString name;
        QString description;
        double altitude = 0;       // 0 = no limit
        QString avoidMode;         // empty = default
        double buffer = 0;         // 0 = default
    };

    explicit ZonePropertiesDialog(const NoFlyZone* zone = nullptr,
                                   QWidget* parent = nullptr);

    Result result() const;
    bool isDeleteRequested() const { return m_deleteRequested; }

private:
    void setupUi();

    const NoFlyZone* m_zone;
    bool m_deleteRequested = false;

    QLineEdit*  m_editName;
    QTextEdit*  m_editDescription;
    QSpinBox*   m_spinAltitude;
    QComboBox*  m_comboAvoidMode;
    QSpinBox*   m_spinBuffer;
    QPushButton* m_btnOk;
};

// ─── ZoneSettingsDialog ─────────────────────────────────────────────────
class ZoneSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ZoneSettingsDialog(const ZoneAvoidanceConfig& config,
                                QWidget* parent = nullptr);

    ZoneAvoidanceConfig getConfig() const;

private:
    void setupUi();
    void onSettlementModeChanged();

    ZoneAvoidanceConfig m_config;

    QComboBox*  m_comboSettlementMode;
    QSpinBox*   m_spinSettlementAltitude;
    QLabel*     m_lblSettlementAltitude;
    QSpinBox*   m_spinSettlementBuffer;
    QComboBox*  m_comboNoflyMode;
    QSpinBox*   m_spinNoflyBuffer;
};

// ─── SettingsDialog ─────────────────────────────────────────────────────
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const GuiConfig& config,
                            QWidget* parent = nullptr);

    GuiConfig getGuiConfig() const;

private:
    void setupUi();

    GuiConfig m_config;
    QSpinBox* m_spinTrackLength;
};

} // namespace vtol
