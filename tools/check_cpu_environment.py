#!/usr/bin/env python3
"""Inspect the existing native/Conda CPU environment without installing packages.

Exact Python, pip and lock versions are deviations by default; --strict makes
these requirements. Tool/library availability, pip consistency, built bindings
and the actual LLVM backend are always required. JSON is written to stdout.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
from importlib import metadata
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

MODULE = Path(__file__).resolve().parents[1]
LOCK = MODULE / 'model/ns3sionna/requirements_cpu_ci.lock'
EXPECTED_PYTHON = '3.12.12'
EXPECTED_PIP = '26.0.1'
EXPECTED_BACKEND = 'llvm_ad_mono_polarized'
BACKEND_PROBE = '''
import json
import sionna.rt
import mitsuba as mi
import drjit as dr
actual = mi.variant()
values = None
if actual == 'llvm_ad_mono_polarized':
    value = mi.Float([1, 2]) + 1
    dr.eval(value)
    values = list(value)
print(json.dumps({'variant': actual, 'llvm_evaluation': values}))
'''


def locked_versions(path):
    result = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith(('#', '--')):
            continue
        if line.count('==') != 1:
            raise ValueError(f'Unsupported lock entry at line {number}')
        package, version = line.split('==')
        if not package or not version or package in result:
            raise ValueError(f'Invalid/duplicate lock entry at line {number}')
        result[package] = version
    if not result:
        raise ValueError('Empty CPU lock')
    return result


def command(args, *, env=None):
    try:
        result = subprocess.run(args, text=True, capture_output=True,
                                timeout=60, env=env, check=False)
        return {'command': list(args), 'returncode': result.returncode,
                'stdout': result.stdout.strip(), 'stderr': result.stderr.strip()}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {'command': list(args), 'returncode': None, 'error': str(error)}


def installed(package):
    try:
        return metadata.version(package)
    except metadata.PackageNotFoundError:
        return None


def llvm_library():
    for name in ['LLVM', *(f'LLVM-{version}' for version in range(30, 7, -1))]:
        library = ctypes.util.find_library(name)
        if library:
            try:
                ctypes.CDLL(library)
                return {'library': library, 'loadable': True}
            except OSError:
                continue
    return {'library': None, 'loadable': False}


def check_environment(*, strict=False, module=MODULE, lock=LOCK):
    checks = []

    def add(name, ok, observed, expected=None, required=True):
        checks.append({'name': name, 'ok': bool(ok), 'required': bool(required),
                       'expected': expected, 'observed': observed})

    python = platform.python_version()
    add('python_supported', sys.version_info >= (3, 11), python, '>=3.11')
    add('python_exact', python == EXPECTED_PYTHON, python, EXPECTED_PYTHON, strict)
    add('pip_exact', installed('pip') == EXPECTED_PIP, installed('pip'), EXPECTED_PIP, strict)
    try:
        packages = locked_versions(lock)
        add('lock_package_count', len(packages) == 55, len(packages), 55)
        for package, expected in packages.items():
            actual = installed(package)
            add('distribution:' + package, actual == expected, actual, expected, strict)
        lock_hash = hashlib.sha256(lock.read_bytes()).hexdigest()
    except (OSError, ValueError) as error:
        add('cpu_lock', False, str(error))
        lock_hash = None
    result = command([sys.executable, '-m', 'pip', 'check'])
    add('pip_check', result['returncode'] == 0, result)
    for tool in ('gcc', 'g++', 'cmake', 'protoc', 'pkg-config'):
        executable = shutil.which(tool)
        result = command([executable, '--version']) if executable else {'error': 'Executable not found'}
        add('tool:' + tool, result.get('returncode') == 0, result)
    pkg_config = shutil.which('pkg-config')
    for package in ('libzmq', 'protobuf'):
        result = command([pkg_config, '--modversion', package]) if pkg_config else {'error': 'pkg-config not found'}
        add('native_package:' + package, result.get('returncode') == 0, result)
    llvm = llvm_library()
    add('llvm_library', llvm['loadable'], llvm)
    proto = module / 'model/message.proto'
    for relative in ('model/message.pb.cc', 'model/message.pb.h',
                     'model/ns3sionna/common/message_pb2.py'):
        path = module / relative
        exists = path.is_file() and path.stat().st_size > 0
        current = exists and proto.is_file() and path.stat().st_mtime_ns >= proto.stat().st_mtime_ns
        add('binding:' + relative, current, {'exists_nonempty': exists,
            'not_older_than_proto': bool(current),
            'freshness_evidence': 'timestamp only; clean-build proof is separate'})
    # A fresh process imports Sionna first and exercises LLVM; no parent variant is reused.
    result = command([sys.executable, '-c', BACKEND_PROBE],
                     env={**os.environ, 'CUDA_VISIBLE_DEVICES': ''})
    try:
        observed = json.loads(result.get('stdout', '').splitlines()[-1])
        ok = (result['returncode'] == 0 and observed['variant'] == EXPECTED_BACKEND
              and observed['llvm_evaluation'] == [2.0, 3.0])
        result['backend'] = observed
    except (IndexError, KeyError, TypeError, ValueError):
        ok = False
    add('actual_cpu_backend', ok, result, EXPECTED_BACKEND)
    failures = [check['name'] for check in checks if check['required'] and not check['ok']]
    deviations = [check['name'] for check in checks if not check['required'] and not check['ok']]
    return {'schema': 'ns3sionna.cpu-environment', 'schema_version': 1,
            'status': 'failed' if failures else 'success', 'strict': strict,
            'python_executable': sys.executable, 'lock_sha256': lock_hash,
            'checks': checks, 'failures': failures, 'deviations': deviations}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--strict', action='store_true', help='Require every exact Python/pip/lock pin')
    args = parser.parse_args()
    report = check_environment(strict=args.strict)
    print(json.dumps(report, indent=2, sort_keys=True))
    return int(report['status'] != 'success')


if __name__ == '__main__':
    raise SystemExit(main())
