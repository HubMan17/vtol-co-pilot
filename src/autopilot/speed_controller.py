from src.core.config import AutopilotConfig
from src.autopilot.pid import PIDController


class SpeedController:
    PWM_MIN = 1000
    PWM_MAX = 2000
    PWM_IDLE = 1100

    def __init__(self, config: AutopilotConfig):
        self._config = config
        self._target_speed = getattr(config, 'target_airspeed', 20.0)
        self._current_error = 0.0
        self._enabled = True

        speed_pid = getattr(config, 'speed_pid', {'p': 50.0, 'i': 10.0, 'd': 5.0})
        self._pid = PIDController(
            kp=speed_pid['p'],
            ki=speed_pid['i'],
            kd=speed_pid['d'],
            output_min=self.PWM_MIN,
            output_max=self.PWM_MAX
        )

    def set_target_speed(self, speed: float):
        self._target_speed = max(15.0, min(35.0, speed))

    def get_target_speed(self) -> float:
        return self._target_speed

    def set_enabled(self, enabled: bool):
        self._enabled = enabled
        if not enabled:
            self._pid.reset()

    def is_enabled(self) -> bool:
        return self._enabled

    def initialize_from_current(self, current_airspeed: float):
        """Initialize controller with current airspeed to avoid sudden throttle changes"""
        if current_airspeed > self._target_speed + 2.0:
            self._target_speed = current_airspeed
        self._pid.reset()

    def update(self, current_airspeed: float, dt: float) -> int:
        if not self._enabled:
            return 0

        error = self._target_speed - current_airspeed
        self._current_error = error

        # Gradually reduce target speed to 20 m/s if above
        if self._target_speed > 20.0:
            self._target_speed = max(20.0, self._target_speed - 0.5 * dt)

        throttle_pwm = int(self._pid.update(error, dt))
        throttle_pwm = max(self.PWM_MIN, min(self.PWM_MAX, throttle_pwm))

        return throttle_pwm

    def reset(self):
        self._pid.reset()
        self._current_error = 0.0

    def is_on_speed(self, tolerance: float = 2.0) -> bool:
        return abs(self._current_error) <= tolerance

    def get_current_error(self) -> float:
        return self._current_error

    def get_pid_state(self):
        return self._pid.get_state()
