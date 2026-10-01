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
commands, execution statuses, portable hashes and remaining R1/R3 limitations.
The general server requirements and existing container recipes are unchanged.
