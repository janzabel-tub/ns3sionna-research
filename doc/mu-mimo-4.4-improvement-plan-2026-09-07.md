# Subphase 4.4 correction plan, 7 September 2026

Baseline: `1fc72f9`; previous review: `mu-mimo-review-2026-09-05.md`.
The current checkout already contains 4.5. Preserve its runtime API and SU behavior.

1. **Separate server correction.** Convert `fixed_coherence_time_ms` to nanoseconds
   once when receiving configuration. All three fixed-time branches use the converted
   duration; zero selects the existing dynamic calculation. Add focused tests that
   execute the real server paths without requiring ray tracing. Review, run tests,
   update model/PDF once, and commit separately.
2. **RU frequency contract.** Add optional occupied CFR indices to the existing
   user transmission structures. The direct numerical API retains empty=full-grid
   compatibility. Runtime must supply the RU's explicit occupied data and pilot
   tones from `HeRu::GetSubcarrierGroup`, on the transmit PSD/CFR grid. Exclude
   DC, nulls, guards, and other RUs from EESM. Reject duplicates/out-of-range bins.
   Test frequency-selective allocation and invariance to outside-RU changes.
3. **RU power contract.** On every tone compute active group energy
   `E_g[k] = sum_(j in g) ||w_j[k]||^2` and use `rho_u/E_g[k]` in MMSE.
   Reject zero group energy. Preserve relative weights within a RU and the old
   single-group normalization. Test norms 1.5/0.5 with equal and unequal RU power,
   same-RU unequal weights, and zero-power rejection.
4. **Reception contract.** Match installed state by directed transmitter/receiver.
   Other receivers and other transmitters use their fixed/cache physical channel.
   Common `PSD_NON_HE_PORTION` uses equal TX power, first RX branch physical gain
   `sum_tx |H[k,0,tx]|^2/N_tx`; no payload W, beta, or inter-user interference.
   Sample matching frequency centres when the non-HE grid is four times coarser.
   Apply MMSE/EESM only to addressed `PSD_HE_PORTION`, scaling the receiver's RU.
   Preserve physical outside-RU energy and skip payload tagging for bystanders
   and common fields. Keep strict metadata validation for installed participants.
5. **Reception evidence.** Retain the direct PPDU wiring test and add a real
   `SpectrumWifiPhy::Send` through `MultiModelSpectrumChannel`, one AP, two
   scheduled users, and a listening bystander. Assert preamble/payload progress,
   independently expected PHY SINR and decoded packets, with no bystander decode.
   Cover correlated payload interference separately where deterministic.
6. **Acceptance and traceability.** Run builds/tests sequentially after edits
   stabilize: focused native suites, `sionna`, `dl-mu-mimo-runtime`, CSI feedback,
   all existing SU positive/negative examples, and Python contracts. Update the
   German model's status, formulas, acceptance text, limits, and actual evidence.
   Render/validate its PDF exactly once after final documentation edits for this
   subphase, run diff checks, and commit only task files.

No scheduler, helper MU enablement, uplink, speculative cache, or calibration work
is included. Full group evaluation remains an explicit performance limitation.

## Resumed implementation, 11 September 2026

The 7 September pause was saved in `8c112b6`; work resumed on 11 September.
The corrected Python fixtures and the complete server suite now pass: 19 tests
in 229.102 s, including real 2x2 CFR. Native verification passed on 12 September 2026.

Implementation and acceptance mapping:

| Review finding | Implementation | Regression evidence passed |
| --- | --- | --- |
| Unscheduled receivers abort | Directed state matching; physical fallback for bystanders and unrelated transmitters | Real three-STA channel case and direct unrelated-AP case |
| EESM includes other frequencies | Explicit occupied data/pilot CFR indices; DC/null/guard exclusion | Numerical two-bin counterexample; independent full242, split106 and centre26 RU mappings |
| RU power changes with column norms | Per-tone active-column energy denominator | 1.5/0.5 column powers with equal/unequal budgets, same-RU weights, zero-group rejection |
| Payload abstraction affects preamble | Separate non-HE physical contract and 4:1 frequency-centre mapping | Frequency-selective coarse-grid check; orthogonal success and correlated payload failure after preamble |
| Only direct PPDU adapter tested | Actual `SpectrumWifiPhy::Send` through installed channel | PHY payload callbacks, measured SINR, bystander CCA and no decode |
| Fixed time uses wrong units | Boundary conversion to nanoseconds (commit `8c112b6`) | 19 Python/CPU-server tests passed |

Final review additionally corrected other allocated RUs' physical receive energy
at an addressed receiver: use the actual `H*W` columns and group energy, not
isotropic gain. The direct test requires zero other-RU energy when that group's
precoder is orthogonal to the receiver. Unallocated spectral-mask bins retain
an explicit equal-TX-power approximation.

Production changes stay in the existing numerical/HE/spectrum files. New test
files are `test/dl-mu-mimo-ru-test-suite.cc` and
`test/he-dl-mu-spectrum-ru-test-suite.cc`; the real reception case extends
`test/sionna-test-suite.cc`. Full group evaluation remains a known performance
limitation. Helper MU enablement, automatic scheduling, uplink, and calibration
remain outside this correction.

Validation completed on 12 September 2026: the native `dl-mu-mimo-ru`,
`he-dl-mu-spectrum-ru`, `sionna`, `dl-mu-mimo-runtime`, CSI-feedback, and all
listed SU example suites passed. The complete 19-case Python/CPU-server suite
also passed. Subphase 4.4 is accepted with the deferred limits stated above.
