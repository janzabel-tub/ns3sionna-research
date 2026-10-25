import sionna.rt  # Must run before importing Mitsuba so Sionna selects its RT backend.

import os
import argparse
import numpy as np
import time
import GPUtil
import zmq
import gc

from common import message_pb2
from random_streams import resolve_randomness, mobility_rng
from common.message_debug import *

from collections import deque
import warnings

import mitsuba as mi
import math
from millify import millify

from mimo_data_contract import (ResourceLimits, lookahead_depth, array_ports,
                                RX_ARRAY_DEFAULTS, TX_ARRAY_DEFAULTS,
                                append_cfr_to_message, append_user_channel_identity,
                                extract_link_cfr,
                                normalize_link_cfr, resolve_array_config,
                                resolve_node_orientation,
                                validate_multi_user_snapshot)

from ns3sionna_utils import subcarrier_frequencies, compute_coherence_time, SECOND, MILLISECOND, coherence_from_velocities, \
    MAX_COHERENCE_TIME

from sionna.rt import load_scene, Camera, Transmitter, Receiver, PlanarArray, PathSolver

# import mobility models
from mobility import *

class SionnaEnv:

    # just compute the given single point-to-point channel
    MODE_P2P        = 1
    # extend the given P2P channel to include all receivers to take broadcast nature of wireless into account
    MODE_P2MP       = 2
    # compute also future channels; used only if the transmitter is static (receivers are mobile)
    MODE_P2MP_LAH   = 3
    MOBILITY_EPS = 1e-9
    MOBILITY_NUDGE = 1e-7
    MAX_MOBILITY_BOUNCES = 256

    """
    This class represents the Sionna component of ns3sionna. It represents the environment where the node
    placement, mobility is controlled from the client component of ns3sionna. For IPC ZMQ is used.

    author: Pilz, Zubow
    """
    def __init__(self, model_folder='./models/', rt_fast=False, default_mode=MODE_P2P,
                 rt_max_parallel_links=256, est_csi=True, VERBOSE=True,
                 CHECKS_ENABLED=True, equidistant_walk_steps=0,
                 mobility_bbox_margin=1.0, mobility_bbox_enabled=False, resource_limits=None,
                 mobility_history_limit=1024):
        if type(mobility_history_limit) is not int or mobility_history_limit < 0:
            raise ValueError("mobility_history_limit must be a nonnegative integer")
        self.mobility_history_limit = mobility_history_limit
        self._lookahead_current_states = 0
        self._lookahead_peak_states = 0
        self._prediction_nodes = 0
        self.resource_limits = resource_limits or ResourceLimits()
        self.model_folder = model_folder
        self.rt_fast = rt_fast
        if rt_fast:
            self.rt_max_depth = 3  # very small
            self.rt_samples_per_src = 10 ** 6
            self.rt_los = True  # compute and include the direct Line-of-Sight path when it exists
            self.rt_specular_reflection = True  # Can rays bounce off surfaces?
            self.rt_diffuse_reflection = False
            self.rt_refraction = True  # Can rays pass through materials?
            self.rt_synthetic_array = False  # Set True for fast simulation using one ray trace for whole array; per-element effects computed analytically
            self.rt_diffraction = False  # costly
            self.rt_edge_diffraction = False  # rays that bend around edges
            self.rt_diffraction_lit_region = False  # higher physical accuracy; for mmWave or THz channels
        else: # realistic but slow
            self.rt_max_depth = 5  # sufficient even for rich multipath
            self.rt_samples_per_src = 10 ** 6  # 10 ** 6
            self.rt_los = True  # compute and include the direct Line-of-Sight path when it exists
            self.rt_specular_reflection = True  # Can rays bounce off surfaces?
            self.rt_diffuse_reflection = True
            self.rt_refraction = True  # Can rays pass through materials?
            self.rt_synthetic_array = False  # Set True for fast simulation using one ray trace for whole array; per-element effects computed analytically
            self.rt_diffraction = True  # costly
            self.rt_edge_diffraction = True  # rays that bend around edges
            self.rt_diffraction_lit_region = True  # higher physical accuracy; for mmWave or THz channels

        # default mode
        self.default_mode = default_mode

        # maximum number of parallel computations (needed to fit GPU)
        self.rt_max_parallel_links = rt_max_parallel_links

        # estimate small-scale fading
        self.est_csi = est_csi

        print(f'Init ns3sionna with rt_fast={rt_fast}, est_csi={est_csi}')

        self.VERBOSE = VERBOSE
        self.CHECKS_ENABLED = CHECKS_ENABLED

        # Sionna RT 2.x selects a CUDA or LLVM Mitsuba variant during import.
        self.rt_backend = mi.variant()
        self.uses_gpu = self.rt_backend is not None and self.rt_backend.startswith("cuda")

        if self.uses_gpu:
            print("Using GPU ray-tracing backend:", self.rt_backend)
        else:
            print("Using CPU ray-tracing backend:", self.rt_backend)

        self.disp_r = 10
        # storing information about every node under simulation
        self.node_info = {}
        # all node which are currently placed on the scene
        self.placed_radio_node_names = []

        self.equidistant_walk_steps = int(equidistant_walk_steps)
        if self.equidistant_walk_steps < 0:
            raise ValueError("equidistant_walk_steps must be non-negative")
        self.absolute_time_mobility = self.equidistant_walk_steps > 0
        self.mobility_bbox_margin = float(mobility_bbox_margin)
        if self.mobility_bbox_margin < 0:
            raise ValueError("mobility_bbox_margin must be non-negative")
        self.mobility_bbox_enabled = bool(mobility_bbox_enabled)
        self.mobility_bounds = None


    def init_simulation_env(self, sim_init_msg):
        '''
        Initializes the Sionna environment
        :param sim_init_msg: the received ZMQ message
        :return: (success, error_msg)
        '''
        if self.VERBOSE:
            print_sim_init(sim_init_msg)

        try:
            self.randomness = resolve_randomness(sim_init_msg)
        except ValueError as error:
            return False, str(error)

        # Load the sionna scene
        filepath = os.path.join(self.model_folder, sim_init_msg.scene_fname)
        try:
            self.scene = load_scene(filepath)
        except Exception as e:
            return False, "Failed to load scene file in: " + filepath + ", error: " + str(e)

        self.bbox = self.scene.mi_scene.bbox()
        self._init_mobility_bounds()

        if self.VERBOSE:
            # show some stats about the scene
            dx = self.bbox.max.x - self.bbox.min.x
            dy = self.bbox.max.y - self.bbox.min.y
            dz = self.bbox.max.z - self.bbox.min.z
            print(f'Scenario with dx={dx:.2f}, dy={dy:.2f}, dz={dz:.2f}')

        # set mode/submode if valid
        if sim_init_msg.mode > -1:
            self.mode = sim_init_msg.mode
        else:
            self.mode = self.default_mode # default mode

        # todo: use submode
        if sim_init_msg.sub_mode > -1:
            self.sub_mode = sim_init_msg.sub_mode
        else:
            self.sub_mode = self.rt_max_parallel_links

        self.time_evo_model = sim_init_msg.time_evo_model

        # Set scene parameters
        self.scene.frequency = sim_init_msg.frequency * 1e6
        self.scene.bandwidth = sim_init_msg.channel_bw * 1e6 # max channel bandwidth

        self.fc = sim_init_msg.frequency * 1e6
        self.fft_size = sim_init_msg.fft_size  # max FFT size
        # The protobuf value is in milliseconds; keep all internal times in ns.
        # Zero selects the normal dynamic coherence-time calculation.
        self.fixed_coherence_time_ns = sim_init_msg.fixed_coherence_time_ms * MILLISECOND
        self.subcarrier_spacing = sim_init_msg.subcarrier_spacing # in Hz

        print(f'Operating in mode: {self.mode}, sub_mode: {self.sub_mode}, time_evo_model: {self.time_evo_model}'
              f', fc: {sim_init_msg.frequency} MHz, B: {sim_init_msg.channel_bw} MHz, FFT size: {self.fft_size}')

        # Missing messages deliberately reproduce the legacy 1x1 settings.
        tx_array_msg = sim_init_msg.tx_array if sim_init_msg.HasField("tx_array") else None
        rx_array_msg = sim_init_msg.rx_array if sim_init_msg.HasField("rx_array") else None
        tx_array_config = resolve_array_config(tx_array_msg, TX_ARRAY_DEFAULTS)
        rx_array_config = resolve_array_config(rx_array_msg, RX_ARRAY_DEFAULTS)
        self.rx_array_config = rx_array_config

        self.tx_array_config = tx_array_config
        self.resource_limits.channel(self.fft_size, array_ports(rx_array_config),
                                     array_ports(tx_array_config))
        if self.mode == SionnaEnv.MODE_P2MP_LAH:
            lookahead_depth(self.sub_mode, len(sim_init_msg.nodes) - 1)

        self.scene.tx_array = PlanarArray(**tx_array_config)
        self.scene.rx_array = PlanarArray(**rx_array_config)

        # Subcarrier frequencies
        self.frequencies = subcarrier_frequencies(num_subcarriers=self.fft_size, subcarrier_spacing=self.subcarrier_spacing)

        # Set the random seed for reproducibility
        np.random.seed(self.randomness["mobility_seed"])
        self.my_seed = self.randomness["rt_seed"]

        # configure mobility models
        self._init_mobility(sim_init_msg)

        # for mode 3 if only constant speed model supported
        if self.mode == SionnaEnv.MODE_P2MP_LAH:
            speed_arr = []
            for node_id in list(self.node_info.keys()):
                if isinstance(self.node_info[node_id], RandomWalkMobility):
                    if self.node_info[node_id].speed != RandomWalkMobility.SPEED_CONSTANT:
                        warnings.warn(f"Only constant speed model is supported when using mode P2MP(LAH); switching to mode P2P.", UserWarning)
                        self.mode = SionnaEnv.MODE_P2MP
                        break
                    else:
                        speed_arr.append(self.node_info[node_id].speed_params[0])
            # compute coherence time assuming worst case: fastest nodes move away from each other
            speed_arr.sort(reverse=True)
            if len(speed_arr) >= 2:
                self.chan_coh_time_mode3 = compute_coherence_time(speed_arr[0] + speed_arr[1], self.fc, model='rappaport2')
            elif len(speed_arr) == 1:
                self.chan_coh_time_mode3 = compute_coherence_time(speed_arr[0], self.fc, model='rappaport2')
            else:
                self.chan_coh_time_mode3 = MAX_COHERENCE_TIME

            print(f'Running mode=3 w/ Tc: {self.chan_coh_time_mode3/1e6}ms')

        # set current sim time to 0ns
        self.sim_time = 0

        return True, "OK"


    @staticmethod
    def _scalar_to_float(value):
        if hasattr(value, "numpy"):
            value = value.numpy()
        return float(np.asarray(value).reshape(-1)[0])


    def _init_mobility_bounds(self):
        self.mobility_bounds = None
        if not self.mobility_bbox_enabled:
            return

        margin = self.mobility_bbox_margin
        xmin = self._scalar_to_float(self.bbox.min.x) - margin
        xmax = self._scalar_to_float(self.bbox.max.x) + margin
        ymin = self._scalar_to_float(self.bbox.min.y) - margin
        ymax = self._scalar_to_float(self.bbox.max.y) + margin
        bounds = (xmin, xmax, ymin, ymax)
        if not all(math.isfinite(value) for value in bounds) or xmin >= xmax or ymin >= ymax:
            raise ValueError(f"Invalid scene bounding box for mobility: {bounds}")
        self.mobility_bounds = bounds


    def _next_bbox_hit(self, pos, direction, max_distance):
        if self.mobility_bounds is None:
            return None

        xmin, xmax, ymin, ymax = self.mobility_bounds
        candidates = []
        for axis, lower, upper in ((0, xmin, xmax), (1, ymin, ymax)):
            component = direction[axis]
            if abs(component) <= self.MOBILITY_EPS:
                continue
            boundary = upper if component > 0 else lower
            distance = (boundary - pos[axis]) / component
            if -self.MOBILITY_EPS <= distance <= max_distance + self.MOBILITY_EPS:
                candidates.append((max(0.0, distance), axis))

        if not candidates:
            return None
        hit_distance = min(distance for distance, _ in candidates)
        hit_axes = [axis for distance, axis in candidates
                    if abs(distance - hit_distance) <= self.MOBILITY_NUDGE]
        return hit_distance, hit_axes


    def _update_nodes_to_time(self, target_time, node_ids):
        if not self.absolute_time_mobility:
            dt = target_time - self.sim_time
            for node_id in node_ids:
                self._walk(node_id, dt)
            return

        for node_id in node_ids:
            node = self.node_info[node_id]
            while node.last_update < target_time:
                next_tick = ((node.last_update // self.equidistant_walk_steps) + 1) * self.equidistant_walk_steps
                step_end = min(target_time, next_tick)
                self._walk(node_id,
                           step_end - node.last_update,
                           start_time=node.last_update,
                           allow_velocity_change=step_end == next_tick)
                node = self.node_info[node_id]


    def _make_mobility_rng(self, node_id):
        # Legacy inputs retain their original node generator exactly.
        mapping = getattr(self, "randomness", {"mobility_seed": self.my_seed})
        return mobility_rng(mapping, node_id)


    def _check_batch_budget(self, receivers, depth=1):
        config = self.rx_array_config
        identity_bytes = len(config["pattern"].encode()) + len(config["polarization"].encode())
        return self.resource_limits.plan(self.fft_size, array_ports(config),
            array_ports(self.tx_array_config), receivers, depth, identity_bytes, self.est_csi)

    def compute_cfr(self, csi_req, reply_wrapper):

        tx_node_id = csi_req.tx_node
        rx_node_id = csi_req.rx_node

        if self.mode == SionnaEnv.MODE_P2MP_LAH:
            if isinstance(self.node_info[tx_node_id], RandomWalkMobility) and isinstance(self.node_info[rx_node_id], ConstantMobility):
                # Exploit channel reciprocity - swap mobile TX with static RX
                csi_req.tx_node = rx_node_id
                csi_req.rx_node = tx_node_id

        # check if mode 3 can be used
        if self.mode == SionnaEnv.MODE_P2MP_LAH and isinstance(self.node_info[tx_node_id], ConstantMobility):
            # mode=3 is feasible if TX is fixed
            return self.compute_cfr_with_lookahead(csi_req, reply_wrapper)
        else:
            req_mode = self.mode
            # mode=1/2 or if TX node is mobile
            if self.mode == SionnaEnv.MODE_P2MP_LAH:
                req_mode = SionnaEnv.MODE_P2MP
                print(f'Fallback to mode={req_mode} as TX node is mobile')

            return self.compute_cfr_classic(csi_req, reply_wrapper, req_mode)


    def mobility_storage_statistics(self):
        """Retained history and transient pose-array bytes, excluding Python/RNG/RT overhead."""
        nodes = getattr(self, "_lookahead_committed_nodes", self.node_info)
        histories = {str(node_id): node.history_statistics() for node_id, node in nodes.items()}
        states = getattr(self, "_lookahead_current_states", 0)
        prediction_nodes = getattr(self, "_prediction_nodes", 0)
        return {"nodes": histories, "history_entries": sum(n["entries"] for n in histories.values()),
                "history_bytes": sum(n["bytes"] for n in histories.values()),
                "lookahead_states": states, "lookahead_bytes": 48 * states,
                "peak_lookahead_states": getattr(self, "_lookahead_peak_states", 0),
                "prediction_nodes": prediction_nodes, "prediction_bytes": 48 * prediction_nodes}


    def compute_cfr_with_lookahead(self, csi_req, reply_wrapper):
        """Forecast on history-free clones; every forecast exit restores committed motion."""
        node_ids = sorted(self.node_info)
        depth = lookahead_depth(self.sub_mode, len(node_ids) - 1)
        self._check_batch_budget(len(node_ids) - 1, depth)
        # Commit only motion up to the actual requested timestamp.
        self._update_nodes_to_time(csi_req.time, node_ids)
        self.sim_time = csi_req.time
        committed = self.node_info
        rng_state = np.random.get_state()  # Legacy nodes may use the global generator.
        try:
            self.node_info = {node_id: node.clone_for_prediction()
                              for node_id, node in committed.items()}
            self._lookahead_committed_nodes = committed
            self._prediction_nodes = len(committed)
            return self._compute_cfr_with_lookahead(csi_req, reply_wrapper)
        finally:
            self.node_info = committed
            self.sim_time = csi_req.time
            np.random.set_state(rng_state)
            self._lookahead_current_states = 0
            self._prediction_nodes = 0
            self.__dict__.pop("_lookahead_committed_nodes", None)


    def _compute_cfr_with_lookahead(self, csi_req, reply_wrapper):
        '''
        Compute the requested CFR
        :param csi_req: received CSI request (ZMQ)
        :param reply_wrapper: the response
        '''

        if self.VERBOSE:
            print_csi_request(csi_req)

        tx_node_id = csi_req.tx_node
        rx_node_id = csi_req.rx_node # this rx node must be included in result set
        req_sim_time = csi_req.time # we need CFR at that point in time [ns]

        assert self.time_evo_model == 'position'

        # execute mobility

        # update position of all nodes
        nodes_to_update = sorted(self.node_info)

        # compute look-ahead
        look_ahead = lookahead_depth(self.sub_mode, len(nodes_to_update) - 1)

        print(f'compute CFR to #RX={len(nodes_to_update) - 1} with LAH={look_ahead}')

        # sim future node positions
        lah_time_vec = []
        lah_states = []
        for lah_i in range(look_ahead):
            self._update_nodes_to_time(req_sim_time, nodes_to_update)
            lah_states.append({node_id: (node.pos.copy(), node.velocity.copy())
                               for node_id, node in self.node_info.items() if node_id != tx_node_id})
            self._lookahead_current_states = len(lah_states) * (len(nodes_to_update) - 1)
            self._lookahead_peak_states = max(getattr(self, "_lookahead_peak_states", 0),
                                             self._lookahead_current_states)

            csi_tc_arr = []
            for node_id in nodes_to_update:
                if node_id != tx_node_id:
                    if self.fixed_coherence_time_ns == 0:
                        tc = coherence_from_velocities(self.node_info[node_id].velocity,
                                                        self.node_info[tx_node_id].velocity, self.fc,
                                                        pos_tx=self.node_info[node_id].pos,
                                                        pos_rx=self.node_info[tx_node_id].pos)
                        csi_tc_arr.append(tc)
                    else:
                        csi_tc_arr.append(self.fixed_coherence_time_ns)

            # take the worst case Tc from all RX nodes
            Tc_p2mp = int(np.min(np.asarray(csi_tc_arr)))

            # update time
            self.sim_time = req_sim_time
            lah_time_vec.append(req_sim_time)
            # new req time is old + Tc
            req_sim_time = req_sim_time + Tc_p2mp


        # place TX and RX nodes together with their future positions
        rx_nodes = nodes_to_update
        rx_nodes.remove(tx_node_id)

        self._place_tx_rx_nodes_with_lah(lah_time_vec, tx_node_id, rx_nodes, lah_states)

        # create pathsolver; todo: check reuse
        # Sionna RT 2.2 requires this for repeatable paths at a fixed seed.
        p_solver = PathSolver(deterministic=True)

        # Compute propagation paths
        paths = p_solver(scene=self.scene,
                         max_depth=self.rt_max_depth,
                         samples_per_src=self.rt_samples_per_src,
                         los=self.rt_los,
                         specular_reflection=self.rt_specular_reflection,  # Can rays bounce off surfaces?
                         diffuse_reflection=self.rt_diffuse_reflection,
                         refraction=self.rt_refraction,  # Can rays pass through materials?
                         synthetic_array=self.rt_synthetic_array,
                         diffraction=self.rt_diffraction,  # costly
                         edge_diffraction=self.rt_edge_diffraction,  # rays that bend around edges
                         diffraction_lit_region=self.rt_diffraction_lit_region,  # higher physical accuracy
                         seed=self.my_seed)

        # AZU: sampling_frequency is only used if num_time_steps > 1
        # a: shape [num_rx, num_rx_ant, num_tx, num_tx_ant, num_paths, num_time_steps],
        a, tau = paths.cir(sampling_frequency=1e9, normalize_delays=False, out_type="numpy")

        # shape: [num_rx, num_rx_ant, num_tx, num_tx_ant, num_ofdm_symbols, num_subcarriers]
        h_raw = paths.cfr(frequencies=self.frequencies,
                  sampling_frequency=1.0,  # not used
                  num_time_steps=1,
                  normalize_delays=True,
                  # If set to True, path delays are normalized such that the first path between any pair of
                  # antennas of a transmitter and receiver arrives at tau=0
                  normalize=False,  # Normalize energy
                  out_type="numpy")

        # max single transmitter
        assert h_raw.shape[2] == 1
        num_computed_lnks = h_raw.shape[0]

        # Create ZMQ response
        chan_response = reply_wrapper.channel_state_response
        multi_user_snapshot = self.est_csi and len(rx_nodes) > 1

        Tc_p2mp_lah = []
        for lah_time_idx, lah_time in enumerate(lah_time_vec): # iterate over time
            lah_positions = {node_id: state[0] for node_id, state in lah_states[lah_time_idx].items()}
            lah_velocities = {node_id: state[1] for node_id, state in lah_states[lah_time_idx].items()}
            csi = chan_response.csi.add()

            csi.start_time = lah_time
            # tx node is fixed
            tx_pos = self.node_info[tx_node_id].pos
            csi.tx_node.id = tx_node_id
            csi.tx_node.position.x = tx_pos[0]
            csi.tx_node.position.y = tx_pos[1]
            csi.tx_node.position.z = tx_pos[2]

            # get receiver(s)
            csi_tc_arr = []
            for curr_rx_id, curr_rx_node in enumerate(rx_nodes):

                rx_id = lah_time_idx * len(rx_nodes) + curr_rx_id

                h = extract_link_cfr(h_raw, rx_id)
                lnk_delay, lnk_loss, h_normalized = self._extract_link_channel(
                    tau[rx_id], h, tx_pos, lah_positions[curr_rx_node])

                if self.CHECKS_ENABLED:
                    power_normalized = np.mean(np.abs(h_normalized) ** 2)
                    assert math.isclose(power_normalized, 1.0, rel_tol=1e-3)   # Should be close to 1

                if self.VERBOSE:
                    print(f'{lah_time/1e9}s: {tx_node_id}->{curr_rx_node} lnk_delay = {lnk_delay}ns, wb_loss = {lnk_loss:.3f}dB, CFR shape: {h_normalized.shape}')

                rx_node_info = csi.rx_nodes.add()
                rx_pos = lah_positions[curr_rx_node]
                rx_node_info.id = curr_rx_node
                rx_node_info.position.x = rx_pos[0]
                rx_node_info.position.y = rx_pos[1]
                rx_node_info.position.z = rx_pos[2]
                rx_node_info.delay = lnk_delay
                rx_node_info.wb_loss = lnk_loss

                if self.est_csi:
                    append_cfr_to_message(rx_node_info, self.frequencies, h_normalized, self.resource_limits)
                if multi_user_snapshot:
                    append_user_channel_identity(rx_node_info, curr_rx_node,
                                                 tx_node_id, csi.start_time,
                                                 self.rx_array_config)
                if self.fixed_coherence_time_ns == 0:
                    tc = coherence_from_velocities(lah_velocities[curr_rx_node],
                                                    self.node_info[tx_node_id].velocity, self.fc,
                                                    pos_tx=lah_positions[curr_rx_node],
                                                    pos_rx=self.node_info[tx_node_id].pos)
                else:
                    tc = self.fixed_coherence_time_ns

                rx_node_info.end_time2 = csi.start_time + tc
                csi_tc_arr.append(tc)

            if multi_user_snapshot:
                validate_multi_user_snapshot(csi, self.resource_limits)

            # take the worst case Tc from all RX nodes
            Tc_p2mp = int(np.min(np.asarray(csi_tc_arr)))
            Tc_p2mp_lah.append(Tc_p2mp)
            csi.end_time = csi.start_time + Tc_p2mp - 1 # -1ns to have non-overlapping intervals

        print(f'{self.sim_time / 1e9}s: Computed CSI with Tc: {np.round(np.asarray(Tc_p2mp_lah) / 1e6,2)}ms, #links: {num_computed_lnks}')

        return num_computed_lnks


    def _place_tx_rx_nodes_with_lah(self, lah_time_vec: list, tx_node: int, rx_nodes: list, lah_states=None):
        '''
        Place the given nodes together with their lookahead positions in the scenario
        :param lah_time_vec: time vector containing the lah time vector
        :param tx_node: the transmitting node
        :param rx_nodes: the receiver nodes
        '''

        # remove old tx and rx nodes
        for placed_node in self.placed_radio_node_names:
            self.scene.remove(placed_node)
        self.placed_radio_node_names.clear()

        # only supported if TX is fixed
        fixed_tx_node = isinstance(self.node_info[tx_node], ConstantMobility)
        assert fixed_tx_node

        # Create transmitter
        tx_pos = self.node_info[tx_node].pos
        tx_node_name = "tx"
        tx = Transmitter(name=tx_node_name, position=tx_pos,
                         orientation=self.node_orientations[tx_node], display_radius=self.disp_r)
        self.scene.add(tx)
        self.placed_radio_node_names.append(tx_node_name)

        for lah_time_idx, lah_time in enumerate(lah_time_vec):
            # Create a receiver(s)
            for rx_node_i in rx_nodes:
                rx_node_name = "rx" + str(rx_node_i) + "." + str(lah_time_idx)
                rx_pos = (lah_states[lah_time_idx][rx_node_i][0] if lah_states is not None else
                          self.node_info[rx_node_i].get_pos_at(lah_time))
                rx = Receiver(name=rx_node_name, position=rx_pos,
                              orientation=self.node_orientations[rx_node_i], display_radius=self.disp_r)
                self.scene.add(rx)
                self.placed_radio_node_names.append(rx_node_name)


    def compute_cfr_classic(self, csi_req, reply_wrapper, req_mode):
        '''
        Compute the requested CFR
        :param csi_req: received CSI request (ZMQ)
        :param reply_wrapper: the response
        '''

        if self.VERBOSE:
            print_csi_request(csi_req)

        tx_node_id = csi_req.tx_node
        rx_node_id = csi_req.rx_node # this rx node must be included in result set
        req_sim_time = csi_req.time # we need CFR at that point in time [ns]

        if self.time_evo_model == 'doppler':
            #(lnk_delay, lnk_loss, h_normalized) = self.compute_cfr_via_doppler()
            pass
        else: # position=based
            (rx_nodes, lnk_delay, lnk_loss, h_normalized) = self._compute_cfr_via_position(req_sim_time, tx_node_id, rx_node_id, req_mode)

        # Create ZMQ response
        chan_response = reply_wrapper.channel_state_response
        csi = chan_response.csi.add()

        csi.start_time = self.sim_time
        # tx node info
        tx_pos = self.node_info[tx_node_id].pos
        csi.tx_node.id = tx_node_id
        csi.tx_node.position.x = tx_pos[0]
        csi.tx_node.position.y = tx_pos[1]
        csi.tx_node.position.z = tx_pos[2]

        multi_user_snapshot = self.est_csi and len(rx_nodes) > 1
        csi_tc_arr = []
        # for all rx nodes
        for idx, comp_rx_node_id in enumerate(rx_nodes):
            rx_node_info = csi.rx_nodes.add()
            rx_pos = self.node_info[comp_rx_node_id].pos
            rx_node_info.id = comp_rx_node_id
            rx_node_info.position.x = rx_pos[0]
            rx_node_info.position.y = rx_pos[1]
            rx_node_info.position.z = rx_pos[2]
            rx_node_info.delay = lnk_delay[idx]
            rx_node_info.wb_loss = lnk_loss[idx]

            if self.est_csi:
                append_cfr_to_message(rx_node_info, self.frequencies, h_normalized[idx], self.resource_limits)
            if multi_user_snapshot:
                append_user_channel_identity(rx_node_info, comp_rx_node_id,
                                             tx_node_id, csi.start_time,
                                             self.rx_array_config)

            # compute coherence time: with direction vectors you can compute the radial (projected) relative
            # speed directly and from that the Doppler and coherence time.
            if self.fixed_coherence_time_ns == 0:
                tc = coherence_from_velocities(self.node_info[comp_rx_node_id].velocity,
                                                self.node_info[tx_node_id].velocity, self.fc,
                                                pos_tx=self.node_info[comp_rx_node_id].pos,
                                                pos_rx=self.node_info[tx_node_id].pos)
            else:
                tc = self.fixed_coherence_time_ns
            rx_node_info.end_time2 = tc
            csi_tc_arr.append(tc)

        if multi_user_snapshot:
            validate_multi_user_snapshot(csi, self.resource_limits)

        # take the worst case Tc from all RX nodes
        Tc_p2mp = int(np.min(np.asarray(csi_tc_arr)))

        print(f'{self.sim_time / 1e9}s: Computed CSI with Tc: {round(Tc_p2mp / 1e6,2)}ms, #links: {len(rx_nodes)}')

        csi.end_time = self.sim_time + Tc_p2mp
        return len(rx_nodes)


    def _walk(self, node_id, dt, start_time=None, allow_velocity_change=True):
        """Advance one node by ``dt`` nanoseconds, reflecting at scene and bbox walls."""
        if dt <= 0:
            return

        node = self.node_info[node_id]
        start_time = self.sim_time if start_time is None else start_time
        target_time = start_time + dt
        pos = np.asarray(node.pos, dtype=float).copy()
        velocity = np.asarray(node.velocity, dtype=float).copy()
        speed = np.linalg.norm(velocity)

        if not np.all(np.isfinite(pos)) or not np.all(np.isfinite(velocity)):
            raise ValueError(f"Non-finite mobility state for node {node_id}")
        if speed <= self.MOBILITY_EPS:
            node.update_pos(target_time, pos, velocity, False)
            node.check_set_new_velocity(target_time, 0.0, allow_velocity_change)
            return

        direction = velocity / speed
        distance = speed * dt / 1e9
        initial_distance = distance

        if self.mobility_bounds is not None:
            xmin, xmax, ymin, ymax = self.mobility_bounds
            for axis, (lower, upper) in enumerate(((xmin, xmax), (ymin, ymax))):
                if pos[axis] < lower or pos[axis] > upper:
                    side = -1 if pos[axis] < lower else 1
                    pos[axis] = np.clip(pos[axis], lower + self.MOBILITY_NUDGE,
                                       upper - self.MOBILITY_NUDGE)
                    if direction[axis] * side > 0:
                        direction[axis] *= -1

        bounces = 0
        while distance > self.MOBILITY_EPS:
            bounces += 1
            if bounces > self.MAX_MOBILITY_BOUNCES:
                warnings.warn(f"Node {node_id} exceeded the mobility bounce limit")
                break

            bbox_hit = self._next_bbox_hit(pos, direction, distance)
            ray_distance = distance if bbox_hit is None else min(distance, bbox_hit[0])
            ray = mi.Ray3f(mi.Point3f(pos), mi.Vector3f(direction))
            ray.maxt = mi.Float(ray_distance)
            interaction = self.scene.mi_scene.ray_intersect(
                ray, mi.RayFlags.Minimal, False, True)

            scene_distance = None
            if interaction.is_valid():
                scene_distance = max(float(interaction.t.numpy()[0]), 0.0)
            scene_hit = (scene_distance is not None and
                         (bbox_hit is None or
                          scene_distance < bbox_hit[0] - self.MOBILITY_EPS))

            if scene_hit:
                travelled = min(max(scene_distance - 0.01, 0.0), distance)
                pos += travelled * direction
                distance -= travelled

                normal = np.squeeze(interaction.n.numpy())
                tangent = np.asarray([normal[1], -normal[0], 0.0], dtype=float)
                tangent_norm = np.linalg.norm(tangent)
                if tangent_norm <= self.MOBILITY_EPS:
                    direction = -direction
                else:
                    tangent /= tangent_norm
                    angle = node.get_next_direction_angle()
                    direction = -(direction - 2 * (np.dot(direction, tangent) + angle) * tangent)
                    direction_norm = np.linalg.norm(direction)
                    direction = (-velocity / speed if direction_norm <= self.MOBILITY_EPS
                                 else direction / direction_norm)

                nudge = min(self.MOBILITY_NUDGE, distance)
                pos += nudge * direction
                distance -= nudge
            elif bbox_hit is not None:
                travelled, hit_axes = bbox_hit
                pos += travelled * direction
                distance -= travelled
                for axis in hit_axes:
                    direction[axis] *= -1

                xmin, xmax, ymin, ymax = self.mobility_bounds
                for axis in hit_axes:
                    lower, upper = ((xmin, xmax), (ymin, ymax))[axis]
                    pos[axis] = np.clip(pos[axis], lower + self.MOBILITY_NUDGE,
                                       upper - self.MOBILITY_NUDGE)
            else:
                pos += distance * direction
                distance = 0.0

        velocity = direction * speed
        node.update_pos(target_time, pos, velocity, bounces > 1)
        node.check_set_new_velocity(target_time,
                                    initial_distance - distance,
                                    allow_velocity_change)


    def _place_tx_rx_node(self, tx_node: int, rx_nodes: list):
        '''
        Place the given nodes in the scenario
        :param tx_node: the transmitting node
        :param rx_nodes: the receiver nodes
        '''

        # remove old tx and rx nodes
        for placed_node in self.placed_radio_node_names:
            self.scene.remove(placed_node)
        self.placed_radio_node_names.clear()

        # Create transmitter
        tx_pos = self.node_info[tx_node].pos
        tx_node_name = "tx"
        tx = Transmitter(name=tx_node_name, position=tx_pos,
                         orientation=self.node_orientations[tx_node], display_radius=self.disp_r)
        self.scene.add(tx)
        self.placed_radio_node_names.append(tx_node_name)

        # Create a receiver(s)
        for rx_node_i in rx_nodes:
            rx_node_name = "rx" + str(rx_node_i)
            rx_pos = self.node_info[rx_node_i].pos
            rx = Receiver(name=rx_node_name, position=rx_pos,
                          orientation=self.node_orientations[rx_node_i], display_radius=self.disp_r)
            self.scene.add(rx)
            self.placed_radio_node_names.append(rx_node_name)


    def _extract_link_channel(self, tau_link, h, tx_pos, rx_pos):
        """Keep links without RT paths unreachable, as on the janzabel branch."""
        delays = np.asarray(tau_link)
        valid_delays = delays[np.isfinite(delays) & (delays >= 0)]
        if valid_delays.size and np.any(h):
            loss, normalized = normalize_link_cfr(h, self.resource_limits)
            return int(round(np.min(valid_delays) * 1e9)), loss, normalized

        if not np.all(np.isfinite(h)):
            raise ValueError("CFR contains non-finite coefficients")
        # A finite unit-power placeholder preserves the wire contract and
        # supports SVD with multiple streams. 300 dB loss makes it unreachable;
        # these coefficients do not represent a measured propagation channel.
        placeholder = np.broadcast_to(np.eye(h.shape[0], h.shape[1])[:, :, None], h.shape)
        _, normalized = normalize_link_cfr(placeholder, self.resource_limits)
        delay = int(round(np.linalg.norm(np.asarray(tx_pos) - np.asarray(rx_pos))
                          / 299792458.0 * 1e9))
        warnings.warn(f"No valid Sionna path; using unreachable 300 dB fallback. "
                      f"tx_pos={tx_pos}, rx_pos={rx_pos}", UserWarning)
        return delay, 300.0, normalized


    def _compute_cfr_via_position(self, req_sim_time, tx_node, rx_node, req_mode):
        '''
        Compute the link propagation delay, wideband loss and normalized CFR
        :param req_sim_time: current simulation time
        :param tx_node: the transmitter node id
        :param rx_node: the receiver node id
        :return: (list(rx_node), list(link propagation delay), list(wideband loss), list(normalized CFR))
        '''

        # estimate the node we need to update their position
        if req_mode == SionnaEnv.MODE_P2P:
            nodes_to_update = [tx_node, rx_node] # only TX and RX
        else:
            # both P2MP and P2MP_LAH
            nodes_to_update = sorted(self.node_info)

        self._check_batch_budget(len(nodes_to_update) - 1)
        self._update_nodes_to_time(req_sim_time, nodes_to_update)

        # update time
        self.sim_time = req_sim_time

        # place TX and RX
        rx_nodes = nodes_to_update
        rx_nodes.remove(tx_node)
        self._place_tx_rx_node(tx_node, rx_nodes)

        # create pathsolver; todo: check reuse
        # Sionna RT 2.2 requires this for repeatable paths at a fixed seed.
        p_solver = PathSolver(deterministic=True)

        # Compute propagation paths
        paths = p_solver(scene=self.scene,
                         max_depth=self.rt_max_depth,
                         samples_per_src=self.rt_samples_per_src,
                         los=self.rt_los,
                         specular_reflection=self.rt_specular_reflection,  # Can rays bounce off surfaces?
                         diffuse_reflection=self.rt_diffuse_reflection,
                         refraction=self.rt_refraction,  # Can rays pass through materials?
                         synthetic_array=self.rt_synthetic_array,
                         diffraction=self.rt_diffraction,  # costly
                         edge_diffraction=self.rt_edge_diffraction,  # rays that bend around edges
                         diffraction_lit_region=self.rt_diffraction_lit_region,  # higher physical accuracy
                         seed=self.my_seed)

        # AZU: sampling_frequency is only used if num_time_steps > 1
        # a: shape [num_rx, num_rx_ant, num_tx, num_tx_ant, num_paths, num_time_steps],
        a, tau = paths.cir(sampling_frequency=1e9, normalize_delays=False, out_type="numpy")

        # shape: [num_rx, num_rx_ant, num_tx, num_tx_ant, num_ofdm_symbols, num_subcarriers]
        h_raw = paths.cfr(frequencies=self.frequencies,
                  sampling_frequency=1.0,  # not used
                  num_time_steps=1,
                  normalize_delays=True,
                  # If set to True, path delays are normalized such that the first path between any pair of
                  # antennas of a transmitter and receiver arrives at tau=0
                  normalize=False,  # Normalize energy
                  out_type="numpy")

        lnk_delay_arr = []
        lnk_loss_arr = []
        h_normalized_arr = []

        for rx_id, curr_rx_node in enumerate(rx_nodes):
            h = extract_link_cfr(h_raw, rx_id)
            lnk_delay, lnk_loss, h_normalized = self._extract_link_channel(
                tau[rx_id], h, self.node_info[tx_node].pos, self.node_info[curr_rx_node].pos)

            if self.CHECKS_ENABLED:
                power_normalized = np.mean(np.abs(h_normalized) ** 2)
                assert math.isclose(power_normalized, 1.0, rel_tol=1e-3)   # Should be close to 1

            if self.VERBOSE:
                print(f'{self.sim_time/1e9}s: lnk_delay = {lnk_delay}ns, wb_loss = {lnk_loss:.3f}dB, CFR shape: {h_normalized.shape}')

            lnk_delay_arr.append(lnk_delay)
            lnk_loss_arr.append(lnk_loss)
            h_normalized_arr.append(h_normalized)

        return rx_nodes, lnk_delay_arr, lnk_loss_arr, h_normalized_arr


    def _get_mobility_history(self, node_id):
        ts = sorted(self.node_info[node_id].pos_history.keys())
        pos = [self.node_info[node_id].pos_history[t].copy() for t in ts]
        return ts, pos


    def _init_mobility(self, sim_init_msg):
        # Store information about each node: ID, mobility model
        self.node_orientations = {}
        for node_info in sim_init_msg.nodes:
            self.node_orientations[node_info.id] = resolve_node_orientation(node_info)
            if (node_info.HasField("constant_position_model")):
                # fixed position; no mobility
                pos = node_info.constant_position_model.position
                self.node_info[node_info.id] = ConstantMobility(node_info.id, [pos.x, pos.y, pos.z],
                    history_limit=self.mobility_history_limit)
            elif (node_info.HasField("random_walk_model")):
                # mobile scenario
                random_walk_model = node_info.random_walk_model
                pos = random_walk_model.position

                mode = None
                if random_walk_model.HasField("wall_value"):
                    mode = RandomWalkMobility.MODE_WALL
                    mode_params = random_walk_model.wall_value
                elif random_walk_model.HasField("time_value"):
                    mode = RandomWalkMobility.MODE_TIME
                    mode_params = random_walk_model.time_value
                elif random_walk_model.HasField("distance_value"):
                    mode = RandomWalkMobility.MODE_DISTANCE
                    mode_params = random_walk_model.distance_value

                speed = None
                if random_walk_model.speed.HasField("uniform"):
                    speed = RandomWalkMobility.SPEED_UNIFORM
                    speed_params = (random_walk_model.speed.uniform.min, random_walk_model.speed.uniform.max)
                elif random_walk_model.speed.HasField("constant"):
                    speed = RandomWalkMobility.SPEED_CONSTANT
                    speed_params = (random_walk_model.speed.constant.value,)
                elif random_walk_model.speed.HasField("normal"):
                    speed = RandomWalkMobility.SPEED_NORMAL
                    speed_params = (random_walk_model.speed.normal.mean, random_walk_model.speed.normal.variance)

                direction = None
                if random_walk_model.direction.HasField("uniform"):
                    direction = RandomWalkMobility.DIRECTION_UNIFORM
                    direction_params = (random_walk_model.direction.uniform.min,
                                        random_walk_model.direction.uniform.max)
                elif random_walk_model.direction.HasField("constant"):
                    direction = RandomWalkMobility.DIRECTION_CONSTANT
                    direction_params = (random_walk_model.direction.constant.value,)
                elif random_walk_model.direction.HasField("normal"):
                    direction = RandomWalkMobility.DIRECTION_NORMAL
                    direction_params = (random_walk_model.direction.normal.mean,
                                        random_walk_model.direction.normal.variance)

                rng = self._make_mobility_rng(node_info.id) if (self.absolute_time_mobility or
                    self.randomness["mapping_version"] == 1) else None
                self.node_info[node_info.id] = RandomWalkMobility(node_info.id, [pos.x, pos.y, pos.z],
                                                                  mode, mode_params, speed, speed_params,
                                                                  direction, direction_params, rng=rng,
                                                                  history_limit=self.mobility_history_limit)


    def run(self):
        '''
        Handles communication with the ns3 simulator using ZMQ socket
        '''

        context = zmq.Context()
        socket = zmq.Socket(context, zmq.REP)
        socket.bind("tcp://*:5555")

        print("Sionna server socket ready ...")

        last_call_times = deque(maxlen=10)
        total_num_csi_samples = 0

        do_terminate = False
        while not do_terminate:
            # Receive message from ns3 simulator
            ns3_msg_str = socket.recv()

            # Deserialize received message
            ns3_msg = message_pb2.Wrapper()
            ns3_msg.ParseFromString(ns3_msg_str)

            # Prepare reply message
            resp_msg = message_pb2.Wrapper()

            # Fill the reply message
            if ns3_msg.HasField("sim_init_msg"):
                # handle SimInitMessage & send ACK
                try:
                    successful, error_msg = self.init_simulation_env(ns3_msg.sim_init_msg)
                except ValueError as error:
                    successful, error_msg = False, str(error)
                resp_msg.sim_ack.no_error = successful
                resp_msg.sim_ack.error_msg = error_msg
                resp_msg.sim_ack.SetInParent()

                if successful:
                    print("Sionna server init sucessful ...")
                else:
                    print("Sionna server init failed ...")
                    do_terminate = True

            elif ns3_msg.HasField("channel_state_request"):
                # handle ChannelStateRequest by sending ChannelStateResponse
                start_time = time.time()
                try:
                    num_csi_req = self.compute_cfr(ns3_msg.channel_state_request, resp_msg)
                    self.resource_limits.check(resp_msg.ByteSize(), "reply_bytes")
                except ValueError as error:
                    resp_msg.Clear()
                    resp_msg.sim_ack.no_error = False
                    resp_msg.sim_ack.error_msg = str(error)
                    num_csi_req = 0
                total_num_csi_samples += num_csi_req
                last_call_times.append(time.time() - start_time)

                if total_num_csi_samples % 1000 == 0: # every 1k make printout
                    print(f'Total no. computed CSI samples: {millify(total_num_csi_samples)}')

                if self.VERBOSE:
                    avg_call_time = sum(last_call_times) / len(last_call_times)
                    print("t=%.9fs: average event processing time: %.2f sec"
                          % (ns3_msg.channel_state_request.time/1e9, avg_call_time))
                    # show GPU load
                    if self.uses_gpu:
                        GPUtil.showUtilization()

            elif ns3_msg.HasField("sim_close_request"):
                do_terminate = True
                resp_msg.sim_ack.SetInParent()

            # Serialize and send the reply message
            socket.send(resp_msg.SerializeToString())

        socket.close()
        print("Computed no. CSI samples: %d" % total_num_csi_samples)
        print("Sionna server socket closed.")


    def release(self):
        # delete / release the scene before loading a new one
        del self.scene
        gc.collect()  # force garbage collection


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument("--model_folder", type=str, default='models/', help="The folder containing the XML files of the scenes")
    parser.add_argument("--single_run", help="Whether not to terminate after single run", action='store_true')
    parser.add_argument("--default_mode", type=int, default=SionnaEnv.MODE_P2MP, help="Which mode to use if not set by ns3")
    parser.add_argument("--rt_fast", help="Use simplified raytracing for faster computations", action='store_true')
    parser.add_argument("--rt_max_parallel_links", type=int, default=256, help="Max no. of link simulated at once; depends on GPU memory")
    parser.add_argument("--est_csi", help="Whether to estimate complex CSI per OFDM subcarrier", type=bool, default=True)
    parser.add_argument("--verbose", help="Whether to run in verbose mode", action='store_true')
    parser.add_argument("--equidistant_walk_steps", type=int, default=0,
                        help="Fixed internal mobility step in ns; 0 uses request intervals")
    parser.add_argument("--mobility_history_limit", type=int, default=1024,
                        help="Maximum retained history records per node; 0 disables history")
    parser.add_argument("--mobility_bbox_toggle", action='store_true',
                        help="Reflect mobile nodes at the scene x/y bounding box")
    parser.add_argument("--mobility_bbox_margin", type=float, default=1.0,
                        help="Outward scene bounding-box margin in meters")
    for name, value in vars(ResourceLimits()).items():
        parser.add_argument("--" + name, type=int, default=value,
                            help="Positive inclusive channel resource limit")
    args = parser.parse_args()

    print("ns3sionna v1.0")
    while True:
        print("Using config: model_folder=%s, single_run=%s, mode=%d, rt_fast=%s, rt_max_parallel_links=%d, est_csi=%r"
              % (args.model_folder, args.single_run, args.default_mode, args.rt_fast, args.rt_max_parallel_links, args.est_csi))
        print("Waiting for new job ...")
        env = SionnaEnv(args.model_folder, args.rt_fast, args.default_mode, args.rt_max_parallel_links,
                        args.est_csi, VERBOSE=args.verbose,
                        equidistant_walk_steps=args.equidistant_walk_steps,
                        mobility_bbox_margin=args.mobility_bbox_margin,
                        mobility_bbox_enabled=args.mobility_bbox_toggle,
                        mobility_history_limit=args.mobility_history_limit,
                        resource_limits=ResourceLimits(**{
                            name: getattr(args, name) for name in vars(ResourceLimits())}))
        env.run()

        if args.single_run:
            break
