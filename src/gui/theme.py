"""
Dark analytics dashboard theme for VTOL Co-Pilot.
Inspired by Stripe Dashboard, Linear, Vercel — dark mode with blue accents.
"""


class Colors:
    # Backgrounds (dark to light)
    BG_APP = "#0B0F1A"
    BG_CARD = "#111827"
    BG_SIDEBAR = "#0D1117"
    BG_INPUT = "#1A2035"
    BG_HOVER = "#1A2035"
    BG_TOOLTIP = "#1E293B"

    # Accents
    PRIMARY = "#2563EB"
    PRIMARY_HOVER = "#3B82F6"
    PRIMARY_LIGHT = "#3B82F6"
    SUCCESS = "#22C55E"
    SUCCESS_BG = "rgba(34, 197, 94, 0.1)"
    ERROR = "#EF4444"
    ERROR_BG = "rgba(239, 68, 68, 0.1)"
    WARNING = "#F59E0B"
    WARNING_HOVER = "#FBBF24"

    # Text
    TEXT_PRIMARY = "#F9FAFB"
    TEXT_SECONDARY = "#9CA3AF"
    TEXT_TERTIARY = "#6B7280"
    TEXT_DATA = "#D1D5DB"

    # Borders
    BORDER = "#1F2937"
    BORDER_TABLE = "#1E293B"
    BORDER_BUTTON = "#374151"


class Fonts:
    FAMILY = "Segoe UI, Inter, -apple-system, sans-serif"
    MONO = "Consolas, JetBrains Mono, Fira Code, monospace"
    SIZE_METRIC_LARGE = 26
    SIZE_METRIC = 18
    SIZE_HEADING = 14
    SIZE_BODY = 13
    SIZE_SMALL = 11
    SIZE_BADGE = 11


STYLESHEET = f"""
/* ===== Global ===== */
QMainWindow, QWidget {{
    background-color: {Colors.BG_APP};
    color: {Colors.TEXT_PRIMARY};
    font-family: {Fonts.FAMILY};
    font-size: {Fonts.SIZE_BODY}px;
}}

/* ===== Scrollbar ===== */
QScrollBar:vertical {{
    background: transparent;
    width: 6px;
    margin: 0;
}}
QScrollBar::handle:vertical {{
    background: {Colors.BORDER_BUTTON};
    border-radius: 3px;
    min-height: 20px;
}}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
    height: 0;
}}
QScrollBar:horizontal {{
    background: transparent;
    height: 6px;
    margin: 0;
}}
QScrollBar::handle:horizontal {{
    background: {Colors.BORDER_BUTTON};
    border-radius: 3px;
    min-width: 20px;
}}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {{
    width: 0;
}}

/* ===== QPushButton ===== */
QPushButton {{
    background-color: {Colors.BG_INPUT};
    color: {Colors.TEXT_DATA};
    border: 1px solid {Colors.BORDER_BUTTON};
    border-radius: 8px;
    padding: 8px 16px;
    font-size: {Fonts.SIZE_BODY}px;
    font-weight: 500;
}}
QPushButton:hover {{
    background-color: {Colors.BG_TOOLTIP};
    border-color: {Colors.TEXT_TERTIARY};
}}
QPushButton:pressed {{
    background-color: {Colors.BORDER_BUTTON};
}}
QPushButton:disabled {{
    background-color: {Colors.BG_CARD};
    color: {Colors.TEXT_TERTIARY};
    border-color: {Colors.BORDER};
}}
QPushButton:checked {{
    background-color: {Colors.PRIMARY};
    color: #ffffff;
    border-color: {Colors.PRIMARY};
}}
QPushButton:checked:hover {{
    background-color: {Colors.PRIMARY_HOVER};
}}

/* Primary button class */
QPushButton[cssClass="primary"] {{
    background-color: {Colors.PRIMARY};
    color: #ffffff;
    border: none;
    font-weight: 600;
}}
QPushButton[cssClass="primary"]:hover {{
    background-color: {Colors.PRIMARY_HOVER};
}}
QPushButton[cssClass="primary"]:pressed {{
    background-color: #1D4ED8;
}}

/* Danger button */
QPushButton[cssClass="danger"] {{
    background-color: transparent;
    color: {Colors.ERROR};
    border: 1px solid {Colors.ERROR};
}}
QPushButton[cssClass="danger"]:hover {{
    background-color: {Colors.ERROR_BG};
}}

/* Success button */
QPushButton[cssClass="success"] {{
    background-color: {Colors.SUCCESS};
    color: #ffffff;
    border: none;
}}
QPushButton[cssClass="success"]:hover {{
    background-color: #16A34A;
}}

/* Warning button */
QPushButton[cssClass="warning"] {{
    background-color: {Colors.WARNING};
    color: #000000;
    border: none;
    font-weight: 600;
}}
QPushButton[cssClass="warning"]:hover {{
    background-color: {Colors.WARNING_HOVER};
}}

/* Small icon button */
QPushButton[cssClass="icon"] {{
    padding: 6px;
    min-width: 32px;
    max-width: 32px;
    min-height: 32px;
    max-height: 32px;
    border-radius: 8px;
    font-size: 14px;
}}

/* ===== QLabel ===== */
QLabel {{
    color: {Colors.TEXT_PRIMARY};
    background: transparent;
}}

/* ===== QFrame (Card) ===== */
QFrame[cssClass="card"] {{
    background-color: {Colors.BG_CARD};
    border: 1px solid {Colors.BORDER};
    border-radius: 12px;
    padding: 0px;
}}

/* ===== QSpinBox ===== */
QSpinBox {{
    background-color: {Colors.BG_INPUT};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 8px;
    padding: 6px 8px;
    font-family: {Fonts.MONO};
    font-size: {Fonts.SIZE_BODY}px;
    selection-background-color: {Colors.PRIMARY};
}}
QSpinBox:focus {{
    border-color: {Colors.PRIMARY};
}}
QSpinBox:disabled {{
    background-color: {Colors.BG_CARD};
    color: {Colors.TEXT_TERTIARY};
    border-color: {Colors.BORDER};
}}
QSpinBox::up-button, QSpinBox::down-button {{
    width: 20px;
    border: none;
    background: transparent;
}}
QSpinBox::up-arrow {{
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-bottom: 5px solid {Colors.TEXT_SECONDARY};
    width: 0; height: 0;
}}
QSpinBox::down-arrow {{
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid {Colors.TEXT_SECONDARY};
    width: 0; height: 0;
}}

/* ===== QComboBox ===== */
QComboBox {{
    background-color: {Colors.BG_INPUT};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 8px;
    padding: 6px 12px;
    font-size: {Fonts.SIZE_BODY}px;
}}
QComboBox:focus {{
    border-color: {Colors.PRIMARY};
}}
QComboBox::drop-down {{
    border: none;
    width: 24px;
}}
QComboBox::down-arrow {{
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid {Colors.TEXT_SECONDARY};
    width: 0; height: 0;
}}
QComboBox QAbstractItemView {{
    background-color: {Colors.BG_TOOLTIP};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 8px;
    selection-background-color: {Colors.BG_INPUT};
    outline: none;
    padding: 4px;
}}

/* ===== QCheckBox ===== */
QCheckBox {{
    color: {Colors.TEXT_PRIMARY};
    spacing: 8px;
    font-size: {Fonts.SIZE_BODY}px;
}}
QCheckBox::indicator {{
    width: 18px;
    height: 18px;
    border: 1px solid {Colors.BORDER_BUTTON};
    border-radius: 4px;
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
    border-radius: 12px;
    margin-top: 16px;
    padding: 16px 12px 12px 12px;
    font-size: {Fonts.SIZE_HEADING}px;
    font-weight: 600;
    color: {Colors.TEXT_PRIMARY};
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    subcontrol-position: top left;
    padding: 0 8px;
    left: 12px;
    color: {Colors.TEXT_SECONDARY};
    font-size: {Fonts.SIZE_SMALL}px;
    font-weight: 600;
    text-transform: uppercase;
}}

/* ===== QStatusBar ===== */
QStatusBar {{
    background-color: {Colors.BG_SIDEBAR};
    color: {Colors.TEXT_SECONDARY};
    border-top: 1px solid {Colors.BORDER};
    font-size: {Fonts.SIZE_SMALL}px;
    padding: 4px 12px;
}}

/* ===== QSplitter ===== */
QSplitter::handle {{
    background-color: {Colors.BORDER};
    width: 1px;
}}
QSplitter::handle:hover {{
    background-color: {Colors.PRIMARY};
}}

/* ===== QMenu (context) ===== */
QMenu {{
    background-color: {Colors.BG_TOOLTIP};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 8px;
    padding: 4px;
}}
QMenu::item {{
    padding: 8px 16px;
    border-radius: 4px;
}}
QMenu::item:selected {{
    background-color: {Colors.BG_INPUT};
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

/* ===== QToolTip ===== */
QToolTip {{
    background-color: {Colors.BG_TOOLTIP};
    color: {Colors.TEXT_PRIMARY};
    border: 1px solid {Colors.BORDER};
    border-radius: 6px;
    padding: 6px 10px;
    font-size: {Fonts.SIZE_SMALL}px;
}}

/* ===== QMessageBox ===== */
QMessageBox {{
    background-color: {Colors.BG_APP};
}}
QMessageBox QLabel {{
    color: {Colors.TEXT_PRIMARY};
}}

/* ===== QFileDialog ===== */
QFileDialog {{
    background-color: {Colors.BG_APP};
}}
"""
