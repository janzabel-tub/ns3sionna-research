# Saturated UDP controls for the antenna and station sweeps

Both monolith examples accept `--transport=tcp|udp`; both shell runners and
sweeps accept `TRANSPORT=tcp|udp`. The default remains TCP/Cubic. UDP uses the
same continuously-on OnOff source and per-station PacketSink, 1472 application
bytes per packet, offered rate per station, placement, channel, power, and
measurement window. `config.txt` records `transport` and
`applicationPacketSizeBytes`. UDP ignores `tcpVariant`.

With the usual Sionna server running, from the ns-3 directory:

```sh
TRANSPORT=udp BUILD=true ENABLE_RATE_TRACE=true ENABLE_MIMO_TRACE=true \
  OUTPUT_DIR="$PWD/results/antenna-udp" \
  bash contrib/sionna/examples/performance-sionna-sumimo-antenna-sweep.sh
```

The SU runner uses that external server. Stop it when the antenna sweep has
finished. With no external server running, the MU runner can own a fresh server
for each station count:

```sh
TRANSPORT=udp BUILD=true START_SERVER=true RT_FAST=false MAX_RETRIES=0 \
  ENABLE_MU_DIAGNOSTICS=true \
  OUTPUT_DIR="$PWD/results/stations-udp" \
  bash contrib/sionna/examples/performance-sionna-mumimo-sta-sweep.sh
```

The MU runner starts and stops its own server for each case. Use a new output directory for every sweep.
Explicit `BUILD=true` or `false` also avoids an unrelated `BUILD` value inherited
from a Conda compiler environment.

The antenna sweep uses **20 MHz**, SVD for 2–8 antennas, and 10,000 Mb/s offered
load. Its antenna count/NSS settings are ceilings under Minstrel. The station
sweep offers 600 Mb/s **per STA**, one stream per selected user, with adaptive
MU groups and ideal-CSI MU rates; SU fallback retains its independent rate
controller. UDP removes TCP feedback; native Wi-Fi ACKs, BlockAcks, BAR polls,
contention, aggregation limits, and MAC retries still apply. Confirm saturation
from queues/aggregation and delivered rate rather than assuming it from load.

`--mimoTrace=true` in the SU monolith additionally enables the existing passive
PPDU/PHY/MAC/queue diagnostic CSVs. With SVD, `channel_snapshot.csv` saves the
**first** intended-link normalized CFR on HE data tones for each transmitter
and channel width, including physical scalar received power before MIMO.
It is an initial snapshot, not a channel-history trace. Receiver gain is zero
in these examples. Offline SVD gives per-mode SINR
`rho * sigma_s^2 / actualNss`, using `rho = receivedPower / (k*290*B*noiseFactor)`.
Do not treat numerical rank alone as a count of useful high-MCS modes.

For a longer learning test, extend `SIMULATION_TIME_SECONDS`, keep payload
traffic starting early, and evaluate later throughput CSV intervals. Increasing
`TRAFFIC_START_SECONDS` alone delays both payload traffic and measurement;
it does not train Minstrel on saturated data.

For fixed-NSS/MCS controls use the monolith directly: the antenna sweep owns
its per-case `--streams` value. For example, hold the 8×8 geometry fixed and
compare `--streams=4 --mcs=7` with `--streams=6 --mcs=7` and
`--streams=8 --mcs=3`, each with `--rateAdaptation=fixed`, `--precoding=svd`
and `--transport=udp`. Keep placement, width, total power, coding and seed identical.

Run the server-free interface checks after building both monolith targets:

```sh
python contrib/sionna/tools/test_monolith_transport.py
```

The November 10 workspace study in `analysis/udp-throughput-20261110` contains
a serial paired comparison driver, raw records, source/binary identities,
an independent CFR/SVD analyzer, and the resulting report. Its driver owns a
fresh full-RT server per case and cleans it up on completion or interruption.
