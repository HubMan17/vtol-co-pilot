from PyQt5.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QFormLayout,
    QLabel, QSpinBox, QDoubleSpinBox, QComboBox,
    QPushButton, QGroupBox
)
from PyQt5.QtCore import Qt


class WaypointDialog(QDialog):
    WAYPOINT_TYPES = [
        ("FLYTHROUGH", "Пролёт"),
        ("ORBIT_ALTITUDE", "Кружить до высоты"),
        ("ORBIT_TURNS", "Кружить N кругов"),
        ("ORBIT_INFINITE", "Кружить бесконечно"),
    ]

    def __init__(self, parent=None, lat: float = 0.0, lon: float = 0.0):
        super().__init__(parent)
        self._lat = lat
        self._lon = lon
        self._setup_ui()

    def _setup_ui(self):
        self.setWindowTitle("Добавить точку маршрута")
        self.setMinimumWidth(300)

        layout = QVBoxLayout(self)

        coord_group = QGroupBox("Координаты")
        coord_layout = QFormLayout(coord_group)

        self.lbl_lat = QLabel(f"{self._lat:.6f}")
        self.lbl_lon = QLabel(f"{self._lon:.6f}")
        coord_layout.addRow("Широта:", self.lbl_lat)
        coord_layout.addRow("Долгота:", self.lbl_lon)

        layout.addWidget(coord_group)

        params_group = QGroupBox("Параметры")
        params_layout = QFormLayout(params_group)

        self.spin_altitude = QSpinBox()
        self.spin_altitude.setRange(10, 5000)
        self.spin_altitude.setValue(100)
        self.spin_altitude.setSuffix(" м")
        params_layout.addRow("Высота:", self.spin_altitude)

        self.combo_type = QComboBox()
        for type_id, type_name in self.WAYPOINT_TYPES:
            self.combo_type.addItem(type_name, type_id)
        self.combo_type.currentIndexChanged.connect(self._on_type_changed)
        params_layout.addRow("Тип:", self.combo_type)

        self.spin_radius = QSpinBox()
        self.spin_radius.setRange(10, 1000)
        self.spin_radius.setValue(50)
        self.spin_radius.setSuffix(" м")
        self.spin_radius.setEnabled(False)
        params_layout.addRow("Радиус принятия:", self.spin_radius)

        layout.addWidget(params_group)

        orbit_group = QGroupBox("Параметры кружения")
        orbit_layout = QFormLayout(orbit_group)

        self.spin_orbit_radius = QSpinBox()
        self.spin_orbit_radius.setRange(30, 500)
        self.spin_orbit_radius.setValue(100)
        self.spin_orbit_radius.setSuffix(" м")
        orbit_layout.addRow("Радиус кружения:", self.spin_orbit_radius)

        self.spin_orbit_turns = QSpinBox()
        self.spin_orbit_turns.setRange(1, 100)
        self.spin_orbit_turns.setValue(1)
        orbit_layout.addRow("Количество кругов:", self.spin_orbit_turns)

        self.spin_target_altitude = QSpinBox()
        self.spin_target_altitude.setRange(10, 5000)
        self.spin_target_altitude.setValue(100)
        self.spin_target_altitude.setSuffix(" м")
        orbit_layout.addRow("Целевая высота:", self.spin_target_altitude)

        self.orbit_group = orbit_group
        self.orbit_group.setVisible(False)
        layout.addWidget(orbit_group)

        btn_layout = QHBoxLayout()
        self.btn_ok = QPushButton("Добавить")
        self.btn_ok.clicked.connect(self.accept)
        self.btn_cancel = QPushButton("Отмена")
        self.btn_cancel.clicked.connect(self.reject)

        btn_layout.addStretch()
        btn_layout.addWidget(self.btn_ok)
        btn_layout.addWidget(self.btn_cancel)

        layout.addLayout(btn_layout)

        self._on_type_changed(0)

    def _on_type_changed(self, index):
        type_id = self.combo_type.currentData()

        is_orbit = type_id != "FLYTHROUGH"
        self.orbit_group.setVisible(is_orbit)

        self.spin_orbit_turns.setEnabled(type_id == "ORBIT_TURNS")
        self.spin_target_altitude.setEnabled(type_id == "ORBIT_ALTITUDE")

        self.adjustSize()

    def get_waypoint_data(self) -> dict:
        type_id = self.combo_type.currentData()

        data = {
            'lat': self._lat,
            'lon': self._lon,
            'altitude': self.spin_altitude.value(),
            'action': type_id,
            'radius': self.spin_radius.value(),
        }

        if type_id != "FLYTHROUGH":
            data['orbit_radius'] = self.spin_orbit_radius.value()

            if type_id == "ORBIT_TURNS":
                data['orbit_turns'] = self.spin_orbit_turns.value()
            elif type_id == "ORBIT_ALTITUDE":
                data['target_altitude'] = self.spin_target_altitude.value()

        return data
