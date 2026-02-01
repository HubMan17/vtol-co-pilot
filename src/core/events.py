from enum import Enum, auto
from typing import Callable, Dict, List, Any


class Event(Enum):
    TELEMETRY_UPDATE = auto()
    POSITION_UPDATE = auto()
    WAYPOINT_REACHED = auto()
    MODE_CHANGE = auto()
    AUTOPILOT_ENGAGE = auto()
    AUTOPILOT_DISENGAGE = auto()
    CONNECTION_LOST = auto()
    CONNECTION_RESTORED = auto()
    RC_OVERRIDE_SENT = auto()


class EventBus:
    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._subscribers: Dict[Event, List[Callable]] = {}
        return cls._instance

    def subscribe(self, event: Event, callback: Callable):
        if event not in self._subscribers:
            self._subscribers[event] = []
        self._subscribers[event].append(callback)

    def unsubscribe(self, event: Event, callback: Callable):
        if event in self._subscribers:
            self._subscribers[event].remove(callback)

    def emit(self, event: Event, data: Any = None):
        if event in self._subscribers:
            for callback in self._subscribers[event]:
                callback(data)

    def clear(self):
        self._subscribers.clear()
