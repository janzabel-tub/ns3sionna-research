import faulthandler
faulthandler.enable()
faulthandler.dump_traceback_later(60, repeat=True)
import sionna.rt # must be the first import as otherwise Python crashes
import drjit as dr
dr.set_flag(dr.JitFlag.Debug, True)

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

from ns3sionna_utils import subcarrier_frequencies, compute_coherence_time, SECOND, MILLISECOND, coherence_from_velocities, MAX_COHERENCE_TIME

import sionna
from sionna.rt import load_scene, Camera, Transmitter, Receiver, PlanarArray, PathSolver

from mobility import *

###
# Reuse receiver, no more self.scene.add/remove
# Immediatlely evaluate its mathematical graphs
# Garbage collect the memory buffer
# 3D scene cached globally, reuse scene between simulations (This might not be a good idea)
# Check if previous simulation used same scene, if yes, reuse
# Assert distance is larger than 0, angle is not NaN
# Have two different scenes for Rays and collisions: Never modified, GPU pipeline calculated once, no corruption
# Both Gemini and Claude think it has something to do with SBT (Shader binding table)
# Claudes opinion: Cachine the scene is bad because the SBT mutates and grows, so the scene should be deleted
# TODO: Undo scene caching this
# TODO: Claude recommends adding receivers at the beginning, all of them, and just placing the not needed ones far away.
# TODO: Verify if eval() at start of walk is useful, it makes sure the old scene is done before we issue new things
# My eval placement after cir and cfr makes sure the path tracing results are available before they are read
###

class SionnaEnv:

    MODE_P2P        = 1
    MODE_P2MP       = 2
    MODE_P2MP_LAH   = 3

    def __init__(self, model_folder='./models/', rt_fast=False, default_mode=MODE_P2P, rt_max_parallel_links=256, est_csi=True, VERBOSE=True, CHECKS_ENABLED=True):
        self.model_folder = model_folder
        self.rt_fast = rt_fast
        if rt_fast:
            self.rt_max_depth = 3
            self.rt_samples_per_src = 10 ** 6
            self.rt_los = True
            self.rt_specular_reflection = True
            self.rt_diffuse_reflection = False
            self.rt_refraction = True
            self.rt_synthetic_array = False
            self.rt_diffraction = False
            self.rt_edge_diffraction = False
            self.rt_diffraction_lit_region = False
        else:
            self.rt_max_depth = 5
            self.rt_samples_per_src = 10 ** 6
            self.rt_los = True
            self.rt_specular_reflection = True
            self.rt_diffuse_reflection = True
            self.rt_refraction = True
            self.rt_synthetic_array = False
            self.rt_diffraction = True
            self.rt_edge_diffraction = True
            self.rt_diffraction_lit_region = True

        self.default_mode = default_mode
        self.rt_max_parallel_links = rt_max_parallel_links
        self.est_csi = est_csi

        print(f'Init ns3sionna with rt_fast={rt_fast}, est_csi={est_csi}')

        self.VERBOSE = VERBOSE
        self.CHECKS_ENABLED = CHECKS_ENABLED

        self.gpus = tf.config.list_physical_devices("GPU")
        if len(self.gpus) > 0:
            print("GPU support detected; no GPUs:", self.gpus)
        else:
            print("Using CPU backend")

        # THE FIX: Set display radius to 0 so Sionna doesn't create geometric meshes for TX/RX!
        self.disp_r = 0
        self.node_info = {}

        self.scene = None
        self.p_solver = None
        self.current_filepath = None

    def init_simulation_env(self, sim_init_msg):
        if self.VERBOSE:
            print_sim_init(sim_init_msg)

        filepath = os.path.join(self.model_folder, sim_init_msg.scene_fname)

        # Reuse the scene or unload the old scene
        if self.current_filepath != filepath:
            if self.scene is not None:
                self.release_scene_resources()
            try:
                self.scene = load_scene(filepath)
                self.p_solver = PathSolver()
                self.current_filepath = filepath
                self.bbox = self.scene.mi_scene.bbox()
            except Exception as e:
                return False, f"Failed to load scene file {filepath}: {e}"
        else:
            if self.VERBOSE:
                print(f"Scene {sim_init_msg.scene_fname} already loaded. Reusing compiled JIT kernels...")

        if self.VERBOSE:
            dx = self.bbox.max.x - self.bbox.min.x
            dy = self.bbox.max.y - self.bbox.min.y
            dz = self.bbox.max.z - self.bbox.min.z
            print(f'Scenario with dx={dx:.2f}, dy={dy:.2f}, dz={dz:.2f}')

        self.mode = sim_init_msg.mode if sim_init_msg.mode > -1 else self.default_mode
        self.sub_mode = sim_init_msg.sub_mode if sim_init_msg.sub_mode > -1 else self.rt_max_parallel_links

        self.time_evo_model = sim_init_msg.time_evo_model
        self.scene.frequency = sim_init_msg.frequency * 1e6
        self.scene.bandwidth = sim_init_msg.channel_bw * 1e6
        self.fc = sim_init_msg.frequency * 1e6
        self.fft_size = sim_init_msg.fft_size

        if sim_init_msg.min_coherence_time_ms != 0:
            self.min_coherence_time_ms = sim_init_msg.min_coherence_time_ms * 100000
        else:
            self.min_coherence_time_ms = 0

        self.subcarrier_spacing = sim_init_msg.subcarrier_spacing

        print(f'Operating in mode: {self.mode}, sub_mode: {self.sub_mode}, time_evo_model: {self.time_evo_model}, fc: {sim_init_msg.frequency} MHz, B: {sim_init_msg.channel_bw} MHz, FFT size: {self.fft_size}')

        self.frequencies = subcarrier_frequencies(num_subcarriers=self.fft_size, subcarrier_spacing=self.subcarrier_spacing)

        np.random.seed(sim_init_msg.seed)
        tf.random.set_seed(sim_init_msg.seed)
        self.my_seed = sim_init_msg.seed

        self.node_info = {}
        self.sim_time = 0
        self._init_mobility(sim_init_msg)

        if self.mode == SionnaEnv.MODE_P2MP_LAH:
            speed_arr =[]
            for node_id in list(self.node_info.keys()):
                if isinstance(self.node_info[node_id], RandomWalkMobility):
                    if self.node_info[node_id].speed != RandomWalkMobility.SPEED_CONSTANT:
                        warnings.warn(f"Only constant speed model is supported when using mode P2MP(LAH); switching to mode P2P.", UserWarning)
                        self.mode = SionnaEnv.MODE_P2MP
                        break
                    else:
                        speed_arr.append(self.node_info[node_id].speed_params[0])
            speed_arr.sort(reverse=True)
            if len(speed_arr) >= 2:
                self.chan_coh_time_mode3 = compute_coherence_time(speed_arr[0] + speed_arr[1], self.fc, model='rappaport2')
            elif len(speed_arr) == 1:
                self.chan_coh_time_mode3 = compute_coherence_time(speed_arr[0], self.fc, model='rappaport2')
            else:
                self.chan_coh_time_mode3 = MAX_COHERENCE_TIME

            print(f'Running mode=3 w/ Tc: {self.chan_coh_time_mode3/1e6}ms')

        self.scene.tx_array = PlanarArray(num_rows=1, num_cols=1, vertical_spacing=0.5, horizontal_spacing=0.5, pattern="tr38901", polarization="V")
        self.scene.rx_array = PlanarArray(num_rows=1, num_cols=1, vertical_spacing=0.5, horizontal_spacing=0.5, pattern="dipole", polarization="V")

        return True, "OK"

    def compute_cfr(self, csi_req, reply_wrapper):
        tx_node_id = csi_req.tx_node
        rx_node_id = csi_req.rx_node

        if self.mode == SionnaEnv.MODE_P2MP_LAH:
            if isinstance(self.node_info[tx_node_id], RandomWalkMobility) and isinstance(self.node_info[rx_node_id], ConstantMobility):
                csi_req.tx_node = rx_node_id
                csi_req.rx_node = tx_node_id

        if self.mode == SionnaEnv.MODE_P2MP_LAH and isinstance(self.node_info[tx_node_id], ConstantMobility):
            return self.compute_cfr_with_lookahead(csi_req, reply_wrapper)
        else:
            req_mode = self.mode
            if self.mode == SionnaEnv.MODE_P2MP_LAH:
                req_mode = SionnaEnv.MODE_P2MP
                print(f'Fallback to mode={req_mode} as TX node is mobile')

            return self.compute_cfr_classic(csi_req, reply_wrapper, req_mode)

    def compute_cfr_with_lookahead(self, csi_req, reply_wrapper):
        if self.VERBOSE:
            print_csi_request(csi_req)

        tx_node_id = csi_req.tx_node
        rx_node_id = csi_req.rx_node
        req_sim_time = csi_req.time

        assert self.time_evo_model == 'position'

        nodes_to_update = list(self.node_info.keys())
        look_ahead = math.floor(self.sub_mode / (len(nodes_to_update) - 1))

        lah_time_vec =[]
        for lah_i in range(look_ahead):
            dt = req_sim_time - self.sim_time

            csi_tc_arr =[]
            for node_id in nodes_to_update:
                self._walk(node_id, dt)
                if node_id != tx_node_id:
                    tc = coherence_from_velocities(self.node_info[node_id].velocity, self.node_info[tx_node_id].velocity, self.fc, pos_tx=self.node_info[node_id].pos, pos_rx=self.node_info[tx_node_id].pos)
                    if self.min_coherence_time_ms != 0:
                        tc = self.min_coherence_time_ms
                    csi_tc_arr.append(tc)

            Tc_p2mp = int(np.min(np.asarray(csi_tc_arr)))

            self.sim_time = req_sim_time
            lah_time_vec.append(req_sim_time)
            req_sim_time = req_sim_time + Tc_p2mp

        rx_nodes = nodes_to_update
        rx_nodes.remove(tx_node_id)

        self._place_tx_rx_nodes_with_lah(lah_time_vec, tx_node_id, rx_nodes)

        paths = self.p_solver(scene=self.scene, max_depth=self.rt_max_depth, samples_per_src=self.rt_samples_per_src,
                              los=self.rt_los, specular_reflection=self.rt_specular_reflection, diffuse_reflection=self.rt_diffuse_reflection,
                              refraction=self.rt_refraction, synthetic_array=self.rt_synthetic_array, diffraction=self.rt_diffraction,
                              edge_diffraction=self.rt_edge_diffraction, diffraction_lit_region=self.rt_diffraction_lit_region)

        a, tau = paths.cir(sampling_frequency=1e9, normalize_delays=False, out_type="numpy")
        dr.eval()

        h_raw = paths.cfr(frequencies=self.frequencies, sampling_frequency=1.0, num_time_steps=1,
                          normalize_delays=True, normalize=False, out_type="numpy")
        dr.eval()

        del paths
        dr.sync_thread()

        num_computed_lnks = h_raw.shape[0]

        chan_response = reply_wrapper.channel_state_response
        Tc_p2mp_lah =[]
        for lah_time_idx, lah_time in enumerate(lah_time_vec):
            csi = chan_response.csi.add()

            csi.start_time = lah_time
            tx_pos = self.node_info[tx_node_id].pos
            csi.tx_node.id = tx_node_id
            csi.tx_node.position.x = tx_pos[0]
            csi.tx_node.position.y = tx_pos[1]
            csi.tx_node.position.z = tx_pos[2]

            csi_tc_arr =[]
            for curr_rx_id, curr_rx_node in enumerate(rx_nodes):
                rx_id = lah_time_idx * len(rx_nodes) + curr_rx_id
                lnk_tau = np.squeeze(tau[rx_id, :, :, :, :])
                lnk_delay = int(round(np.min(lnk_tau[lnk_tau >= 0] * 1e9), 0))
                h = np.squeeze(h_raw[rx_id, :, :, :, :, ])

                lnk_loss = float(-10 * np.log10(np.mean(np.abs(h) ** 2)))
                power = np.mean(np.abs(h) ** 2)
                h_normalized = h / np.sqrt(power)

                if self.VERBOSE:
                    print(f'{lah_time/1e9}s: {tx_node_id}->{curr_rx_node} lnk_delay = {lnk_delay}ns, wb_loss = {lnk_loss:.3f}dB')

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

                rx_node_info.end_time2 = csi.start_time + tc
                csi_tc_arr.append(tc)

            Tc_p2mp = int(np.min(np.asarray(csi_tc_arr)))
            Tc_p2mp_lah.append(Tc_p2mp)
            csi.end_time = csi.start_time + Tc_p2mp - 1

        return num_computed_lnks

    def _sync_scene_radio_nodes(self, tx_pos, rx_pos_list):
        if not hasattr(self, '_tx'):
            self._tx = Transmitter(name="tx", position=tx_pos, orientation=[0, -180, 0], display_radius=self.disp_r)
            self.scene.add(self._tx)
        else:
            self._tx.position = tx_pos

        if not hasattr(self, '_rxs'):
            self._rxs =[]

        while len(self._rxs) < len(rx_pos_list):
            rx = Receiver(name=f"rx_{len(self._rxs)}", position=[0,0,0], orientation=[0, -180, 0], display_radius=self.disp_r)
            self.scene.add(rx)
            self._rxs.append(rx)

        while len(self._rxs) > len(rx_pos_list):
            rx = self._rxs.pop()
            self.scene.remove(rx.name)

        for rx, pos in zip(self._rxs, rx_pos_list):
            rx.position = pos

    def _place_tx_rx_nodes_with_lah(self, lah_time_vec: list, tx_node: int, rx_nodes: list):
        tx_pos = self.node_info[tx_node].pos
        rx_pos_list =[]
        for lah_time in lah_time_vec:
            for rx_node_i in rx_nodes:
                rx_pos_list.append(self.node_info[rx_node_i].get_pos_at(lah_time))

        self._sync_scene_radio_nodes(tx_pos, rx_pos_list)

    def _place_tx_rx_node(self, tx_node: int, rx_nodes: list):
        tx_pos = self.node_info[tx_node].pos
        rx_pos_list =[self.node_info[rx_node_i].pos for rx_node_i in rx_nodes]
        self._sync_scene_radio_nodes(tx_pos, rx_pos_list)

    def compute_cfr_classic(self, csi_req, reply_wrapper, req_mode):
        if self.VERBOSE:
            print_csi_request(csi_req)

        tx_node_id = csi_req.tx_node
        rx_node_id = csi_req.rx_node
        req_sim_time = csi_req.time

        if self.time_evo_model == 'doppler':
            pass
        else:
            (rx_nodes, lnk_delay, lnk_loss, h_normalized) = self._compute_cfr_via_position(req_sim_time, tx_node_id, rx_node_id, req_mode)

        chan_response = reply_wrapper.channel_state_response
        csi = chan_response.csi.add()

        csi.start_time = self.sim_time
        tx_pos = self.node_info[tx_node_id].pos
        csi.tx_node.id = tx_node_id
        csi.tx_node.position.x = tx_pos[0]
        csi.tx_node.position.y = tx_pos[1]
        csi.tx_node.position.z = tx_pos[2]

        csi_tc_arr =[]
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

            tc = coherence_from_velocities(self.node_info[comp_rx_node_id].velocity, self.node_info[tx_node_id].velocity, self.fc,
                                           pos_tx=self.node_info[comp_rx_node_id].pos, pos_rx=self.node_info[tx_node_id].pos)
            if self.min_coherence_time_ms != 0:
                tc = self.min_coherence_time_ms

            rx_node_info.end_time2 = tc
            csi_tc_arr.append(tc)

        Tc_p2mp = int(np.min(np.asarray(csi_tc_arr)))

        csi.end_time = self.sim_time + Tc_p2mp
        return len(rx_nodes)

    def _walk(self, node_id, dt):
        dr.eval()
        dr.sync_thread()

        init_dt = dt
        pos = self.node_info[node_id].pos
        velocity = self.node_info[node_id].velocity

        speed = np.linalg.norm(velocity)
        if speed == 0:
            return

        direction = velocity / speed
        distance = np.linalg.norm(velocity * dt / 1e9)

        while distance > 1e-6:
            ray = mi.Ray3f(mi.Point3f(pos), mi.Vector3f(direction))
            ray.maxt = mi.Float(distance)

            si = self.scene.mi_scene.ray_intersect(ray, mi.RayFlags.Minimal, False, True)
            dr.eval(si)

            is_valid = bool(si.is_valid().numpy()[0])

            if is_valid:
                t_np = si.t.numpy()
                t_raw = float(t_np[0])

                if math.isnan(t_raw):
                    del si
                    del ray
                    break

                t = max(0.0, t_raw - 0.01)
                pos = pos + t * direction

                n = np.squeeze(si.n.numpy())

                if np.any(np.isnan(n)):
                    direction = -direction
                else:
                    n = [n[1], -n[0], 0.0]
                    n_norm = np.linalg.norm(n)
                    if n_norm > 1e-6:
                        n = n / n_norm
                    else:
                        direction = -direction

                    mob_theta = self.node_info[node_id].get_next_direction_angle()
                    direction = - (direction - 2 * (np.dot(direction, n) + mob_theta) * n)

                dir_norm = np.linalg.norm(direction)
                if dir_norm > 1e-6:
                    direction = direction / dir_norm
                else:
                    direction = np.array([1.0, 0.0, 0.0])

                velocity = direction * speed

                step_dist = max(t_raw, 1e-4)
                distance -= step_dist
                dt -= (step_dist / speed) * 1e9

                del si
                del ray
            else:
                del si
                del ray
                break

        if dt < 0:
            dt = 0

        next_pos = pos + (velocity * dt / 1e9)

        self.node_info[node_id].update_pos(self.sim_time + init_dt, next_pos, velocity, False)
        self.node_info[node_id].check_set_new_velocity(self.sim_time + init_dt, distance)


    def _compute_cfr_via_position(self, req_sim_time, tx_node, rx_node, req_mode):
        dt = req_sim_time - self.sim_time

        if req_mode == SionnaEnv.MODE_P2P:
            nodes_to_update =[tx_node, rx_node]
        else:
            nodes_to_update = list(self.node_info.keys())

        for node_id in nodes_to_update:
            self._walk(node_id, dt)

        self.sim_time = req_sim_time

        rx_nodes = nodes_to_update
        rx_nodes.remove(tx_node)
        self._place_tx_rx_node(tx_node, rx_nodes)

        paths = self.p_solver(scene=self.scene, max_depth=self.rt_max_depth, samples_per_src=self.rt_samples_per_src,
                              los=self.rt_los, specular_reflection=self.rt_specular_reflection, diffuse_reflection=self.rt_diffuse_reflection,
                              refraction=self.rt_refraction, synthetic_array=self.rt_synthetic_array, diffraction=self.rt_diffraction,
                              edge_diffraction=self.rt_edge_diffraction, diffraction_lit_region=self.rt_diffraction_lit_region)

        a, tau = paths.cir(sampling_frequency=1e9, normalize_delays=False, out_type="numpy")
        dr.eval()

        h_raw = paths.cfr(frequencies=self.frequencies, sampling_frequency=1.0, num_time_steps=1,
                          normalize_delays=True, normalize=False, out_type="numpy")
        dr.eval()

        del paths
        dr.sync_thread()

        lnk_delay_arr =[]
        lnk_loss_arr =[]
        h_normalized_arr =[]

        for rx_id, curr_rx_node in enumerate(rx_nodes):
            lnk_tau = np.squeeze(tau[rx_id, :, :, :, :])
            lnk_delay = int(round(np.min(lnk_tau[lnk_tau >= 0] * 1e9), 0))

            h = np.squeeze(h_raw[rx_id, :, :, :, :, ])

            lnk_loss = float(-10 * np.log10(np.mean(np.abs(h) ** 2)))
            power = np.mean(np.abs(h) ** 2)

            h_normalized = h / np.sqrt(power)

            if self.CHECKS_ENABLED:
                power_normalized = np.mean(np.abs(h_normalized) ** 2)
                assert math.isclose(power_normalized, 1.0, rel_tol=1e-3)

            if self.VERBOSE:
                print(f'{self.sim_time/1e9}s: lnk_delay = {lnk_delay}ns, wb_loss = {lnk_loss:.3f}dB')

            lnk_delay_arr.append(lnk_delay)
            lnk_loss_arr.append(lnk_loss)
            h_normalized_arr.append(h_normalized)

        return rx_nodes, lnk_delay_arr, lnk_loss_arr, h_normalized_arr

    def _get_mobility_history(self, node_id):
        ts = sorted(self.node_info[node_id].pos_history.keys())
        pos =[self.node_info[node_id].pos_history[t] for t in ts]
        return ts, pos

    def _init_mobility(self, sim_init_msg):
        for node_info in sim_init_msg.nodes:
            if (node_info.HasField("constant_position_model")):
                pos = node_info.constant_position_model.position
                self.node_info[node_info.id] = ConstantMobility(node_info.id,[pos.x, pos.y, pos.z])
            elif (node_info.HasField("random_walk_model")):
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
                    direction_params = (random_walk_model.direction.uniform.min, random_walk_model.direction.uniform.max)
                elif random_walk_model.direction.HasField("constant"):
                    direction = RandomWalkMobility.DIRECTION_CONSTANT
                    direction_params = (random_walk_model.direction.constant.value,)
                elif random_walk_model.direction.HasField("normal"):
                    direction = RandomWalkMobility.DIRECTION_NORMAL
                    direction_params = (random_walk_model.direction.normal.mean, random_walk_model.direction.normal.variance)

                self.node_info[node_info.id] = RandomWalkMobility(node_info.id, [pos.x, pos.y, pos.z],
                                                                  mode, mode_params, speed, speed_params,
                                                                  direction, direction_params)

    def run(self, single_run=False):
        context = zmq.Context()
        socket = zmq.Socket(context, zmq.REP)
        socket.bind("tcp://*:5555")

        print("Sionna server socket ready ...")

        last_call_times = deque(maxlen=10)
        total_num_csi_samples = 0

        do_terminate = False
        while not do_terminate:
            ns3_msg_str = socket.recv()
            ns3_msg = message_pb2.Wrapper()
            ns3_msg.ParseFromString(ns3_msg_str)
            resp_msg = message_pb2.Wrapper()

            if ns3_msg.HasField("sim_init_msg"):
                successful, error_msg = self.init_simulation_env(ns3_msg.sim_init_msg)
                resp_msg.sim_ack.no_error = successful
                resp_msg.sim_ack.error_msg = error_msg
                resp_msg.sim_ack.SetInParent()

                if successful:
                    print("Sionna server init sucessful ...")
                else:
                    print("Sionna server init failed ...")
                    do_terminate = True

            elif ns3_msg.HasField("channel_state_request"):
                start_time = time.time()
                num_csi_req = self.compute_cfr(ns3_msg.channel_state_request, resp_msg)
                total_num_csi_samples += num_csi_req
                last_call_times.append(time.time() - start_time)

                if total_num_csi_samples % 1000 == 0:
                    print(f'Total no. computed CSI samples: {millify(total_num_csi_samples)}')

                if self.VERBOSE:
                    avg_call_time = sum(last_call_times) / len(last_call_times)
                    print("t=%.9fs: average event processing time: %.2f sec" % (ns3_msg.channel_state_request.time/1e9, avg_call_time))
                    if len(self.gpus) > 0:
                        GPUtil.showUtilization()

            elif ns3_msg.HasField("sim_close_request"):
                resp_msg.sim_ack.SetInParent()
                if single_run:
                    do_terminate = True
                else:
                    print(f"Simulation closed. Computed CSI samples so far: {total_num_csi_samples}. Waiting for next job...")

            socket.send(resp_msg.SerializeToString())

        socket.close()
        print("Computed no. CSI samples: %d" % total_num_csi_samples)
        print("Sionna server socket closed.")

    def release_scene_resources(self):
        if hasattr(self, '_tx'):
            del self._tx
        if hasattr(self, '_rxs'):
            for rx in self._rxs:
                del rx
            del self._rxs

        if self.scene is not None:
            del self.scene
            self.scene = None

        if self.p_solver is not None:
            del self.p_solver
            self.p_solver = None

        dr.sync_thread()
        gc.collect()

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
    print("Using config: model_folder=%s, single_run=%s, mode=%d, rt_fast=%s, rt_max_parallel_links=%d, est_csi=%r"
          % (args.model_folder, args.single_run, args.default_mode, args.rt_fast, args.rt_max_parallel_links, args.est_csi))
    print("Waiting for new job ...")

    env = SionnaEnv(args.model_folder, args.rt_fast, args.default_mode, args.rt_max_parallel_links,
                    args.est_csi, VERBOSE=args.verbose)

    env.run(single_run=args.single_run)

    env.release_scene_resources()
    del env
    gc.collect()
