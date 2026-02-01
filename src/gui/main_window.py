from PyQt5.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QPushButton, QLabel, QFrame, QSplitter, QStatusBar
)
from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtGui import QFont

from src.core.config import AppConfig
from src.core.events import EventBus, Event
from src.mavlink.proxy import MAVLinkProxy
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
        self.btn_set_pos.setEnabled(False)
        action_layout.addWidget(self.btn_set_pos)

        self.btn_load_route = QPushButton("Load Route")
        self.btn_load_route.setEnabled(False)
        action_layout.addWidget(self.btn_load_route)

        layout.addLayout(action_layout)

        return frame

    def _setup_connections(self):
        self.btn_connect.clicked.connect(self._on_connect)
        self.btn_disconnect.clicked.connect(self._on_disconnect)

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
        self.lbl_status.setText("CONNECTED")
        self.lbl_status.setStyleSheet("color: green; font-weight: bold;")
        self.statusbar.showMessage(f"Connected to SITL. Proxy on port {self.config.mavlink.proxy_port}")

    def _on_connection_lost(self, data):
        self.btn_connect.setEnabled(True)
        self.btn_disconnect.setEnabled(False)
        self.btn_hdg_hold.setEnabled(False)
        self.btn_nav.setEnabled(False)
        self.btn_set_pos.setEnabled(False)
        self.btn_load_route.setEnabled(False)
        self.lbl_status.setText("DISCONNECTED")
        self.lbl_status.setStyleSheet("color: red; font-weight: bold;")
        self.statusbar.showMessage("Disconnected")

    def _update_display(self):
        if not self.proxy.is_connected():
            return

        telemetry = self.proxy.get_telemetry()
        self.status_panel.update_telemetry(telemetry)
        self.lbl_mode.setText(f"Mode: {telemetry.mode}")

        if telemetry.position:
            self.map_widget.update_aircraft(
                telemetry.position.lat,
                telemetry.position.lon,
                telemetry.heading
            )

    def closeEvent(self, event):
        self.proxy.stop()
        super().closeEvent(event)
