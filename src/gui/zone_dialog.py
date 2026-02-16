import qtawesome as qta
from PyQt5.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QFormLayout,
    QLabel, QLineEdit, QTextEdit, QSpinBox,
    QPushButton, QGroupBox
)
from PyQt5.QtCore import Qt

from src.gui.theme import Colors, Fonts, apply_dark_titlebar


class ZonePropertiesDialog(QDialog):
    DELETE_REQUESTED = 1001

    def __init__(self, parent=None, zone=None):
        super().__init__(parent)
        self._zone = zone
        self._delete_requested = False
        self._setup_ui()
        apply_dark_titlebar(int(self.winId()))

    def _setup_ui(self):
        editing = self._zone is not None
        self.setWindowTitle("Редактировать зону" if editing else "Запретная зона")
        self.setMinimumWidth(340)

        layout = QVBoxLayout(self)
        layout.setSpacing(12)
        layout.setContentsMargins(16, 16, 16, 16)

        # Parameters
        params_group = QGroupBox("Параметры")
        params_layout = QFormLayout(params_group)
        params_layout.setSpacing(8)

        self.edit_name = QLineEdit()
        if editing and self._zone.name:
            self.edit_name.setText(self._zone.name)
        params_layout.addRow("Имя:", self.edit_name)

        self.edit_description = QTextEdit()
        self.edit_description.setFixedHeight(60)
        if editing and self._zone.description:
            self.edit_description.setPlainText(self._zone.description)
        params_layout.addRow("Описание:", self.edit_description)

        self.spin_altitude = QSpinBox()
        self.spin_altitude.setRange(0, 10000)
        self.spin_altitude.setSingleStep(10)
        self.spin_altitude.setSuffix(" м")
        self.spin_altitude.setSpecialValueText(" ")
        if editing and self._zone.altitude is not None:
            self.spin_altitude.setValue(int(self._zone.altitude))
        params_layout.addRow("Макс. высота:", self.spin_altitude)

        layout.addWidget(params_group)

        # Buttons
        btn_layout = QHBoxLayout()
        btn_layout.setSpacing(8)

        if editing:
            self.btn_delete = QPushButton("  Удалить")
            self.btn_delete.setIcon(qta.icon("mdi.delete-outline", color=Colors.ERROR))
            self.btn_delete.setFixedHeight(32)
            self.btn_delete.setStyleSheet(f"""
                QPushButton {{
                    background-color: {Colors.ERROR_BG};
                    color: {Colors.ERROR}; border: 1px solid {Colors.ERROR};
                    border-radius: 6px; padding: 0 12px; font-weight: 600;
                }}
                QPushButton:hover {{ background-color: {Colors.ERROR}; color: #fff; }}
            """)
            self.btn_delete.clicked.connect(self._on_delete)
            btn_layout.addWidget(self.btn_delete)

        btn_layout.addStretch()

        self.btn_cancel = QPushButton("Отмена")
        self.btn_cancel.setFixedHeight(32)
        self.btn_cancel.clicked.connect(self.reject)
        btn_layout.addWidget(self.btn_cancel)

        ok_text = "  Сохранить" if editing else "  Создать"
        ok_icon = "mdi.content-save" if editing else "mdi.plus"
        self.btn_ok = QPushButton(ok_text)
        self.btn_ok.setIcon(qta.icon(ok_icon, color="#fff"))
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

    def _on_delete(self):
        self._delete_requested = True
        self.done(self.DELETE_REQUESTED)

    def is_delete_requested(self) -> bool:
        return self._delete_requested

    def get_zone_data(self) -> dict:
        alt_val = self.spin_altitude.value()
        return {
            'name': self.edit_name.text().strip(),
            'description': self.edit_description.toPlainText().strip(),
            'altitude': float(alt_val) if alt_val > 0 else None,
        }
