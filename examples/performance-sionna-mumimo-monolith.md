## TCP and saturated UDP

TCP remains the default. Set `TRANSPORT=udp` (or `--transport=udp`) in either
monolith/sweep for continuously-on UDP, preserving the same offered load and
native Wi-Fi acknowledgments. See [transport comparison](performance-sionna-transport-comparison.md)
for commands, diagnostics, and fixed-NSS controls.

## Reciprocal HE reverse path

`REVERSE_PATH=reciprocal-he` (the example and station-sweep default) sends
STA TCP ACK data using single-stream HE Minstrel, MCS 0–9, and native
A-MPDU aggregation. The AP combines all of its receive antenna ports with the
existing MMSE/MRC abstraction. `REVERSE_PATH=legacy24` restores the fixed
legacy 24 Mbit/s reverse-data baseline. The equivalent C++ option is
`--reversePath=reciprocal-he|legacy24`.

This is an explicit ideal-reciprocity model: `H_UL[k] = transpose(H_DL[k])`,
without conjugation, with the same antenna ports, loss, delay, frequency grid,
and propagation-cache validity. An opt-in AP/STA registration canonicalizes
requests to AP→STA even when the first transmission is uplink; no server or
IPC changes are required. Uplink reception ignores the downlink precoder for
that reception while preserving installed downlink state. STA-to-STA
hearing and non-HE control retain the existing scalar approximation.

The small configure-time `ns-3.40-minstrel-he-mcs-list.patch` makes native
Minstrel initialization honor the actual local HE MCS list rather than indexing
12 entries unconditionally. This enables the existing STA MCS-9 capability
ceiling without changing rate learning or native aggregation.

`config.txt` records `reversePath`, `staDataMode`, and `staUplinkModel`.
This change neither schedules uplink users nor changes EDCA/TXOP, downlink
rates/grouping, BlockAck sequence, power, or aggregation limits. Legacy and HE
comparisons must use the same source version and channel/cache settings.
The older legacy-reverse descriptions and November 8 results below describe
`REVERSE_PATH=legacy24`.

## Combined MU rate and grouping policy

The default is now `MU_GROUPING=adaptive MU_RATE_CONTROL=ideal-csi`. The configured
station count is a ceiling: each actual MU transmission has 2–N eligible users
sharing RU242, one NSS each. The oldest-served eligible user is mandatory during
bounded weakest-user pruning, so channel-based pruning cannot continually exclude
that peer. Fewer than two eligible/feasible users use native SU fallback with MRT.
The strict full-group reference remains available with `MU_GROUPING=fixed`.

MU rates are explicitly ideal-CSI MCS 0–9/BCC, selected with the existing
ZF/RZF, mean-BER/EESM mapping and receiver error model. A bounded coordinate
search scores predicted successful PSDU bytes per native complete exchange
time (common PPDU, protection and acknowledgment). Queue-based size estimates
are refined once after native aggregation using actual PSDU sizes; the refined
TxVector is installed only if all MPDUs still satisfy native limits. This is a
bounded heuristic, not an exhaustive optimal scheduler or collision predictor.
Physical SINR tables are reused only for exactly unchanged channels, losses,
frequency grids, RX gains and transmit power; per-group precoders retain the
existing rank checks and total AP power normalization.

SU still uses Minstrel (or the selected fixed AP rate). The small configure-time
`ns-3.40-external-mu-rate.patch` excludes externally selected MU data feedback
from SU Minstrel statistics without disabling native BlockAck/retry bookkeeping.
The exclusion follows the last actual QoS-data format per peer, including BAR
recovery; a later SU data transmission re-enables SU learning. The ideal-CSI MU
policy has no feedback-learning state to mix with SU. This is not a separate
feedback-driven MU Minstrel implementation.

For the original rate/group behavior select both:

```sh
MU_GROUPING=fixed MU_RATE_CONTROL=station-manager bash contrib/sionna/examples/performance-sionna-mumimo-monolith.sh
```

`mu_group_sizes.csv` reports actual user counts. In `mu_summary.csv`,
`MuPpdusConfiguredUsers` counts only full-ceiling groups; the appended
`MuPpdusTotal` includes all MU sizes. Existing strict-mode columns keep their
meaning. `config.txt` records both policies and the ideal-CSI assumption.
Diagnostics and PHY allocation assertions apply to each actual subset. Native
EDCA/TXOP, bandwidth, power and aggregation limits are unchanged.
The reverse-data model follows `REVERSE_PATH` as described above. The earlier fixed-group description below applies to the
explicit legacy baseline.

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

For compact scheduler and frame-exchange diagnostics, set
`ENABLE_MU_DIAGNOSTICS=true` (or `--muDiagnostics=true`). This leaves selection,
power, aggregation limits and rate control unchanged and enables the existing
MIMO SINR/PER traces alongside four CSVs:

- `mu_scheduler.csv`: one row per configured user per scheduler invocation,
  with decision ID, outcome/detail, TID, queue counts, eligibility, CW, last backoff draw
  and available time. Queue counts include in-flight/expired entries;
  `queued_not_transmittable` is distinct from an empty queue. `not_checked`
  means selection returned before evaluating that user. Runtime exception
  details are retained, including rank and incomplete-group rejections.
  BE CW/backoff values come from native traces; the backoff draw is not a count
  of slots remaining at the scheduler invocation. `Available_ns` retains native
  `Time::Min()` for an initial frame without a remaining-TXOP time budget.
- `ppdu_tx.csv`: actual AP and STA transmissions, one row per receiver PSDU,
  with a run-local `TxId`, native duration calculation, NAV Duration/ID, frame
  kind (including TCP ACK, MAC ACK, BAR and BlockAck), MCS/GI/NSS/coding/RU,
  aggregation, retries, power, queue bytes and CW. MU receiver rows share one
  `TxId`; count airtime once. Same-time scheduler decisions are linked when
  available. Native HE-FEM bypasses before BA establishment or for pending BARs
  are not custom-scheduler rejections; deferred/unmatched selection is labeled.
- `phy_tx.csv`: exact native PHY TX-state start/duration, one row per `TxId`.
  Use this for airtime and overlaps; it does not use microsecond PCAP rounding.
- `mac_events.csv`: individual ACK/negative-BlockAck/drop/timeout events,
  addressed PHY drops, and BE CW/backoff changes on every device. `TxId=0` means
  no known data attempt (for example a control timeout or a pre-TX drop).
  `Value` contains the native timeout/drop enum or new contention value;
  `Cw` is the CW at the callback, with subsequent updates recorded separately.
  Attempt linkage includes packet UID, sequence, TID and peer to avoid sequence
  wraparound and cross-user ambiguity. Events are not deduplicated by packet UID.

The rate CSV correctly removes A-MPDU delimiters before decoding MAC addresses.
The diagnostics are single-link records with nanosecond timestamps and include
startup; apply the configured traffic window during analysis. Model PER remains
a prediction rather than an acknowledgment outcome. Full PCAP is optional.

The frame-exchange audit retains native `TryAddMpdu` acknowledgment/protection
budgeting, native A-MSDU/A-MPDU construction, retry sequence numbers and
HE-FEM's normal/BlockAck retry/NAV/TXOP handling. Partial MU success may reset
CW under native rules; this alone is not an integration fault. SU-derived MU
rate selection and legacy TCP-ACK uplink remain separate limitations. No CW,
TXOP, acknowledgment policy or PHY error model is changed by this option.

Combined verification on Sionna 2.2.0 (2026-11-08) passed the existing Sionna,
MU runtime, BlockAck, HE multi-user MAC and TXOP suites. Paired seed/run-1
eight-STA runs with diagnostics off/on produced identical throughput and
MU/SU summaries (44.9804 Mbit/s, 116 MU / 3,902 HE-SU PPDUs). All 48,106 TX
records matched native PHY durations; all 71,287 rate-log attempts matched
per-peer MPDU counts. No native MAC behavioral change was justified by the audit.

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
claim universally calibrated coded PER.
