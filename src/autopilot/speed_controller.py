from src.core.config import AutopilotConfig
from src.autopilot.pid import PIDController


class SpeedController:
    PWM_MIN = 1000
    PWM_MAX = 2000
    PWM_CRUISE = 1300  # baseline throttle for level cruise at target airspeed

    def __init__(self, config: AutopilotConfig):
        self._config = config
        self._target_speed = getattr(config, 'target_airspeed', 20.0)
        self._current_error = 0.0
        self._enabled = True

        speed_pid = getattr(config, 'speed_pid', {'p': 50.0, 'i': 10.0, 'd': 5.0})
        # PID outputs offset from PWM_CRUISE (not raw PWM)
        self._pid = PIDController(
            kp=speed_pid['p'],
            ki=speed_pid['i'],
            kd=speed_pid['d'],
            output_min=self.PWM_MIN - self.PWM_CRUISE,   # -300
            output_max=self.PWM_MAX - self.PWM_CRUISE     # +700
        )
        self._pid.set_integral_limit(30)  # max I term = ki*30 = 300 PWM

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
        """Reset PID for clean start (target stays at configured airspeed)"""
        self._pid.reset()

    def update(self, current_airspeed: float, dt: float) -> int:
        if not self._enabled:
            return 0

        error = self._target_speed - current_airspeed
        self._current_error = error

        offset = self._pid.update(error, dt)
        throttle_pwm = int(self.PWM_CRUISE + offset)
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
