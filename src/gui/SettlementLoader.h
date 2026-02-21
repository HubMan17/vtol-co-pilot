#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QString>
#include <QSet>
#include <QMap>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <tuple>

namespace vtol {

/// Compact settlement feature: polygon or circle (node)
struct SettlementFeature {
    char form = 'p';                        // 'p' = polygon, 'n' = node (circle)
    std::vector<std::array<double, 2>> coords; // for 'p': [[lat,lon],...]
    double lat = 0, lon = 0;                // for 'n'
    std::string type;                       // "city","town","village","hamlet"
};

using FeatureList = std::vector<SettlementFeature>;

/// Tile bounds: south, west, north, east
using TileBounds = std::tuple<double, double, double, double>;

// ─────────────────────────────────────────────────────────────────────────
class SettlementLoader;

/// Background worker — batches queued tiles into one Overpass request
class SettlementFetchWorker : public QThread {
    Q_OBJECT
public:
    explicit SettlementFetchWorker(SettlementLoader* loader);
    void run() override;

signals:
    void tileLoaded(const QString& key, const FeatureList& features);

private:
    static QMap<QString, FeatureList>
    distribute(const FeatureList& features, const std::vector<TileBounds>& tiles);

    SettlementLoader* m_loader;
};

// ─────────────────────────────────────────────────────────────────────────
class SettlementLoader : public QObject {
    Q_OBJECT
    friend class SettlementFetchWorker;

public:
    explicit SettlementLoader(QObject* parent = nullptr);

    /// Request settlements for viewport bbox
    void request(double south, double west, double north, double east);

    /// Emit cached tiles that overlap given bbox (no fetch)
    void preloadCachedTiles(double south, double west, double north, double east);

    /// Clear all cached tiles from disk and memory
    void clearCache();

signals:
    void tileLoaded(const QString& key, const FeatureList& features);

private:
    void kickWorker();
    void onWorkerDone();
    void loadDiskCache();

    // ── Processing helpers (static) ──
    static FeatureList processElements(const nlohmann::json& elements);
    static std::vector<std::array<double, 2>>
        mergeRelation(const nlohmann::json& members);
    static std::vector<std::array<double, 2>>
        simplifyDP(const std::vector<std::array<double, 2>>& coords, double tolerance);

    static QString tileKey(const TileBounds& t);
    static std::vector<TileBounds> gridTiles(double s, double w, double n, double e);
    static void saveTile(const QString& key, const FeatureList& features);

    static constexpr double TILE_GRID = 0.1;
    static constexpr double DP_TOLERANCE = 0.0005;

    QMutex m_mutex;
    std::vector<TileBounds> m_queue;
    QSet<QString> m_loadedKeys;
    QSet<QString> m_emittedKeys;
    QMap<QString, FeatureList> m_cache;
    QString m_cacheDir;

    SettlementFetchWorker* m_worker;
};

} // namespace vtol
