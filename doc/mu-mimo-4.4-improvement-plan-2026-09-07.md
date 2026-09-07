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

## Stopped checkpoint (user requested immediate stop)

The user requested stopping due to session budget. No 4.4 acceptance is claimed.
- Coherence units fix: reviewed; 11 lightweight tests passed. Server fixtures now
  use 5 ms. Full server rerun is outstanding (initial run stopped after three
  passes because old fixtures requested 100-second look-ahead increments).
- MU production edits are in the working tree. An intermediate `./ns3 build
  sionna -j 2` passed before final selected-tone/runtime edits.
- Delegated agents were interrupted at the user's stop request. Only the PHY
  fixture helper classes have been added to `test/sionna-test-suite.cc`; the real
  reception test is not yet written. The planned standalone RU regression file
  and its CMake registration do not yet exist. Inspect helpers before compiling.
- Next: build `sionna-test`, run `sionna` and the new RU suite if registered;
  finish/execute AP + two users + bystander reception and RU-grid invariance
  regressions, then 4.5/CSI/SU regressions. Run the corrected server suite.
- Only then apply the detailed model rewrite prepared at
  `/tmp/update_mu_documentation.py` (it currently says validated; adjust to actual
  evidence), update this checkpoint, regenerate PDF once, and commit 4.4.
- Unrelated initial `.idea/`, generated protobuf/cache files, plots, and
  `tools/plot_current_progress.py` were present before this task; leave them alone.
