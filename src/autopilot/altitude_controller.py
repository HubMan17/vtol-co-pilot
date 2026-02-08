from src.core.config import AutopilotConfig


class AltitudeController:

    def __init__(self, config: AutopilotConfig):
        self._config = config
        self._target_altitude = 0.0
        self._current_error = 0.0

    def set_target_altitude(self, altitude: float):
        self._target_altitude = altitude

    def get_target_altitude(self) -> float:
        return self._target_altitude

    def update(self, current_altitude: float):
        """Update error tracking. GUIDED mode handles actual pitch control."""
        self._current_error = self._target_altitude - current_altitude

    def reset(self):
        self._current_error = 0.0

    def is_on_altitude(self, tolerance: float = 5.0) -> bool:
        return abs(self._current_error) <= tolerance

    def get_current_error(self) -> float:
        return self._current_error
