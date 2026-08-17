/* SPDX-License-Identifier: GPL-2.0-only */
// Saturated 2x2 HE-SU TCP downlink in the Sionna apartment scene.

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/packet-sink.h"
#include "ns3/sionna-helper.h"
#include "ns3/sionna-mimo-helper.h"
#include "ns3/sionna-mobility-model.h"
#include "ns3/sionna-propagation-cache.h"
#include "ns3/sionna-propagation-delay-model.h"
#include "ns3/sionna-propagation-loss-model.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/spectrum-module.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/ssid.h"
#include "ns3/wifi-module.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <string>

using namespace ns3;

namespace
{
Ptr<PacketSink> g_sink;
uint64_t g_lastRxBytes{0};
std::ofstream g_throughput;

void
ReportThroughput()
{
    const auto now = Simulator::Now();
    const auto totalRxBytes = g_sink->GetTotalRx();
    g_throughput << now.GetSeconds() << ','
                 << (totalRxBytes - g_lastRxBytes) * 8.0 / 1e5 << '\n';
    g_lastRxBytes = totalRxBytes;
    Simulator::Schedule(MilliSeconds(100), &ReportThroughput);
}

void
Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::invalid_argument(message);
    }
}
} // namespace

int
main(int argc, char* argv[])
{
    std::string environment{"apartment/apartment.xml"};
    std::string serverUrl{"tcp://localhost:5555"};
    double mobileSpeedMps{1.0};
    double txPowerDbm{20.0};
    double simulationTimeSeconds{10.0};
    uint8_t mcs{11};
    double eesmBetaLinear{std::numeric_limits<double>::quiet_NaN()};
    std::string offeredRate{"1Gbps"};
    uint32_t seed{1};
    bool caching{false};
    int fixedCoherenceTimeMs{0};
    CommandLine cmd(__FILE__);
    cmd.AddValue("environment", "Sionna scene XML", environment);
    cmd.AddValue("serverUrl", "Sionna server endpoint", serverUrl);
    cmd.AddValue("mobileSpeedMps", "STA random-walk speed in m/s", mobileSpeedMps);
    cmd.AddValue("txPowerDbm", "Total AP transmit power in dBm", txPowerDbm);
    cmd.AddValue("simulationTimeSeconds", "Simulation duration in seconds", simulationTimeSeconds);
    cmd.AddValue("mcs", "Fixed HE MCS (0--11)", mcs);
    cmd.AddValue("eesmBetaLinear", "Required calibrated EESM beta (linear)", eesmBetaLinear);
    cmd.AddValue("offeredRate", "TCP offered load; keep above PHY rate", offeredRate);
    cmd.AddValue("seed", "ns-3/Sionna random seed", seed);
    cmd.AddValue("caching", "Enable propagation-cache reuse", caching);
    cmd.AddValue("fixedCoherenceTimeMs", "Optional fixed Sionna coherence time in ms", fixedCoherenceTimeMs);
    cmd.Parse(argc, argv);
    try
    {
        Require(mcs <= 11, "--mcs must be in [0, 11]");
        Require(std::isfinite(eesmBetaLinear) && eesmBetaLinear > 0.0,
                "provide --eesmBetaLinear=<positive calibrated value>");
        Require(mobileSpeedMps >= 0.0 && simulationTimeSeconds > 2.0,
                "speed must be non-negative and duration must exceed 2 s");
        RngSeedManager::SetSeed(seed);
        SionnaHelper sionnaHelper(environment, serverUrl);
        NodeContainer staNode;
        staNode.Create(1);
        NodeContainer apNode;
        apNode.Create(1);

        auto cache = CreateObject<SionnaPropagationCache>();
        cache->SetSionnaHelper(sionnaHelper);
        cache->SetCaching(caching);
        auto channel = CreateObject<MultiModelSpectrumChannel>();
        auto pathLoss = CreateObject<SionnaPropagationLossModel>();
        pathLoss->SetPropagationCache(cache);
        channel->AddPropagationLossModel(pathLoss);
        auto spectrumLoss = CreateObject<SionnaSpectrumPropagationLossModel>();
        spectrumLoss->SetPropagationCache(cache);
        channel->AddSpectrumPropagationLossModel(spectrumLoss);
        auto delay = CreateObject<SionnaPropagationDelayModel>();
        delay->SetPropagationCache(cache);
        channel->SetPropagationDelayModel(delay);

        SpectrumWifiPhyHelper apPhy;
        SpectrumWifiPhyHelper staPhy;
        SionnaMimoConfiguration mimo;
        mimo.mode = SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
        mimo.txArray.numColumns = 2;
        mimo.rxArray.numColumns = 2;
        mimo.numTxAntennas = 2;
        mimo.numRxAntennas = 2;
        mimo.maxNss = 2;
        mimo.receiverNoiseFigureDb = 7.0;
        mimo.eesmBetaLinear.emplace(mcs, eesmBetaLinear);
        SionnaMimoHelper(mimo).ApplyForHeMcs(mcs, sionnaHelper, apPhy, staPhy, spectrumLoss);
        for (auto* phy : {&apPhy, &staPhy})
        {
            phy->SetChannel(channel);
            phy->SetErrorRateModel("ns3::TableBasedErrorRateModel");
            phy->Set("ChannelSettings", StringValue("{36, 20, BAND_5GHZ, 0}"));
            phy->Set("TxPowerStart", DoubleValue(txPowerDbm));
            phy->Set("TxPowerEnd", DoubleValue(txPowerDbm));
        }

        WifiHelper wifi;
        wifi.SetStandard(WIFI_STANDARD_80211ax);
        wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
        wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                     "DataMode", StringValue("HeMcs" + std::to_string(mcs)),
                                     "ControlMode", StringValue("OfdmRate24Mbps"));
        WifiMacHelper mac;
        const Ssid ssid("sionna-apartment-2x2");
        mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(false));
        const auto staDevices = wifi.Install(staPhy, mac, staNode);
        mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
        const auto apDevices = wifi.Install(apPhy, mac, apNode);

        MobilityHelper mobility;
        mobility.SetMobilityModel("ns3::SionnaMobilityModel");
        mobility.Install(apNode);
        mobility.SetMobilityModel("ns3::SionnaMobilityModel",
                                  "Model", EnumValue(SionnaMobilityModel::MODEL_RANDOM_WALK),
                                  "Speed", StringValue("ns3::ConstantRandomVariable[Constant=" +
                                                       std::to_string(mobileSpeedMps) + "]"),
                                  "Wall", BooleanValue(true));
        mobility.Install(staNode);
        apNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(-1.0, 0.5, 1.5));
        staNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(-2.0, 1.5, 1.6));

        InternetStackHelper internet;
        internet.Install(apNode);
        internet.Install(staNode);
        Ipv4AddressHelper addresses;
        addresses.SetBase("10.1.1.0", "255.255.255.0");
        addresses.Assign(apDevices);
        const auto staInterfaces = addresses.Assign(staDevices);
        constexpr uint16_t port = 9;
        PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        const auto sinkApps = sinkHelper.Install(staNode.Get(0));
        g_sink = DynamicCast<PacketSink>(sinkApps.Get(0));
        OnOffHelper source("ns3::TcpSocketFactory",
                           InetSocketAddress(staInterfaces.GetAddress(0), port));
        source.SetAttribute("PacketSize", UintegerValue(1472));
        source.SetAttribute("DataRate", DataRateValue(DataRate(offeredRate)));
        source.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        source.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        const auto sourceApps = source.Install(apNode.Get(0));
        sinkApps.Start(Seconds(0.5));
        sinkApps.Stop(Seconds(simulationTimeSeconds));
        sourceApps.Start(Seconds(1.0));
        sourceApps.Stop(Seconds(simulationTimeSeconds - 0.1));

        sionnaHelper.Configure(get_center_freq(apDevices.Get(0)), 20,
                               getFFTSize(WIFI_STANDARD_80211ax, 20),
                               getSubcarrierSpacing(WIFI_STANDARD_80211ax), fixedCoherenceTimeMs);
        g_throughput.open("throughput_results.csv");
        g_throughput << "time_s,throughput_mbps\n";
        Simulator::Schedule(Seconds(1.1), &ReportThroughput);
        Simulator::Stop(Seconds(simulationTimeSeconds));
        sionnaHelper.Start();
        Simulator::Run();
        const double meanMbps = g_sink->GetTotalRx() * 8.0 /
                                (simulationTimeSeconds - 1.0) / 1e6;
        std::cout << std::fixed << std::setprecision(3)
                  << "2x2 apartment TCP mean_goodput_mbps=" << meanMbps
                  << " mcs=" << unsigned(mcs) << " channel_width_mhz=20"
                  << " sta_speed_mps=" << mobileSpeedMps << std::endl;
        Simulator::Destroy();
        sionnaHelper.Destroy();
        g_throughput.close();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "configuration_error=" << exception.what() << std::endl;
        return 2;
    }
    return 0;
}
