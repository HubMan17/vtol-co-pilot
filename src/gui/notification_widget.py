"""
Notification widget — toast-like card with title, message, action buttons,
and a countdown progress bar.  Designed for the right sidebar panel.
"""
import logging
from collections import deque
from dataclasses import dataclass, field
from enum import Enum
from typing import Callable, List, Optional, Tuple

from PyQt5.QtCore import Qt, QTimer, pyqtSignal, QObject
from PyQt5.QtWidgets import (
    QFrame, QVBoxLayout, QHBoxLayout, QLabel, QPushButton,
    QProgressBar, QWidget, QSizePolicy,
)

from src.gui.theme import Colors, Fonts

logger = logging.getLogger(__name__)


# ─── Data types ───────────────────────────────────────────────

class NotificationLevel(Enum):
    INFO = "info"
    WARNING = "warning"
    CRITICAL = "critical"


_LEVEL_COLORS = {
    NotificationLevel.INFO: Colors.SUCCESS,
    NotificationLevel.WARNING: Colors.WARNING,
    NotificationLevel.CRITICAL: Colors.ERROR,
}

_LEVEL_BG = {
    NotificationLevel.INFO: Colors.SUCCESS_BG,
    NotificationLevel.WARNING: Colors.WARNING_BG,
    NotificationLevel.CRITICAL: Colors.ERROR_BG,
}


@dataclass
class Notification:
    level: NotificationLevel
    title: str
    message: str
    duration: int = 60          # seconds
    actions: List[Tuple[str, Callable]] = field(default_factory=list)
    tag: str = ""               # for deduplication


# ─── Widget ───────────────────────────────────────────────────

class NotificationWidget(QFrame):
    """Single notification card shown in the sidebar."""

    notification_closed = pyqtSignal()

    _TICK_MS = 100

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self._current: Optional[Notification] = None
        self._elapsed_ms: int = 0
        self._duration_ms: int = 0

        self._timer = QTimer(self)
        self._timer.setInterval(self._TICK_MS)
        self._timer.timeout.connect(self._tick)

        self._action_buttons: List[QPushButton] = []

        self._setup_ui()
        self.hide()

    # ── UI ──

    def _setup_ui(self):
        self.setObjectName("notif_card")
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # Inner frame with left-border accent
        self._inner = QFrame()
        self._inner.setObjectName("notif_inner")
        inner_lay = QVBoxLayout(self._inner)
        inner_lay.setContentsMargins(12, 10, 12, 6)
        inner_lay.setSpacing(4)

        # Title
        self._lbl_title = QLabel()
        self._lbl_title.setWordWrap(True)
        self._lbl_title.setStyleSheet(f"""
            font-family: "{Fonts.FAMILY}";
            font-size: 13px;
            font-weight: 700;
            border: none;
            background: transparent;
        """)
        inner_lay.addWidget(self._lbl_title)

        # Message
        self._lbl_message = QLabel()
        self._lbl_message.setWordWrap(True)
        self._lbl_message.setStyleSheet(f"""
            color: {Colors.TEXT_SECONDARY};
            font-size: 12px;
            border: none;
            background: transparent;
        """)
        inner_lay.addWidget(self._lbl_message)

        # Action buttons row
        self._btn_container = QWidget()
        self._btn_container.setStyleSheet("background: transparent;")
        self._btn_lay = QHBoxLayout(self._btn_container)
        self._btn_lay.setContentsMargins(0, 4, 0, 2)
        self._btn_lay.setSpacing(6)
        self._btn_lay.addStretch()
        inner_lay.addWidget(self._btn_container)
        self._btn_container.hide()

        root.addWidget(self._inner)

        # Progress bar (bottom edge)
        self._progress = QProgressBar()
        self._progress.setTextVisible(False)
        self._progress.setFixedHeight(3)
        self._progress.setRange(0, 1000)
        self._progress.setValue(1000)
        self._progress.setStyleSheet(f"""
            QProgressBar {{
                background-color: {Colors.BG_INPUT};
                border: none;
                border-radius: 0px;
            }}
            QProgressBar::chunk {{
                background-color: {Colors.PRIMARY};
                border-radius: 0px;
            }}
        """)
        root.addWidget(self._progress)

    # ── Public API ──

    def show_notification(self, notification: Notification):
        self._stop()
        self._current = notification
        self._elapsed_ms = 0
        self._duration_ms = notification.duration * 1000

        color = _LEVEL_COLORS[notification.level]
        bg = _LEVEL_BG[notification.level]

        # Card style
        self._inner.setStyleSheet(f"""
            #notif_inner {{
                background-color: {bg};
                border: 1px solid {Colors.BORDER};
                border-left: 3px solid {color};
                border-radius: 8px;
            }}
        """)

        # Title color
        self._lbl_title.setText(notification.title)
        self._lbl_title.setStyleSheet(f"""
            color: {color};
            font-family: "{Fonts.FAMILY}";
            font-size: 13px;
            font-weight: 700;
            border: none;
            background: transparent;
        """)

        # Message
        self._lbl_message.setText(notification.message)

        # Progress bar color
        self._progress.setStyleSheet(f"""
            QProgressBar {{
                background-color: {Colors.BG_INPUT};
                border: none;
                border-radius: 0px;
            }}
            QProgressBar::chunk {{
                background-color: {color};
                border-radius: 0px;
            }}
        """)
        self._progress.setValue(1000)

        # Action buttons
        self._clear_buttons()
        if notification.actions:
            for label, callback in notification.actions:
                btn = QPushButton(label)
                btn.setCursor(Qt.PointingHandCursor)
                btn.setFixedHeight(28)
                btn.setStyleSheet(f"""
                    QPushButton {{
                        background-color: {Colors.BG_INPUT};
                        color: {Colors.TEXT_SECONDARY};
                        border: 1px solid {Colors.BORDER};
                        border-radius: 6px;
                        padding: 0 12px;
                        font-size: 11px;
                        font-weight: 600;
                    }}
                    QPushButton:hover {{
                        background-color: {Colors.BG_HOVER};
                        color: {Colors.TEXT_PRIMARY};
                        border-color: {Colors.BORDER_LIGHT};
                    }}
                """)
                # capture label/callback in closure
                btn.clicked.connect(self._make_action_handler(label, callback))
                self._btn_lay.insertWidget(self._btn_lay.count() - 1, btn)
                self._action_buttons.append(btn)
            self._btn_container.show()
        else:
            self._btn_container.hide()

        self.show()
        self._timer.start()

        logger.info("NOTIFICATION: [%s] %s — %s",
                     notification.level.value, notification.title, notification.message)

    def dismiss(self):
        if self._current:
            title = self._current.title
            self._stop()
            self._current = None
            self.hide()
            logger.debug("NOTIFICATION: dismissed '%s'", title)
            self.notification_closed.emit()

    def is_showing(self) -> bool:
        return self._current is not None

    # ── Private ──

    def _tick(self):
        self._elapsed_ms += self._TICK_MS
        remaining = max(0, self._duration_ms - self._elapsed_ms)
        self._progress.setValue(int(remaining * 1000 / self._duration_ms) if self._duration_ms else 0)
        if remaining <= 0:
            title = self._current.title if self._current else "?"
            self._stop()
            self._current = None
            self.hide()
            logger.debug("NOTIFICATION: auto-closed '%s'", title)
            self.notification_closed.emit()

    def _stop(self):
        self._timer.stop()
        self._clear_buttons()

    def _clear_buttons(self):
        for btn in self._action_buttons:
            self._btn_lay.removeWidget(btn)
            btn.deleteLater()
        self._action_buttons.clear()

    def _make_action_handler(self, label: str, callback: Callable):
        def handler():
            title = self._current.title if self._current else "?"
            logger.info("NOTIFICATION: action '%s' on '%s'", label, title)
            callback()
            self.dismiss()
        return handler


# ─── Manager (queue) ──────────────────────────────────────────

class NotificationManager(QObject):
    """Priority queue that feeds notifications one-by-one into the widget."""

    _PRIORITY = {
        NotificationLevel.CRITICAL: 0,
        NotificationLevel.WARNING: 1,
        NotificationLevel.INFO: 2,
    }

    def __init__(self, widget: NotificationWidget, parent: QObject = None):
        super().__init__(parent)
        self._widget = widget
        self._queue: deque[Notification] = deque()
        self._tags: dict[str, Notification] = {}
        widget.notification_closed.connect(self._show_next)

    def push(self, notification: Notification):
        """Add a notification. Shows immediately if nothing is displayed."""
        self._insert_by_priority(notification)
        if notification.tag:
            self._tags[notification.tag] = notification
        logger.debug("NOTIFICATION_QUEUE: push '%s' [%s], queue_size=%d",
                      notification.title, notification.level.value, len(self._queue))
        if not self._widget.is_showing():
            self._show_next()

    def push_or_replace(self, notification: Notification, tag: str):
        """Replace an existing notification with the same tag, or add new."""
        notification.tag = tag
        if tag in self._tags:
            old = self._tags[tag]
            # Remove old from queue if still queued
            try:
                self._queue.remove(old)
            except ValueError:
                pass
            # If old is currently showing — dismiss and show replacement
            if (self._widget.is_showing()
                    and self._widget._current is old):
                self._widget.dismiss()
        self.push(notification)

    def clear(self):
        """Clear the queue and dismiss current notification."""
        self._queue.clear()
        self._tags.clear()
        self._widget.dismiss()
        logger.info("NOTIFICATION_QUEUE: cleared")

    # ── Private ──

    def _insert_by_priority(self, notification: Notification):
        prio = self._PRIORITY[notification.level]
        # Find insertion point — after all items with same or higher priority
        for i, existing in enumerate(self._queue):
            if self._PRIORITY[existing.level] > prio:
                self._queue.insert(i, notification)
                return
        self._queue.append(notification)

    def _show_next(self):
        if self._queue:
            notif = self._queue.popleft()
            if notif.tag and notif.tag in self._tags:
                del self._tags[notif.tag]
            logger.debug("NOTIFICATION_QUEUE: showing next, remaining=%d", len(self._queue))
            self._widget.show_notification(notif)
        else:
            self._widget.hide()
