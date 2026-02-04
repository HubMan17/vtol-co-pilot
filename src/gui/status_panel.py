from PyQt5.QtWidgets import QWidget, QVBoxLayout, QGridLayout, QHBoxLayout, QLabel, QFrame, QSpinBox, QPushButton
from PyQt5.QtGui import QFont
from PyQt5.QtCore import Qt, pyqtSignal
import math

from src.mavlink.telemetry import TelemetryState


class StatusPanel(QWidget):
    orbit_radius_changed = pyqtSignal(int)
    target_altitude_changed = pyqtSignal(int)
    target_airspeed_changed = pyqtSignal(int)

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
            ("Приборная", "м/с", 0, 0),
            ("Путевая", "м/с", 0, 1),
            ("Курс", "°", 1, 0),
            ("Трек", "°", 1, 1),
            ("Высота", "м", 2, 0),
            ("Высота AGL", "м", 2, 1),
            ("Верт. скор.", "м/с", 3, 0),
            ("Ветер", "", 3, 1),
            ("Крен", "°", 4, 0),
            ("Тангаж", "°", 4, 1),
            ("Батарея", "В", 5, 0),
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

        nav_title = QLabel("НАВИГАЦИЯ")
        nav_title.setFont(QFont("Arial", 10, QFont.Bold))
        nav_title.setAlignment(Qt.AlignCenter)
        layout.addWidget(nav_title)

        nav_frame = QFrame()
        nav_frame.setFrameStyle(QFrame.StyledPanel)
        nav_layout = QGridLayout(nav_frame)

        nav_fields = [
            ("Точка", "", 0, 0),
            ("Дистанция", "", 0, 1),
            ("Время приб.", "", 1, 0),
            ("Бок. уклон.", "", 1, 1),
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

        ap_title = QLabel("АВТОПИЛОТ")
        ap_title.setFont(QFont("Arial", 10, QFont.Bold))
        ap_title.setAlignment(Qt.AlignCenter)
        layout.addWidget(ap_title)

        ap_frame = QFrame()
        ap_frame.setFrameStyle(QFrame.StyledPanel)
        ap_layout = QGridLayout(ap_frame)

        ap_layout.addWidget(QLabel("Режим:"), 0, 0)
        self.lbl_ap_mode = QLabel("РУЧНОЙ")
        self.lbl_ap_mode.setFont(QFont("Consolas", 12, QFont.Bold))
        self.lbl_ap_mode.setStyleSheet("color: gray;")
        ap_layout.addWidget(self.lbl_ap_mode, 0, 1)

        ap_layout.addWidget(QLabel("Действие:"), 1, 0)
        self.lbl_ap_action = QLabel("---")
        self.lbl_ap_action.setFont(QFont("Consolas", 11, QFont.Bold))
        self.lbl_ap_action.setStyleSheet("color: #888;")
        ap_layout.addWidget(self.lbl_ap_action, 1, 1)

        ap_layout.addWidget(QLabel("Целевой курс:"), 2, 0)
        self.lbl_ap_target = QLabel("---")
        self.lbl_ap_target.setFont(QFont("Consolas", 12, QFont.Bold))
        ap_layout.addWidget(self.lbl_ap_target, 2, 1)

        ap_layout.addWidget(QLabel("Ошибка курса:"), 3, 0)
        self.lbl_ap_error = QLabel("---")
        self.lbl_ap_error.setFont(QFont("Consolas", 12, QFont.Bold))
        ap_layout.addWidget(self.lbl_ap_error, 3, 1)

        ap_layout.addWidget(QLabel("Ошибка высоты:"), 4, 0)
        self.lbl_ap_alt_error = QLabel("---")
        self.lbl_ap_alt_error.setFont(QFont("Consolas", 12, QFont.Bold))
        ap_layout.addWidget(self.lbl_ap_alt_error, 4, 1)

        layout.addWidget(ap_frame)

        ap_ctrl_frame = QFrame()
        ap_ctrl_frame.setFrameStyle(QFrame.StyledPanel)
        ap_ctrl_layout = QGridLayout(ap_ctrl_frame)

        ap_ctrl_layout.addWidget(QLabel("Цел. высота:"), 0, 0)
        self.spin_target_alt = QSpinBox()
        self.spin_target_alt.setRange(10, 5000)
        self.spin_target_alt.setValue(100)
        self.spin_target_alt.setSuffix(" м")
        self.spin_target_alt.setEnabled(False)
        ap_ctrl_layout.addWidget(self.spin_target_alt, 0, 1)

        ap_ctrl_layout.addWidget(QLabel("Радиус круж.:"), 1, 0)
        self.spin_orbit_radius = QSpinBox()
        self.spin_orbit_radius.setRange(30, 500)
        self.spin_orbit_radius.setValue(100)
        self.spin_orbit_radius.setSuffix(" м")
        self.spin_orbit_radius.setEnabled(False)
        ap_ctrl_layout.addWidget(self.spin_orbit_radius, 1, 1)

        ap_ctrl_layout.addWidget(QLabel("Цел. скорость:"), 2, 0)
        self.spin_target_speed = QSpinBox()
        self.spin_target_speed.setRange(15, 35)
        self.spin_target_speed.setValue(20)
        self.spin_target_speed.setSuffix(" м/с")
        self.spin_target_speed.setEnabled(False)
        ap_ctrl_layout.addWidget(self.spin_target_speed, 2, 1)

        self.btn_apply_params = QPushButton("Установить")
        self.btn_apply_params.setEnabled(False)
        self.btn_apply_params.clicked.connect(self._on_apply_params)
        ap_ctrl_layout.addWidget(self.btn_apply_params, 3, 0, 1, 2)

        layout.addWidget(ap_ctrl_frame)
        layout.addStretch()

    def update_telemetry(self, state: TelemetryState):
        self._set_value("Приборная", state.airspeed, 1)
        self._set_value("Путевая", state.groundspeed, 1)
        self._set_value("Курс", state.heading, 0)
        self._set_value("Трек", state.heading, 0)
        self._set_value("Высота", state.altitude, 0)
        self._set_value("Высота AGL", state.altitude_agl, 0)
        self._set_value("Верт. скор.", state.climb_rate, 1)

        wind_str = f"{int(state.wind_direction):03d}° / {state.wind_speed:.0f} м/с"
        self.labels["Ветер"][0].setText(wind_str)

        self._set_value("Крен", math.degrees(state.roll), 1)
        self._set_value("Тангаж", math.degrees(state.pitch), 1)
        self._set_value("Батарея", state.battery_voltage, 1)

        gps_label = self.labels["GPS"][0]
        if state.gps_fix >= 3:
            gps_label.setText(f"OK ({state.satellites})")
            gps_label.setStyleSheet("color: #00ff00; font-weight: bold;")
        else:
            gps_label.setText(f"OFF ({state.satellites})")
            gps_label.setStyleSheet("color: #ff3333; font-weight: bold;")

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
        self.labels["Точка"][0].setText(f"{waypoint_idx} / {total_waypoints}")
        self.labels["Дистанция"][0].setText(f"{distance/1000:.2f} км")

        minutes = int(eta_seconds // 60)
        seconds = int(eta_seconds % 60)
        self.labels["Время приб."][0].setText(f"{minutes:02d}:{seconds:02d}")

        sign = "+" if xtk >= 0 else ""
        self.labels["Бок. уклон."][0].setText(f"{sign}{xtk:.0f} м")

    def _on_apply_params(self):
        self.orbit_radius_changed.emit(self.spin_orbit_radius.value())
        self.target_altitude_changed.emit(self.spin_target_alt.value())
        self.target_airspeed_changed.emit(self.spin_target_speed.value())

    def update_autopilot(self, mode: str, status: dict = None):
        mode_names = {
            'MANUAL': 'РУЧНОЙ',
            'NAV': 'НАВИГАЦИЯ'
        }
        display_mode = mode_names.get(mode, mode)
        self.lbl_ap_mode.setText(display_mode)

        if mode == 'MANUAL':
            self.lbl_ap_mode.setStyleSheet("color: gray;")
            self.lbl_ap_action.setText("---")
            self.lbl_ap_action.setStyleSheet("color: #888;")
            self.lbl_ap_target.setText("---")
            self.lbl_ap_error.setText("---")
            self.lbl_ap_alt_error.setText("---")
            self.spin_target_alt.setEnabled(False)
            self.spin_orbit_radius.setEnabled(False)
            self.spin_target_speed.setEnabled(False)
            self.btn_apply_params.setEnabled(False)
        else:
            self.lbl_ap_mode.setStyleSheet("color: #00ff00; font-weight: bold;")

            if status:
                action = status.get('action', 'IDLE')

                # Parse action and vertical movement
                if action.startswith('ORBIT_') and '/' in action:
                    # Format: ORBIT_1/3 (набор) or ORBIT_1/3
                    parts = action.split('(')
                    orbit_part = parts[0].strip()
                    vertical = f" ({parts[1]}" if len(parts) > 1 else ""
                    action_text = f"Круг {orbit_part.split('_')[1]}{vertical}"
                elif action.startswith('ORBIT_INF'):
                    parts = action.split('(')
                    vertical = f" ({parts[1]}" if len(parts) > 1 else ""
                    action_text = f"Кружение ∞{vertical}"
                elif action.startswith('TO_WAYPOINT'):
                    parts = action.split('(')
                    vertical = f" ({parts[1]}" if len(parts) > 1 else ""
                    action_text = f"К точке{vertical}"
                elif action == 'ORBITING':
                    action_text = "Кружение"
                elif action == 'IDLE':
                    action_text = "---"
                else:
                    action_text = action

                self.lbl_ap_action.setText(action_text)
                if status.get('is_orbiting', False):
                    self.lbl_ap_action.setStyleSheet("color: #ff6600; font-weight: bold;")
                else:
                    self.lbl_ap_action.setStyleSheet("color: #00ccff;")

                target_heading = status.get('target_heading')
                if target_heading is not None:
                    self.lbl_ap_target.setText(f"{target_heading:.0f}°")

                error = status.get('heading_error')
                if error is not None:
                    sign = "+" if error >= 0 else ""
                    self.lbl_ap_error.setText(f"{sign}{error:.1f}°")

                alt_error = status.get('altitude_error')
                if alt_error is not None:
                    sign = "+" if alt_error >= 0 else ""
                    self.lbl_ap_alt_error.setText(f"{sign}{alt_error:.0f} м")

                self.spin_target_alt.setEnabled(True)
                self.spin_orbit_radius.setEnabled(True)
                self.spin_target_speed.setEnabled(True)
                self.btn_apply_params.setEnabled(True)

                target_alt = status.get('target_altitude', 100)
                if target_alt > 0:
                    self.spin_target_alt.blockSignals(True)
                    self.spin_target_alt.setValue(int(target_alt))
                    self.spin_target_alt.blockSignals(False)

                orbit_radius = status.get('orbit_radius', 100)
                if orbit_radius > 0:
                    self.spin_orbit_radius.blockSignals(True)
                    self.spin_orbit_radius.setValue(int(orbit_radius))
                    self.spin_orbit_radius.blockSignals(False)

                target_speed = status.get('target_airspeed', 20)
                if target_speed > 0:
                    self.spin_target_speed.blockSignals(True)
                    self.spin_target_speed.setValue(int(target_speed))
                    self.spin_target_speed.blockSignals(False)
