"""Shape-safe conversion between Sionna CFR arrays and Protocol Buffers."""

from dataclasses import dataclass
import sys

import numpy as np


@dataclass(frozen=True)
class ResourceLimits:
    """Positive inclusive limits; wire planning is a conservative upper bound."""
    max_subcarriers: int = 4096
    max_rx_antennas: int = 64
    max_tx_antennas: int = 64
    max_coefficients_per_channel: int = 4194304
    max_batch_coefficients: int = 16777216
    max_reply_bytes: int = 268435456
    max_snapshots: int = 65536
    max_payload_bytes: int = 536870912

    def __post_init__(self):
        if any(type(value) is not int or value <= 0 for value in vars(self).values()):
            raise ValueError("Channel resource limits must be positive integers")

    def check(self, value, name):
        if value > getattr(self, "max_" + name):
            raise ValueError("Channel resource limit exceeded: " + name)
        return value

    def channel(self, sc, rx, tx):
        for value, name in ((sc, "subcarriers"), (rx, "rx_antennas"), (tx, "tx_antennas")):
            if value <= 0:
                raise ValueError("Channel dimensions must be positive")
            self.check(value, name)
        count = sc * rx * tx  # Python integers do not wrap.
        if count > sys.maxsize // 16:
            raise ValueError("CFR allocation bytes overflow size_t")
        return self.check(count, "coefficients_per_channel")

    def plan(self, sc, rx, tx, receivers, depth=1, identity_bytes=0, est_csi=True):
        count = self.channel(sc, rx, tx)
        if receivers <= 0 or depth <= 0:
            raise ValueError("Channel batch needs positive receivers and depth")
        snapshots = self.check(receivers * depth, "snapshots")
        batch_count = self.check(count * snapshots, "batch_coefficients")
        if batch_count > sys.maxsize // 16:
            raise ValueError("Batch CFR allocation bytes overflow size_t")
        self.check(snapshots * (16 * count + 4 * sc) if est_csi else 0, "payload_bytes")
        # Packed doubles: 16 bytes/coefficient; int32 frequencies can consume
        # ten bytes each (negative varints). Include nested tags/lengths,
        # positions, endpoints, times and optional array identity strings.
        wire_bound = 64 + depth * 256 + snapshots * (
            512 + identity_bytes + (16 * count + 10 * sc if est_csi else 0))
        self.check(wire_bound, "reply_bytes")
        return wire_bound


def lookahead_depth(sub_mode, receiver_count):
    """sub_mode limits parallel links, so actual depth is floor(sub_mode/R)."""
    if receiver_count <= 0:
        raise ValueError("Look-ahead requires at least one receiver")
    depth = sub_mode // receiver_count
    if depth <= 0:
        raise ValueError("Zero look-ahead depth: sub_mode must be at least receiver_count")
    return depth


def array_ports(config):
    return config["num_rows"] * config["num_cols"] * (2 if config["polarization"] == "VH" else 1)


TX_ARRAY_DEFAULTS = {
    "num_rows": 1,
    "num_cols": 1,
    "vertical_spacing": 0.5,
    "horizontal_spacing": 0.5,
    "pattern": "tr38901",
    "polarization": "V",
}

RX_ARRAY_DEFAULTS = {
    "num_rows": 1,
    "num_cols": 1,
    "vertical_spacing": 0.5,
    "horizontal_spacing": 0.5,
    "pattern": "dipole",
    "polarization": "V",
}

LEGACY_RADIO_ORIENTATION_RAD = [0.0, -180.0, 0.0]
CHANNEL_DIRECTION_DOWNLINK = 1


def resolve_node_orientation(node_info):
    """Return one node's Sionna Euler angles in radians."""
    if not node_info.HasField("orientation"):
        return LEGACY_RADIO_ORIENTATION_RAD.copy()

    orientation = node_info.orientation
    angles_degrees = np.asarray(
        [
            orientation.azimuth,
            orientation.elevation,
            orientation.roll,
        ],
        dtype=float,
    )
    if not np.all(np.isfinite(angles_degrees)):
        raise ValueError("Node orientation must be finite")
    if not (
        -180.0 <= angles_degrees[0] <= 180.0
        and -90.0 <= angles_degrees[1] <= 90.0
        and -180.0 <= angles_degrees[2] <= 180.0
    ):
        raise ValueError("Node orientation is outside the supported degree range")
    return np.deg2rad(angles_degrees).tolist()


def resolve_array_config(config, defaults):
    """Return validated PlanarArray keyword arguments.

    A config value of None denotes a legacy SimInitMessage. In that case the
    original 1x1 server configuration is returned unchanged.
    """
    if config is None:
        return defaults.copy()

    result = {
        "num_rows": int(config.num_rows),
        "num_cols": int(config.num_cols),
        "vertical_spacing": float(config.vertical_spacing),
        "horizontal_spacing": float(config.horizontal_spacing),
        "pattern": config.pattern,
        "polarization": config.polarization,
    }

    if result["num_rows"] <= 0 or result["num_cols"] <= 0:
        raise ValueError("Antenna-array rows and columns must be positive")
    if result["vertical_spacing"] <= 0 or result["horizontal_spacing"] <= 0:
        raise ValueError("Antenna-array spacings must be positive")
    if not result["pattern"] or not result["polarization"]:
        raise ValueError("Antenna-array pattern and polarization must be set")

    return result


def extract_link_cfr(cfr, link_index):
    """Extract one link as [num_rx_ant, num_tx_ant, num_subcarriers].

    Sionna returns
    [num_rx, num_rx_ant, num_tx, num_tx_ant, num_time_steps, num_sc].
    The ns3sionna request computes one transmitter and one time step.
    """
    cfr = np.asarray(cfr)
    if cfr.ndim != 6:
        raise ValueError("Expected a six-dimensional Sionna CFR, got {}".format(cfr.shape))
    if cfr.shape[2] != 1:
        raise ValueError("Expected one transmitter, got {}".format(cfr.shape[2]))
    if cfr.shape[4] != 1:
        raise ValueError("Expected one CFR time step, got {}".format(cfr.shape[4]))
    if link_index < 0 or link_index >= cfr.shape[0]:
        raise IndexError("CFR link index is out of range")

    return cfr[link_index, :, 0, :, 0, :]


def normalize_link_cfr(cfr, limits=None):
    """Separate wideband loss and normalize over sc/rx/tx coefficients."""
    cfr = np.asarray(cfr)
    if cfr.ndim != 3:
        raise ValueError("Expected CFR shape [rx, tx, sc], got {}".format(cfr.shape))
    if 0 in cfr.shape:
        raise ValueError("CFR antenna and subcarrier dimensions must be non-zero")
    (limits or ResourceLimits()).channel(cfr.shape[2], cfr.shape[0], cfr.shape[1])
    if not np.all(np.isfinite(cfr)):
        raise ValueError("CFR contains non-finite coefficients")

    mean_power = float(np.mean(np.abs(cfr) ** 2))
    if not np.isfinite(mean_power) or mean_power <= 0.0:
        raise ValueError("CFR mean power must be finite and positive")

    wideband_loss_db = float(-10.0 * np.log10(mean_power))
    return wideband_loss_db, cfr / np.sqrt(mean_power)


def append_cfr_to_message(rx_node_info, frequencies, cfr, limits=None):
    """Serialize [rx, tx, sc] CFR in flat sc/rx/tx order."""
    cfr = np.asarray(cfr)
    if cfr.ndim != 3:
        raise ValueError("Expected CFR shape [rx, tx, sc], got {}".format(cfr.shape))

    num_rx_antennas, num_tx_antennas, num_subcarriers = cfr.shape
    (limits or ResourceLimits()).channel(num_subcarriers, num_rx_antennas, num_tx_antennas)
    frequencies = np.asarray(frequencies).reshape(-1)
    if frequencies.size != num_subcarriers:
        raise ValueError("Frequency and CFR subcarrier counts do not match")

    # Transpose to [sc, rx, tx] before C-order flattening. Thus tx is the
    # fastest-changing index, followed by rx and then the subcarrier.
    flat_cfr = np.transpose(cfr, (2, 0, 1)).reshape(-1)

    rx_node_info.frequencies.extend(int(value) for value in frequencies)
    rx_node_info.csi_real.extend(float(value) for value in np.real(flat_cfr))
    rx_node_info.csi_imag.extend(float(value) for value in np.imag(flat_cfr))

    # Leaving both scalar fields at their proto3 default preserves the exact
    # legacy 1x1 wire representation. A zero dimension is decoded as one.
    if num_rx_antennas != 1 or num_tx_antennas != 1:
        rx_node_info.num_rx_antennas = num_rx_antennas
        rx_node_info.num_tx_antennas = num_tx_antennas


def append_user_channel_identity(
    rx_node_info, user_id, tx_node_id, csi_timestamp, rx_array_config
):
    """Attach explicit downlink user, direction, time, and RX-array identity."""
    identity = rx_node_info.user_identity
    identity.user_id = user_id
    identity.tx_node_id = tx_node_id
    identity.rx_node_id = rx_node_info.id
    identity.direction = CHANNEL_DIRECTION_DOWNLINK
    identity.csi_timestamp = csi_timestamp
    array = identity.rx_array
    array.num_rows = rx_array_config["num_rows"]
    array.num_cols = rx_array_config["num_cols"]
    array.vertical_spacing = rx_array_config["vertical_spacing"]
    array.horizontal_spacing = rx_array_config["horizontal_spacing"]
    array.pattern = rx_array_config["pattern"]
    array.polarization = rx_array_config["polarization"]


def validate_multi_user_snapshot(channel_state, limits=None):
    """Validate a complete Phase-4.1 snapshot and return users by stable ID."""
    if not channel_state.rx_nodes:
        raise ValueError("A multi-user snapshot needs at least one user")

    limits = limits or ResourceLimits()
    limits.check(len(channel_state.rx_nodes), "snapshots")
    batch_count = 0
    for node in channel_state.rx_nodes:
        batch_count += limits.channel(len(node.frequencies), node.num_rx_antennas or 1,
                                      node.num_tx_antennas or 1)
    limits.check(batch_count, "batch_coefficients")
    users = sorted(channel_state.rx_nodes, key=lambda node: node.user_identity.user_id)
    seen = set()
    frequencies = None
    num_tx_antennas = None
    for node in users:
        if not node.HasField("user_identity"):
            raise ValueError("Every user channel needs an explicit identity")
        identity = node.user_identity
        if identity.user_id in seen:
            raise ValueError("A multi-user snapshot contains a duplicate user")
        seen.add(identity.user_id)
        if (
            identity.user_id != node.id
            or identity.tx_node_id != channel_state.tx_node.id
            or identity.rx_node_id != node.id
        ):
            raise ValueError("User and directed endpoint identities are inconsistent")
        if identity.direction != CHANNEL_DIRECTION_DOWNLINK:
            raise ValueError("Phase 4.1 requires a downlink channel direction")
        if identity.csi_timestamp != channel_state.start_time:
            raise ValueError("User channels do not share one CSI timestamp")
        if not identity.HasField("rx_array"):
            raise ValueError("Every user channel needs an RX-array identity")

        config = resolve_array_config(identity.rx_array, RX_ARRAY_DEFAULTS)
        if config["polarization"] not in ("V", "H", "VH") or not np.all(
            np.isfinite([config["vertical_spacing"], config["horizontal_spacing"]])
        ):
            raise ValueError("RX-array identity is invalid")
        polarization_ports = 2 if config["polarization"] == "VH" else 1
        expected_rx = config["num_rows"] * config["num_cols"] * polarization_ports
        actual_rx = node.num_rx_antennas or 1
        actual_tx = node.num_tx_antennas or 1
        if actual_rx != expected_rx:
            raise ValueError("RX-array identity does not match the CFR dimension")

        node_frequencies = tuple(node.frequencies)
        if not node_frequencies:
            raise ValueError("A user CFR needs subcarrier frequencies")
        expected_coefficients = len(node_frequencies) * actual_rx * actual_tx
        if len(node.csi_real) != expected_coefficients or len(node.csi_imag) != expected_coefficients:
            raise ValueError("CFR coefficient count does not match sc/rx/tx dimensions")
        if frequencies is None:
            frequencies = node_frequencies
            num_tx_antennas = actual_tx
        elif node_frequencies != frequencies or actual_tx != num_tx_antennas:
            raise ValueError("User channels use inconsistent frequency grids or TX dimensions")

        coefficients = np.asarray(node.csi_real) + 1j * np.asarray(node.csi_imag)
        if not np.all(np.isfinite(coefficients)) or not np.isfinite(node.wb_loss):
            raise ValueError("User channel data must be finite")
        if not np.isclose(np.mean(np.abs(coefficients) ** 2), 1.0, rtol=1e-3):
            raise ValueError("Every user CFR must use the common unit-power normalization")

    return tuple(users)
