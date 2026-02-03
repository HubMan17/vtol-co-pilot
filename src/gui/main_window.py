from pathlib import Path
from typing import Optional
from PyQt5.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QPushButton, QLabel, QFrame, QSplitter, QStatusBar, QFileDialog, QMessageBox
)
from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtGui import QFont

from src.core.config import AppConfig
from src.core.events import EventBus, Event
from src.mavlink.proxy import MAVLinkProxy
from src.mavlink.telemetry import LatLon
from src.navigation.dead_reckoning import DeadReckoningEngine
from src.navigation.route_planner import RoutePlanner
from src.autopilot.autopilot_manager import AutopilotManager, AutopilotMode
from src.gui.status_panel import StatusPanel
from src.gui.map_widget import MapWidget
from src.gui.waypoint_dialog import WaypointDialog


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

        self.dead_reckoning = DeadReckoningEngine(
            drift_coefficient=config.navigation.drift_coefficient
        )
        self.route_planner = RoutePlanner()
        self.autopilot = AutopilotManager(self.proxy, config.autopilot)
        self.autopilot.set_route_planner(self.route_planner)

        self._set_position_mode = False
        self._use_dr_position = False
        self._home_position: Optional[LatLon] = None

        self._setup_ui()
        self._setup_connections()
        self._setup_timer()

    def _setup_ui(self):
        self.setWindowTitle("VTOL Со-Пилот")
        self.setMinimumSize(1200, 800)

        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(5, 5, 5, 5)
        main_layout.setSpacing(5)

        toolbar = self._create_toolbar()
        main_layout.addWidget(toolbar)

        splitter = QSplitter(Qt.Horizontal)

        self.map_widget = MapWidget(self.config.gui.map_center, self.config.gui.map_zoom)
        splitter.addWidget(self.map_widget)

        right_panel = QWidget()
        right_layout = QVBoxLayout(right_panel)
        right_layout.setContentsMargins(0, 0, 0, 0)

        self.status_panel = StatusPanel()
        right_layout.addWidget(self.status_panel)

        control_panel = self._create_control_panel()
        right_layout.addWidget(control_panel)

        splitter.addWidget(right_panel)
        splitter.setSizes([800, 400])

        main_layout.addWidget(splitter)

        self.statusbar = QStatusBar()
        self.setStatusBar(self.statusbar)
        self.statusbar.showMessage("Отключено")

    def _create_toolbar(self) -> QFrame:
        frame = QFrame()
        frame.setFrameStyle(QFrame.StyledPanel)
        layout = QHBoxLayout(frame)
        layout.setContentsMargins(10, 5, 10, 5)

        self.btn_connect = QPushButton("Подключить")
        self.btn_connect.setFixedWidth(120)
        layout.addWidget(self.btn_connect)

        self.btn_disconnect = QPushButton("Отключить")
        self.btn_disconnect.setFixedWidth(120)
        self.btn_disconnect.setEnabled(False)
        layout.addWidget(self.btn_disconnect)

        layout.addStretch()

        self.lbl_mode = QLabel("Режим: ---")
        self.lbl_mode.setFont(QFont("Consolas", 12, QFont.Bold))
        layout.addWidget(self.lbl_mode)

        layout.addStretch()

        self.lbl_dr_status = QLabel("Счисление: ВЫКЛ")
        self.lbl_dr_status.setFont(QFont("Consolas", 10))
        self.lbl_dr_status.setStyleSheet("color: gray;")
        layout.addWidget(self.lbl_dr_status)

        layout.addSpacing(20)

        self.lbl_status = QLabel("ОТКЛЮЧЕНО")
        self.lbl_status.setStyleSheet("color: red; font-weight: bold;")
        layout.addWidget(self.lbl_status)

        return frame

    def _create_control_panel(self) -> QFrame:
        frame = QFrame()
        frame.setFrameStyle(QFrame.StyledPanel)
        layout = QVBoxLayout(frame)

        mode_layout = QHBoxLayout()

        self.btn_hdg_hold = QPushButton("Удержание курса")
        self.btn_hdg_hold.setCheckable(True)
        self.btn_hdg_hold.setEnabled(False)
        mode_layout.addWidget(self.btn_hdg_hold)

        self.btn_nav = QPushButton("Навигация")
        self.btn_nav.setCheckable(True)
        self.btn_nav.setEnabled(False)
        mode_layout.addWidget(self.btn_nav)

        layout.addLayout(mode_layout)

        action_layout = QHBoxLayout()

        self.btn_set_pos = QPushButton("Установить позицию")
        self.btn_set_pos.setCheckable(True)
        self.btn_set_pos.setEnabled(False)
        action_layout.addWidget(self.btn_set_pos)

        self.btn_load_route = QPushButton("Загрузить маршрут")
        self.btn_load_route.setEnabled(False)
        action_layout.addWidget(self.btn_load_route)

        layout.addLayout(action_layout)

        dr_layout = QHBoxLayout()

        self.btn_use_dr = QPushButton("Использовать счисление")
        self.btn_use_dr.setCheckable(True)
        self.btn_use_dr.setEnabled(False)
        dr_layout.addWidget(self.btn_use_dr)

        self.btn_clear_track = QPushButton("Очистить трек")
        self.btn_clear_track.setEnabled(False)
        dr_layout.addWidget(self.btn_clear_track)

        layout.addLayout(dr_layout)

        map_layout = QHBoxLayout()

        self.btn_follow = QPushButton("Следовать")
        self.btn_follow.setCheckable(True)
        self.btn_follow.setEnabled(False)
        map_layout.addWidget(self.btn_follow)

        self.btn_home = QPushButton("Домой")
        self.btn_home.setCheckable(True)
        self.btn_home.setEnabled(False)
        map_layout.addWidget(self.btn_home)

        layout.addLayout(map_layout)

        return frame

    def _setup_connections(self):
        self.btn_connect.clicked.connect(self._on_connect)
        self.btn_disconnect.clicked.connect(self._on_disconnect)
        self.btn_set_pos.clicked.connect(self._on_set_position_toggle)
        self.btn_load_route.clicked.connect(self._on_load_route)
        self.btn_use_dr.clicked.connect(self._on_use_dr_toggle)
        self.btn_clear_track.clicked.connect(self._on_clear_track)
        self.btn_hdg_hold.clicked.connect(self._on_heading_hold_toggle)
        self.btn_nav.clicked.connect(self._on_nav_toggle)
        self.btn_follow.clicked.connect(self._on_follow_toggle)
        self.btn_home.clicked.connect(self._on_home_toggle)

        self.map_widget.bridge.position_clicked.connect(self._on_map_clicked)
        self.map_widget.set_position_requested.connect(self._on_context_set_position)
        self.map_widget.add_waypoint_requested.connect(self._on_context_add_waypoint)
        self.map_widget.set_home_requested.connect(self._on_context_set_home)

        self.event_bus.subscribe(Event.CONNECTION_RESTORED, self._on_connection_restored)
        self.event_bus.subscribe(Event.CONNECTION_LOST, self._on_connection_lost)
        self.event_bus.subscribe(Event.AUTOPILOT_ENGAGE, self._on_autopilot_engage)
        self.event_bus.subscribe(Event.AUTOPILOT_DISENGAGE, self._on_autopilot_disengage)
        self.event_bus.subscribe(Event.WAYPOINT_REACHED, self._on_waypoint_reached)

    def _setup_timer(self):
        self.update_timer = QTimer()
        self.update_timer.timeout.connect(self._update_display)
        self.update_timer.start(100)

    def _on_connect(self):
        self.statusbar.showMessage("Подключение...")
        if self.proxy.connect():
            self.proxy.start()
            self.proxy.request_data_streams(10)

    def _on_disconnect(self):
        self.proxy.stop()
        self._on_connection_lost(None)

    def _on_connection_restored(self, data):
        self.btn_connect.setEnabled(False)
        self.btn_disconnect.setEnabled(True)
        self.btn_hdg_hold.setEnabled(True)
        self.btn_nav.setEnabled(True)
        self.btn_set_pos.setEnabled(True)
        self.btn_load_route.setEnabled(True)
        self.btn_use_dr.setEnabled(True)
        self.btn_clear_track.setEnabled(True)
        self.btn_follow.setEnabled(True)
        self.btn_home.setEnabled(True)
        self.lbl_status.setText("ПОДКЛЮЧЕНО")
        self.lbl_status.setStyleSheet("color: green; font-weight: bold;")
        self.statusbar.showMessage(f"Подключено к SITL на порту {self.config.mavlink.sitl_port}")

    def _on_connection_lost(self, data):
        self.btn_connect.setEnabled(True)
        self.btn_disconnect.setEnabled(False)
        self.btn_hdg_hold.setEnabled(False)
        self.btn_nav.setEnabled(False)
        self.btn_set_pos.setEnabled(False)
        self.btn_load_route.setEnabled(False)
        self.btn_use_dr.setEnabled(False)
        self.btn_clear_track.setEnabled(False)
        self.btn_follow.setEnabled(False)
        self.btn_home.setEnabled(False)
        self.lbl_status.setText("ОТКЛЮЧЕНО")
        self.lbl_status.setStyleSheet("color: red; font-weight: bold;")
        self.statusbar.showMessage("Отключено")

    def _on_set_position_toggle(self):
        self._set_position_mode = self.btn_set_pos.isChecked()
        if self._set_position_mode:
            self.statusbar.showMessage("Кликните на карте для установки позиции...")
            self.btn_set_pos.setStyleSheet("background-color: #ffcc00;")
        else:
            self.statusbar.showMessage("Режим установки позиции отменён")
            self.btn_set_pos.setStyleSheet("")

    def _on_map_clicked(self, lat: float, lon: float):
        if self._set_position_mode:
            self._set_dr_position(lat, lon)

    def _on_context_set_position(self, lat: float, lon: float):
        self._set_dr_position(lat, lon)

    def _set_dr_position(self, lat: float, lon: float):
        self.dead_reckoning.set_position(lat, lon)
        self._set_position_mode = False
        self.btn_set_pos.setChecked(False)
        self.btn_set_pos.setStyleSheet("")
        self.map_widget.set_aircraft_position(lat, lon)

        if not self._use_dr_position:
            self._use_dr_position = True
            self.btn_use_dr.setChecked(True)
            self.lbl_dr_status.setText("Счисление: ВКЛ")
            self.lbl_dr_status.setStyleSheet("color: #00ff00; font-weight: bold;")

        self.statusbar.showMessage(f"Позиция установлена: {lat:.6f}, {lon:.6f}")

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
            orbit_radius=wp_data.get('orbit_radius', 100.0),
            orbit_turns=wp_data.get('orbit_turns', 1),
            target_altitude=wp_data.get('target_altitude', 0.0)
        )

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

    def _on_use_dr_toggle(self):
        self._use_dr_position = self.btn_use_dr.isChecked()
        if self._use_dr_position:
            self.lbl_dr_status.setText("Счисление: ВКЛ")
            self.lbl_dr_status.setStyleSheet("color: #00ff00; font-weight: bold;")
        else:
            self.lbl_dr_status.setText("Счисление: ВЫКЛ")
            self.lbl_dr_status.setStyleSheet("color: gray;")

    def _on_clear_track(self):
        self.dead_reckoning.clear_track()
        self.map_widget.clear_track()
        self.statusbar.showMessage("Трек очищен")

    def _on_follow_toggle(self):
        follow = self.btn_follow.isChecked()
        self.map_widget.set_follow_mode(follow)
        if follow:
            self.btn_follow.setStyleSheet("background-color: #00cc00;")
            self.statusbar.showMessage("Слежение за самолётом включено")
        else:
            self.btn_follow.setStyleSheet("")
            self.statusbar.showMessage("Слежение отключено")

    def _on_context_set_home(self, lat: float, lon: float):
        self._home_position = LatLon(lat, lon)
        self.map_widget.set_home_marker(lat, lon)
        self.statusbar.showMessage(f"Дом установлен: {lat:.6f}, {lon:.6f}")

    def _on_home_toggle(self):
        if self.btn_home.isChecked():
            if not self._home_position:
                self.btn_home.setChecked(False)
                self.statusbar.showMessage("Сначала установите точку Дом на карте")
                return

            self.route_planner.create_route("Home")
            self.route_planner.clear_waypoints()
            self.route_planner.add_waypoint(self._home_position.lat, self._home_position.lon, 100.0)
            self._refresh_map_waypoints()

            if self.autopilot.engage_nav():
                self.btn_home.setStyleSheet("background-color: #ff6600;")
                self.btn_nav.setChecked(False)
                self.btn_nav.setStyleSheet("")
                self.btn_hdg_hold.setChecked(False)
                self.btn_hdg_hold.setStyleSheet("")
                self.statusbar.showMessage("Возврат домой активирован")
            else:
                self.btn_home.setChecked(False)
                self.statusbar.showMessage("Не удалось активировать возврат домой")
        else:
            self.autopilot.disengage("Отключено пользователем")

    def _on_heading_hold_toggle(self):
        if self.btn_hdg_hold.isChecked():
            if self.autopilot.engage_heading_hold():
                self.btn_hdg_hold.setStyleSheet("background-color: #00cc00;")
                hdg = self.autopilot.get_target_heading()
                self.statusbar.showMessage(f"Удержание курса: {hdg:.0f}°")
            else:
                self.btn_hdg_hold.setChecked(False)
                self.statusbar.showMessage("Не удалось включить удержание курса")
        else:
            self.autopilot.disengage("Отключено пользователем")

    def _on_nav_toggle(self):
        if self.btn_nav.isChecked():
            if self.autopilot.engage_nav():
                self.btn_nav.setStyleSheet("background-color: #00cc00;")
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

    def _on_autopilot_engage(self, data):
        mode = data.get('mode', '')
        if mode == 'HEADING_HOLD':
            self.btn_hdg_hold.setChecked(True)
            self.btn_hdg_hold.setStyleSheet("background-color: #00cc00;")
            self.btn_nav.setChecked(False)
            self.btn_nav.setStyleSheet("")
            target = data.get('target', 0)
            self.statusbar.showMessage(f"Удержание курса: {target:.0f}°")
        elif mode == 'NAV':
            self.btn_nav.setChecked(True)
            self.btn_nav.setStyleSheet("background-color: #00cc00;")
            self.btn_hdg_hold.setChecked(False)
            self.btn_hdg_hold.setStyleSheet("")
            wp_id = data.get('waypoint', 0)
            total = data.get('total', 0)
            self.statusbar.showMessage(f"Навигация: точка {wp_id}/{total}")

    def _on_autopilot_disengage(self, data):
        self.btn_hdg_hold.setChecked(False)
        self.btn_hdg_hold.setStyleSheet("")
        self.btn_nav.setChecked(False)
        self.btn_nav.setStyleSheet("")
        self.btn_home.setChecked(False)
        self.btn_home.setStyleSheet("")

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

    def _update_display(self):
        if not self.proxy.is_connected():
            return

        telemetry = self.proxy.get_telemetry()
        self.status_panel.update_telemetry(telemetry)
        self.lbl_mode.setText(f"Режим: {telemetry.mode}")

        dr_position = self.dead_reckoning.update(telemetry)

        if self._use_dr_position and dr_position:
            display_position = dr_position
        elif telemetry.position:
            display_position = telemetry.position
        else:
            display_position = dr_position

        if display_position:
            self.map_widget.update_aircraft(
                display_position.lat,
                display_position.lon,
                telemetry.heading
            )

            if self.route_planner.get_route():
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

        self.autopilot.update(display_position)
        self._update_autopilot_display()

    def _update_autopilot_display(self):
        mode = self.autopilot.get_mode()
        if mode == AutopilotMode.MANUAL:
            self.status_panel.update_autopilot('MANUAL')
        elif mode == AutopilotMode.HEADING_HOLD:
            target = self.autopilot.get_target_heading()
            error = self.autopilot.get_heading_error()
            self.status_panel.update_autopilot('HEADING_HOLD', target, error)
        elif mode == AutopilotMode.NAV:
            target = self.autopilot.get_target_heading()
            error = self.autopilot.get_heading_error()
            self.status_panel.update_autopilot('NAV', target, error)

    def closeEvent(self, event):
        self.autopilot.disengage()
        self.proxy.stop()
        super().closeEvent(event)
