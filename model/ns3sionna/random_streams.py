"""Versioned ns-3 identity -> Sionna RT/mobility RNG domains.

This is a seed mapping, not an emulation of ns-3's MRG32k3a sequence.
Stream allocation isolates draws; determinism is not statistical independence proof.
"""
import hashlib

import numpy as np


def _seed(seed, run, stream, domain):
    label = f'ns3sionna-rng-v1:{seed}:{run}:{stream}:{domain}'
    return int.from_bytes(hashlib.sha256(label.encode('ascii')).digest()[:4], 'big')


def resolve_randomness(config):
    if not config.HasField('randomness'):
        if not 0 <= config.seed <= 0x7fffffff:
            raise ValueError('Legacy seed must be a nonnegative int32')
        return {'mapping_version': 0, 'ns3_seed': config.seed, 'ns3_run': None,
                'rt_stream': None, 'mobility_stream': None,
                'rt_seed': config.seed, 'mobility_seed': config.seed}
    r = config.randomness
    if r.mapping_version != 1:
        raise ValueError('Unsupported randomness mapping version')
    if r.ns3_seed == 0 or r.ns3_run == 0:
        raise ValueError('Explicit ns-3 seed and run must be positive')
    if max(r.rt_stream, r.mobility_stream) > 0x7fffffffffffffff or r.rt_stream == r.mobility_stream:
        raise ValueError('RT/mobility streams must be distinct nonnegative int64 IDs')
    return {'mapping_version': 1, 'ns3_seed': r.ns3_seed, 'ns3_run': r.ns3_run,
            'rt_stream': r.rt_stream, 'mobility_stream': r.mobility_stream,
            'rt_seed': r.ns3_seed if r.ns3_run == 1 and r.rt_stream == 0 else
                _seed(r.ns3_seed, r.ns3_run, r.rt_stream, 'rt'),
            'mobility_seed': _seed(r.ns3_seed, r.ns3_run, r.mobility_stream, 'mobility')}


def mobility_rng(mapping, node_id):
    if not 0 <= node_id <= 0xffffffff:
        raise ValueError('Node ID must be uint32')
    return np.random.default_rng(np.random.SeedSequence([
        mapping['mobility_seed'], node_id, 0x51A0]))
