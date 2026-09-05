# MU-MIMO implementation review, 5 September 2026

Reviewed revision: `4ba0191`. This report reviews the unfinished implementation;
it does not change simulation behavior. Findings distinguish implemented defects,
missing validation, and deliberately deferred functionality.

**Assessment**

The architecture and numerical foundation are promising. Phase 4.4 needs another
correctness and reception-validation pass before its acceptance criterion should
be considered satisfied. The current code is useful as an explicit numerical and
adapter API, but is not yet a usable, validated MU-MIMO network simulation feature.

| Area | Rating | Reason |
| --- | --- | --- |
| Plan structure | 8/10 | Clear contracts, analytical acceptance criteria, and sensible separation of downlink and uplink. Phase 4.5 combines too many independent concerns. |
| Phase 4.1–4.3 numerical foundation | 8/10 | Dimensioned snapshots, explicit stream mapping, full MMSE covariance, SVD-based ZF/RZF, and substantial analytical tests. Native tests were inspected, not rerun here. |
| Phase 4.4 correctness/readiness | 4/10 | RU frequency selection, RU power normalization, bystander reception, and preamble handling need correction. |
| Verification | 6/10 | Strong numerical coverage, but the PPDU test stops at adapter output and misses important physical reception cases. |
| Minimal implementation | 7/10 | Reuses existing contracts and ns-3 metadata; repeated group evaluation and validation offer concrete simplification opportunities. |
| Documentation and change discipline | 7/10 | Phase commits include tests and model documentation; stale status text and an undocumented, untested coherence-time change weaken traceability. |

Four of eight Phase-4 subphases are labelled implemented. That is a milestone
count, not 50% of the engineering effort. My approximate effort-based estimate is
50–60% of the usable downlink objective (4.1–4.6), and 35–45% of the complete
downlink/uplink objective (4.1–4.8). Scheduler, runtime state, helper integration,
actual traffic examples, uplink, and the fixes below are substantial remaining
work. These estimates exclude the separate Phase-5 calibration programme.

**Commit and phase audit**

| Commit | Scope | Review |
| --- | --- | --- |
| `c69852d` | Sionna 2.0.1 migration and expanded MU plan | Good baseline separation and explicit CPU reference; recorded historical results are not evidence for later server changes. |
| `9a2e9fe` | 4.1 snapshots | Additive protobuf identity, stable ordering, heterogeneous RX representation, and validation before cache insertion are appropriate. Actual RT still uses a shared RX array, as documented. |
| `45fa545` | 4.2 fixed MU core | Desired/inter-user/intra-user terms and per-user MCS/PER are separated correctly in the reviewed algebra. Four new model files are reasonable boundaries, not inherently excessive. |
| `6cdcd07` | 4.3 ZF/RZF | Correct full-row-rank SVD inverse construction and joint normalization. Explicit positive alpha and one-RX/one-stream restrictions are clear. |
| `5760ed9` | Coherence-time behavior | Contains a milliseconds/nanoseconds bug; no tests or documentation changed in this commit. Helper line-ending changes inflate its apparent size. |
| `7204954`, merged by `4ba0191` | 4.4 HE-DL-MU adapter | Reuses STA-ID/RU/MCS/NSS, but its acceptance claim exceeds the current test and implementation evidence. |

Phases 1–3 were examined as the supporting contracts and regression baseline;
this is not an exhaustive re-audit of every SU-MIMO commit or feedback formula.

**Actionable findings, in priority order**

1. **[P1] Unscheduled receivers terminate normal channel delivery.**
   `model/sionna-spectrum-propagation-loss-model.cc:599–615` aborts when the
   current receiver is absent from the installed group, and requires every
   snapshot transmitter to match the current transmitter. A normal AP plus
   three-STA network scheduling only two STAs can therefore abort at the third
   station. This is ordinary overhearing, not a malformed scheduled user.
   The ns-3.40 channel distributes signals to receiving PHYs and calls spectrum
   propagation before the receiving PHY filters or decodes them
   ([upstream channel source](https://www.nsnam.org/docs/release/3.40/doxygen/d8/dc0/multi-model-spectrum-channel_8cc_source.html)).
   Preserve a physical propagation/interference path for bystanders and unrelated
   transmissions; reserve group validation failures for addressed participants.
   Test one AP, two scheduled STAs, and a third listening STA through the channel.
   The model document's instruction to abort unaddressed receivers also needs
   correction; the defect is in that design choice as well as its implementation.

2. **[P1] EESM uses frequencies outside the receiver's RU.**
   `model/he-dl-mu-mimo-link-abstraction.cc:225–236` collects every subcarrier;
   `model/sionna-spectrum-propagation-loss-model.cc:480–510` passes only an
   orthogonality-group identifier and RU bandwidth, not selected CFR indices.
   The runtime snapshot covers the full PSD grid (`:628`), including frequencies
   outside the RU. Consequently, fading on another RU or guard frequencies
   changes this user's effective SINR and PER. Even the full-width RU requires
   an explicit occupied-tone contract.
   For an illustrative two-bin SINR vector `[10, 0]` and beta 2, all-bin EESM is
   `1.3728636641`; if only the first bin is allocated, the result must be 10.
   Map RU frequencies to CFR/precoder indices before reduction, with an explicit
   data/pilot/null-tone policy. Test frequency-selective RUs and invariance to
   changes outside the allocated RU; flat one-bin cases cannot detect this.

3. **[P1] Global precoder normalization does not guarantee per-RU power.**
   `model/dl-mu-mimo-link-model.cc:517–533` divides reference SNR by the number
   of streams in each orthogonality group. The existing matrix contract only
   requires the sum of all column powers to equal the total stream count.
   With column powers 1.5 and 0.5, that contract is satisfied for two streams.
   Put those columns on separate RUs with powers 0.8 and 0.2: the implemented
   scaling implies transmitted power `0.8*1.5 + 0.2*0.5 = 1.3`, although the PSD
   provides 1.0. This is a normalization failure, independent of legitimate
   receive beamforming gain. Unequal ZF/RZF column norms can also expose it.
   Define per-RU normalization of the active columns, or reject incompatible
   group norms until that contract exists. Preserve intentional within-RU
   weighting. Test non-unit column norms with both equal and unequal RU powers.

4. **[P1] The MU payload abstraction is also applied to the common preamble.**
   The runtime selects its MU branch solely by `WIFI_PREAMBLE_HE_MU`
   (`model/sionna-spectrum-propagation-loss-model.cc:585–592`, `:657`) and scales
   the complete PSD using payload MMSE/EESM (`:756–770`). It does not inspect
   `HePpdu::GetTxPsdFlag()`. ns-3.40 transmits separate non-HE and HE portions
   of the same MU PPDU
   ([upstream HePhy source](https://www.nsnam.org/docs/release/3.40/doxygen/d9/d70/he-phy_8cc_source.html#l01443)).
   Thus payload inter-user interference and payload MCS beta also affect common
   preamble/header reception. For correlated users, payload SINR can saturate
   near 0 dB while the common preamble's physical SNR continues to rise.
   Give the non-HE portion an explicit propagation contract and apply payload
   abstraction to the appropriate HE portion. Verify preamble detection,
   headers, and actual payload reception using a normal PHY transmission.

5. **[P1] Fixed coherence time is one million times too short.**
   In `model/ns3sionna/ns3sionna_server.py:277`, `:386`, and `:500`, values
   from `min_coherence_time_ms` are used directly as nanosecond durations.
   `model/message.proto:46` defines milliseconds; the dynamic helper returns
   nanoseconds (`model/ns3sionna/ns3sionna_utils.py:137`). Requesting 5 ms
   therefore yields 5 ns, affecting cache expiry and look-ahead spacing.
   Executing the actual three fixed-time source branches confirmed this value.
   Convert once at the server configuration boundary, keep internal time in
   nanoseconds, and test positive fixed time and zero/dynamic mode. This bug is
   from the intervening coherence-time commit, not the MU matrix equations.

6. **[P2] The advertised PPDU integration test does not verify PHY reception.**
   `test/sionna-test-suite.cc:3073–3253` creates an HE-portion PPDU, calls
   `CalcRxPowerSpectralDensity` directly, and compares its output with
   `CalculateHeDlMuMimoRx` from the same implementation. It does not install the
   adapter on the channel and run transmission/reception events, nor assert
   observed PHY SINR, decoded packets, or reception PER. It is a useful adapter
   wiring test, but cannot establish the Phase-4.4 reception acceptance claim.
   Keep it; add one deterministic PHY/channel reception case with independent
   expectations, preamble plus payload, and a bystander. A manually scheduled PHY
   transmission can do this without implementing the Phase-4.5 scheduler first.

**What appears correct, and deliberate limitations**

For a single RU and the stated full-rank one-RX/one-stream contract, the ZF
construction `V diag(1/sigma) U^H` and RZF construction
`V diag(sigma/(sigma^2+alpha)) U^H` are appropriate. Joint Frobenius normalization
preserves the Phase-4.2 power convention. I found no algebraic error in that
construction. Building the full receive covariance and then separating desired,
intra-user, inter-user, and noise terms gives the intended linear-MMSE SINR.

Rejecting rank-deficient groups for RZF is stricter than mathematically necessary,
but is explicitly planned and tested; it is not a defect against this contract.
Likewise, helper rejection of MU, absent runtime CSI/scheduler, absent uplink,
shared RT receive geometry, and missing calibrated beta defaults are documented
scope limits. They should not be presented as completed functionality. The
desired/interference diagnostics are combiner-dependent normalized quantities,
not automatically physical received watts; preserve that distinction in examples.

**Minimal next implementation sequence**

1. Fix fixed-time units in the existing server file, add focused tests to the
   existing Python suite, document the behavior, and commit separately.
2. Reopen 4.4 for RU tone selection, per-RU power, preamble handling, and
   bystander reception. Use the existing MU model/adapter files and native test
   suite. Add the reception test before declaring this milestone complete.
3. Split 4.5 into independently accepted steps: directed runtime CSI/precoder
   identity; fixed deterministic scheduler group/RU selection; then optional
   correlation grouping and MU feedback. Move correlation heuristics after the
   first working helper/example if necessary. Avoid designing a general cache
   framework before the fixed-group path works.
4. Complete the 4.6 helper and reproducible SU-round-robin versus MU example.
   State precisely what equal power and equal airtime mean, including the
   handling of overhead. Require deterministic per-user receive results.
5. Start uplink only after that downlink path is accepted. Keep link-level
   calibration separate, and label any uncalibrated PER examples accordingly.

The runtime currently evaluates every group user again for each receiving STA
(`model/sionna-spectrum-propagation-loss-model.cc:690–761`), repeatedly discovers
devices and PSDUs, and constructs a new abstraction. This makes the group
calculation quadratic in receiver count at the adapter level. Prefer evaluating
only the current receiver with the shared precoder, or once per immutable PPDU
state after 4.5 defines its identity. Do not add a speculative cache now.

Native snapshot validation is also split: protobuf deserialization checks array
identity, endpoints, loss and normalization, while the direct numerical validator
checks a smaller subset. An explicit installed-state validator would make the
claim of a fully validated snapshot accurate without repeating checks throughout
the runtime. Keep any intentionally permissive synthetic numerical API distinct.

Phase 4.4 changed nine files, including 621 added test lines and four existing
MU model files; the test volume is justified. Minimize production indirection
and duplicated checks rather than deleting useful tests or merging unrelated
classes merely to reduce the file count. Avoid unrelated line-ending rewrites.

**Documentation and verification evidence**

The model's headline still says 21 August / completed through 4.3, although its
4.4 section and September validation log say otherwise. Its opening progress
list also stops at 4.3, and its last heading is the leftover `Check code in
ZeroForcing`. The coherence-time commit changes neither tests nor documentation,
and the public comments still call the parameter a minimum. Update these in the
relevant fix commits and regenerate the model PDF when that model changes.

Executed during this review:

- Existing Python contract suite: **7/7 passed in 0.002 s**. Used the available
  Windows `sionna` environment (Python 3.12, NumPy 1.26.4, protobuf 6.33.1), with
  bindings generated from the current `message.proto` using a temporary
  `grpcio-tools 1.76.0` installation. This is not the documented Linux RT stack.
- Executed the actual fixed-time `else` branches via Python AST, without loading
  RT: all three yielded 5 for an input of 5 ms; the required nanosecond value is
  5,000,000.
- Independently calculated the EESM and RU-power counterexamples above with
  Python/NumPy. These are checks of the mathematical failure cases, not native
  C++ execution or substitutes for regression tests.
- Working-tree `git diff --check`: passed before adding this report. Historical
  `c69852d..HEAD` checking reports CRLF as trailing whitespace with the local
  default; `git -c core.whitespace=cr-at-eol diff --check c69852d..HEAD` passes.

No usable ns-3.40 build/test harness was found in this checkout or the searched
university workspace. Native suites and RT/server integration were not rerun.
The model records 36 native cases and SU example regressions passing on
3 September; those are historical results, not newly verified results here.
In particular, the older server-validation log predates `5760ed9`.

This review adds only this report. Generated protobuf bindings and temporary
test dependencies are outside the repository. The pre-existing untracked
`.idea/` directory is unrelated and was left untouched.
