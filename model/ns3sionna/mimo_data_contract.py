"""Shape-safe conversion between Sionna CFR arrays and Protocol Buffers."""

import numpy as np


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


def normalize_link_cfr(cfr):
    """Separate wideband loss and normalize over sc/rx/tx coefficients."""
    cfr = np.asarray(cfr)
    if cfr.ndim != 3:
        raise ValueError("Expected CFR shape [rx, tx, sc], got {}".format(cfr.shape))
    if 0 in cfr.shape:
        raise ValueError("CFR antenna and subcarrier dimensions must be non-zero")
    if not np.all(np.isfinite(cfr)):
        raise ValueError("CFR contains non-finite coefficients")

    mean_power = float(np.mean(np.abs(cfr) ** 2))
    if not np.isfinite(mean_power) or mean_power <= 0.0:
        raise ValueError("CFR mean power must be finite and positive")

    wideband_loss_db = float(-10.0 * np.log10(mean_power))
    return wideband_loss_db, cfr / np.sqrt(mean_power)


def append_cfr_to_message(rx_node_info, frequencies, cfr):
    """Serialize [rx, tx, sc] CFR in flat sc/rx/tx order."""
    cfr = np.asarray(cfr)
    if cfr.ndim != 3:
        raise ValueError("Expected CFR shape [rx, tx, sc], got {}".format(cfr.shape))

    num_rx_antennas, num_tx_antennas, num_subcarriers = cfr.shape
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
