#pragma once

#include <QObject>
#include <QTcpServer>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>
#include <QVector>

class QTcpSocket;

namespace vtol {

/// Localhost HTTP tile proxy — feeds Google Hybrid tiles to Qt's OSM plugin.
/// Qt's OSM plugin sends requests without browser headers, so Google blocks them.
/// This proxy receives standard z/x/y tile requests and forwards them with proper headers.
class TileProxy : public QObject {
    Q_OBJECT
public:
    explicit TileProxy(QObject* parent = nullptr);
    ~TileProxy() override = default;

    /// Start proxy on a free port. Returns port number, or -1 on failure.
    int startProxy();

    /// Get the port the proxy is listening on.
    [[nodiscard]] int port() const { return m_port; }

    /// Get full base URL, e.g. "http://127.0.0.1:12345"
    [[nodiscard]] QString baseUrl() const;

private slots:
    void onNewConnection();

private:
    void handleRequest(class QTcpSocket* socket, const QByteArray& requestData);
    void serveTile(class QTcpSocket* socket, int z, int x, int y, int scale);
    void sendTileResponse(class QTcpSocket* socket, const QByteArray& tile, const QByteArray& contentType);
    void sendError(class QTcpSocket* socket, int code, const QByteArray& reason);

    static QString cachePath(int z, int x, int y, int scale);
    static QString googleUrl(int z, int x, int y, int scale);

    void processNextQueued();

    QTcpServer m_server;
    QNetworkAccessManager m_nam;
    int m_port = 0;
    int m_loggedRequests = 0;
    int m_pendingFetches = 0;

    struct QueuedRequest {
        QPointer<QTcpSocket> socket;
        int z, x, y, scale;
    };
    QVector<QueuedRequest> m_fetchQueue;

    static constexpr int MAX_CONCURRENT_FETCHES = 8;
    static constexpr auto CACHE_DIR = "cache/tiles";
};

} // namespace vtol
