#include <QApplication>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QDir>
#include <QQmlEngine>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <filesystem>

#include "core/Config.h"
#include "gui/MainWindow.h"

namespace {

void setupLogging()
{
    namespace fs = std::filesystem;
    fs::create_directories("logs");

    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_level(spdlog::level::debug);

    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/app.log", /*truncate=*/true);
    fileSink->set_level(spdlog::level::debug);

    auto logger = std::make_shared<spdlog::logger>("vtol",
        spdlog::sinks_init_list{consoleSink, fileSink});
    logger->set_level(spdlog::level::debug);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");
    logger->flush_on(spdlog::level::debug);

    spdlog::set_default_logger(logger);
    SPDLOG_INFO("VTOL Co-Pilot C++ starting...");
}

} // namespace

int main(int argc, char* argv[])
{
    // Basic render loop — avoids threaded vsync synchronization overhead
    qputenv("QSG_RENDER_LOOP", "basic");

    // Disable VSync — let the app run at max FPS
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    // Required for QQuickWidget (QML Map) to share OpenGL context
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    app.setApplicationName("VTOL Co-Pilot");
    app.setApplicationVersion("1.0.0");

    // OpenGL backend for QML
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    // Add QML import paths for vcpkg-installed Qt modules
    auto qmlDir = QDir(QApplication::applicationDirPath() + "/qml");
    if (qmlDir.exists()) {
        qputenv("QML2_IMPORT_PATH", qmlDir.absolutePath().toUtf8());
    }

    setupLogging();
    SPDLOG_INFO("Qt version: {}", qVersion());

    auto config = vtol::loadConfig();

    vtol::MainWindow window(std::move(config));
    window.show();

    SPDLOG_INFO("Main window shown, entering event loop");
    int result = app.exec();

    SPDLOG_INFO("Application exiting with code {}", result);
    return result;
}
