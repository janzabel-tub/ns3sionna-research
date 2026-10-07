import numpy as np
from abc import ABC, abstractmethod
from copy import copy as _copy, deepcopy as _deepcopy
from numbers import Integral

'''
    All mobility models defined here

    author: Zubow
'''
class AbstractMobility(ABC):
    '''
    Base class for all mobile models

    author: Zubow
    '''
    def __init__(self, node_id, pos, collect_history=True, history_limit=1024):
        if isinstance(history_limit, bool) or not isinstance(history_limit, Integral) or history_limit < 0:
            raise ValueError('history_limit must be a nonnegative integer')
        self.node_id = node_id
        self.pos = np.array(pos, dtype=np.float64, copy=True)
        self.velocity = np.array([0.0, 0.0, 0.0])
        self.last_update = 0

        self.collect_history = collect_history
        self.history_limit = int(history_limit)
        self._reset_history()

    def _reset_history(self):
        self.pos_history = {} # time -> independently owned position
        self.velo_history = {} # time -> independently owned velocity
        self._history_allocations = 0
        self._history_reuses = 0
        self._history_overwrites = 0
        self._history_evictions = 0

    def _record_history(self, sim_time):
        """Keep paired float64 vectors; after filling the cap, recycle the oldest pair."""
        if not self.collect_history or self.history_limit == 0:
            return
        if sim_time in self.pos_history:
            saved_pos = self.pos_history[sim_time]
            saved_velocity = self.velo_history[sim_time]
            self._history_overwrites += 1
        elif len(self.pos_history) >= self.history_limit:
            oldest = next(iter(self.pos_history))
            saved_pos = self.pos_history.pop(oldest)
            saved_velocity = self.velo_history.pop(oldest)
            self.pos_history[sim_time] = saved_pos
            self.velo_history[sim_time] = saved_velocity
            self._history_reuses += 1
            self._history_evictions += 1
        else:
            saved_pos = np.empty(3, dtype=np.float64)
            saved_velocity = np.empty(3, dtype=np.float64)
            self.pos_history[sim_time] = saved_pos
            self.velo_history[sim_time] = saved_velocity
            self._history_allocations += 1
        np.copyto(saved_pos, self.pos)
        np.copyto(saved_velocity, self.velocity)

    def history_statistics(self):
        """JSON-ready storage counters; allocations/reuses count vector pairs.

        Bytes cover retained NumPy vector data only, excluding dictionary overhead.
        """
        return {
            'enabled': bool(self.collect_history and self.history_limit),
            'limit': self.history_limit,
            'entries': len(self.pos_history),
            'bytes': sum(value.nbytes for value in self.pos_history.values())
                     + sum(value.nbytes for value in self.velo_history.values()),
            'allocations': self._history_allocations,
            'reuses': self._history_reuses,
            'overwrites': self._history_overwrites,
            'evictions': self._history_evictions,
        }

    def clone_for_prediction(self):
        """Copy mutable motion state without copying retained history.

        A missing local RNG intentionally keeps the legacy global NumPy RNG;
        the prediction caller must save and restore that global state.
        """
        clone = _copy(self)
        clone.pos = self.pos.copy()
        clone.velocity = self.velocity.copy()
        memo = {}
        for name in ('mode_params', 'time_period', 'distance_period',
                     'speed_params', 'direction_params', 'rng'):
            if hasattr(self, name):
                setattr(clone, name, _deepcopy(getattr(self, name), memo))
        clone.collect_history = False
        clone.history_limit = 0
        clone._reset_history()
        return clone


    def update_pos(self, sim_time, pos, velocity, hit_wall):
        assert sim_time >= self.last_update
        self.last_update = sim_time
        self.pos = np.array(pos, dtype=np.float64, copy=True)
        self.velocity = np.array(velocity, dtype=np.float64, copy=True)
        self.hit_wall = hit_wall

        self._record_history(sim_time)


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
    def check_set_new_velocity(self, sim_time, distance, allow_change=True):
        pass


    def print(self):
        print(f'Mobility:: Id: {self.node_id}, t: {self.last_update}, pos: {self.pos}, velocity: {self.velocity}')


class ConstantMobility(AbstractMobility):
    '''
    Just static placement of nodes. The position is set from ns3.
    '''
    def __init__(self, node_id, pos, collect_history=True, history_limit=1024):
        super(ConstantMobility, self).__init__(node_id, pos, collect_history, history_limit)
        self.update_pos(0, self.pos.copy(), self.velocity.copy(), False)


    def get_next_direction_angle(self):
        return 0.0


    def check_set_new_velocity(self, sim_time, distance, allow_change=True):
        pass


    def get_pos_at(self, req_time):
        return self.pos.copy()


    def get_velo_at(self, req_time):
        return self.velocity.copy()


class RandomWalkMobility(AbstractMobility):
    # modes:
    MODE_WALL = 1 # constant speed & duration until hitting the wall
    MODE_TIME = 2
    MODE_DISTANCE = 3

    # speed:
    SPEED_UNIFORM = 1
    SPEED_CONSTANT = 2
    SPEED_NORMAL = 3

    # direction
    DIRECTION_UNIFORM = 1
    DIRECTION_CONSTANT = 2
    DIRECTION_NORMAL = 3

    '''
    A random walk mobility model with different modes and configurations.
    '''
    def __init__(self, node_id, pos, mode, mode_params, speed, speed_params, direction, direction_params, rng=None, collect_history=True, history_limit=1024):
        super(RandomWalkMobility, self).__init__(node_id, pos, collect_history, history_limit)
        self.mode = mode
        self.mode_params = mode_params
        self.speed = speed
        self.speed_params = speed_params
        self.direction = direction
        self.direction_params = direction_params
        self.rng = rng

        if self.mode == RandomWalkMobility.MODE_WALL:
            pass
        elif self.mode == RandomWalkMobility.MODE_TIME:
            self.time_period = self.mode_params
        elif self.mode == RandomWalkMobility.MODE_DISTANCE:
            self.distance_period = self.mode_params

        # init with some speed and direction
        self._set_new_velocity()
        self.update_pos(0, self.pos.copy(), self.velocity.copy(), False)


    def get_pos_at(self, req_time):
        if req_time == self.last_update:
            return self.pos.copy()
        return self.pos_history[req_time].copy()


    def get_velo_at(self, req_time):
        if req_time == self.last_update:
            return self.velocity.copy()
        return self.velo_history[req_time].copy()


    def _set_new_velocity(self, ts=0):
        speed = self._get_next_speed()
        direction_rad = self.get_next_direction_angle()
        # Calculate new velocity
        self.velocity = np.array([np.cos(direction_rad) * speed, np.sin(direction_rad) * speed, 0.0])
        self.last_update_velocity = ts # MODE_TIME
        self.segment_distance = 0.0 # MODE_DISTANCE


    def check_set_new_velocity(self, sim_time, distance, allow_change=True):
        if self.mode == RandomWalkMobility.MODE_WALL:
            return
        elif self.mode == RandomWalkMobility.MODE_TIME:
            if allow_change and sim_time - self.last_update_velocity >= self.time_period:
                self._set_new_velocity(sim_time)
                self._record_history(sim_time)
        elif self.mode == RandomWalkMobility.MODE_DISTANCE:
            self.segment_distance += distance
            if allow_change and self.segment_distance >= self.distance_period:
                self._set_new_velocity(sim_time)
                self._record_history(sim_time)


    def _get_next_speed(self):
        if self.speed == RandomWalkMobility.SPEED_UNIFORM:
            if self.rng is None:
                return np.random.uniform(self.speed_params[0], self.speed_params[1])
            return self.rng.uniform(self.speed_params[0], self.speed_params[1])
        elif self.speed == RandomWalkMobility.SPEED_CONSTANT:
            return self.speed_params[0]
        elif self.speed == RandomWalkMobility.SPEED_NORMAL:
            if self.rng is None:
                return np.random.normal(self.speed_params[0], self.speed_params[1])
            return self.rng.normal(self.speed_params[0], self.speed_params[1])


    def get_next_direction_angle(self):
        if self.direction == RandomWalkMobility.DIRECTION_UNIFORM:
            if self.rng is None:
                return np.random.uniform(self.direction_params[0], self.direction_params[1])
            return self.rng.uniform(self.direction_params[0], self.direction_params[1])
        elif self.direction == RandomWalkMobility.DIRECTION_CONSTANT:
            return self.direction_params[0]
        elif self.direction == RandomWalkMobility.DIRECTION_NORMAL:
            if self.rng is None:
                return np.random.normal(self.direction_params[0], self.direction_params[1])
            return self.rng.normal(self.direction_params[0], self.direction_params[1])
