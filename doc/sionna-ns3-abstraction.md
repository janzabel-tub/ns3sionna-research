# Sionna-to-ns-3 Channel Abstraction

Sionna RT supplies the physical channel; ns-3 computes MIMO reception and packet outcomes.

- **Sionna:** ray traces the scene and antenna arrays, then exports wideband path loss, propagation delay and normalized complex matrices $H[k]$ with antenna dimensions and validity times.
- **Interface:** Protobuf/ZMQ transfers channel snapshots. ns-3 validates whole replies before caching them, preserves directed MU identities and future snapshots, and removes expired entries on cache access. With caching disabled, each request uses its fresh reply.
- **ns-3:** selects/applies precoding, computes stream SINRs (SU MMSE, DL-MU with inter-user interference, UL-MU ZF/MMSE), and maps them through EESM and the configured error model to packet reception in `SpectrumWifiPhy`. CSI feedback and aging also belong to ns-3; UL integration uses explicit simultaneous-user channel state.

```text
Sionna RT → path loss + delay + normalized H[k]
          → Protobuf/ZMQ → validated ns-3 cache
          → ns-3 precoding/detection → stream SINRs
          → EESM → error model / PER → SpectrumWifiPhy
```

This is a packet-level abstraction, without waveform demodulation. PER calibration is validated only for HE MCS 0/BCC/32-byte/1x1; EHT PER and broader profiles remain open. The sections below detail the SU path; current MU behavior and validation are documented in [the model](../ns3sionna_mimo_model.md).

## 1. Meaning of the tensor dimensions

Sionna returns a channel frequency response of the form

$$
H_{\mathrm{raw}}[\ell,r,q,t,n,k],
$$

where:

- $\ell$ is the receiver or link index;
- $r$ is the RX-antenna index;
- $q$ is the transmitter index;
- $t$ is the TX-antenna index;
- $n$ is the Sionna time-step index;
- $k$ is the OFDM-subcarrier index.

The link and time indices are not complex values embedded in an individual matrix. They are tensor dimensions used to select the required channel.

One ns3sionna request contains one transmitter and asks Sionna for one CFR time step. Therefore, $q=0$ and $n=0$. After selecting receiver/link $\ell$, the result is

$$
H_{\ell}[r,t,k]
=
H_{\mathrm{raw}}[\ell,r,0,t,0,k].
$$

For a fixed subcarrier $k$, $H[k]$ is an $N_{\mathrm{rx}}\times N_{\mathrm{tx}}$ matrix. All subcarrier matrices together form a three-dimensional tensor with logical shape `[rx, tx, subcarrier]`.

## 2. ns-3 requests the channel at the current time

The `SionnaPropagationCache` is queried when ns-3 needs propagation information. If it has no entry valid at `Simulator::Now()`, ns-3 constructs a request containing the transmitter ID, receiver ID, and current simulation time.

Relevant code from [`model/sionna-propagation-cache.cc`](../model/sionna-propagation-cache.cc#L289):

```cpp
ns3sionna::Wrapper wrapper;

ns3sionna::ChannelStateRequest* propagation_request =
    wrapper.mutable_channel_state_request();

propagation_request->set_tx_node(node_a->GetId());
propagation_request->set_rx_node(node_b->GetId());
propagation_request->set_time(current_time.GetNanoSeconds());

std::string serialized_message;
wrapper.SerializeToString(&serialized_message);

zmq::message_t zmq_message(serialized_message.data(),
                           serialized_message.size());
m_sionnaHelper->m_zmq_socket.send(zmq_message,
                                  zmq::send_flags::none);
```

The corresponding message definition in [`model/message.proto`](../model/message.proto#L113) is:

```proto
message ChannelStateRequest {
    uint32 tx_node = 1; // TX node ID
    uint32 rx_node = 2; // RX node ID
    uint64 time = 3; // simulation time (in ns)
}
```

The actual simulation time is therefore transmitted separately. It is not included in the flattened channel-coefficient arrays.

## 3. Sionna updates mobility and ray traces the scene

The Python server advances the relevant node positions to the requested simulation time and places the transmitter and receivers in the Sionna scene. It then runs the path solver.

Relevant code from [`model/ns3sionna/ns3sionna_server.py`](../model/ns3sionna/ns3sionna_server.py#L616):

```python
p_solver = PathSolver()

paths = p_solver(
    scene=self.scene,
    max_depth=self.rt_max_depth,
    samples_per_src=self.rt_samples_per_src,
    los=self.rt_los,
    specular_reflection=self.rt_specular_reflection,
    diffuse_reflection=self.rt_diffuse_reflection,
    refraction=self.rt_refraction,
    synthetic_array=self.rt_synthetic_array,
    diffraction=self.rt_diffraction,
    edge_diffraction=self.rt_edge_diffraction,
    diffraction_lit_region=self.rt_diffraction_lit_region)
```

The server calculates both the channel impulse response and the channel frequency response:

```python
a, tau = paths.cir(
    sampling_frequency=1e9,
    normalize_delays=False,
    out_type="numpy")

h_raw = paths.cfr(
    frequencies=self.frequencies,
    sampling_frequency=1.0,
    num_time_steps=1,
    normalize_delays=True,
    normalize=False,
    out_type="numpy")
```

These results serve different purposes:

- `tau` contains the physical ray delays. Its smallest non-negative value becomes the ns-3 propagation delay.
- `h_raw` contains the complex frequency-selective channel.
- `normalize_delays=True` shifts the first path to zero delay when constructing the CFR.
- `normalize=False` means Sionna does not remove the channel's received-power scale. ns3sionna explicitly separates that scale in the next step.

The shortest propagation delay is selected with:

```python
lnk_tau = np.squeeze(tau[rx_id, :, :, :, :])
lnk_delay = int(round(
    np.min(lnk_tau[lnk_tau >= 0] * 1e9), 0))
```

## 4. Selecting one link from the raw tensor

The exact tensor selection is implemented in [`model/ns3sionna/mimo_data_contract.py`](../model/ns3sionna/mimo_data_contract.py#L80):

```python
def extract_link_cfr(cfr, link_index):
    cfr = np.asarray(cfr)

    if cfr.ndim != 6:
        raise ValueError(
            "Expected a six-dimensional Sionna CFR, got {}".format(cfr.shape))

    if cfr.shape[2] != 1:
        raise ValueError(
            "Expected one transmitter, got {}".format(cfr.shape[2]))

    if cfr.shape[4] != 1:
        raise ValueError(
            "Expected one CFR time step, got {}".format(cfr.shape[4]))

    if link_index < 0 or link_index >= cfr.shape[0]:
        raise IndexError("CFR link index is out of range")

    return cfr[link_index, :, 0, :, 0, :]
```

The selected result has shape

```text
[num_rx_antennas, num_tx_antennas, num_subcarriers].
```

In normal P2P or P2MP operation, `link_index` selects a receiver. In look-ahead mode, future receiver positions are represented as additional virtual receiver entries. The mapping in [`model/ns3sionna/ns3sionna_server.py`](../model/ns3sionna/ns3sionna_server.py#L343) is:

```python
for curr_rx_id, curr_rx_node in enumerate(rx_nodes):
    rx_id = lah_time_idx * len(rx_nodes) + curr_rx_id
    h = extract_link_cfr(h_raw, rx_id)
```

Each future point becomes a separate `ChannelState` response with its own `start_time` and `end_time`. A time dimension is not retained in the flattened per-link CFR.

## 5. Separating wideband loss from small-scale fading

The raw Sionna CFR contains both large-scale attenuation and frequency/spatial channel structure. A CFR coefficient is a complex *field/amplitude* gain: for a SISO subcarrier, $y=hx$. Thus, for a transmit signal with power $\mathbb{E}[|x|^2]$, the received signal power is $\mathbb{E}[|y|^2]=|h|^2\mathbb{E}[|x|^2]$. This is why the coefficient's power gain is $|h|^2=\operatorname{Re}(h)^2+\operatorname{Im}(h)^2$, rather than $h$, $|h|$, or the square of an average complex coefficient.

ns3sionna takes the unweighted empirical mean of those power gains over all subcarriers and all RX--TX antenna pairs:

$$
\bar P =
\frac{1}{N_{\mathrm{sc}}N_{\mathrm{rx}}N_{\mathrm{tx}}}
\sum_{k,r,t}|H[r,t,k]|^2.
$$

It then creates the scalar wideband loss

$$
L_{\mathrm{wb}}=-10\log_{10}(\bar P)\ \mathrm{dB}
$$

and the normalized channel

$$
H_{\mathrm{norm}}[r,t,k]
=
\frac{H[r,t,k]}{\sqrt{\bar P}}.
$$

The exact implementation in [`model/ns3sionna/mimo_data_contract.py`](../model/ns3sionna/mimo_data_contract.py#L100) is:

```python
def normalize_link_cfr(cfr):
    cfr = np.asarray(cfr)

    mean_power = float(np.mean(np.abs(cfr) ** 2))

    if not np.isfinite(mean_power) or mean_power <= 0.0:
        raise ValueError("CFR mean power must be finite and positive")

    wideband_loss_db = float(-10.0 * np.log10(mean_power))
    return wideband_loss_db, cfr / np.sqrt(mean_power)
```

After normalization,

$$
\frac{1}{N_{\mathrm{sc}}N_{\mathrm{rx}}N_{\mathrm{tx}}}
\sum_{k,r,t}|H_{\mathrm{norm}}[r,t,k]|^2=1.
$$

The square root in the denominator is essential: it scales an *amplitude* coefficient. Squaring the resulting magnitude gives $|H_{\mathrm{norm}}|^2=|H|^2/\bar P$, whose average is one. Dividing $H$ by $\bar P$ itself would instead make its mean power $1/\bar P$.

This quantity is **not** the time-average power of a transmitted waveform, nor necessarily the total received signal power across the array and bandwidth. It is a per-coefficient channel-power reference for the one selected link and one CFR time step; every reported subcarrier and antenna pair receives equal weight. The physical received signal power also depends on the transmit PSD, transmit spatial covariance/precoder, and receiver combining. For a per-subcarrier transmit covariance $Q_x[k]$, the total power at the receive-antenna inputs is $\operatorname{tr}(H[k]Q_x[k]H[k]^H)$. The normalization deliberately leaves those frequency-selective and spatial effects in $H_{\mathrm{norm}}$ while putting its common scale into $L_{\mathrm{wb}}$.

The split prevents path loss from being applied twice:

- `wb_loss` determines total received power;
- normalized $H[k]$ determines frequency selectivity and spatial behavior.

## 6. Flattening the normalized CFR

The selected channel initially uses order `[rx][tx][subcarrier]`. Before flattening, it is transposed to `[subcarrier][rx][tx]`.

The implementation in [`model/ns3sionna/mimo_data_contract.py`](../model/ns3sionna/mimo_data_contract.py#L118) is:

```python
num_rx_antennas, num_tx_antennas, num_subcarriers = cfr.shape

# Transpose to [sc, rx, tx] before C-order flattening. Thus tx is the
# fastest-changing index, followed by rx and then the subcarrier.
flat_cfr = np.transpose(cfr, (2, 0, 1)).reshape(-1)

rx_node_info.frequencies.extend(int(value) for value in frequencies)
rx_node_info.csi_real.extend(float(value) for value in np.real(flat_cfr))
rx_node_info.csi_imag.extend(float(value) for value in np.imag(flat_cfr))
```

For a 2x2 channel, the flat order is:

```text
H[0](rx0,tx0)
H[0](rx0,tx1)
H[0](rx1,tx0)
H[0](rx1,tx1)
H[1](rx0,tx0)
H[1](rx0,tx1)
H[1](rx1,tx0)
H[1](rx1,tx1)
...
```

The corresponding flat-index formula is

$$
i=((kN_{\mathrm{rx}})+r)N_{\mathrm{tx}}+t.
$$

Real and imaginary components are transmitted as separate arrays because the Protobuf schema has no native complex-number type. The response also carries the TX/RX IDs and positions, delay, wideband loss, frequencies, antenna dimensions, and validity interval. See [`model/message.proto`](../model/message.proto#L120).

## 7. Reconstructing the CFR in ns-3

ns-3 validates the array lengths and dimensions, joins each real/imaginary pair, and constructs a `MimoChannelMatrix`.

Relevant code from [`model/sionna-propagation-cache.cc`](../model/sionna-propagation-cache.cc#L159):

```cpp
const int coefficientCount = message.csi_real_size();

NS_ABORT_MSG_IF(message.csi_imag_size() != coefficientCount,
                "Real and imaginary CFR arrays have different sizes");

const int numSubcarriers = message.frequencies_size();

const uint32_t numRxAntennas =
    message.num_rx_antennas() == 0 ? 1 : message.num_rx_antennas();

const uint32_t numTxAntennas =
    message.num_tx_antennas() == 0 ? 1 : message.num_tx_antennas();

const std::size_t expectedCount =
    static_cast<std::size_t>(numSubcarriers) *
    numRxAntennas *
    numTxAntennas;

std::vector<std::complex<double>> coefficients;
coefficients.reserve(expectedCount);

for (int i = 0; i < coefficientCount; ++i)
{
    coefficients.emplace_back(message.csi_real(i), message.csi_imag(i));
}

return MimoChannelMatrix(numSubcarriers,
                         numRxAntennas,
                         numTxAntennas,
                         coefficients);
```

The C++ class uses the same index formula as Python. From [`model/mimo-channel-matrix.cc`](../model/mimo-channel-matrix.cc#L85):

```cpp
return (static_cast<std::size_t>(subcarrier) * m_numRxAntennas + rx) *
           m_numTxAntennas +
       tx;
```

Consequently, `channel.Get(k, r, t)` returns matrix element $H[k]_{r,t}$.

## 8. Caching by link and validity interval

The reconstructed channel is cached together with its wideband loss, delay, validity interval, node identities and positions, and subcarrier frequencies.

Relevant code from [`model/sionna-propagation-cache.cc`](../model/sionna-propagation-cache.cc#L336):

```cpp
MimoChannelMatrix channel = DeserializeChannelMatrix(rxMessage);

CacheEntry entry = CacheEntry(delay,
                              wb_loss,
                              start_time,
                              end_time,
                              channel,
                              txId,
                              rxId,
                              Vector(txPos.x(), txPos.y(), txPos.z()),
                              Vector(rxPos.x(), rxPos.y(), rxPos.z()));
```

Later packets reuse an entry while the current simulation time lies between `m_start_time` and `m_end_time`. Sionna therefore does not need to run once per packet.

## 9. SISO and non-HE control-frame processing

For a 1x1 channel, or for a non-HE frame, ns-3 multiplies every applicable PSD bin by the channel power gain:

$$
S_{\mathrm{rx}}[k]
=
S_{\mathrm{pathloss}}[k]|H[k]|^2.
$$

The exact branch in [`model/sionna-spectrum-propagation-loss-model.cc`](../model/sionna-spectrum-propagation-loss-model.cc#L380) is:

```cpp
const bool legacySiso = channel.GetNumRxAntennas() == 1 &&
                        channel.GetNumTxAntennas() == 1;

const bool scalarCompatibilityPath = legacySiso || !isHeSu;

if (scalarCompatibilityPath)
{
    auto value = rxPsd->ValuesBegin();

    for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc, ++value)
    {
        *value *= std::norm(channel.Get(sc, 0, 0));
    }
}
```

`std::norm(h)` evaluates

$$
|h|^2=\operatorname{Re}(h)^2+\operatorname{Im}(h)^2.
$$

Thus the SISO packet-reception path retains frequency-selective power fading but does not use channel phase for decoding. Non-HE control frames on a MIMO PHY use the first antenna pair `(rx=0, tx=0)`. The complex CFR remains available through the packet's diagnostic/application tag.

## 10. HE SU-MIMO reference SNR

The scalar wideband path loss is applied before the spectrum MIMO adapter. The scalar propagation-loss implementation in [`model/sionna-propagation-loss-model.cc`](../model/sionna-propagation-loss-model.cc#L46) is:

```cpp
return txPowerDbm -
       m_propagationCache->GetPropagationLoss(a, b, txPowerDbm);
```

The MIMO path integrates this already attenuated PSD and includes the receiver gain:

```cpp
const double rxGainLinear = DbToRatio(rxPhy->GetRxGain());
const double receivedPowerW = Integral(*params->psd) * rxGainLinear;
```

Receiver noise is

$$
N=kTB\,10^{NF/10},
$$

and the total reference SNR is

$$
\rho=\frac{P_{\mathrm{rx}}}{N}.
$$

The MIMO adapter invokes the link-budget calculation in [`model/sionna-spectrum-propagation-loss-model.cc`](../model/sionna-spectrum-propagation-loss-model.cc#L283):

```cpp
const auto linkBudget =
    WifiLinkBudget::Calculate(WToDbm(receivedPowerW),
                              0.0,
                              bandwidthHz,
                              WIFI_NOISE_TEMPERATURE_K,
                              receiverNoiseFigureDb);
```

It passes `0.0` as path loss because `receivedPowerW` already includes path loss. The underlying implementation in [`model/wifi-link-budget.cc`](../model/wifi-link-budget.cc#L83) is:

```cpp
result.m_receivedPowerW = ns3::DbmToW(txPowerDbm - pathLossDb);
result.m_thermalNoisePowerW = BOLTZMANN_J_PER_K * temperatureK * bandwidthHz;
result.m_receiverNoisePowerW =
    result.m_thermalNoisePowerW * DbToLinear(receiverNoiseFigureDb);
result.m_referenceSnrLinear =
    result.m_receivedPowerW / result.m_receiverNoisePowerW;
```

## 11. Optional precoding

Without a configured precoder, one stream is transmitted per TX antenna:

$$
N_s=N_{\mathrm{tx}}.
$$

The model divides total reference power equally among those streams. With a configured precoder $W[k]$, ns-3 constructs the effective channel

$$
H_{\mathrm{eff}}[k]=H[k]W[k].
$$

The multiplication in [`model/mimo-precoding-matrix.cc`](../model/mimo-precoding-matrix.cc#L486) is:

```cpp
for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
{
    for (uint32_t rx = 0; rx < channel.GetNumRxAntennas(); ++rx)
    {
        for (uint32_t stream = 0; stream < m_numStreams; ++stream)
        {
            std::complex<double> coefficient{0.0, 0.0};

            for (uint32_t tx = 0; tx < m_numTxAntennas; ++tx)
            {
                coefficient += channel.Get(sc, rx, tx) *
                               Get(sc, tx, stream);
            }

            effectiveChannel.Set(sc, rx, stream, coefficient);
        }
    }
}
```

The configured matrix can represent fixed precoding, MRT, or SVD-derived precoding. Sionna continues to deliver only $H[k]$; $W[k]$ is selected and applied on the ns-3 side.

## 12. Per-subcarrier MMSE detection

For every subcarrier, ns-3 constructs the complex matrix used by the receiver:

```cpp
Eigen::MatrixXcd h(numRxAntennas, numStreams);

for (uint32_t rx = 0; rx < numRxAntennas; ++rx)
{
    for (uint32_t stream = 0; stream < numStreams; ++stream)
    {
        h(rx, stream) = channel.Get(sc, rx, stream);
    }
}
```

The total reference SNR is divided equally among streams:

```cpp
const double streamSnr = referenceSnrLinear / numStreams;
```

For each subcarrier, the linear MMSE error covariance is

$$
E[k]
=
\left(I+\frac{\rho}{N_s}H[k]^H H[k]\right)^{-1}.
$$

The post-processing SINR of stream $s$ is

$$
\gamma_s[k]=\frac{1}{E_{s,s}[k]}-1.
$$

The exact implementation in [`model/su-mimo-link-model.cc`](../model/su-mimo-link-model.cc#L95) is:

```cpp
const Eigen::MatrixXcd identity =
    Eigen::MatrixXcd::Identity(numStreams, numStreams);

const Eigen::MatrixXcd mmseMatrix =
    identity + streamSnr * h.adjoint() * h;

const Eigen::LDLT<Eigen::MatrixXcd> decomposition(mmseMatrix);
const Eigen::MatrixXcd inverse = decomposition.solve(identity);

for (uint32_t stream = 0; stream < numStreams; ++stream)
{
    const double inverseDiagonal = inverse(stream, stream).real();
    const double sinr = std::max(0.0, 1.0 / inverseDiagonal - 1.0);

    result.m_sinrLinear[
        static_cast<std::size_t>(sc) * numStreams + stream] = sinr;
}
```

Unlike the SISO power-only branch, this operation uses the full complex channel, including relative phases between antennas. It accounts jointly for desired-stream power, inter-stream interference, receive combining, array gain, and MMSE noise enhancement.

The model also calculates the minimum numerical channel rank and the ideal spectral-efficiency diagnostic

$$
\eta=
\frac{1}{N_{\mathrm{sc}}}
\sum_{k,s}\log_2(1+\gamma_s[k]).
$$

This is a diagnostic metric, not the final Wi-Fi data rate or packet-error probability.

## 13. EESM compression

The MMSE stage produces one SINR per stream and subcarrier, but ns-3's Wi-Fi error models require a scalar SINR. The MCS-specific exponential effective-SINR mapping is

$$
\gamma_{\mathrm{eff}}
=
-\beta_m\ln\left(
\frac{1}{N_{\mathrm{sc}}N_s}
\sum_{k,s}\exp\left(-\frac{\gamma_s[k]}{\beta_m}\right)
\right),
$$

where $\beta_m>0$ is configured for HE MCS $m$.

The numerically stable implementation in [`model/he-su-mimo-link-abstraction.cc`](../model/he-su-mimo-link-abstraction.cc#L125) first finds the minimum SINR:

```cpp
const double beta = GetEesmBeta(mcs);
double minimumSinr = std::numeric_limits<double>::infinity();

for (uint32_t sc = 0; sc < mimoResult.GetNumSubcarriers(); ++sc)
{
    for (uint32_t stream = 0; stream < mimoResult.GetNumStreams(); ++stream)
    {
        minimumSinr =
            std::min(minimumSinr, mimoResult.GetSinrLinear(sc, stream));
    }
}
```

It then evaluates:

```cpp
double exponentialSum = 0.0;

for (uint32_t sc = 0; sc < mimoResult.GetNumSubcarriers(); ++sc)
{
    for (uint32_t stream = 0; stream < mimoResult.GetNumStreams(); ++stream)
    {
        const double sinr = mimoResult.GetSinrLinear(sc, stream);
        exponentialSum += std::exp(-(sinr - minimumSinr) / beta);
    }
}

const double numSamples =
    static_cast<double>(mimoResult.GetNumSubcarriers()) *
    mimoResult.GetNumStreams();

const double effectiveSinr =
    minimumSinr - beta * std::log(exponentialSum / numSamples);
```

Low-SINR samples therefore have a stronger effect than they would under an arithmetic mean. This implementation deliberately supplies no production default beta values. A suitable beta must be configured for every HE MCS used by the simulation.

## 14. Mapping effective SINR to packet-error behavior

The effective SINR, Wi-Fi mode, `WifiTxVector`, packet length, and RX-antenna count are passed to ns-3's configured `ErrorRateModel`.

Relevant code from [`model/he-su-mimo-link-abstraction.cc`](../model/he-su-mimo-link-abstraction.cc#L171):

```cpp
result.m_effectiveSinrLinear =
    CalculateEffectiveSinr(result.m_mimoResult, mode.GetMcsValue());

result.m_packetSuccessRate =
    m_errorRateModel->GetChunkSuccessRate(
        mode,
        txVector,
        result.m_effectiveSinrLinear,
        static_cast<uint64_t>(packetSizeBytes) * 8,
        static_cast<uint8_t>(channel.GetNumRxAntennas()));
```

The reported packet-error rate is

$$
PER=1-P_{\mathrm{success}}.
$$

This direct result is made available through the `MimoRx` trace.

## 15. Reinserting the MIMO result into SpectrumWifiPhy

`SpectrumWifiPhy` expects a scalar PSD, not a matrix-valued channel. ns3sionna therefore constructs an equivalent scalar received power:

$$
P_{\mathrm{eq}}
=
\frac{\gamma_{\mathrm{eff}}N}{G_{\mathrm{diversity}}}.
$$

The diversity correction prevents ns-3's AWGN error model from adding an RX-array gain that MMSE already included. The code in [`model/sionna-spectrum-propagation-loss-model.cc`](../model/sionna-spectrum-propagation-loss-model.cc#L303) is:

```cpp
const double diversityGain =
    errorRateModel->IsAwgn() && rxPhyAntennas > nss
        ? static_cast<double>(rxPhyAntennas) / nss
        : 1.0;

result.m_equivalentReceivedPowerW =
    result.m_linkResult.GetEffectiveSinrLinear() *
    linkBudget.GetReceiverNoisePowerW() /
    diversityGain;
```

The complete received PSD is rescaled to this equivalent power:

```cpp
const double equivalentPowerBeforeRxGain =
    mimoRx.GetEquivalentReceivedPowerW() / rxGainLinear;

const double currentPowerBeforeRxGain = Integral(*rxPsd);

*rxPsd *= equivalentPowerBeforeRxGain / currentPowerBeforeRxGain;
```

Native ns-3 processing then continues:

1. `SpectrumWifiPhy` receives the packet.
2. `InterferenceHelper` adds thermal noise and concurrent scalar interference.
3. The normal ns-3 Wi-Fi reception procedure determines packet success.

Without additional interference, the PHY sees the EESM effective SINR. With interference, native ns-3 processing reduces that SINR through the ordinary scalar interference model. Interference is not inserted into a frequency-dependent MIMO covariance matrix; it is applied after MIMO and EESM compression.

## 16. CFR packet tag

The complex CFR and wideband path loss are also attached to the packet as a `CFRTag`:

```cpp
CFRTag tag;
tag.SetComplexes(tagCoefficients);
tag.SetPathloss(wbLoss);
p->AddPacketTag(tag);
```

See [`model/sionna-spectrum-propagation-loss-model.cc`](../model/sionna-spectrum-propagation-loss-model.cc#L441). This makes the channel available to sensing or application code after reception. The tag stores the flat complex vector and path loss but does not store the MIMO dimensions, so consuming application code must know the configured dimensions.

## 17. Complete mathematical summary

The complete abstraction can be summarized as

$$
\boxed{
H_{\mathrm{raw}}
\longrightarrow
(L_{\mathrm{wb}},H_{\mathrm{norm}}[k])
\longrightarrow
H_{\mathrm{eff}}[k]
\longrightarrow
\gamma_s[k]
\longrightarrow
\gamma_{\mathrm{EESM}}
\longrightarrow
PER
\longrightarrow
\text{equivalent ns-3 PSD}
}.
$$

The most important boundary is that ns-3 does not receive or demodulate complex I/Q samples. It uses the complex channel matrix mathematically to estimate the quality that a packet would have and maps that result back into the standard packet-level ns-3 PHY.
