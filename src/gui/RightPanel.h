#pragma once

#include <QWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QFrame>
#include <QScrollArea>
#include <QTimer>
#include <QVariantMap>
#include <QVector>
#include <optional>

#include "autopilot/AutopilotManager.h"
#include "gui/NotificationWidget.h"

namespace vtol {

class TelemetryState;

// ─── PanelCell ─── compact metric display cell ─────────────────────────────
class PanelCell : public QWidget {
    Q_OBJECT
public:
    PanelCell(const QString& label, const QString& unit = "",
              const QString& color = "#e8ecf4", QWidget* parent = nullptr);
    void setValue(double value, int decimals = 1);
    void setText(const QString& text);
    void setColor(const QString& color);
private:
    QString m_unit;
    QString m_color;
    QLabel* m_labelWidget  = nullptr;
    QLabel* m_valueLabel   = nullptr;
};

// ─── RightPanel ─── unified right sidebar ──────────────────────────────────
class RightPanel : public QWidget {
    Q_OBJECT
public:
    explicit RightPanel(QWidget* parent = nullptr);

    // ── Data updates ──────────────────────────────────────────────────────
    void updateTelemetry(const TelemetryState& state);
    void updateNavigation(int wpIdx, int total,
                          double distance, double etaSec, double xtk);
    void updateAutopilot(const QString& mode, const AutopilotStatus& status);
    void refreshRoute(const QVector<QVariantMap>& waypoints, int activeIdx);

    // ── Header / connection state ─────────────────────────────────────────
    void setConnected(bool connected);
    void setFlightMode(const QString& mode);   // GUIDED, MANUAL, AUTO, etc.

    // ── Button states ─────────────────────────────────────────────────────
    void setNavActive(bool active);
    void setFollowActive(bool active);
    void setHomeActive(bool active);
    void setResumeRouteVisible(bool visible, bool hasRoute = true);
    void setPositionMode(bool active);
    void setHomePlacementMode(bool active);
    void setDrawZoneMode(bool active);
    void setAddWaypointMode(bool active);
    void enableControls(bool enabled);

    // ── Waypoint nav spinbox ──────────────────────────────────────────────
    void setWaypointRange(int minVal, int maxVal);
    void setWaypointValue(int value);

    // ── Operational point ─────────────────────────────────────────────────
    void setOperationalPoint(bool active, const QString& desc = {});

    // ── Notifications (pass-through) ──────────────────────────────────────
    NotificationManager* notificationManager() const { return m_notifManager; }

signals:
    // Connection
    void connectRequested();
    void disconnectRequested();

    // Autopilot / Navigation
    void navToggled(bool active);
    void homeToggled(bool active);
    void followToggled(bool active);
    void resumeRouteRequested();

    // Map operations
    void setPositionToggled(bool active);
    void setHomeToggled(bool active);
    void clearTrackRequested();
    void drawZoneToggled(bool active);
    void settingsRequested();
    void addWaypointToggled(bool active);

    // Waypoint navigation row
    void wpPrevRequested();
    void wpNextRequested();
    void wpSelected(int value);

    // Route planner
    void editWaypointRequested(int idx);
    void deleteWaypointRequested(int idx);
    void reorderWaypointRequested(int from, int to);
    void centerOnWaypointRequested(int idx);
    void clearRouteRequested();
    void loadRouteRequested();
    void saveRouteRequested();

    // StatusPanel params
    void orbitRadiusChanged(int r);
    void targetAltitudeChanged(int a);
    void targetAirspeedChanged(int s);
    void windOverrideRequested(int dir, int spd);

private:
    // ── UI construction helpers ───────────────────────────────────────────
    void setupUi();
    QWidget* buildHeader();
    QWidget* buildTabBar();
    QWidget* buildMainTab();
    QWidget* buildRouteTab();
    QWidget* buildSystemTab();
    QWidget* buildCommandArea();
    QWidget* buildBottomNav();

    // Reusable block/cell helpers
    QFrame*  makeBlock(const QString& icon, const QString& title,
                       QWidget* content, QLabel** badgeOut = nullptr);
    QFrame*  makeDataRow(std::initializer_list<QWidget*> cells);
    QFrame*  makeSeparator();
    QWidget* makeScrollPage(QWidget* content);

    // Autopilot section content (switched on mode change)
    void rebuildAutopilotContent(bool navActive);

    // Tab switching
    void switchTab(int index);

    // Wind popup
    void setupWindPopup();

    // Route tab helpers
    void rebuildWpList();
    void selectWpItem(int idx);
    QString wpActionLabel(const QString& action, int turns) const;

    // ── Header widgets ────────────────────────────────────────────────────
    QPushButton* m_btnConnect      = nullptr;
    QPushButton* m_btnDisconnect   = nullptr;
    QLabel*      m_lblMode         = nullptr;
    QFrame*      m_statusDot       = nullptr;

    // ── Tab bar ───────────────────────────────────────────────────────────
    QPushButton* m_tabBtns[3]      = {};
    QFrame*      m_tabIndicators[3] = {};
    QLabel*      m_routeBadge      = nullptr;
    QStackedWidget* m_stack        = nullptr;
    int          m_activeTab       = 0;

    // ── Telemetry cells ───────────────────────────────────────────────────
    PanelCell* m_airspeed    = nullptr;
    PanelCell* m_groundspeed = nullptr;
    PanelCell* m_altAgl      = nullptr;
    QLabel*    m_wind        = nullptr;   // direct label (no PanelCell wrapper)
    PanelCell* m_battery     = nullptr;
    PanelCell* m_gps         = nullptr;

    // Wind popup
    QFrame*      m_windPopup    = nullptr;
    QPushButton* m_btnWindDrop  = nullptr;
    QSpinBox*    m_spinWindDir  = nullptr;
    QSpinBox*    m_spinWindSpd  = nullptr;

    // ── Navigation cells ─────────────────────────────────────────────────
    PanelCell* m_wpCell   = nullptr;
    PanelCell* m_distCell = nullptr;
    PanelCell* m_etaCell  = nullptr;
    PanelCell* m_xtkCell  = nullptr;

    // ── Autopilot block ───────────────────────────────────────────────────
    QLabel*  m_apBadge     = nullptr;
    QWidget* m_apContent   = nullptr;   // replaced on mode change
    QLabel*  m_apModeLine  = nullptr;
    QLabel*  m_apAction    = nullptr;
    QLabel*  m_apTarget    = nullptr;
    QLabel*  m_apError     = nullptr;
    QLabel*  m_apAltError  = nullptr;
    QSpinBox*    m_spinAlt    = nullptr;
    QSpinBox*    m_spinRadius = nullptr;
    QSpinBox*    m_spinSpeed  = nullptr;
    QPushButton* m_btnSetAlt    = nullptr;
    QPushButton* m_btnSetRadius = nullptr;
    QPushButton* m_btnSetSpeed  = nullptr;
    QVBoxLayout* m_apContentLay = nullptr;  // layout of autopilot block body

    bool     m_lastApNav     = false;
    QString  m_lastApMode;
    bool     m_manualAlt     = false;
    bool     m_manualRadius  = false;
    bool     m_manualSpeed   = false;

    // ── Route tab ─────────────────────────────────────────────────────────
    QWidget*     m_wpListWidget    = nullptr;
    QVBoxLayout* m_wpListLay       = nullptr;
    QLabel*      m_wpEmptyLabel    = nullptr;
    QPushButton* m_btnRpAdd        = nullptr;
    QPushButton* m_btnRpEdit       = nullptr;
    QPushButton* m_btnRpDelete     = nullptr;
    int          m_selWpIdx        = -1;
    QVector<QVariantMap> m_waypoints;
    int          m_activeWpIdx     = -1;
    QVector<QWidget*> m_wpItems;

    // ── System tab ────────────────────────────────────────────────────────
    QFrame*  m_battFill        = nullptr;
    QLabel*  m_battVoltLabel   = nullptr;
    QLabel*  m_battPctLabel    = nullptr;
    QLabel*  m_battTimeLabel   = nullptr;
    QLabel*  m_battCurrLabel   = nullptr;
    QLabel*  m_battConsumed    = nullptr;
    QLabel*  m_battSufficient  = nullptr;
    QLabel*  m_motorVals[5]    = {};

    // ── Command area ──────────────────────────────────────────────────────
    NotificationWidget*  m_notifWidget  = nullptr;
    NotificationManager* m_notifManager = nullptr;
    QFrame*  m_opPointCard = nullptr;
    QLabel*  m_opPointDesc = nullptr;
    QPushButton* m_btnNav         = nullptr;
    QPushButton* m_btnResumeRoute = nullptr;

    // ── Bottom nav ────────────────────────────────────────────────────────
    QPushButton* m_btnFollow    = nullptr;
    QPushButton* m_btnHome      = nullptr;
    QPushButton* m_btnClearTrack = nullptr;
    QPushButton* m_btnSetPos    = nullptr;
    QPushButton* m_btnSetHome   = nullptr;
    QPushButton* m_btnLoadRoute = nullptr;
    QPushButton* m_btnDrawZone  = nullptr;
    QPushButton* m_btnSettings  = nullptr;
    QPushButton* m_btnAddWp     = nullptr;
    QSpinBox*    m_spinWp       = nullptr;
    QPushButton* m_btnWpPrev    = nullptr;
    QPushButton* m_btnWpNext    = nullptr;
    QLabel*      m_lblWpInfo    = nullptr;
};

} // namespace vtol
