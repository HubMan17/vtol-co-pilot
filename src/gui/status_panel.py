import math

import qtawesome as qta
from PyQt5.QtWidgets import (
    QWidget, QVBoxLayout, QGridLayout, QHBoxLayout, QLabel, QFrame,
    QSpinBox, QPushButton, QScrollArea, QSizePolicy
)
from PyQt5.QtGui import QFont, QColor
from PyQt5.QtCore import Qt, pyqtSignal

from src.mavlink.telemetry import TelemetryState
from src.gui.theme import Colors, Fonts


# ─── Helper widgets ───

class _Separator(QFrame):
    def __init__(self):
        super().__init__()
        self.setFixedHeight(1)
        self.setStyleSheet(f"background-color: {Colors.BORDER};")


class _SectionHeader(QWidget):
    """Section label: icon + text, dim color, uppercase."""

    def __init__(self, icon_name: str, text: str):
        super().__init__()
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 6, 0, 2)
        layout.setSpacing(6)

        icon = QLabel()
        icon.setPixmap(qta.icon(icon_name, color=Colors.TEXT_DIM).pixmap(14, 14))
        layout.addWidget(icon)

        label = QLabel(text.upper())
        label.setStyleSheet(f"""
            color: {Colors.TEXT_DIM};
            font-size: 10px;
            font-weight: 700;
            letter-spacing: 1.2px;
        """)
        layout.addWidget(label)
        layout.addStretch()


class _MetricCell(QWidget):
    """Compact metric: small label on top, bold value below."""

    def __init__(self, label: str, unit: str = ""):
        super().__init__()
        self._unit = unit

        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 6, 10, 6)
        layout.setSpacing(1)

        self.name_label = QLabel(label)
        self.name_label.setStyleSheet(f"color: {Colors.TEXT_TERTIARY}; font-size: 10px;")
        layout.addWidget(self.name_label)

        self.value_label = QLabel("---")
        self.value_label.setStyleSheet(f"""
            color: {Colors.TEXT_PRIMARY};
            font-family: "{Fonts.MONO}";
            font-size: 16px;
            font-weight: 700;
        """)
        layout.addWidget(self.value_label)

    def set_value(self, value: float, decimals: int = 1):
        text = f"{int(value)}" if decimals == 0 else f"{value:.{decimals}f}"
        if self._unit:
            text += f" {self._unit}"
        self.value_label.setText(text)

    def set_text(self, text: str):
        self.value_label.setText(text)

    def set_color(self, color: str):
        self.value_label.setStyleSheet(f"""
            color: {color};
            font-family: "{Fonts.MONO}";
            font-size: 16px;
            font-weight: 700;
        """)


def _metric_row(*cells) -> QFrame:
    """Create a card-style row containing multiple metric cells with dividers."""
    card = QFrame()
    card.setStyleSheet(f"""
        QFrame {{
            background-color: {Colors.BG_CARD};
            border: 1px solid {Colors.BORDER};
            border-radius: 8px;
        }}
    """)
    grid = QGridLayout(card)
    grid.setContentsMargins(0, 0, 0, 0)
    grid.setSpacing(0)

    for i, cell in enumerate(cells):
        if i > 0:
            sep = QFrame()
            sep.setFixedWidth(1)
            sep.setStyleSheet(f"background-color: {Colors.BORDER};")
            grid.addWidget(sep, 0, i * 2 - 1)
        grid.addWidget(cell, 0, i * 2)

    return card


# ─── Main panel ───

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
        self.setFixedWidth(310)
        self.setStyleSheet(f"background-color: {Colors.BG_SIDEBAR};")

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        scroll.setStyleSheet("QScrollArea { background: transparent; border: none; }")

        content = QWidget()
        content.setStyleSheet("background: transparent;")
        lay = QVBoxLayout(content)
        lay.setContentsMargins(10, 10, 10, 10)
        lay.setSpacing(6)

        # ── TELEMETRY ──
        lay.addWidget(_SectionHeader("mdi.antenna", "Телеметрия"))

        self.m_airspeed = _MetricCell("IAS", "м/с")
        self.m_groundspeed = _MetricCell("GS", "м/с")
        lay.addWidget(_metric_row(self.m_airspeed, self.m_groundspeed))

        self.m_heading = _MetricCell("HDG", "°")
        self.m_track = _MetricCell("TRK", "°")
        lay.addWidget(_metric_row(self.m_heading, self.m_track))

        self.m_altitude = _MetricCell("ALT MSL", "м")
        self.m_altitude_agl = _MetricCell("ALT AGL", "м")
        lay.addWidget(_metric_row(self.m_altitude, self.m_altitude_agl))

        self.m_climb = _MetricCell("V/S", "м/с")
        self.m_wind = _MetricCell("WIND")
        lay.addWidget(_metric_row(self.m_climb, self.m_wind))

        self.m_roll = _MetricCell("ROLL", "°")
        self.m_pitch = _MetricCell("PITCH", "°")
        lay.addWidget(_metric_row(self.m_roll, self.m_pitch))

        self.m_battery = _MetricCell("BAT", "В")
        self.m_gps = _MetricCell("GPS")
        lay.addWidget(_metric_row(self.m_battery, self.m_gps))

        # Wind override
        lay.addWidget(_Separator())

        wind_row = QHBoxLayout()
        wind_row.setSpacing(4)

        wlbl = QLabel()
        wlbl.setPixmap(qta.icon("mdi.weather-windy", color=Colors.TEXT_TERTIARY).pixmap(14, 14))
        wind_row.addWidget(wlbl)

        self.spin_wind_dir = QSpinBox()
        self.spin_wind_dir.setRange(0, 360)
        self.spin_wind_dir.setWrapping(True)
        self.spin_wind_dir.setSuffix("°")
        self.spin_wind_dir.setFixedWidth(60)
        self.spin_wind_dir.setFixedHeight(26)
        wind_row.addWidget(self.spin_wind_dir)

        self.spin_wind_speed = QSpinBox()
        self.spin_wind_speed.setRange(0, 30)
        self.spin_wind_speed.setSuffix(" м/с")
        self.spin_wind_speed.setFixedWidth(68)
        self.spin_wind_speed.setFixedHeight(26)
        wind_row.addWidget(self.spin_wind_speed)

        self.btn_set_wind = QPushButton()
        self.btn_set_wind.setIcon(qta.icon("mdi.check", color="#fff"))
        self.btn_set_wind.setFixedSize(26, 26)
        self.btn_set_wind.setStyleSheet(f"""
            QPushButton {{
                background-color: {Colors.PRIMARY};
                border: none;
                border-radius: 6px;
            }}
            QPushButton:hover {{ background-color: {Colors.PRIMARY_HOVER}; }}
        """)
        self.btn_set_wind.clicked.connect(self._on_set_wind)
        wind_row.addWidget(self.btn_set_wind)

        wind_row.addStretch()
        lay.addLayout(wind_row)

        # ── NAVIGATION ──
        lay.addWidget(_Separator())
        lay.addWidget(_SectionHeader("mdi.navigation-variant-outline", "Навигация"))

        self.m_waypoint = _MetricCell("WPT")
        self.m_distance = _MetricCell("DIST")
        lay.addWidget(_metric_row(self.m_waypoint, self.m_distance))

        self.m_eta = _MetricCell("ETA")
        self.m_xtk = _MetricCell("XTK")
        lay.addWidget(_metric_row(self.m_eta, self.m_xtk))

        # Compat mapping for MainWindow
        self.labels = {
            "Точка": (self.m_waypoint.value_label, ""),
            "Дистанция": (self.m_distance.value_label, ""),
            "Время приб.": (self.m_eta.value_label, ""),
            "Бок. уклон.": (self.m_xtk.value_label, ""),
        }

        # ── AUTOPILOT ──
        lay.addWidget(_Separator())
        lay.addWidget(_SectionHeader("mdi.airplane-cog", "Автопилот"))

        # Mode + action row
        ap_card = QFrame()
        ap_card.setStyleSheet(f"""
            QFrame {{
                background-color: {Colors.BG_CARD};
                border: 1px solid {Colors.BORDER};
                border-radius: 8px;
            }}
        """)
        ap_card_lay = QVBoxLayout(ap_card)
        ap_card_lay.setContentsMargins(10, 8, 10, 8)
        ap_card_lay.setSpacing(4)

        mode_row = QHBoxLayout()
        mode_row.setSpacing(8)
        mode_lbl = QLabel("Режим")
        mode_lbl.setStyleSheet(f"color: {Colors.TEXT_TERTIARY}; font-size: 10px;")
        mode_row.addWidget(mode_lbl)
        mode_row.addStretch()

        self.lbl_ap_mode = QLabel("РУЧНОЙ")
        self.lbl_ap_mode.setStyleSheet(f"""
            color: {Colors.TEXT_DIM};
            font-family: "{Fonts.MONO}";
            font-size: 12px;
            font-weight: 700;
            padding: 2px 8px;
            border-radius: 4px;
            background-color: {Colors.BG_INPUT};
        """)
        mode_row.addWidget(self.lbl_ap_mode)
        ap_card_lay.addLayout(mode_row)

        self.lbl_ap_action = QLabel("---")
        self.lbl_ap_action.setAlignment(Qt.AlignCenter)
        self.lbl_ap_action.setStyleSheet(f"""
            color: {Colors.TEXT_DIM};
            font-family: "{Fonts.MONO}";
            font-size: 11px;
        """)
        ap_card_lay.addWidget(self.lbl_ap_action)

        # Data rows
        for attr, label_text in [
            ("lbl_ap_target", "Цел. курс"),
            ("lbl_ap_error", "Ош. курса"),
            ("lbl_ap_alt_error", "Ош. высоты"),
        ]:
            row = QHBoxLayout()
            row.setSpacing(0)
            lbl = QLabel(label_text)
            lbl.setStyleSheet(f"color: {Colors.TEXT_TERTIARY}; font-size: 10px;")
            row.addWidget(lbl)
            row.addStretch()
            val = QLabel("---")
            val.setStyleSheet(f"""
                color: {Colors.TEXT_PRIMARY};
                font-family: "{Fonts.MONO}";
                font-size: 13px;
                font-weight: 600;
            """)
            row.addWidget(val)
            setattr(self, attr, val)
            ap_card_lay.addLayout(row)

        lay.addWidget(ap_card)

        # Controls grid
        ctrl_card = QFrame()
        ctrl_card.setStyleSheet(f"""
            QFrame {{
                background-color: {Colors.BG_CARD};
                border: 1px solid {Colors.BORDER};
                border-radius: 8px;
            }}
        """)
        ctrl_lay = QGridLayout(ctrl_card)
        ctrl_lay.setContentsMargins(10, 8, 10, 8)
        ctrl_lay.setSpacing(6)

        for row_idx, (label_text, attr_spin, attr_btn, suffix, lo, hi, default) in enumerate([
            ("ALT", "spin_target_alt", "btn_set_altitude", " м", 10, 5000, 100),
            ("RAD", "spin_orbit_radius", "btn_set_radius", " м", 30, 500, 150),
            ("SPD", "spin_target_speed", "btn_set_speed", " м/с", 15, 35, 20),
        ]):
            lbl = QLabel(label_text)
            lbl.setStyleSheet(f"color: {Colors.TEXT_TERTIARY}; font-size: 10px; font-weight: 600;")
            ctrl_lay.addWidget(lbl, row_idx, 0)

            spin = QSpinBox()
            spin.setRange(lo, hi)
            spin.setValue(default)
            spin.setSuffix(suffix)
            spin.setEnabled(False)
            spin.setFixedHeight(26)
            setattr(self, attr_spin, spin)
            ctrl_lay.addWidget(spin, row_idx, 1)

            btn = QPushButton()
            btn.setIcon(qta.icon("mdi.check", color="#fff"))
            btn.setFixedSize(26, 26)
            btn.setEnabled(False)
            btn.setStyleSheet(f"""
                QPushButton {{
                    background-color: {Colors.PRIMARY};
                    border: none;
                    border-radius: 6px;
                }}
                QPushButton:hover {{ background-color: {Colors.PRIMARY_HOVER}; }}
                QPushButton:disabled {{
                    background-color: {Colors.BG_INPUT};
                    border: 1px solid {Colors.BORDER};
                }}
            """)
            setattr(self, attr_btn, btn)
            ctrl_lay.addWidget(btn, row_idx, 2)

        self.btn_set_altitude.clicked.connect(self._on_set_altitude)
        self.btn_set_radius.clicked.connect(self._on_set_radius)
        self.btn_set_speed.clicked.connect(self._on_set_speed)

        lay.addWidget(ctrl_card)
        lay.addStretch()

        scroll.setWidget(content)
        outer.addWidget(scroll)

    # ─── Updates ───

    def update_telemetry(self, state: TelemetryState):
        self.m_airspeed.set_value(state.airspeed, 1)
        self.m_groundspeed.set_value(state.groundspeed, 1)
        self.m_heading.set_value(state.heading, 0)
        self.m_track.set_value(state.heading, 0)
        self.m_altitude.set_value(state.altitude, 0)
        self.m_altitude_agl.set_value(state.altitude_agl, 0)
        self.m_climb.set_value(state.climb_rate, 1)

        self.m_wind.set_text(f"{int(state.wind_direction):03d}°/{state.wind_speed:.0f}")

        self.m_roll.set_value(math.degrees(state.roll), 1)
        self.m_pitch.set_value(math.degrees(state.pitch), 1)
        self.m_battery.set_value(state.battery_voltage, 1)

        if state.gps_fix >= 3:
            self.m_gps.set_text(f"3D ({state.satellites})")
            self.m_gps.set_color(Colors.SUCCESS)
        else:
            self.m_gps.set_text(f"NO FIX ({state.satellites})")
            self.m_gps.set_color(Colors.ERROR)

    def update_navigation(self, waypoint_idx, total_waypoints, distance, eta_seconds, xtk):
        self.m_waypoint.set_text(f"{waypoint_idx}/{total_waypoints}")
        self.m_distance.set_text(f"{distance / 1000:.2f} км")
        m, s = int(eta_seconds // 60), int(eta_seconds % 60)
        self.m_eta.set_text(f"{m:02d}:{s:02d}")
        sign = "+" if xtk >= 0 else ""
        self.m_xtk.set_text(f"{sign}{xtk:.0f} м")

    # ─── Controls ───

    def _confirm_btn(self, btn):
        btn.setIcon(qta.icon("mdi.check", color=Colors.SUCCESS))

    def _on_set_altitude(self):
        self._manual_alt_override = True
        self.target_altitude_changed.emit(self.spin_target_alt.value())
        self._confirm_btn(self.btn_set_altitude)

    def _on_set_radius(self):
        self._manual_radius_override = True
        self.orbit_radius_changed.emit(self.spin_orbit_radius.value())
        self._confirm_btn(self.btn_set_radius)

    def _on_set_speed(self):
        self._manual_speed_override = True
        self.target_airspeed_changed.emit(self.spin_target_speed.value())
        self._confirm_btn(self.btn_set_speed)

    def _on_set_wind(self):
        self.wind_override_requested.emit(
            self.spin_wind_dir.value(),
            self.spin_wind_speed.value()
        )
        self.btn_set_wind.setIcon(qta.icon("mdi.check", color=Colors.SUCCESS))

    # ─── Autopilot display ───

    def update_autopilot(self, mode: str, status: dict = None):
        mode_names = {'MANUAL': 'РУЧНОЙ', 'NAV': 'НАВИГАЦИЯ'}
        self.lbl_ap_mode.setText(mode_names.get(mode, mode))

        if mode == 'MANUAL':
            self.lbl_ap_mode.setStyleSheet(f"""
                color: {Colors.TEXT_DIM};
                font-family: "{Fonts.MONO}"; font-size: 12px; font-weight: 700;
                padding: 2px 8px; border-radius: 4px;
                background-color: {Colors.BG_INPUT};
            """)
            if status and status.get('disengage_reason'):
                self.lbl_ap_action.setText(status['disengage_reason'])
                self.lbl_ap_action.setStyleSheet(f"color: {Colors.WARNING}; font-family: '{Fonts.MONO}'; font-size: 11px;")
            else:
                self.lbl_ap_action.setText("---")
                self.lbl_ap_action.setStyleSheet(f"color: {Colors.TEXT_DIM}; font-family: '{Fonts.MONO}'; font-size: 11px;")

            self.lbl_ap_target.setText("---")
            self.lbl_ap_error.setText("---")
            self.lbl_ap_alt_error.setText("---")

            for w in (self.spin_target_alt, self.spin_orbit_radius, self.spin_target_speed):
                w.setEnabled(False)
            for w in (self.btn_set_altitude, self.btn_set_radius, self.btn_set_speed):
                w.setEnabled(False)
                w.setIcon(qta.icon("mdi.check", color="#fff"))

            self._manual_alt_override = False
            self._manual_radius_override = False
            self._manual_speed_override = False
        else:
            self.lbl_ap_mode.setStyleSheet(f"""
                color: #fff;
                font-family: "{Fonts.MONO}"; font-size: 12px; font-weight: 700;
                padding: 2px 8px; border-radius: 4px;
                background-color: {Colors.SUCCESS};
            """)

            if status:
                action = status.get('action', 'IDLE')
                action_text = self._format_action(action)
                self.lbl_ap_action.setText(action_text)

                if status.get('is_orbiting'):
                    self.lbl_ap_action.setStyleSheet(f"color: {Colors.WARNING}; font-family: '{Fonts.MONO}'; font-size: 11px; font-weight: 700;")
                else:
                    self.lbl_ap_action.setStyleSheet(f"color: {Colors.PRIMARY_LIGHT}; font-family: '{Fonts.MONO}'; font-size: 11px;")

                th = status.get('target_heading')
                if th is not None:
                    self.lbl_ap_target.setText(f"{th:.0f}°")
                he = status.get('heading_error')
                if he is not None:
                    self.lbl_ap_error.setText(f"{'+' if he >= 0 else ''}{he:.1f}°")
                ae = status.get('altitude_error')
                if ae is not None:
                    self.lbl_ap_alt_error.setText(f"{'+' if ae >= 0 else ''}{ae:.0f} м")

                for w in (self.spin_target_alt, self.spin_orbit_radius, self.spin_target_speed):
                    w.setEnabled(True)
                for w in (self.btn_set_altitude, self.btn_set_radius, self.btn_set_speed):
                    w.setEnabled(True)

                if not self._manual_alt_override:
                    v = status.get('target_altitude', 100)
                    if v > 0:
                        self.spin_target_alt.blockSignals(True)
                        self.spin_target_alt.setValue(int(v))
                        self.spin_target_alt.blockSignals(False)
                if not self._manual_radius_override:
                    v = status.get('orbit_radius', 150)
                    if v > 0:
                        self.spin_orbit_radius.blockSignals(True)
                        self.spin_orbit_radius.setValue(int(v))
                        self.spin_orbit_radius.blockSignals(False)
                if not self._manual_speed_override:
                    v = status.get('target_airspeed', 20)
                    if v > 0:
                        self.spin_target_speed.blockSignals(True)
                        self.spin_target_speed.setValue(int(v))
                        self.spin_target_speed.blockSignals(False)

    @staticmethod
    def _format_action(action: str) -> str:
        if action.startswith('ORBIT_') and '/' in action:
            parts = action.split('(')
            orbit_part = parts[0].strip()
            vertical = f" ({parts[1]}" if len(parts) > 1 else ""
            return f"Круг {orbit_part.split('_')[1]}{vertical}"
        if action.startswith('ORBIT_INF'):
            parts = action.split('(')
            vertical = f" ({parts[1]}" if len(parts) > 1 else ""
            return f"Кружение ∞{vertical}"
        if action.startswith('ALTITUDE_ORBIT'):
            parts = action.split('(')
            vertical = f" ({parts[1]}" if len(parts) > 1 else ""
            return f"Набор высоты{vertical}"
        if action.startswith('TO_WAYPOINT'):
            parts = action.split('(')
            vertical = f" ({parts[1]}" if len(parts) > 1 else ""
            return f"К точке{vertical}"
        if action.startswith('ОЖИДАНИЕ_ВЫСОТЫ'):
            parts = action.split('(')
            vertical = f" ({parts[1]}" if len(parts) > 1 else ""
            return f"Ожидание высоты{vertical}"
        if action == 'ORBITING':
            return "Кружение"
        if action == 'IDLE':
            return "---"
        return action
