import logging
from pathlib import Path
from typing import Optional

import qtawesome as qta

logger = logging.getLogger(__name__)
from PyQt5.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QPushButton, QLabel, QFrame, QStatusBar, QFileDialog,
    QMessageBox, QSpinBox, QApplication, QSplitter, QMenu, QAction
)
from PyQt5.QtCore import Qt, QTimer, QPoint
from PyQt5.QtGui import QFont, QIcon, QCursor

from src.core.config import AppConfig
from src.core.events import EventBus, Event
from src.mavlink.proxy import MAVLinkProxy
from src.mavlink.telemetry import LatLon
from src.navigation.calculations import haversine_distance, eta_seconds
from src.navigation.route_planner import RoutePlanner
from src.autopilot.autopilot_manager import AutopilotManager, AutopilotMode
from src.gui.status_panel import StatusPanel
from src.gui.map_widget import MapWidget
from src.gui.waypoint_dialog import WaypointDialog
from src.gui.zone_dialog import ZonePropertiesDialog
from src.gui.zone_settings_dialog import ZoneSettingsDialog
from src.gui.theme import STYLESHEET, Colors, Fonts, apply_dark_titlebar
from src.navigation.zone_manager import ZoneManager
from src.navigation.zone_checker import ZoneChecker
from src.navigation.path_planner import PathPlanner


class MainWindow(QMainWindow):
    def __init__(self, config: AppConfig):
        super().__init__()
        self.config = config
        self.event_bus = EventBus()

        self.proxy = MAVLinkProxy(
            sitl_host=config.mavlink.sitl_host,
            sitl_port=config.mavlink.sitl_port,
            proxy_port=config.mavlink.proxy_port,
            system_id=config.mavlink.system_id,
            component_id=config.mavlink.component_id
        )

        self.route_planner = RoutePlanner()
        self.autopilot = AutopilotManager(self.proxy, config.autopilot)
        self.autopilot.set_route_planner(self.route_planner)

        self.zone_manager = ZoneManager()
        self.zone_checker = ZoneChecker(self.zone_manager, config.zone_avoidance)
        self.path_planner = PathPlanner(self.zone_checker)
        self.autopilot.set_zone_checker(self.zone_checker)
        self.autopilot.set_path_planner(self.path_planner)

        # Load settlement cache for zone checking
        cache_dir = Path(__file__).parent.parent.parent / 'cache' / 'settlements_v5'
        if cache_dir.exists():
            self.zone_checker.set_settlement_cache_dir(cache_dir)

        self._gui_avoidance_result = None  # (result, wp1, wp2) from background thread

        self._set_position_mode = False
        self._set_home_mode = False
        self._drawing_zone_mode = False
        self._home_position: Optional[LatLon] = None

        QApplication.instance().setStyleSheet(STYLESHEET)

        self._setup_ui()
        self._setup_connections()
        self._setup_timer()

        apply_dark_titlebar(int(self.winId()))
        self.showMaximized()

    # ────────────────────── UI ──────────────────────

    def _setup_ui(self):
        self.setWindowTitle("VTOL Co-Pilot")
        self.setMinimumSize(1200, 800)

        # ── Splitter: Map | Panel ──
        splitter = QSplitter(Qt.Horizontal)
        splitter.setChildrenCollapsible(False)
        splitter.setHandleWidth(4)
        self.setCentralWidget(splitter)

        # ── LEFT: Map ──
        self.map_widget = MapWidget(self.config.gui.map_center, self.config.gui.map_zoom)
        self.map_widget.setMinimumWidth(400)
        splitter.addWidget(self.map_widget)

        # ── RIGHT: Panel ──
        right = QWidget()
        right.setStyleSheet(f"background-color: {Colors.BG_SIDEBAR};")
        right.setMinimumWidth(280)
        right.setMaximumWidth(800)
        right_lay = QVBoxLayout(right)
        right_lay.setContentsMargins(0, 0, 0, 0)
        right_lay.setSpacing(0)

        # Header inside panel
        right_lay.addWidget(self._build_panel_header())

        # Separator
        sep = QFrame()
        sep.setFixedHeight(1)
        sep.setStyleSheet(f"background-color: {Colors.BORDER};")
        right_lay.addWidget(sep)

        # Status panel (scrollable telemetry + nav + autopilot)
        self.status_panel = StatusPanel()
        right_lay.addWidget(self.status_panel, 1)

        # Separator
        sep2 = QFrame()
        sep2.setFixedHeight(1)
        sep2.setStyleSheet(f"background-color: {Colors.BORDER};")
        right_lay.addWidget(sep2)

        # Controls footer
        right_lay.addWidget(self._build_controls())

        splitter.addWidget(right)

        # Initial proportions: ~60% map, ~40% panel
        splitter.setSizes([900, 580])
        splitter.setStretchFactor(0, 1)
        splitter.setStretchFactor(1, 0)

        # Status bar
        self.statusbar = QStatusBar()
        self.setStatusBar(self.statusbar)
        self.statusbar.showMessage("Отключено")

        sb_label_style = f"color: {Colors.TEXT_TERTIARY}; font-size: 11px; border: none; padding: 0 6px;"
        sb_value_style = f'color: {Colors.TEXT_PRIMARY}; font-family: "{Fonts.MONO}"; font-size: 11px; font-weight: 600; border: none; padding: 0 4px;'
        sb_dim_style = f'color: {Colors.TEXT_DIM}; font-family: "{Fonts.MONO}"; font-size: 11px; border: none; padding: 0 4px;'

        self.sb_zoom_lbl = QLabel("Масштаб")
        self.sb_zoom_lbl.setStyleSheet(sb_label_style)
        self.sb_zoom_val = QLabel(f"{self.config.gui.map_zoom}")
        self.sb_zoom_val.setStyleSheet(sb_value_style)

        self.sb_layer_val = QLabel("Спутник")
        self.sb_layer_val.setStyleSheet(sb_dim_style)

        self.sb_ac_lbl = QLabel("ЛА")
        self.sb_ac_lbl.setStyleSheet(sb_label_style)
        self.sb_ac_val = QLabel("--- , ---")
        self.sb_ac_val.setStyleSheet(sb_value_style)

        self.sb_cur_lbl = QLabel("Курсор")
        self.sb_cur_lbl.setStyleSheet(sb_label_style)
        self.sb_cur_val = QLabel("--- , ---")
        self.sb_cur_val.setStyleSheet(sb_dim_style)

        for w in (self.sb_zoom_lbl, self.sb_zoom_val, self.sb_layer_val,
                  self.sb_ac_lbl, self.sb_ac_val, self.sb_cur_lbl, self.sb_cur_val):
            self.statusbar.addPermanentWidget(w)

    def _build_panel_header(self) -> QWidget:
        """Connection controls + mode badge."""
        hdr = QWidget()
        hdr.setFixedHeight(48)
        lay = QHBoxLayout(hdr)
        lay.setContentsMargins(10, 0, 10, 0)
        lay.setSpacing(8)

        # Connect
        self.btn_connect = QPushButton("Подключить")
        self.btn_connect.setIcon(qta.icon("mdi.lan-connect", color="#fff"))
        self.btn_connect.setFixedHeight(30)
        self.btn_connect.setStyleSheet(f"""
            QPushButton {{
                background-color: {Colors.PRIMARY};
                color: #fff; border: none; border-radius: 6px;
                padding: 0 12px; font-size: 12px; font-weight: 600;
            }}
            QPushButton:hover {{ background-color: {Colors.PRIMARY_HOVER}; }}
        """)
        lay.addWidget(self.btn_connect)

        # Disconnect
        self.btn_disconnect = QPushButton("Откл.")
        self.btn_disconnect.setIcon(qta.icon("mdi.lan-disconnect", color=Colors.TEXT_SECONDARY))
        self.btn_disconnect.setFixedHeight(30)
        self.btn_disconnect.setEnabled(False)
        lay.addWidget(self.btn_disconnect)

        lay.addStretch()

        # Mode badge
        self.lbl_mode = QLabel("---")
        self.lbl_mode.setStyleSheet(f"""
            color: {Colors.TEXT_TERTIARY};
            font-family: "{Fonts.MONO}";
            font-size: 12px; font-weight: 700;
            padding: 3px 10px; border-radius: 4px;
            background-color: {Colors.BG_INPUT};
        """)
        lay.addWidget(self.lbl_mode)

        # Status dot
        self.lbl_status = QLabel("OFF")
        self.lbl_status.setStyleSheet(f"""
            color: {Colors.ERROR};
            font-size: 10px; font-weight: 700;
            padding: 2px 8px; border-radius: 10px;
            background-color: {Colors.ERROR_BG};
        """)
        lay.addWidget(self.lbl_status)

        return hdr

    def _build_controls(self) -> QWidget:
        """Action buttons at the bottom of the right panel."""
        panel = QWidget()
        lay = QVBoxLayout(panel)
        lay.setContentsMargins(10, 8, 10, 10)
        lay.setSpacing(6)

        # NAV button — big, prominent
        self.btn_nav = QPushButton("  Навигация")
        self.btn_nav.setIcon(qta.icon("mdi.play", color="#fff"))
        self.btn_nav.setCheckable(True)
        self.btn_nav.setEnabled(False)
        self.btn_nav.setFixedHeight(36)
        self.btn_nav.setStyleSheet(f"""
            QPushButton {{
                background-color: {Colors.BG_INPUT};
                color: {Colors.TEXT_SECONDARY};
                border: 1px solid {Colors.BORDER};
                border-radius: 8px;
                font-size: 13px; font-weight: 600;
                padding: 0 16px;
            }}
            QPushButton:hover {{
                background-color: {Colors.BG_HOVER};
                color: {Colors.TEXT_PRIMARY};
                border-color: {Colors.BORDER_LIGHT};
            }}
            QPushButton:checked {{
                background-color: {Colors.SUCCESS};
                color: #fff;
                border-color: {Colors.SUCCESS};
            }}
            QPushButton:disabled {{
                background-color: {Colors.BG_CARD};
                color: {Colors.TEXT_DIM};
                border-color: {Colors.BORDER_SUBTLE};
            }}
        """)
        lay.addWidget(self.btn_nav)

        # Row 1: Position + Home + Route
        r1 = QHBoxLayout()
        r1.setSpacing(4)

        self.btn_set_pos = self._action_btn("mdi.crosshairs-gps", "Коррекция", checkable=True)
        r1.addWidget(self.btn_set_pos)

        self.btn_set_home = self._action_btn("mdi.home-map-marker", "Дом", checkable=True)
        r1.addWidget(self.btn_set_home)

        self.btn_load_route = self._action_btn("mdi.folder-open-outline", "Маршрут")
        r1.addWidget(self.btn_load_route)

        lay.addLayout(r1)

        # Row 2: Follow + RTH + Clear
        r2 = QHBoxLayout()
        r2.setSpacing(4)

        self.btn_follow = self._action_btn("mdi.eye-outline", "Слежение", checkable=True)
        r2.addWidget(self.btn_follow)

        self.btn_home = QPushButton("  Домой")
        self.btn_home.setIcon(qta.icon("mdi.home-import-outline", color=Colors.TEXT_SECONDARY))
        self.btn_home.setCheckable(True)
        self.btn_home.setEnabled(False)
        self.btn_home.setFixedHeight(30)
        self.btn_home.setStyleSheet(f"""
            QPushButton {{
                background-color: {Colors.BG_INPUT};
                color: {Colors.TEXT_SECONDARY};
                border: 1px solid {Colors.BORDER};
                border-radius: 6px;
                padding: 0 10px; font-size: 12px; font-weight: 500;
            }}
            QPushButton:hover {{
                background-color: {Colors.BG_HOVER};
                color: {Colors.TEXT_PRIMARY};
            }}
            QPushButton:checked {{
                background-color: {Colors.WARNING};
                color: #000;
                border-color: {Colors.WARNING};
            }}
            QPushButton:disabled {{
                background-color: {Colors.BG_CARD};
                color: {Colors.TEXT_DIM};
                border-color: {Colors.BORDER_SUBTLE};
            }}
        """)
        r2.addWidget(self.btn_home)

        self.btn_clear_track = self._action_btn("mdi.eraser", "Трек")
        r2.addWidget(self.btn_clear_track)

        self.btn_draw_zone = QPushButton("  Зоны")
        self.btn_draw_zone.setIcon(qta.icon("mdi.shield-alert-outline", color=Colors.TEXT_SECONDARY))
        self.btn_draw_zone.setCheckable(True)
        self.btn_draw_zone.setFixedHeight(30)
        r2.addWidget(self.btn_draw_zone)

        self.btn_zone_settings = QPushButton()
        self.btn_zone_settings.setIcon(qta.icon("mdi.shield-lock-outline", color=Colors.TEXT_SECONDARY))
        self.btn_zone_settings.setFixedSize(30, 30)
        self.btn_zone_settings.setToolTip("Настройки обхода зон")
        r2.addWidget(self.btn_zone_settings)

        lay.addLayout(r2)

        # Waypoint nav row
        wr = QHBoxLayout()
        wr.setSpacing(4)

        self.btn_wp_prev = QPushButton()
        self.btn_wp_prev.setIcon(qta.icon("mdi.chevron-left", color=Colors.TEXT_SECONDARY))
        self.btn_wp_prev.setFixedSize(28, 28)
        self.btn_wp_prev.setEnabled(False)
        wr.addWidget(self.btn_wp_prev)

        wl = QLabel("WPT")
        wl.setStyleSheet(f"color: {Colors.TEXT_TERTIARY}; font-size: 10px;")
        wr.addWidget(wl)

        self.spin_waypoint = QSpinBox()
        self.spin_waypoint.setMinimum(1)
        self.spin_waypoint.setMaximum(1)
        self.spin_waypoint.setEnabled(False)
        self.spin_waypoint.setFixedSize(52, 28)
        wr.addWidget(self.spin_waypoint)

        self.btn_wp_next = QPushButton()
        self.btn_wp_next.setIcon(qta.icon("mdi.chevron-right", color=Colors.TEXT_SECONDARY))
        self.btn_wp_next.setFixedSize(28, 28)
        self.btn_wp_next.setEnabled(False)
        wr.addWidget(self.btn_wp_next)

        wr.addStretch()
        lay.addLayout(wr)

        return panel

    def _action_btn(self, icon_name: str, text: str, checkable: bool = False) -> QPushButton:
        btn = QPushButton(f"  {text}")
        btn.setIcon(qta.icon(icon_name, color=Colors.TEXT_SECONDARY))
        btn.setCheckable(checkable)
        btn.setEnabled(False)
        btn.setFixedHeight(30)
        return btn

    # ────────────────────── Connections ──────────────────────

    def _setup_connections(self):
        self.btn_connect.clicked.connect(self._on_connect)
        self.btn_disconnect.clicked.connect(self._on_disconnect)
        self.btn_set_pos.clicked.connect(self._on_set_position_toggle)
        self.btn_set_home.clicked.connect(self._on_set_home_toggle)
        self.btn_load_route.clicked.connect(self._on_load_route)
        self.btn_clear_track.clicked.connect(self._on_clear_track)
        self.btn_draw_zone.clicked.connect(self._on_draw_zone_toggle)
        self.btn_zone_settings.clicked.connect(self._on_zone_settings)
        self.btn_nav.clicked.connect(self._on_nav_toggle)
        self.btn_follow.clicked.connect(self._on_follow_toggle)
        self.btn_home.clicked.connect(self._on_home_toggle)
        self.btn_wp_prev.clicked.connect(self._on_wp_prev)
        self.btn_wp_next.clicked.connect(self._on_wp_next)
        self.spin_waypoint.valueChanged.connect(self._on_wp_select)

        self.map_widget.bridge.position_clicked.connect(self._on_map_clicked)
        self.map_widget.set_position_requested.connect(self._on_context_set_position)
        self.map_widget.add_waypoint_requested.connect(self._on_context_add_waypoint)
        self.map_widget.set_home_requested.connect(self._on_context_set_home)
        self.map_widget.draw_zone_requested.connect(self._on_start_zone_drawing)
        self.map_widget.zone_drawing_finished.connect(self._on_zone_drawing_finished)
        self.map_widget.zone_drawing_cancelled.connect(self._on_zone_drawing_cancelled)
        self.map_widget.zone_double_clicked.connect(self._on_zone_double_clicked)
        self.map_widget.zone_context_menu_requested.connect(self._on_zone_context_menu)
        self.map_widget.zone_editing_finished.connect(self._on_zone_editing_finished)
        self.map_widget.zone_vertices_updated.connect(self._on_zone_vertices_updated)
        self.map_widget.page_loaded.connect(self._load_zones)
        self.map_widget.mouse_moved.connect(self._on_map_mouse_move)
        self.map_widget.zoom_changed.connect(self._on_map_zoom_changed)
        self.map_widget._settlement_loader.tile_loaded.connect(self._on_settlement_tile_loaded)

        self.status_panel.orbit_radius_changed.connect(self._on_orbit_radius_changed)
        self.status_panel.target_altitude_changed.connect(self._on_target_altitude_changed)
        self.status_panel.target_airspeed_changed.connect(self._on_target_airspeed_changed)
        self.status_panel.wind_override_requested.connect(self._on_wind_override)

        self.event_bus.subscribe(Event.CONNECTION_RESTORED, self._on_connection_restored)
        self.event_bus.subscribe(Event.CONNECTION_LOST, self._on_connection_lost)
        self.event_bus.subscribe(Event.AUTOPILOT_ENGAGE, self._on_autopilot_engage)
        self.event_bus.subscribe(Event.AUTOPILOT_DISENGAGE, self._on_autopilot_disengage)
        self.event_bus.subscribe(Event.WAYPOINT_REACHED, self._on_waypoint_reached)

    def _setup_timer(self):
        self.update_timer = QTimer()
        self.update_timer.timeout.connect(self._update_display)
        self.update_timer.start(100)

    # ────────────────────── Connection ──────────────────────

    def _on_connect(self):
        self.statusbar.showMessage("Подключение...")
        if self.proxy.connect():
            self.proxy.start()
            self.proxy.request_data_streams(4)

    def _on_disconnect(self):
        self.proxy.stop()
        self._on_connection_lost(None)

    def _enable_controls(self, enabled: bool):
        for btn in (self.btn_nav, self.btn_set_pos, self.btn_set_home,
                     self.btn_load_route, self.btn_clear_track,
                     self.btn_follow, self.btn_home):
            btn.setEnabled(enabled)

    def _on_connection_restored(self, data):
        self.btn_connect.setEnabled(False)
        self.btn_disconnect.setEnabled(True)
        self._enable_controls(True)

        self.lbl_status.setText("ON")
        self.lbl_status.setStyleSheet(f"""
            color: {Colors.SUCCESS}; font-size: 10px; font-weight: 700;
            padding: 2px 8px; border-radius: 10px;
            background-color: {Colors.SUCCESS_BG};
        """)
        self.statusbar.showMessage(f"Подключено — порт {self.config.mavlink.sitl_port}")

        self.btn_follow.setChecked(True)
        self.map_widget.set_follow_mode(True)

    def _on_connection_lost(self, data):
        self.btn_connect.setEnabled(True)
        self.btn_disconnect.setEnabled(False)
        self._enable_controls(False)

        self.lbl_status.setText("OFF")
        self.lbl_status.setStyleSheet(f"""
            color: {Colors.ERROR}; font-size: 10px; font-weight: 700;
            padding: 2px 8px; border-radius: 10px;
            background-color: {Colors.ERROR_BG};
        """)
        self.statusbar.showMessage("Отключено")

    # ────────────────────── Position / Home ──────────────────────

    def _on_set_position_toggle(self):
        self._set_position_mode = self.btn_set_pos.isChecked()
        if self._set_position_mode:
            self.statusbar.showMessage("Кликните на карте для коррекции позиции EKF...")
        else:
            self.statusbar.showMessage("Режим коррекции отменён")

    def _on_set_home_toggle(self):
        self._set_home_mode = self.btn_set_home.isChecked()
        if self._set_home_mode:
            self.statusbar.showMessage("Кликните на карте для установки точки дома...")
        else:
            self.statusbar.showMessage("Режим установки дома отменён")

    def _on_map_clicked(self, lat: float, lon: float):
        if self._set_position_mode:
            self._set_correction_position(lat, lon)
        elif self._set_home_mode:
            self._set_home_position(lat, lon)

    def _on_context_set_position(self, lat: float, lon: float):
        self._set_correction_position(lat, lon)

    def _set_correction_position(self, lat: float, lon: float):
        self.proxy.send_position_reset(lat, lon)
        self._set_position_mode = False
        self.btn_set_pos.setChecked(False)
        self.map_widget.set_aircraft_position(lat, lon)
        self.statusbar.showMessage(f"Коррекция позиции: {lat:.6f}, {lon:.6f}")

    def _on_context_add_waypoint(self, lat: float, lon: float):
        dialog = WaypointDialog(self, lat, lon, zone_checker=self.zone_checker)
        if dialog.exec_() != WaypointDialog.Accepted:
            return

        wp_data = dialog.get_waypoint_data()

        if not self.route_planner.get_route():
            self.route_planner.create_route("Новый маршрут")

        wp = self.route_planner.add_waypoint(
            lat=wp_data['lat'], lon=wp_data['lon'],
            altitude=wp_data['altitude'], radius=wp_data['radius'],
            action=wp_data['action'],
            orbit_radius=wp_data.get('orbit_radius', 150.0),
            orbit_turns=wp_data.get('orbit_turns', 1),
            climb_enroute=wp_data.get('climb_enroute', False)
        )

        if self.autopilot.is_engaged():
            status = self.autopilot.get_status()
            if status.get('is_orbiting') or status.get('returning_home'):
                self.route_planner.set_active_waypoint(
                    self.route_planner.get_waypoint_count() - 1)

        self._refresh_map_waypoints()
        self.statusbar.showMessage(f"Добавлена точка {wp.id}: {lat:.6f}, {lon:.6f}")

    def _on_context_set_home(self, lat: float, lon: float):
        self._set_home_position(lat, lon)

    def _set_home_position(self, lat: float, lon: float):
        self._home_position = LatLon(lat, lon)
        self.autopilot.set_home_position(self._home_position)
        self.map_widget.set_home_marker(lat, lon)
        self._set_home_mode = False
        self.btn_set_home.setChecked(False)
        self.statusbar.showMessage(f"Дом: {lat:.6f}, {lon:.6f}")

    # ────────────────────── Route / Waypoints ──────────────────────

    def _on_load_route(self):
        routes_dir = Path(__file__).parent.parent.parent / "routes"
        routes_dir.mkdir(exist_ok=True)
        file_path, _ = QFileDialog.getOpenFileName(
            self, "Загрузить маршрут", str(routes_dir), "JSON файлы (*.json)")
        if file_path:
            route = self.route_planner.load_route(Path(file_path))
            if route:
                self._refresh_map_waypoints()
                self.statusbar.showMessage(f"Маршрут: {route.name} ({len(route.waypoints)} точек)")
            else:
                QMessageBox.warning(self, "Ошибка", "Не удалось загрузить маршрут")

    def _refresh_map_waypoints(self):
        waypoints = self.route_planner.get_waypoints_for_display()
        active_idx = self.route_planner.get_active_waypoint_index()
        self.map_widget.set_waypoints(waypoints, active_idx)
        self._update_waypoint_controls()
        self._check_route_conflicts(waypoints)

    def _update_waypoint_controls(self):
        n = self.route_planner.get_waypoint_count()
        for w in (self.btn_wp_prev, self.btn_wp_next, self.spin_waypoint):
            w.setEnabled(n > 0)
        if n > 0:
            self.spin_waypoint.blockSignals(True)
            self.spin_waypoint.setMaximum(n)
            self.spin_waypoint.setValue(self.route_planner.get_active_waypoint_index() + 1)
            self.spin_waypoint.blockSignals(False)

    def _on_wp_prev(self):
        self.route_planner.prev_waypoint()
        self._refresh_map_waypoints()

    def _on_wp_next(self):
        self.route_planner.next_waypoint()
        self._refresh_map_waypoints()

    def _on_wp_select(self, value: int):
        self.route_planner.set_active_waypoint(value - 1)
        self._refresh_map_waypoints()

    def _check_route_conflicts(self, waypoints: list):
        """Check route segments for zone conflicts and show on map."""
        if len(waypoints) < 2:
            self.map_widget.clear_route_conflicts()
            return

        conflicts = []
        for i in range(len(waypoints) - 1):
            wp1 = waypoints[i]
            wp2 = waypoints[i + 1]
            alt = wp2.get('altitude', 100)
            if self.zone_checker.segment_intersects_obstacles(
                wp1['lat'], wp1['lon'], wp2['lat'], wp2['lon'], alt
            ):
                _, reason = self.zone_checker.is_point_restricted(
                    (wp1['lat'] + wp2['lat']) / 2,
                    (wp1['lon'] + wp2['lon']) / 2,
                    alt
                )
                conflicts.append({
                    'from_idx': i,
                    'to_idx': i + 1,
                    'reason': reason or 'Маршрут пересекает запретную область'
                })

        if conflicts:
            self.map_widget.set_route_conflicts(conflicts)
            # Compute avoidance path in background thread to avoid UI freeze
            self._compute_avoidance_async(waypoints, conflicts[0])
        else:
            self.map_widget.clear_route_conflicts()

    def _compute_avoidance_async(self, waypoints: list, conflict: dict):
        """Run plan_path in a background thread, update map on completion."""
        import threading

        wp1 = waypoints[conflict['from_idx']]
        wp2 = waypoints[conflict['to_idx']]
        alt = wp2.get('altitude', 100)

        # Use aircraft position as start when autopilot is engaged
        # (avoidance should show from where the aircraft IS, not from the waypoint)
        start_lat, start_lon = wp1['lat'], wp1['lon']
        if self.autopilot.is_engaged():
            telemetry = self.proxy.get_telemetry()
            pos = telemetry.position
            if pos:
                start_lat, start_lon = pos.lat, pos.lon

        logger.info("GUI avoidance: computing path from (%.5f,%.5f) to (%.5f,%.5f) alt=%d",
                     start_lat, start_lon, wp2['lat'], wp2['lon'], alt)

        start = {'lat': start_lat, 'lon': start_lon}

        def worker():
            try:
                result = self.path_planner.plan_path(
                    start['lat'], start['lon'], wp2['lat'], wp2['lon'], alt
                )
                logger.info("GUI avoidance: plan_path returned %s",
                            f"{len(result)} points" if result else "None")
            except Exception as e:
                logger.error("GUI avoidance: plan_path error: %s", e)
                result = None
            self._gui_avoidance_result = (result, start, wp2)

        threading.Thread(target=worker, daemon=True).start()

    def _on_avoidance_computed(self, avoidance, wp1: dict, wp2: dict):
        """Callback from background thread — update map with avoidance path."""
        if avoidance is not None and len(avoidance) > 0:
            path_points = [{'lat': wp1['lat'], 'lon': wp1['lon']}]
            for pt in avoidance:
                path_points.append({'lat': pt[0], 'lon': pt[1]})
            path_points.append({'lat': wp2['lat'], 'lon': wp2['lon']})
            logger.info("GUI avoidance: drawing %d-point path on map", len(path_points))
            self.map_widget.set_avoidance_path(path_points)
        else:
            logger.info("GUI avoidance: no path to draw (result=%s)", type(avoidance).__name__)
            self.map_widget.set_avoidance_path([])

    # ────────────────────── Map controls ──────────────────────

    def _on_clear_track(self):
        self.map_widget.clear_track()
        self.statusbar.showMessage("Трек очищен")

    def _on_follow_toggle(self):
        self.map_widget.set_follow_mode(self.btn_follow.isChecked())

    # ────────────────────── Settlements ──────────────────────

    def _on_settlement_tile_loaded(self, key: str, features: list):
        """Feed newly fetched settlement tile into ZoneChecker for avoidance."""
        if features:
            self.zone_checker.add_settlement_features(features)

    # ────────────────────── No-Fly Zones ──────────────────────

    def _load_zones(self):
        zones = self.zone_manager.get_all_zones()
        if zones:
            zone_dicts = [{'id': z.id, 'points': z.points, 'name': z.name}
                          for z in zones]
            self.map_widget.load_all_zones(zone_dicts)

    def _on_zone_settings(self):
        from src.core.config import save_config
        dialog = ZoneSettingsDialog(self, self.config.zone_avoidance)
        if dialog.exec_() != ZoneSettingsDialog.Accepted:
            return
        self.config.zone_avoidance = dialog.get_config()
        self.zone_checker.set_config(self.config.zone_avoidance)
        save_config(self.config)
        # Reset autopilot avoidance cache so it recomputes with new settings
        self.autopilot.reset_zone_avoidance()
        # Re-check route conflicts with new settings
        waypoints = self.route_planner.get_waypoints_for_display()
        self._check_route_conflicts(waypoints)
        self.statusbar.showMessage("Настройки обхода зон сохранены")

    def _on_draw_zone_toggle(self):
        if self.btn_draw_zone.isChecked():
            self._on_start_zone_drawing()
        else:
            self._cancel_zone_drawing()

    def _on_start_zone_drawing(self):
        self._set_position_mode = False
        self._set_home_mode = False
        self.btn_set_pos.setChecked(False)
        self.btn_set_home.setChecked(False)

        self._drawing_zone_mode = True
        self.btn_draw_zone.setChecked(True)
        self.map_widget.start_zone_drawing()
        self.statusbar.showMessage("Кликайте по карте для создания зоны... (двойной клик или клик на первую точку для завершения, Esc — отмена)")

    def _cancel_zone_drawing(self):
        self._drawing_zone_mode = False
        self.btn_draw_zone.setChecked(False)
        self.map_widget.cancel_zone_drawing()
        self.statusbar.showMessage("")

    def _on_zone_drawing_finished(self, points: list):
        self._drawing_zone_mode = False
        self.btn_draw_zone.setChecked(False)

        dialog = ZonePropertiesDialog(self)
        result = dialog.exec_()
        if result != ZonePropertiesDialog.Accepted:
            self.statusbar.showMessage("Создание зоны отменено")
            return

        data = dialog.get_zone_data()
        zone = self.zone_manager.add_zone(
            points=points,
            name=data['name'],
            description=data['description'],
            altitude=data['altitude'],
        )
        self.map_widget.add_restricted_zone(zone.id, zone.points, zone.name)
        self.map_widget.set_layer_visibility('restricted', True)
        name = zone.name or "Без названия"
        self.statusbar.showMessage(f"Запретная зона создана: {name}")

    def _on_zone_drawing_cancelled(self):
        self._drawing_zone_mode = False
        self.btn_draw_zone.setChecked(False)
        self.statusbar.showMessage("Рисование зоны отменено")

    def _on_zone_context_menu(self, zone_id: str, screen_x: int, screen_y: int):
        zone = self.zone_manager.get_zone(zone_id)
        if not zone:
            return

        menu = QMenu(self)
        menu.setStyleSheet(f"""
            QMenu {{
                background-color: {Colors.BG_TOOLTIP};
                color: {Colors.TEXT_PRIMARY};
                border: 1px solid {Colors.BORDER};
                border-radius: 8px; padding: 4px;
            }}
            QMenu::item {{ padding: 8px 16px; border-radius: 4px; }}
            QMenu::item:selected {{ background-color: {Colors.BG_INPUT}; }}
            QMenu::separator {{ height: 1px; background: {Colors.BORDER}; margin: 4px 8px; }}
        """)

        act_edit = menu.addAction(
            qta.icon("mdi.vector-polyline-edit", color=Colors.TEXT_SECONDARY),
            "Редактировать вершины"
        )
        act_props = menu.addAction(
            qta.icon("mdi.cog-outline", color=Colors.TEXT_SECONDARY),
            "Свойства"
        )
        menu.addSeparator()
        act_delete = menu.addAction(
            qta.icon("mdi.delete-outline", color=Colors.ERROR),
            "Удалить зону"
        )

        action = menu.exec_(QCursor.pos())

        if action == act_edit:
            self.map_widget.enable_zone_editing(zone_id)
            self.statusbar.showMessage(
                "Перетаскивайте вершины. ПКМ на вершину — удалить. "
                "Серые точки — добавить вершину. Клик вне зоны или Esc — завершить."
            )
        elif action == act_props:
            self._on_zone_double_clicked(zone_id)
        elif action == act_delete:
            self.zone_manager.remove_zone(zone_id)
            self.map_widget.remove_restricted_zone(zone_id)
            name = zone.name or "Без названия"
            self.statusbar.showMessage(f"Запретная зона удалена: {name}")

    def _on_zone_double_clicked(self, zone_id: str):
        zone = self.zone_manager.get_zone(zone_id)
        if not zone:
            return

        self.map_widget.disable_zone_editing(zone_id)

        dialog = ZonePropertiesDialog(self, zone=zone)
        result = dialog.exec_()

        if result == ZonePropertiesDialog.DELETE_REQUESTED:
            self.zone_manager.remove_zone(zone_id)
            self.map_widget.remove_restricted_zone(zone_id)
            name = zone.name or "Без названия"
            self.statusbar.showMessage(f"Запретная зона удалена: {name}")
        elif result == ZonePropertiesDialog.Accepted:
            data = dialog.get_zone_data()
            self.zone_manager.update_zone(zone_id, **data)
            updated = self.zone_manager.get_zone(zone_id)
            if updated:
                self.map_widget.remove_restricted_zone(zone_id)
                self.map_widget.add_restricted_zone(zone_id, updated.points, updated.name)
            name = data.get('name') or "Без названия"
            self.statusbar.showMessage(f"Запретная зона обновлена: {name}")
        else:
            self.statusbar.showMessage("")

    def _on_zone_editing_finished(self):
        self.statusbar.showMessage("")

    def _on_zone_vertices_updated(self, zone_id: str, points: list):
        self.zone_manager.update_zone_points(zone_id, points)

    # ────────────────────── Autopilot ──────────────────────

    def _on_home_toggle(self):
        if self.btn_home.isChecked():
            if not self._home_position:
                self.btn_home.setChecked(False)
                self.statusbar.showMessage("Установите точку Дом на карте")
                return
            if self.autopilot.engage_home():
                self.btn_nav.setChecked(False)
                self.statusbar.showMessage("Возврат домой")
            else:
                self.btn_home.setChecked(False)
        else:
            self.autopilot.disengage("Отключено пользователем")

    def _on_nav_toggle(self):
        if self.btn_nav.isChecked():
            if self.autopilot.engage_nav():
                route = self.route_planner.get_route()
                wp = self.route_planner.get_active_waypoint()
                if route and wp:
                    self.statusbar.showMessage(f"Навигация: WPT {wp.id}/{len(route.waypoints)}")
            else:
                self.btn_nav.setChecked(False)
                if not self.route_planner.get_route():
                    self.statusbar.showMessage("Загрузите маршрут")
                else:
                    self.statusbar.showMessage("Не удалось включить навигацию")
        else:
            self.autopilot.disengage("Отключено пользователем")

    def _on_autopilot_engage(self, data):
        if data.get('mode') == 'NAV':
            self.btn_nav.setChecked(True)

    def _on_autopilot_disengage(self, data):
        self.btn_nav.setChecked(False)
        self.btn_home.setChecked(False)
        reason = data.get('reason', '')
        self.statusbar.showMessage(f"АП откл.: {reason}" if reason else "АП отключен")

    def _on_waypoint_reached(self, data):
        next_wp = data.get('next', 0)
        total = self.route_planner.get_waypoint_count()
        self.map_widget.update_active_waypoint(next_wp - 1)
        self.statusbar.showMessage(f"WPT {data.get('reached', 0)} reached → {next_wp}/{total}")

    def _on_map_mouse_move(self, lat, lon):
        self.sb_cur_val.setText(f"{lat:.6f} , {lon:.6f}")

    def _on_map_zoom_changed(self, zoom):
        self.sb_zoom_val.setText(str(zoom))

    # ────────────────────── Param changes ──────────────────────

    def _on_orbit_radius_changed(self, r):
        self.autopilot.set_orbit_radius(float(r))

    def _on_target_altitude_changed(self, a):
        self.autopilot.set_target_altitude(float(a))

    def _on_target_airspeed_changed(self, s):
        self.autopilot.set_target_airspeed(float(s))

    def _on_wind_override(self, direction, speed):
        self.proxy.send_wind_override(direction, speed)
        self.statusbar.showMessage(f"Ветер: {direction}° / {speed} м/с")

    # ────────────────────── Display loop ──────────────────────

    def _update_display(self):
        import time as _t
        _t0 = _t.perf_counter()

        # Check for completed GUI avoidance computation (from background thread)
        if self._gui_avoidance_result is not None:
            result, wp1, wp2 = self._gui_avoidance_result
            self._gui_avoidance_result = None
            self._on_avoidance_computed(result, wp1, wp2)

        if not self.proxy.is_connected():
            return

        telemetry = self.proxy.get_telemetry()
        self.status_panel.update_telemetry(telemetry)
        self.lbl_mode.setText(telemetry.mode or "---")

        pos = telemetry.position
        if pos:
            _t1 = _t.perf_counter()
            self.map_widget.update_aircraft(pos.lat, pos.lon, telemetry.heading)
            _t2 = _t.perf_counter()
            self.sb_ac_val.setText(f"{pos.lat:.6f} , {pos.lon:.6f}")

            ap = self.autopilot.get_status()
            if ap.get('returning_home') and self._home_position:
                d = haversine_distance(pos.lat, pos.lon,
                                       self._home_position.lat, self._home_position.lon)
                e = eta_seconds(d, telemetry.groundspeed)
                m = int(e // 60) if e != float('inf') else 99
                s = int(e % 60) if e != float('inf') else 99
                self.status_panel.labels["Точка"][0].setText("HOME")
                self.status_panel.labels["Дистанция"][0].setText(f"{d / 1000:.2f} км")
                self.status_panel.labels["Время приб."][0].setText(f"{m:02d}:{s:02d}")
                self.status_panel.labels["Бок. уклон."][0].setText("---")
            elif self.route_planner.get_route():
                if not self.autopilot.is_engaged():
                    if self.route_planner.is_waypoint_reached(pos):
                        old = self.route_planner.get_active_waypoint_index()
                        self.route_planner.next_waypoint()
                        new = self.route_planner.get_active_waypoint_index()
                        if new != old:
                            self.map_widget.update_active_waypoint(new)

                dist = self.route_planner.distance_to_waypoint(pos)
                eta = self.route_planner.eta_to_waypoint(pos, telemetry.groundspeed)
                xtk = self.route_planner.cross_track_error(pos)
                idx = self.route_planner.get_active_waypoint_index()
                total = self.route_planner.get_waypoint_count()
                self.status_panel.update_navigation(idx + 1, total, dist, eta, xtk)

            _t3 = _t.perf_counter()

        self.autopilot.update()

        mode = self.autopilot.get_mode()
        status = self.autopilot.get_status()
        if mode == AutopilotMode.MANUAL:
            self.status_panel.update_autopilot('MANUAL', status)
        elif mode == AutopilotMode.NAV:
            self.status_panel.update_autopilot('NAV', status)

        _elapsed = (_t.perf_counter() - _t0) * 1000
        if _elapsed > 10:
            _js_ms = (_t2 - _t1) * 1000 if pos else 0
            _nav_ms = (_t3 - _t2) * 1000 if pos else 0
            logger.warning("[PERF] _update_display: %.1fms (runJS=%.1fms nav=%.1fms)",
                           _elapsed, _js_ms, _nav_ms)

    def closeEvent(self, event):
        self.autopilot.disengage()
        self.proxy.stop()
        super().closeEvent(event)
