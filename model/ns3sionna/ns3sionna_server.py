import sionna.rt # must be the first import as otherwise Python crashes
import drjit as dr
# import traceback
# dr.set_flag(dr.JitFlag.Debug, True)

import os
import argparse
import numpy as np
import time
import GPUtil
import zmq
import gc

from common import message_pb2
from common.message_debug import *

from collections import deque
import warnings

import tensorflow as tf
import mitsuba as mi
import math
from millify import millify

from ns3sionna_utils import subcarrier_frequencies, compute_coherence_time, SECOND, MILLISECOND, coherence_from_velocities, \
    MAX_COHERENCE_TIME

import sionna
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

    """
    This class represents the Sionna component of ns3sionna. It represents the environment where the node
    placement, mobility is controlled from the client component of ns3sionna. For IPC ZMQ is used.

    author: Pilz, Zubow
    """
    def __init__(self, model_folder='./models/', rt_fast=False, default_mode=MODE_P2P, rt_max_parallel_links=256, est_csi=True, VERBOSE=True,
                 CHECKS_ENABLED=True):
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

        #todo: diffuse reflection next, activate refraction again
        # default mode
        self.default_mode = default_mode

        # maximum number of parallel computations (needed to fit GPU)
        self.rt_max_parallel_links = rt_max_parallel_links

        # estimate small-scale fading
        self.est_csi = est_csi

        print(f'Init ns3sionna with rt_fast={rt_fast}, est_csi={est_csi}')

        self.VERBOSE = VERBOSE
        self.CHECKS_ENABLED = CHECKS_ENABLED

        # check GPU support
        self.gpus = tf.config.list_physical_devices("GPU")

        if len(self.gpus) > 0:
            print("TensorFlow GPU devices:", self.gpus)
        else:
            print("No TensorFlow GPU devices visible")

        print("Mitsuba backend:", mi.variant())

        self.disp_r = 10
        # storing information about every node under simulation
        self.node_info = {}
        # all node which are currently placed on the scene
        self.placed_radio_node_names = []


    def init_simulation_env(self, sim_init_msg):
        '''
        Initializes the Sionna environment
        :param sim_init_msg: the received ZMQ message
        :return: (success, error_msg)
        '''
        if self.VERBOSE:
            print_sim_init(sim_init_msg)

        # self.stepCounter = 0

        # Load the sionna scene
        filepath = os.path.join(self.model_folder, sim_init_msg.scene_fname)
        try:
            self.scene = load_scene(filepath)
            self.p_solver  = PathSolver()
        except Exception as e:
            # Print full traceback to console so the cause is visible
            print("Exception while loading scene:", filepath)
            traceback.print_exc()
            return False, "Failed to load scene file in: " + filepath + ", error: " + str(e) + " (see server console for traceback)"

        self.bbox = self.scene.mi_scene.bbox()

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
        if sim_init_msg.min_coherence_time_ms != 0:
            self.min_coherence_time_ms = sim_init_msg.min_coherence_time_ms * 100000 # min Tc, multiplied for 0.1ms
        else:
            self.min_coherence_time_ms = 0
        self.subcarrier_spacing = sim_init_msg.subcarrier_spacing # in Hz
        self.mobility_trace_csv = sim_init_msg.mobility_trace_csv
        self.mobility_trace_interval_ns = sim_init_msg.mobility_trace_interval_ns
        self.sim_end_time_ns = sim_init_msg.sim_end_time_ns

        print(f'Operating in mode: {self.mode}, sub_mode: {self.sub_mode}, time_evo_model: {self.time_evo_model}'
              f', fc: {sim_init_msg.frequency} MHz, B: {sim_init_msg.channel_bw} MHz, FFT size: {self.fft_size}')

        # Subcarrier frequencies
        self.frequencies = subcarrier_frequencies(num_subcarriers=self.fft_size, subcarrier_spacing=self.subcarrier_spacing)

        # Set the random seed for reproducibility
        np.random.seed(sim_init_msg.seed)
        tf.random.set_seed(sim_init_msg.seed)
        self.my_seed = sim_init_msg.seed

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
            #if self.min_coherence_time_ms != 0:
            #   self.chan_coh_time_mode3 = self.min_coherence_time_ms

            print(f'Running mode=3 w/ Tc: {self.chan_coh_time_mode3/1e6}ms')

        # Configure antenna array for all transmitters/receivers
        self.scene.tx_array = PlanarArray(num_rows=1, num_cols=1, vertical_spacing=0.5, horizontal_spacing=0.5,
                                     pattern="tr38901", polarization="V")

        self.scene.rx_array = PlanarArray(num_rows=1, num_cols=1, vertical_spacing=0.5, horizontal_spacing=0.5,
                                     pattern="dipole", polarization="V")

        # set current sim time to 0ns
        self.sim_time = 0

        return True, "OK"

    def _make_mobility_rng(self, node_id):
        seed_sequence = np.random.SeedSequence([int(self.my_seed), int(node_id), 0x51A0])
        return np.random.default_rng(seed_sequence)

    def _advance_nodes_to(self, target_time, node_ids=None):
        if node_ids is None:
            node_ids = list(self.node_info.keys())

        for node_id in node_ids:
            self._advance_node_to(node_id, float(target_time))

    def _advance_node_to(self, node_id, target_time):
        node = self.node_info[node_id]
        if target_time <= node.last_update + 1e-9:
            return

        if isinstance(node, ConstantMobility):
            node.update_pos(target_time, node.pos.copy(), node.velocity.copy(), False)
            return

        while node.last_update + 1e-9 < target_time:
            segment_start_time = node.last_update
            next_velocity_change = node.time_to_next_velocity_change()
            velocity_change_time = segment_start_time + next_velocity_change if np.isfinite(next_velocity_change) else np.inf
            segment_end_time = min(target_time, velocity_change_time)

            self._walk_segment(node_id, segment_end_time)
            node = self.node_info[node_id]

            if np.isfinite(velocity_change_time) and abs(segment_end_time - velocity_change_time) <= 1e-6:
                node.resample_velocity(segment_end_time)

    def _get_first_wall_hit(self, pos, direction, max_distance):
        ray = mi.Ray3f(mi.Point3f(pos), mi.Vector3f(direction))
        ray.maxt = mi.Float(max_distance)

        si = self.scene.mi_scene.ray_intersect(ray, mi.RayFlags.Minimal, False, True)
        if not si.is_valid():
            return None, None

        hit_distance = float(np.squeeze(si.t.numpy()))
        surface_normal = np.squeeze(si.n.numpy())
        planar_normal = np.array([surface_normal[1], -surface_normal[0], 0.0], dtype=float)
        norm = np.linalg.norm(planar_normal)
        if norm == 0.0:
            return None, None

        return hit_distance, planar_normal / norm

    @staticmethod
    def _reflect_direction(direction, normal, mobility_theta):
        reflected = -(direction - 2 * (np.dot(direction, normal) + mobility_theta) * normal)
        reflected_norm = np.linalg.norm(reflected)
        if reflected_norm == 0.0:
            return -direction
        return reflected / reflected_norm

    def _walk_segment(self, node_id, target_time):
        node = self.node_info[node_id]
        if target_time <= node.last_update + 1e-9:
            return

        while node.last_update + 1e-9 < target_time:
            pos = node.pos.copy()
            velocity = node.velocity.copy()
            speed = np.linalg.norm(velocity)
            if speed == 0.0:
                node.update_pos(target_time, pos, velocity, False)
                return

            direction = velocity / speed
            remaining_time = target_time - node.last_update
            remaining_distance = speed * remaining_time / 1e9

            hit_distance, normal = self._get_first_wall_hit(pos, direction, remaining_distance)
            if hit_distance is None or hit_distance >= remaining_distance:
                node.register_travel(remaining_distance)
                next_pos = pos + velocity * remaining_time / 1e9
                node.update_pos(target_time, next_pos, velocity, False)
                return

            if hit_distance <= 1e-9:
                node.update_pos(target_time, pos, velocity, False)
                return

            clearance = min(0.01, hit_distance * 0.5)
            travel_distance = max(hit_distance - clearance, 0.0)
            travel_time = (travel_distance / speed) * 1e9
            collision_time = node.last_update + travel_time

            node.register_travel(travel_distance)
            collision_pos = pos + direction * travel_distance

            next_direction = self._reflect_direction(direction, normal, node.get_next_direction_angle())
            next_velocity = next_direction * speed
            node.update_pos(collision_time, collision_pos, next_velocity, True)


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


    def compute_cfr_with_lookahead(self, csi_req, reply_wrapper):
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
        nodes_to_update = list(self.node_info.keys())

        # compute look-ahead
        look_ahead = math.floor(self.sub_mode / (len(nodes_to_update) - 1))

        print(f'compute CFR to #RX={len(nodes_to_update) - 1} with LAH={look_ahead}')

        # sim future node positions
        lah_time_vec = []
        for lah_i in range(look_ahead):
            self._advance_nodes_to(req_sim_time, nodes_to_update)
            csi_tc_arr = []
            for node_id in nodes_to_update:
                if node_id != tx_node_id:
                    tc = coherence_from_velocities(self.node_info[node_id].velocity,
                                                    self.node_info[tx_node_id].velocity, self.fc,
                                                    pos_tx=self.node_info[node_id].pos,
                                                    pos_rx=self.node_info[tx_node_id].pos)
                    # JZA: Set min Tc
                    if self.min_coherence_time_ms != 0:
                        tc = self.min_coherence_time_ms
                    '''
                    if tc < self.min_coherence_time_ms:
                        tc = self.min_coherence_time_ms
                    '''
                    csi_tc_arr.append(tc)

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

        self._place_tx_rx_nodes_with_lah(lah_time_vec, tx_node_id, rx_nodes)

        # Compute propagation paths
        paths = self.p_solver(scene=self.scene,
                         max_depth=self.rt_max_depth,
                         samples_per_src=self.rt_samples_per_src,
                         los=self.rt_los,
                         specular_reflection=self.rt_specular_reflection,  # Can rays bounce off surfaces?
                         diffuse_reflection=self.rt_diffuse_reflection,
                         refraction=self.rt_refraction,  # Can rays pass through materials?
                         synthetic_array=self.rt_synthetic_array,
                         diffraction=self.rt_diffraction,  # costly
                         edge_diffraction=self.rt_edge_diffraction,  # rays that bend around edges
                         diffraction_lit_region=self.rt_diffraction_lit_region)  # higher physical accuracy

        # AZU: sampling_frequency is only used if num_time_steps > 1
        # a: shape [num_rx, num_rx_ant, num_tx, num_tx_ant, num_paths, num_time_steps],
        a, tau = paths.cir(sampling_frequency=1e9, normalize_delays=False, out_type="numpy")
        dr.eval()

        # shape: [num_rx, num_rx_ant, num_tx, num_tx_ant, num_ofdm_symbols, num_subcarriers]
        h_raw = paths.cfr(frequencies=self.frequencies,
                  sampling_frequency=1.0,  # not used
                  num_time_steps=1,
                  normalize_delays=True,
                  # If set to True, path delays are normalized such that the first path between any pair of
                  # antennas of a transmitter and receiver arrives at tau=0
                  normalize=False,  # Normalize energy
                  out_type="numpy")
        dr.eval()

        del paths          # <-- explicitly release GPU buffers
        dr.sync_thread()   # <-- wait for deallocation to complete

        # max single transmitter
        assert h_raw.shape[2] == 1
        num_computed_lnks = h_raw.shape[0]

        # Create ZMQ response
        chan_response = reply_wrapper.channel_state_response

        Tc_p2mp_lah = []
        for lah_time_idx, lah_time in enumerate(lah_time_vec): # iterate over time
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

                lnk_tau = np.squeeze(tau[rx_id, :, :, :, :])
                lnk_delay = int(round(np.min(lnk_tau[lnk_tau >= 0] * 1e9), 0))

                h = np.squeeze(h_raw[rx_id, :, :, :, :, ])

                # see Parseval's theorem
                lnk_loss = float(-10 * np.log10(np.mean(np.abs(h) ** 2)))

                # for frequency-selective channel
                power = np.mean(np.abs(h) ** 2)  # shape [batch_size, 1, 1, 1]

                h_normalized = h / np.sqrt(power)

                # plausibility test
                if self.CHECKS_ENABLED:
                    power_normalized = np.mean(np.abs(h_normalized) ** 2)
                    assert math.isclose(power_normalized, 1.0, rel_tol=1e-3)   # Should be close to 1

                if self.VERBOSE:
                    print(f'{lah_time/1e9}s: {tx_node_id}->{curr_rx_node} lnk_delay = {lnk_delay}ns, wb_loss = {lnk_loss:.3f}dB, CFR shape: {h_normalized.shape}')

                rx_node_info = csi.rx_nodes.add()
                rx_pos = self.node_info[curr_rx_node].get_pos_at(lah_time)
                rx_node_info.id = curr_rx_node
                rx_node_info.position.x = rx_pos[0]
                rx_node_info.position.y = rx_pos[1]
                rx_node_info.position.z = rx_pos[2]
                rx_node_info.delay = lnk_delay
                rx_node_info.wb_loss = lnk_loss

                if self.est_csi:
                    rx_node_info.frequencies.extend(self.frequencies.tolist())
                    rx_node_info.csi_imag.extend(np.imag(h_normalized).tolist())
                    rx_node_info.csi_real.extend(np.real(h_normalized).tolist())

                tc = coherence_from_velocities(self.node_info[curr_rx_node].get_velo_at(lah_time),
                                                self.node_info[tx_node_id].velocity, self.fc,
                                                pos_tx=self.node_info[curr_rx_node].get_pos_at(lah_time),
                                                pos_rx=self.node_info[tx_node_id].pos)

                if self.min_coherence_time_ms != 0:
                    tc = self.min_coherence_time_ms
                '''
                if tc < self.min_coherence_time_ms:
                    tc = self.min_coherence_time_ms
                '''

                rx_node_info.end_time2 = csi.start_time + tc
                csi_tc_arr.append(tc)

            # take the worst case Tc from all RX nodes
            Tc_p2mp = int(np.min(np.asarray(csi_tc_arr)))
            Tc_p2mp_lah.append(Tc_p2mp)
            csi.end_time = csi.start_time + Tc_p2mp - 1 # -1ns to have non-overlapping intervals

        # print(f'{self.sim_time / 1e9}s: Computed CSI with Tc: {np.round(np.asarray(Tc_p2mp_lah) / 1e6,2)}ms, #links: {num_computed_lnks}')

        return num_computed_lnks


    def _place_tx_rx_nodes_with_lah(self, lah_time_vec: list, tx_node: int, rx_nodes: list):
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
        tx = Transmitter(name=tx_node_name, position=tx_pos, orientation=[0, -180, 0], display_radius=self.disp_r)
        self.scene.add(tx)
        self.placed_radio_node_names.append(tx_node_name)

        for lah_time_idx, lah_time in enumerate(lah_time_vec):
            # Create a receiver(s)
            for rx_node_i in rx_nodes:
                rx_node_name = "rx" + str(rx_node_i) + "." + str(lah_time_idx)
                rx_pos = self.node_info[rx_node_i].get_pos_at(lah_time)
                rx = Receiver(name=rx_node_name, position=rx_pos, orientation=[0, -180, 0], display_radius=self.disp_r)
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
                rx_node_info.frequencies.extend(self.frequencies.tolist())
                rx_node_info.csi_imag.extend(np.imag(h_normalized[idx]).tolist())
                rx_node_info.csi_real.extend(np.real(h_normalized[idx]).tolist())

            # compute coherence time: with direction vectors you can compute the radial (projected) relative
            # speed directly and from that the Doppler and coherence time.
            tc = coherence_from_velocities(self.node_info[comp_rx_node_id].velocity,
                                            self.node_info[tx_node_id].velocity, self.fc,
                                            pos_tx=self.node_info[comp_rx_node_id].pos,
                                            pos_rx=self.node_info[tx_node_id].pos)
            # JZA: This is used most often
            if self.min_coherence_time_ms != 0:
                tc = self.min_coherence_time_ms
                # print("TC SET")
            '''
            if tc < self.min_coherence_time_ms:
                tc = self.min_coherence_time_ms
            '''
            # print("Current TC: ", tc)
            rx_node_info.end_time2 = tc
            csi_tc_arr.append(tc)

        # take the worst case Tc from all RX nodes
        Tc_p2mp = int(np.min(np.asarray(csi_tc_arr)))

        # print(f'{self.sim_time / 1e9}s: Computed CSI with Tc: {round(Tc_p2mp / 1e6,2)}ms, #links: {len(rx_nodes)}')

        csi.end_time = self.sim_time + Tc_p2mp
        return len(rx_nodes)


    def _walk(self, node_id, dt):
        """
        Move the given node to the given time interval.
        :param node_id: node_id of the node to move
        :param dt: time interval in ns
        """
        dr.eval()
        dr.sync_thread()
        self._advance_node_to(node_id, self.node_info[node_id].last_update + dt)


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
        tx = Transmitter(name=tx_node_name, position=tx_pos, orientation=[0, -180, 0], display_radius=self.disp_r)
        self.scene.add(tx)
        self.placed_radio_node_names.append(tx_node_name)

        # Create a receiver(s)
        for rx_node_i in rx_nodes:
            rx_node_name = "rx" + str(rx_node_i)
            rx_pos = self.node_info[rx_node_i].pos
            rx = Receiver(name=rx_node_name, position=rx_pos, orientation=[0, -180, 0], display_radius=self.disp_r)
            self.scene.add(rx)
            self.placed_radio_node_names.append(rx_node_name)


    def _compute_cfr_via_position(self, req_sim_time, tx_node, rx_node, req_mode):
        '''
        Compute the link propagation delay, wideband loss and normalized CFR
        :param req_sim_time: current simulation time
        :param tx_node: the transmitter node id
        :param rx_node: the receiver node id
        :return: (list(rx_node), list(link propagation delay), list(wideband loss), list(normalized CFR))
        '''

        # execute mobility
        # estimate the node we need to update their position
        if req_mode == SionnaEnv.MODE_P2P:
            nodes_to_update = [tx_node, rx_node] # only TX and RX
        else:
            # both P2MP and P2MP_LAH
            nodes_to_update = list(self.node_info.keys())

        self._advance_nodes_to(req_sim_time, nodes_to_update)

        # update time
        self.sim_time = req_sim_time

        # place TX and RX
        rx_nodes = nodes_to_update
        rx_nodes.remove(tx_node)
        self._place_tx_rx_node(tx_node, rx_nodes)

        # Compute propagation paths
        paths = self.p_solver(scene=self.scene,
                         max_depth=self.rt_max_depth,
                         samples_per_src=self.rt_samples_per_src,
                         los=self.rt_los,
                         specular_reflection=self.rt_specular_reflection,  # Can rays bounce off surfaces?
                         diffuse_reflection=self.rt_diffuse_reflection,
                         refraction=self.rt_refraction,  # Can rays pass through materials?
                         synthetic_array=self.rt_synthetic_array,
                         diffraction=self.rt_diffraction,  # costly
                         edge_diffraction=self.rt_edge_diffraction,  # rays that bend around edges
                         diffraction_lit_region=self.rt_diffraction_lit_region)  # higher physical accuracy

        # AZU: sampling_frequency is only used if num_time_steps > 1
        # a: shape [num_rx, num_rx_ant, num_tx, num_tx_ant, num_paths, num_time_steps],
        a, tau = paths.cir(sampling_frequency=1e9, normalize_delays=False, out_type="numpy")
        dr.eval()

        # shape: [num_rx, num_rx_ant, num_tx, num_tx_ant, num_ofdm_symbols, num_subcarriers]
        h_raw = paths.cfr(frequencies=self.frequencies,
                  sampling_frequency=1.0,  # not used
                  num_time_steps=1,
                  normalize_delays=True,
                  # If set to True, path delays are normalized such that the first path between any pair of
                  # antennas of a transmitter and receiver arrives at tau=0
                  normalize=False,  # Normalize energy
                  out_type="numpy")
        dr.eval()

        del paths          # <-- explicitly release GPU buffers
        dr.sync_thread()   # <-- wait for deallocation to complete

        lnk_delay_arr = []
        lnk_loss_arr = []
        h_normalized_arr = []

        for rx_id, curr_rx_node in enumerate(rx_nodes):
            lnk_tau = np.squeeze(tau[rx_id, :, :, :, :])
            lnk_delay = int(round(np.min(lnk_tau[lnk_tau >= 0] * 1e9), 0))

            h = np.squeeze(h_raw[rx_id, :, :, :, :, ])

            # see Parseval's theorem
            lnk_loss = float(-10 * np.log10(np.mean(np.abs(h) ** 2)))

            # for frequency-selective channel
            power = np.mean(np.abs(h) ** 2)  # shape [batch_size, 1, 1, 1]

            h_normalized = h / np.sqrt(power)

            # plausibility test
            if self.CHECKS_ENABLED:
                power_normalized = np.mean(np.abs(h_normalized) ** 2)
                assert math.isclose(power_normalized, 1.0, rel_tol=1e-3)   # Should be close to 1

            if self.VERBOSE:
                print(f'{self.sim_time/1e9}s: lnk_delay = {lnk_delay}ns, wb_loss = {lnk_loss:.3f}dB, CFR shape: {h_normalized.shape}')

            lnk_delay_arr.append(lnk_delay)
            lnk_loss_arr.append(lnk_loss)
            h_normalized_arr.append(h_normalized)

        return rx_nodes, lnk_delay_arr, lnk_loss_arr, h_normalized_arr

    def _export_mobility_trace(self):
        if not self.mobility_trace_csv or self.mobility_trace_interval_ns == 0 or self.sim_end_time_ns == 0:
            return

        self._advance_nodes_to(self.sim_end_time_ns)

        trace_dir = os.path.dirname(self.mobility_trace_csv)
        if trace_dir:
            os.makedirs(trace_dir, exist_ok=True)

        node_ids = sorted(
            node_id for node_id, node in self.node_info.items() if isinstance(node, RandomWalkMobility)
        )
        if not node_ids:
            node_ids = sorted(self.node_info.keys())
        sample_times = list(np.arange(0, self.sim_end_time_ns + self.mobility_trace_interval_ns, self.mobility_trace_interval_ns))
        if not sample_times or sample_times[-1] != self.sim_end_time_ns:
            sample_times.append(self.sim_end_time_ns)

        with open(self.mobility_trace_csv, "w", encoding="ascii") as trace_file:
            trace_file.write("Time_s,STA_NodeId,pos_x,pos_y,pos_z\n")
            for sample_time in sample_times:
                for node_id in node_ids:
                    pos = self.node_info[node_id].get_pos_at(sample_time)
                    trace_file.write(
                        f"{sample_time / 1e9:.9f},{node_id},{pos[0]},{pos[1]},{pos[2]}\n"
                    )

        print(f"Mobility trace written to {self.mobility_trace_csv}")

    def _get_mobility_history(self, node_id):
        ts = list(self.node_info[node_id].history_times)
        pos = [self.node_info[node_id].pos_history[t] for t in ts]
        return ts, pos


    def _init_mobility(self, sim_init_msg):
        # Store information about each node: ID, mobility model
        for node_info in sim_init_msg.nodes:
            if (node_info.HasField("constant_position_model")):
                # fixed position; no mobility
                pos = node_info.constant_position_model.position
                self.node_info[node_info.id] = ConstantMobility(node_info.id, [pos.x, pos.y, pos.z])
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

                self.node_info[node_info.id] = RandomWalkMobility(node_info.id, [pos.x, pos.y, pos.z],
                                                                  mode, mode_params, speed, speed_params,
                                                                  direction, direction_params,
                                                                  rng=self._make_mobility_rng(node_info.id))


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
                successful, error_msg = self.init_simulation_env(ns3_msg.sim_init_msg)
                resp_msg.sim_ack.no_error = successful
                resp_msg.sim_ack.error_msg = error_msg
                resp_msg.sim_ack.SetInParent()

                if successful:
                    print("Sionna server init sucessful ...")
                else:
                    print("Sionna server init failed ...")
                    # Print detailed error message returned by init_simulation_env
                    if error_msg:
                        print("Error message:\n", error_msg)
                    do_terminate = True

            elif ns3_msg.HasField("channel_state_request"):
                # handle ChannelStateRequest by sending ChannelStateResponse
                start_time = time.time()
                num_csi_req = self.compute_cfr(ns3_msg.channel_state_request, resp_msg)
                total_num_csi_samples += num_csi_req
                last_call_times.append(time.time() - start_time)

                if total_num_csi_samples % 1000 == 0: # every 1k make printout
                    print(f'Total no. computed CSI samples: {millify(total_num_csi_samples)}')

                if self.VERBOSE:
                    avg_call_time = sum(last_call_times) / len(last_call_times)
                    print("t=%.9fs: average event processing time: %.2f sec"
                          % (ns3_msg.channel_state_request.time/1e9, avg_call_time))
                    # show GPU load
                    if len(self.gpus) > 0:
                        GPUtil.showUtilization()

            elif ns3_msg.HasField("sim_close_request"):
                self._export_mobility_trace()
                do_terminate = True
                resp_msg.sim_ack.SetInParent()

            # Serialize and send the reply message
            socket.send(resp_msg.SerializeToString())

        """ No step counter needed anymore
        try:
            f = open('stepcounter.txt', 'a')
            f.write(repr(self.stepCounter) + "\n")
            f.close()
        except FileExistsError:
            print("Something went wrong")
        """

        socket.close()
        print("Computed no. CSI samples: %d" % total_num_csi_samples)
        print("Sionna server socket closed.")


    def release(self):
        # delete / release the scene before loading a new one
        del self.scene
        del self.p_solver
        dr.sync_thread()
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
    args = parser.parse_args()

    print("ns3sionna v1.0")
    while True:
        print("Using config: model_folder=%s, single_run=%s, mode=%d, rt_fast=%s, rt_max_parallel_links=%d, est_csi=%r"
              % (args.model_folder, args.single_run, args.default_mode, args.rt_fast, args.rt_max_parallel_links, args.est_csi))
        print("Waiting for new job ...")
        env = SionnaEnv(args.model_folder, args.rt_fast, args.default_mode, args.rt_max_parallel_links,
                        args.est_csi, VERBOSE=args.verbose)
        env.run()

        if args.single_run:
            break
