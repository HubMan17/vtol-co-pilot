from src.core.config import AutopilotConfig


class SpeedController:

    def __init__(self, config: AutopilotConfig):
        self._config = config
        self._target_speed = getattr(config, 'target_airspeed', 20.0)
        self._current_error = 0.0

    def set_target_speed(self, speed: float):
        self._target_speed = max(15.0, min(35.0, speed))

    def get_target_speed(self) -> float:
        return self._target_speed

    def update(self, current_airspeed: float):
        """Update error tracking. GUIDED mode handles actual throttle control."""
        self._current_error = self._target_speed - current_airspeed

    def reset(self):
        self._current_error = 0.0

    def is_on_speed(self, tolerance: float = 2.0) -> bool:
        return abs(self._current_error) <= tolerance

    def get_current_error(self) -> float:
        return self._current_error
