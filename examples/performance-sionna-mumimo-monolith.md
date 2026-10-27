`performance-sionna-mumimo-monolith.cc` is an additive MU version of
`performance-sionna-mimo-monolith.cc`. Existing C++ examples and shell runners
remain usable; the monolith target now accepts one through eight stations and
two through eight AP antennas. The default runner configuration remains four
stations and four AP antennas. The example target is
`ns3sionna-performance-sionna-mumimo-monolith`.

It creates one AP and single-antenna STAs. For two through eight stations,
all configured MU users share the same full-band 242-tone RU at 20 MHz, with
one stream per STA. A one-station run is an HE-SU MRT baseline, since MU needs
at least two users.
Stable MU user IDs 1–N correspond to STA indices 0–N-1; node IDs are recorded
separately. The default AP array is one row of four vertically polarized ports,
with 0.5 wavelength spacing and the helper's TR-38.901 TX / dipole RX patterns.
`ARRAY_ROWS` selects a planar AP array when it divides the antenna count. The
helper retains its default orientations.

TCP OnOff traffic, per-STA PacketSinks, TCP variant/segment size, mobility,
scene placement, RNG streams, flags, optional diagnostics, throughput windows
and CSV layout come from the TCP/MIMO performance examples, principally the
MIMO monolith. Each STA receives an independent TCP flow from the same AP;
`DATA_RATE=600` is offered Mb/s **per STA**, not measured throughput. Defaults
are TcpCubic, 1472-byte application packets/segments, static nodes, traffic
starting at 1 s and simulation ending at 10 s. Mobile mode keeps the AP static
and uses the existing wall-aware Sionna random walk. Apartment placement keeps
the existing same-room selection; Munich and other scene placements are also
retained. Explicit positions require one AP triple and one STA triple per
configured station.

MU configuration follows `ns3_wifi_mumimo.cc`, using
`SionnaMimoHelper::Apply[ForHeMcs]` and `InstallDownlink`. The provider queries
the live `SionnaPropagationCache` for AP-to-STA CFRs, frequency vectors and
wideband losses. It freezes one complete group per simulator timestamp and
supplies the native scheduler's current-CSI and transmission snapshots. There
are no deterministic synthetic channel matrices or new precoding equations.
The cache's distant-link shortcut is disabled so MU always receives real CFRs.

The model contract is [ns3sionna_mimo_model.md](../ns3sionna_mimo_model.md),
especially its MU installation and general BER sections (5.20 and 6.2).
Sionna supplies the normalized 768-bin 1xN CFR and separate scalar loss;
ns-3 computes ZF/RZF, spatial interference, SINR and the per-user
`MEAN_UNCODED_BER` effective-SINR mapping before its existing error model.
This retains the performance monolith's NistErrorRateModel. No EESM beta or
calibration table is required. Common AP power is preserved by the native
precoder normalization; `TX_POWER` is total device power, not power per stream.

For later use, from the ns-3 root, inspect the generated command without building,
starting a server or creating results:

```bash
bash contrib/sionna/examples/performance-sionna-mumimo-monolith.sh --dry-run
```

Use an already running compatible Sionna server (default), or let the runner
own a local one. The following commands build and simulate **when you choose
to execute them**:

```bash
# Existing server; default ZF and AP Minstrel.
bash contrib/sionna/examples/performance-sionna-mumimo-monolith.sh

# Owned local server, with an explicit illustrative RZF alpha (not a fitted beta).
START_SERVER=true SERVER_PYTHON=python3 PRECODING=rzf RZF_ALPHA=0.25 \
  bash contrib/sionna/examples/performance-sionna-mumimo-monolith.sh

# Fixed HE rate, mobile STAs and a chosen fresh result root.
RATE_ADAPTATION=fixed MCS=5 MOBILE_SCENARIO=true MOBILE_SPEED=1 \
OUTPUT_DIR=/tmp/mumimo-example \
  bash contrib/sionna/examples/performance-sionna-mumimo-monolith.sh
```

All environment settings are grouped at the top of the runner. Extra C++
`--name=value` flags override their defaults, for example
`--mobile_scenario=true --simulationTimeSeconds=20 --prtTput=true`.
Position values containing semicolons must be quoted:
`--staPositions='x,y,z;x,y,z;x,y,z;x,y,z'`.
`PRECODING=rzf` requires an explicit finite positive `RZF_ALPHA` (or
`--rzfAlpha=...`); no alpha is silently selected. ZF is the default.
The original monolith runner defaults to four stations and four AP antennas.
The target accepts station counts from one through eight and AP antenna counts
from two through eight; each STA has one antenna and `streams` must remain one.
Native MU operation is limited to 20 MHz. A valid 20 MHz 5 GHz channel can be
chosen with `CHANNEL`/`--channel`.

The target is registered in the example CMake list. On an already configured
tree, rerun your usual ns-3 configure command with `--enable-examples` so the
wrapper discovers the new target; retain your existing configuration options.
Use your existing Sionna Python environment. `BUILD=true`
builds that target with two jobs; `BUILD=false` reuses a build you have made.
`--help` shows runner usage without a build; `--PrintHelp` invokes the existing
binary with `--no-build`, so that form requires the binary to exist already.

Outputs from the original four-station runner are under
`OUTPUT_DIR/run/stas-4/`. The sweep script adds an outer `N-sta/` directory and
writes each case under `N-sta/run/stas-N/`:

- `throughput_results.csv`: TCP payload goodput per STA, default 100 ms intervals;
  the last interval uses its actual shorter duration if needed. It is long-form:
  each interval has one row per STA. Plot `Time_s` against `Throughput_Mbps`
  grouped by `STA_Index` or `STA_NodeId`; no aggregate rows are emitted.
- `summary.csv`: per-STA bytes and mean goodput over the traffic window.
- `mu_summary.csv`: counts of actual configured-group HE-MU PPDUs and HE-SU
  PPDUs whose transmissions start inside that window. These are transmission counts,
  including retries, not delivered packets or airtime-normalized throughput.
- `config.txt`: settings, initial positions, node/user mapping, array dimensions,
  mapping/coding assumptions and timing; `computationtime.csv`: wall time.

`OUTPUT_DIR/run/run.log` preserves process output. All detailed recording is
off by default. `ENABLE_SNR`, `ENABLE_POSITIONS`, `ENABLE_RATE_TRACE`,
`ENABLE_PCAP`, `ENABLE_MIMO_TRACE` and `ENABLE_THROUGHPUT_10MS` retain their
monolith flags. `ENABLE_MIMO_TRACE=true` adds the existing SU SINR/PER/rank CSV
and a MU CSV with per-user effective SINR, model PER and normalized desired/IUI
power at bin zero; that bin is a diagnostic sample, not an RU average or an
empirical packet error rate. Rate traces use each MU user's STA-ID for MCS,
rate and NSS. PCAP and per-reception traces can be large. `PRINT_THROUGHPUT=true`
prints per-STA interval rates while retaining the complete log.

Server supervision and retries reuse `performance-sionna-mimo.sh`;
arguments/output handling reuse the monolith runner. `START_SERVER=false`
leaves an external server running. `START_SERVER=true` supports only
`tcp://localhost:5555`, rejects an occupied local port, waits up to 30 s for
readiness, monitors the owned server and stops it on exit. A managed server
crash or simulation timeout can restart the server and retry up to
`MAX_RETRIES=3`; unsuccessful attempts move to `run.failed-attempt-N` and
server logs remain in `OUTPUT_DIR`. Other failures, or failures with an external
server, are not retried. Existing result roots are rejected.
`RUN_TIMEOUT_SECONDS=1800` limits each simulation attempt, excluding build and
server startup; zero disables it. GNU timeout sends TERM, then KILL after 5 s.
Process-group cleanup and signal handling retain the existing supervision logic.

The native scheduler requires queued data and established BlockAck agreements
for **all configured users**. `BE_BlockAckThreshold=1` initiates agreements
promptly; the monolith's normal aggregation limits remain. Association, TCP
startup, sparse queues, failed agreements, transmission-time limits or rejected
channel groups can produce SU fallback. Every emitted MU PPDU is checked for
the configured user count, 20 MHz, one NSS each and the shared RU. Throughput
includes MU and SU delivery. Check `mu_summary.csv` before interpreting a run
as simultaneous full-group service; zero MU transmissions are explicitly
reported in the log. The example does not force a full-group transmission when
the native scheduler cannot produce one. Both ZF and RZF reject rank-deficient
groups on any required subcarrier; RZF does not lift this full-rank requirement.

Run the one-AP user-count sweep (1 through 8 STAs, with an eight-antenna AP)
from any directory:

```bash
bash contrib/sionna/examples/performance-sionna-mumimo-sta-sweep.sh
```

It writes one result subdirectory per station count. The one-STA folder contains
the MRT baseline; folders for two through eight STAs configure one fixed MU
group. The sweep uses a 2x4 AP array at eight antennas to provide a planar array
for the full eight-user case. Apartment station positions are drawn from the
existing same-room range. Set `MOBILE_SCENARIO=true` to enable the existing
wall-aware station mobility.

Minstrel is used on the AP, where the scheduler obtains each user's current HE
MCS from its SU TxVector. The scheduler fixes MU GI at 800 ns and NSS at one;
Minstrel's MU policy is therefore not a separate optimized MU rate controller.
The current native scheduler creates BCC TxVectors. STA HE capabilities are
limited to MCS 0–9, including with Minstrel, while the AP keeps the complete
mode list required by Minstrel's initialization. Fixed MCS 10/11 is rejected.
`LDPC=false` is the default; enabling it advertises SU LDPC support but does
not change native MU coding or remove the MCS ceiling.

The server's TX/RX arrays are global rather than per node. STA TCP ACK data
therefore uses fixed `OfdmRate24Mbps` and the existing legacy first-antenna-pair
propagation path; control/management frames retain that compatibility path too.
Reverse HE 1x4 reception and uplink MU are not modeled by this example. This
approximation can affect TCP ACK airtime and throughput; the results are not a
complete bidirectional asymmetric-array model.

CSI is ideal current-query CSI under the selected propagation-cache coherence
policy. Timestamp freezing ensures consistent selection at a simulator time;
it does not expose the original age of a cached RT sample. Positive feedback
delay, explicit CSI aging, MAC sounding and compressed MU feedback are not
added. Cache/lookahead/mobility semantics remain those of the existing scripts;
no historical channel is fabricated. External interference retains the native
scalar treatment after spatial abstraction. Beta-free BER mapping does not
claim universally calibrated coded PER. No build, syntax check, test or
simulation was run while creating these files; runtime behavior remains to be
validated when execution is authorized.
