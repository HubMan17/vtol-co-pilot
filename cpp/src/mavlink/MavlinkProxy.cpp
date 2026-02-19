#include "MavlinkProxy.h"
#include <spdlog/spdlog.h>

namespace vtol {

MavlinkProxy::MavlinkProxy(int proxyPort, QObject* parent)
    : QObject(parent)
    , m_proxyPort(proxyPort)
{
    SPDLOG_DEBUG("MavlinkProxy created, port={}", proxyPort);
}

MavlinkProxy::~MavlinkProxy()
{
    stop();
}

bool MavlinkProxy::start()
{
    if (m_running) return true;

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &MavlinkProxy::onNewConnection);

    if (!m_server->listen(QHostAddress::Any, m_proxyPort)) {
        SPDLOG_ERROR("MavlinkProxy: failed to listen on port {}: {}",
            m_proxyPort, m_server->errorString().toStdString());
        delete m_server;
        m_server = nullptr;
        return false;
    }

    m_running = true;
    SPDLOG_INFO("MavlinkProxy: listening on port {}", m_proxyPort);
    return true;
}

void MavlinkProxy::stop()
{
    if (!m_running) return;
    m_running = false;

    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }

    QMutexLocker lock(&m_mutex);
    for (auto* client : m_clients) {
        client->disconnectFromHost();
        client->deleteLater();
    }
    m_clients.clear();
    SPDLOG_INFO("MavlinkProxy stopped");
}

int MavlinkProxy::clientCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_clients.size();
}

void MavlinkProxy::broadcastToClients(const QByteArray& data)
{
    QMutexLocker lock(&m_mutex);
    QList<QTcpSocket*> dead;

    for (auto* client : m_clients) {
        if (client->state() != QAbstractSocket::ConnectedState) {
            dead.append(client);
            continue;
        }
        qint64 written = client->write(data);
        if (written < 0) {
            dead.append(client);
        }
    }

    for (auto* client : dead) {
        removeClient(client);
    }
}

void MavlinkProxy::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        auto* client = m_server->nextPendingConnection();
        QString addr = QString("%1:%2").arg(client->peerAddress().toString()).arg(client->peerPort());

        connect(client, &QTcpSocket::readyRead, this, &MavlinkProxy::onClientData);
        connect(client, &QTcpSocket::disconnected, this, &MavlinkProxy::onClientDisconnected);

        {
            QMutexLocker lock(&m_mutex);
            m_clients.append(client);
        }

        SPDLOG_INFO("MavlinkProxy: client connected: {}", addr.toStdString());
        emit clientConnected(addr);
    }
}

void MavlinkProxy::onClientData()
{
    auto* client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;

    QByteArray data = client->readAll();
    if (!data.isEmpty()) {
        emit dataFromClient(data);
    }
}

void MavlinkProxy::onClientDisconnected()
{
    auto* client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;

    QMutexLocker lock(&m_mutex);
    removeClient(client);
}

void MavlinkProxy::removeClient(QTcpSocket* client)
{
    QString addr = QString("%1:%2").arg(client->peerAddress().toString()).arg(client->peerPort());
    m_clients.removeAll(client);
    client->deleteLater();
    SPDLOG_INFO("MavlinkProxy: client disconnected: {}", addr.toStdString());
    emit clientDisconnected(addr);
}

} // namespace vtol
