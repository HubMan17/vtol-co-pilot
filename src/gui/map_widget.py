from PyQt5.QtWidgets import QWidget, QVBoxLayout
from PyQt5.QtWebEngineWidgets import QWebEngineView
from PyQt5.QtWebChannel import QWebChannel
from PyQt5.QtCore import QObject, pyqtSlot, pyqtSignal, QUrl
from pathlib import Path
import json


class MapBridge(QObject):
    position_clicked = pyqtSignal(float, float)

    @pyqtSlot(float, float)
    def onMapClick(self, lat, lon):
        self.position_clicked.emit(lat, lon)


class MapWidget(QWidget):
    def __init__(self, center: tuple = (59.939, 30.315), zoom: int = 14):
        super().__init__()
        self.center = center
        self.zoom = zoom
        self._setup_ui()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self.web_view = QWebEngineView()

        self.bridge = MapBridge()
        self.channel = QWebChannel()
        self.channel.registerObject("bridge", self.bridge)
        self.web_view.page().setWebChannel(self.channel)

        html = self._generate_html()
        self.web_view.setHtml(html)

        layout.addWidget(self.web_view)

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

        var aircraftSvg = `
            <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">
                <path d="M16 2 L14 12 L4 14 L4 18 L14 16 L14 26 L10 28 L10 30 L16 28 L22 30 L22 28 L18 26 L18 16 L28 18 L28 14 L18 12 Z"
                      fill="#00ff00" stroke="#000" stroke-width="1"/>
            </svg>
        `;

        var aircraftIcon = L.divIcon({{
            html: aircraftSvg,
            className: 'aircraft-icon',
            iconSize: [32, 32],
            iconAnchor: [16, 16]
        }});

        var aircraftMarker = null;
        var trackLine = null;
        var trackPoints = [];
        var waypointMarkers = [];

        function updateAircraft(lat, lon, heading) {{
            if (!aircraftMarker) {{
                aircraftMarker = L.marker([lat, lon], {{
                    icon: aircraftIcon,
                    rotationAngle: heading,
                    rotationOrigin: 'center center'
                }}).addTo(map);
            }} else {{
                aircraftMarker.setLatLng([lat, lon]);
                var iconEl = aircraftMarker.getElement();
                if (iconEl) {{
                    iconEl.style.transform = iconEl.style.transform.replace(/rotate\\([^)]*\\)/, '') + ' rotate(' + heading + 'deg)';
                }}
            }}

            trackPoints.push([lat, lon]);
            if (trackPoints.length > 1000) trackPoints.shift();

            if (trackLine) {{
                trackLine.setLatLngs(trackPoints);
            }} else {{
                trackLine = L.polyline(trackPoints, {{color: '#00ff00', weight: 2}}).addTo(map);
            }}
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

        function setWaypoints(waypoints) {{
            waypointMarkers.forEach(m => map.removeLayer(m));
            waypointMarkers = [];

            waypoints.forEach((wp, i) => {{
                var marker = L.circleMarker([wp.lat, wp.lon], {{
                    radius: 8,
                    fillColor: '#ff6600',
                    color: '#fff',
                    weight: 2,
                    fillOpacity: 0.8
                }}).addTo(map);
                marker.bindTooltip(String(i + 1), {{permanent: true, direction: 'center', className: 'wp-label'}});
                waypointMarkers.push(marker);
            }});

            if (waypoints.length > 1) {{
                var routeLine = L.polyline(waypoints.map(wp => [wp.lat, wp.lon]), {{
                    color: '#ff6600',
                    weight: 2,
                    dashArray: '5, 10'
                }}).addTo(map);
                waypointMarkers.push(routeLine);
            }}
        }}

        function centerOn(lat, lon) {{
            map.setView([lat, lon], map.getZoom());
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

    def set_waypoints(self, waypoints: list):
        wp_json = json.dumps(waypoints)
        self.web_view.page().runJavaScript(f"setWaypoints({wp_json});")

    def center_on(self, lat: float, lon: float):
        self.web_view.page().runJavaScript(f"centerOn({lat}, {lon});")
