import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from PyQt5.QtWidgets import QApplication
from src.gui.main_window import MainWindow
from src.core.config import load_config


def main():
    config = load_config()

    app = QApplication(sys.argv)
    app.setApplicationName("VTOL Co-Pilot")

    window = MainWindow(config)
    window.show()

    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
