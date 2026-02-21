import QtQuick 2.15
import QtLocation 5.15
import QtPositioning 5.15
import QtQuick.Controls 2.15

Item {
    id: root
    anchors.fill: parent
    focus: true

    property real defaultLat: 59.939
    property real defaultLon: 30.315
    property int defaultZoom: 14
    property real _wheelZoomAcc: 0.0

    // Debounce bounds notifications — avoid spamming during init
    property bool _mapReady: false

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

    Map {
        id: map
        anchors.fill: parent
        zoomLevel: defaultZoom
        center: QtPositioning.coordinate(defaultLat, defaultLon)

        plugin: Plugin {
            id: osmPlugin
            name: "osm"
            PluginParameter { name: "osm.mapping.custom.host"; value: backend ? backend.tileServerUrl : "" }
            PluginParameter { name: "osm.mapping.highdpi_tiles"; value: true }
        }

        function _applyMapType() {
            if (map.supportedMapTypes.length === 0) return;
            if (backend && backend.tileServerUrl !== "") {
                map.activeMapType = map.supportedMapTypes[map.supportedMapTypes.length - 1];
                return;
            }
            map.activeMapType = map.supportedMapTypes[0];
        }

        onSupportedMapTypesChanged: _applyMapType()

        Connections {
            target: backend
            function onTileServerUrlChanged() { _applyMapType(); }
        }

        copyrightsVisible: false
        color: "#0B0F1A"

        // ── Gesture handling ──
        gesture.acceptedGestures: MapGestureArea.PanGesture |
                                   MapGestureArea.PinchGesture |
                                   MapGestureArea.FlickGesture

        gesture.onPanStarted: {
            if (backend) backend.followAircraft = false;
        }

        Component.onCompleted: {
            root._mapReady = true;
            _notifyBounds();
        }

        // ── Follow mode binding ──
        Connections {
            target: backend
            function onAircraftPositionChanged() {
                if (backend && backend.followAircraft && backend.aircraftPosition.isValid)
                    map.center = backend.aircraftPosition;
                map._updateActiveWpLine();
            }
            function onActiveWpPositionChanged() {
                map._updateActiveWpLine();
            }
            function onMapCenterRequested(lat, lon) {
                map.center = QtPositioning.coordinate(lat, lon);
            }
            function onZoomRequested(level) {
                map.zoomLevel = level;
            }
        }

        function _updateActiveWpLine() {
            if (!backend || !backend.aircraftPosition.isValid || !backend.activeWpPosition.isValid) {
                activeWpLine.path = [];
                return;
            }
            activeWpLine.path = [backend.aircraftPosition, backend.activeWpPosition];
        }

        // ── Notify backend of bounds/zoom changes ──
        onCenterChanged: _notifyBounds()
        onZoomLevelChanged: {
            _notifyBounds();
            if (backend) backend.onZoomChanged(Math.round(map.zoomLevel));
        }

        function _notifyBounds() {
            if (!root._mapReady || !backend || map.width < 1 || map.height < 1)
                return;
            var tl = map.toCoordinate(Qt.point(0, 0));
            var br = map.toCoordinate(Qt.point(map.width, map.height));
            if (isNaN(tl.latitude) || isNaN(br.latitude))
                return;
            var south = Math.min(tl.latitude, br.latitude);
            var north = Math.max(tl.latitude, br.latitude);
            var west = Math.min(tl.longitude, br.longitude);
            var east = Math.max(tl.longitude, br.longitude);
            backend.onBoundsChanged(south, west, north, east);
        }

        // ════════════════════ Map Overlays ════════════════════

        // ── Flight track (managed imperatively — no path binding) ──
        MapPolyline {
            id: trackLine
            visible: backend ? backend.showTrack : true
            line.width: 2
            line.color: "#22C55E"
        }

        Connections {
            target: backend
            function onTrackCoordinateAdded(lat, lon) {
                trackLine.addCoordinate(QtPositioning.coordinate(lat, lon));
            }
            function onTrackPathChanged() {
                trackLine.path = backend ? backend.trackPath : [];
            }
        }

        // ── Route lines ──
        MapItemView {
            model: (backend && backend.showWaypoints) ? backend.routeSegmentModel : null
            delegate: MapPolyline {
                line.width: model.segState === "active" ? 3 : 2
                line.color: model.segState === "past" ? "#55FFFFFF" :
                            model.segState === "active" ? "#FFFFFF" : "#AAFFFFFF"
                path: [
                    QtPositioning.coordinate(model.fromLat, model.fromLon),
                    QtPositioning.coordinate(model.toLat, model.toLon)
                ]
            }
        }

        // ── Waypoint markers ──
        MapItemView {
            model: (backend && backend.showWaypoints) ? backend.waypointModel : null
            delegate: MapQuickItem {
                coordinate: QtPositioning.coordinate(model.lat, model.lon)
                anchorPoint.x: 14
                anchorPoint.y: 14
                zoomLevel: 0

                sourceItem: Rectangle {
                    width: 28; height: 28
                    radius: 14
                    color: model.wpState === "past" ? "#55888888" :
                           model.wpState === "active" ? "#22C55E" : "#AAFFFFFF"
                    border.color: model.wpState === "active" ? "#FFFFFF" : "transparent"
                    border.width: model.wpState === "active" ? 2 : 0

                    Text {
                        anchors.centerIn: parent
                        text: (model.wpIndex + 1).toString()
                        color: model.wpState === "past" ? "#AAAAAA" : "#000000"
                        font.pixelSize: 12
                        font.bold: true
                    }
                }
            }
        }

        // ── Active waypoint line (aircraft → current WP) ──
        // Drawn ON TOP of route segments and waypoint markers for visibility
        MapPolyline {
            id: activeWpLine
            visible: backend ? (backend.aircraftVisible && backend.showWaypoints &&
                                backend.activeWpPosition.isValid) : false
            line.width: 3
            line.color: "#EEF59E0B"
        }

        // ── Aircraft marker ──
        MapQuickItem {
            id: aircraftMarker
            visible: backend ? backend.aircraftVisible : false
            coordinate: backend ? backend.aircraftPosition : QtPositioning.coordinate(0, 0)
            anchorPoint.x: 20
            anchorPoint.y: 20
            zoomLevel: 0

            sourceItem: Item {
                width: 40; height: 40

                Rectangle {
                    anchors.fill: parent
                    radius: 20
                    color: "#3322C55E"
                    border.color: "#22C55E"
                    border.width: 1.5
                }

                Item {
                    anchors.centerIn: parent
                    width: 24; height: 24
                    rotation: backend ? backend.aircraftHeading : 0

                    Image {
                        anchors.fill: parent
                        source: "icons/plane-green.svg"
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        antialiasing: true
                    }
                }
            }
        }

        // ── Home marker ──
        MapQuickItem {
            id: homeMarker
            visible: backend ? backend.homeVisible : false
            coordinate: backend ? backend.homePosition : QtPositioning.coordinate(0, 0)
            anchorPoint.x: 16
            anchorPoint.y: 16
            zoomLevel: 0

            sourceItem: Rectangle {
                width: 32; height: 32
                radius: 16
                color: "#332563EB"
                border.color: "#2563EB"
                border.width: 2

                Text {
                    anchors.centerIn: parent
                    text: "H"
                    color: "#2563EB"
                    font.pixelSize: 14
                    font.bold: true
                }
            }
        }

        // ── Restricted zones: dark fill (hidden at zoom ≤ 13, base opacity 60%) ──
        MapItemView {
            model: (backend && backend.showZones && map.zoomLevel >= 14) ? backend.zoneModel : null
            delegate: MapPolygon {
                border.width: 0
                color: "#CC1A0505"
                opacity: map.zoomLevel < 15 ? 0.35 : 0.6
                path: {
                    var pts = [];
                    var coords = model.zoneCoords;
                    if (coords) {
                        for (var i = 0; i < coords.length; i++)
                            pts.push(QtPositioning.coordinate(coords[i][0], coords[i][1]));
                    }
                    return pts;
                }
            }
        }

        // ── Restricted zones: dashed border segments ──
        MapItemView {
            model: (backend && backend.showZones && map.zoomLevel >= 14) ? backend.zoneBorderModel : null
            delegate: MapPolyline {
                line.width: 3
                line.color: "#DD8B1A1A"
                opacity: map.zoomLevel < 15 ? 0.45 : 0.7
                path: [
                    QtPositioning.coordinate(model.fromLat, model.fromLon),
                    QtPositioning.coordinate(model.toLat, model.toLon)
                ]
            }
        }

        // ── Settlement polygons (60% transparent fill, no solid border) ──
        MapItemView {
            model: (backend && backend.showSettlements && map.zoomLevel >= 14) ? backend.settlementPolyModel : null
            delegate: MapPolygon {
                border.width: 0
                color: "#66EF4444"
                opacity: map.zoomLevel <= 14 ? 1.0 :
                         map.zoomLevel <= 15 ? 0.8 :
                         map.zoomLevel <= 16 ? 0.5 : 0.3
                path: {
                    var pts = [];
                    var coords = model.coords;
                    if (coords) {
                        for (var i = 0; i < coords.length; i++)
                            pts.push(QtPositioning.coordinate(coords[i][0], coords[i][1]));
                    }
                    return pts;
                }
            }
        }

        // ── Settlement dashed borders ──
        MapItemView {
            model: (backend && backend.showSettlements && map.zoomLevel >= 14) ? backend.settlementBorderModel : null
            delegate: MapPolyline {
                line.width: 2
                line.color: "#CCEF4444"
                opacity: map.zoomLevel <= 14 ? 1.0 :
                         map.zoomLevel <= 15 ? 0.8 :
                         map.zoomLevel <= 16 ? 0.5 : 0.3
                path: [
                    QtPositioning.coordinate(model.fromLat, model.fromLon),
                    QtPositioning.coordinate(model.toLat, model.toLon)
                ]
            }
        }

        // ── Settlement circles (60% transparent fill) ──
        MapItemView {
            model: (backend && backend.showSettlements && map.zoomLevel >= 14) ? backend.settlementCircleModel : null
            delegate: MapCircle {
                center: QtPositioning.coordinate(model.lat, model.lon)
                radius: model.radius
                border.width: 2
                border.color: "#CCEF4444"
                color: "#66EF4444"
                opacity: map.zoomLevel <= 14 ? 1.0 :
                         map.zoomLevel <= 15 ? 0.8 :
                         map.zoomLevel <= 16 ? 0.5 : 0.3
            }
        }

        // ── Conflict segments ──
        MapItemView {
            model: backend ? backend.conflictModel : null
            delegate: MapPolyline {
                line.width: 3
                line.color: "#EF4444"
                path: [
                    QtPositioning.coordinate(model.fromLat, model.fromLon),
                    QtPositioning.coordinate(model.toLat, model.toLon)
                ]
            }
        }

        // ── Planned direct path (start -> target, intersects obstacle) ──
        MapPolyline {
            id: plannedDirectLine
            visible: backend ? backend.plannedDirectPath.length > 0 : false
            line.width: 4
            line.color: "#FDE047"
            opacity: 0.95
            path: backend ? backend.plannedDirectPath : []
        }

        // ── Conflict points with reason tooltip ──
        MapItemView {
            model: backend ? backend.conflictPointModel : null
            delegate: MapQuickItem {
                coordinate: QtPositioning.coordinate(model.lat, model.lon)
                anchorPoint.x: 12
                anchorPoint.y: 12
                zoomLevel: 0

                sourceItem: Item {
                    id: warningRoot
                    width: 24
                    height: 24

                    Rectangle {
                        anchors.fill: parent
                        radius: 12
                        color: "#FEE2E2"
                        border.width: 2
                        border.color: "#EF4444"
                    }

                    Text {
                        anchors.centerIn: parent
                        text: "!"
                        color: "#B91C1C"
                        font.pixelSize: 15
                        font.bold: true
                    }

                    MouseArea {
                        id: warningHoverAreaLegacy
                        anchors.fill: parent
                        acceptedButtons: Qt.NoButton
                        hoverEnabled: true
                    }

                    HoverHandler {
                        id: warningHover
                    }

                    Rectangle {
                        visible: warningHover.hovered || warningHoverAreaLegacy.containsMouse
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.top
                        anchors.bottomMargin: 8
                        radius: 6
                        color: "#DD111827"
                        border.width: 1
                        border.color: "#334155"
                        z: 2000
                        width: Math.min(320, tipText.implicitWidth + 14)
                        height: tipText.implicitHeight + 10

                        Text {
                            id: tipText
                            anchors.centerIn: parent
                            width: parent.width - 10
                            text: model.reason
                            color: "#F8FAFC"
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
            }
        }

        // ── Avoidance path ──
        MapPolyline {
            id: avoidanceLine
            visible: backend ? backend.avoidancePath.length > 0 : false
            line.width: 3
            line.color: "#F59E0B"
            path: backend ? backend.avoidancePath : []
        }

        // ── Drawing mode overlay ──
        MapPolyline {
            id: drawingPreview
            visible: backend ? backend.drawingMode : false
            line.width: 2
            line.color: "#FFFF00"
            path: backend ? backend.drawingPath : []
        }

        MapItemView {
            model: backend ? backend.drawingVertexModel : null
            delegate: MapQuickItem {
                coordinate: QtPositioning.coordinate(model.lat, model.lon)
                anchorPoint.x: model.vertexIndex === 0 ? 8 : 5
                anchorPoint.y: model.vertexIndex === 0 ? 8 : 5
                zoomLevel: 0
                sourceItem: Rectangle {
                    width: model.vertexIndex === 0 ? 16 : 10
                    height: width
                    radius: width / 2
                    color: model.vertexIndex === 0 ? "#FFFF00" : "#FFFFFF"
                    border.color: "#000000"
                    border.width: 1
                }
            }
        }

        // ── Editing mode: vertex markers ──
        MapItemView {
            model: backend ? backend.editingVertexModel : null
            delegate: MapQuickItem {
                id: editVtxItem
                coordinate: QtPositioning.coordinate(model.lat, model.lon)
                anchorPoint.x: 8
                anchorPoint.y: 8
                zoomLevel: 0

                property int vtxIdx: model.vertexIndex

                sourceItem: Rectangle {
                    width: 16; height: 16
                    radius: 8
                    color: vtxDragArea.pressed ? "#EF4444" : "#FFFFFF"
                    border.color: "#EF4444"
                    border.width: 2

                    MouseArea {
                        id: vtxDragArea
                        anchors.fill: parent
                        anchors.margins: -12
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        preventStealing: true

                        property point startScreenPos
                        property bool dragging: false

                        onPressed: {
                            if (mouse.button === Qt.RightButton) {
                                if (backend) backend.deleteEditingVertex(editVtxItem.vtxIdx);
                                mouse.accepted = true;
                                return;
                            }
                            startScreenPos = mapToItem(map, mouse.x, mouse.y);
                            dragging = true;
                            mouse.accepted = true;
                        }

                        onPositionChanged: {
                            if (!dragging) return;
                            var screenPt = mapToItem(map, mouse.x, mouse.y);
                            var coord = map.toCoordinate(screenPt);
                            if (backend && !isNaN(coord.latitude))
                                backend.moveEditingVertex(editVtxItem.vtxIdx, coord.latitude, coord.longitude);
                        }

                        onReleased: {
                            dragging = false;
                        }
                    }
                }
            }
        }

        // ── Editing mode: midpoint markers ──
        MapItemView {
            model: backend ? backend.editingMidpointModel : null
            delegate: MapQuickItem {
                coordinate: QtPositioning.coordinate(model.lat, model.lon)
                anchorPoint.x: 5
                anchorPoint.y: 5
                zoomLevel: 0

                property int midIdx: model.midpointIndex

                sourceItem: Rectangle {
                    width: 10; height: 10
                    radius: 5
                    color: "#888888"
                    border.color: "#FFFFFF"
                    border.width: 1

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -8
                        onClicked: {
                            if (backend) backend.insertEditingVertex(midIdx);
                        }
                    }
                }
            }
        }

        // ════════════════════ Mouse handling ════════════════════

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            hoverEnabled: true
            onWheel: {
                var dy = wheel.angleDelta.y;
                if (dy === 0) {
                    wheel.accepted = false;
                    return;
                }

                // Consume wheel to avoid fractional native zoom that creates tile seam artifacts.
                if (root._wheelZoomAcc * dy < 0)
                    root._wheelZoomAcc = 0.0;
                root._wheelZoomAcc += dy / 120.0;

                var steps = root._wheelZoomAcc > 0
                          ? Math.floor(root._wheelZoomAcc)
                          : Math.ceil(root._wheelZoomAcc);
                if (steps !== 0) {
                    root._wheelZoomAcc -= steps;
                    var target = map.zoomLevel + steps;
                    if (target < map.minimumZoomLevel) target = map.minimumZoomLevel;
                    if (target > map.maximumZoomLevel) target = map.maximumZoomLevel;
                    map.zoomLevel = target;
                }
                wheel.accepted = true;
            }
            onPositionChanged: {
                if (!backend) return;
                var coord = map.toCoordinate(Qt.point(mouse.x, mouse.y));
                if (!isNaN(coord.latitude))
                    backend.onMouseMove(coord.latitude, coord.longitude);
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: {
                if (!backend) return;
                var coord = map.toCoordinate(Qt.point(mouse.x, mouse.y));
                if (!isNaN(coord.latitude))
                    backend.onContextMenu(coord.latitude, coord.longitude, mouse.x, mouse.y);
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            visible: backend ? (backend.drawingMode || backend.leftClickMode) : false
            enabled: visible

            onClicked: {
                if (!backend) return;
                var coord = map.toCoordinate(Qt.point(mouse.x, mouse.y));
                if (!isNaN(coord.latitude))
                    backend.onMapClick(coord.latitude, coord.longitude);
            }

            onDoubleClicked: {
                if (!backend) return;
                var coord = map.toCoordinate(Qt.point(mouse.x, mouse.y));
                if (!isNaN(coord.latitude))
                    backend.onMapDoubleClick(coord.latitude, coord.longitude);
            }
        }

        // ── Obstacle warning icons (⚠) ──
        // Placed AFTER full-screen MouseAreas so delegates sit on top in z-order
        // and can receive hover events.
        MapItemView {
            model: (backend && map.zoomLevel >= 10 && map.zoomLevel <= 16)
                   ? backend.obstacleWarningModel : null
            delegate: MapQuickItem {
                coordinate: QtPositioning.coordinate(model.lat, model.lon)
                anchorPoint.x: 16; anchorPoint.y: 16
                zoomLevel: 0

                visible: {
                    if (!backend) return false;
                    if (model.source === "zone") return backend.showZones;
                    return backend.showSettlements;
                }

                sourceItem: Item {
                    width: 32; height: 32

                    opacity: {
                        if (!backend || !backend.aircraftVisible || !backend.aircraftPosition.isValid)
                            return 1.0;
                        var dist = backend.aircraftPosition.distanceTo(
                            QtPositioning.coordinate(model.lat, model.lon));
                        return Math.max(0.0, Math.min(1.0, (dist - 1000) / 4000));
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: 24; height: 24
                        radius: 4
                        color: model.source === "zone" ? "#FEF3C7" : "#FEE2E2"
                        border.width: 2
                        border.color: model.source === "zone" ? "#F59E0B" : "#EF4444"

                        Text {
                            anchors.centerIn: parent
                            text: "\u26A0"
                            color: model.source === "zone" ? "#92400E" : "#B91C1C"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    MouseArea {
                        id: obstWarnMouse
                        anchors.fill: parent
                        anchors.margins: -6
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        preventStealing: true
                        onClicked: {
                            obstTipRect.visible = !obstTipRect.visible;
                            mouse.accepted = true;
                        }
                    }

                    Rectangle {
                        id: obstTipRect
                        visible: obstWarnMouse.containsMouse
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.top
                        anchors.bottomMargin: 4
                        radius: 6
                        color: "#DD111827"
                        border.width: 1
                        border.color: "#334155"
                        z: 2000
                        width: Math.min(320, obstTipText.implicitWidth + 14)
                        height: obstTipText.implicitHeight + 10

                        Text {
                            id: obstTipText
                            anchors.centerIn: parent
                            width: parent.width - 10
                            text: model.tooltip
                            color: "#F8FAFC"
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
            }
        }

        // ════════════════════ UI Panels ════════════════════

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

                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -4
                                onClicked: {
                                    if (backend) backend[modelData.prop] = !backend[modelData.prop];
                                }
                            }
                        }

                        Text {
                            text: modelData.label
                            color: "#CBD5E1"
                            font.pixelSize: 11
                            anchors.verticalCenter: parent.verticalCenter

                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    if (backend) backend[modelData.prop] = !backend[modelData.prop];
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── FPS counter (QML render rate + Python update stats) ──
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
}
