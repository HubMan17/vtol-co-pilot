from pathlib import Path
from typing import Optional
from PyQt5.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QPushButton, QLabel, QFrame, QSplitter, QStatusBar, QFileDialog,
    QMessageBox, QSpinBox, QApplication
)
from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtGui import QFont

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
from src.gui.theme import STYLESHEET, Colors, Fonts


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

        self._set_position_mode = False
        self._set_home_mode = False
        self._home_position: Optional[LatLon] = None

        # Apply global dark theme
        QApplication.instance().setStyleSheet(STYLESHEET)

        self._setup_ui()
        self._setup_connections()
        self._setup_timer()

    def _setup_ui(self):
        self.setWindowTitle("VTOL Со-Пилот")
        self.setMinimumSize(1200, 800)

        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # --- Header bar ---
        header = self._create_header()
        main_layout.addWidget(header)

        # --- Content: sidebar + map ---
        content = QWidget()
        content_layout = QHBoxLayout(content)
        content_layout.setContentsMargins(0, 0, 0, 0)
        content_layout.setSpacing(0)

        # Left sidebar (status + controls)
        sidebar = QWidget()
        sidebar.setStyleSheet(f"background-color: {Colors.BG_SIDEBAR};")
        sidebar_layout = QVBoxLayout(sidebar)
        sidebar_layout.setContentsMargins(0, 0, 0, 0)
        sidebar_layout.setSpacing(0)

        self.status_panel = StatusPanel()
        sidebar_layout.addWidget(self.status_panel, 1)

        # Control buttons at bottom of sidebar
        control_panel = self._create_control_panel()
        sidebar_layout.addWidget(control_panel)

        # Separator line between sidebar and map
        sep = QFrame()
        sep.setFixedWidth(1)
        sep.setStyleSheet(f"background-color: {Colors.BORDER};")

        # Map (main content area)
        self.map_widget = MapWidget(self.config.gui.map_center, self.config.gui.map_zoom)

        content_layout.addWidget(sidebar)
        content_layout.addWidget(sep)
        content_layout.addWidget(self.map_widget, 1)

        main_layout.addWidget(content, 1)

        # Status bar
        self.statusbar = QStatusBar()
        self.setStatusBar(self.statusbar)
        self.statusbar.showMessage("Отключено")

    def _create_header(self) -> QFrame:
        header = QFrame()
        header.setFixedHeight(52)
        header.setStyleSheet(f"""
            QFrame {{
                background-color: {Colors.BG_SIDEBAR};
                border-bottom: 1px solid {Colors.BORDER};
            }}
        """)

        layout = QHBoxLayout(header)
        layout.setContentsMargins(16, 0, 16, 0)
        layout.setSpacing(12)

        # Connect / disconnect
        self.btn_connect = QPushButton("Подключить")
        self.btn_connect.setProperty("cssClass", "primary")
        self.btn_connect.setFixedHeight(34)
        self.btn_connect.setFixedWidth(130)
        layout.addWidget(self.btn_connect)

        self.btn_disconnect = QPushButton("Отключить")
        self.btn_disconnect.setFixedHeight(34)
        self.btn_disconnect.setFixedWidth(130)
        self.btn_disconnect.setEnabled(False)
        layout.addWidget(self.btn_disconnect)

        layout.addStretch()

        # ArduPilot mode badge
        self.lbl_mode = QLabel("---")
        self.lbl_mode.setFont(QFont(Fonts.MONO, 13, QFont.Bold))
        self.lbl_mode.setStyleSheet(f"""
            color: {Colors.TEXT_SECONDARY};
            font-size: 13px;
            font-weight: 700;
            padding: 4px 14px;
            border-radius: 6px;
            background-color: {Colors.BG_INPUT};
        """)
        self.lbl_mode.setAlignment(Qt.AlignCenter)
        layout.addWidget(self.lbl_mode)

        layout.addStretch()

        # Connection status indicator
        self.lbl_status = QLabel("ОТКЛЮЧЕНО")
        self.lbl_status.setFont(QFont(Fonts.FAMILY, 11, QFont.Bold))
        self.lbl_status.setStyleSheet(f"""
            color: {Colors.ERROR};
            font-size: 11px;
            font-weight: 700;
            padding: 4px 12px;
            border-radius: 12px;
            background-color: {Colors.ERROR_BG};
        """)
        layout.addWidget(self.lbl_status)

        return header

    def _create_control_panel(self) -> QFrame:
        panel = QFrame()
        panel.setStyleSheet(f"""
            QFrame {{
                background-color: {Colors.BG_SIDEBAR};
                border-top: 1px solid {Colors.BORDER};
            }}
        """)

        layout = QVBoxLayout(panel)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(8)

        # Main autopilot button
        self.btn_nav = QPushButton("▶  Навигация")
        self.btn_nav.setCheckable(True)
        self.btn_nav.setEnabled(False)
        self.btn_nav.setFixedHeight(40)
        self.btn_nav.setFont(QFont(Fonts.FAMILY, 13, QFont.Bold))
        self.btn_nav.setStyleSheet(f"""
            QPushButton {{
                background-color: {Colors.BG_INPUT};
                color: {Colors.TEXT_DATA};
                border: 1px solid {Colors.BORDER_BUTTON};
                border-radius: 10px;
                padding: 8px 16px;
                font-size: 13px;
                font-weight: 600;
            }}
            QPushButton:hover {{
                background-color: {Colors.BG_TOOLTIP};
                border-color: {Colors.TEXT_TERTIARY};
            }}
            QPushButton:checked {{
                background-color: {Colors.SUCCESS};
                color: #ffffff;
                border-color: {Colors.SUCCESS};
            }}
            QPushButton:disabled {{
                background-color: {Colors.BG_CARD};
                color: {Colors.TEXT_TERTIARY};
                border-color: {Colors.BORDER};
            }}
        """)
        layout.addWidget(self.btn_nav)

        # Action row 1
        row1 = QHBoxLayout()
        row1.setSpacing(8)

        self.btn_set_pos = QPushButton("📍 Коррекция")
        self.btn_set_pos.setCheckable(True)
        self.btn_set_pos.setEnabled(False)
        self.btn_set_pos.setFixedHeight(34)
        row1.addWidget(self.btn_set_pos)

        self.btn_set_home = QPushButton("🏠 Дом")
        self.btn_set_home.setCheckable(True)
        self.btn_set_home.setEnabled(False)
        self.btn_set_home.setFixedHeight(34)
        row1.addWidget(self.btn_set_home)

        self.btn_load_route = QPushButton("📂 Маршрут")
        self.btn_load_route.setEnabled(False)
        self.btn_load_route.setFixedHeight(34)
        row1.addWidget(self.btn_load_route)

        layout.addLayout(row1)

        # Action row 2
        row2 = QHBoxLayout()
        row2.setSpacing(8)

        self.btn_follow = QPushButton("👁 Слежение")
        self.btn_follow.setCheckable(True)
        self.btn_follow.setEnabled(False)
        self.btn_follow.setFixedHeight(34)
        row2.addWidget(self.btn_follow)

        self.btn_home = QPushButton("⟲  Домой")
        self.btn_home.setCheckable(True)
        self.btn_home.setEnabled(False)
        self.btn_home.setFixedHeight(34)
        self.btn_home.setStyleSheet(f"""
            QPushButton {{
                background-color: {Colors.BG_INPUT};
                color: {Colors.TEXT_DATA};
                border: 1px solid {Colors.BORDER_BUTTON};
                border-radius: 8px;
                padding: 8px 16px;
                font-size: {Fonts.SIZE_BODY}px;
                font-weight: 500;
            }}
            QPushButton:hover {{
                background-color: {Colors.BG_TOOLTIP};
                border-color: {Colors.TEXT_TERTIARY};
            }}
            QPushButton:checked {{
                background-color: {Colors.WARNING};
                color: #000000;
                border-color: {Colors.WARNING};
            }}
            QPushButton:disabled {{
                background-color: {Colors.BG_CARD};
                color: {Colors.TEXT_TERTIARY};
                border-color: {Colors.BORDER};
            }}
        """)
        row2.addWidget(self.btn_home)

        self.btn_clear_track = QPushButton("✕ Трек")
        self.btn_clear_track.setEnabled(False)
        self.btn_clear_track.setFixedHeight(34)
        row2.addWidget(self.btn_clear_track)

        layout.addLayout(row2)

        # Waypoint navigation row
        wp_row = QHBoxLayout()
        wp_row.setSpacing(6)

        self.btn_wp_prev = QPushButton("◀")
        self.btn_wp_prev.setProperty("cssClass", "icon")
        self.btn_wp_prev.setEnabled(False)
        wp_row.addWidget(self.btn_wp_prev)

        wp_label = QLabel("Точка:")
        wp_label.setStyleSheet(f"color: {Colors.TEXT_SECONDARY}; font-size: {Fonts.SIZE_SMALL}px;")
        wp_row.addWidget(wp_label)

        self.spin_waypoint = QSpinBox()
        self.spin_waypoint.setMinimum(1)
        self.spin_waypoint.setMaximum(1)
        self.spin_waypoint.setEnabled(False)
        self.spin_waypoint.setFixedWidth(60)
        self.spin_waypoint.setFixedHeight(32)
        wp_row.addWidget(self.spin_waypoint)

        self.btn_wp_next = QPushButton("▶")
        self.btn_wp_next.setProperty("cssClass", "icon")
        self.btn_wp_next.setEnabled(False)
        wp_row.addWidget(self.btn_wp_next)

        wp_row.addStretch()
        layout.addLayout(wp_row)

        return panel

    def _setup_connections(self):
        self.btn_connect.clicked.connect(self._on_connect)
        self.btn_disconnect.clicked.connect(self._on_disconnect)
        self.btn_set_pos.clicked.connect(self._on_set_position_toggle)
        self.btn_set_home.clicked.connect(self._on_set_home_toggle)
        self.btn_load_route.clicked.connect(self._on_load_route)
        self.btn_clear_track.clicked.connect(self._on_clear_track)
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

    # ─── Connection ───

    def _on_connect(self):
        self.statusbar.showMessage("Подключение...")
        if self.proxy.connect():
            self.proxy.start()
            self.proxy.request_data_streams(4)

    def _on_disconnect(self):
        self.proxy.stop()
        self._on_connection_lost(None)

    def _on_connection_restored(self, data):
        self.btn_connect.setEnabled(False)
        self.btn_disconnect.setEnabled(True)
        self.btn_nav.setEnabled(True)
        self.btn_set_pos.setEnabled(True)
        self.btn_set_home.setEnabled(True)
        self.btn_load_route.setEnabled(True)
        self.btn_clear_track.setEnabled(True)
        self.btn_follow.setEnabled(True)
        self.btn_home.setEnabled(True)

        self.lbl_status.setText("ПОДКЛЮЧЕНО")
        self.lbl_status.setStyleSheet(f"""
            color: {Colors.SUCCESS};
            font-size: 11px; font-weight: 700;
            padding: 4px 12px; border-radius: 12px;
            background-color: {Colors.SUCCESS_BG};
        """)
        self.statusbar.showMessage(f"Подключено к SITL на порту {self.config.mavlink.sitl_port}")

        self.btn_follow.setChecked(True)
        self.map_widget.set_follow_mode(True)

    def _on_connection_lost(self, data):
        self.btn_connect.setEnabled(True)
        self.btn_disconnect.setEnabled(False)
        self.btn_nav.setEnabled(False)
        self.btn_set_pos.setEnabled(False)
        self.btn_set_home.setEnabled(False)
        self.btn_load_route.setEnabled(False)
        self.btn_clear_track.setEnabled(False)
        self.btn_follow.setEnabled(False)
        self.btn_home.setEnabled(False)

        self.lbl_status.setText("ОТКЛЮЧЕНО")
        self.lbl_status.setStyleSheet(f"""
            color: {Colors.ERROR};
            font-size: 11px; font-weight: 700;
            padding: 4px 12px; border-radius: 12px;
            background-color: {Colors.ERROR_BG};
        """)
        self.statusbar.showMessage("Отключено")

    # ─── Position / Home ───

    def _on_set_position_toggle(self):
        self._set_position_mode = self.btn_set_pos.isChecked()
        if self._set_position_mode:
            self.statusbar.showMessage("Кликните на карте для коррекции позиции EKF...")
        else:
            self.statusbar.showMessage("Режим коррекции позиции отменён")

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
        self.statusbar.showMessage(f"Коррекция позиции отправлена: {lat:.6f}, {lon:.6f}")

    def _on_context_add_waypoint(self, lat: float, lon: float):
        dialog = WaypointDialog(self, lat, lon)
        if dialog.exec_() != WaypointDialog.Accepted:
            return

        wp_data = dialog.get_waypoint_data()

        if not self.route_planner.get_route():
            self.route_planner.create_route("Новый маршрут")

        wp = self.route_planner.add_waypoint(
            lat=wp_data['lat'],
            lon=wp_data['lon'],
            altitude=wp_data['altitude'],
            radius=wp_data['radius'],
            action=wp_data['action'],
            orbit_radius=wp_data.get('orbit_radius', 150.0),
            orbit_turns=wp_data.get('orbit_turns', 1),
            climb_enroute=wp_data.get('climb_enroute', False)
        )

        if self.autopilot.is_engaged():
            status = self.autopilot.get_status()
            if status.get('is_orbiting') or status.get('returning_home'):
                new_idx = self.route_planner.get_waypoint_count() - 1
                self.route_planner.set_active_waypoint(new_idx)

        self._refresh_map_waypoints()
        self.statusbar.showMessage(f"Добавлена точка {wp.id}: {lat:.6f}, {lon:.6f}")

    def _on_load_route(self):
        routes_dir = Path(__file__).parent.parent.parent / "routes"
        routes_dir.mkdir(exist_ok=True)

        file_path, _ = QFileDialog.getOpenFileName(
            self, "Загрузить маршрут", str(routes_dir), "JSON файлы (*.json)"
        )

        if file_path:
            route = self.route_planner.load_route(Path(file_path))
            if route:
                self._refresh_map_waypoints()
                self.statusbar.showMessage(f"Загружен маршрут: {route.name} ({len(route.waypoints)} точек)")
            else:
                QMessageBox.warning(self, "Ошибка", "Не удалось загрузить файл маршрута")

    def _refresh_map_waypoints(self):
        waypoints = self.route_planner.get_waypoints_for_display()
        active_idx = self.route_planner.get_active_waypoint_index()
        self.map_widget.set_waypoints(waypoints, active_idx)
        self._update_waypoint_controls()

    def _update_waypoint_controls(self):
        wp_count = self.route_planner.get_waypoint_count()
        has_waypoints = wp_count > 0

        self.btn_wp_prev.setEnabled(has_waypoints)
        self.btn_wp_next.setEnabled(has_waypoints)
        self.spin_waypoint.setEnabled(has_waypoints)

        if has_waypoints:
            self.spin_waypoint.blockSignals(True)
            self.spin_waypoint.setMaximum(wp_count)
            self.spin_waypoint.setValue(self.route_planner.get_active_waypoint_index() + 1)
            self.spin_waypoint.blockSignals(False)

    # ─── Waypoints ───

    def _on_wp_prev(self):
        self.route_planner.prev_waypoint()
        self._refresh_map_waypoints()
        wp = self.route_planner.get_active_waypoint()
        if wp:
            self.statusbar.showMessage(f"Активная точка: {wp.id}")

    def _on_wp_next(self):
        self.route_planner.next_waypoint()
        self._refresh_map_waypoints()
        wp = self.route_planner.get_active_waypoint()
        if wp:
            self.statusbar.showMessage(f"Активная точка: {wp.id}")

    def _on_wp_select(self, value: int):
        self.route_planner.set_active_waypoint(value - 1)
        self._refresh_map_waypoints()
        wp = self.route_planner.get_active_waypoint()
        if wp:
            self.statusbar.showMessage(f"Активная точка: {wp.id}")

    # ─── Map controls ───

    def _on_clear_track(self):
        self.map_widget.clear_track()
        self.statusbar.showMessage("Трек очищен")

    def _on_follow_toggle(self):
        follow = self.btn_follow.isChecked()
        self.map_widget.set_follow_mode(follow)
        if follow:
            self.statusbar.showMessage("Слежение за самолётом включено")
        else:
            self.statusbar.showMessage("Слежение отключено")

    # ─── Home ───

    def _on_context_set_home(self, lat: float, lon: float):
        self._set_home_position(lat, lon)

    def _set_home_position(self, lat: float, lon: float):
        self._home_position = LatLon(lat, lon)
        self.autopilot.set_home_position(self._home_position)
        self.map_widget.set_home_marker(lat, lon)
        self._set_home_mode = False
        self.btn_set_home.setChecked(False)
        self.statusbar.showMessage(f"Дом установлен: {lat:.6f}, {lon:.6f}")

    def _on_home_toggle(self):
        if self.btn_home.isChecked():
            if not self._home_position:
                self.btn_home.setChecked(False)
                self.statusbar.showMessage("Сначала установите точку Дом на карте")
                return

            if self.autopilot.engage_home():
                self.btn_nav.setChecked(False)
                self.statusbar.showMessage("Возврат домой активирован")
            else:
                self.btn_home.setChecked(False)
                self.statusbar.showMessage("Не удалось активировать возврат домой")
        else:
            self.autopilot.disengage("Отключено пользователем")

    # ─── Nav ───

    def _on_nav_toggle(self):
        if self.btn_nav.isChecked():
            if self.autopilot.engage_nav():
                route = self.route_planner.get_route()
                wp = self.route_planner.get_active_waypoint()
                if route and wp:
                    self.statusbar.showMessage(f"Навигация: точка {wp.id}/{len(route.waypoints)}")
            else:
                self.btn_nav.setChecked(False)
                if not self.route_planner.get_route():
                    self.statusbar.showMessage("Загрузите маршрут для навигации")
                else:
                    self.statusbar.showMessage("Не удалось включить навигацию")
        else:
            self.autopilot.disengage("Отключено пользователем")

    # ─── Autopilot events ───

    def _on_autopilot_engage(self, data):
        mode = data.get('mode', '')
        if mode == 'NAV':
            self.btn_nav.setChecked(True)
            wp_id = data.get('waypoint', 0)
            total = data.get('total', 0)
            self.statusbar.showMessage(f"Навигация: точка {wp_id}/{total}")

    def _on_autopilot_disengage(self, data):
        self.btn_nav.setChecked(False)
        self.btn_home.setChecked(False)

        reason = data.get('reason', '')
        if reason:
            self.statusbar.showMessage(f"Автопилот отключён: {reason}")
        else:
            self.statusbar.showMessage("Автопилот отключён")

    def _on_waypoint_reached(self, data):
        reached = data.get('reached', 0)
        next_wp = data.get('next', 0)
        total = self.route_planner.get_waypoint_count()
        self.map_widget.update_active_waypoint(next_wp - 1)
        self.statusbar.showMessage(f"Достигнута точка {reached}, следующая: {next_wp}/{total}")

    # ─── Parameter changes ───

    def _on_orbit_radius_changed(self, radius: int):
        self.autopilot.set_orbit_radius(float(radius))
        self.statusbar.showMessage(f"Радиус кружения: {radius} м")

    def _on_target_altitude_changed(self, altitude: int):
        self.autopilot.set_target_altitude(float(altitude))
        self.statusbar.showMessage(f"Целевая высота: {altitude} м")

    def _on_target_airspeed_changed(self, speed: int):
        self.autopilot.set_target_airspeed(float(speed))
        self.statusbar.showMessage(f"Целевая скорость: {speed} м/с")

    def _on_wind_override(self, direction: int, speed: int):
        self.proxy.send_wind_override(direction, speed)
        self.statusbar.showMessage(f"Ветер установлен: {direction}° / {speed} м/с")

    # ─── Display update ───

    def _update_display(self):
        if not self.proxy.is_connected():
            return

        telemetry = self.proxy.get_telemetry()

        self.status_panel.update_telemetry(telemetry)

        # Update mode badge in header
        mode_text = telemetry.mode or "---"
        self.lbl_mode.setText(mode_text)

        display_position = telemetry.position

        if display_position:
            self.map_widget.update_aircraft(
                display_position.lat,
                display_position.lon,
                telemetry.heading
            )

            ap_status = self.autopilot.get_status()
            if ap_status.get('returning_home', False) and self._home_position:
                distance = haversine_distance(
                    display_position.lat, display_position.lon,
                    self._home_position.lat, self._home_position.lon
                )
                eta = eta_seconds(distance, telemetry.groundspeed)
                minutes = int(eta // 60) if eta != float('inf') else 99
                seconds = int(eta % 60) if eta != float('inf') else 99
                self.status_panel.labels["Точка"][0].setText("Дом")
                self.status_panel.labels["Дистанция"][0].setText(f"{distance / 1000:.2f} км")
                self.status_panel.labels["Время приб."][0].setText(f"{minutes:02d}:{seconds:02d}")
                self.status_panel.labels["Бок. уклон."][0].setText("--- м")
            elif self.route_planner.get_route():
                if not self.autopilot.is_engaged():
                    if self.route_planner.is_waypoint_reached(display_position):
                        old_idx = self.route_planner.get_active_waypoint_index()
                        self.route_planner.next_waypoint()
                        new_idx = self.route_planner.get_active_waypoint_index()
                        if new_idx != old_idx:
                            self.map_widget.update_active_waypoint(new_idx)
                            self.statusbar.showMessage(f"Достигнута точка {old_idx + 1}, следующая: {new_idx + 1}")

                distance = self.route_planner.distance_to_waypoint(display_position)
                eta = self.route_planner.eta_to_waypoint(display_position, telemetry.groundspeed)
                xtk = self.route_planner.cross_track_error(display_position)
                wp_idx = self.route_planner.get_active_waypoint_index()
                wp_total = self.route_planner.get_waypoint_count()

                self.status_panel.update_navigation(wp_idx + 1, wp_total, distance, eta, xtk)

        self.autopilot.update()
        self._update_autopilot_display()

    def _update_autopilot_display(self):
        mode = self.autopilot.get_mode()
        status = self.autopilot.get_status()

        if mode == AutopilotMode.MANUAL:
            self.status_panel.update_autopilot('MANUAL', status)
        elif mode == AutopilotMode.NAV:
            self.status_panel.update_autopilot('NAV', status)

    def closeEvent(self, event):
        self.autopilot.disengage()
        self.proxy.stop()
        super().closeEvent(event)
