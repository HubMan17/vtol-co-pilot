#include "Theme.h"

namespace vtol::theme {

QString stylesheet()
{
    return QStringLiteral(R"QSS(
/* ===== Global — force dark on everything ===== */
* {
    background-color: transparent;
    color: #FFFFFF;
}
QMainWindow {
    background-color: #0B0F1A;
}
QWidget {
    font-family: "Segoe UI";
    font-size: 13px;
    color: #FFFFFF;
    background-color: transparent;
}

/* ===== Scrollbars ===== */
QScrollBar:vertical {
    background: transparent; width: 5px; margin: 0;
}
QScrollBar::handle:vertical {
    background: #334155; border-radius: 2px; min-height: 24px;
}
QScrollBar::handle:vertical:hover { background: #CBD5E1; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QScrollBar:horizontal {
    background: transparent; height: 5px; margin: 0;
}
QScrollBar::handle:horizontal {
    background: #334155; border-radius: 2px; min-width: 24px;
}
QScrollBar::handle:horizontal:hover { background: #CBD5E1; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }

/* ===== QPushButton ===== */
QPushButton {
    background-color: #1A2035; color: #E2E8F0;
    border: 1px solid #1E293B; border-radius: 6px;
    padding: 6px 14px; font-weight: 500; font-size: 12px;
}
QPushButton:hover { background-color: #1E293B; color: #FFFFFF; border-color: #334155; }
QPushButton:pressed { background-color: #334155; }
QPushButton:disabled { background-color: #111827; color: #94A3B8; border-color: #1A2332; }
QPushButton:checked { background-color: #2563EB; color: #ffffff; border-color: #2563EB; }
QPushButton:checked:hover { background-color: #3B82F6; }

/* ===== QSpinBox / QDoubleSpinBox ===== */
QSpinBox, QDoubleSpinBox {
    background-color: #1A2035; color: #FFFFFF;
    border: 1px solid #1E293B; border-radius: 6px;
    padding: 4px 6px; font-family: "Consolas"; font-size: 12px;
    selection-background-color: #2563EB;
}
QSpinBox:focus, QDoubleSpinBox:focus { border-color: #2563EB; }
QSpinBox:disabled, QDoubleSpinBox:disabled { background-color: #111827; color: #94A3B8; border-color: #1A2332; }
QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 16px; border: none; background: transparent; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    image: none; border-left: 3px solid transparent;
    border-right: 3px solid transparent; border-bottom: 4px solid #CBD5E1; width: 0; height: 0;
}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    image: none; border-left: 3px solid transparent;
    border-right: 3px solid transparent; border-top: 4px solid #CBD5E1; width: 0; height: 0;
}

/* ===== QLineEdit / QTextEdit / QPlainTextEdit ===== */
QLineEdit, QTextEdit, QPlainTextEdit {
    background-color: #1A2035; color: #FFFFFF;
    border: 1px solid #1E293B; border-radius: 6px;
    padding: 4px 8px; selection-background-color: #2563EB;
}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus { border-color: #2563EB; }

/* ===== QComboBox ===== */
QComboBox {
    background-color: #1A2035; color: #FFFFFF;
    border: 1px solid #1E293B; border-radius: 6px;
    padding: 5px 10px; font-size: 12px;
}
QComboBox:focus { border-color: #2563EB; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox::down-arrow {
    image: none; border-left: 3px solid transparent;
    border-right: 3px solid transparent; border-top: 4px solid #CBD5E1; width: 0; height: 0;
}
QComboBox QAbstractItemView {
    background-color: #111827; color: #FFFFFF;
    border: 1px solid #1E293B; selection-background-color: #1E293B;
    outline: none; padding: 2px;
}

/* ===== QCheckBox ===== */
QCheckBox { color: #FFFFFF; spacing: 6px; font-size: 12px; }
QCheckBox::indicator {
    width: 16px; height: 16px;
    border: 1px solid #334155; border-radius: 3px;
    background-color: #1A2035;
}
QCheckBox::indicator:checked { background-color: #2563EB; border-color: #2563EB; }

/* ===== QGroupBox ===== */
QGroupBox {
    background-color: #111827; border: 1px solid #1E293B; border-radius: 8px;
    margin-top: 14px; padding: 14px 10px 10px 10px; font-weight: 600; color: #FFFFFF;
}
QGroupBox::title {
    subcontrol-origin: margin; subcontrol-position: top left;
    padding: 0 6px; left: 10px; color: #CBD5E1; font-size: 10px; font-weight: 600;
}

/* ===== QLabel / QFrame ===== */
QLabel { background: transparent; border: none; }
QFrame { border: none; }

/* ===== QStatusBar ===== */
QStatusBar {
    background-color: #0F1320; color: #CBD5E1;
    border-top: 1px solid #1A2332; font-size: 11px; padding: 2px 10px;
}
QStatusBar QLabel { color: #CBD5E1; }

/* ===== QSplitter ===== */
QSplitter::handle { background-color: #1E293B; }
QSplitter::handle:hover { background-color: #2563EB; }
QSplitter::handle:pressed { background-color: #3B82F6; }

/* ===== QMenu ===== */
QMenu {
    background-color: #111827; color: #FFFFFF;
    border: 1px solid #1E293B; border-radius: 8px; padding: 4px;
}
QMenu::item { padding: 6px 14px; border-radius: 4px; }
QMenu::item:selected { background-color: #1E293B; }
QMenu::separator { height: 1px; background: #1E293B; margin: 4px 8px; }

/* ===== QDialog / QMessageBox / QFileDialog ===== */
QDialog { background-color: #0B0F1A; color: #FFFFFF; }
QMessageBox { background-color: #0B0F1A; color: #FFFFFF; }
QMessageBox QLabel { color: #FFFFFF; }
QFileDialog { background-color: #0B0F1A; color: #FFFFFF; }

/* ===== QToolTip ===== */
QToolTip {
    background-color: #111827; color: #FFFFFF;
    border: 1px solid #1E293B; border-radius: 4px;
    padding: 4px 8px; font-size: 11px;
}

/* ===== QScrollArea ===== */
QScrollArea { background: transparent; border: none; }

/* ===== QTabWidget / QTabBar ===== */
QTabWidget::pane { background-color: #0B0F1A; border: 1px solid #1E293B; }
QTabBar::tab {
    background-color: #111827; color: #E2E8F0;
    border: 1px solid #1E293B; padding: 6px 14px;
}
QTabBar::tab:selected { background-color: #0B0F1A; color: #FFFFFF; border-bottom-color: #0B0F1A; }

/* ===== QHeaderView ===== */
QHeaderView::section {
    background-color: #111827; color: #E2E8F0;
    border: 1px solid #1E293B; padding: 4px 8px; font-weight: 600;
}

/* ===== QTableView / QTreeView / QListView ===== */
QTableView, QTreeView, QListView {
    background-color: #0B0F1A; color: #FFFFFF;
    border: 1px solid #1E293B; selection-background-color: #1E293B;
    gridline-color: #1E293B; alternate-background-color: #111827;
}

/* ===== QProgressBar ===== */
QProgressBar {
    background-color: #1A2035; border: 1px solid #1E293B;
    border-radius: 4px; text-align: center; color: #FFFFFF;
}
QProgressBar::chunk { background-color: #2563EB; border-radius: 3px; }
)QSS");
}

} // namespace vtol::theme
