#include "MapWidget.h"
#include "FlightMapCanvas.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSurfaceFormat>
#include <spdlog/spdlog.h>

namespace vtol {

MapWidget::MapWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_quickWidget = new QQuickWidget(this);
    m_quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);

    // Register custom QML types
    qmlRegisterType<FlightMapCanvas>("VtolCoPilot", 1, 0, "FlightMapCanvas");

    // Set MapBackend as QML context property (available before QML load)
    m_quickWidget->rootContext()->setContextProperty("backend", &m_backend);

    layout->addWidget(m_quickWidget);
}

void MapWidget::setTileServerUrl(const QString& url)
{
    m_backend.setTileServer(url);
}

void MapWidget::loadQml()
{
    m_quickWidget->setSource(QUrl("qrc:/qml/FlightMap.qml"));

    if (m_quickWidget->status() == QQuickWidget::Error) {
        for (const auto& err : m_quickWidget->errors())
            SPDLOG_ERROR("[MapWidget] QML error: {}", err.toString().toStdString());
    }
}

} // namespace vtol
