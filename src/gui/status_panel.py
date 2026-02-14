from PyQt5.QtWidgets import (
    QWidget, QVBoxLayout, QGridLayout, QHBoxLayout, QLabel, QFrame,
    QSpinBox, QPushButton, QScrollArea, QSizePolicy
)
from PyQt5.QtGui import QFont
from PyQt5.QtCore import Qt, pyqtSignal
import math

from src.mavlink.telemetry import TelemetryState
from src.gui.theme import Colors, Fonts


def _make_card() -> QFrame:
    """Create a styled card frame."""
    card = QFrame()
    card.setProperty("cssClass", "card")
    card.setStyleSheet(f"""
        QFrame[cssClass="card"] {{
            background-color: {Colors.BG_CARD};
            border: 1px solid {Colors.BORDER};
            border-radius: 12px;
        }}
    """)
    return card


def _section_title(text: str) -> QLabel:
    """Create an uppercase section title label."""
    lbl = QLabel(text)
    lbl.setFont(QFont(Fonts.FAMILY, Fonts.SIZE_SMALL, QFont.Bold))
    lbl.setStyleSheet(f"""
        color: {Colors.TEXT_TERTIARY};
        font-size: {Fonts.SIZE_SMALL}px;
        font-weight: 600;
        letter-spacing: 1px;
        padding: 0px;
        margin: 0px;
    """)
    lbl.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
    return lbl


class _MetricWidget(QWidget):
    """A single telemetry metric: small label + large value."""

    def __init__(self, label: str, unit: str = "", parent=None):
        super().__init__(parent)
        self._unit = unit
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 8, 12, 8)
        layout.setSpacing(2)

        self.name_label = QLabel(label)
        self.name_label.setFont(QFont(Fonts.FAMILY, Fonts.SIZE_SMALL))
        self.name_label.setStyleSheet(f"color: {Colors.TEXT_SECONDARY}; font-size: {Fonts.SIZE_SMALL}px;")
        layout.addWidget(self.name_label)

        self.value_label = QLabel("---")
        self.value_label.setFont(QFont(Fonts.MONO, Fonts.SIZE_METRIC, QFont.Bold))
        self.value_label.setStyleSheet(f"color: {Colors.TEXT_PRIMARY}; font-size: {Fonts.SIZE_METRIC}px; font-weight: 700;")
        self.value_label.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        layout.addWidget(self.value_label)

    def set_value(self, value: float, decimals: int = 1):
        if decimals == 0:
            text = f"{int(value)}"
        else:
            text = f"{value:.{decimals}f}"
        if self._unit:
            text += f" {self._unit}"
        self.value_label.setText(text)

    def set_text(self, text: str):
        self.value_label.setText(text)

    def set_color(self, color: str):
        self.value_label.setStyleSheet(f"color: {color}; font-size: {Fonts.SIZE_METRIC}px; font-weight: 700;")


class StatusPanel(QWidget):
    orbit_radius_changed = pyqtSignal(int)
    target_altitude_changed = pyqtSignal(int)
    target_airspeed_changed = pyqtSignal(int)
    wind_override_requested = pyqtSignal(int, int)

    def __init__(self):
        super().__init__()
        self._manual_alt_override = False
        self._manual_radius_override = False
        self._manual_speed_override = False
        self.labels = {}
        self._setup_ui()

    def _setup_ui(self):
        self.setFixedWidth(340)
        self.setStyleSheet(f"background-color: {Colors.BG_SIDEBAR};")

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        scroll.setStyleSheet(f"""
            QScrollArea {{
                background: transparent;
                border: none;
            }}
            QScrollArea > QWidget > QWidget {{
                background: transparent;
            }}
        """)

        content = QWidget()
        layout = QVBoxLayout(content)
        layout.setContentsMargins(12, 16, 12, 16)
        layout.setSpacing(12)

        # --- App title ---
        title_row = QHBoxLayout()
        app_icon = QLabel("✈")
        app_icon.setFont(QFont(Fonts.FAMILY, 18))
        app_icon.setStyleSheet(f"color: {Colors.PRIMARY};")
        title_row.addWidget(app_icon)

        app_title = QLabel("VTOL Со-Пилот")
        app_title.setFont(QFont(Fonts.FAMILY, 16, QFont.Bold))
        app_title.setStyleSheet(f"color: {Colors.TEXT_PRIMARY}; font-weight: 700;")
        title_row.addWidget(app_title)
        title_row.addStretch()
        layout.addLayout(title_row)

        # Separator
        sep = QFrame()
        sep.setFixedHeight(1)
        sep.setStyleSheet(f"background-color: {Colors.BORDER};")
        layout.addWidget(sep)

        # ===================== TELEMETRY SECTION =====================
        layout.addWidget(_section_title("ТЕЛЕМЕТРИЯ"))

        # Speed KPI row
        speed_card = _make_card()
        speed_layout = QGridLayout(speed_card)
        speed_layout.setContentsMargins(4, 4, 4, 4)
        speed_layout.setSpacing(0)

        self.m_airspeed = _MetricWidget("Приборная", "м/с")
        self.m_groundspeed = _MetricWidget("Путевая", "м/с")
        speed_layout.addWidget(self.m_airspeed, 0, 0)

        # Vertical separator
        vsep1 = QFrame()
        vsep1.setFixedWidth(1)
        vsep1.setStyleSheet(f"background-color: {Colors.BORDER};")
        speed_layout.addWidget(vsep1, 0, 1)

        speed_layout.addWidget(self.m_groundspeed, 0, 2)
        layout.addWidget(speed_card)

        # Heading row
        heading_card = _make_card()
        heading_layout = QGridLayout(heading_card)
        heading_layout.setContentsMargins(4, 4, 4, 4)
        heading_layout.setSpacing(0)

        self.m_heading = _MetricWidget("Курс", "°")
        self.m_track = _MetricWidget("Трек", "°")
        heading_layout.addWidget(self.m_heading, 0, 0)

        vsep2 = QFrame()
        vsep2.setFixedWidth(1)
        vsep2.setStyleSheet(f"background-color: {Colors.BORDER};")
        heading_layout.addWidget(vsep2, 0, 1)

        heading_layout.addWidget(self.m_track, 0, 2)
        layout.addWidget(heading_card)

        # Altitude row
        alt_card = _make_card()
        alt_layout = QGridLayout(alt_card)
        alt_layout.setContentsMargins(4, 4, 4, 4)
        alt_layout.setSpacing(0)

        self.m_altitude = _MetricWidget("Высота MSL", "м")
        self.m_altitude_agl = _MetricWidget("Высота AGL", "м")
        alt_layout.addWidget(self.m_altitude, 0, 0)

        vsep3 = QFrame()
        vsep3.setFixedWidth(1)
        vsep3.setStyleSheet(f"background-color: {Colors.BORDER};")
        alt_layout.addWidget(vsep3, 0, 1)

        alt_layout.addWidget(self.m_altitude_agl, 0, 2)
        layout.addWidget(alt_card)

        # Vario + Attitude + Battery + GPS + Wind
        misc_card = _make_card()
        misc_layout = QGridLayout(misc_card)
        misc_layout.setContentsMargins(4, 4, 4, 4)
        misc_layout.setSpacing(0)

        self.m_climb = _MetricWidget("Верт. скор.", "м/с")
        self.m_wind = _MetricWidget("Ветер")
        misc_layout.addWidget(self.m_climb, 0, 0)

        vsep4 = QFrame()
        vsep4.setFixedWidth(1)
        vsep4.setStyleSheet(f"background-color: {Colors.BORDER};")
        misc_layout.addWidget(vsep4, 0, 1)

        misc_layout.addWidget(self.m_wind, 0, 2)

        self.m_roll = _MetricWidget("Крен", "°")
        self.m_pitch = _MetricWidget("Тангаж", "°")

        vsep5 = QFrame()
        vsep5.setFixedWidth(1)
        vsep5.setStyleSheet(f"background-color: {Colors.BORDER};")

        misc_layout.addWidget(self.m_roll, 1, 0)
        misc_layout.addWidget(vsep5, 1, 1)
        misc_layout.addWidget(self.m_pitch, 1, 2)

        self.m_battery = _MetricWidget("Батарея", "В")
        self.m_gps = _MetricWidget("GPS")

        vsep6 = QFrame()
        vsep6.setFixedWidth(1)
        vsep6.setStyleSheet(f"background-color: {Colors.BORDER};")

        misc_layout.addWidget(self.m_battery, 2, 0)
        misc_layout.addWidget(vsep6, 2, 1)
        misc_layout.addWidget(self.m_gps, 2, 2)

        layout.addWidget(misc_card)

        # Wind override controls
        wind_ctrl = _make_card()
        wind_layout = QHBoxLayout(wind_ctrl)
        wind_layout.setContentsMargins(12, 8, 12, 8)
        wind_layout.setSpacing(8)

        wind_lbl = QLabel("Ветер:")
        wind_lbl.setStyleSheet(f"color: {Colors.TEXT_SECONDARY}; font-size: {Fonts.SIZE_SMALL}px;")
        wind_layout.addWidget(wind_lbl)

        self.spin_wind_dir = QSpinBox()
        self.spin_wind_dir.setRange(0, 360)
        self.spin_wind_dir.setWrapping(True)
        self.spin_wind_dir.setSuffix("°")
        self.spin_wind_dir.setValue(0)
        self.spin_wind_dir.setFixedWidth(68)
        wind_layout.addWidget(self.spin_wind_dir)

        self.spin_wind_speed = QSpinBox()
        self.spin_wind_speed.setRange(0, 30)
        self.spin_wind_speed.setSuffix(" м/с")
        self.spin_wind_speed.setValue(0)
        self.spin_wind_speed.setFixedWidth(78)
        wind_layout.addWidget(self.spin_wind_speed)

        self.btn_set_wind = QPushButton("Задать")
        self.btn_set_wind.setProperty("cssClass", "primary")
        self.btn_set_wind.setFixedHeight(32)
        self.btn_set_wind.clicked.connect(self._on_set_wind)
        wind_layout.addWidget(self.btn_set_wind)

        layout.addWidget(wind_ctrl)

        # ===================== NAVIGATION SECTION =====================
        sep2 = QFrame()
        sep2.setFixedHeight(1)
        sep2.setStyleSheet(f"background-color: {Colors.BORDER};")
        layout.addWidget(sep2)

        layout.addWidget(_section_title("НАВИГАЦИЯ"))

        nav_card = _make_card()
        nav_layout = QGridLayout(nav_card)
        nav_layout.setContentsMargins(4, 4, 4, 4)
        nav_layout.setSpacing(0)

        self.m_waypoint = _MetricWidget("Точка")
        self.m_distance = _MetricWidget("Дистанция")
        self.m_eta = _MetricWidget("Время приб.")
        self.m_xtk = _MetricWidget("Бок. уклон.")

        nav_layout.addWidget(self.m_waypoint, 0, 0)
        vsep_n1 = QFrame()
        vsep_n1.setFixedWidth(1)
        vsep_n1.setStyleSheet(f"background-color: {Colors.BORDER};")
        nav_layout.addWidget(vsep_n1, 0, 1)
        nav_layout.addWidget(self.m_distance, 0, 2)

        nav_layout.addWidget(self.m_eta, 1, 0)
        vsep_n2 = QFrame()
        vsep_n2.setFixedWidth(1)
        vsep_n2.setStyleSheet(f"background-color: {Colors.BORDER};")
        nav_layout.addWidget(vsep_n2, 1, 1)
        nav_layout.addWidget(self.m_xtk, 1, 2)

        layout.addWidget(nav_card)

        # Compatibility mapping for MainWindow access
        self.labels = {
            "Точка": (self.m_waypoint.value_label, ""),
            "Дистанция": (self.m_distance.value_label, ""),
            "Время приб.": (self.m_eta.value_label, ""),
            "Бок. уклон.": (self.m_xtk.value_label, ""),
        }

        # ===================== AUTOPILOT SECTION =====================
        sep3 = QFrame()
        sep3.setFixedHeight(1)
        sep3.setStyleSheet(f"background-color: {Colors.BORDER};")
        layout.addWidget(sep3)

        layout.addWidget(_section_title("АВТОПИЛОТ"))

        ap_card = _make_card()
        ap_layout = QVBoxLayout(ap_card)
        ap_layout.setContentsMargins(12, 12, 12, 12)
        ap_layout.setSpacing(8)

        # Mode badge row
        mode_row = QHBoxLayout()

        mode_lbl = QLabel("Режим")
        mode_lbl.setStyleSheet(f"color: {Colors.TEXT_SECONDARY}; font-size: {Fonts.SIZE_SMALL}px;")
        mode_row.addWidget(mode_lbl)

        self.lbl_ap_mode = QLabel("РУЧНОЙ")
        self.lbl_ap_mode.setFont(QFont(Fonts.MONO, 14, QFont.Bold))
        self.lbl_ap_mode.setStyleSheet(f"""
            color: {Colors.TEXT_TERTIARY};
            font-size: 14px;
            font-weight: 700;
            padding: 2px 10px;
            border-radius: 6px;
            background-color: {Colors.BG_INPUT};
        """)
        self.lbl_ap_mode.setAlignment(Qt.AlignCenter)
        mode_row.addStretch()
        mode_row.addWidget(self.lbl_ap_mode)
        mode_row.addStretch()
        ap_layout.addLayout(mode_row)

        # Action
        self.lbl_ap_action = QLabel("---")
        self.lbl_ap_action.setFont(QFont(Fonts.MONO, 12, QFont.Bold))
        self.lbl_ap_action.setStyleSheet(f"color: {Colors.TEXT_TERTIARY}; font-size: 12px;")
        self.lbl_ap_action.setAlignment(Qt.AlignCenter)
        ap_layout.addWidget(self.lbl_ap_action)

        # Autopilot data grid
        ap_data = QGridLayout()
        ap_data.setSpacing(4)

        # Target heading
        th_lbl = QLabel("Цел. курс")
        th_lbl.setStyleSheet(f"color: {Colors.TEXT_SECONDARY}; font-size: {Fonts.SIZE_SMALL}px;")
        ap_data.addWidget(th_lbl, 0, 0)

        self.lbl_ap_target = QLabel("---")
        self.lbl_ap_target.setFont(QFont(Fonts.MONO, 14, QFont.Bold))
        self.lbl_ap_target.setStyleSheet(f"color: {Colors.TEXT_PRIMARY}; font-size: 14px; font-weight: 700;")
        self.lbl_ap_target.setAlignment(Qt.AlignRight)
        ap_data.addWidget(self.lbl_ap_target, 0, 1)

        # Heading error
        he_lbl = QLabel("Ошибка курса")
        he_lbl.setStyleSheet(f"color: {Colors.TEXT_SECONDARY}; font-size: {Fonts.SIZE_SMALL}px;")
        ap_data.addWidget(he_lbl, 1, 0)

        self.lbl_ap_error = QLabel("---")
        self.lbl_ap_error.setFont(QFont(Fonts.MONO, 14, QFont.Bold))
        self.lbl_ap_error.setStyleSheet(f"color: {Colors.TEXT_PRIMARY}; font-size: 14px; font-weight: 700;")
        self.lbl_ap_error.setAlignment(Qt.AlignRight)
        ap_data.addWidget(self.lbl_ap_error, 1, 1)

        # Altitude error
        ae_lbl = QLabel("Ошибка высоты")
        ae_lbl.setStyleSheet(f"color: {Colors.TEXT_SECONDARY}; font-size: {Fonts.SIZE_SMALL}px;")
        ap_data.addWidget(ae_lbl, 2, 0)

        self.lbl_ap_alt_error = QLabel("---")
        self.lbl_ap_alt_error.setFont(QFont(Fonts.MONO, 14, QFont.Bold))
        self.lbl_ap_alt_error.setStyleSheet(f"color: {Colors.TEXT_PRIMARY}; font-size: 14px; font-weight: 700;")
        self.lbl_ap_alt_error.setAlignment(Qt.AlignRight)
        ap_data.addWidget(self.lbl_ap_alt_error, 2, 1)

        ap_layout.addLayout(ap_data)
        layout.addWidget(ap_card)

        # Autopilot controls
        ctrl_card = _make_card()
        ctrl_layout = QGridLayout(ctrl_card)
        ctrl_layout.setContentsMargins(12, 12, 12, 12)
        ctrl_layout.setSpacing(8)

        # Target altitude
        alt_lbl = QLabel("Цел. высота")
        alt_lbl.setStyleSheet(f"color: {Colors.TEXT_SECONDARY}; font-size: {Fonts.SIZE_SMALL}px;")
        ctrl_layout.addWidget(alt_lbl, 0, 0)

        self.spin_target_alt = QSpinBox()
        self.spin_target_alt.setRange(10, 5000)
        self.spin_target_alt.setValue(100)
        self.spin_target_alt.setSuffix(" м")
        self.spin_target_alt.setEnabled(False)
        ctrl_layout.addWidget(self.spin_target_alt, 0, 1)

        self.btn_set_altitude = QPushButton("✓")
        self.btn_set_altitude.setProperty("cssClass", "icon")
        self.btn_set_altitude.setEnabled(False)
        self.btn_set_altitude.clicked.connect(self._on_set_altitude)
        ctrl_layout.addWidget(self.btn_set_altitude, 0, 2)

        # Orbit radius
        rad_lbl = QLabel("Радиус круж.")
        rad_lbl.setStyleSheet(f"color: {Colors.TEXT_SECONDARY}; font-size: {Fonts.SIZE_SMALL}px;")
        ctrl_layout.addWidget(rad_lbl, 1, 0)

        self.spin_orbit_radius = QSpinBox()
        self.spin_orbit_radius.setRange(30, 500)
        self.spin_orbit_radius.setValue(150)
        self.spin_orbit_radius.setSuffix(" м")
        self.spin_orbit_radius.setEnabled(False)
        ctrl_layout.addWidget(self.spin_orbit_radius, 1, 1)

        self.btn_set_radius = QPushButton("✓")
        self.btn_set_radius.setProperty("cssClass", "icon")
        self.btn_set_radius.setEnabled(False)
        self.btn_set_radius.clicked.connect(self._on_set_radius)
        ctrl_layout.addWidget(self.btn_set_radius, 1, 2)

        # Target speed
        spd_lbl = QLabel("Цел. скорость")
        spd_lbl.setStyleSheet(f"color: {Colors.TEXT_SECONDARY}; font-size: {Fonts.SIZE_SMALL}px;")
        ctrl_layout.addWidget(spd_lbl, 2, 0)

        self.spin_target_speed = QSpinBox()
        self.spin_target_speed.setRange(15, 35)
        self.spin_target_speed.setValue(20)
        self.spin_target_speed.setSuffix(" м/с")
        self.spin_target_speed.setEnabled(False)
        ctrl_layout.addWidget(self.spin_target_speed, 2, 1)

        self.btn_set_speed = QPushButton("✓")
        self.btn_set_speed.setProperty("cssClass", "icon")
        self.btn_set_speed.setEnabled(False)
        self.btn_set_speed.clicked.connect(self._on_set_speed)
        ctrl_layout.addWidget(self.btn_set_speed, 2, 2)

        layout.addWidget(ctrl_card)

        layout.addStretch()

        scroll.setWidget(content)
        outer.addWidget(scroll)

    # ─── Telemetry updates ───

    def update_telemetry(self, state: TelemetryState):
        self.m_airspeed.set_value(state.airspeed, 1)
        self.m_groundspeed.set_value(state.groundspeed, 1)
        self.m_heading.set_value(state.heading, 0)
        self.m_track.set_value(state.heading, 0)
        self.m_altitude.set_value(state.altitude, 0)
        self.m_altitude_agl.set_value(state.altitude_agl, 0)
        self.m_climb.set_value(state.climb_rate, 1)

        wind_str = f"{int(state.wind_direction):03d}° / {state.wind_speed:.0f} м/с"
        self.m_wind.set_text(wind_str)

        self.m_roll.set_value(math.degrees(state.roll), 1)
        self.m_pitch.set_value(math.degrees(state.pitch), 1)
        self.m_battery.set_value(state.battery_voltage, 1)

        if state.gps_fix >= 3:
            self.m_gps.set_text(f"3D ({state.satellites})")
            self.m_gps.set_color(Colors.SUCCESS)
        else:
            self.m_gps.set_text(f"NO FIX ({state.satellites})")
            self.m_gps.set_color(Colors.ERROR)

    # ─── Navigation updates ───

    def update_navigation(self, waypoint_idx: int, total_waypoints: int,
                          distance: float, eta_seconds: float, xtk: float):
        self.m_waypoint.set_text(f"{waypoint_idx} / {total_waypoints}")
        self.m_distance.set_text(f"{distance / 1000:.2f} км")

        minutes = int(eta_seconds // 60)
        seconds = int(eta_seconds % 60)
        self.m_eta.set_text(f"{minutes:02d}:{seconds:02d}")

        sign = "+" if xtk >= 0 else ""
        self.m_xtk.set_text(f"{sign}{xtk:.0f} м")

    # ─── Autopilot controls ───

    def _on_set_altitude(self):
        self._manual_alt_override = True
        self.target_altitude_changed.emit(self.spin_target_alt.value())
        self.btn_set_altitude.setStyleSheet(f"""
            background-color: {Colors.SUCCESS};
            color: #fff;
            border: none;
            border-radius: 8px;
            padding: 6px;
            min-width: 32px; max-width: 32px;
            min-height: 32px; max-height: 32px;
        """)

    def _on_set_radius(self):
        self._manual_radius_override = True
        self.orbit_radius_changed.emit(self.spin_orbit_radius.value())
        self.btn_set_radius.setStyleSheet(f"""
            background-color: {Colors.SUCCESS};
            color: #fff;
            border: none;
            border-radius: 8px;
            padding: 6px;
            min-width: 32px; max-width: 32px;
            min-height: 32px; max-height: 32px;
        """)

    def _on_set_speed(self):
        self._manual_speed_override = True
        self.target_airspeed_changed.emit(self.spin_target_speed.value())
        self.btn_set_speed.setStyleSheet(f"""
            background-color: {Colors.SUCCESS};
            color: #fff;
            border: none;
            border-radius: 8px;
            padding: 6px;
            min-width: 32px; max-width: 32px;
            min-height: 32px; max-height: 32px;
        """)

    def _on_set_wind(self):
        self.wind_override_requested.emit(
            self.spin_wind_dir.value(),
            self.spin_wind_speed.value()
        )
        self.btn_set_wind.setStyleSheet(f"""
            background-color: {Colors.SUCCESS};
            color: #fff;
            border: none;
            border-radius: 8px;
            padding: 8px 16px;
            font-weight: 600;
        """)

    # ─── Autopilot display ───

    def update_autopilot(self, mode: str, status: dict = None):
        mode_names = {
            'MANUAL': 'РУЧНОЙ',
            'NAV': 'НАВИГАЦИЯ'
        }
        display_mode = mode_names.get(mode, mode)
        self.lbl_ap_mode.setText(display_mode)

        if mode == 'MANUAL':
            self.lbl_ap_mode.setStyleSheet(f"""
                color: {Colors.TEXT_TERTIARY};
                font-size: 14px; font-weight: 700;
                padding: 2px 10px; border-radius: 6px;
                background-color: {Colors.BG_INPUT};
            """)

            if status and status.get('disengage_reason'):
                self.lbl_ap_action.setText(f"Отключен: {status['disengage_reason']}")
                self.lbl_ap_action.setStyleSheet(f"color: {Colors.WARNING}; font-size: 12px;")
            else:
                self.lbl_ap_action.setText("---")
                self.lbl_ap_action.setStyleSheet(f"color: {Colors.TEXT_TERTIARY}; font-size: 12px;")

            self.lbl_ap_target.setText("---")
            self.lbl_ap_error.setText("---")
            self.lbl_ap_alt_error.setText("---")
            self.spin_target_alt.setEnabled(False)
            self.spin_orbit_radius.setEnabled(False)
            self.spin_target_speed.setEnabled(False)
            self.btn_set_altitude.setEnabled(False)
            self.btn_set_radius.setEnabled(False)
            self.btn_set_speed.setEnabled(False)

            self._manual_alt_override = False
            self._manual_radius_override = False
            self._manual_speed_override = False
            self.btn_set_altitude.setStyleSheet("")
            self.btn_set_radius.setStyleSheet("")
            self.btn_set_speed.setStyleSheet("")
        else:
            self.lbl_ap_mode.setStyleSheet(f"""
                color: #ffffff;
                font-size: 14px; font-weight: 700;
                padding: 2px 10px; border-radius: 6px;
                background-color: {Colors.SUCCESS};
            """)

            if status:
                action = status.get('action', 'IDLE')

                if action.startswith('ORBIT_') and '/' in action:
                    parts = action.split('(')
                    orbit_part = parts[0].strip()
                    vertical = f" ({parts[1]}" if len(parts) > 1 else ""
                    action_text = f"Круг {orbit_part.split('_')[1]}{vertical}"
                elif action.startswith('ORBIT_INF'):
                    parts = action.split('(')
                    vertical = f" ({parts[1]}" if len(parts) > 1 else ""
                    action_text = f"Кружение ∞{vertical}"
                elif action.startswith('ALTITUDE_ORBIT'):
                    parts = action.split('(')
                    vertical = f" ({parts[1]}" if len(parts) > 1 else ""
                    action_text = f"Набор высоты{vertical}"
                elif action.startswith('TO_WAYPOINT'):
                    parts = action.split('(')
                    vertical = f" ({parts[1]}" if len(parts) > 1 else ""
                    action_text = f"К точке{vertical}"
                elif action.startswith('ОЖИДАНИЕ_ВЫСОТЫ'):
                    parts = action.split('(')
                    vertical = f" ({parts[1]}" if len(parts) > 1 else ""
                    action_text = f"Ожидание высоты{vertical}"
                elif action == 'ORBITING':
                    action_text = "Кружение"
                elif action == 'IDLE':
                    action_text = "---"
                else:
                    action_text = action

                self.lbl_ap_action.setText(action_text)
                if status.get('is_orbiting', False):
                    self.lbl_ap_action.setStyleSheet(f"color: {Colors.WARNING}; font-size: 12px; font-weight: 700;")
                else:
                    self.lbl_ap_action.setStyleSheet(f"color: {Colors.PRIMARY_LIGHT}; font-size: 12px;")

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
                self.btn_set_altitude.setEnabled(True)
                self.btn_set_radius.setEnabled(True)
                self.btn_set_speed.setEnabled(True)

                if not self._manual_alt_override:
                    target_alt = status.get('target_altitude', 100)
                    if target_alt > 0:
                        self.spin_target_alt.blockSignals(True)
                        self.spin_target_alt.setValue(int(target_alt))
                        self.spin_target_alt.blockSignals(False)

                if not self._manual_radius_override:
                    orbit_radius = status.get('orbit_radius', 150)
                    if orbit_radius > 0:
                        self.spin_orbit_radius.blockSignals(True)
                        self.spin_orbit_radius.setValue(int(orbit_radius))
                        self.spin_orbit_radius.blockSignals(False)

                if not self._manual_speed_override:
                    target_speed = status.get('target_airspeed', 20)
                    if target_speed > 0:
                        self.spin_target_speed.blockSignals(True)
                        self.spin_target_speed.setValue(int(target_speed))
                        self.spin_target_speed.blockSignals(False)
