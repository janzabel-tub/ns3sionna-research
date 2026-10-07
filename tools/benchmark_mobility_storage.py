#!/usr/bin/env python3
"""R3 bounded mobility storage experiment, using the existing motion model.

This measures retained storage and validates unchanged trajectories against a
history-disabled twin. It does not trace channels or assess physical Doppler.
Export the completed output with tools/experiment_bundle.py export.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import subprocess
import sys
import time

import numpy as np

import provenance

MODULE = Path(__file__).resolve().parents[1]
NS3 = MODULE.parents[1]
SERVER = MODULE / 'model/ns3sionna'
sys.path.insert(0, str(SERVER))
from common import message_pb2
from mobility import RandomWalkMobility
from random_streams import mobility_rng, resolve_randomness

CAPACITIES = (0, 1, 8, 64, 1024)
MODES = (
    ('WALL', RandomWalkMobility.MODE_WALL, True, 42),
    ('TIME', RandomWalkMobility.MODE_TIME, 7, 43),
    ('DISTANCE', RandomWalkMobility.MODE_DISTANCE, 0.15, 44),
)
CHECKPOINT_INTERVAL = 256
CSV_FIELDS = ('case_id', 'limit', 'mode', 'seed', 'time', 'entries', 'bytes',
              'allocations', 'reuses', 'overwrites', 'evictions', 'elapsed_seconds')


def positive_steps(value):
    value = int(value)
    if not 1 <= value <= 1_000_000:
        raise argparse.ArgumentTypeError('steps must be between 1 and 1000000')
    return value


def runner_path():
    candidates = sorted(path for path in (NS3 / 'build/utils').glob('*test-runner*')
                        if path.is_file())
    if len(candidates) != 1:
        raise FileNotFoundError('Expected one built build/utils/*test-runner executable')
    return candidates[0]


def seed_configuration(seed):
    config = message_pb2.SimInitMessage(seed=seed)
    config.randomness.mapping_version = 1
    config.randomness.ns3_seed = seed
    config.randomness.ns3_run = 1
    config.randomness.rt_stream = 0
    config.randomness.mobility_stream = 1
    return config


def node_for(mapping, mode, period, limit, collect):
    return RandomWalkMobility(
        1, [1.0, 2.0, 3.0], mode, period,
        RandomWalkMobility.SPEED_UNIFORM, [0.5, 1.5],
        RandomWalkMobility.DIRECTION_UNIFORM, [-np.pi, np.pi],
        rng=mobility_rng(mapping, 1), collect_history=collect, history_limit=limit,
    )


def advance(node, timestamp):
    # Same free-motion integration as the long storage regression. The integer
    # timestamp is a step index; TIME's period is seven steps, dt is 0.01 s.
    distance = np.linalg.norm(node.velocity) * 0.01
    node.update_pos(timestamp, node.pos + node.velocity * 0.01, node.velocity, False)
    node.check_set_new_velocity(timestamp, distance)


def assert_twins(node, reference):
    if not np.array_equal(node.pos, reference.pos):
        raise AssertionError('Position differs from history-disabled reference')
    if not np.array_equal(node.velocity, reference.velocity):
        raise AssertionError('Velocity differs from history-disabled reference')
    for name in ('last_update', 'last_update_velocity', 'segment_distance', 'hit_wall'):
        if getattr(node, name) != getattr(reference, name):
            raise AssertionError(f'Motion control differs: {name}')
    if node.rng.bit_generator.state != reference.rng.bit_generator.state:
        raise AssertionError('RNG state differs from history-disabled reference')


def assert_storage(node, timestamp):
    stats = node.history_statistics()
    expected_entries = min(node.history_limit, timestamp + 1)
    if stats['entries'] != expected_entries or stats['bytes'] != 48 * expected_entries:
        raise AssertionError('Retained history exceeds or differs from the planned bound')
    if stats['allocations'] != expected_entries:
        raise AssertionError('History allocated again after the slot pool filled')
    expected_reuses = max(0, timestamp + 1 - node.history_limit) if node.history_limit else 0
    if stats['reuses'] != expected_reuses or stats['evictions'] != expected_reuses:
        raise AssertionError('History did not reuse exactly one slot per eviction')
    if list(node.pos_history) != list(node.velo_history):
        raise AssertionError('Position and velocity history keys differ')
    return stats


def native_check(binary, output):
    command = [str(binary), '--suite=sionna', '--verbose']
    provenance.native_command(command)
    # Stream directly to the artifact so native failure/timeout retains output.
    with (output / 'native.log').open('w', encoding='utf-8') as log:
        subprocess.run(command, cwd=NS3, stdout=log, stderr=subprocess.STDOUT,
                       check=True, text=True, timeout=180)
    summaries = []
    for line in (output / 'native.log').read_text().splitlines():
        if line.startswith('R3_POOL_SUMMARY '):
            summaries.append(json.loads(line.removeprefix('R3_POOL_SUMMARY ')))
    if len(summaries) != 1:
        raise AssertionError('Expected one R3_POOL_SUMMARY in native output')
    summary = summaries[0]
    if (summary['requests'], summary['allocations'], summary['hits']) != (512, 1, 511):
        raise AssertionError('Existing native pool fixture failed allocation/hit acceptance')
    if not 0 <= summary['capacity_bytes'] <= summary['limit_bytes']:
        raise AssertionError('Existing native pool fixture exceeded its byte cap')
    return summary


def plan_for(steps, native):
    cases = []
    for limit in CAPACITIES:
        for label, mode, period, seed in MODES:
            cases.append({'case_id': f'limit_{limit}_{label.lower()}', 'limit': limit,
                          'mode': label, 'mode_value': mode, 'mode_period': period,
                          'seed': seed, 'randomness': resolve_randomness(seed_configuration(seed))})
    return {
        'schema_version': 1, 'subphase': 'R3.storage',
        'controls': {
            'scene': 'simple_room/simple_room.xml', 'scene_used_for_rt': False,
            'run_ns3': 1, 'steps': steps, 'capacities': list(CAPACITIES),
            'checkpoint_interval': CHECKPOINT_INTERVAL,
            'initial_position_m': [1.0, 2.0, 3.0], 'node_id': 1,
            'integration_step_seconds': 0.01, 'timestamp_unit': 'integer step index',
            'speed_distribution': 'uniform [0.5, 1.5] m/s',
            'direction_distribution': 'uniform [-pi, pi] radians',
            'native_check': native,
        },
        'cases': cases,
        'scope': 'retained mobility storage; free motion without collisions or channel tracing',
        'acceptance': {
            'reference': 'identically seeded collect_history=False twin',
            'comparison': 'exact position, velocity, control scalars, and local RNG state every update',
            'storage': 'entries<=limit; bytes=48*entries; allocations plateau; paired keys and slot reuse',
            'native': 'optional existing sionna fixture: requests=512, allocations=1, hits=511, capacity<=limit',
            'elapsed_seconds': 'descriptive paired evolution plus validation; excluded from numerical acceptance',
            'fresh_rt_traces': 0,
        },
    }


def run(output, steps=20_000, native=False, test_runner=None):
    if not isinstance(steps, int) or isinstance(steps, bool) or not 1 <= steps <= 1_000_000:
        raise ValueError('steps must be an integer between 1 and 1000000')
    output = Path(output).resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError('Output must be a new or empty directory; preserve existing measurements')
    output.mkdir(parents=True, exist_ok=True)
    plan = plan_for(steps, native)
    # Declare recovered implementation bytes and the actual dependency lock as
    # Run inputs, so the existing bundle exporter copies them without rewriting.
    inputs = [SERVER / 'mobility.py', SERVER / 'random_streams.py',
              SERVER / 'requirements_cpu_ci.lock', SERVER / 'common/message_pb2.py',
              Path(__file__).resolve(), MODULE / 'tools/provenance.py']
    with provenance.Run(output / 'run.json', plan, inputs=inputs) as recorded:
        recorded.data['validation'] = {'verdict': 'not completed'}
        provenance.atomic_json(output / 'plan.json', plan)
        results = []
        rows = 0
        with (output / 'storage.csv').open('w', encoding='utf-8', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
            writer.writeheader()
            for case in plan['cases']:
                config = seed_configuration(case['seed'])
                provenance.randomness_selected(config)
                node = node_for(case['randomness'], case['mode_value'],
                                case['mode_period'], case['limit'], True)
                reference = node_for(case['randomness'], case['mode_value'],
                                     case['mode_period'], case['limit'], False)
                assert_twins(node, reference)
                started = time.perf_counter()
                # IDs of both vectors must stay in the original bounded slot
                # pool. This catches allocating replacements behind counters.
                slot_ids = set()
                for timestamp in range(steps + 1):
                    if timestamp:
                        advance(node, timestamp)
                        advance(reference, timestamp)
                        assert_twins(node, reference)
                    stats = assert_storage(node, timestamp)
                    if node.history_limit:
                        current_pair = (id(node.pos_history[timestamp]), id(node.velo_history[timestamp]))
                        if timestamp < node.history_limit:
                            slot_ids.add(current_pair)
                        elif current_pair not in slot_ids:
                            raise AssertionError('Eviction allocated vectors instead of reusing the slot pool')
                    if reference.history_statistics()['entries'] != 0:
                        raise AssertionError('History-disabled reference retained history')
                    if timestamp % CHECKPOINT_INTERVAL == 0 or timestamp == steps:
                        writer.writerow({key: value for key, value in {
                            'case_id': case['case_id'], 'limit': case['limit'], 'mode': case['mode'],
                            'seed': case['seed'], 'time': timestamp, **stats,
                            'elapsed_seconds': time.perf_counter() - started,
                        }.items() if key in CSV_FIELDS})
                        rows += 1
                results.append({**case, 'updates_compared': steps,
                                'final_position_m': node.pos.tolist(),
                                'final_velocity_m_s': node.velocity.tolist(),
                                'final_control': {name: getattr(node, name) for name in (
                                    'last_update', 'last_update_velocity', 'segment_distance', 'hit_wall')},
                                'final_rng_state': node.rng.bit_generator.state,
                                'storage': node.history_statistics(),
                                'elapsed_seconds': time.perf_counter() - started})
                stream.flush()
        summary = None
        if native:
            binary = Path(test_runner).resolve() if test_runner else runner_path()
            recorded.data['build'] = {'executable': str(binary), 'digest': provenance.digest(binary),
                                      'ns3_source': provenance.ns3_identity()}
            summary = native_check(binary, output)
        validation = {
            'verdict': 'passed', 'cases': len(results),
            'updates_compared': steps * len(results), 'checkpoint_rows': rows,
            'exact_trajectory_control_rng': 'passed', 'bounded_storage_and_slot_reuse': 'passed',
            'native_pool_fixture': summary if native else 'not requested',
            'fresh_rt_traces': 0, 'physical_doppler': 'not assessed',
        }
        source_hashes = {str(path.relative_to(MODULE)): provenance.digest(path)['raw_sha256']
                         for path in inputs}
        manifest = {**plan, 'plan_sha256': provenance.digest(output / 'plan.json')['raw_sha256'],
                    'results': results, 'validation': validation,
                    'provenance': {'backend': 'numpy-local-mobility', 'source_sha256': source_hashes}}
        provenance.atomic_json(output / 'manifest.json', manifest)
        recorded.data['validation'] = validation
        recorded.data['backend'] = 'numpy-local-mobility'
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--steps', type=positive_steps, default=20_000)
    parser.add_argument('--native-check', action='store_true',
                        help='also run the existing built sionna native test suite')
    parser.add_argument('--test-runner', type=Path,
                        help='explicit existing native runner, used with --native-check')
    args = parser.parse_args()
    if args.test_runner and not args.native_check:
        parser.error('--test-runner requires --native-check')
    manifest = run(args.output, args.steps, args.native_check, args.test_runner)
    print(json.dumps(manifest['validation'], sort_keys=True))


if __name__ == '__main__':
    main()
