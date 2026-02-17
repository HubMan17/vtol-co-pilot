import qtawesome as qta
from PyQt5.QtWidgets import (
    QWidget, QVBoxLayout, QGridLayout, QHBoxLayout, QLabel, QFrame,
    QSpinBox, QPushButton, QScrollArea, QSizePolicy, QAbstractSpinBox
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
    """Compact metric: small label on top, large colored value below (Mission Planner style)."""

    def __init__(self, label: str, unit: str = "", color: str = Colors.TEXT_PRIMARY):
        super().__init__()
        self._unit = unit
        self._color = color

        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 8, 10, 8)
        layout.setSpacing(2)

        self.name_label = QLabel(label)
        self.name_label.setStyleSheet(f"color: {Colors.TEXT_DIM}; font-size: 10px; border: none;")
        layout.addWidget(self.name_label)

        self.value_label = QLabel("---")
        self._apply_value_style(color)
        layout.addWidget(self.value_label)

    def _apply_value_style(self, color: str):
        self.value_label.setStyleSheet(f"""
            color: {color};
            font-family: "{Fonts.MONO}";
            font-size: 20px;
            font-weight: 700;
            border: none;
        """)

    def set_value(self, value: float, decimals: int = 1):
        text = f"{int(value)}" if decimals == 0 else f"{value:.{decimals}f}"
        if self._unit:
            text += f" {self._unit}"
        self.value_label.setText(text)

    def set_text(self, text: str):
        self.value_label.setText(text)

    def set_color(self, color: str):
        self._color = color
        self._apply_value_style(color)


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
        grid.setColumnStretch(i * 2, 1)

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

        self.m_airspeed = _MetricCell("Возд. скорость", "м/с", Colors.METRIC_SPEED)
        self.m_groundspeed = _MetricCell("Пут. скорость", "м/с", Colors.METRIC_GS)
        self.m_altitude_agl = _MetricCell("Высота отн.", "м", Colors.METRIC_ALT)
        lay.addWidget(_metric_row(self.m_airspeed, self.m_groundspeed, self.m_altitude_agl))

        # Wind cell with dropdown override
        wind_cell = QWidget()
        wc_lay = QVBoxLayout(wind_cell)
        wc_lay.setContentsMargins(10, 8, 10, 8)
        wc_lay.setSpacing(2)

        # Header row: label + dropdown arrow
        wind_hdr = QHBoxLayout()
        wind_hdr.setContentsMargins(0, 0, 0, 0)
        wind_hdr.setSpacing(4)

        wind_name = QLabel("Ветер")
        wind_name.setStyleSheet(f"color: {Colors.TEXT_DIM}; font-size: 10px; border: none;")
        wind_hdr.addWidget(wind_name)
        wind_hdr.addStretch()

        self.btn_wind_dropdown = QPushButton()
        self.btn_wind_dropdown.setIcon(qta.icon("mdi.chevron-down", color=Colors.TEXT_DIM))
        self.btn_wind_dropdown.setFixedSize(18, 18)
        self.btn_wind_dropdown.setCursor(Qt.PointingHandCursor)
        self.btn_wind_dropdown.setToolTip("Задать ветер")
        self.btn_wind_dropdown.setStyleSheet(f"""
            QPushButton {{
                background-color: transparent;
                border: none;
                border-radius: 4px;
            }}
            QPushButton:hover {{
                background-color: {Colors.BG_HOVER};
            }}
        """)
        self.btn_wind_dropdown.clicked.connect(self._toggle_wind_popup)
        wind_hdr.addWidget(self.btn_wind_dropdown)
        wc_lay.addLayout(wind_hdr)

        self.m_wind_value = QLabel("---")
        self.m_wind_value.setStyleSheet(f"""
            color: {Colors.METRIC_WIND};
            font-family: "{Fonts.MONO}";
            font-size: 20px;
            font-weight: 700;
            border: none;
        """)
        wc_lay.addWidget(self.m_wind_value)

        self.m_battery = _MetricCell("Батарея", "В", Colors.METRIC_BAT)
        self.m_gps = _MetricCell("GPS", "", Colors.METRIC_GPS)
        lay.addWidget(_metric_row(wind_cell, self.m_battery, self.m_gps))

        # Wind popup (floating, created once, shown/hidden on click)
        self._wind_popup = QFrame(self, Qt.Popup)
        self._wind_popup.setStyleSheet(f"""
            QFrame {{
                background-color: {Colors.BG_CARD};
                border: 1px solid {Colors.BORDER_LIGHT};
                border-radius: 8px;
            }}
        """)
        pop_lay = QVBoxLayout(self._wind_popup)
        pop_lay.setContentsMargins(12, 10, 12, 10)
        pop_lay.setSpacing(8)

        pop_title = QLabel("Задать ветер")
        pop_title.setStyleSheet(f"""
            color: {Colors.TEXT_TERTIARY};
            font-size: 11px;
            font-weight: 700;
            border: none;
        """)
        pop_lay.addWidget(pop_title)

        pop_spin_style = f"""
            QSpinBox {{
                background-color: {Colors.BG_INPUT};
                color: {Colors.TEXT_PRIMARY};
                border: 1px solid {Colors.BORDER};
                border-radius: 6px;
                padding: 4px 8px;
                font-family: "{Fonts.MONO}";
                font-size: 13px;
                font-weight: 600;
            }}
            QSpinBox:focus {{ border-color: {Colors.PRIMARY}; }}
        """

        # Direction
        dir_lbl = QLabel("Направление")
        dir_lbl.setStyleSheet(f"color: {Colors.TEXT_DIM}; font-size: 10px; border: none;")
        pop_lay.addWidget(dir_lbl)

        self.spin_wind_dir = QSpinBox()
        self.spin_wind_dir.setButtonSymbols(QAbstractSpinBox.NoButtons)
        self.spin_wind_dir.setRange(0, 360)
        self.spin_wind_dir.setWrapping(True)
        self.spin_wind_dir.setSuffix("°")
        self.spin_wind_dir.setFixedHeight(30)
        self.spin_wind_dir.setStyleSheet(pop_spin_style)
        pop_lay.addWidget(self.spin_wind_dir)

        # Speed
        spd_lbl = QLabel("Скорость")
        spd_lbl.setStyleSheet(f"color: {Colors.TEXT_DIM}; font-size: 10px; border: none;")
        pop_lay.addWidget(spd_lbl)

        self.spin_wind_speed = QSpinBox()
        self.spin_wind_speed.setButtonSymbols(QAbstractSpinBox.NoButtons)
        self.spin_wind_speed.setRange(0, 30)
        self.spin_wind_speed.setSuffix(" м/с")
        self.spin_wind_speed.setFixedHeight(30)
        self.spin_wind_speed.setStyleSheet(pop_spin_style)
        pop_lay.addWidget(self.spin_wind_speed)

        # Apply
        self.btn_set_wind = QPushButton("  Применить")
        self.btn_set_wind.setIcon(qta.icon("mdi.check", color="#fff"))
        self.btn_set_wind.setFixedHeight(30)
        self.btn_set_wind.setCursor(Qt.PointingHandCursor)
        self.btn_set_wind.setStyleSheet(f"""
            QPushButton {{
                background-color: {Colors.PRIMARY};
                color: #fff;
                border: none;
                border-radius: 6px;
                font-size: 12px;
                font-weight: 600;
                padding: 0 14px;
            }}
            QPushButton:hover {{ background-color: {Colors.PRIMARY_HOVER}; }}
        """)
        self.btn_set_wind.clicked.connect(self._on_set_wind)
        pop_lay.addWidget(self.btn_set_wind)

        self._wind_popup.setFixedWidth(180)
        self._wind_popup.adjustSize()

        # ── NAVIGATION ──
        lay.addWidget(_Separator())
        lay.addWidget(_SectionHeader("mdi6.navigation-variant-outline", "Навигация"))

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
        lay.addWidget(_SectionHeader("mdi6.airplane-cog", "Автопилот"))

        # Mode + action row
        ap_card = QFrame()
        ap_card.setObjectName("ap_card")
        ap_card.setStyleSheet(f"""
            #ap_card {{
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
        mode_lbl.setStyleSheet(f"color: {Colors.TEXT_TERTIARY}; font-size: 12px; border: none;")
        mode_row.addWidget(mode_lbl)
        mode_row.addStretch()

        self.lbl_ap_mode = QLabel("РУЧНОЙ")
        self.lbl_ap_mode.setStyleSheet(f"""
            color: {Colors.TEXT_DIM};
            font-family: "{Fonts.MONO}";
            font-size: 13px;
            font-weight: 700;
            padding: 3px 10px;
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
            font-size: 13px;
            border: none;
        """)
        ap_card_lay.addWidget(self.lbl_ap_action)

        # Data rows
        for attr, label_text in [
            ("lbl_ap_target", "Целевой курс"),
            ("lbl_ap_error", "Ошибка курса"),
            ("lbl_ap_alt_error", "Ошибка высоты"),
        ]:
            row = QHBoxLayout()
            row.setSpacing(0)
            lbl = QLabel(label_text)
            lbl.setStyleSheet(f"color: {Colors.TEXT_TERTIARY}; font-size: 12px; border: none;")
            row.addWidget(lbl)
            row.addStretch()
            val = QLabel("---")
            val.setStyleSheet(f"""
                color: {Colors.TEXT_PRIMARY};
                font-family: "{Fonts.MONO}";
                font-size: 15px;
                font-weight: 600;
                border: none;
            """)
            row.addWidget(val)
            setattr(self, attr, val)
            ap_card_lay.addLayout(row)

        lay.addWidget(ap_card)

        # Controls grid
        ctrl_card = QFrame()
        ctrl_card.setObjectName("ctrl_card")
        ctrl_card.setStyleSheet(f"""
            #ctrl_card {{
                background-color: {Colors.BG_CARD};
                border: 1px solid {Colors.BORDER};
                border-radius: 8px;
            }}
        """)
        ctrl_lay = QVBoxLayout(ctrl_card)
        ctrl_lay.setContentsMargins(10, 8, 10, 8)
        ctrl_lay.setSpacing(8)

        spin_style = f"""
            QSpinBox {{
                background-color: {Colors.BG_INPUT};
                color: {Colors.TEXT_PRIMARY};
                border: 1px solid {Colors.BORDER};
                border-radius: 8px;
                padding: 4px 10px;
                font-family: "{Fonts.MONO}";
                font-size: 14px;
                font-weight: 600;
                selection-background-color: {Colors.PRIMARY};
            }}
            QSpinBox:focus {{
                border-color: {Colors.PRIMARY};
            }}
            QSpinBox:disabled {{
                background-color: {Colors.BG_CARD};
                color: {Colors.TEXT_DIM};
                border-color: {Colors.BORDER_SUBTLE};
            }}
        """
        btn_style = f"""
            QPushButton {{
                background-color: {Colors.PRIMARY};
                border: none;
                border-radius: 8px;
            }}
            QPushButton:hover {{ background-color: {Colors.PRIMARY_HOVER}; }}
            QPushButton:disabled {{
                background-color: {Colors.BG_INPUT};
                border: 1px solid {Colors.BORDER};
            }}
        """

        for label_text, attr_spin, attr_btn, suffix, lo, hi, default in [
            ("Высота", "spin_target_alt", "btn_set_altitude", " м", 10, 5000, 100),
            ("Радиус", "spin_orbit_radius", "btn_set_radius", " м", 30, 500, 150),
            ("Скорость", "spin_target_speed", "btn_set_speed", " м/с", 15, 35, 20),
        ]:
            lbl = QLabel(label_text)
            lbl.setStyleSheet(f"color: {Colors.TEXT_TERTIARY}; font-size: 11px; font-weight: 600; border: none;")
            ctrl_lay.addWidget(lbl)

            input_row = QHBoxLayout()
            input_row.setSpacing(6)

            spin = QSpinBox()
            spin.setButtonSymbols(QAbstractSpinBox.NoButtons)
            spin.setRange(lo, hi)
            spin.setValue(default)
            spin.setSuffix(suffix)
            spin.setEnabled(False)
            spin.setFixedHeight(34)
            spin.setStyleSheet(spin_style)
            setattr(self, attr_spin, spin)
            input_row.addWidget(spin)

            btn = QPushButton()
            btn.setIcon(qta.icon("mdi.check", color="#fff"))
            btn.setFixedSize(34, 34)
            btn.setEnabled(False)
            btn.setStyleSheet(btn_style)
            setattr(self, attr_btn, btn)
            input_row.addWidget(btn)

            ctrl_lay.addLayout(input_row)

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
        self.m_altitude_agl.set_value(state.altitude_agl, 0)

        self.m_wind_value.setText(f"{int(state.wind_direction):03d}°/{state.wind_speed:.0f}")

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

    def _toggle_wind_popup(self):
        if self._wind_popup.isVisible():
            self._wind_popup.hide()
            return
        # Position below the dropdown arrow button
        btn = self.btn_wind_dropdown
        pos = btn.mapToGlobal(btn.rect().bottomLeft())
        self._wind_popup.move(pos.x() - self._wind_popup.width() + btn.width(), pos.y() + 4)
        self._wind_popup.show()

    def _on_set_wind(self):
        self.wind_override_requested.emit(
            self.spin_wind_dir.value(),
            self.spin_wind_speed.value()
        )
        self.btn_set_wind.setIcon(qta.icon("mdi.check", color=Colors.SUCCESS))
        self._wind_popup.hide()

    # ─── Autopilot display ───

    def update_autopilot(self, mode: str, status: dict = None):
        mode_names = {'MANUAL': 'РУЧНОЙ', 'NAV': 'НАВИГАЦИЯ'}
        self.lbl_ap_mode.setText(mode_names.get(mode, mode))

        if mode == 'MANUAL':
            self.lbl_ap_mode.setStyleSheet(f"""
                color: {Colors.TEXT_DIM};
                font-family: "{Fonts.MONO}"; font-size: 13px; font-weight: 700;
                padding: 3px 10px; border-radius: 4px;
                background-color: {Colors.BG_INPUT};
            """)
            if status and status.get('disengage_reason'):
                self.lbl_ap_action.setText(status['disengage_reason'])
                self.lbl_ap_action.setStyleSheet(f"color: {Colors.WARNING}; font-family: '{Fonts.MONO}'; font-size: 13px; border: none;")
            else:
                self.lbl_ap_action.setText("---")
                self.lbl_ap_action.setStyleSheet(f"color: {Colors.TEXT_DIM}; font-family: '{Fonts.MONO}'; font-size: 13px; border: none;")

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
                font-family: "{Fonts.MONO}"; font-size: 13px; font-weight: 700;
                padding: 3px 10px; border-radius: 4px;
                background-color: {Colors.SUCCESS};
            """)

            if status:
                action = status.get('action', 'IDLE')
                action_text = self._format_action(action)
                self.lbl_ap_action.setText(action_text)

                if status.get('is_orbiting'):
                    self.lbl_ap_action.setStyleSheet(f"color: {Colors.WARNING}; font-family: '{Fonts.MONO}'; font-size: 13px; font-weight: 700; border: none;")
                else:
                    self.lbl_ap_action.setStyleSheet(f"color: {Colors.PRIMARY_LIGHT}; font-family: '{Fonts.MONO}'; font-size: 13px; border: none;")

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
