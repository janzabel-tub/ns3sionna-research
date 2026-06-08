# ns3sionna Jan feature guide

This is a short usage note for the Jan branch. It assumes ns3sionna is already
installed and built as `contrib/sionna` inside an ns-3 tree. See `README.md` for
installation details.

The main new knobs are fixed coherence time, absolute-time mobility with
equidistant internal walk steps, and a scene bounding-box mobility guard.

## Start the Sionna server

Default server, as in `model/ns3sionna/run.sh`:

```bash
cd ns-3.40/contrib/sionna/model/ns3sionna
./run.sh
```

Server with the new mobility options:

```bash
cd ns-3.40/contrib/sionna/model/ns3sionna
python ns3sionna_server.py \
  --equidistant_walk_steps 100000 \
  --mobility_bbox_toggle true \
  --mobility_bbox_margin 1.0
```

The same settings can also be passed through environment variables while still
using `run.sh`:

```bash
cd ns-3.40/contrib/sionna/model/ns3sionna
NS3SIONNA_EQUIDISTANT_WALK_STEPS=100000 \
NS3SIONNA_MOBILITY_BBOX_TOGGLE=true \
NS3SIONNA_MOBILITY_BBOX_MARGIN=1.0 \
./run.sh
```

`--equidistant_walk_steps` is in nanoseconds. `0` keeps the old request-based
mobility update mode. `100000` means the Python server advances mobility in
fixed 0.1 ms steps between ns-3 channel requests.

The bounding-box guard is enabled by default. It uses the Sionna scene bbox in
x/y, adds `--mobility_bbox_margin` meters around it, and reflects/clamps random
walk movement at that outer limit. Scene walls are still handled by the normal
ray intersection logic. Disable it with:

```bash
python ns3sionna_server.py --mobility_bbox_toggle false
```

## Run ns-3 with fixed coherence time

Open a second terminal at the ns-3 root, the directory containing `./ns3`.

```bash
cd ns-3.40
./ns3 run "ns3sionna-performance-sionna-tcp \
  --sim_max_stas=1 \
  --environment=apartment/apartment.xml \
  --mobile_scenario=true \
  --mobile_speed=7.0 \
  --caching=true \
  --mode=2 \
  --sub_mode=16 \
  --minCoherenceTimeMs=100 \
  --dataRate=600 \
  --txPower=20 \
  --seed=1"
```

`--minCoherenceTimeMs=0` keeps the velocity-based coherence time. Any non-zero
value forces the server to use a fixed coherence time for channel validity.

Note: in the current Jan branch the server scales this raw value by `100000 ns`,
so the effective value is in 0.1 ms units:

```text
--minCoherenceTimeMs=1     -> 0.1 ms
--minCoherenceTimeMs=10    -> 1 ms
--minCoherenceTimeMs=100   -> 10 ms
--minCoherenceTimeMs=1000  -> 100 ms
```

For Munich, keep the same server command and change the environment:

```bash
cd ns-3.40
./ns3 run "ns3sionna-performance-sionna-tcp \
  --sim_max_stas=1 \
  --environment=munich/munich.xml \
  --mobile_scenario=true \
  --mobile_speed=7.0 \
  --caching=true \
  --mode=2 \
  --sub_mode=16 \
  --minCoherenceTimeMs=100 \
  --dataRate=600 \
  --txPower=30 \
  --seed=1"
```

## Small manual sweep

This replaces the important part of `watchdog.sh` without requiring that script.
Restart the server only when you change server-side settings such as
`--equidistant_walk_steps` or `--mobility_bbox_margin`. This example keeps only
the normal throughput and computation-time outputs.

```bash
cd ns-3.40
for tc in 0 10 100 1000; do
  for seed in 1 2 3; do
    ./ns3 run "ns3sionna-performance-sionna-tcp \
      --sim_max_stas=1 \
      --environment=apartment/apartment.xml \
      --mobile_scenario=true \
      --mobile_speed=7.0 \
      --caching=true \
      --mode=2 \
      --sub_mode=16 \
      --minCoherenceTimeMs=${tc} \
      --dataRate=600 \
      --txPower=20 \
      --seed=${seed}"

    out="contrib/sionna/examples/results_jan/tc${tc}"
    mkdir -p "${out}"
    mv throughput_results.csv "${out}/throughput_results_${tc}_${seed}.csv"
    if [ -f computationtime.csv ]; then
      mv computationtime.csv "${out}/computationtime_${tc}_${seed}.csv"
    fi
  done
done
```

## Other implemented features

These are present in the branch, but are not required for the basic guide above:

- Deterministic PathSolver seeding from run seed, request time, node IDs, mode,
  submode, receiver set, and lookahead times.
- Configurable Sionna RT path limit:
  `python ns3sionna_server.py --rt_max_num_paths_per_src 10000000`.
- AP-to-STA TCP traffic setup in `performance-sionna-tcp.cc`.
- `--txPower` in the TCP performance examples.
- Apartment and Munich-specific starting positions in the TCP performance
  example.
- Watchdog/runner convenience logic: coherence-time sweeps, seed sweeps, restart
  after server crashes, timeout retries, and output directory management.

## Debug/logging features not covered above

These were mostly useful for measurement and debugging, so they are intentionally
not part of the minimal usage commands:

- `--snr=true` writes PHY-level reception samples to `snr_results.csv`,
  including OK, ERROR, and DROP receptions.
- `--positions=true` writes PHY reception positions to `position_results.csv`.
- `--minstrelRateTrace=true` writes `minstrel_rate_results.csv` and
  `minstrel_rate_changes.csv`.
- `--wb_loss_log <file>` on `ns3sionna_server.py` writes per-link wideband loss
  samples.
- `computationtime.csv`, result CSV renaming, and result folder normalization are
  runner/plotting conveniences, not required ns3sionna features.
