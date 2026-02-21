cmake_minimum_required(VERSION 3.21)
project(vtol-copilot VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# --- Force UTF-8 source/execution charset for MSVC ---
if(MSVC)
    add_compile_options(/utf-8)
endif()

# --- Qt6 ---
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Widgets
    Quick
    QuickWidgets
    Network
    Positioning
    Location
    Concurrent
    OpenGLWidgets
)

# --- QMapLibre (built from source for vcpkg Qt 6.10 compatibility) ---
set(QMapLibre_DIR "C:/ml/install/lib/cmake/QMapLibre")
find_package(QMapLibre REQUIRED COMPONENTS Core Widgets)

# --- Eigen3 (system or vcpkg) ---
find_package(Eigen3 REQUIRED NO_MODULE)

# --- MAVSDK ---
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(MAVSDK REQUIRED)

# --- FetchContent dependencies (spdlog, nlohmann_json) ---
include(cmake/Dependencies.cmake)

# --- Sources ---
set(SOURCES
    src/main.cpp
    src/core/Config.cpp
    src/mavlink/TelemetryState.cpp
    src/mavlink/MavlinkConnection.cpp
    src/mavlink/MavlinkProxy.cpp
    src/navigation/Calculations.cpp
    src/navigation/BatchGeometry.cpp
    src/navigation/RoutePlanner.cpp
    src/navigation/ZoneManager.cpp
    src/navigation/ZoneChecker.cpp
    src/navigation/PathPlanner.cpp
    src/autopilot/AutopilotManager.cpp
    src/gui/Theme.cpp
    src/gui/TileProxy.cpp
    src/gui/models/MapModels.cpp
    src/gui/FlightMapCanvas.cpp
    src/gui/MapBackend.cpp
    src/gui/MapWidget.cpp
    src/gui/MapLibreAdapter.cpp
    src/gui/MapLegend.cpp
    src/gui/StatusPanel.cpp
    src/gui/NotificationWidget.cpp
    src/gui/Dialogs.cpp
    src/gui/SettlementLoader.cpp
    src/gui/MainWindow.cpp
)

# --- Resources ---
qt_add_resources(RESOURCES resources/resources.qrc)

# --- Executable ---
add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${RESOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickWidgets
    Qt6::Network
    Qt6::Positioning
    Qt6::Location
    Qt6::Concurrent
    Qt6::OpenGLWidgets
    Eigen3::Eigen
    MAVSDK::mavsdk
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    QMapLibre::Core
    QMapLibre::Widgets
)

# --- Copy config/data/routes to build dir ---
# Config: copy only if not exists (user settings must not be overwritten by build)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config"
    COMMAND ${CMAKE_COMMAND}
        -Dsrc="${CMAKE_SOURCE_DIR}/config/settings.json"
        -Ddst="$<TARGET_FILE_DIR:${PROJECT_NAME}>/config/settings.json"
        -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/data"
    COMMAND ${CMAKE_COMMAND}
        -Dsrc="${CMAKE_SOURCE_DIR}/data/zones.json"
        -Ddst="$<TARGET_FILE_DIR:${PROJECT_NAME}>/data/zones.json"
        -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/routes" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/routes"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/logs"
    COMMENT "Copying config/data/routes to build directory"
)

# --- Copy MapLibre DLLs ---
set(_MAPLIBRE_BIN "C:/ml/install/bin")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_MAPLIBRE_BIN}/QMapLibre.dll"
        "${_MAPLIBRE_BIN}/QMapLibreWidgets.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
    COMMENT "Copying MapLibre DLLs..."
)

# --- Copy MAVSDK DLL ---
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/mavsdk/Release/bin/mavsdk.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
    COMMENT "Copying MAVSDK DLL..."
)

# --- Copy vcpkg runtime DLLs ---
# Derive vcpkg bin from CMAKE_PREFIX_PATH (works without vcpkg toolchain)
if(NOT _VCPKG_INSTALLED_DIR AND CMAKE_PREFIX_PATH)
    list(GET CMAKE_PREFIX_PATH 0 _FIRST_PREFIX)
    set(_VCPKG_BIN "${_FIRST_PREFIX}/bin")
else()
    set(_VCPKG_BIN "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
endif()
file(GLOB _RUNTIME_DLLS "${_VCPKG_BIN}/*.dll")
if(_RUNTIME_DLLS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
        COMMENT "Copying vcpkg runtime DLLs..."
    )
endif()

# --- Windows: copy Qt DLLs + QML modules ---
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND Qt6::windeployqt "$<TARGET_FILE:${PROJECT_NAME}>"
            --qmldir "${CMAKE_SOURCE_DIR}/resources/qml"
        COMMENT "Running windeployqt..."
    )
    # Copy vcpkg QML modules (QtLocation, QtPositioning, etc.)
    get_filename_component(_VCPKG_ROOT "${_VCPKG_BIN}" DIRECTORY)
    if(EXISTS "${_VCPKG_ROOT}/Qt6/qml")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_VCPKG_ROOT}/Qt6/qml"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/qml"
            COMMENT "Copying vcpkg QML modules..."
        )
    endif()
endif()
