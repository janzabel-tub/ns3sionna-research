# R4: live CPU server and HE-SU PHY scaling

R4 is one complete subphase with a fixed workload and acceptance policy. The
benchmark sends real HE-SU PPDUs through SpectrumWifiPhy, the production Sionna
cache, and a separate local ZMQ server process. The server uses the production
`SionnaEnv.init_simulation_env` and `compute_cfr` dispatch, including
PathSolver, CIR/CFR extraction, normalization, identity metadata and Protobuf
construction. The experiment-local REP loop adds observation and a randomly
assigned localhost port. It uses fresh responses for every request; it does not
reuse frozen data. There are no changes to the production server, cache or PHY
implementation.

The fixed acceptance workload uses the existing Conda CPU environment and
`llvm_ad_mono_polarized`, one Dr.Jit thread, `simple_room/simple_room.xml`, seed
42, run 1 and RT/mobility streams 0/1. Receivers remain stationary. Paired cache
settings cover six representative receiver/antenna/width/look-ahead combinations
at 10000 samples, plus the 4-receiver/2x2/40-MHz/depth-1 pair at 100000 samples.
Each setting runs in three fresh process pairs. This is a representative matrix,
not a Cartesian sweep or an RT sample-convergence test. Each process sends two
PPDUs per epoch over four epochs, starting at 1 ms with 6 ms epoch spacing and 1
ms packet spacing. Coherence is 5 ms; PHY events therefore finish between
packets and cache validity boundaries are not used as accidental controls.

HE MCS 0, BCC, equal streams per transmit antenna, 20 dBm total transmit power,
explicit EESM beta 2 and packet size remain fixed. Beta 2 is an uncalibrated
diagnostic setting. Successful delivery and native SINR/PER diagnostics do not
extend the closed R2 calibration scope or establish scientific Wi-Fi throughput
accuracy. This benchmark does not install a MAC scheduler, model MU traffic, or
simulate physical channel aging.

## Measurement boundaries

`WifiPhy::Send` invokes scalar propagation loss and delay, while ns-3 schedules
CFR spectrum processing later. The example times Send and an example-local
forwarding call to the actual spectrum adapter separately. The latter includes
any CFR RPC, native MMSE/EESM work, the scalar spectrum handoff and the MIMO
trace observer's CSV formatting/stream write inside that call. The whole
`Simulator::Run` duration includes event processing and reception. These clocks
are elapsed wall time, not simulated radio airtime. Initialization and process
startup are separate from the simulation.

The server times parsing, production computation (including materialization of
RT/CIR/CFR into Protobuf), serialization and socket send separately. Raw request
and response files, response dimensions, simulated timestamps, byte lengths and
resource observations remain available. Server response validation and raw
artifact writes occur outside its stage timers. Native adapter trace writes
remain inside the adapter timer, and the whole simulation includes native memory
reads/CSV observation as well as server transcript and validation overhead
between RPCs. These are instrumented live workload timings, not isolated
uninstrumented kernel costs. Process scheduling also influences measurements;
differences between stage sums and whole-process time are not pure network
latency.

For measured durations x_1,...,x_n, summaries report n, median and the empirical
95th percentile. Cold first computations and subsequent computations are
separate. A case with no subsequent computation has no warm estimate. Three
identically seeded fresh processes are timing repetitions, not independent
channel samples; small-sample percentiles describe this workload only.

Native and server `/proc/self/status` observations report sampled current VmRSS
and process VmHWM. Native CSVs use bytes; server records retain labelled KiB.
The report converts Linux KiB to bytes by multiplying by 1024. HWM includes
imports, initialization, JIT caches and temporary RT/CIR data; snapshots do not
identify which allocator or object caused a peak. The separate process peaks
need not occur simultaneously and must not be added as a simultaneous total. The
native `peak_rss_bytes` is the maximum sampled current RSS; VmHWM is the kernel
lifetime peak. RSS is an observed process cost, not a hard resource budget or a
leak assertion.

Logical CSI payload, structural pool capacity and mobility array bytes are
reported separately. For L receivers, D future snapshots, Nsc tones and A
antennas per end, the decoded CSI payload of one full retained batch is

    B_batch = L D Nsc (16 A^2 + 4) bytes.

The production helper includes SpectrumWifiPhy guard bands: a 20/40-MHz PHY uses
a 60/120-MHz CFR grid with 768/1536 bins. This differs from the earlier 256-bin
numerical studies. The largest planned batch (L=4,D=4,Nsc=1536,A=2) has 1671168
logical bytes. This formula excludes container capacity, metadata, Protobuf/ZMQ
buffers, intermediates and allocator overhead. Disabled caching must retain zero
CSI payload and capacity. The terminal live occupancy is sampled at the workload
end; long-run expiry correctness remains covered by the existing R3 regression.
The R3 empty staging pool remains bounded independently; it is not a CFR pool.

## Acceptance and artifacts

Before measurements, `plan.json` fixes the controls and matrix. Every live
response must have the requested TX/RX identity, complete receiver group,
correct timestamps, depth, antenna dimensions and frequency grid, finite loss
and coefficients, and the production identity metadata where applicable. Native
observations must contain successful complete PHY receptions and exactly the
planned spectrum calls. Cache controls compare the same static geometry and
backend; latency and memory values are excluded from numerical equality.
Failures retain native/server logs, captured protocol bytes and an atomic failed
run status. All subprocesses have bounded lifetimes and are reaped.

`run.json` records source/build/environment/input identities and artifact
hashes. The local experiment catalog records the acceptance paths and trusted
digests; raw data remain local workspace artifacts. Existing R1/R2/R3 and
historical benchmark exports are unchanged. Optional local-artifact tests verify
the new live export; the normal CPU regression tests do not need ray tracing or
a live server. R5 physical aging and R6 CUDA/backend completion remain open.

The initial 0 dBm diagnostic run exposed early preamble drops in some 2x2 cases
despite finite matrix diagnostics. Its partial logs remain preserved. The final
acceptance plan fixes 20 dBm before the accepted measurements and requires at
least one successful complete PHY reception in every process. Preamble detection
and receive-error behavior are unchanged in the production model; this choice is
a workload control, not a PER calibration.

## Accepted results

The final 20 dBm run passed all 42 process pairs and 21 cache controls: 1890
actual RPCs, 11556 returned link/time snapshots, 336 sends and 912 successful
complete PHY receptions, with zero failures. The maximum retained decoded CSI
payload was exactly 1671168 bytes, and disabled caching retained zero CSI. Linux
high-water observations were 485756928 bytes for the server and 35504128 bytes
for ns-3, considered separately. On this AMD Ryzen 5 PRO 2500U host, the largest
depth-4 workload had median instrumented Simulator::Run durations of 56.13
seconds uncached and 0.54 seconds cached. This is an observed effect of existing
cache reuse, without a universal latency or memory guarantee.

Seven native suites (60 cases) passed. All 142 Python tests completed: 140
passed and two optional older export checks skipped. All 20 new R4 tests ran,
including the three complete live-export audits. The native target is included
in the CPU CI build; hosted CI and CUDA were not executed here.

The [catalog](../experiments/r4/catalog.json) records the complete relocated
bundle and external trusted index identity, sealed live records, derived report,
build/regression/verification logs and preserved earlier diagnostics. Bundle
verification does not claim a new portable R4 replay or a full GPU reproduction
package. The model document contains the per-setting latency table and commands.
