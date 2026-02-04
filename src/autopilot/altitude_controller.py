from src.core.config import AutopilotConfig
from src.autopilot.pid import PIDController


class AltitudeController:
    PWM_CENTER = 1500
    PWM_RANGE = 500
    MAX_STICK_DEG = 30.0
    DEADBAND_METERS = 3.0  # Release control when within 3 meters

    def __init__(self, config: AutopilotConfig):
        self._config = config
        self._target_altitude = 0.0
        self._current_error = 0.0

        self._pitch_limit_up = getattr(config, 'pitch_limit_up', 12.0)
        self._pitch_limit_down = getattr(config, 'pitch_limit_down', 15.0)

        self._pid = PIDController(
            kp=config.altitude_pid['p'],
            ki=config.altitude_pid['i'],
            kd=config.altitude_pid['d'],
            output_min=-self._pitch_limit_down,
            output_max=self._pitch_limit_up
        )

    def set_target_altitude(self, altitude: float):
        self._target_altitude = altitude

    def get_target_altitude(self) -> float:
        return self._target_altitude

    def update(self, current_altitude: float, dt: float) -> int:
        error = self._target_altitude - current_altitude
        self._current_error = error

        # Release control when altitude reached - let aircraft maintain itself
        if abs(error) < self.DEADBAND_METERS:
            self._pid.reset()
            return self.PWM_CENTER  # Neutral - let aircraft autopilot maintain

        pitch_angle = self._pid.update(error, dt)

        pwm = self.PWM_CENTER + int(pitch_angle * self.PWM_RANGE / self.MAX_STICK_DEG)
        pwm = max(1000, min(2000, pwm))

        return pwm

    def reset(self):
        self._pid.reset()
        self._current_error = 0.0

    def is_on_altitude(self, tolerance: float = 10.0) -> bool:
        return abs(self._current_error) <= tolerance

    def get_current_error(self) -> float:
        return self._current_error

    def get_pid_state(self):
        return self._pid.get_state()
