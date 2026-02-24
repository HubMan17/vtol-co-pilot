#pragma once

#include <QDialog>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>
#include <QListWidget>
#include <QStackedWidget>

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
        bool isOperational = false;
        QString operationalMode;  // "GOTO" or "VIA_POINT"
    };

    explicit WaypointDialog(double lat, double lon,
                            const ZoneChecker* zoneChecker = nullptr,
                            bool operational = false,
                            bool allowVia = true,
                            QWidget* parent = nullptr);

    Result result() const;

private:
    void setupUi();
    void onTypeChanged(int index);
    void checkZoneRestriction();

    double m_lat;
    double m_lon;
    const ZoneChecker* m_zoneChecker;
    bool m_operational;
    bool m_allowVia;

    QComboBox*  m_comboOpMode = nullptr;
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

// ─── SettingsDialog (unified, sidebar navigation) ───────────────────────
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    enum Page { PageMap = 0, PageZones = 1, PageData = 2, PageHome = 3, PageNotifications = 4, PageSystem = 5 };

    explicit SettingsDialog(const AppConfig& config,
                            Page initialPage = PageMap,
                            QWidget* parent = nullptr);

    AppConfig getConfig() const;

signals:
    void clearSettlementsRequested();
    void clearZonesRequested();
    void clearAllRequested();

private:
    void setupUi(Page initialPage);
    QWidget* createMapPage();
    QWidget* createZonesPage();
    QWidget* createDataPage();
    QWidget* createHomePage();
    QWidget* createNotificationsPage();
    QWidget* createSystemPage();
    void onSettlementModeChanged();

    AppConfig m_config;

    QListWidget*    m_sidebar = nullptr;
    QStackedWidget* m_stack   = nullptr;

    // Map page
    QSpinBox* m_spinTrackLength = nullptr;

    // Zones page
    QComboBox* m_comboSettlementMode     = nullptr;
    QSpinBox*  m_spinSettlementAltitude  = nullptr;
    QLabel*    m_lblSettlementAltitude   = nullptr;
    QSpinBox*  m_spinSettlementBuffer    = nullptr;
    QComboBox* m_comboNoflyMode          = nullptr;
    QSpinBox*  m_spinNoflyBuffer         = nullptr;

    // Home page
    QCheckBox* m_chkAutoFromDrone     = nullptr;
    QComboBox* m_comboOverwriteMode   = nullptr;
    QCheckBox* m_chkNotifyAutoSet     = nullptr;
    QCheckBox* m_chkNotifyNoHome      = nullptr;

    // Notifications page
    QSpinBox* m_spinInfoDuration     = nullptr;
    QSpinBox* m_spinWarningDuration  = nullptr;
    QSpinBox* m_spinCriticalDuration = nullptr;
    QSpinBox* m_spinCurtailPct       = nullptr;

    // System page
    QDoubleSpinBox* m_spinVMax           = nullptr;
    QDoubleSpinBox* m_spinVOpZero        = nullptr;
    QDoubleSpinBox* m_spinVAbsZero       = nullptr;
    QDoubleSpinBox* m_spinVCritical      = nullptr;
    QDoubleSpinBox* m_spinHysteresis     = nullptr;
    QDoubleSpinBox* m_spinAmpWarning     = nullptr;
    QComboBox*      m_comboActionLimit   = nullptr;
    QComboBox*      m_comboActionCritical = nullptr;

    // 50% decision flow
    QSpinBox*       m_spinBatt50MaxNotif      = nullptr;
    QSpinBox*       m_spinBatt50Timeout       = nullptr;
    QSpinBox*       m_spinNoHomeMaxWarnings   = nullptr;

    // Home orbit behavior
    QComboBox*      m_comboHomeOrbitAction    = nullptr;
    QSpinBox*       m_spinHomeDecisionReminder = nullptr;
};

} // namespace vtol
