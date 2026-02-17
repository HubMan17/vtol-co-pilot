import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

# ── Logging setup (file + console, file cleared each run) ──
log_dir = Path(__file__).parent.parent / "logs"
log_dir.mkdir(exist_ok=True)
log_file = log_dir / "app.log"

logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s.%(msecs)03d %(levelname)s [%(name)s]: %(message)s',
    datefmt='%H:%M:%S',
    handlers=[
        logging.FileHandler(log_file, mode='w', encoding='utf-8'),
        logging.StreamHandler(),
    ]
)

from PyQt5.QtCore import Qt
from PyQt5.QtWidgets import QApplication
from src.gui.main_window import MainWindow
from src.core.config import load_config


def main():
    # High-DPI rendering: avoid OS bitmap upscaling blur on Windows.
    if hasattr(Qt, "AA_EnableHighDpiScaling"):
        QApplication.setAttribute(Qt.AA_EnableHighDpiScaling, True)
    if hasattr(Qt, "AA_UseHighDpiPixmaps"):
        QApplication.setAttribute(Qt.AA_UseHighDpiPixmaps, True)

    # Required for QQuickWidget (QML Map) to share OpenGL context
    QApplication.setAttribute(Qt.AA_ShareOpenGLContexts, True)

    config = load_config()

    app = QApplication(sys.argv)
    app.setApplicationName("VTOL Co-Pilot")

    window = MainWindow(config)
    window.show()

    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
