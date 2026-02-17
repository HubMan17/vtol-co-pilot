import qtawesome as qta
from PyQt5.QtWidgets import (
    QDialog, QVBoxLayout, QFormLayout,
    QSpinBox, QPushButton, QHBoxLayout, QGroupBox
)
from PyQt5.QtCore import Qt

from src.core.config import GUIConfig
from src.gui.theme import Colors, apply_dark_titlebar


class SettingsDialog(QDialog):

    def __init__(self, parent=None, config: GUIConfig = None):
        super().__init__(parent)
        self._config = config or GUIConfig()
        self._setup_ui()
        apply_dark_titlebar(int(self.winId()))

    def _setup_ui(self):
        self.setWindowTitle("Настройки")
        self.setMinimumWidth(360)

        layout = QVBoxLayout(self)
        layout.setSpacing(12)
        layout.setContentsMargins(16, 16, 16, 16)

        # Map / track group
        map_group = QGroupBox("Карта")
        ml = QFormLayout(map_group)
        ml.setSpacing(8)

        self.spin_track_length = QSpinBox()
        self.spin_track_length.setRange(100, 200_000)
        self.spin_track_length.setSingleStep(100)
        self.spin_track_length.setValue(self._config.track_length)
        self.spin_track_length.setSuffix(" точек")
        ml.addRow("Длина трека:", self.spin_track_length)

        layout.addWidget(map_group)

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

    def get_gui_config(self) -> GUIConfig:
        return GUIConfig(
            map_center=self._config.map_center,
            map_zoom=self._config.map_zoom,
            track_length=self.spin_track_length.value(),
        )
