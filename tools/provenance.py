"""Additive run-manifest v1; historical export schemas/hashes remain immutable."""
from __future__ import annotations

import contextvars
from datetime import datetime, timezone
import functools
import hashlib
import importlib.metadata
import inspect
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import traceback

MODULE = Path(__file__).resolve().parents[1]
TEXT_SUFFIXES = {'.py', '.cc', '.h', '.proto', '.txt', '.csv', '.json', '.xml', '.obj', '.mtl', '.lock', '.yml', '.sh'}
ACTIVE_RUN = contextvars.ContextVar('provenance_run', default=None)


def digest(path):
    """Raw SHA-256 for every file; text additionally has strict UTF-8/CRLF->LF v1."""
    path = Path(path)
    raw = path.read_bytes()
    result = {'raw_sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw)}
    if path.suffix.lower() in TEXT_SUFFIXES:
        # Only CRLF is normalized: lone CR, whitespace and final newlines matter.
        canonical = raw.decode('utf-8').replace('\r\n', '\n').encode('utf-8')
        result.update(text_hash_version=1, lf_sha256=hashlib.sha256(canonical).hexdigest())
    return result


def verify_digest(path, expected):
    if expected.get('text_hash_version') not in (None, 1):
        raise ValueError('Unsupported text hash version')
    actual = digest(path)
    key = 'lf_sha256' if expected.get('text_hash_version') == 1 else 'raw_sha256'
    if actual.get(key) != expected[key]:
        raise ValueError(f'Hash mismatch: {path}')
    return actual['raw_sha256'] == expected['raw_sha256']


def atomic_json(path, data):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w', encoding='utf-8', newline='\n') as stream:
        json.dump(data, stream, indent=2, sort_keys=True, default=str)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def utc():
    return datetime.now(timezone.utc).isoformat()


def cpu_model():
    info = Path('/proc/cpuinfo')
    if info.exists():
        return next((line.split(':', 1)[1].strip() for line in info.read_text().splitlines()
                     if line.startswith('model name')), platform.processor())
    return platform.processor()


def tool_versions():
    versions = {}
    for command in ('c++', 'cmake', 'protoc'):
        try:
            versions[command] = subprocess.check_output([command, '--version'],
                stderr=subprocess.STDOUT, timeout=10).decode().splitlines()[0]
        except (OSError, subprocess.SubprocessError) as error:
            versions[command] = str(error)
    return versions


def source_identity():
    def git(*args):
        return subprocess.check_output(['git', '-C', str(MODULE), *args], timeout=10)
    try:
        if Path(git('rev-parse', '--show-toplevel').decode().strip()).resolve() != MODULE.resolve():
            raise ValueError('Module is a source snapshot inside another repository')
        paths = git('ls-files', '--cached', '--others', '--exclude-standard', '-z').decode().split('\0')
        revision = git('rev-parse', 'HEAD').decode().strip()
        changes = git('status', '--porcelain').decode().splitlines()
        delta_hash = hashlib.sha256(git('diff', 'HEAD', '--binary')).hexdigest()
    except (OSError, ValueError, subprocess.SubprocessError):
        # A copied module is a source snapshot, not a pristine Git checkout.
        paths = [str(p.relative_to(MODULE)) for p in MODULE.rglob('*') if p.is_file()
                 and not any(part in ('__pycache__', '.git', '.idea') for part in p.parts)]
        revision, changes, delta_hash = None, None, None
    sources = {name: digest(MODULE / name) for name in sorted(paths)
               if name and not name.startswith('data/') and Path(name).suffix in TEXT_SUFFIXES}
    return {'revision': revision, 'local_changes': changes,
            'tracked_diff_sha256': delta_hash, 'files': sources}


def ns3_identity():
    root = MODULE.parents[1]
    result = {'version': (root / 'VERSION').read_text().strip(), 'revision': None}
    if (root / '.git').exists():
        def git(*args):
            return subprocess.check_output(['git', '-C', str(root), *args], timeout=10)
        result.update(revision=git('rev-parse', 'HEAD').decode().strip(),
                      local_changes=git('status', '--porcelain').decode().splitlines(),
                      tracked_diff_sha256=hashlib.sha256(git('diff', 'HEAD', '--binary')).hexdigest())
    return result


class Run:
    """Atomic terminal status; a killed process leaves an explicit incomplete run.

    Success means the tool returned, not that its scientific acceptance policy passed.
    No resume/overwrite is offered. Store scientific verdicts separately.
    """
    def __init__(self, path, configuration, inputs=(), binary=None, artifacts=None):
        self.path = Path(path)
        self.configuration = configuration
        self.inputs = [Path(p) for p in inputs]
        self.binary = Path(binary) if binary else None
        self.artifacts = artifacts

    def __enter__(self):
        if self.path.exists():
            raise ValueError('Existing run manifest must be preserved')
        config = json.dumps(self.configuration, sort_keys=True, default=str).encode()
        self.data = {'schema': 'ns3sionna.run', 'schema_version': 1,
                     'hash_contract': 'raw-sha256+utf8-crlf-to-lf-v1',
                     'status': 'incomplete', 'started_utc': utc(), 'finished_utc': None,
                     'configuration': self.configuration,
                     'configuration_sha256': hashlib.sha256(config).hexdigest(),
                     'command': [sys.executable, *sys.argv], 'native_commands': [],
                     'backend': None, 'inputs': {}, 'artifacts': {},
                     'randomness': {'run_ns3': self.configuration.get('controls', {}).get('run_ns3'),
                         'configurations': [],
                         'contract': 'per-configuration mapping; absent Protobuf fields mean legacy v0'},
                     'environment': {'platform': platform.platform(), 'machine': platform.machine(),
                         'processor': cpu_model(), 'python': platform.python_version(),
                         'cpu_count': os.cpu_count(), 'tools': tool_versions(),
                         'packages': {d.metadata['Name']: d.version for d in importlib.metadata.distributions()}},
                     'measurement_sources': None}
        atomic_json(self.path, self.data)
        self.token = ACTIVE_RUN.set(self)
        try:
            self.data['measurement_sources'] = source_identity()
            self.data['inputs'] = {str(p.resolve()): digest(p) for p in self.inputs}
            if self.binary:
                self.data['build'] = {'executable': str(self.binary), 'digest': digest(self.binary),
                                      'ns3_source': ns3_identity()}
                for name in ('cmake-cache/CMakeCache.txt', 'cmake-cache/compile_commands.json'):
                    p = MODULE.parents[1] / name
                    if p.exists():
                        self.data['build'][name] = digest(p)
            atomic_json(self.path, self.data)
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise
        return self

    def _failure(self, kind, error, tb):
        self.data['status'] = 'incomplete' if isinstance(error, (KeyboardInterrupt, SystemExit)) else 'failed'
        log = ''.join(traceback.format_exception(kind, error, tb))
        if isinstance(error, subprocess.SubprocessError):
            log += f'\nstdout: {getattr(error, "stdout", None)}\nstderr: {getattr(error, "stderr", None)}\n'
        log_path = self.path.with_suffix('.error.log')
        log_path.write_text(log, encoding='utf-8')
        self.data['error_log'] = log_path.name
        self.data['finished_utc'] = utc()
        atomic_json(self.path, self.data)

    def __exit__(self, kind, error, tb):
        try:
            if error is not None:
                self._failure(kind, error, tb)
            else:
                try:
                    paths = self.artifacts if self.artifacts is not None else self.path.parent.rglob('*')
                    self.data['artifacts'] = {str(p.relative_to(self.path.parent)): digest(p)
                        for p in sorted(map(Path, paths)) if p.is_file() and p != self.path}
                    self.data['status'] = 'success'
                    self.data['finished_utc'] = utc()
                    atomic_json(self.path, self.data)
                except BaseException:
                    self._failure(*sys.exc_info())
                    raise
        finally:
            ACTIVE_RUN.reset(self.token)
        return False


def randomness_selected(config):
    """Record actual exported tuples and derived seeds without rewriting legacy data."""
    run = ACTIVE_RUN.get()
    if run:
        from random_streams import resolve_randomness
        record = resolve_randomness(config)
        if record not in run.data['randomness']['configurations']:
            run.data['randomness']['configurations'].append(record)


def backend_selected(name):
    run = ACTIVE_RUN.get()
    if run:
        run.data['backend'] = name
        atomic_json(run.path, run.data)


def native_command(command):
    run = ACTIVE_RUN.get()
    if run:
        # Keep instrumentation outside native timing: seal commands on exit.
        run.data['native_commands'].append(list(map(str, command)))


def benchmark_run(configuration, inputs=()):
    """Wrap existing runners without changing their legacy export layout/schema."""
    def decorate(function):
        @functools.wraps(function)
        def wrapped(*args, **kwargs):
            bound = inspect.signature(function).bind(*args, **kwargs)
            bound.apply_defaults()
            arguments = bound.arguments
            output = Path(arguments['output'])
            if output.exists() and any(output.iterdir()):
                raise ValueError('Use an empty output directory to preserve results')
            output.mkdir(parents=True, exist_ok=True)
            with Run(output / 'run.json', configuration(arguments), inputs, arguments['binary']) as run:
                result = function(*args, **kwargs)
                # Preserve case/configuration/seed identities in the legacy artifact.
                run.data['legacy_manifest'] = {'path': 'manifest.json',
                    'schema_version': result['schema_version'], 'subphase': result['subphase']}
                run.data['cases'] = result['configurations']
                run.data['validation'] = result['validation']
                return result
        return wrapped
    return decorate
