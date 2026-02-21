#pragma once

#include <QAbstractListModel>
#include <QVariant>
#include <QVector>
#include <QString>
#include <QSet>
#include <cmath>
#include <vector>
#include <tuple>

namespace vtol {

// ═══════════════════════════════════════════════════════════
//  WaypointListModel
// ═══════════════════════════════════════════════════════════

class WaypointListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        LatRole = Qt::UserRole + 1, LonRole, AltRole,
        ActionRole, WpIndexRole, WpStateRole
    };

    struct Item { double lat, lon, altitude; QString action, state; };

    explicit WaypointListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return static_cast<int>(m_items.size()); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setWaypoints(const QVector<QVariantMap>& waypoints, int activeIdx);
    void updateActive(int activeIdx);

    const auto& items() const { return m_items; }

private:
    QVector<Item> m_items;
};

// ═══════════════════════════════════════════════════════════
//  RouteSegmentModel
// ═══════════════════════════════════════════════════════════

class RouteSegmentModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { FromLatRole = Qt::UserRole + 1, FromLonRole, ToLatRole, ToLonRole, SegStateRole };

    struct Item { double fromLat, fromLon, toLat, toLon; QString state; };

    explicit RouteSegmentModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return static_cast<int>(m_items.size()); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setSegments(const QVector<QVariantMap>& waypoints, int activeIdx);

    const auto& items() const { return m_items; }

private:
    QVector<Item> m_items;
};

// ═══════════════════════════════════════════════════════════
//  ZoneListModel
// ═══════════════════════════════════════════════════════════

class ZoneListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { ZoneIdRole = Qt::UserRole + 1, ZoneCoordsRole, ZoneNameRole };

    struct Item { QString id; QVariantList points; QString name; };

    explicit ZoneListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return static_cast<int>(m_items.size()); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void addZone(const QString& id, const QVariantList& points, const QString& name = "");
    void removeZone(const QString& id);
    void updateZonePoints(const QString& id, const QVariantList& points);
    void clear();

    const auto& items() const { return m_items; }

private:
    QVector<Item> m_items;
};

// ═══════════════════════════════════════════════════════════
//  DashBorderModel (shared for zones and settlements)
// ═══════════════════════════════════════════════════════════

class DashBorderModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { FromLatRole = Qt::UserRole + 1, FromLonRole, ToLatRole, ToLonRole };

    DashBorderModel(double dashDeg, double gapDeg, int maxSegments = 0, QObject* parent = nullptr)
        : QAbstractListModel(parent), m_dashDeg(dashDeg), m_gapDeg(gapDeg), m_maxSegments(maxSegments) {}

    int rowCount(const QModelIndex& parent = {}) const override { return static_cast<int>(m_items.size()); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Rebuild from zone polygon data: zones[i]['points'] is QVariantList of [lat,lon] lists.
    void rebuildFromZones(const QVector<QVariantList>& polygons);
    /// Rebuild from raw coordinate lists: each entry is QVariantList of [lat,lon].
    void rebuildFromCoords(const QVector<QVariantList>& polygonCoords);

private:
    struct Dash { double fromLat, fromLon, toLat, toLon; };
    QVector<Dash> m_items;
    double m_dashDeg;
    double m_gapDeg;
    int m_maxSegments;

    void generateDashes(const QVector<std::tuple<double, double>>& polygon);
};

// ═══════════════════════════════════════════════════════════
//  SettlementPolyModel
// ═══════════════════════════════════════════════════════════

class SettlementPolyModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { CoordsRole = Qt::UserRole + 1 };

    explicit SettlementPolyModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return static_cast<int>(m_items.size()); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void addPolys(const QVariantList& features);
    void clear();

    static constexpr int MAX_ITEMS = 80;
    const auto& items() const { return m_items; }

private:
    QVector<QVariantList> m_items;
    QSet<QString> m_sigs;
    QVector<QString> m_sigByIndex;
};

// ═══════════════════════════════════════════════════════════
//  SettlementCircleModel
// ═══════════════════════════════════════════════════════════

class SettlementCircleModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { LatRole = Qt::UserRole + 1, LonRole, RadiusRole };

    struct Item { double lat, lon, radius; };

    explicit SettlementCircleModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return static_cast<int>(m_items.size()); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void addCircles(const QVariantList& nodes);
    void clear();

    static constexpr int MAX_ITEMS = 40;
    int itemCount() const { return static_cast<int>(m_items.size()); }

    const auto& items() const { return m_items; }

private:
    QVector<Item> m_items;
    QSet<QString> m_sigs;
    QVector<QString> m_sigByIndex;

    static double placeRadius(const QString& type);
};

// ═══════════════════════════════════════════════════════════
//  ConflictSegmentModel
// ═══════════════════════════════════════════════════════════

class ConflictSegmentModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { FromLatRole = Qt::UserRole + 1, FromLonRole, ToLatRole, ToLonRole };

    struct Item { double fromLat, fromLon, toLat, toLon; };

    explicit ConflictSegmentModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return static_cast<int>(m_items.size()); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setConflicts(const QVariantList& conflicts, const QVector<QVariantMap>& waypoints);
    void clear();

    const auto& items() const { return m_items; }

private:
    QVector<Item> m_items;
};

// ═══════════════════════════════════════════════════════════
//  ConflictPointModel
// ═══════════════════════════════════════════════════════════

class ConflictPointModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { LatRole = Qt::UserRole + 1, LonRole, ReasonRole };

    struct Item { double lat, lon; QString reason; };

    explicit ConflictPointModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return static_cast<int>(m_items.size()); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setConflicts(const QVariantList& conflicts, const QVector<QVariantMap>& waypoints);
    void setPoints(const QVariantList& points);
    void clear();

    const auto& items() const { return m_items; }

private:
    QVector<Item> m_items;
};

// ═══════════════════════════════════════════════════════════
//  ObstacleWarningModel
// ═══════════════════════════════════════════════════════════

class ObstacleWarningModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { LatRole = Qt::UserRole + 1, LonRole, TooltipRole, SourceRole };

    struct Item { double lat, lon; QString tooltip; QString source; /* "zone" | "settlement" */ };

    explicit ObstacleWarningModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return static_cast<int>(m_items.size()); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setItems(QVector<Item> items);
    void appendItems(const QVector<Item>& items);
    void clear();

    static constexpr int MAX_ITEMS = 150;
    const auto& items() const { return m_items; }

private:
    QVector<Item> m_items;
};

// ═══════════════════════════════════════════════════════════
//  SimpleVertexModel
// ═══════════════════════════════════════════════════════════

class SimpleVertexModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { LatRole = Qt::UserRole + 1, LonRole, VertexIndexRole, MidpointIndexRole };

    explicit SimpleVertexModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return static_cast<int>(m_items.size()); }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setPoints(const QVector<QPointF>& points);
    void addPoint(double lat, double lon);
    void updatePoint(int idx, double lat, double lon);
    void removePoint(int idx);
    void clear();
    QVector<QPointF> getPoints() const { return m_items; }

private:
    QVector<QPointF> m_items;  // x=lat, y=lon
};

} // namespace vtol
