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

        title = QLabel("ТЕЛЕМЕТРИЯ")
        title.setFont(QFont("Arial", 10, QFont.Bold))
        title.setAlignment(Qt.AlignCenter)
        layout.addWidget(title)

        grid = QGridLayout()
        grid.setSpacing(5)

        self.labels = {}
        fields = [
            ("ПРВ", "м/с", 0, 0),
            ("ПУТ", "м/с", 0, 1),
            ("КУРС", "°", 1, 0),
            ("ТРЕК", "°", 1, 1),
            ("ВЫС", "м", 2, 0),
            ("AGL", "м", 2, 1),
            ("ВЕРТ", "м/с", 3, 0),
            ("ВЕТЕР", "", 3, 1),
            ("КРЕН", "°", 4, 0),
            ("ТАНГАЖ", "°", 4, 1),
            ("АКБ", "В", 5, 0),
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
            ("ТЧК", "", 0, 0),
            ("ДИСТ", "км", 0, 1),
            ("ВПР", "", 1, 0),
            ("БУО", "м", 1, 1),
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
        self._set_value("ПРВ", state.airspeed, 1)
        self._set_value("ПУТ", state.groundspeed, 1)
        self._set_value("КУРС", state.heading, 0)
        self._set_value("ТРЕК", state.heading, 0)
        self._set_value("ВЫС", state.altitude, 0)
        self._set_value("AGL", state.altitude_agl, 0)
        self._set_value("ВЕРТ", state.climb_rate, 1)

        wind_str = f"{int(state.wind_direction):03d}/{state.wind_speed:.0f}"
        self.labels["ВЕТЕР"][0].setText(wind_str)

        self._set_value("КРЕН", math.degrees(state.roll), 1)
        self._set_value("ТАНГАЖ", math.degrees(state.pitch), 1)
        self._set_value("АКБ", state.battery_voltage, 1)

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
        self.labels["ТЧК"][0].setText(f"{waypoint_idx}/{total_waypoints}")
        self.labels["ДИСТ"][0].setText(f"{distance/1000:.2f} км")

        minutes = int(eta_seconds // 60)
        seconds = int(eta_seconds % 60)
        self.labels["ВПР"][0].setText(f"{minutes:02d}:{seconds:02d}")

        sign = "+" if xtk >= 0 else ""
        self.labels["БУО"][0].setText(f"{sign}{xtk:.0f} м")
