#include "SettlementLoader.h"
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QUrlQuery>
#include <cmath>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace vtol {

static const QStringList OVERPASS_ENDPOINTS = {
    QStringLiteral("https://overpass-api.de/api/interpreter"),
    QStringLiteral("https://overpass.kumi.systems/api/interpreter"),
};

// ═════════════════════════════════════════════════════════════════════════
//  SettlementFetchWorker
// ═════════════════════════════════════════════════════════════════════════

SettlementFetchWorker::SettlementFetchWorker(SettlementLoader* loader)
    : QThread(loader), m_loader(loader) {}

void SettlementFetchWorker::run()
{
    while (true) {
        std::vector<TileBounds> tiles;
        {
            QMutexLocker lk(&m_loader->m_mutex);
            if (m_loader->m_queue.empty()) return;
            // Limit batch size to avoid Overpass timeout on large bbox
            constexpr size_t MAX_BATCH = 6;
            if (m_loader->m_queue.size() <= MAX_BATCH) {
                tiles = std::move(m_loader->m_queue);
                m_loader->m_queue.clear();
            } else {
                tiles.assign(m_loader->m_queue.begin(),
                             m_loader->m_queue.begin() + MAX_BATCH);
                m_loader->m_queue.erase(m_loader->m_queue.begin(),
                                         m_loader->m_queue.begin() + MAX_BATCH);
            }
        }

        // Compute merged bbox
        double s = 90, w = 180, n = -90, e = -180;
        for (auto& [ts, tw, tn, te] : tiles) {
            s = std::min(s, ts); w = std::min(w, tw);
            n = std::max(n, tn); e = std::max(e, te);
        }

        auto query = QString(
            "[out:json][timeout:30];("
            "way[\"place\"~\"^(city|town|village|hamlet)$\"](%1,%2,%3,%4);"
            "relation[\"place\"~\"^(city|town|village|hamlet)$\"](%1,%2,%3,%4);"
            "node[\"place\"~\"^(city|town|village|hamlet)$\"](%1,%2,%3,%4);"
            ");out geom;")
            .arg(s, 0, 'f', 6).arg(w, 0, 'f', 6)
            .arg(n, 0, 'f', 6).arg(e, 0, 'f', 6);

        SPDLOG_INFO("[SettlementLoader] Fetching {} tiles, bbox [{:.4f},{:.4f},{:.4f},{:.4f}]",
                    tiles.size(), s, w, n, e);

        // Try each endpoint
        bool success = false;
        for (int attempt = 0; attempt < OVERPASS_ENDPOINTS.size(); ++attempt) {
            try {
                QNetworkAccessManager nam;
                QNetworkRequest req{QUrl{OVERPASS_ENDPOINTS[attempt]}};
                req.setHeader(QNetworkRequest::ContentTypeHeader,
                              "application/x-www-form-urlencoded");

                QUrlQuery postData;
                postData.addQueryItem("data", query);

                QEventLoop loop;
                auto* reply = nam.post(req, postData.toString(QUrl::FullyEncoded).toUtf8());
                QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
                loop.exec();

                if (reply->error() != QNetworkReply::NoError) {
                    SPDLOG_WARN("[SettlementLoader] HTTP error: {}",
                                reply->errorString().toStdString());
                    reply->deleteLater();
                    if (attempt < OVERPASS_ENDPOINTS.size() - 1)
                        QThread::sleep(2);
                    continue;
                }

                auto raw = nlohmann::json::parse(reply->readAll().toStdString());
                reply->deleteLater();

                auto elements = raw.value("elements", nlohmann::json::array());
                auto allFeatures = SettlementLoader::processElements(elements);
                auto tileMap = distribute(allFeatures, tiles);

                {
                    QMutexLocker lk(&m_loader->m_mutex);
                    for (auto it = tileMap.begin(); it != tileMap.end(); ++it) {
                        m_loader->m_cache[it.key()] = it.value();
                        m_loader->m_emittedKeys.insert(it.key());
                    }
                }

                for (auto it = tileMap.begin(); it != tileMap.end(); ++it) {
                    SettlementLoader::saveTile(it.key(), it.value());
                    emit tileLoaded(it.key(), it.value());
                }

                SPDLOG_DEBUG("[SettlementLoader] Fetched {} features → {} tiles",
                             allFeatures.size(), tileMap.size());
                success = true;
                break;

            } catch (const std::exception& ex) {
                SPDLOG_WARN("[SettlementLoader] Attempt {} failed: {}",
                            attempt + 1, ex.what());
                if (attempt < OVERPASS_ENDPOINTS.size() - 1)
                    QThread::sleep(2);
            }
        }

        if (!success) {
            SPDLOG_ERROR("[SettlementLoader] All endpoints failed");
            QMutexLocker lk(&m_loader->m_mutex);
            for (auto& t : tiles)
                m_loader->m_loadedKeys.remove(SettlementLoader::tileKey(t));
        }
    }
}

QMap<QString, FeatureList>
SettlementFetchWorker::distribute(const FeatureList& features,
                                   const std::vector<TileBounds>& tiles)
{
    QMap<QString, FeatureList> result;
    for (auto& t : tiles)
        result[SettlementLoader::tileKey(t)] = {};

    for (auto& f : features) {
        double fs, fw, fn, fe;
        if (f.form == 'p') {
            fs = fn = f.coords[0][0];
            fw = fe = f.coords[0][1];
            for (auto& c : f.coords) {
                fs = std::min(fs, c[0]); fn = std::max(fn, c[0]);
                fw = std::min(fw, c[1]); fe = std::max(fe, c[1]);
            }
        } else {
            fs = fn = f.lat;
            fw = fe = f.lon;
        }
        for (auto& t : tiles) {
            auto [ts, tw, tn, te] = t;
            if (!(fn < ts || fs > tn || fe < tw || fw > te))
                result[SettlementLoader::tileKey(t)].push_back(f);
        }
    }
    return result;
}

// ═════════════════════════════════════════════════════════════════════════
//  SettlementLoader
// ═════════════════════════════════════════════════════════════════════════

SettlementLoader::SettlementLoader(QObject* parent)
    : QObject(parent)
{
    m_cacheDir = QCoreApplication::applicationDirPath() + "/cache/settlements_v5";
    QDir().mkpath(m_cacheDir);

    m_worker = new SettlementFetchWorker(this);
    connect(m_worker, &SettlementFetchWorker::tileLoaded,
            this, &SettlementLoader::tileLoaded);
    connect(m_worker, &QThread::finished, this, &SettlementLoader::onWorkerDone);

    loadDiskCache();
}

void SettlementLoader::request(double south, double west, double north, double east)
{
    auto tiles = gridTiles(south, west, north, east);
    bool added = false;

    {
        QMutexLocker lk(&m_mutex);
        for (auto& t : tiles) {
            auto key = tileKey(t);
            if (!m_loadedKeys.contains(key)) {
                m_loadedKeys.insert(key);
                m_queue.push_back(t);
                added = true;
            }
        }
    }

    if (added) kickWorker();

    // Emit cached tiles that haven't been emitted yet
    preloadCachedTiles(south, west, north, east);
}

void SettlementLoader::preloadCachedTiles(double south, double west,
                                           double north, double east)
{
    auto tiles = gridTiles(south, west, north, east);
    QMutexLocker lk(&m_mutex);
    for (auto& t : tiles) {
        auto key = tileKey(t);
        if (m_cache.contains(key) && !m_cache[key].empty()
            && !m_emittedKeys.contains(key))
        {
            m_emittedKeys.insert(key);
            auto features = m_cache[key];  // copy under lock
            lk.unlock();
            emit tileLoaded(key, features);
            lk.relock();
        }
    }
}

void SettlementLoader::kickWorker()
{
    if (!m_worker->isRunning())
        m_worker->start();
}

void SettlementLoader::onWorkerDone()
{
    QMutexLocker lk(&m_mutex);
    if (!m_queue.empty()) {
        lk.unlock();
        m_worker->start();
    }
}

void SettlementLoader::clearCache()
{
    QMutexLocker lk(&m_mutex);
    m_cache.clear();
    m_loadedKeys.clear();
    m_emittedKeys.clear();
    m_queue.clear();
    lk.unlock();

    // Remove all files from cache dir
    QDir dir(m_cacheDir);
    for (auto& f : dir.entryList({"*.json"}, QDir::Files))
        dir.remove(f);
    SPDLOG_INFO("[SettlementLoader] Cache cleared: {}", m_cacheDir.toStdString());
}

// ── Disk cache ──────────────────────────────────────────────────────────

void SettlementLoader::loadDiskCache()
{
    QDir dir(m_cacheDir);
    auto files = dir.entryList({"*.json"}, QDir::Files);
    for (auto& fname : files) {
        auto key = fname.left(fname.size() - 5); // strip .json
        try {
            QFile f(dir.filePath(fname));
            if (!f.open(QIODevice::ReadOnly)) continue;
            auto data = nlohmann::json::parse(f.readAll().toStdString());

            FeatureList features;
            for (auto& el : data) {
                SettlementFeature sf;
                sf.form = el.value("F", "p") == "p" ? 'p' : 'n';
                sf.type = el.value("t", "");
                if (sf.form == 'p') {
                    for (auto& c : el["c"])
                        sf.coords.push_back({c[0].get<double>(), c[1].get<double>()});
                } else {
                    sf.lat = el.value("lat", 0.0);
                    sf.lon = el.value("lon", 0.0);
                }
                features.push_back(std::move(sf));
            }
            m_cache[key] = std::move(features);
            m_loadedKeys.insert(key);
        } catch (const std::exception& ex) {
            SPDLOG_WARN("[SettlementLoader] Cache load error for {}: {}",
                        fname.toStdString(), ex.what());
        }
    }
    SPDLOG_INFO("[SettlementLoader] Loaded {} cached tiles", m_cache.size());
}

void SettlementLoader::saveTile(const QString& key, const FeatureList& features)
{
    try {
        // Use application dir path — but we're in a worker thread, so compose path
        auto dir = QCoreApplication::applicationDirPath() + "/cache/settlements_v5";
        QDir().mkpath(dir);
        auto path = dir + "/" + key + ".json";

        nlohmann::json arr = nlohmann::json::array();
        for (auto& f : features) {
            nlohmann::json obj;
            obj["F"] = (f.form == 'p') ? "p" : "n";
            obj["t"] = f.type;
            if (f.form == 'p') {
                nlohmann::json coords = nlohmann::json::array();
                for (auto& c : f.coords)
                    coords.push_back({std::round(c[0] * 1e5) / 1e5,
                                      std::round(c[1] * 1e5) / 1e5});
                obj["c"] = coords;
            } else {
                obj["lat"] = f.lat;
                obj["lon"] = f.lon;
            }
            arr.push_back(obj);
        }

        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            auto str = arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore);
            file.write(str.data(), static_cast<qint64>(str.size()));
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[SettlementLoader] Cache save error: {}", ex.what());
    }
}

// ── Processing ──────────────────────────────────────────────────────────

FeatureList SettlementLoader::processElements(const nlohmann::json& elements)
{
    FeatureList polys;
    std::vector<SettlementFeature> nodes;
    std::vector<std::array<double, 4>> polyBboxes; // s,w,n,e

    for (auto& el : elements) {
        auto place = el.value("/tags/place"_json_pointer, std::string());
        if (place.empty()) continue;

        auto type = el.value("type", "");

        if (type == "way" && el.contains("geometry")) {
            std::vector<std::array<double, 2>> coords;
            for (auto& p : el["geometry"])
                coords.push_back({p["lat"].get<double>(), p["lon"].get<double>()});
            if (coords.size() < 3) continue;

            double minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
            for (auto& c : coords) {
                minLat = std::min(minLat, c[0]); maxLat = std::max(maxLat, c[0]);
                minLon = std::min(minLon, c[1]); maxLon = std::max(maxLon, c[1]);
            }
            double span = std::max(maxLat - minLat, maxLon - minLon);
            double tol = DP_TOLERANCE;
            if (span > 0.5) tol = 0.005;
            else if (span > 0.2) tol = 0.002;
            else if (span > 0.1) tol = 0.001;

            SettlementFeature sf;
            sf.form = 'p';
            sf.coords = simplifyDP(coords, tol);
            sf.type = place;
            polys.push_back(std::move(sf));
            polyBboxes.push_back({minLat, minLon, maxLat, maxLon});

        } else if (type == "relation" && el.contains("members")) {
            auto ring = mergeRelation(el["members"]);
            if (ring.size() < 3) continue;

            double minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
            for (auto& c : ring) {
                minLat = std::min(minLat, c[0]); maxLat = std::max(maxLat, c[0]);
                minLon = std::min(minLon, c[1]); maxLon = std::max(maxLon, c[1]);
            }
            double span = std::max(maxLat - minLat, maxLon - minLon);
            double tol = DP_TOLERANCE;
            if (span > 0.5) tol = 0.005;
            else if (span > 0.2) tol = 0.002;
            else if (span > 0.1) tol = 0.001;

            SettlementFeature sf;
            sf.form = 'p';
            sf.coords = simplifyDP(ring, tol);
            sf.type = place;
            polys.push_back(std::move(sf));
            polyBboxes.push_back({minLat, minLon, maxLat, maxLon});

        } else if (type == "node") {
            SettlementFeature sf;
            sf.form = 'n';
            sf.lat = el["lat"].get<double>();
            sf.lon = el["lon"].get<double>();
            sf.type = place;
            nodes.push_back(std::move(sf));
        }
    }

    // Filter nodes covered by existing polygons
    FeatureList result = std::move(polys);
    for (auto& nd : nodes) {
        bool covered = false;
        for (auto& [s, w, n, e] : polyBboxes) {
            if (nd.lat >= s && nd.lat <= n && nd.lon >= w && nd.lon <= e) {
                covered = true;
                break;
            }
        }
        if (!covered)
            result.push_back(std::move(nd));
    }
    return result;
}

std::vector<std::array<double, 2>>
SettlementLoader::mergeRelation(const nlohmann::json& members)
{
    std::vector<std::vector<std::array<double, 2>>> segments;

    for (auto& m : members) {
        if (m.value("type", "") != "way") continue;
        auto role = m.value("role", "outer");
        if (role != "outer" && !role.empty()) continue;
        if (!m.contains("geometry")) continue;

        std::vector<std::array<double, 2>> seg;
        for (auto& p : m["geometry"])
            seg.push_back({p["lat"].get<double>(), p["lon"].get<double>()});
        if (!seg.empty())
            segments.push_back(std::move(seg));
    }

    if (segments.empty()) return {};

    auto ring = segments[0];
    std::vector<std::vector<std::array<double, 2>>> remaining(
        segments.begin() + 1, segments.end());

    auto close = [](const std::array<double, 2>& a, const std::array<double, 2>& b) {
        return std::abs(a[0] - b[0]) < 1e-6 && std::abs(a[1] - b[1]) < 1e-6;
    };

    int maxIter = static_cast<int>(remaining.size()) * 2;
    for (int i = 0; i < maxIter && !remaining.empty(); ++i) {
        bool matched = false;
        for (size_t idx = 0; idx < remaining.size(); ++idx) {
            if (close(ring.back(), remaining[idx].front())) {
                ring.insert(ring.end(), remaining[idx].begin() + 1, remaining[idx].end());
                remaining.erase(remaining.begin() + static_cast<ptrdiff_t>(idx));
                matched = true;
                break;
            }
            if (close(ring.back(), remaining[idx].back())) {
                auto& seg = remaining[idx];
                for (int j = static_cast<int>(seg.size()) - 2; j >= 0; --j)
                    ring.push_back(seg[static_cast<size_t>(j)]);
                remaining.erase(remaining.begin() + static_cast<ptrdiff_t>(idx));
                matched = true;
                break;
            }
        }
        if (!matched) break;
    }
    return ring;
}

std::vector<std::array<double, 2>>
SettlementLoader::simplifyDP(const std::vector<std::array<double, 2>>& coords,
                              double tolerance)
{
    if (coords.size() <= 4) {
        std::vector<std::array<double, 2>> out;
        out.reserve(coords.size());
        for (auto& c : coords)
            out.push_back({std::round(c[0] * 1e5) / 1e5,
                           std::round(c[1] * 1e5) / 1e5});
        return out;
    }

    auto perpDist = [](const std::array<double, 2>& p,
                       const std::array<double, 2>& a,
                       const std::array<double, 2>& b) -> double {
        double dx = b[0] - a[0], dy = b[1] - a[1];
        if (dx == 0 && dy == 0)
            return std::hypot(p[0] - a[0], p[1] - a[1]);
        double t = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / (dx * dx + dy * dy);
        t = std::clamp(t, 0.0, 1.0);
        return std::hypot(p[0] - (a[0] + t * dx), p[1] - (a[1] + t * dy));
    };

    // Iterative Douglas-Peucker using stack
    std::vector<bool> keep(coords.size(), false);
    keep[0] = true;
    keep[coords.size() - 1] = true;

    struct Range { size_t first, last; };
    std::vector<Range> stack;
    stack.push_back({0, coords.size() - 1});

    while (!stack.empty()) {
        auto [first, last] = stack.back();
        stack.pop_back();

        double maxD = 0;
        size_t maxI = first;
        for (size_t i = first + 1; i < last; ++i) {
            double d = perpDist(coords[i], coords[first], coords[last]);
            if (d > maxD) { maxD = d; maxI = i; }
        }
        if (maxD > tolerance) {
            keep[maxI] = true;
            if (maxI - first > 1) stack.push_back({first, maxI});
            if (last - maxI > 1) stack.push_back({maxI, last});
        }
    }

    std::vector<std::array<double, 2>> result;
    for (size_t i = 0; i < coords.size(); ++i) {
        if (keep[i])
            result.push_back({std::round(coords[i][0] * 1e5) / 1e5,
                              std::round(coords[i][1] * 1e5) / 1e5});
    }
    return result;
}

QString SettlementLoader::tileKey(const TileBounds& t)
{
    return QStringLiteral("%1,%2")
        .arg(std::get<0>(t), 0, 'f', 4)
        .arg(std::get<1>(t), 0, 'f', 4);
}

std::vector<TileBounds> SettlementLoader::gridTiles(double south, double west,
                                                     double north, double east)
{
    std::vector<TileBounds> tiles;
    double g = TILE_GRID;
    double s = std::floor(south / g) * g;
    while (s < north) {
        double wCur = std::floor(west / g) * g;
        while (wCur < east) {
            tiles.emplace_back(
                std::round(s * 1e4) / 1e4,
                std::round(wCur * 1e4) / 1e4,
                std::round((s + g) * 1e4) / 1e4,
                std::round((wCur + g) * 1e4) / 1e4
            );
            wCur += g;
        }
        s += g;
    }
    return tiles;
}

} // namespace vtol
