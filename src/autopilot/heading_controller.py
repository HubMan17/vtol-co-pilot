from src.core.config import AutopilotConfig
from src.autopilot.pid import PIDController
from src.navigation.calculations import heading_difference, normalize_heading


class HeadingController:
    PWM_CENTER = 1500
    PWM_RANGE = 500
    MAX_DEFLECTION_DEG = 45.0

    def __init__(self, config: AutopilotConfig):
        self._config = config
        self._target_heading = 0.0
        self._current_error = 0.0

        self._pid = PIDController(
            kp=config.heading_pid['p'],
            ki=config.heading_pid['i'],
            kd=config.heading_pid['d'],
            output_min=-config.bank_limit,
            output_max=config.bank_limit
        )

    def set_target_heading(self, heading: float):
        self._target_heading = normalize_heading(heading)

    def get_target_heading(self) -> float:
        return self._target_heading

    def update(self, current_heading: float, dt: float) -> int:
        error = heading_difference(current_heading, self._target_heading)
        self._current_error = error

        bank_angle = self._pid.update(error, dt)

        pwm = self.PWM_CENTER + int(bank_angle * self.PWM_RANGE / self.MAX_DEFLECTION_DEG)
        pwm = max(1000, min(2000, pwm))

        return pwm

    def reset(self):
        self._pid.reset()
        self._current_error = 0.0

    def is_on_heading(self, tolerance: float = 5.0) -> bool:
        return abs(self._current_error) <= tolerance

    def get_current_error(self) -> float:
        return self._current_error

    def get_pid_state(self):
        return self._pid.get_state()
