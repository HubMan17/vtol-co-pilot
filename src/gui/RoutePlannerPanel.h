#pragma once

#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QVariantMap>
#include <QVector>

namespace vtol {

class ZoneChecker;

/// Collapsible route planning panel.
/// Shows a list of all waypoints with type, altitude and state.
/// Provides actions: add (placement mode), edit, delete, clear, load, save.
class RoutePlannerPanel : public QWidget {
    Q_OBJECT
public:
    explicit RoutePlannerPanel(QWidget* parent = nullptr);

    /// Refresh from current route data.
    /// @param waypoints  QVariantMap list with keys: lat, lon, altitude, action, orbitTurns, wpIndex
    /// @param activeIdx  0-based index of the currently active (navigating to) waypoint
    void refresh(const QVector<QVariantMap>& waypoints, int activeIdx);

    /// Collapse / expand the panel content
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

signals:
    /// User clicked "Добавить точку" — host should activate placement mode on the map
    void addWaypointRequested();

    /// User wants to edit a waypoint (open WaypointDialog with existing data)
    void editWaypointRequested(int wpIndex);

    /// User wants to delete a waypoint
    void deleteWaypointRequested(int wpIndex);

    /// User wants to reorder: move wpIndex to newIndex
    void reorderWaypointRequested(int wpIndex, int newIndex);

    /// User clicked on a waypoint row — center map on it
    void centerOnWaypointRequested(int wpIndex);

    /// User pressed "Очистить маршрут"
    void clearRouteRequested();

    /// User pressed "Загрузить маршрут"
    void loadRouteRequested();

    /// User pressed "Сохранить маршрут"
    void saveRouteRequested();

private slots:
    void onItemDoubleClicked(QListWidgetItem* item);
    void onSelectionChanged();
    void onEditClicked();
    void onDeleteClicked();

private:
    QWidget*      m_content   = nullptr;   // collapsible content
    QListWidget*  m_list      = nullptr;
    QPushButton*  m_btnToggle = nullptr;   // collapse/expand
    QPushButton*  m_btnAdd    = nullptr;
    QPushButton*  m_btnEdit   = nullptr;
    QPushButton*  m_btnDelete = nullptr;
    QLabel*       m_lblTitle  = nullptr;
    bool          m_expanded  = true;

    static QString actionLabel(const QString& action, int orbitTurns);
};

} // namespace vtol
