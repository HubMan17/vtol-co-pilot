#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMutex>
#include <QList>
#include <cstdint>

namespace vtol {

class MavlinkProxy : public QObject {
    Q_OBJECT
public:
    explicit MavlinkProxy(int proxyPort = 14550, QObject* parent = nullptr);
    ~MavlinkProxy() override;

    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const { return m_running; }
    [[nodiscard]] int clientCount() const;

public slots:
    void broadcastToClients(const QByteArray& data);

signals:
    void clientConnected(const QString& address);
    void clientDisconnected(const QString& address);
    void dataFromClient(const QByteArray& data);

private slots:
    void onNewConnection();
    void onClientData();
    void onClientDisconnected();

private:
    void removeClient(QTcpSocket* client);

    int m_proxyPort;
    bool m_running = false;
    QTcpServer* m_server = nullptr;
    QList<QTcpSocket*> m_clients;
    mutable QMutex m_mutex;
};

} // namespace vtol
