"""
QML Map Widget — interactive flight map using QtLocation OSM plugin.

Uses QQuickWidget + QML Map for native GPU rendering.
"""
import json
import logging
import math
import os

from PyQt5.QtWidgets import QWidget, QVBoxLayout, QMenu, QAction
from PyQt5.QtQuickWidgets import QQuickWidget
from PyQt5.QtCore import pyqtSignal, QUrl, Qt
from PyQt5.QtGui import QCursor, QColor

import qtawesome as qta

from src.gui.theme import Colors
from src.gui.qml_map_backend import QmlMapBackend
from src.gui.settlement_loader import SettlementLoader

logger = logging.getLogger(__name__)


class QmlMapWidget(QWidget):
    """Native QML Map widget for flight visualization."""
    set_position_requested = pyqtSignal(float, float)
    add_waypoint_requested = pyqtSignal(float, float)
    set_home_requested = pyqtSignal(float, float)
    center_map_requested = pyqtSignal(float, float)
    zone_drawing_finished = pyqtSignal(list)
    zone_drawing_cancelled = pyqtSignal()
    zone_double_clicked = pyqtSignal(str)
    zone_context_menu_requested = pyqtSignal(str, int, int)
    zone_editing_finished = pyqtSignal()
    zone_vertices_updated = pyqtSignal(str, list)
    draw_zone_requested = pyqtSignal()
    page_loaded = pyqtSignal()
    mouse_moved = pyqtSignal(float, float)
    zoom_changed = pyqtSignal(int)

    def __init__(self, center: tuple = (59.939, 30.315), zoom: int = 14):
        super().__init__()
        self.center = center
        self.zoom = zoom
        self._zoom_level = zoom
        self._last_bounds = None
        self._context_lat = 0.0
        self._context_lon = 0.0

        # Settlement loader
        self._settlement_loader = SettlementLoader()
        self._settlement_loader.tile_loaded.connect(self._on_tile_loaded)

        # Backend (QObject bridge to QML)
        self._backend = QmlMapBackend(self)

        # Connect backend signals → widget signals
        self._backend.mapClicked.connect(self._on_map_click)
        self._backend.contextMenuRequested.connect(self._show_context_menu)
        self._backend.boundsChanged.connect(self._on_bounds_changed)
        self._backend.mouseMoved.connect(self.mouse_moved.emit)
        self._backend.zoomChanged.connect(self.zoom_changed.emit)
        self._backend.zoomChanged.connect(self._on_backend_zoom_changed)
        self._backend.showSettlementsChanged.connect(self._on_show_settlements_changed)
        self._backend.drawingFinished.connect(self._on_drawing_finished)
        self._backend.drawingCancelled.connect(self.zone_drawing_cancelled.emit)
        self._backend.zoneDoubleClicked.connect(self.zone_double_clicked.emit)
        self._backend.zoneContextMenuRequested.connect(self.zone_context_menu_requested.emit)
        self._backend.zoneEditingFinished.connect(self.zone_editing_finished.emit)
        self._backend.zoneVerticesUpdated.connect(self._on_zone_vertices_updated)

        self._setup_ui()

    @property
    def bridge(self):
        """QML backend — main_window accesses bridge for signal connections."""
        return self._backend

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self._quick_widget = QQuickWidget()
        self._quick_widget.setResizeMode(QQuickWidget.SizeRootObjectToView)
        self._quick_widget.setClearColor(QColor(Colors.BG_APP))

        # Expose backend to QML as context property
        ctx = self._quick_widget.rootContext()
        ctx.setContextProperty("backend", self._backend)

        # Connect statusChanged BEFORE setSource to catch synchronous Ready
        self._quick_widget.statusChanged.connect(self._on_status_changed)

        # Load QML
        qml_path = os.path.join(os.path.dirname(__file__), 'qml', 'FlightMap.qml')
        self._quick_widget.setSource(QUrl.fromLocalFile(qml_path))

        # Check for errors
        if self._quick_widget.status() == QQuickWidget.Error:
            errors = self._quick_widget.errors()
            for err in errors:
                logger.error("QML Error: %s", err.toString())
        else:
            logger.info("QML Map loaded successfully")

        # Set initial center/zoom via root object
        root = self._quick_widget.rootObject()
        if root:
            root.setProperty("defaultLat", self.center[0])
            root.setProperty("defaultLon", self.center[1])
            root.setProperty("defaultZoom", self.zoom)

        layout.addWidget(self._quick_widget)

        # Fallback: if QML was already Ready before connection, emit manually
        if self._quick_widget.status() == QQuickWidget.Ready:
            logger.info("QML Map was already Ready — emitting page_loaded")
            self.page_loaded.emit()

    def _on_status_changed(self, status):
        if status == QQuickWidget.Ready:
            logger.info("QML Map ready — emitting page_loaded")
            self.page_loaded.emit()

    # ════════════════════ Public API ════════════════════

    def update_aircraft(self, lat: float, lon: float, heading: float):
        self._backend.update_aircraft(lat, lon, heading)

    def set_aircraft_position(self, lat: float, lon: float):
        self._backend.set_aircraft_position(lat, lon)

    def set_track_max_length(self, length: int):
        self._backend.set_track_max_length(length)

    def clear_track(self):
        self._backend.clear_track()

    def set_waypoints(self, waypoints: list, active_idx: int = 0):
        self._backend.set_waypoints(waypoints, active_idx)

    def update_active_waypoint(self, index: int):
        self._backend.update_active_waypoint(index)

    def add_waypoint(self, lat: float, lon: float, index: int, wp_data: dict = None):
        # Not commonly used — refresh via set_waypoints instead
        pass

    def center_on(self, lat: float, lon: float):
        self._backend.center_on(lat, lon)

    def set_follow_mode(self, enabled: bool):
        self._backend.set_follow_mode(enabled)

    def set_home_marker(self, lat: float, lon: float):
        self._backend.set_home(lat, lon)

    def set_layer_visibility(self, layer_name: str, visible: bool):
        self._backend.set_layer_visibility(layer_name, visible)

    def set_route_conflicts(self, conflicts: list):
        self._backend.set_route_conflicts(conflicts)

    def set_avoidance_path(self, points: list):
        self._backend.set_avoidance_path(points)

    def set_planned_direct_path(self, points: list):
        self._backend.set_planned_direct_path(points)

    def set_conflict_points(self, points: list):
        self._backend.set_conflict_points(points)

    def clear_route_conflicts(self):
        self._backend.clear_route_conflicts()

    # ── Zone API ──

    def start_zone_drawing(self):
        self._backend.start_drawing()
        self._quick_widget.setFocus()  # QML needs focus for Escape key

    def cancel_zone_drawing(self):
        self._backend.cancel_drawing()

    def add_restricted_zone(self, zone_id: str, points: list, name: str = ""):
        self._backend.add_zone(zone_id, points, name)

    def remove_restricted_zone(self, zone_id: str):
        self._backend.remove_zone(zone_id)

    def update_restricted_zone(self, zone_id: str, points: list):
        self._backend._zoneModel.update_zone_points(zone_id, points)

    def enable_zone_editing(self, zone_id: str):
        self._backend.enable_zone_editing(zone_id)

    def disable_zone_editing(self, zone_id: str = ""):
        self._backend.disable_zone_editing()

    def highlight_zone(self, zone_id: str):
        pass  # TODO: highlight in QML

    def unhighlight_zone(self, zone_id: str):
        pass  # TODO: unhighlight in QML

    def load_all_zones(self, zones: list):
        self._backend.load_all_zones(zones)

    # ════════════════════ Internal handlers ════════════════════

    def _on_map_click(self, lat, lon):
        # This is only emitted when NOT in drawing mode
        # Expose as bridge.position_clicked compatibility
        pass

    def _on_drawing_finished(self, points_json):
        try:
            points = json.loads(points_json)
            self.zone_drawing_finished.emit(points)
        except json.JSONDecodeError:
            pass

    def _on_zone_vertices_updated(self, zone_id, points):
        self.zone_vertices_updated.emit(zone_id, points)

    def _on_bounds_changed(self, south, west, north, east):
        if math.isnan(south) or math.isnan(west) or math.isnan(north) or math.isnan(east):
            return
        self._last_bounds = (south, west, north, east)
        self._request_settlements_for_bounds(south, west, north, east)

    def _request_settlements_for_bounds(self, south, west, north, east):
        # Load settlements only when layer is enabled and zoom > 13
        if not self._backend.showSettlements:
            return
        if self._zoom_level <= 13:
            return
        self._settlement_loader.preload_cached_tiles(south, west, north, east)
        self._settlement_loader.request(south, west, north, east)

    def _on_backend_zoom_changed(self, zoom):
        self._zoom_level = int(zoom)
        if self._last_bounds:
            self._request_settlements_for_bounds(*self._last_bounds)

    def _on_show_settlements_changed(self):
        if self._last_bounds:
            self._request_settlements_for_bounds(*self._last_bounds)

    def _on_tile_loaded(self, key, features):
        if features:
            self._backend.add_settlement_features(features)

    def _show_context_menu(self, lat: float, lon: float, screen_x: int, screen_y: int):
        self._context_lat = lat
        self._context_lon = lon

        menu = QMenu(self)
        menu.setStyleSheet(f"""
            QMenu {{
                background-color: {Colors.BG_TOOLTIP};
                color: {Colors.TEXT_PRIMARY};
                border: 1px solid {Colors.BORDER};
                border-radius: 8px;
                padding: 4px;
            }}
            QMenu::item {{
                padding: 8px 16px;
                border-radius: 4px;
            }}
            QMenu::item:selected {{
                background-color: {Colors.BG_INPUT};
            }}
            QMenu::separator {{
                height: 1px;
                background: {Colors.BORDER};
                margin: 4px 8px;
            }}
        """)

        action_set_pos = QAction("Установить позицию здесь", self)
        action_set_pos.setIcon(qta.icon("mdi.crosshairs-gps", color=Colors.TEXT_SECONDARY))
        action_set_pos.triggered.connect(self._on_set_position)
        menu.addAction(action_set_pos)

        action_add_wp = QAction("Добавить точку маршрута", self)
        action_add_wp.setIcon(qta.icon("mdi.map-marker-plus", color=Colors.TEXT_SECONDARY))
        action_add_wp.triggered.connect(self._on_add_waypoint)
        menu.addAction(action_add_wp)

        action_set_home = QAction("Установить дом", self)
        action_set_home.setIcon(qta.icon("mdi.home-map-marker", color=Colors.TEXT_SECONDARY))
        action_set_home.triggered.connect(self._on_set_home)
        menu.addAction(action_set_home)

        menu.addSeparator()

        action_center = QAction("Центрировать карту", self)
        action_center.setIcon(qta.icon("mdi.crosshairs", color=Colors.TEXT_SECONDARY))
        action_center.triggered.connect(self._on_center_map)
        menu.addAction(action_center)

        action_clear_track = QAction("Очистить трек", self)
        action_clear_track.setIcon(qta.icon("mdi.eraser", color=Colors.TEXT_SECONDARY))
        action_clear_track.triggered.connect(self._on_clear_track)
        menu.addAction(action_clear_track)

        menu.addSeparator()

        action_draw_zone = QAction("Нарисовать запретную зону", self)
        action_draw_zone.setIcon(qta.icon("mdi.shield-alert-outline", color=Colors.TEXT_SECONDARY))
        action_draw_zone.triggered.connect(self._on_draw_zone)
        menu.addAction(action_draw_zone)

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

    def _on_draw_zone(self):
        self.draw_zone_requested.emit()
