import QtQuick
import QtQuick.Controls
import VtolCoPilot 1.0

Item {
    id: root
    anchors.fill: parent
    focus: true

    // Capture context property before FlightMapCanvas shadows it with its own 'backend' property
    readonly property var _backend: backend

    Keys.onEscapePressed: {
        if (backend && backend.drawingMode)
            backend.cancelDrawing();
    }

    // ── QML FPS counter (measures actual scene graph render rate) ──
    Item {
        id: fpsCounter
        visible: false
        property int frames: 0
        property int displayFps: 0

        NumberAnimation on rotation {
            from: 0; to: 360
            duration: 10000
            loops: Animation.Infinite
        }
        onRotationChanged: frames++

        Timer {
            interval: 1000
            repeat: true
            running: true
            onTriggered: {
                fpsCounter.displayFps = fpsCounter.frames;
                fpsCounter.frames = 0;
            }
        }
    }

    // ── Custom canvas map (replaces QtLocation Map) ──
    FlightMapCanvas {
        id: mapCanvas
        anchors.fill: parent
        backend: root._backend
    }

    // ── Layer panel ──
    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 10
        width: 150
        height: layerColumn.height + 16
        radius: 8
        color: "#CC111827"
        border.color: "#1E293B"
        border.width: 1
        visible: backend ? backend.showLayerPanel : true

        Column {
            id: layerColumn
            anchors.centerIn: parent
            spacing: 4

            Repeater {
                model: [
                    { label: "Трек", prop: "showTrack" },
                    { label: "Маршрут", prop: "showWaypoints" },
                    { label: "Зоны", prop: "showZones" },
                    { label: "Нас. пункты", prop: "showSettlements" }
                ]

                Row {
                    spacing: 6
                    Rectangle {
                        width: 14; height: 14
                        radius: 3
                        color: backend && backend[modelData.prop] ? "#2563EB" : "#1A2035"
                        border.color: "#334155"
                        border.width: 1
                        anchors.verticalCenter: parent.verticalCenter

                        Text {
                            anchors.centerIn: parent
                            text: backend && backend[modelData.prop] ? "\u2713" : ""
                            color: "#FFFFFF"
                            font.pixelSize: 10
                            font.bold: true
                        }

                        TapHandler {
                            onTapped: {
                                if (backend) backend[modelData.prop] = !backend[modelData.prop];
                            }
                        }
                    }

                    Text {
                        text: modelData.label
                        color: "#CBD5E1"
                        font.pixelSize: 11
                        anchors.verticalCenter: parent.verticalCenter

                        TapHandler {
                            onTapped: {
                                if (backend) backend[modelData.prop] = !backend[modelData.prop];
                            }
                        }
                    }
                }
            }
        }
    }

    // ── FPS counter ──
    Text {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        color: fpsCounter.displayFps < 30 ? "#FF6B6B" : "#88FFFFFF"
        font.pixelSize: 10
        font.family: "Consolas"
        text: fpsCounter.displayFps + " fps" + (backend && backend.fpsText !== "" ? " | " + backend.fpsText : "")
        visible: true
    }
}
