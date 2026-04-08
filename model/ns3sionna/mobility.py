import bisect
import numpy as np
from abc import ABC, abstractmethod

"""
    All mobility models defined here

    author: Zubow
"""


class AbstractMobility(ABC):
    """
    Base class for all mobility models.

    author: Zubow
    """

    def __init__(self, node_id, pos, collect_history=True):
        self.node_id = node_id
        self.pos = np.array(pos, dtype=float)
        self.velocity = np.array([0.0, 0.0, 0.0], dtype=float)
        self.last_update = 0.0
        self.hit_wall = False

        self.collect_history = collect_history
        if self.collect_history:
            self.history_times = []
            self.pos_history = {}
            self.velo_history = {}

    def _record_state(self, sim_time, pos, velocity):
        if not self.collect_history:
            return

        pos = np.array(pos, dtype=float)
        velocity = np.array(velocity, dtype=float)

        if self.history_times and abs(self.history_times[-1] - sim_time) < 1e-9:
            ts = self.history_times[-1]
            self.pos_history[ts] = pos
            self.velo_history[ts] = velocity
            return

        self.history_times.append(sim_time)
        self.pos_history[sim_time] = pos
        self.velo_history[sim_time] = velocity

    def update_pos(self, sim_time, pos, velocity, hit_wall):
        assert sim_time >= self.last_update
        self.last_update = sim_time
        self.pos = np.array(pos, dtype=float)
        self.velocity = np.array(velocity, dtype=float)
        self.hit_wall = hit_wall
        self._record_state(sim_time, self.pos, self.velocity)

    def _state_at(self, req_time):
        if not self.collect_history:
            raise RuntimeError("History collection disabled")
        if req_time > self.last_update + 1e-9:
            raise KeyError(f"No mobility history for future time {req_time}")

        idx = bisect.bisect_right(self.history_times, req_time) - 1
        if idx < 0:
            raise KeyError(f"No mobility history for time {req_time}")

        ts = self.history_times[idx]
        pos = self.pos_history[ts]
        velocity = self.velo_history[ts]
        return ts, pos, velocity

    @abstractmethod
    def get_pos_at(self, req_time):
        pass

    @abstractmethod
    def get_velo_at(self, req_time):
        pass

    @abstractmethod
    def get_next_direction_angle(self):
        pass

    @abstractmethod
    def check_set_new_velocity(self, sim_time, distance):
        pass

    def print(self):
        print(f"Mobility:: Id: {self.node_id}, t: {self.last_update}, pos: {self.pos}, velocity: {self.velocity}")


class ConstantMobility(AbstractMobility):
    """
    Static placement of a node. The position is set from ns-3.
    """

    def __init__(self, node_id, pos, collect_history=True):
        super(ConstantMobility, self).__init__(node_id, pos, collect_history)
        self.update_pos(0.0, self.pos.copy(), self.velocity.copy(), False)

    def get_next_direction_angle(self):
        return 0.0

    def check_set_new_velocity(self, sim_time, distance):
        pass

    def get_pos_at(self, req_time):
        return self.pos

    def get_velo_at(self, req_time):
        return self.velocity


class RandomWalkMobility(AbstractMobility):
    # modes:
    MODE_WALL = 1
    MODE_TIME = 2
    MODE_DISTANCE = 3

    # speed:
    SPEED_UNIFORM = 1
    SPEED_CONSTANT = 2
    SPEED_NORMAL = 3

    # direction:
    DIRECTION_UNIFORM = 1
    DIRECTION_CONSTANT = 2
    DIRECTION_NORMAL = 3

    """
    Random walk mobility driven by a per-node RNG and event history.
    """

    def __init__(
        self,
        node_id,
        pos,
        mode,
        mode_params,
        speed,
        speed_params,
        direction,
        direction_params,
        rng=None,
        collect_history=True,
    ):
        super(RandomWalkMobility, self).__init__(node_id, pos, collect_history)
        self.mode = mode
        self.mode_params = mode_params
        self.speed = speed
        self.speed_params = speed_params
        self.direction = direction
        self.direction_params = direction_params
        self.rng = rng if rng is not None else np.random.default_rng()

        if self.mode == RandomWalkMobility.MODE_TIME:
            self.time_period = self.mode_params
        elif self.mode == RandomWalkMobility.MODE_DISTANCE:
            self.distance_period = self.mode_params

        self.last_update_velocity = 0.0
        self.segment_distance = 0.0

        self._set_new_velocity()
        self.update_pos(0.0, self.pos.copy(), self.velocity.copy(), False)

    def get_pos_at(self, req_time):
        ts, pos, velocity = self._state_at(req_time)
        dt = max(0.0, req_time - ts)
        return pos + velocity * (dt / 1e9)

    def get_velo_at(self, req_time):
        _, _, velocity = self._state_at(req_time)
        return velocity

    def _set_new_velocity(self, ts=0.0):
        speed = self._get_next_speed()
        direction_rad = self.get_next_direction_angle()
        self.velocity = np.array([np.cos(direction_rad) * speed, np.sin(direction_rad) * speed, 0.0], dtype=float)
        self.last_update_velocity = ts
        self.segment_distance = 0.0

    def register_travel(self, distance):
        if self.mode == RandomWalkMobility.MODE_DISTANCE:
            self.segment_distance += distance

    def time_to_next_velocity_change(self):
        if self.mode == RandomWalkMobility.MODE_WALL:
            return np.inf

        if self.mode == RandomWalkMobility.MODE_TIME:
            return max(0.0, self.last_update_velocity + self.time_period - self.last_update)

        speed = np.linalg.norm(self.velocity)
        if speed == 0.0:
            return np.inf

        remaining_distance = max(0.0, self.distance_period - self.segment_distance)
        return (remaining_distance / speed) * 1e9

    def resample_velocity(self, sim_time):
        self._set_new_velocity(sim_time)
        self.update_pos(sim_time, self.pos.copy(), self.velocity.copy(), False)

    def check_set_new_velocity(self, sim_time, distance):
        self.register_travel(distance)

        if self.mode == RandomWalkMobility.MODE_WALL:
            return

        if self.mode == RandomWalkMobility.MODE_TIME:
            if sim_time - self.last_update_velocity >= self.time_period:
                self._set_new_velocity(sim_time)
        elif self.mode == RandomWalkMobility.MODE_DISTANCE:
            if self.segment_distance >= self.distance_period:
                self._set_new_velocity(sim_time)

    def _get_next_speed(self):
        if self.speed == RandomWalkMobility.SPEED_UNIFORM:
            return self.rng.uniform(self.speed_params[0], self.speed_params[1])
        if self.speed == RandomWalkMobility.SPEED_CONSTANT:
            return self.speed_params[0]
        if self.speed == RandomWalkMobility.SPEED_NORMAL:
            return self.rng.normal(self.speed_params[0], self.speed_params[1])
        raise ValueError(f"Unsupported speed model {self.speed}")

    def get_next_direction_angle(self):
        if self.direction == RandomWalkMobility.DIRECTION_UNIFORM:
            return self.rng.uniform(self.direction_params[0], self.direction_params[1])
        if self.direction == RandomWalkMobility.DIRECTION_CONSTANT:
            return self.direction_params[0]
        if self.direction == RandomWalkMobility.DIRECTION_NORMAL:
            return self.rng.normal(self.direction_params[0], self.direction_params[1])
        raise ValueError(f"Unsupported direction model {self.direction}")
