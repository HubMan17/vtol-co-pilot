from PyQt5.QtWidgets import QWidget, QVBoxLayout, QGridLayout, QLabel, QFrame
from PyQt5.QtGui import QFont
from PyQt5.QtCore import Qt
import math

from src.mavlink.telemetry import TelemetryState


class StatusPanel(QWidget):
    def __init__(self):
        super().__init__()
        self._setup_ui()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(5, 5, 5, 5)

        title = QLabel("TELEMETRY")
        title.setFont(QFont("Arial", 10, QFont.Bold))
        title.setAlignment(Qt.AlignCenter)
        layout.addWidget(title)

        grid = QGridLayout()
        grid.setSpacing(5)

        self.labels = {}
        fields = [
            ("IAS", "m/s", 0, 0),
            ("GS", "m/s", 0, 1),
            ("HDG", "°", 1, 0),
            ("TRK", "°", 1, 1),
            ("ALT", "m", 2, 0),
            ("AGL", "m", 2, 1),
            ("VS", "m/s", 3, 0),
            ("WIND", "", 3, 1),
            ("ROLL", "°", 4, 0),
            ("PITCH", "°", 4, 1),
            ("BAT", "V", 5, 0),
            ("GPS", "", 5, 1),
        ]

        for name, unit, row, col in fields:
            frame = QFrame()
            frame.setFrameStyle(QFrame.StyledPanel)
            frame_layout = QVBoxLayout(frame)
            frame_layout.setContentsMargins(5, 2, 5, 2)
            frame_layout.setSpacing(0)

            name_label = QLabel(name)
            name_label.setFont(QFont("Arial", 8))
            name_label.setStyleSheet("color: gray;")
            frame_layout.addWidget(name_label)

            value_label = QLabel("---")
            value_label.setFont(QFont("Consolas", 14, QFont.Bold))
            value_label.setAlignment(Qt.AlignRight)
            frame_layout.addWidget(value_label)

            self.labels[name] = (value_label, unit)
            grid.addWidget(frame, row, col)

        layout.addLayout(grid)

        nav_frame = QFrame()
        nav_frame.setFrameStyle(QFrame.StyledPanel)
        nav_layout = QGridLayout(nav_frame)

        nav_fields = [
            ("WPT", "", 0, 0),
            ("DIST", "km", 0, 1),
            ("ETA", "", 1, 0),
            ("XTK", "m", 1, 1),
        ]

        for name, unit, row, col in nav_fields:
            name_label = QLabel(f"{name}:")
            name_label.setFont(QFont("Arial", 9))
            nav_layout.addWidget(name_label, row, col * 2)

            value_label = QLabel("---")
            value_label.setFont(QFont("Consolas", 11, QFont.Bold))
            nav_layout.addWidget(value_label, row, col * 2 + 1)

            self.labels[name] = (value_label, unit)

        layout.addWidget(nav_frame)
        layout.addStretch()

    def update_telemetry(self, state: TelemetryState):
        self._set_value("IAS", state.airspeed, 1)
        self._set_value("GS", state.groundspeed, 1)
        self._set_value("HDG", state.heading, 0)
        self._set_value("TRK", state.heading, 0)
        self._set_value("ALT", state.altitude, 0)
        self._set_value("AGL", state.altitude_agl, 0)
        self._set_value("VS", state.climb_rate, 1)

        wind_str = f"{int(state.wind_direction):03d}/{state.wind_speed:.0f}"
        self.labels["WIND"][0].setText(wind_str)

        self._set_value("ROLL", math.degrees(state.roll), 1)
        self._set_value("PITCH", math.degrees(state.pitch), 1)
        self._set_value("BAT", state.battery_voltage, 1)

        gps_str = f"{state.gps_fix}D/{state.satellites}"
        self.labels["GPS"][0].setText(gps_str)

    def _set_value(self, name: str, value: float, decimals: int = 1):
        label, unit = self.labels[name]
        if decimals == 0:
            text = f"{int(value)}"
        else:
            text = f"{value:.{decimals}f}"
        if unit:
            text += f" {unit}"
        label.setText(text)

    def update_navigation(self, waypoint_idx: int, total_waypoints: int,
                          distance: float, eta_seconds: float, xtk: float):
        self.labels["WPT"][0].setText(f"{waypoint_idx}/{total_waypoints}")
        self.labels["DIST"][0].setText(f"{distance/1000:.2f} km")

        minutes = int(eta_seconds // 60)
        seconds = int(eta_seconds % 60)
        self.labels["ETA"][0].setText(f"{minutes:02d}:{seconds:02d}")

        sign = "+" if xtk >= 0 else ""
        self.labels["XTK"][0].setText(f"{sign}{xtk:.0f} m")
