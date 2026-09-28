/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/core-module.h"
#include "ns3/sionna-helper.h"
#include "ns3/sionna-mimo-helper.h"
#include "ns3/sionna-propagation-cache.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/spectrum-channel.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/su-mimo-link-model.h"
#include "ns3/wifi-link-budget.h"

#include <Eigen/SVD>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ns3;

namespace
{

constexpr std::array<double, 3> TX_POSITION{3.0, 2.0, 2.0};
constexpr std::array<double, 3> RX_POSITION{4.0, 3.0, 1.0};
constexpr double TX_POWER_DBM = 20.0; // Total power, shared equally by both streams.
constexpr double BANDWIDTH_HZ = 20e6;
constexpr double TEMPERATURE_K = 290.0;
constexpr double NOISE_FIGURE_DB = 7.0;

void
Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::invalid_argument(message);
    }
}

template <typename Position>
bool
MatchesPosition(const Position& position, const std::array<double, 3>& expected)
{
    return std::isfinite(position.x()) && std::isfinite(position.y()) &&
           std::isfinite(position.z()) && std::abs(position.x() - expected[0]) <= 1e-12 &&
           std::abs(position.y() - expected[1]) <= 1e-12 &&
           std::abs(position.z() - expected[2]) <= 1e-12;
}

void
WriteConfiguration(const std::string& output,
                   double spacingLambda,
                   const SionnaMimoOrientation& txOrientation,
                   const SionnaMimoOrientation& rxOrientation)
{
    SionnaMimoConfiguration configuration;
    configuration.mode = SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
    configuration.txArray.numColumns = 2;
    configuration.rxArray.numColumns = 2;
    configuration.txArray.horizontalSpacing = spacingLambda;
    configuration.rxArray.horizontalSpacing = spacingLambda;
    configuration.numTxAntennas = 2;
    configuration.numRxAntennas = 2;
    configuration.maxNss = 2;
    configuration.receiverNoiseFigureDb = NOISE_FIGURE_DB;
    configuration.nodeOrientations.emplace(0, txOrientation);
    configuration.nodeOrientations.emplace(1, rxOrientation);

    SionnaHelper sionna("simple_room/simple_room.xml", "inproc://sumimo-benchmark");
    sionna.Configure(5180, 20, 256, 78125, 5);
    sionna.SetMode(SionnaHelper::MODE_P2P);
    sionna.SetSubMode(SionnaHelper::SUB_MODE_IGNORED);
    SpectrumWifiPhyHelper txPhy;
    SpectrumWifiPhyHelper rxPhy;
    // Apply exercises the existing high-level array/orientation contract without
    // installing a network. No HE PER is evaluated, so no EESM calibration is needed.
    SionnaMimoHelper(configuration)
        .Apply(sionna, txPhy, rxPhy, CreateObject<SionnaSpectrumPropagationLossModel>());

    ns3sionna::SimInitMessage message;
    message.set_scene_fname("simple_room/simple_room.xml");
    message.set_seed(RngSeedManager::GetSeed());
    message.set_frequency(5180);
    message.set_channel_bw(20);
    message.set_fft_size(256);
    message.set_subcarrier_spacing(78125);
    message.set_mode(SionnaHelper::MODE_P2P);
    message.set_sub_mode(SionnaHelper::SUB_MODE_IGNORED);
    message.set_fixed_coherence_time_ms(5);
    message.set_time_evo_model("position");
    for (uint32_t id = 0; id < 2; ++id)
    {
        auto* node = message.add_nodes();
        node->set_id(id);
        auto* position = node->mutable_constant_position_model()->mutable_position();
        const auto& coordinates = id == 0 ? TX_POSITION : RX_POSITION;
        position->set_x(coordinates[0]);
        position->set_y(coordinates[1]);
        position->set_z(coordinates[2]);
    }
    sionna.PopulateMimoConfiguration(message);

    // The offline Python runner consumes raw messages; Wrapper is only for ZMQ IPC.
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    Require(stream.is_open(), "Cannot open configuration output: " + output);
    Require(message.SerializeToOstream(&stream), "Cannot serialize SimInitMessage");
    stream.close();
    Require(!stream.fail(), "Cannot write configuration output: " + output);
}

void
Analyze(const std::string& input, const std::string& output)
{
    std::ifstream stream(input, std::ios::binary);
    Require(stream.is_open(), "Cannot open channel input: " + input);
    const auto start = std::chrono::steady_clock::now();
    ns3sionna::ChannelStateResponse response;
    Require(response.ParseFromIstream(&stream) && !stream.bad(),
            "Cannot deserialize raw ChannelStateResponse");
    Require(response.csi_size() == 1, "Expected exactly one channel snapshot");
    const auto& snapshot = response.csi(0);
    Require(snapshot.has_tx_node() && snapshot.tx_node().id() == 0 &&
                snapshot.tx_node().has_position() &&
                MatchesPosition(snapshot.tx_node().position(), TX_POSITION),
            "Expected TX node 0 at (3, 2, 2)");
    Require(snapshot.rx_nodes_size() == 1, "Expected exactly one receiver");
    const auto& receiver = snapshot.rx_nodes(0);
    Require(receiver.id() == 1 && receiver.has_position() &&
                MatchesPosition(receiver.position(), RX_POSITION),
            "Expected RX node 1 at (4, 3, 1)");
    Require(receiver.num_tx_antennas() == 2 && receiver.num_rx_antennas() == 2,
            "Expected explicit 2x2 antenna dimensions");
    const int numSubcarriers = receiver.frequencies_size();
    Require(numSubcarriers > 0 &&
                static_cast<int64_t>(receiver.csi_real_size()) == 4LL * numSubcarriers &&
                receiver.csi_imag_size() == receiver.csi_real_size(),
            "Expected a nonempty CFR with four complex coefficients per subcarrier");
    Require(std::isfinite(receiver.wb_loss()), "Wideband loss must be finite");
    const double receivedPowerW = std::pow(10.0, (TX_POWER_DBM - receiver.wb_loss()) / 10.0) /
                                  1000.0;
    const double noisePowerW = 1.3803e-23 * TEMPERATURE_K * BANDWIDTH_HZ *
                              std::pow(10.0, NOISE_FIGURE_DB / 10.0);
    const double referenceSnr = receivedPowerW / noisePowerW;
    Require(std::isfinite(receivedPowerW) && receivedPowerW > 0.0 &&
                std::isfinite(referenceSnr) && referenceSnr > 0.0,
            "Wideband loss is outside the supported power range");
    for (int i = 0; i < receiver.csi_real_size(); ++i)
    {
        Require(std::isfinite(receiver.csi_real(i)) && std::isfinite(receiver.csi_imag(i)),
                "CFR coefficients must be finite");
        const long double real = receiver.csi_real(i);
        const long double imag = receiver.csi_imag(i);
        const long double power = real * real + imag * imag;
        Require(power <= std::numeric_limits<double>::max() / 4.0 &&
                    power * (referenceSnr / 2.0) <= std::numeric_limits<double>::max() / 4.0,
                "CFR coefficients exceed the supported MMSE range");
    }

    const auto channel = SionnaPropagationCache::DeserializeChannelMatrix(receiver);
    // L_wb supplies absolute received power; normalized H[k] retains the spatial
    // and frequency variation. The MMSE model splits reference SNR over two streams.
    const auto budget = WifiLinkBudget::Calculate(TX_POWER_DBM,
                                                  receiver.wb_loss(),
                                                  BANDWIDTH_HZ,
                                                  TEMPERATURE_K,
                                                  NOISE_FIGURE_DB);
    const auto link = SuMimoLinkModel::Calculate(channel, budget.GetReferenceSnrLinear());
    struct Diagnostic
    {
        uint32_t rank;
        double sigma0;
        double sigma1;
    };
    std::vector<Diagnostic> diagnostics;
    diagnostics.reserve(numSubcarriers);
    for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
    {
        Eigen::Matrix2cd h;
        for (uint32_t rx = 0; rx < 2; ++rx)
        {
            for (uint32_t tx = 0; tx < 2; ++tx)
            {
                h(rx, tx) = channel.Get(sc, rx, tx);
            }
        }
        const Eigen::JacobiSVD<Eigen::Matrix2cd> svd(h);
        Require(svd.info() == Eigen::Success, "Cannot calculate channel singular values");
        const double sigma0 = svd.singularValues()(0);
        const double sigma1 = svd.singularValues()(1);
        Require(std::isfinite(sigma0) && std::isfinite(sigma1),
                "Channel singular values must be finite");
        // Relative SVD rank is diagnostic only: rank-one and zero channels remain valid.
        const double threshold = 1e-12 * sigma0;
        const uint32_t rank = (sigma0 > threshold ? 1 : 0) + (sigma1 > threshold ? 1 : 0);
        diagnostics.push_back({rank, sigma0, sigma1});
        Require(std::isfinite(link.GetSinrLinear(sc, 0)) &&
                    std::isfinite(link.GetSinrLinear(sc, 1)),
                "Post-MMSE SINRs must be finite");
    }
    Require(std::isfinite(link.GetEffectiveSinrLinear(0)) &&
                std::isfinite(link.GetEffectiveSinrLinear(1)) &&
                std::isfinite(link.GetSpectralEfficiency()),
            "Wideband link metrics must be finite");
    const double analysisSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::ofstream csv(output, std::ios::trunc);
    Require(csv.is_open(), "Cannot open analysis output: " + output);
    csv << "subcarrier,frequency_offset_hz,rank,sigma0,sigma1,sinr0_linear,sinr1_linear,"
           "effective_sinr0_linear,effective_sinr1_linear,spectral_efficiency_bps_hz,"
           "wideband_loss_db,reference_snr_linear,received_power_w,noise_power_w,analysis_seconds\n"
        << std::setprecision(17);
    for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
    {
        const auto& diagnostic = diagnostics[sc];
        // Effective SINR preserves mean log(1+SINR) per stream. Spectral efficiency
        // is the wideband mean sum of log2(1+SINR), not an HE payload throughput.
        csv << sc << ',' << receiver.frequencies(sc) << ',' << diagnostic.rank << ','
            << diagnostic.sigma0 << ',' << diagnostic.sigma1 << ','
            << link.GetSinrLinear(sc, 0) << ',' << link.GetSinrLinear(sc, 1) << ','
            << link.GetEffectiveSinrLinear(0) << ',' << link.GetEffectiveSinrLinear(1) << ','
            << link.GetSpectralEfficiency() << ',' << receiver.wb_loss() << ','
            << budget.GetReferenceSnrLinear() << ',' << budget.GetReceivedPowerW() << ','
            << budget.GetReceiverNoisePowerW() << ',' << analysisSeconds << '\n';
    }
    csv.close();
    Require(!csv.fail(), "Cannot write analysis output: " + output);
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string action{"config"};
    std::string input;
    std::string output;
    double spacingLambda = 0.5;
    SionnaMimoOrientation txOrientation;
    SionnaMimoOrientation rxOrientation;
    CommandLine cmd(__FILE__);
    cmd.AddValue("action", "config or analyze", action);
    cmd.AddValue("input", "Raw ChannelStateResponse protobuf file for analyze", input);
    cmd.AddValue("output", "Raw SimInitMessage file (config) or diagnostic CSV (analyze)", output);
    cmd.AddValue("spacingLambda", "TX/RX horizontal antenna spacing in wavelengths", spacingLambda);
    cmd.AddValue("txAzimuth", "TX azimuth in degrees", txOrientation.azimuthDegrees);
    cmd.AddValue("txElevation", "TX elevation in degrees", txOrientation.elevationDegrees);
    cmd.AddValue("txRoll", "TX roll in degrees", txOrientation.rollDegrees);
    cmd.AddValue("rxAzimuth", "RX azimuth in degrees", rxOrientation.azimuthDegrees);
    cmd.AddValue("rxElevation", "RX elevation in degrees", rxOrientation.elevationDegrees);
    cmd.AddValue("rxRoll", "RX roll in degrees", rxOrientation.rollDegrees);
    cmd.Parse(argc, argv);

    try
    {
        RngSeedManager::SetSeed(42);
        RngSeedManager::SetRun(1);
        Require(!output.empty(), "Provide --output=<path>");
        if (action == "config")
        {
            WriteConfiguration(output, spacingLambda, txOrientation, rxOrientation);
        }
        else if (action == "analyze")
        {
            Require(!input.empty(), "Provide --input=<path> for analyze");
            Require(input != output, "Input and output paths must differ");
            Analyze(input, output);
        }
        else
        {
            throw std::invalid_argument("Unknown --action=" + action);
        }
    }
    catch (const std::exception& exception)
    {
        std::cerr << "benchmark_error=" << exception.what() << std::endl;
        return 2;
    }
    return 0;
}
