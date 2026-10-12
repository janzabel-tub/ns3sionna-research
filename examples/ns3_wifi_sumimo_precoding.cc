/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/applications-module.h"
#include "ns3/arp-cache.h"
#include "ns3/core-module.h"
#include "ns3/he-phy.h"
#include "ns3/ipv4-interface.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/internet-module.h"
#include "ns3/mimo-channel-matrix.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/propagation-module.h"
#include "ns3/sionna-helper.h"
#include "ns3/sionna-mimo-helper.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/spectrum-module.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/wifi-link-budget.h"
#include "ns3/wifi-module.h"
#include "ns3/wifi-tx-vector.h"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ns3;

namespace
{

constexpr uint32_t NUM_SUBCARRIERS = 768; // 20 MHz plus SpectrumWifiPhy guard bands
constexpr double BANDWIDTH_HZ = 20e6;
constexpr double NOISE_FIGURE_DB = 7.0;
constexpr double TEMPERATURE_K = 290.0;
constexpr double PI = 3.141592653589793238462643383279502884;

struct ScenarioResult
{
    std::string name;
    std::string mode;
    HeSuMimoLinkResult link;
    double throughputMbps{0.0};
    double expectedThroughputMbps{0.0};
    double precodingNorm{0.0};
    std::vector<double> powerFractions;
    int64_t csiAgeNs{-1};
    uint64_t feedbackBits{0};
    double chordalError{0.0};
    double rmsCoefficientError{0.0};
};

WifiTxVector
MakeTxVector(WifiMode mode, uint8_t nss)
{
    return WifiTxVector(mode,
                        0,
                        WIFI_PREAMBLE_HE_SU,
                        800,
                        nss,
                        nss,
                        0,
                        20,
                        false);
}

MimoChannelMatrix
MakeTwoByTwoChannel(double rightRotationDegrees)
{
    const double angle = rightRotationDegrees * PI / 180.0;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const double strong = std::sqrt(3.6);
    const double weak = std::sqrt(0.4);

    MimoChannelMatrix channel(NUM_SUBCARRIERS, 2, 2);
    for (uint32_t sc = 0; sc < NUM_SUBCARRIERS; ++sc)
    {
        channel.Set(sc, 0, 0, {strong * cosine, 0.0});
        channel.Set(sc, 0, 1, {strong * sine, 0.0});
        channel.Set(sc, 1, 0, {-weak * sine, 0.0});
        channel.Set(sc, 1, 1, {weak * cosine, 0.0});
    }
    return channel;
}

MimoChannelMatrix
MakeMisoChannel()
{
    MimoChannelMatrix channel(NUM_SUBCARRIERS, 1, 2);
    for (uint32_t sc = 0; sc < NUM_SUBCARRIERS; ++sc)
    {
        channel.Set(sc, 0, 0, {1.0, 0.0});
        channel.Set(sc, 0, 1, {1.0, 0.0});
    }
    return channel;
}

SionnaMimoConfiguration
MakeTwoByTwoConfiguration(uint8_t mcs, double eesmBetaLinear)
{
    SionnaMimoConfiguration configuration;
    configuration.effectiveSinrModel = SionnaMimoEffectiveSinrModel::EESM;
    configuration.mode = SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
    configuration.txArray.numColumns = 2;
    configuration.rxArray.numColumns = 2;
    configuration.numTxAntennas = 2;
    configuration.numRxAntennas = 2;
    configuration.maxNss = 2;
    configuration.receiverNoiseFigureDb = NOISE_FIGURE_DB;
    configuration.eesmBetaLinear.emplace(mcs, eesmBetaLinear);
    return configuration;
}

MimoFeedbackCodebook
ParseQuantization(const std::string& quantization)
{
    if (quantization == "none")
    {
        return MimoFeedbackCodebook::UNQUANTIZED;
    }
    if (quantization == "coarse")
    {
        return MimoFeedbackCodebook::SU_COARSE;
    }
    if (quantization == "fine")
    {
        return MimoFeedbackCodebook::SU_FINE;
    }
    throw std::invalid_argument(
        "quantization must be one of none, coarse, or fine");
}

double
RunNetworkScenario(const std::string& name,
                   const MimoChannelMatrix& transmissionChannel,
                   const MimoChannelMatrix* csiChannel,
                   Time csiTimestamp,
                   Time transmissionTimestamp,
                   SionnaMimoHelper& mimo,
                   uint8_t mcs,
                   uint32_t packetSizeBytes,
                   double receivedPowerDbm)
{
    NodeContainer nodes;
    nodes.Create(2);
    NodeContainer txNode(nodes.Get(0));
    NodeContainer rxNode(nodes.Get(1));

    if (mimo.GetConfiguration().mode == SionnaMimoMode::SU_BEAMFORMING)
    {
        if (!csiChannel)
        {
            throw std::invalid_argument("beamforming scenario has no CSI channel");
        }
        mimo.SelectPrecoding(*csiChannel,
                             nodes.Get(0)->GetId(),
                             nodes.Get(1)->GetId(),
                             csiTimestamp,
                             transmissionTimestamp);
    }

    auto spectrumChannel = CreateObject<MultiModelSpectrumChannel>();
    auto fixedRss = CreateObject<FixedRssLossModel>();
    fixedRss->SetRss(receivedPowerDbm);
    spectrumChannel->AddPropagationLossModel(fixedRss);

    SionnaHelper sionnaHelper("", "inproc://ns3sionna-phase35-" + name);
    SpectrumWifiPhyHelper txPhy;
    SpectrumWifiPhyHelper rxPhy;
    auto mimoLoss = CreateObject<SionnaSpectrumPropagationLossModel>();
    mimo.ApplyForHeMcs(mcs, sionnaHelper, txPhy, rxPhy, mimoLoss);
    mimoLoss->SetFixedChannelMatrix(transmissionChannel, -receivedPowerDbm);
    spectrumChannel->AddSpectrumPropagationLossModel(mimoLoss);
    spectrumChannel->SetPropagationDelayModel(
        CreateObject<ConstantSpeedPropagationDelayModel>());

    auto configurePhy = [&spectrumChannel](SpectrumWifiPhyHelper& phy) {
        phy.SetChannel(spectrumChannel);
        phy.SetErrorRateModel("ns3::TableBasedErrorRateModel");
        phy.Set("ChannelSettings", StringValue("{36, 20, BAND_5GHZ, 0}"));
        phy.Set("TxPowerStart", DoubleValue(0.0));
        phy.Set("TxPowerEnd", DoubleValue(0.0));
    };
    configurePhy(txPhy);
    configurePhy(rxPhy);

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211ax);
    wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifi.SetRemoteStationManager(
        "ns3::ConstantRateWifiManager",
        "DataMode",
        StringValue("HeMcs" + std::to_string(mcs)),
        "ControlMode",
        StringValue("OfdmRate24Mbps"));

    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");
    NetDeviceContainer txDevices = wifi.Install(txPhy, mac, txNode);
    NetDeviceContainer rxDevices = wifi.Install(rxPhy, mac, rxNode);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);
    nodes.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, 0.0));
    nodes.Get(1)->GetObject<MobilityModel>()->SetPosition(Vector(1.0, 0.0, 0.0));

    InternetStackHelper internet;
    internet.Install(nodes);
    Ipv4AddressHelper addresses;
    addresses.SetBase("10.1.0.0", "255.255.255.0");
    addresses.Assign(txDevices);
    const Ipv4InterfaceContainer rxInterfaces = addresses.Assign(rxDevices);

    auto txIpv4 = txNode.Get(0)->GetObject<Ipv4L3Protocol>();
    const int32_t txInterface = txIpv4->GetInterfaceForDevice(txDevices.Get(0));
    if (txInterface < 0)
    {
        throw std::runtime_error("TX Wi-Fi device has no IPv4 interface");
    }
    auto arpCache = txIpv4->GetInterface(txInterface)->GetArpCache();
    auto arpEntry = arpCache->Add(rxInterfaces.GetAddress(0));
    arpEntry->SetMacAddress(rxDevices.Get(0)->GetAddress());
    arpEntry->MarkPermanent();

    constexpr uint16_t port = 9000;
    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApps = sinkHelper.Install(rxNode.Get(0));
    sinkApps.Start(Seconds(0.5));
    sinkApps.Stop(Seconds(2.1));

    OnOffHelper source("ns3::UdpSocketFactory",
                       InetSocketAddress(rxInterfaces.GetAddress(0), port));
    source.SetAttribute("DataRate", DataRateValue(DataRate("1Gbps")));
    source.SetAttribute("PacketSize", UintegerValue(packetSizeBytes));
    source.SetAttribute("OnTime",
                        StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    source.SetAttribute("OffTime",
                        StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer sourceApps = source.Install(txNode.Get(0));
    sourceApps.Start(Seconds(1.0));
    sourceApps.Stop(Seconds(2.0));

    Simulator::Stop(Seconds(2.1));
    Simulator::Run();
    const uint64_t receivedBytes =
        DynamicCast<PacketSink>(sinkApps.Get(0))->GetTotalRx();
    Simulator::Destroy();
    return receivedBytes * 8.0 / 1e6;
}

ScenarioResult
EvaluateScenario(const std::string& name,
                 const std::string& modeName,
                 const MimoChannelMatrix& channel,
                 SionnaMimoHelper& mimo,
                 uint8_t mcs,
                 uint32_t packetSizeBytes,
                 double receivedPowerW,
                 double throughputMbps)
{
    SionnaHelper diagnosticSionna("", "inproc://ns3sionna-phase35-diagnostic-" + name);
    SpectrumWifiPhyHelper txPhy;
    SpectrumWifiPhyHelper rxPhy;
    auto receiver = CreateObject<SionnaSpectrumPropagationLossModel>();
    mimo.ApplyForHeMcs(mcs, diagnosticSionna, txPhy, rxPhy, receiver);

    const uint8_t nss = static_cast<uint8_t>(mimo.GetConfiguration().maxNss);
    const WifiMode wifiMode = HePhy::GetHeMcs(mcs);
    const WifiTxVector txVector = MakeTxVector(wifiMode, nss);
    const auto spectrumResult = receiver->CalculateMimoRx(
        channel,
        receivedPowerW,
        txVector,
        packetSizeBytes,
        static_cast<uint8_t>(channel.GetNumTxAntennas()),
        static_cast<uint8_t>(channel.GetNumRxAntennas()),
        BANDWIDTH_HZ,
        NOISE_FIGURE_DB,
        CreateObject<TableBasedErrorRateModel>());

    ScenarioResult result;
    result.name = name;
    result.mode = modeName;
    result.link = spectrumResult.GetLinkResult();
    result.throughputMbps = throughputMbps;
    result.expectedThroughputMbps =
        wifiMode.GetDataRate(txVector) * result.link.GetPacketSuccessRate() / 1e6;

    const MimoPrecodingMatrix* precoding = nullptr;
    if (mimo.HasSelectedPrecoding())
    {
        const auto& selection = mimo.GetSelectedPrecoding();
        precoding = &selection.GetPrecodingMatrix();
        result.csiAgeNs = selection.GetCsiAge().GetNanoSeconds();
        if (selection.HasCsiSelectionResult())
        {
            const auto& diagnostics =
                selection.GetCsiSelectionResult().GetDiagnostics();
            result.feedbackBits = diagnostics.GetFeedbackBitCount();
            result.chordalError = diagnostics.GetMeanChordalError();
            result.rmsCoefficientError = diagnostics.GetMeanRmsCoefficientError();
        }
    }
    else if (mimo.HasPrecodingMatrix())
    {
        precoding = &mimo.GetPrecodingMatrix();
    }

    if (precoding)
    {
        result.precodingNorm = std::sqrt(precoding->GetPower(0));
        for (uint32_t stream = 0; stream < precoding->GetNumStreams(); ++stream)
        {
            double fraction = 0.0;
            for (uint32_t tx = 0; tx < precoding->GetNumTxAntennas(); ++tx)
            {
                fraction += std::norm(precoding->Get(0, tx, stream));
            }
            result.powerFractions.push_back(fraction / precoding->GetNumStreams());
        }
    }
    else
    {
        result.precodingNorm = std::sqrt(static_cast<double>(nss));
        result.powerFractions.assign(nss, 1.0 / nss);
    }
    return result;
}

void
PrintResult(const ScenarioResult& result)
{
    const auto& mimo = result.link.GetMimoResult();
    std::cout << "scenario=" << result.name
              << " mode=" << result.mode
              << " throughput_mbps=" << result.throughputMbps
              << " expected_throughput_mbps=" << result.expectedThroughputMbps
              << " per=" << result.link.GetPacketErrorRate()
              << " channel_rank=" << mimo.GetMinimumChannelRank()
              << " precoding_norm=" << result.precodingNorm
              << " power_fractions=";
    for (std::size_t i = 0; i < result.powerFractions.size(); ++i)
    {
        std::cout << (i == 0 ? "" : ",") << result.powerFractions.at(i);
    }
    std::cout << " csi_age_ns=" << result.csiAgeNs
              << " feedback_bits=" << result.feedbackBits
              << " chordal_error=" << result.chordalError
              << " rms_coefficient_error=" << result.rmsCoefficientError
              << " stream_sinr_db=";
    for (uint32_t stream = 0; stream < mimo.GetNumStreams(); ++stream)
    {
        std::cout << (stream == 0 ? "" : ",")
                  << WifiLinkBudget::LinearToDb(mimo.GetEffectiveSinrLinear(stream));
    }
    std::cout << " effective_sinr_db="
              << WifiLinkBudget::LinearToDb(result.link.GetEffectiveSinrLinear())
              << std::endl;
}

int
RunFailureCase(const std::string& failureCase,
               uint8_t mcs,
               double eesmBetaLinear,
               double referenceSnrLinear)
{
    try
    {
        auto configuration = MakeTwoByTwoConfiguration(mcs, eesmBetaLinear);
        configuration.mode = SionnaMimoMode::SU_BEAMFORMING;
        configuration.maxNss = 1;
        configuration.beamforming = SionnaMimoBeamformingConfiguration{};

        if (failureCase == "unselected-beamforming")
        {
            SionnaMimoHelper helper(configuration);
            SionnaHelper sionna("", "inproc://ns3sionna-phase35-unselected");
            SpectrumWifiPhyHelper txPhy;
            SpectrumWifiPhyHelper rxPhy;
            helper.ApplyForHeMcs(
                mcs,
                sionna,
                txPhy,
                rxPhy,
                CreateObject<SionnaSpectrumPropagationLossModel>());
        }
        else if (failureCase == "water-filling-quantized")
        {
            configuration.beamforming->svd.powerAllocation =
                MimoPowerAllocationMode::WATER_FILLING;
            configuration.beamforming->svd.referenceSnrLinear = referenceSnrLinear;
            configuration.beamforming->feedback.codebook =
                MimoFeedbackCodebook::SU_COARSE;
            SionnaMimoHelper helper(configuration);
            static_cast<void>(helper);
        }
        else if (failureCase == "mrt-aging")
        {
            configuration.rxArray.numColumns = 1;
            configuration.numRxAntennas = 1;
            configuration.beamforming->method = SionnaMimoBeamformingMethod::MRT;
            configuration.beamforming->csiTiming.feedbackDelay = MilliSeconds(1);
            SionnaMimoHelper helper(configuration);
            static_cast<void>(helper);
        }
        else if (failureCase == "stale-timestamp")
        {
            configuration.beamforming->csiTiming.updateInterval = MilliSeconds(5);
            configuration.beamforming->csiTiming.feedbackDelay = MilliSeconds(2);
            SionnaMimoHelper helper(configuration);
            const auto channel = MakeTwoByTwoChannel(45.0);
            helper.SelectPrecoding(channel,
                                   0,
                                   1,
                                   MilliSeconds(9),
                                   MilliSeconds(13));
        }
        else
        {
            throw std::invalid_argument("unknown failureCase");
        }
    }
    catch (const std::invalid_argument& exception)
    {
        std::cerr << "configuration_error=" << exception.what() << std::endl;
        return 2;
    }

    std::cerr << "The requested invalid configuration was not rejected" << std::endl;
    return 1;
}

} // namespace

int
main(int argc, char* argv[])
{
    uint8_t mcs = 5;
    uint32_t packetSizeBytes = 1200;
    double referenceSnrDb = 16.0;
    double eesmBetaLinear = std::numeric_limits<double>::quiet_NaN();
    uint32_t svdStreams = 1;
    bool waterFilling = false;
    std::string quantization{"none"};
    uint64_t csiUpdateIntervalMs = 0;
    uint64_t feedbackDelayMs = 0;
    uint64_t transmissionTimeMs = 20;
    double agingRotationDegrees = 0.0;
    std::string failureCase{"none"};

    CommandLine cmd(__FILE__);
    cmd.AddValue("mcs", "HE MCS (0 through 11)", mcs);
    cmd.AddValue("packetSize", "PSDU size in bytes", packetSizeBytes);
    cmd.AddValue("referenceSnrDb", "Total pre-CFR reference SNR in dB", referenceSnrDb);
    cmd.AddValue("eesmBetaLinear",
                 "Required EESM beta in linear SINR units; no default is supplied",
                 eesmBetaLinear);
    cmd.AddValue("svdStreams", "Number of SVD streams (1 or 2)", svdStreams);
    cmd.AddValue("waterFilling", "Enable spatial SVD water-filling", waterFilling);
    cmd.AddValue("quantization", "SVD feedback: none, coarse, or fine", quantization);
    cmd.AddValue("csiUpdateIntervalMs",
                 "Periodic CSI update interval; zero is continuous",
                 csiUpdateIntervalMs);
    cmd.AddValue("feedbackDelayMs", "CSI feedback delay", feedbackDelayMs);
    cmd.AddValue("transmissionTimeMs", "Timestamp of the modeled transmission", transmissionTimeMs);
    cmd.AddValue("agingRotationDegrees",
                 "Deterministic right-subspace rotation from CSI time to TX time",
                 agingRotationDegrees);
    cmd.AddValue("failureCase",
                 "Negative case: none, unselected-beamforming, water-filling-quantized, "
                 "mrt-aging, or stale-timestamp",
                 failureCase);
    cmd.Parse(argc, argv);

    if (mcs > 11 || packetSizeBytes == 0 || !std::isfinite(referenceSnrDb) ||
        !std::isfinite(eesmBetaLinear) || eesmBetaLinear <= 0.0 ||
        svdStreams < 1 || svdStreams > 2 || !std::isfinite(agingRotationDegrees))
    {
        std::cerr << "Provide valid MCS/SNR/packet/stream values and "
                     "--eesmBetaLinear=<positive value>; no production beta default exists"
                  << std::endl;
        return 2;
    }

    const double referenceSnrLinear = WifiLinkBudget::DbToLinear(referenceSnrDb);
    if (failureCase != "none")
    {
        return RunFailureCase(failureCase,
                              mcs,
                              eesmBetaLinear,
                              referenceSnrLinear);
    }

    try
    {
        const MimoFeedbackCodebook feedbackCodebook =
            ParseQuantization(quantization);
        const auto noiseReference =
            WifiLinkBudget::Calculate(0.0,
                                      0.0,
                                      BANDWIDTH_HZ,
                                      TEMPERATURE_K,
                                      NOISE_FIGURE_DB);
        const double receivedPowerW =
            referenceSnrLinear * noiseReference.GetReceiverNoisePowerW();
        const double receivedPowerDbm = WToDbm(receivedPowerW);

        const MimoChannelMatrix csiChannel = MakeTwoByTwoChannel(45.0);
        const MimoChannelMatrix transmissionChannel =
            MakeTwoByTwoChannel(45.0 + agingRotationDegrees);
        const MimoChannelMatrix misoChannel = MakeMisoChannel();

        auto openConfiguration = MakeTwoByTwoConfiguration(mcs, eesmBetaLinear);
        SionnaMimoHelper openHelper(openConfiguration);

        auto fixedConfiguration = openConfiguration;
        fixedConfiguration.rxArray.numColumns = 1;
        fixedConfiguration.numRxAntennas = 1;
        fixedConfiguration.maxNss = 1;
        fixedConfiguration.precodingMatrix = MimoPrecodingMatrix::FrequencyFlat(
            NUM_SUBCARRIERS,
            2,
            1,
            {{1.0, 0.0}, {0.0, 0.0}});
        SionnaMimoHelper fixedHelper(fixedConfiguration);

        SionnaMimoConfiguration mrtConfiguration;
        mrtConfiguration.effectiveSinrModel = SionnaMimoEffectiveSinrModel::EESM;
        mrtConfiguration.mode = SionnaMimoMode::SU_BEAMFORMING;
        mrtConfiguration.txArray.numColumns = 2;
        mrtConfiguration.numTxAntennas = 2;
        mrtConfiguration.receiverNoiseFigureDb = NOISE_FIGURE_DB;
        mrtConfiguration.eesmBetaLinear.emplace(mcs, eesmBetaLinear);
        mrtConfiguration.beamforming = SionnaMimoBeamformingConfiguration{};
        mrtConfiguration.beamforming->method = SionnaMimoBeamformingMethod::MRT;
        SionnaMimoHelper mrtHelper(mrtConfiguration);

        auto svdConfiguration = openConfiguration;
        svdConfiguration.mode = SionnaMimoMode::SU_BEAMFORMING;
        svdConfiguration.maxNss = svdStreams;
        svdConfiguration.beamforming = SionnaMimoBeamformingConfiguration{};
        svdConfiguration.beamforming->svd.referenceSnrLinear = referenceSnrLinear;
        svdConfiguration.beamforming->svd.noisePower = 1.0;
        svdConfiguration.beamforming->svd.powerAllocation =
            waterFilling ? MimoPowerAllocationMode::WATER_FILLING
                         : MimoPowerAllocationMode::EQUAL_POWER;
        svdConfiguration.beamforming->feedback.codebook = feedbackCodebook;
        svdConfiguration.beamforming->csiTiming.updateInterval =
            MilliSeconds(csiUpdateIntervalMs);
        svdConfiguration.beamforming->csiTiming.feedbackDelay =
            MilliSeconds(feedbackDelayMs);
        SionnaMimoHelper svdHelper(svdConfiguration);

        const Time transmissionTimestamp = MilliSeconds(transmissionTimeMs);
        const Time csiTimestamp =
            svdConfiguration.beamforming->csiTiming.ResolveCsiTimestamp(
                transmissionTimestamp);
        const Time csiAge = transmissionTimestamp - csiTimestamp;
        if (csiAge.IsZero() && std::abs(agingRotationDegrees) > 0.0)
        {
            throw std::invalid_argument(
                "agingRotationDegrees requires a positive configured CSI age");
        }

        const double openThroughput =
            RunNetworkScenario("open-loop",
                               transmissionChannel,
                               nullptr,
                               NanoSeconds(0),
                               NanoSeconds(0),
                               openHelper,
                               mcs,
                               packetSizeBytes,
                               receivedPowerDbm);
        const double fixedThroughput =
            RunNetworkScenario("fixed",
                               misoChannel,
                               nullptr,
                               NanoSeconds(0),
                               NanoSeconds(0),
                               fixedHelper,
                               mcs,
                               packetSizeBytes,
                               receivedPowerDbm);
        const double mrtThroughput =
            RunNetworkScenario("mrt",
                               misoChannel,
                               &misoChannel,
                               transmissionTimestamp,
                               transmissionTimestamp,
                               mrtHelper,
                               mcs,
                               packetSizeBytes,
                               receivedPowerDbm);
        const double svdThroughput =
            RunNetworkScenario("svd",
                               transmissionChannel,
                               &csiChannel,
                               csiTimestamp,
                               transmissionTimestamp,
                               svdHelper,
                               mcs,
                               packetSizeBytes,
                               receivedPowerDbm);

        const ScenarioResult open =
            EvaluateScenario("open-loop",
                             "open-loop",
                             transmissionChannel,
                             openHelper,
                             mcs,
                             packetSizeBytes,
                             receivedPowerW,
                             openThroughput);
        const ScenarioResult fixed =
            EvaluateScenario("fixed",
                             "fixed",
                             misoChannel,
                             fixedHelper,
                             mcs,
                             packetSizeBytes,
                             receivedPowerW,
                             fixedThroughput);
        const ScenarioResult mrt =
            EvaluateScenario("mrt",
                             "mrt",
                             misoChannel,
                             mrtHelper,
                             mcs,
                             packetSizeBytes,
                             receivedPowerW,
                             mrtThroughput);
        const ScenarioResult svd =
            EvaluateScenario("svd",
                             waterFilling ? "svd-water-filling" : "svd-equal",
                             transmissionChannel,
                             svdHelper,
                             mcs,
                             packetSizeBytes,
                             receivedPowerW,
                             svdThroughput);

        std::cout << std::fixed << std::setprecision(6);
        std::cout << "configuration reference_snr_db=" << referenceSnrDb
                  << " total_power=constant"
                  << " quantization=" << quantization
                  << " water_filling=" << (waterFilling ? 1 : 0)
                  << " csi_update_interval_ms=" << csiUpdateIntervalMs
                  << " feedback_delay_ms=" << feedbackDelayMs
                  << " csi_age_ns=" << csiAge.GetNanoSeconds()
                  << " aging_rotation_degrees=" << agingRotationDegrees
                  << " explicit_eesm_beta_linear=" << eesmBetaLinear
                  << std::endl;
        PrintResult(open);
        PrintResult(fixed);
        PrintResult(mrt);
        PrintResult(svd);

        const double expectedMrtSinr = 2.0 * referenceSnrLinear;
        const double actualMrtSinr =
            mrt.link.GetMimoResult().GetEffectiveSinrLinear(0);
        const bool powerPreserved =
            std::abs(open.precodingNorm * open.precodingNorm - 2.0) < 1e-10 &&
            std::abs(fixed.precodingNorm * fixed.precodingNorm - 1.0) < 1e-10 &&
            std::abs(mrt.precodingNorm * mrt.precodingNorm - 1.0) < 1e-10 &&
            std::abs(svd.precodingNorm * svd.precodingNorm - svdStreams) < 1e-10;
        const bool mrtGainCorrect = std::abs(actualMrtSinr - expectedMrtSinr) < 1e-10 &&
                                    actualMrtSinr >
                                        fixed.link.GetMimoResult().GetEffectiveSinrLinear(0);
        const bool idealSingleStreamSvdGain =
            svdStreams != 1 || !csiAge.IsZero() ||
                    feedbackCodebook != MimoFeedbackCodebook::UNQUANTIZED ||
                    svd.link.GetEffectiveSinrLinear() >
                        fixed.link.GetEffectiveSinrLinear();
        if (!powerPreserved || !mrtGainCorrect || !idealSingleStreamSvdGain)
        {
            std::cerr << "validation_error=precoder gain or total-power invariant failed"
                      << std::endl;
            return 1;
        }

        std::cout << "validation=PASS" << std::endl;
        return 0;
    }
    catch (const std::invalid_argument& exception)
    {
        std::cerr << "configuration_error=" << exception.what() << std::endl;
        return 2;
    }
}
