# R5: physical CSI aging acceptance contract

R5 is one complete CPU study, fixed before measurement. The scene is
`simple_room`, carrier 5180 MHz, 20 MHz/256 FFT bins, 2x2 arrays at 0.5 lambda,
TX/RX positions (3,2,2)/(4,3,1) m, TX/RX azimuths 90/90 and 90/0 degrees.
Seeds 42/43/44, run 1, streams 0/1, 100000 samples, depth 3, explicit arrays,
and one LLVM thread match the existing channel study. Each setting has two
fresh repetitions. RX velocity is either zero or (1,0,0) m/s; TX and scene
objects are stationary. Transmission is at 50 ms, with CSI ages 0/1/5/20/50 ms.

Two physical models are measured separately, without asserting equivalence:

* `position`: production `SionnaEnv.compute_cfr` retraces the actual linear
  receiver trajectory at each sounding/transmission time. The server's existing
  constant-speed/direction mobility contract is used, without wall crossings.
* `doppler`: Sionna RT `Paths.cfr` evolves paths traced at time zero, using
  receiver velocity and each path's Doppler shift. Geometry, delays, amplitudes
  and visibility are held at that anchor. This is a local constant-path
  approximation, not a retraced moving scene. Recorded positions describe the
  corresponding linear trajectory; the anchor is recorded separately.

No analytic TX-basis rotation is used. Normalize each time snapshot with the
production pair-power convention and retain its own wideband loss. ns-3 selects
equal-power, unquantized SVD W from the sounding CFR and applies it to the
separate transmission CFR. All ages for one trajectory share the identical
transmission channel and total power. Perfect RX CSI is retained. MI and SINR
are diagnostic, without a new PER, throughput or mobility calibration claim.

Acceptance requires complete identities/timestamps/grids/finite unit-power
channels; native SVD/MMSE/MI agreement with independent NumPy at rtol/atol 1e-8
(singular values 1e-10); independent path-sum Doppler agreement at rtol 2e-4,
atol 2e-9; zero-age and stationary controls; positive-age channel change for
each moving trajectory; repeatable raw responses and deterministic native
metrics; relocatable bundle integrity and sequential CPU regressions. Fresh
full-stream SVD attains the logdet bound; delayed linear-MMSE MI cannot exceed
it (tolerance 1e-7). No monotonic per-stream loss or convergence is required.
The fixed sample budget is not a convergence certificate. Position and Doppler
results, delays, loss, correlation, per-stream SINR, MI loss and timing are
exported separately. Automatic sounding, production Doppler dispatch, MAC
feedback, cache scheduling, moving scatterers and GPU validation are later work.

From the ns-3 directory, in the existing CPU Conda environment:

    ./ns3 build -j 2 ns3_wifi_sumimo_benchmark sionna-test
    python contrib/sionna/tools/benchmark_physical_aging.py --output=/tmp/r5-study
    PHYSICAL_AGING_RESULTS=/tmp/r5-study python contrib/sionna/tools/run_cpu_regressions.py

Nondefault samples/repetitions require a fresh output directory and remain
explicitly separate from the fixed acceptance. `run.json` records failures,
inputs, source identities and artifacts. `validate_export` checks completeness,
saved pairs and scientific metrics independently; optional native replay uses
the paired `physical-aging` action. Export the full study with
`experiment_bundle.py`, retain its external index hash, move it and verify it.
