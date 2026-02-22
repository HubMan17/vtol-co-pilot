#include "MapModels.h"
#include <QPointF>
#include <cmath>
#include <spdlog/spdlog.h>

namespace vtol {

// ═══════════════════════════════════════════════════════════
//  WaypointListModel
// ═══════════════════════════════════════════════════════════

QHash<int, QByteArray> WaypointListModel::roleNames() const {
    return {
        {LatRole, "lat"}, {LonRole, "lon"}, {AltRole, "altitude"},
        {ActionRole, "action"}, {WpIndexRole, "wpIndex"}, {WpStateRole, "wpState"},
    };
}

QVariant WaypointListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const auto& it = m_items[index.row()];
    switch (role) {
    case LatRole:     return it.lat;
    case LonRole:     return it.lon;
    case AltRole:     return it.altitude;
    case ActionRole:  return it.action;
    case WpIndexRole: return index.row();
    case WpStateRole: return it.state;
    }
    return {};
}

void WaypointListModel::setWaypoints(const QVector<QVariantMap>& waypoints, int activeIdx) {
    // Fast path: single waypoint appended to end (most common case) — use insertRows
    if (waypoints.size() == m_items.size() + 1) {
        const auto& wp = waypoints.last();
        Item it;
        it.lat = wp["lat"].toDouble();
        it.lon = wp["lon"].toDouble();
        it.altitude = wp.value("altitude", 0).toDouble();
        it.action = wp.value("action", "").toString();
        it.state = "future";
        int row = m_items.size();
        beginInsertRows({}, row, row);
        m_items.append(it);
        endInsertRows();
        // Update active states for existing items if needed
        updateActive(activeIdx);
        return;
    }

    // Full reset for all other cases (load route, remove waypoint, etc.)
    beginResetModel();
    m_items.clear();
    for (int i = 0; i < waypoints.size(); ++i) {
        const auto& wp = waypoints[i];
        Item it;
        it.lat = wp["lat"].toDouble();
        it.lon = wp["lon"].toDouble();
        it.altitude = wp.value("altitude", 0).toDouble();
        it.action = wp.value("action", "").toString();
        if (i < activeIdx) it.state = "past";
        else if (i == activeIdx) it.state = "active";
        else it.state = "future";
        m_items.append(it);
    }
    endResetModel();
}

void WaypointListModel::updateActive(int activeIdx) {
    for (int i = 0; i < m_items.size(); ++i) {
        QString newState;
        if (i < activeIdx) newState = "past";
        else if (i == activeIdx) newState = "active";
        else newState = "future";
        if (m_items[i].state != newState) {
            m_items[i].state = newState;
            auto idx = index(i);
            emit dataChanged(idx, idx, {WpStateRole});
        }
    }
}

void WaypointListModel::updateItemPosition(int idx, double lat, double lon)
{
    if (idx < 0 || idx >= m_items.size()) return;
    m_items[idx].lat = lat;
    m_items[idx].lon = lon;
    auto modelIdx = index(idx);
    emit dataChanged(modelIdx, modelIdx, {LatRole, LonRole});
}

// ═══════════════════════════════════════════════════════════
//  RouteSegmentModel
// ═══════════════════════════════════════════════════════════

QHash<int, QByteArray> RouteSegmentModel::roleNames() const {
    return {
        {FromLatRole, "fromLat"}, {FromLonRole, "fromLon"},
        {ToLatRole, "toLat"}, {ToLonRole, "toLon"}, {SegStateRole, "segState"},
    };
}

QVariant RouteSegmentModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const auto& it = m_items[index.row()];
    switch (role) {
    case FromLatRole: return it.fromLat;
    case FromLonRole: return it.fromLon;
    case ToLatRole:   return it.toLat;
    case ToLonRole:   return it.toLon;
    case SegStateRole: return it.state;
    }
    return {};
}

void RouteSegmentModel::setSegments(const QVector<QVariantMap>& waypoints, int activeIdx) {
    int expectedSegs = waypoints.size() > 1 ? waypoints.size() - 1 : 0;

    // Fast path: one new waypoint appended → one new segment at end
    if (waypoints.size() >= 2 && expectedSegs == m_items.size() + 1) {
        const auto& wp1 = waypoints[waypoints.size() - 2];
        const auto& wp2 = waypoints[waypoints.size() - 1];
        Item it;
        it.fromLat = wp1["lat"].toDouble();
        it.fromLon = wp1["lon"].toDouble();
        it.toLat = wp2["lat"].toDouble();
        it.toLon = wp2["lon"].toDouble();
        it.state = "future";
        int row = m_items.size();
        beginInsertRows({}, row, row);
        m_items.append(it);
        endInsertRows();
        return;
    }

    // Full reset for all other cases
    beginResetModel();
    m_items.clear();
    for (int i = 0; i < waypoints.size() - 1; ++i) {
        const auto& wp1 = waypoints[i];
        const auto& wp2 = waypoints[i + 1];
        Item it;
        it.fromLat = wp1["lat"].toDouble();
        it.fromLon = wp1["lon"].toDouble();
        it.toLat = wp2["lat"].toDouble();
        it.toLon = wp2["lon"].toDouble();
        if (i < activeIdx) it.state = "past";
        else if (i == activeIdx) it.state = "active";
        else it.state = "future";
        m_items.append(it);
    }
    endResetModel();
}

// ═══════════════════════════════════════════════════════════
//  ZoneListModel
// ═══════════════════════════════════════════════════════════

QHash<int, QByteArray> ZoneListModel::roleNames() const {
    return {{ZoneIdRole, "zoneId"}, {ZoneCoordsRole, "zoneCoords"}, {ZoneNameRole, "zoneName"}};
}

QVariant ZoneListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const auto& it = m_items[index.row()];
    switch (role) {
    case ZoneIdRole:    return it.id;
    case ZoneCoordsRole: return it.points;
    case ZoneNameRole:  return it.name;
    }
    return {};
}

void ZoneListModel::addZone(const QString& id, const QVariantList& points, const QString& name) {
    int row = rowCount();
    beginInsertRows({}, row, row);
    m_items.append({id, points, name});
    endInsertRows();
}

void ZoneListModel::removeZone(const QString& id) {
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id == id) {
            SPDLOG_INFO("[ZoneListModel] removeZone '{}' at index {}, size {}→{}", id.toStdString(), i, m_items.size(), m_items.size() - 1);
            beginRemoveRows({}, i, i);
            m_items.remove(i);
            endRemoveRows();
            return;
        }
    }
    SPDLOG_WARN("[ZoneListModel] removeZone '{}' NOT FOUND in {} items", id.toStdString(), m_items.size());
}

void ZoneListModel::updateZonePoints(const QString& id, const QVariantList& points) {
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id == id) {
            m_items[i].points = points;
            auto idx = index(i);
            emit dataChanged(idx, idx, {ZoneCoordsRole});
            return;
        }
    }
}

void ZoneListModel::clear() {
    if (!m_items.isEmpty()) {
        beginResetModel();
        m_items.clear();
        endResetModel();
    }
}

// ═══════════════════════════════════════════════════════════
//  DashBorderModel
// ═══════════════════════════════════════════════════════════

QHash<int, QByteArray> DashBorderModel::roleNames() const {
    return {
        {FromLatRole, "fromLat"}, {FromLonRole, "fromLon"},
        {ToLatRole, "toLat"}, {ToLonRole, "toLon"},
    };
}

QVariant DashBorderModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const auto& it = m_items[index.row()];
    switch (role) {
    case FromLatRole: return it.fromLat;
    case FromLonRole: return it.fromLon;
    case ToLatRole:   return it.toLat;
    case ToLonRole:   return it.toLon;
    }
    return {};
}

void DashBorderModel::rebuildFromZones(const QVector<QVariantList>& polygons) {
    beginResetModel();
    m_items.clear();
    for (const auto& pts : polygons) {
        QVector<std::tuple<double, double>> polygon;
        for (const auto& pt : pts) {
            auto list = pt.toList();
            if (list.size() >= 2)
                polygon.append({list[0].toDouble(), list[1].toDouble()});
        }
        generateDashes(polygon);
        if (m_maxSegments > 0 && m_items.size() >= m_maxSegments) break;
    }
    endResetModel();
}

void DashBorderModel::rebuildFromCoords(const QVector<QVariantList>& polygonCoords) {
    beginResetModel();
    m_items.clear();
    for (const auto& coords : polygonCoords) {
        QVector<std::tuple<double, double>> polygon;
        for (const auto& pt : coords) {
            auto list = pt.toList();
            if (list.size() >= 2)
                polygon.append({list[0].toDouble(), list[1].toDouble()});
        }
        generateDashes(polygon);
        if (m_maxSegments > 0 && m_items.size() >= m_maxSegments) break;
    }
    endResetModel();
}

void DashBorderModel::generateDashes(const QVector<std::tuple<double, double>>& polygon) {
    int n = polygon.size();
    if (n < 3) return;

    double step = m_dashDeg + m_gapDeg;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        auto [lat1, lon1] = polygon[i];
        auto [lat2, lon2] = polygon[j];
        double dlat = lat2 - lat1;
        double dlon = lon2 - lon1;
        double edgeLen = std::sqrt(dlat * dlat + dlon * dlon);
        if (edgeLen < 1e-9) continue;

        double t = 0.0;
        while (t < edgeLen) {
            double tEnd = std::min(t + m_dashDeg, edgeLen);
            double f1 = t / edgeLen;
            double f2 = tEnd / edgeLen;
            m_items.append({
                lat1 + f1 * dlat, lon1 + f1 * dlon,
                lat1 + f2 * dlat, lon1 + f2 * dlon
            });
            if (m_maxSegments > 0 && m_items.size() >= m_maxSegments) return;
            t += step;
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  SettlementPolyModel
// ═══════════════════════════════════════════════════════════

QHash<int, QByteArray> SettlementPolyModel::roleNames() const {
    return {{CoordsRole, "coords"}};
}

QVariant SettlementPolyModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    if (role == CoordsRole) return m_items[index.row()];
    return {};
}

void SettlementPolyModel::addPolys(const QVariantList& features) {
    QVector<QVariantList> newItems;
    QVector<QString> newSigs;
    for (const auto& f : features) {
        auto map = f.toMap();
        auto coords = map["c"].toList();
        if (coords.isEmpty()) continue;
        auto first = coords[0].toList();
        if (first.size() < 2) continue;
        QString sig = QString("p_%1_%2_%3")
                          .arg(first[0].toDouble(), 0, 'f', 4)
                          .arg(first[1].toDouble(), 0, 'f', 4)
                          .arg(coords.size());
        if (!m_sigs.contains(sig)) {
            m_sigs.insert(sig);
            newItems.append(coords);
            newSigs.append(sig);
        }
    }
    if (newItems.isEmpty()) return;

    int overflow = m_items.size() + newItems.size() - MAX_ITEMS;
    if (overflow > 0) {
        // Remove evicted signatures so they can be re-added later
        for (int i = 0; i < overflow && i < m_sigByIndex.size(); ++i)
            m_sigs.remove(m_sigByIndex[i]);
        beginRemoveRows({}, 0, overflow - 1);
        m_items.remove(0, overflow);
        m_sigByIndex.remove(0, overflow);
        endRemoveRows();
    }
    int start = m_items.size();
    beginInsertRows({}, start, start + newItems.size() - 1);
    for (auto& item : newItems) m_items.append(std::move(item));
    m_sigByIndex.append(newSigs);
    endInsertRows();
}

void SettlementPolyModel::clear() {
    if (m_items.isEmpty()) return;
    beginRemoveRows({}, 0, m_items.size() - 1);
    m_items.clear();
    m_sigs.clear();
    m_sigByIndex.clear();
    endRemoveRows();
}

// ═══════════════════════════════════════════════════════════
//  SettlementCircleModel
// ═══════════════════════════════════════════════════════════

QHash<int, QByteArray> SettlementCircleModel::roleNames() const {
    return {{LatRole, "lat"}, {LonRole, "lon"}, {RadiusRole, "radius"}};
}

QVariant SettlementCircleModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const auto& it = m_items[index.row()];
    switch (role) {
    case LatRole:    return it.lat;
    case LonRole:    return it.lon;
    case RadiusRole: return it.radius;
    }
    return {};
}

double SettlementCircleModel::placeRadius(const QString& type) {
    if (type == "city") return 5000;
    if (type == "town") return 2000;
    if (type == "village") return 800;
    if (type == "hamlet") return 400;
    return 500;
}

void SettlementCircleModel::addCircles(const QVariantList& nodes) {
    QVector<Item> newItems;
    QVector<QString> newSigs;
    for (const auto& f : nodes) {
        auto map = f.toMap();
        double lat = map["lat"].toDouble();
        double lon = map["lon"].toDouble();
        QString sig = QString("n_%1_%2").arg(lat, 0, 'f', 5).arg(lon, 0, 'f', 5);
        if (!m_sigs.contains(sig)) {
            m_sigs.insert(sig);
            double r = placeRadius(map.value("t", "").toString());
            newItems.append({lat, lon, r});
            newSigs.append(sig);
        }
    }
    if (newItems.isEmpty()) return;

    int overflow = m_items.size() + newItems.size() - MAX_ITEMS;
    if (overflow > 0) {
        // Remove evicted signatures so they can be re-added later
        for (int i = 0; i < overflow && i < m_sigByIndex.size(); ++i)
            m_sigs.remove(m_sigByIndex[i]);
        beginRemoveRows({}, 0, overflow - 1);
        m_items.remove(0, overflow);
        m_sigByIndex.remove(0, overflow);
        endRemoveRows();
    }
    int start = m_items.size();
    beginInsertRows({}, start, start + newItems.size() - 1);
    m_items.append(newItems);
    m_sigByIndex.append(newSigs);
    endInsertRows();
}

void SettlementCircleModel::clear() {
    if (m_items.isEmpty()) return;
    beginRemoveRows({}, 0, m_items.size() - 1);
    m_items.clear();
    m_sigs.clear();
    m_sigByIndex.clear();
    endRemoveRows();
}

// ═══════════════════════════════════════════════════════════
//  ConflictSegmentModel
// ═══════════════════════════════════════════════════════════

QHash<int, QByteArray> ConflictSegmentModel::roleNames() const {
    return {
        {FromLatRole, "fromLat"}, {FromLonRole, "fromLon"},
        {ToLatRole, "toLat"}, {ToLonRole, "toLon"},
    };
}

QVariant ConflictSegmentModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const auto& it = m_items[index.row()];
    switch (role) {
    case FromLatRole: return it.fromLat;
    case FromLonRole: return it.fromLon;
    case ToLatRole:   return it.toLat;
    case ToLonRole:   return it.toLon;
    }
    return {};
}

void ConflictSegmentModel::setConflicts(const QVariantList& conflicts, const QVector<QVariantMap>& waypoints) {
    beginResetModel();
    m_items.clear();
    for (const auto& c : conflicts) {
        auto map = c.toMap();
        int fi = map["from_idx"].toInt();
        int ti = map["to_idx"].toInt();
        if (fi >= 0 && ti >= 0 && fi < waypoints.size() && ti < waypoints.size()) {
            m_items.append({
                waypoints[fi]["lat"].toDouble(), waypoints[fi]["lon"].toDouble(),
                waypoints[ti]["lat"].toDouble(), waypoints[ti]["lon"].toDouble(),
            });
        }
    }
    endResetModel();
}

void ConflictSegmentModel::clear() {
    if (!m_items.isEmpty()) {
        beginResetModel();
        m_items.clear();
        endResetModel();
    }
}

// ═══════════════════════════════════════════════════════════
//  ConflictPointModel
// ═══════════════════════════════════════════════════════════

QHash<int, QByteArray> ConflictPointModel::roleNames() const {
    return {{LatRole, "lat"}, {LonRole, "lon"}, {ReasonRole, "reason"}};
}

QVariant ConflictPointModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const auto& it = m_items[index.row()];
    switch (role) {
    case LatRole:    return it.lat;
    case LonRole:    return it.lon;
    case ReasonRole: return it.reason;
    }
    return {};
}

void ConflictPointModel::setConflicts(const QVariantList& conflicts, const QVector<QVariantMap>& waypoints) {
    beginResetModel();
    m_items.clear();
    for (const auto& c : conflicts) {
        auto map = c.toMap();
        int fi = map.value("from_idx", -1).toInt();
        int ti = map.value("to_idx", -1).toInt();
        if (fi < 0 || ti < 0 || fi >= waypoints.size() || ti >= waypoints.size()) continue;
        m_items.append({
            (waypoints[fi]["lat"].toDouble() + waypoints[ti]["lat"].toDouble()) / 2.0,
            (waypoints[fi]["lon"].toDouble() + waypoints[ti]["lon"].toDouble()) / 2.0,
            map.value("reason", "Конфликт маршрута").toString(),
        });
    }
    endResetModel();
}

void ConflictPointModel::setPoints(const QVariantList& points) {
    beginResetModel();
    m_items.clear();
    for (const auto& p : points) {
        auto map = p.toMap();
        m_items.append({
            map["lat"].toDouble(), map["lon"].toDouble(),
            map.value("reason", "").toString(),
        });
    }
    endResetModel();
}

void ConflictPointModel::clear() {
    if (!m_items.isEmpty()) {
        beginResetModel();
        m_items.clear();
        endResetModel();
    }
}

// ═══════════════════════════════════════════════════════════
//  ObstacleWarningModel
// ═══════════════════════════════════════════════════════════

QHash<int, QByteArray> ObstacleWarningModel::roleNames() const {
    return {{LatRole, "lat"}, {LonRole, "lon"}, {TooltipRole, "tooltip"}, {SourceRole, "source"}};
}

QVariant ObstacleWarningModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const auto& it = m_items[index.row()];
    switch (role) {
    case LatRole:     return it.lat;
    case LonRole:     return it.lon;
    case TooltipRole: return it.tooltip;
    case SourceRole:  return it.source;
    }
    return {};
}

void ObstacleWarningModel::setItems(QVector<Item> items) {
    beginResetModel();
    if (items.size() > MAX_ITEMS) items.resize(MAX_ITEMS);
    m_items = std::move(items);
    endResetModel();
}

void ObstacleWarningModel::appendItems(const QVector<Item>& items) {
    if (items.isEmpty()) return;
    int overflow = m_items.size() + items.size() - MAX_ITEMS;
    if (overflow > 0) {
        beginRemoveRows({}, 0, overflow - 1);
        m_items.remove(0, overflow);
        endRemoveRows();
    }
    int start = m_items.size();
    int count = std::min(static_cast<int>(items.size()), MAX_ITEMS);
    beginInsertRows({}, start, start + count - 1);
    for (int i = 0; i < count; ++i)
        m_items.append(items[i]);
    endInsertRows();
}

void ObstacleWarningModel::clear() {
    if (!m_items.isEmpty()) {
        beginResetModel();
        m_items.clear();
        endResetModel();
    }
}

// ═══════════════════════════════════════════════════════════
//  SimpleVertexModel
// ═══════════════════════════════════════════════════════════

QHash<int, QByteArray> SimpleVertexModel::roleNames() const {
    return {
        {LatRole, "lat"}, {LonRole, "lon"},
        {VertexIndexRole, "vertexIndex"}, {MidpointIndexRole, "midpointIndex"},
    };
}

QVariant SimpleVertexModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const auto& pt = m_items[index.row()];
    switch (role) {
    case LatRole:           return pt.x();
    case LonRole:           return pt.y();
    case VertexIndexRole:   return index.row();
    case MidpointIndexRole: return index.row();
    }
    return {};
}

void SimpleVertexModel::setPoints(const QVector<QPointF>& points) {
    beginResetModel();
    m_items = points;
    endResetModel();
}

void SimpleVertexModel::addPoint(double lat, double lon) {
    int row = m_items.size();
    beginInsertRows({}, row, row);
    m_items.append(QPointF(lat, lon));
    endInsertRows();
}

void SimpleVertexModel::updatePoint(int idx, double lat, double lon) {
    if (idx >= 0 && idx < m_items.size()) {
        m_items[idx] = QPointF(lat, lon);
        auto midx = index(idx);
        emit dataChanged(midx, midx, {LatRole, LonRole});
    }
}

void SimpleVertexModel::removePoint(int idx) {
    if (idx >= 0 && idx < m_items.size()) {
        beginRemoveRows({}, idx, idx);
        m_items.remove(idx);
        endRemoveRows();
    }
}

void SimpleVertexModel::clear() {
    if (!m_items.isEmpty()) {
        beginResetModel();
        m_items.clear();
        endResetModel();
    }
}

} // namespace vtol
