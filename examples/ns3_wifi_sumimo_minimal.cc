/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/he-phy.h"
#include "ns3/internet-module.h"
#include "ns3/mimo-channel-matrix.h"
#include "ns3/mimo-precoding-matrix.h"
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
#include <stdexcept>
#include <string>

using namespace ns3;

namespace
{

struct ScenarioResult
{
    std::string name;
    double expectedThroughputMbps;
    HeSuMimoLinkResult link;
    double throughputMbps;
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
MakeSisoChannel(uint32_t numSubcarriers)
{
    MimoChannelMatrix channel(numSubcarriers, 1, 1);
    for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
    {
        channel.Set(sc, 0, 0, {1.0, 0.0});
    }
    return channel;
}

MimoChannelMatrix
MakeWellConditionedChannel(uint32_t numSubcarriers)
{
    MimoChannelMatrix channel(numSubcarriers, 2, 2);
    for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
    {
        channel.Set(sc, 0, 0, {std::sqrt(2.0), 0.0});
        channel.Set(sc, 1, 1, {std::sqrt(2.0), 0.0});
    }
    return channel;
}

MimoChannelMatrix
MakeRankOneChannel(uint32_t numSubcarriers)
{
    MimoChannelMatrix channel(numSubcarriers, 2, 2);
    for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
    {
        for (uint32_t rx = 0; rx < 2; ++rx)
        {
            for (uint32_t tx = 0; tx < 2; ++tx)
            {
                channel.Set(sc, rx, tx, {1.0, 0.0});
            }
        }
    }
    return channel;
}

SionnaMimoConfiguration
MakeMimoConfiguration(uint8_t antennas,
                      uint8_t mcs,
                      double eesmBetaLinear,
                      double receiverNoiseFigureDb)
{
    SionnaMimoConfiguration configuration;

    if (antennas == 2)
    {
        configuration.receiverNoiseFigureDb = receiverNoiseFigureDb;
        configuration.eesmBetaLinear.emplace(mcs, eesmBetaLinear);
        configuration.mode = SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
        configuration.txArray.numColumns = 2;
        configuration.rxArray.numColumns = 2;
        configuration.numTxAntennas = 2;
        configuration.numRxAntennas = 2;
        configuration.maxNss = 2;
    }
    else if (antennas != 1)
    {
        throw std::invalid_argument("The example supports only 1x1 and 2x2 configurations");
    }
    return configuration;
}

ScenarioResult
Evaluate(const std::string& name,
         const MimoChannelMatrix& channel,
         uint8_t nss,
         double receivedPowerW,
         WifiMode mode,
         uint32_t packetSizeBytes,
         double receiverNoiseFigureDb,
         Ptr<SionnaSpectrumPropagationLossModel> receiver,
         Ptr<ErrorRateModel> errorRateModel)
{
    const WifiTxVector txVector = MakeTxVector(mode, nss);
    const auto spectrumResult = receiver->CalculateMimoRx(channel,
                                                          receivedPowerW,
                                                          txVector,
                                                          packetSizeBytes,
                                                          static_cast<uint8_t>(
                                                              channel.GetNumTxAntennas()),
                                                          static_cast<uint8_t>(
                                                              channel.GetNumRxAntennas()),
                                                          20e6,
                                                          receiverNoiseFigureDb,
                                                          errorRateModel);

    ScenarioResult result;
    result.name = name;
    result.link = spectrumResult.GetLinkResult();
    result.expectedThroughputMbps =
        mode.GetDataRate(txVector) * result.link.GetPacketSuccessRate() / 1e6;
    return result;
}

double
RunNetworkScenario(const MimoChannelMatrix& channel,
                   const SionnaMimoConfiguration& configuration,
                   uint8_t mcs,
                   uint32_t packetSizeBytes,
                   double receivedPowerDbm)
{
    NodeContainer nodes;
    nodes.Create(2);
    NodeContainer staNode(nodes.Get(0));
    NodeContainer apNode(nodes.Get(1));

    auto spectrumChannel = CreateObject<MultiModelSpectrumChannel>();
    auto fixedRss = CreateObject<FixedRssLossModel>();
    fixedRss->SetRss(receivedPowerDbm);
    spectrumChannel->AddPropagationLossModel(fixedRss);

    SionnaHelper sionnaHelper("", "inproc://ns3sionna-mimo-helper-example");
    SpectrumWifiPhyHelper txPhy;
    SpectrumWifiPhyHelper rxPhy;
    auto mimoLoss = CreateObject<SionnaSpectrumPropagationLossModel>();
    SionnaMimoHelper(configuration).ApplyForHeMcs(mcs,
                                                  sionnaHelper,
                                                  txPhy,
                                                  rxPhy,
                                                  mimoLoss);
    mimoLoss->SetFixedChannelMatrix(channel, -receivedPowerDbm);
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
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue("HeMcs" + std::to_string(mcs)),
                                 "ControlMode",
                                 StringValue("OfdmRate24Mbps"));

    const Ssid ssid("ns3-sumimo-minimal");
    WifiMacHelper mac;
    mac.SetType("ns3::StaWifiMac",
                "Ssid",
                SsidValue(ssid),
                "ActiveProbing",
                BooleanValue(false));
    NetDeviceContainer staDevices = wifi.Install(txPhy, mac, staNode);
    mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
    NetDeviceContainer apDevices = wifi.Install(rxPhy, mac, apNode);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);
    nodes.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, 0.0));
    nodes.Get(1)->GetObject<MobilityModel>()->SetPosition(Vector(1.0, 0.0, 0.0));

    InternetStackHelper internet;
    internet.Install(nodes);
    Ipv4AddressHelper addresses;
    addresses.SetBase("10.1.0.0", "255.255.255.0");
    addresses.Assign(staDevices);
    const Ipv4InterfaceContainer apInterfaces = addresses.Assign(apDevices);

    constexpr uint16_t port = 9000;
    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApps = sinkHelper.Install(apNode.Get(0));
    sinkApps.Start(Seconds(0.5));
    sinkApps.Stop(Seconds(2.1));

    OnOffHelper source("ns3::UdpSocketFactory",
                       InetSocketAddress(apInterfaces.GetAddress(0), port));
    source.SetAttribute("DataRate", DataRateValue(DataRate("1Gbps")));
    source.SetAttribute("PacketSize", UintegerValue(packetSizeBytes));
    source.SetAttribute("OnTime",
                        StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    source.SetAttribute("OffTime",
                        StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer sourceApps = source.Install(staNode.Get(0));
    sourceApps.Start(Seconds(1.0));
    sourceApps.Stop(Seconds(2.0));

    Simulator::Stop(Seconds(2.1));
    Simulator::Run();
    const uint64_t receivedBytes =
        DynamicCast<PacketSink>(sinkApps.Get(0))->GetTotalRx();
    Simulator::Destroy();
    return receivedBytes * 8.0 / 1e6;
}

void
PrintResult(const ScenarioResult& result)
{
    const auto& mimo = result.link.GetMimoResult();
    std::cout << "scenario=" << result.name
              << " nss=" << mimo.GetNumStreams()
              << " expected_throughput_Mbps=" << result.expectedThroughputMbps
              << " rank=" << mimo.GetMinimumChannelRank()
              << " throughput_Mbps=" << result.throughputMbps
              << " PER=" << result.link.GetPacketErrorRate();
    for (uint32_t stream = 0; stream < mimo.GetNumStreams(); ++stream)
    {
        std::cout << " stream" << stream << "_SINR_dB="
                  << WifiLinkBudget::LinearToDb(mimo.GetEffectiveSinrLinear(stream));
    }
    std::cout << " effective_SINR_dB="
              << WifiLinkBudget::LinearToDb(result.link.GetEffectiveSinrLinear()) << std::endl;
}

} // namespace

int
main(int argc, char* argv[])
{
    uint8_t mcs = 5;
    uint32_t packetSizeBytes = 1200;
    double referenceSnrDb = 20.0;
    double eesmBetaLinear = std::numeric_limits<double>::quiet_NaN();
    std::string failureCase{"none"};

    CommandLine cmd(__FILE__);
    cmd.AddValue("mcs", "HE MCS (0 through 11)", mcs);
    cmd.AddValue("packetSize", "PSDU size in bytes", packetSizeBytes);
    cmd.AddValue("referenceSnrDb", "Total pre-CFR reference SNR in dB", referenceSnrDb);
    cmd.AddValue("eesmBetaLinear",
                 "Required EESM beta in linear SINR units; no default is supplied",
                 eesmBetaLinear);
    cmd.AddValue("failureCase",
                 "Integration-negative case: none, invalid-nss, unsupported-mode, "
                 "missing-beta, or phy-cfr-mismatch",
                 failureCase);
    cmd.Parse(argc, argv);

    if (mcs > 11 || packetSizeBytes == 0 || !std::isfinite(eesmBetaLinear) ||
        eesmBetaLinear <= 0.0)
    {
        std::cerr << "Provide --eesmBetaLinear=<positive value>; the example intentionally has "
                     "no production beta default"
                  << std::endl;
        return 2;
    }

    if (failureCase != "none" && failureCase != "invalid-nss" &&
        failureCase != "unsupported-mode" && failureCase != "missing-beta" &&
        failureCase != "phy-cfr-mismatch")
    {
        std::cerr << "Unknown --failureCase=" << failureCase << std::endl;
        return 2;
    }

    constexpr double bandwidthHz = 20e6;
    constexpr double noiseFigureDb = 7.0;
    constexpr double temperatureK = 290.0;
    const auto noiseReference =
        WifiLinkBudget::Calculate(0.0,
                                  0.0,
                                  bandwidthHz,
                                  temperatureK,
                                  noiseFigureDb);
    const double receivedPowerW =
        WifiLinkBudget::DbToLinear(referenceSnrDb) *
        noiseReference.GetReceiverNoisePowerW();

    const WifiMode mode = HePhy::GetHeMcs(mcs);

    constexpr uint32_t numSubcarriers = 768; // 20 MHz plus SpectrumWifiPhy guard bands
    const MimoChannelMatrix sisoChannel = MakeSisoChannel(numSubcarriers);
    const MimoChannelMatrix wellConditionedChannel =
        MakeWellConditionedChannel(numSubcarriers);
    const MimoChannelMatrix rankOneChannel = MakeRankOneChannel(numSubcarriers);
    const SionnaMimoConfiguration sisoConfiguration =
        MakeMimoConfiguration(1, mcs, eesmBetaLinear, noiseFigureDb);
    const SionnaMimoConfiguration mimoConfiguration =
        MakeMimoConfiguration(2, mcs, eesmBetaLinear, noiseFigureDb);
    SionnaMimoConfiguration identityConfiguration = mimoConfiguration;
    identityConfiguration.precodingMatrix = MimoPrecodingMatrix::FrequencyFlat(
        numSubcarriers,
        2,
        2,
        {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}});
    SionnaMimoConfiguration selectedConfiguration = mimoConfiguration;
    selectedConfiguration.maxNss = 1;
    selectedConfiguration.precodingMatrix = MimoPrecodingMatrix::FrequencyFlat(
        numSubcarriers,
        2,
        1,
        {{1.0, 0.0}, {0.0, 0.0}});
    const double receivedPowerDbm = WToDbm(receivedPowerW);

    if (failureCase == "phy-cfr-mismatch")
    {
        RunNetworkScenario(sisoChannel,
                           mimoConfiguration,
                           mcs,
                           packetSizeBytes,
                           receivedPowerDbm);
        std::cerr << "The PHY/CFR mismatch was not rejected" << std::endl;
        return 1;
    }
    if (failureCase == "missing-beta")
    {
        SionnaMimoConfiguration missingBetaConfiguration = mimoConfiguration;
        missingBetaConfiguration.eesmBetaLinear.clear();
        SionnaHelper negativeSionna("", "inproc://ns3sionna-mimo-helper-missing-beta");
        SpectrumWifiPhyHelper negativeTxPhy;
        SpectrumWifiPhyHelper negativeRxPhy;
        auto negativeModel = CreateObject<SionnaSpectrumPropagationLossModel>();
        try
        {
            SionnaMimoHelper(missingBetaConfiguration)
                .ApplyForHeMcs(mcs,
                               negativeSionna,
                               negativeTxPhy,
                               negativeRxPhy,
                               negativeModel);
        }
        catch (const std::invalid_argument& exception)
        {
            std::cerr << "configuration_error=" << exception.what() << std::endl;
            return 2;
        }
        std::cerr << "The missing EESM beta was not rejected" << std::endl;
        return 1;
    }

    if (failureCase != "none")
    {
        SionnaMimoConfiguration invalidConfiguration = mimoConfiguration;
        if (failureCase == "invalid-nss")
        {
            invalidConfiguration.maxNss = 1;
        }
        else
        {
            invalidConfiguration.mode = SionnaMimoMode::MU_MIMO;
            invalidConfiguration.rxArray.numColumns = 1;
            invalidConfiguration.numRxAntennas = 1;
            invalidConfiguration.maxNss = 1;
        }
        try
        {
            SionnaMimoHelper invalidHelper(invalidConfiguration);
            static_cast<void>(invalidHelper);
        }
        catch (const std::invalid_argument& exception)
        {
            std::cerr << "configuration_error=" << exception.what() << std::endl;
            return 2;
        }
        std::cerr << "The invalid configuration was not rejected" << std::endl;
        return 1;
    }

    SionnaHelper diagnosticSionna("", "inproc://ns3sionna-mimo-helper-diagnostic");
    SpectrumWifiPhyHelper diagnosticTxPhy;
    SpectrumWifiPhyHelper diagnosticRxPhy;
    auto receiver = CreateObject<SionnaSpectrumPropagationLossModel>();
    SionnaMimoHelper(mimoConfiguration)
        .ApplyForHeMcs(mcs,
                       diagnosticSionna,
                       diagnosticTxPhy,
                       diagnosticRxPhy,
                       receiver);
    SionnaHelper identitySionna("", "inproc://ns3sionna-mimo-identity-diagnostic");
    SpectrumWifiPhyHelper identityTxPhy;
    SpectrumWifiPhyHelper identityRxPhy;
    auto identityReceiver = CreateObject<SionnaSpectrumPropagationLossModel>();
    SionnaMimoHelper(identityConfiguration)
        .ApplyForHeMcs(mcs,
                       identitySionna,
                       identityTxPhy,
                       identityRxPhy,
                       identityReceiver);
    SionnaHelper selectedSionna("", "inproc://ns3sionna-mimo-selected-diagnostic");
    SpectrumWifiPhyHelper selectedTxPhy;
    SpectrumWifiPhyHelper selectedRxPhy;
    auto selectedReceiver = CreateObject<SionnaSpectrumPropagationLossModel>();
    SionnaMimoHelper(selectedConfiguration)
        .ApplyForHeMcs(mcs,
                       selectedSionna,
                       selectedTxPhy,
                       selectedRxPhy,
                       selectedReceiver);
    auto errorRateModel = CreateObject<TableBasedErrorRateModel>();

    ScenarioResult siso = Evaluate("siso-1x1",
                                   sisoChannel,
                                   1,
                                   receivedPowerW,
                                   mode,
                                   packetSizeBytes,
                                   noiseFigureDb,
                                   receiver,
                                   errorRateModel);
    ScenarioResult wellConditioned = Evaluate("well-conditioned-2x2",
                                              wellConditionedChannel,
                                              2,
                                              receivedPowerW,
                                              mode,
                                              packetSizeBytes,
                                              noiseFigureDb,
                                              receiver,
                                              errorRateModel);
    ScenarioResult rankOne = Evaluate("rank-one-2x2",
                                      rankOneChannel,
                                      2,
                                      receivedPowerW,
                                      mode,
                                      packetSizeBytes,
                                      noiseFigureDb,
                                      receiver,
                                      errorRateModel);
    ScenarioResult identity = Evaluate("identity-precoded-2x2",
                                       wellConditionedChannel,
                                       2,
                                       receivedPowerW,
                                       mode,
                                       packetSizeBytes,
                                       noiseFigureDb,
                                       identityReceiver,
                                       errorRateModel);
    ScenarioResult selected = Evaluate("selected-stream-2x2",
                                       wellConditionedChannel,
                                       1,
                                       receivedPowerW,
                                       mode,
                                       packetSizeBytes,
                                       noiseFigureDb,
                                       selectedReceiver,
                                       errorRateModel);

    siso.throughputMbps = RunNetworkScenario(sisoChannel,
                                             sisoConfiguration,
                                             mcs,
                                             packetSizeBytes,
                                             receivedPowerDbm);
    wellConditioned.throughputMbps =
        RunNetworkScenario(wellConditionedChannel,
                           mimoConfiguration,
                           mcs,
                           packetSizeBytes,
                           receivedPowerDbm);
    rankOne.throughputMbps = RunNetworkScenario(rankOneChannel,
                                                mimoConfiguration,
                                                mcs,
                                                packetSizeBytes,
                                                receivedPowerDbm);
    identity.throughputMbps = RunNetworkScenario(wellConditionedChannel,
                                                 identityConfiguration,
                                                 mcs,
                                                 packetSizeBytes,
                                                 receivedPowerDbm);
    selected.throughputMbps = RunNetworkScenario(wellConditionedChannel,
                                                 selectedConfiguration,
                                                 mcs,
                                                 packetSizeBytes,
                                                 receivedPowerDbm);

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "explicit_EESM_beta_linear=" << eesmBetaLinear
              << " (caller supplied; not a production calibration)" << std::endl;
    PrintResult(siso);
    PrintResult(wellConditioned);
    PrintResult(rankOne);
    PrintResult(identity);
    PrintResult(selected);

    const bool ranksAreCorrect =
        siso.link.GetMimoResult().GetMinimumChannelRank() == 1 &&
        wellConditioned.link.GetMimoResult().GetMinimumChannelRank() == 2 &&
        rankOne.link.GetMimoResult().GetMinimumChannelRank() == 1 &&
        identity.link.GetMimoResult().GetMinimumChannelRank() == 2 &&
        selected.link.GetMimoResult().GetMinimumChannelRank() == 1;
    const bool multiplexingGain =
        wellConditioned.throughputMbps > 1.5 * siso.throughputMbps &&
        wellConditioned.expectedThroughputMbps > 1.9 * siso.expectedThroughputMbps;
    const bool rankOneDegrades =
        rankOne.link.GetEffectiveSinrLinear() < siso.link.GetEffectiveSinrLinear() &&
        rankOne.throughputMbps < 0.5 * siso.throughputMbps &&
        rankOne.expectedThroughputMbps < siso.expectedThroughputMbps;
    const bool identityPreservesPhase2 =
        identity.link.GetEffectiveSinrLinear() ==
            wellConditioned.link.GetEffectiveSinrLinear() &&
        identity.link.GetPacketErrorRate() == wellConditioned.link.GetPacketErrorRate() &&
        identity.expectedThroughputMbps == wellConditioned.expectedThroughputMbps &&
        std::abs(identity.throughputMbps - wellConditioned.throughputMbps) <
            0.02 * wellConditioned.throughputMbps;
    const bool selectedStreamIsControlled =
        selected.link.GetMimoResult().GetNumStreams() == 1 &&
        std::abs(selected.link.GetEffectiveSinrLinear() - 2.0 *
                     WifiLinkBudget::DbToLinear(referenceSnrDb)) < 1e-10 &&
        selected.throughputMbps > 0.9 * siso.throughputMbps;
    if (!ranksAreCorrect || !multiplexingGain || !rankOneDegrades ||
        !identityPreservesPhase2 || !selectedStreamIsControlled)
    {
        std::cerr << "Validation failed: choose an MCS/SNR point where the calibrated error "
                     "model resolves the expected full-rank gain and rank-one collapse"
                  << std::endl;
        return 1;
    }

    std::cout << "validation=PASS Phase-2 paths preserved; identity precoding matches; "
                 "one 2x2 stream selected"
              << std::endl;
    return 0;
}
