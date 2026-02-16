import qtawesome as qta
from PyQt5.QtWidgets import (
    QDialog, QVBoxLayout, QFormLayout,
    QLabel, QSpinBox, QComboBox, QHBoxLayout,
    QPushButton, QGroupBox
)
from PyQt5.QtCore import Qt

from src.core.config import ZoneAvoidanceConfig
from src.gui.theme import Colors, apply_dark_titlebar


class ZoneSettingsDialog(QDialog):
    _SETTLEMENT_MODES = [
        ("disabled", "Выключено"),
        ("always", "Всегда избегать"),
        ("below_altitude", "Ниже заданной высоты"),
    ]
    _NOFLY_MODES = [
        ("disabled", "Выключено"),
        ("always", "Всегда избегать"),
        ("below_altitude", "Ниже высоты зоны"),
    ]

    def __init__(self, parent=None, config: ZoneAvoidanceConfig = None):
        super().__init__(parent)
        self._config = config or ZoneAvoidanceConfig()
        self._setup_ui()
        apply_dark_titlebar(int(self.winId()))

    def _setup_ui(self):
        self.setWindowTitle("Настройки обхода зон")
        self.setMinimumWidth(380)

        layout = QVBoxLayout(self)
        layout.setSpacing(12)
        layout.setContentsMargins(16, 16, 16, 16)

        # Settlement group
        settlement_group = QGroupBox("Населённые пункты")
        sl = QFormLayout(settlement_group)
        sl.setSpacing(8)

        self.combo_settlement_mode = QComboBox()
        for mode_id, mode_name in self._SETTLEMENT_MODES:
            self.combo_settlement_mode.addItem(mode_name, mode_id)
        idx = self.combo_settlement_mode.findData(self._config.settlement_mode)
        if idx >= 0:
            self.combo_settlement_mode.setCurrentIndex(idx)
        self.combo_settlement_mode.currentIndexChanged.connect(self._on_settlement_mode_changed)
        sl.addRow("Режим:", self.combo_settlement_mode)

        self.spin_settlement_altitude = QSpinBox()
        self.spin_settlement_altitude.setRange(10, 5000)
        self.spin_settlement_altitude.setValue(int(self._config.settlement_min_altitude))
        self.spin_settlement_altitude.setSuffix(" м")
        self.lbl_settlement_altitude = QLabel("Мин. высота:")
        sl.addRow(self.lbl_settlement_altitude, self.spin_settlement_altitude)

        self.spin_settlement_buffer = QSpinBox()
        self.spin_settlement_buffer.setRange(0, 5000)
        self.spin_settlement_buffer.setSingleStep(50)
        self.spin_settlement_buffer.setValue(int(self._config.settlement_buffer))
        self.spin_settlement_buffer.setSuffix(" м")
        sl.addRow("Буфер:", self.spin_settlement_buffer)

        layout.addWidget(settlement_group)

        # NoFly zone group
        nofly_group = QGroupBox("Запретные зоны (по умолчанию)")
        nl = QFormLayout(nofly_group)
        nl.setSpacing(8)

        self.combo_nofly_mode = QComboBox()
        for mode_id, mode_name in self._NOFLY_MODES:
            self.combo_nofly_mode.addItem(mode_name, mode_id)
        idx = self.combo_nofly_mode.findData(self._config.nofly_mode)
        if idx >= 0:
            self.combo_nofly_mode.setCurrentIndex(idx)
        nl.addRow("Режим:", self.combo_nofly_mode)

        self.spin_nofly_buffer = QSpinBox()
        self.spin_nofly_buffer.setRange(0, 5000)
        self.spin_nofly_buffer.setSingleStep(50)
        self.spin_nofly_buffer.setValue(int(self._config.nofly_buffer))
        self.spin_nofly_buffer.setSuffix(" м")
        nl.addRow("Буфер:", self.spin_nofly_buffer)

        layout.addWidget(nofly_group)

        # Buttons
        btn_layout = QHBoxLayout()
        btn_layout.setSpacing(8)

        self.btn_cancel = QPushButton("Отмена")
        self.btn_cancel.setFixedHeight(32)
        self.btn_cancel.clicked.connect(self.reject)
        btn_layout.addWidget(self.btn_cancel)

        btn_layout.addStretch()

        self.btn_ok = QPushButton("  Сохранить")
        self.btn_ok.setIcon(qta.icon("mdi.content-save", color="#fff"))
        self.btn_ok.setFixedHeight(32)
        self.btn_ok.setStyleSheet(f"""
            QPushButton {{
                background-color: {Colors.PRIMARY};
                color: #fff; border: none; border-radius: 6px;
                padding: 0 14px; font-weight: 600;
            }}
            QPushButton:hover {{ background-color: {Colors.PRIMARY_HOVER}; }}
        """)
        self.btn_ok.clicked.connect(self.accept)
        btn_layout.addWidget(self.btn_ok)

        layout.addLayout(btn_layout)
        self._on_settlement_mode_changed()

    def _on_settlement_mode_changed(self, _index=None):
        is_altitude = self.combo_settlement_mode.currentData() == "below_altitude"
        self.spin_settlement_altitude.setVisible(is_altitude)
        self.lbl_settlement_altitude.setVisible(is_altitude)
        self.adjustSize()

    def get_config(self) -> ZoneAvoidanceConfig:
        return ZoneAvoidanceConfig(
            settlement_mode=self.combo_settlement_mode.currentData(),
            settlement_min_altitude=float(self.spin_settlement_altitude.value()),
            settlement_buffer=float(self.spin_settlement_buffer.value()),
            nofly_mode=self.combo_nofly_mode.currentData(),
            nofly_buffer=float(self.spin_nofly_buffer.value()),
        )
