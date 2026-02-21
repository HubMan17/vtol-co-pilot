#include "TileProxy.h"
#include <QTcpSocket>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QDir>
#include <QUrl>
#include <QPointer>
#include <QCoreApplication>
#include <spdlog/spdlog.h>

namespace vtol {

TileProxy::TileProxy(QObject* parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &TileProxy::onNewConnection);
}

int TileProxy::startProxy()
{
    if (!m_server.listen(QHostAddress::LocalHost, 0)) {
        SPDLOG_ERROR("[TileProxy] Failed to listen: {}", m_server.errorString().toStdString());
        return -1;
    }
    m_port = m_server.serverPort();
    SPDLOG_INFO("[TileProxy] Google Hybrid proxy on http://127.0.0.1:{}", m_port);
    return m_port;
}

QString TileProxy::baseUrl() const
{
    return QString("http://127.0.0.1:%1").arg(m_port);
}

void TileProxy::onNewConnection()
{
    while (auto* socket = m_server.nextPendingConnection()) {
        // Prevent double-processing: disconnect readyRead after first complete read
        auto* conn = new QMetaObject::Connection;
        *conn = connect(socket, &QTcpSocket::readyRead, this, [this, socket, conn]() {
            disconnect(*conn);
            delete conn;
            QByteArray data = socket->readAll();
            handleRequest(socket, data);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

void TileProxy::handleRequest(QTcpSocket* socket, const QByteArray& requestData)
{
    QString request = QString::fromUtf8(requestData);
    QStringList lines = request.split("\r\n");
    if (lines.isEmpty()) {
        sendError(socket, 400, "Bad Request");
        return;
    }

    QStringList parts = lines[0].split(' ');
    if (parts.size() < 2 || parts[0] != "GET") {
        sendError(socket, 400, "Bad Request");
        return;
    }

    QString path = parts[1];
    int qIdx = path.indexOf('?');
    QString query;
    if (qIdx >= 0) {
        query = path.mid(qIdx + 1);
        path = path.left(qIdx);
    }

    path = path.trimmed();
    while (path.startsWith('/')) path = path.mid(1);
    while (path.endsWith('/')) path.chop(1);

    QStringList segments = path.split('/');
    if (!segments.isEmpty()) {
        QString& last = segments.last();
        int dotIdx = last.lastIndexOf('.');
        if (dotIdx > 0) last = last.left(dotIdx);
    }

    if (segments.size() != 3) {
        sendError(socket, 404, "Not Found");
        return;
    }

    bool ok1, ok2, ok3;
    int z = segments[0].toInt(&ok1);
    int x = segments[1].toInt(&ok2);

    QString yStr = segments[2];
    int scale = 1;
    if (yStr.endsWith("@2x")) {
        yStr.chop(3);
        scale = 2;
    }
    int y = yStr.toInt(&ok3);

    if (!ok1 || !ok2 || !ok3) {
        sendError(socket, 404, "Not Found");
        return;
    }

    if (query.contains("scale=2")) scale = 2;

    serveTile(socket, z, x, y, scale);
}

void TileProxy::serveTile(QTcpSocket* socket, int z, int x, int y, int scale)
{
    // Check disk cache first
    QString cPath = cachePath(z, x, y, scale);
    QFile cached(cPath);
    if (cached.exists() && cached.open(QIODevice::ReadOnly)) {
        QByteArray tile = cached.readAll();
        cached.close();
        if (tile.size() > 100) {
            QByteArray ct = "image/jpeg";
            if (tile.size() > 4 && tile[0] == '\x89' && tile[1] == 'P')
                ct = "image/png";
            sendTileResponse(socket, tile, ct);
            return;
        }
        QFile::remove(cPath);
    }

    // Throttle concurrent fetches to avoid UI freezes during rapid zoom
    if (m_pendingFetches >= MAX_CONCURRENT_FETCHES) {
        m_fetchQueue.append({QPointer<QTcpSocket>(socket), z, x, y, scale});
        return;
    }

    // Fetch from Google — use QPointer to safely track socket lifetime
    QPointer<QTcpSocket> safeSocket(socket);
    m_pendingFetches++;

    QUrl url(googleUrl(z, x, y, scale));
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
    req.setRawHeader("Referer", "https://www.google.com/");
    req.setRawHeader("Accept", "image/avif,image/apng,image/png,image/jpeg,*/*;q=0.8");

    QNetworkReply* reply = m_nam.get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply, safeSocket, z, x, y, scale]() {
        reply->deleteLater();
        m_pendingFetches--;

        // Socket destroyed while we were fetching? Just cache the tile.
        QByteArray rawTile;
        bool socketAlive = safeSocket && safeSocket->isOpen();

        if (reply->error() != QNetworkReply::NoError) {
            SPDLOG_WARN("[TileProxy] fetch failed z={} x={} y={}: {}",
                         z, x, y, reply->errorString().toStdString());
            if (socketAlive) sendError(safeSocket, 502, "Bad Gateway");
            processNextQueued();
            return;
        }

        rawTile = reply->readAll();

        if (rawTile.size() < 100) {
            SPDLOG_WARN("[TileProxy] Too small response: size={}", rawTile.size());
            if (socketAlive) sendError(safeSocket, 502, "Bad Gateway");
            processNextQueued();
            return;
        }

        QString ct = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (m_loggedRequests < 3) {
            SPDLOG_INFO("[TileProxy] Tile z={} x={} y={}: {}B ct={}",
                         z, x, y, rawTile.size(), ct.toStdString());
            m_loggedRequests++;
        }

        // Save to disk cache
        QString cPath = cachePath(z, x, y, scale);
        QDir().mkpath(QFileInfo(cPath).path());
        QFile f(cPath);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(rawTile);
            f.close();
        }

        if (socketAlive)
            sendTileResponse(safeSocket, rawTile, ct.toUtf8());

        processNextQueued();
    });
}

void TileProxy::processNextQueued()
{
    while (!m_fetchQueue.isEmpty() && m_pendingFetches < MAX_CONCURRENT_FETCHES) {
        auto req = m_fetchQueue.takeFirst();
        if (req.socket && req.socket->isOpen())
            serveTile(req.socket, req.z, req.x, req.y, req.scale);
    }
}

void TileProxy::sendTileResponse(QTcpSocket* socket, const QByteArray& tile, const QByteArray& contentType)
{
    QByteArray response;
    response.append("HTTP/1.1 200 OK\r\n");
    response.append("Content-Type: " + contentType + "\r\n");
    response.append("Content-Length: " + QByteArray::number(tile.size()) + "\r\n");
    response.append("Cache-Control: max-age=604800\r\n");
    response.append("Access-Control-Allow-Origin: *\r\n");
    response.append("Connection: close\r\n");
    response.append("\r\n");
    response.append(tile);

    socket->write(response);
    socket->flush();
    // Let the client close first (Connection: close), or auto-close after timeout
    connect(socket, &QTcpSocket::bytesWritten, socket, [socket]() {
        if (socket->bytesToWrite() == 0)
            socket->disconnectFromHost();
    });
}

void TileProxy::sendError(QTcpSocket* socket, int code, const QByteArray& reason)
{
    QByteArray response;
    response.append("HTTP/1.1 " + QByteArray::number(code) + " " + reason + "\r\n");
    response.append("Content-Length: 0\r\n");
    response.append("Connection: close\r\n");
    response.append("\r\n");

    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

QString TileProxy::cachePath(int z, int x, int y, int scale)
{
    return QCoreApplication::applicationDirPath() + "/" + CACHE_DIR
           + "/" + QString::number(z) + "/" + QString::number(x)
           + "_" + QString::number(y) + "_s" + QString::number(scale) + ".tile";
}

QString TileProxy::googleUrl(int z, int x, int y, int scale)
{
    int server = x % 4;
    return QString("https://mt%1.google.com/vt/lyrs=y&z=%2&x=%3&y=%4&scale=%5")
           .arg(server).arg(z).arg(x).arg(y).arg(scale);
}

} // namespace vtol
