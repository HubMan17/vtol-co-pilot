from src.core.config import AutopilotConfig
from src.navigation.calculations import heading_difference, normalize_heading


class HeadingController:

    def __init__(self, config: AutopilotConfig):
        self._config = config
        self._target_heading = 0.0
        self._current_error = 0.0

    def set_target_heading(self, heading: float):
        self._target_heading = normalize_heading(heading)

    def get_target_heading(self) -> float:
        return self._target_heading

    def update(self, current_heading: float):
        """Update error tracking. GUIDED mode handles actual flight control."""
        self._current_error = heading_difference(current_heading, self._target_heading)

    def get_turn_direction(self) -> int:
        """Return 1 for CW (right), -1 for CCW (left) based on shortest path."""
        return 1 if self._current_error >= 0 else -1

    def reset(self):
        self._current_error = 0.0

    def is_on_heading(self, tolerance: float = 5.0) -> bool:
        return abs(self._current_error) <= tolerance

    def get_current_error(self) -> float:
        return self._current_error
