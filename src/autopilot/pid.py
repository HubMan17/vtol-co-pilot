from dataclasses import dataclass
from typing import Optional


@dataclass
class PIDState:
    p_term: float = 0.0
    i_term: float = 0.0
    d_term: float = 0.0
    output: float = 0.0
    error: float = 0.0


class PIDController:
    def __init__(self, kp: float, ki: float, kd: float,
                 output_min: float = -1.0, output_max: float = 1.0,
                 integral_limit: Optional[float] = None):
        self._kp = kp
        self._ki = ki
        self._kd = kd
        self._output_min = output_min
        self._output_max = output_max
        self._integral_limit = integral_limit if integral_limit else abs(output_max)

        self._integral = 0.0
        self._last_error: Optional[float] = None
        self._state = PIDState()

    def update(self, error: float, dt: float) -> float:
        if dt <= 0:
            return self._state.output

        p_term = self._kp * error

        self._integral += error * dt
        self._integral = max(-self._integral_limit,
                            min(self._integral_limit, self._integral))
        i_term = self._ki * self._integral

        if self._last_error is not None:
            d_term = self._kd * (error - self._last_error) / dt
        else:
            d_term = 0.0
        self._last_error = error

        output = p_term + i_term + d_term
        output = max(self._output_min, min(self._output_max, output))

        self._state = PIDState(p_term, i_term, d_term, output, error)

        return output

    def reset(self):
        self._integral = 0.0
        self._last_error = None
        self._state = PIDState()

    def set_gains(self, kp: float = None, ki: float = None, kd: float = None):
        if kp is not None:
            self._kp = kp
        if ki is not None:
            self._ki = ki
        if kd is not None:
            self._kd = kd

    def get_state(self) -> PIDState:
        return self._state

    def set_output_limits(self, output_min: float, output_max: float):
        self._output_min = output_min
        self._output_max = output_max

    def set_integral_limit(self, limit: float):
        self._integral_limit = limit
