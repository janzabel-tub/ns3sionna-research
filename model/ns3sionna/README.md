ns3sionna
======

Sionna RT meets ns-3

## Starting

To start Python server component:
```
./run.sh
```

or

```
./run_fast.sh
```
if you want faster but less accurate simulations.

Note: in case you have problems with Protocol Buffers you can try:

```
./run_python_proto.sh
```

## TODOs

Work needs to be done on:
- better receiver filtering - no need to compute link between too distant nodes
- mode MODE_P2MP_LAH: swapping mobile TX with fixed RX due to channel reciprocity
- providing support for MODE_P2MP_LAH in case all nodes are mobile

## CPU regression environment

For the initial Linux/Python 3.12.12 CI environment, install the exact dependency
closure with `python -m pip install --no-deps -r requirements_cpu_ci.lock`, then
run `python -m pip check`. Generate fresh C++/Python Protobuf bindings with the
ns-3 build before running the existing server tests. The shared regression entry
point is `../../tools/run_cpu_regressions.py`; see the
[provenance/CI contract](../../doc/provenance-cpu-increment.md) for native build
commands, execution statuses, portable hashes and native R1 reproduction.
Run `python ../../tools/check_cpu_environment.py --strict` in the existing Conda
environment after rebuilding configuration consumers. Explicit seed/run/stream
fields select mapping v1; absent fields preserve historical seed-only behavior.
The general server requirements remain unchanged; R1 needs no container.

## Bounded mobility history

`--mobility_history_limit` defaults to 1024 position/velocity pairs per node;
zero disables retention. Filled histories reuse arrays; getters and exported
histories return copies, while old evicted RandomWalk timestamps raise `KeyError`.
Look-ahead retains explicit epoch poses on history-free motion/RNG clones and
restores committed state after forecast failures. Actual-time motion precedes
forecasting. `mobility_storage_statistics()` reports NumPy data separately from
Python/RT overhead. The [R3 storage command](../../README.md#bounded-mobility-storage-phase-5-r3)
records a fixed plan, CSV checkpoints and existing Run/bundle provenance.
