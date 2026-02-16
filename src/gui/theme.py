"""
Dark theme for VTOL Co-Pilot.
Fully dark — no light surfaces anywhere.
"""
import ctypes
import sys


class Colors:
    # Backgrounds — everything near-black
    BG_APP = "#0B0F1A"
    BG_CARD = "#111827"
    BG_SIDEBAR = "#0F1320"
    BG_INPUT = "#1A2035"
    BG_HOVER = "#1E293B"
    BG_TOOLTIP = "#1E293B"
    BG_ELEVATED = "#162031"

    # Accents
    PRIMARY = "#2563EB"
    PRIMARY_HOVER = "#3B82F6"
    PRIMARY_LIGHT = "#3B82F6"
    PRIMARY_DIM = "rgba(37, 99, 235, 0.15)"
    SUCCESS = "#22C55E"
    SUCCESS_BG = "rgba(34, 197, 94, 0.12)"
    ERROR = "#EF4444"
    ERROR_BG = "rgba(239, 68, 68, 0.12)"
    WARNING = "#F59E0B"
    WARNING_BG = "rgba(245, 158, 11, 0.12)"

    # Text
    TEXT_PRIMARY = "#FFFFFF"
    TEXT_SECONDARY = "#E2E8F0"
    TEXT_TERTIARY = "#CBD5E1"
    TEXT_DIM = "#94A3B8"

    # Metric colors (Mission Planner style)
    METRIC_SPEED = "#FF9F43"       # orange — airspeed
    METRIC_GS = "#2ED573"          # green — groundspeed
    METRIC_HDG = "#FFFFFF"         # white — heading/track
    METRIC_ALT = "#FECA57"         # yellow — altitude
    METRIC_VS = "#DCDDE1"          # light gray — vertical speed
    METRIC_WIND = "#48DBFB"        # cyan — wind
    METRIC_ATT = "#C8A2FF"         # lavender — roll/pitch
    METRIC_BAT = "#FF6B6B"         # red — battery
    METRIC_GPS = "#2ED573"         # green — gps

    # Borders
    BORDER = "#1E293B"
    BORDER_LIGHT = "#334155"
    BORDER_SUBTLE = "#1A2332"


class Fonts:
    FAMILY = "Segoe UI"
    MONO = "Consolas"


def apply_dark_titlebar(hwnd: int):
    """Enable dark title bar on Windows 10/11 via DWM."""
    if sys.platform != "win32":
        return
    try:
        DWMWA_USE_IMMERSIVE_DARK_MODE = 20
        value = ctypes.c_int(1)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(
            hwnd,
            DWMWA_USE_IMMERSIVE_DARK_MODE,
            ctypes.byref(value),
            ctypes.sizeof(value),
        )
    except Exception:
        pass


STYLESHEET = f"""
/* ===== Global — force dark on everything ===== */
* {{
    background-color: transparent;
    color: {Colors.TEXT_PRIMARY};
}}
QMainWindow {{
    background-color: {Colors.BG_APP};
}}
QWidget {{
    font-family: "{Fonts.FAMILY}";
    font-size: 13px;
    color: {Colors.TEXT_PRIMARY};
    background-color: transparent;
}}

/* ===== Scrollbars (both axes) ===== */
QScrollBar:vertical {{
    background: transparent;
    width: 5px;
    margin: 0;
}}
QScrollBar::handle:vertical {{
    background: {Colors.BORDER_LIGHT};
    border-radius: 2px;
    min-height: 24px;
}}
QScrollBar::handle:vertical:hover {{
    background: {Colors.TEXT_TERTIARY};
}}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
    height: 0;
}}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {{
    background: transparent;
}}
QScrollBar:horizontal {{
    background: transparent;
    height: 5px;
    margin: 0;
}}
QScrollBar::handle:horizontal {{
    background: {Colors.BORDER_LIGHT};
    border-radius: 2px;
    min-width: 24px;
}}
QScrollBar::handle:horizontal:hover {{
    background: {Colors.TEXT_TERTIARY};
}}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {{
    width: 0;
}}
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {{
    background: transparent;
}}

/* ===== QPushButton ===== */
QPushButton {{
    background-color: {Colors.BG_INPUT};
    color: {Colors.TEXT_SECONDARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 6px;
    padding: 6px 14px;
    font-weight: 500;
    font-size: 12px;
}}
QPushButton:hover {{
    background-color: {Colors.BG_HOVER};
    color: {Colors.TEXT_PRIMARY};
    border-color: {Colors.BORDER_LIGHT};
}}
QPushButton:pressed {{
    background-color: {Colors.BORDER_LIGHT};
}}
QPushButton:disabled {{
    background-color: {Colors.BG_CARD};
    color: {Colors.TEXT_DIM};
    border-color: {Colors.BORDER_SUBTLE};
}}
QPushButton:checked {{
    background-color: {Colors.PRIMARY};
    color: #ffffff;
    border-color: {Colors.PRIMARY};
}}
QPushButton:checked:hover {{
    background-color: {Colors.PRIMARY_HOVER};
}}

/* ===== QSpinBox / QDoubleSpinBox ===== */
QSpinBox, QDoubleSpinBox {{
    background-color: {Colors.BG_INPUT};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 6px;
    padding: 4px 6px;
    font-family: "{Fonts.MONO}";
    font-size: 12px;
    selection-background-color: {Colors.PRIMARY};
}}
QSpinBox:focus, QDoubleSpinBox:focus {{
    border-color: {Colors.PRIMARY};
}}
QSpinBox:disabled, QDoubleSpinBox:disabled {{
    background-color: {Colors.BG_CARD};
    color: {Colors.TEXT_DIM};
    border-color: {Colors.BORDER_SUBTLE};
}}
QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {{
    width: 16px;
    border: none;
    background: transparent;
}}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {{
    image: none;
    border-left: 3px solid transparent;
    border-right: 3px solid transparent;
    border-bottom: 4px solid {Colors.TEXT_TERTIARY};
    width: 0; height: 0;
}}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {{
    image: none;
    border-left: 3px solid transparent;
    border-right: 3px solid transparent;
    border-top: 4px solid {Colors.TEXT_TERTIARY};
    width: 0; height: 0;
}}

/* ===== QLineEdit / QTextEdit / QPlainTextEdit ===== */
QLineEdit, QTextEdit, QPlainTextEdit {{
    background-color: {Colors.BG_INPUT};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 6px;
    padding: 4px 8px;
    selection-background-color: {Colors.PRIMARY};
}}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {{
    border-color: {Colors.PRIMARY};
}}

/* ===== QComboBox ===== */
QComboBox {{
    background-color: {Colors.BG_INPUT};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 6px;
    padding: 5px 10px;
    font-size: 12px;
}}
QComboBox:focus {{
    border-color: {Colors.PRIMARY};
}}
QComboBox::drop-down {{
    border: none;
    width: 20px;
}}
QComboBox::down-arrow {{
    image: none;
    border-left: 3px solid transparent;
    border-right: 3px solid transparent;
    border-top: 4px solid {Colors.TEXT_TERTIARY};
    width: 0; height: 0;
}}
QComboBox QAbstractItemView {{
    background-color: {Colors.BG_CARD};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    selection-background-color: {Colors.BG_HOVER};
    outline: none;
    padding: 2px;
}}

/* ===== QCheckBox ===== */
QCheckBox {{
    color: {Colors.TEXT_PRIMARY};
    spacing: 6px;
    font-size: 12px;
}}
QCheckBox::indicator {{
    width: 16px;
    height: 16px;
    border: 1px solid {Colors.BORDER_LIGHT};
    border-radius: 3px;
    background-color: {Colors.BG_INPUT};
}}
QCheckBox::indicator:checked {{
    background-color: {Colors.PRIMARY};
    border-color: {Colors.PRIMARY};
}}

/* ===== QGroupBox ===== */
QGroupBox {{
    background-color: {Colors.BG_CARD};
    border: 1px solid {Colors.BORDER};
    border-radius: 8px;
    margin-top: 14px;
    padding: 14px 10px 10px 10px;
    font-weight: 600;
    color: {Colors.TEXT_PRIMARY};
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    subcontrol-position: top left;
    padding: 0 6px;
    left: 10px;
    color: {Colors.TEXT_TERTIARY};
    font-size: 10px;
    font-weight: 600;
}}

/* ===== QLabel ===== */
QLabel {{
    background: transparent;
    border: none;
}}

/* ===== QFrame ===== */
QFrame {{
    border: none;
}}

/* ===== QStatusBar ===== */
QStatusBar {{
    background-color: {Colors.BG_SIDEBAR};
    color: {Colors.TEXT_TERTIARY};
    border-top: 1px solid {Colors.BORDER_SUBTLE};
    font-size: 11px;
    padding: 2px 10px;
}}
QStatusBar QLabel {{
    color: {Colors.TEXT_TERTIARY};
}}

/* ===== QSplitter ===== */
QSplitter::handle {{
    background-color: {Colors.BORDER};
}}
QSplitter::handle:hover {{
    background-color: {Colors.PRIMARY};
}}
QSplitter::handle:pressed {{
    background-color: {Colors.PRIMARY_HOVER};
}}

/* ===== QMenu ===== */
QMenu {{
    background-color: {Colors.BG_CARD};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 8px;
    padding: 4px;
}}
QMenu::item {{
    padding: 6px 14px;
    border-radius: 4px;
}}
QMenu::item:selected {{
    background-color: {Colors.BG_HOVER};
}}
QMenu::separator {{
    height: 1px;
    background: {Colors.BORDER};
    margin: 4px 8px;
}}

/* ===== QDialog ===== */
QDialog {{
    background-color: {Colors.BG_APP};
    color: {Colors.TEXT_PRIMARY};
}}

/* ===== QMessageBox ===== */
QMessageBox {{
    background-color: {Colors.BG_APP};
    color: {Colors.TEXT_PRIMARY};
}}
QMessageBox QLabel {{
    color: {Colors.TEXT_PRIMARY};
}}

/* ===== QFileDialog ===== */
QFileDialog {{
    background-color: {Colors.BG_APP};
    color: {Colors.TEXT_PRIMARY};
}}

/* ===== QToolTip ===== */
QToolTip {{
    background-color: {Colors.BG_CARD};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 11px;
}}

/* ===== QScrollArea ===== */
QScrollArea {{
    background: transparent;
    border: none;
}}

/* ===== QTabWidget / QTabBar ===== */
QTabWidget::pane {{
    background-color: {Colors.BG_APP};
    border: 1px solid {Colors.BORDER};
}}
QTabBar::tab {{
    background-color: {Colors.BG_CARD};
    color: {Colors.TEXT_SECONDARY};
    border: 1px solid {Colors.BORDER};
    padding: 6px 14px;
}}
QTabBar::tab:selected {{
    background-color: {Colors.BG_APP};
    color: {Colors.TEXT_PRIMARY};
    border-bottom-color: {Colors.BG_APP};
}}

/* ===== QHeaderView (table headers) ===== */
QHeaderView::section {{
    background-color: {Colors.BG_CARD};
    color: {Colors.TEXT_SECONDARY};
    border: 1px solid {Colors.BORDER};
    padding: 4px 8px;
    font-weight: 600;
}}

/* ===== QTableView / QTreeView / QListView ===== */
QTableView, QTreeView, QListView {{
    background-color: {Colors.BG_APP};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    selection-background-color: {Colors.BG_HOVER};
    gridline-color: {Colors.BORDER};
    alternate-background-color: {Colors.BG_CARD};
}}

/* ===== QProgressBar ===== */
QProgressBar {{
    background-color: {Colors.BG_INPUT};
    border: 1px solid {Colors.BORDER};
    border-radius: 4px;
    text-align: center;
    color: {Colors.TEXT_PRIMARY};
}}
QProgressBar::chunk {{
    background-color: {Colors.PRIMARY};
    border-radius: 3px;
}}
"""
