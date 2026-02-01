from pathlib import Path
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
from src.gui.status_panel import StatusPanel
from src.gui.map_widget import MapWidget


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

        self._set_position_mode = False
        self._use_dr_position = False

        self._setup_ui()
        self._setup_connections()
        self._setup_timer()

    def _setup_ui(self):
        self.setWindowTitle("VTOL Co-Pilot")
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
        self.statusbar.showMessage("Disconnected")

    def _create_toolbar(self) -> QFrame:
        frame = QFrame()
        frame.setFrameStyle(QFrame.StyledPanel)
        layout = QHBoxLayout(frame)
        layout.setContentsMargins(10, 5, 10, 5)

        self.btn_connect = QPushButton("Connect")
        self.btn_connect.setFixedWidth(100)
        layout.addWidget(self.btn_connect)

        self.btn_disconnect = QPushButton("Disconnect")
        self.btn_disconnect.setFixedWidth(100)
        self.btn_disconnect.setEnabled(False)
        layout.addWidget(self.btn_disconnect)

        layout.addStretch()

        self.lbl_mode = QLabel("Mode: ---")
        self.lbl_mode.setFont(QFont("Consolas", 12, QFont.Bold))
        layout.addWidget(self.lbl_mode)

        layout.addStretch()

        self.lbl_dr_status = QLabel("DR: OFF")
        self.lbl_dr_status.setFont(QFont("Consolas", 10))
        self.lbl_dr_status.setStyleSheet("color: gray;")
        layout.addWidget(self.lbl_dr_status)

        layout.addSpacing(20)

        self.lbl_status = QLabel("DISCONNECTED")
        self.lbl_status.setStyleSheet("color: red; font-weight: bold;")
        layout.addWidget(self.lbl_status)

        return frame

    def _create_control_panel(self) -> QFrame:
        frame = QFrame()
        frame.setFrameStyle(QFrame.StyledPanel)
        layout = QVBoxLayout(frame)

        mode_layout = QHBoxLayout()

        self.btn_hdg_hold = QPushButton("HDG HOLD")
        self.btn_hdg_hold.setCheckable(True)
        self.btn_hdg_hold.setEnabled(False)
        mode_layout.addWidget(self.btn_hdg_hold)

        self.btn_nav = QPushButton("NAV")
        self.btn_nav.setCheckable(True)
        self.btn_nav.setEnabled(False)
        mode_layout.addWidget(self.btn_nav)

        layout.addLayout(mode_layout)

        action_layout = QHBoxLayout()

        self.btn_set_pos = QPushButton("Set Position")
        self.btn_set_pos.setCheckable(True)
        self.btn_set_pos.setEnabled(False)
        action_layout.addWidget(self.btn_set_pos)

        self.btn_load_route = QPushButton("Load Route")
        self.btn_load_route.setEnabled(False)
        action_layout.addWidget(self.btn_load_route)

        layout.addLayout(action_layout)

        dr_layout = QHBoxLayout()

        self.btn_use_dr = QPushButton("Use DR Position")
        self.btn_use_dr.setCheckable(True)
        self.btn_use_dr.setEnabled(False)
        dr_layout.addWidget(self.btn_use_dr)

        self.btn_clear_track = QPushButton("Clear Track")
        self.btn_clear_track.setEnabled(False)
        dr_layout.addWidget(self.btn_clear_track)

        layout.addLayout(dr_layout)

        return frame

    def _setup_connections(self):
        self.btn_connect.clicked.connect(self._on_connect)
        self.btn_disconnect.clicked.connect(self._on_disconnect)
        self.btn_set_pos.clicked.connect(self._on_set_position_toggle)
        self.btn_load_route.clicked.connect(self._on_load_route)
        self.btn_use_dr.clicked.connect(self._on_use_dr_toggle)
        self.btn_clear_track.clicked.connect(self._on_clear_track)

        self.map_widget.bridge.position_clicked.connect(self._on_map_clicked)

        self.event_bus.subscribe(Event.CONNECTION_RESTORED, self._on_connection_restored)
        self.event_bus.subscribe(Event.CONNECTION_LOST, self._on_connection_lost)

    def _setup_timer(self):
        self.update_timer = QTimer()
        self.update_timer.timeout.connect(self._update_display)
        self.update_timer.start(100)

    def _on_connect(self):
        self.statusbar.showMessage("Connecting...")
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
        self.lbl_status.setText("CONNECTED")
        self.lbl_status.setStyleSheet("color: green; font-weight: bold;")
        self.statusbar.showMessage(f"Connected to SITL on port {self.config.mavlink.sitl_port}")

    def _on_connection_lost(self, data):
        self.btn_connect.setEnabled(True)
        self.btn_disconnect.setEnabled(False)
        self.btn_hdg_hold.setEnabled(False)
        self.btn_nav.setEnabled(False)
        self.btn_set_pos.setEnabled(False)
        self.btn_load_route.setEnabled(False)
        self.btn_use_dr.setEnabled(False)
        self.btn_clear_track.setEnabled(False)
        self.lbl_status.setText("DISCONNECTED")
        self.lbl_status.setStyleSheet("color: red; font-weight: bold;")
        self.statusbar.showMessage("Disconnected")

    def _on_set_position_toggle(self):
        self._set_position_mode = self.btn_set_pos.isChecked()
        if self._set_position_mode:
            self.statusbar.showMessage("Click on map to set position...")
            self.btn_set_pos.setStyleSheet("background-color: #ffcc00;")
        else:
            self.statusbar.showMessage("Position set mode cancelled")
            self.btn_set_pos.setStyleSheet("")

    def _on_map_clicked(self, lat: float, lon: float):
        if self._set_position_mode:
            self.dead_reckoning.set_position(lat, lon)
            self._set_position_mode = False
            self.btn_set_pos.setChecked(False)
            self.btn_set_pos.setStyleSheet("")
            self.map_widget.set_aircraft_position(lat, lon)
            self.statusbar.showMessage(f"Position set to {lat:.6f}, {lon:.6f}")

    def _on_load_route(self):
        routes_dir = Path(__file__).parent.parent.parent / "routes"
        routes_dir.mkdir(exist_ok=True)

        file_path, _ = QFileDialog.getOpenFileName(
            self, "Load Route", str(routes_dir), "JSON Files (*.json)"
        )

        if file_path:
            route = self.route_planner.load_route(Path(file_path))
            if route:
                waypoints = self.route_planner.get_waypoints_for_display()
                self.map_widget.set_waypoints(waypoints)
                self.statusbar.showMessage(f"Loaded route: {route.name} ({len(route.waypoints)} waypoints)")
            else:
                QMessageBox.warning(self, "Error", "Failed to load route file")

    def _on_use_dr_toggle(self):
        self._use_dr_position = self.btn_use_dr.isChecked()
        if self._use_dr_position:
            self.lbl_dr_status.setText("DR: ON")
            self.lbl_dr_status.setStyleSheet("color: #00ff00; font-weight: bold;")
        else:
            self.lbl_dr_status.setText("DR: OFF")
            self.lbl_dr_status.setStyleSheet("color: gray;")

    def _on_clear_track(self):
        self.dead_reckoning.clear_track()
        self.map_widget.clear_track()
        self.statusbar.showMessage("Track cleared")

    def _update_display(self):
        if not self.proxy.is_connected():
            return

        telemetry = self.proxy.get_telemetry()
        self.status_panel.update_telemetry(telemetry)
        self.lbl_mode.setText(f"Mode: {telemetry.mode}")

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
                        self.statusbar.showMessage(f"Reached waypoint {old_idx + 1}, next: {new_idx + 1}")

                distance = self.route_planner.distance_to_waypoint(display_position)
                eta = self.route_planner.eta_to_waypoint(display_position, telemetry.groundspeed)
                xtk = self.route_planner.cross_track_error(display_position)
                wp_idx = self.route_planner.get_active_waypoint_index()
                wp_total = self.route_planner.get_waypoint_count()

                self.status_panel.update_navigation(wp_idx + 1, wp_total, distance, eta, xtk)

    def closeEvent(self, event):
        self.proxy.stop()
        super().closeEvent(event)
