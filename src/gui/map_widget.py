from PyQt5.QtWidgets import QWidget, QVBoxLayout, QMenu, QAction
from PyQt5.QtWebEngineWidgets import QWebEngineView
from PyQt5.QtWebChannel import QWebChannel
from PyQt5.QtCore import QObject, pyqtSlot, pyqtSignal, QPoint
from PyQt5.QtGui import QCursor
import json


class MapBridge(QObject):
    position_clicked = pyqtSignal(float, float)
    context_menu_requested = pyqtSignal(float, float, int, int)

    @pyqtSlot(float, float)
    def onMapClick(self, lat, lon):
        self.position_clicked.emit(lat, lon)

    @pyqtSlot(float, float, int, int)
    def onContextMenu(self, lat, lon, screen_x, screen_y):
        self.context_menu_requested.emit(lat, lon, screen_x, screen_y)


class MapWidget(QWidget):
    set_position_requested = pyqtSignal(float, float)
    add_waypoint_requested = pyqtSignal(float, float)
    set_home_requested = pyqtSignal(float, float)
    center_map_requested = pyqtSignal(float, float)

    def __init__(self, center: tuple = (59.939, 30.315), zoom: int = 14):
        super().__init__()
        self.center = center
        self.zoom = zoom
        self._context_lat = 0.0
        self._context_lon = 0.0
        self._setup_ui()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self.web_view = QWebEngineView()

        self.bridge = MapBridge()
        self.channel = QWebChannel()
        self.channel.registerObject("bridge", self.bridge)
        self.web_view.page().setWebChannel(self.channel)

        self.bridge.context_menu_requested.connect(self._show_context_menu)

        html = self._generate_html()
        self.web_view.setHtml(html)

        layout.addWidget(self.web_view)

    def _show_context_menu(self, lat: float, lon: float, screen_x: int, screen_y: int):
        self._context_lat = lat
        self._context_lon = lon

        menu = QMenu(self)

        action_set_pos = QAction("Установить позицию здесь", self)
        action_set_pos.triggered.connect(self._on_set_position)
        menu.addAction(action_set_pos)

        action_add_wp = QAction("Добавить точку маршрута", self)
        action_add_wp.triggered.connect(self._on_add_waypoint)
        menu.addAction(action_add_wp)

        action_set_home = QAction("Установить дом", self)
        action_set_home.triggered.connect(self._on_set_home)
        menu.addAction(action_set_home)

        menu.addSeparator()

        action_center = QAction("Центрировать карту", self)
        action_center.triggered.connect(self._on_center_map)
        menu.addAction(action_center)

        action_clear_track = QAction("Очистить трек", self)
        action_clear_track.triggered.connect(self._on_clear_track)
        menu.addAction(action_clear_track)

        menu.popup(QCursor.pos())

    def _on_set_position(self):
        self.set_position_requested.emit(self._context_lat, self._context_lon)

    def _on_add_waypoint(self):
        self.add_waypoint_requested.emit(self._context_lat, self._context_lon)

    def _on_set_home(self):
        self.set_home_requested.emit(self._context_lat, self._context_lon)

    def _on_center_map(self):
        self.center_on(self._context_lat, self._context_lon)

    def _on_clear_track(self):
        self.clear_track()

    def _generate_html(self) -> str:
        return f'''
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
    <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
    <script src="qrc:///qtwebchannel/qwebchannel.js"></script>
    <style>
        body {{ margin: 0; padding: 0; }}
        #map {{ width: 100%; height: 100vh; }}
        .aircraft-icon {{
            width: 32px;
            height: 32px;
            margin-left: -16px;
            margin-top: -16px;
        }}
        .wp-tooltip {{
            background: rgba(0, 0, 0, 0.85);
            border: 1px solid #ff6600;
            border-radius: 4px;
            padding: 6px 10px;
            font-family: Consolas, monospace;
            font-size: 12px;
            color: #fff;
            white-space: nowrap;
        }}
        .wp-tooltip-active {{
            border-color: #00ff00;
            background: rgba(0, 100, 0, 0.9);
        }}
        .wp-number {{
            font-size: 11px;
            font-weight: bold;
            color: #fff;
            text-shadow: 1px 1px 2px #000;
        }}
    </style>
</head>
<body>
    <div id="map"></div>
    <script>
        var map = L.map('map', {{attributionControl: false}}).setView([{self.center[0]}, {self.center[1]}], {self.zoom});

        L.tileLayer('https://{{s}}.google.com/vt/lyrs=s,h&x={{x}}&y={{y}}&z={{z}}', {{
            maxZoom: 20,
            subdomains: ['mt0', 'mt1', 'mt2', 'mt3']
        }}).addTo(map);

        var aircraftMarker = null;
        var trackLine = null;
        var trackPoints = [];
        var waypointMarkers = [];
        var routeLines = [];
        var activeWaypointLine = null;
        var waypointData = [];
        var activeWaypointIdx = 0;
        var followAircraft = false;
        var lastHeading = 0;
        var lastAircraftPos = null;

        function createAircraftIcon(heading) {{
            return L.divIcon({{
                html: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32" style="transform: rotate(${{heading}}deg);">
                    <path d="M16 2 L14 12 L4 14 L4 18 L14 16 L14 26 L10 28 L10 30 L16 28 L22 30 L22 28 L18 26 L18 16 L28 18 L28 14 L18 12 Z"
                          fill="#00ff00" stroke="#000" stroke-width="1"/>
                </svg>`,
                className: 'aircraft-icon',
                iconSize: [32, 32],
                iconAnchor: [16, 16]
            }});
        }}

        function updateAircraft(lat, lon, heading) {{
            lastAircraftPos = [lat, lon];

            if (!aircraftMarker) {{
                aircraftMarker = L.marker([lat, lon], {{
                    icon: createAircraftIcon(heading),
                    zIndexOffset: 1000
                }}).addTo(map);
            }} else {{
                aircraftMarker.setLatLng([lat, lon]);
                if (Math.abs(heading - lastHeading) > 1) {{
                    aircraftMarker.setIcon(createAircraftIcon(heading));
                    lastHeading = heading;
                }}
            }}

            trackPoints.push([lat, lon]);
            if (trackPoints.length > 1000) trackPoints.shift();

            if (trackLine) {{
                trackLine.setLatLngs(trackPoints);
            }} else {{
                trackLine = L.polyline(trackPoints, {{color: '#00ff00', weight: 2}}).addTo(map);
            }}

            updateActiveWaypointLine();

            if (followAircraft) {{
                map.panTo([lat, lon], {{animate: false}});
            }}
        }}

        function setFollowMode(enabled) {{
            followAircraft = enabled;
        }}

        function setAircraftPosition(lat, lon) {{
            if (aircraftMarker) {{
                aircraftMarker.setLatLng([lat, lon]);
            }}
            trackPoints = [[lat, lon]];
            if (trackLine) {{
                trackLine.setLatLngs(trackPoints);
            }}
            map.setView([lat, lon], map.getZoom());
        }}

        function clearTrack() {{
            trackPoints = [];
            if (trackLine) {{
                trackLine.setLatLngs([]);
            }}
        }}

        function formatTooltip(wp, index, isActive) {{
            var actionNames = {{
                'FLYTHROUGH': 'Пролёт',
                'ORBIT_ALTITUDE': 'Кружить до высоты',
                'ORBIT_TURNS': 'Кружить N кругов',
                'ORBIT_INFINITE': 'Кружить бесконечно'
            }};
            var actionName = wp.action_name || actionNames[wp.action] || wp.action;

            var html = '<div class="wp-tooltip' + (isActive ? ' wp-tooltip-active' : '') + '">';
            html += '<b>Точка ' + (index + 1) + '</b><br>';
            html += 'Высота: ' + wp.altitude + ' м<br>';
            html += 'Тип: ' + actionName;

            if (wp.action === 'ORBIT_TURNS') {{
                html += '<br>Кругов: ' + wp.orbit_turns;
                html += '<br>Радиус: ' + wp.orbit_radius + ' м';
            }} else if (wp.action === 'ORBIT_ALTITUDE') {{
                html += '<br>До высоты: ' + wp.target_altitude + ' м';
                html += '<br>Радиус: ' + wp.orbit_radius + ' м';
            }} else if (wp.action === 'ORBIT_INFINITE') {{
                html += '<br>Радиус: ' + wp.orbit_radius + ' м';
            }}

            html += '</div>';
            return html;
        }}

        function setWaypoints(waypoints, activeIdx) {{
            waypointMarkers.forEach(m => map.removeLayer(m));
            waypointMarkers = [];
            routeLines.forEach(l => map.removeLayer(l));
            routeLines = [];

            waypointData = waypoints;
            if (activeIdx !== undefined) activeWaypointIdx = activeIdx;

            waypoints.forEach((wp, i) => {{
                var isPast = i < activeWaypointIdx;
                var isActive = i === activeWaypointIdx;
                var isFuture = i > activeWaypointIdx;

                var fillColor = isPast ? '#888888' : (isActive ? '#00ff00' : '#ff6600');
                var opacity = isPast ? 0.5 : 0.9;

                var marker = L.circleMarker([wp.lat, wp.lon], {{
                    radius: isActive ? 10 : 8,
                    fillColor: fillColor,
                    color: isActive ? '#00ff00' : '#fff',
                    weight: isActive ? 3 : 2,
                    fillOpacity: opacity
                }}).addTo(map);

                var numberIcon = L.divIcon({{
                    html: '<span class="wp-number">' + (i + 1) + '</span>',
                    className: '',
                    iconSize: [20, 20],
                    iconAnchor: [10, 10]
                }});
                var numberMarker = L.marker([wp.lat, wp.lon], {{
                    icon: numberIcon,
                    interactive: false
                }}).addTo(map);

                marker.bindTooltip(formatTooltip(wp, i, isActive), {{
                    permanent: false,
                    direction: 'top',
                    offset: [0, -10],
                    className: ''
                }});

                waypointMarkers.push(marker);
                waypointMarkers.push(numberMarker);
            }});

            for (var i = 0; i < waypoints.length - 1; i++) {{
                var isPastSegment = i < activeWaypointIdx - 1;
                var isActiveSegment = i === activeWaypointIdx - 1;
                var isFutureSegment = i >= activeWaypointIdx;

                var color, weight, opacity, dashArray;

                if (isPastSegment) {{
                    color = '#888888';
                    weight = 2;
                    opacity = 0.4;
                    dashArray = null;
                }} else if (isActiveSegment) {{
                    color = '#00ffff';
                    weight = 3;
                    opacity = 0.9;
                    dashArray = null;
                }} else {{
                    color = '#ff6600';
                    weight = 2;
                    opacity = 0.7;
                    dashArray = '8, 8';
                }}

                var line = L.polyline([
                    [waypoints[i].lat, waypoints[i].lon],
                    [waypoints[i + 1].lat, waypoints[i + 1].lon]
                ], {{
                    color: color,
                    weight: weight,
                    opacity: opacity,
                    dashArray: dashArray
                }}).addTo(map);
                routeLines.push(line);
            }}

            updateActiveWaypointLine();
        }}

        function updateActiveWaypoint(idx) {{
            activeWaypointIdx = idx;
            if (waypointData.length > 0) {{
                setWaypoints(waypointData, idx);
            }}
        }}

        function updateActiveWaypointLine() {{
            if (activeWaypointLine) {{
                map.removeLayer(activeWaypointLine);
                activeWaypointLine = null;
            }}

            if (lastAircraftPos && waypointData.length > activeWaypointIdx) {{
                var wp = waypointData[activeWaypointIdx];
                activeWaypointLine = L.polyline([
                    lastAircraftPos,
                    [wp.lat, wp.lon]
                ], {{
                    color: '#ff00ff',
                    weight: 2,
                    opacity: 0.8,
                    dashArray: '4, 8'
                }}).addTo(map);
            }}
        }}

        function addWaypoint(lat, lon, index, wpData) {{
            var marker = L.circleMarker([lat, lon], {{
                radius: 8,
                fillColor: '#ff6600',
                color: '#fff',
                weight: 2,
                fillOpacity: 0.8
            }}).addTo(map);

            var tooltip = wpData ? formatTooltip(wpData, index - 1, false) : 'Точка ' + index;
            marker.bindTooltip(tooltip, {{
                permanent: false,
                direction: 'top',
                offset: [0, -10]
            }});

            var numberIcon = L.divIcon({{
                html: '<span class="wp-number">' + index + '</span>',
                className: '',
                iconSize: [20, 20],
                iconAnchor: [10, 10]
            }});
            var numberMarker = L.marker([lat, lon], {{
                icon: numberIcon,
                interactive: false
            }}).addTo(map);

            waypointMarkers.push(marker);
            waypointMarkers.push(numberMarker);

            if (wpData) {{
                waypointData.push(wpData);
            }}
        }}

        function centerOn(lat, lon) {{
            map.setView([lat, lon], map.getZoom());
        }}

        var homeMarker = null;

        function setHomeMarker(lat, lon) {{
            if (homeMarker) {{
                homeMarker.setLatLng([lat, lon]);
            }} else {{
                var homeIcon = L.divIcon({{
                    html: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24">
                        <path d="M12 3L4 9v12h5v-7h6v7h5V9l-8-6z" fill="#ff0000" stroke="#fff" stroke-width="1"/>
                    </svg>`,
                    className: 'home-icon',
                    iconSize: [24, 24],
                    iconAnchor: [12, 24]
                }});
                homeMarker = L.marker([lat, lon], {{icon: homeIcon, zIndexOffset: 500}}).addTo(map);
                homeMarker.bindTooltip('Дом', {{permanent: false, direction: 'top'}});
            }}
        }}

        var bridge = null;
        new QWebChannel(qt.webChannelTransport, function(channel) {{
            bridge = channel.objects.bridge;
        }});

        map.on('click', function(e) {{
            if (bridge) {{
                bridge.onMapClick(e.latlng.lat, e.latlng.lng);
            }}
        }});

        map.on('contextmenu', function(e) {{
            if (bridge) {{
                bridge.onContextMenu(e.latlng.lat, e.latlng.lng, e.originalEvent.screenX, e.originalEvent.screenY);
            }}
        }});
    </script>
</body>
</html>
'''

    def update_aircraft(self, lat: float, lon: float, heading: float):
        self.web_view.page().runJavaScript(f"updateAircraft({lat}, {lon}, {heading});")

    def set_aircraft_position(self, lat: float, lon: float):
        self.web_view.page().runJavaScript(f"setAircraftPosition({lat}, {lon});")

    def clear_track(self):
        self.web_view.page().runJavaScript("clearTrack();")

    def set_waypoints(self, waypoints: list, active_idx: int = 0):
        wp_json = json.dumps(waypoints)
        self.web_view.page().runJavaScript(f"setWaypoints({wp_json}, {active_idx});")

    def update_active_waypoint(self, index: int):
        self.web_view.page().runJavaScript(f"updateActiveWaypoint({index});")

    def add_waypoint(self, lat: float, lon: float, index: int, wp_data: dict = None):
        if wp_data:
            wp_json = json.dumps(wp_data)
            self.web_view.page().runJavaScript(f"addWaypoint({lat}, {lon}, {index}, {wp_json});")
        else:
            self.web_view.page().runJavaScript(f"addWaypoint({lat}, {lon}, {index}, null);")

    def center_on(self, lat: float, lon: float):
        self.web_view.page().runJavaScript(f"centerOn({lat}, {lon});")

    def set_follow_mode(self, enabled: bool):
        self.web_view.page().runJavaScript(f"setFollowMode({'true' if enabled else 'false'});")

    def set_home_marker(self, lat: float, lon: float):
        self.web_view.page().runJavaScript(f"setHomeMarker({lat}, {lon});")
