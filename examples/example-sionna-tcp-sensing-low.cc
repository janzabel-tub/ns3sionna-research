/*
 * Copyright (c) 2026 Jan
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Author: jan.zabel@tu-berlin.de
 */

// Sionna models
#include "ns3/cfr-tag.h"
#include "ns3/sionna-helper.h"
#include "ns3/sionna-mobility-model.h"
#include "ns3/sionna-propagation-cache.h"
#include "ns3/sionna-propagation-delay-model.h"
#include "ns3/sionna-propagation-loss-model.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/sionna-utils.h"

// Ns-3 modules
#include <ns3/wifi-spectrum-phy-interface.h>
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-module.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/ssid.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"

#include <cmath>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

/**
 * Small TCP example showing layer-1 / PHY reception logging with ns3sionna.
 * One AP sends TCP traffic to one mobile STA. The STA logs successful, failed,
 * and dropped PHY data-frame receptions with position, pathloss, RX power and SNR.
 *
 * To run: ./example-sionna-tcp-sensing-low.sh
 */

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ExampleSionnaTcpSensingLow");

static constexpr uint32_t INVALID_NODE_ID = 0xFFFFFFFF;
static constexpr double NOISE_FIGURE_DB = 7.0;
static double g_txPowerDbm = 20.0;
static double g_noiseFloorDbm =
    -174.0 + 10.0 * std::log10(80e6) + NOISE_FIGURE_DB;

std::ofstream g_l1Log;
std::set<uint32_t> g_staNodeIds;

struct PhyRxBeginSample
{
    double timeSeconds = 0.0;
    Vector position;
    bool isDataFrame = false;
    uint32_t packetSize = 0;
    bool hasCfrTag = false;
    double pathLossDb = std::numeric_limits<double>::quiet_NaN();
    double rxPowerDbm = std::numeric_limits<double>::quiet_NaN();
};

std::map<std::pair<uint32_t, uint64_t>, PhyRxBeginSample> g_phyRxBeginSamples;
std::set<std::pair<uint32_t, uint64_t>> g_phyRxFinalized;

std::pair<uint32_t, uint64_t>
MakePhyRxSampleKey(uint32_t nodeId, uint64_t packetUid)
{
    return {nodeId, packetUid};
}

uint32_t
ExtractNodeIdFromContext(const std::string& context)
{
    static const std::string needle = "/NodeList/";
    const size_t start = context.find(needle);
    if (start == std::string::npos)
    {
        return INVALID_NODE_ID;
    }

    const size_t idBegin = start + needle.size();
    const size_t idEnd = context.find('/', idBegin);
    if (idEnd == std::string::npos || idEnd == idBegin)
    {
        return INVALID_NODE_ID;
    }

    try
    {
        return static_cast<uint32_t>(std::stoul(context.substr(idBegin, idEnd - idBegin)));
    }
    catch (const std::exception&)
    {
        return INVALID_NODE_ID;
    }
}

bool
IsStaNode(uint32_t nodeId)
{
    return g_staNodeIds.find(nodeId) != g_staNodeIds.end();
}

Vector
GetNodePosition(uint32_t nodeId)
{
    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<MobilityModel> mobility = node ? node->GetObject<MobilityModel>() : nullptr;
    return mobility ? mobility->GetPosition() : Vector(0.0, 0.0, 0.0);
}

bool
IsWifiDataFrame(Ptr<const Packet> packet)
{
    WifiMacHeader hdr;
    return packet && packet->PeekHeader(hdr) > 0 && hdr.IsData();
}

double
LinearSnrToDb(double snrLinear)
{
    return snrLinear > 0.0 ? 10.0 * std::log10(snrLinear)
                           : std::numeric_limits<double>::quiet_NaN();
}

double
FallbackSnrFromPathLoss(const PhyRxBeginSample& sample)
{
    return sample.hasCfrTag && std::isfinite(sample.rxPowerDbm)
               ? sample.rxPowerDbm - g_noiseFloorDbm
               : std::numeric_limits<double>::quiet_NaN();
}

void
WritePhyLog(uint32_t rxNodeId,
            uint64_t packetUid,
            const PhyRxBeginSample* beginSample,
            Ptr<const Packet> packet,
            const std::string& rxStatus,
            double phySnrDb,
            const std::string& failureReason = "")
{
    if (!g_l1Log.is_open() || !packet)
    {
        return;
    }

    PhyRxBeginSample fallback;
    if (beginSample == nullptr)
    {
        fallback.timeSeconds = Simulator::Now().GetSeconds();
        fallback.position = GetNodePosition(rxNodeId);
        fallback.isDataFrame = IsWifiDataFrame(packet);
        fallback.packetSize = packet->GetSize();

        CFRTag tag;
        fallback.hasCfrTag = packet->PeekPacketTag(tag);
        if (fallback.hasCfrTag)
        {
            fallback.pathLossDb = tag.GetPathloss();
            fallback.rxPowerDbm = g_txPowerDbm - fallback.pathLossDb;
        }

        beginSample = &fallback;
    }

    if (!beginSample->isDataFrame)
    {
        return;
    }

    const double snrDb =
        std::isfinite(phySnrDb) ? phySnrDb : FallbackSnrFromPathLoss(*beginSample);

    g_l1Log << beginSample->timeSeconds
            << "," << rxNodeId
            << "," << beginSample->position.x
            << "," << beginSample->position.y
            << "," << beginSample->position.z
            << "," << beginSample->pathLossDb
            << "," << beginSample->rxPowerDbm
            << "," << snrDb
            << "," << rxStatus
            << "," << packetUid
            << "," << beginSample->packetSize
            << "," << failureReason
            << "\n";
}

void
PhyRxBeginTrace(std::string context,
                Ptr<const Packet> packet,
                std::map<WifiSpectrumBandInfo, double> rxPowerWPerBand)
{
    (void) rxPowerWPerBand;

    const uint32_t rxNodeId = ExtractNodeIdFromContext(context);
    if (rxNodeId == INVALID_NODE_ID || !IsStaNode(rxNodeId) || !packet)
    {
        return;
    }

    PhyRxBeginSample sample;
    sample.timeSeconds = Simulator::Now().GetSeconds();
    sample.position = GetNodePosition(rxNodeId);
    sample.isDataFrame = IsWifiDataFrame(packet);
    sample.packetSize = packet->GetSize();

    CFRTag tag;
    sample.hasCfrTag = packet->PeekPacketTag(tag);
    if (sample.hasCfrTag)
    {
        sample.pathLossDb = tag.GetPathloss();
        sample.rxPowerDbm = g_txPowerDbm - sample.pathLossDb;
    }

    g_phyRxBeginSamples[MakePhyRxSampleKey(rxNodeId, packet->GetUid())] = sample;
}

void
PhyRxOkTrace(std::string context,
             Ptr<const Packet> packet,
             double snrLinear,
             WifiMode mode,
             WifiPreamble preamble)
{
    (void) mode;
    (void) preamble;

    const uint32_t rxNodeId = ExtractNodeIdFromContext(context);
    if (rxNodeId == INVALID_NODE_ID || !IsStaNode(rxNodeId) || !packet)
    {
        return;
    }

    const auto key = MakePhyRxSampleKey(rxNodeId, packet->GetUid());
    if (g_phyRxFinalized.find(key) != g_phyRxFinalized.end())
    {
        return;
    }

    auto it = g_phyRxBeginSamples.find(key);
    WritePhyLog(rxNodeId,
                packet->GetUid(),
                it != g_phyRxBeginSamples.end() ? &it->second : nullptr,
                packet,
                "OK",
                LinearSnrToDb(snrLinear));

    g_phyRxFinalized.insert(key);
    if (it != g_phyRxBeginSamples.end())
    {
        g_phyRxBeginSamples.erase(it);
    }
}

void
PhyRxErrorTrace(std::string context, Ptr<const Packet> packet, double snrLinear)
{
    const uint32_t rxNodeId = ExtractNodeIdFromContext(context);
    if (rxNodeId == INVALID_NODE_ID || !IsStaNode(rxNodeId) || !packet)
    {
        return;
    }

    const auto key = MakePhyRxSampleKey(rxNodeId, packet->GetUid());
    if (g_phyRxFinalized.find(key) != g_phyRxFinalized.end())
    {
        return;
    }

    auto it = g_phyRxBeginSamples.find(key);
    WritePhyLog(rxNodeId,
                packet->GetUid(),
                it != g_phyRxBeginSamples.end() ? &it->second : nullptr,
                packet,
                "ERROR",
                LinearSnrToDb(snrLinear));

    g_phyRxFinalized.insert(key);
    if (it != g_phyRxBeginSamples.end())
    {
        g_phyRxBeginSamples.erase(it);
    }
}

void
PhyRxDropTrace(std::string context, Ptr<const Packet> packet, WifiPhyRxfailureReason reason)
{
    const uint32_t rxNodeId = ExtractNodeIdFromContext(context);
    if (rxNodeId == INVALID_NODE_ID || !IsStaNode(rxNodeId) || !packet)
    {
        return;
    }

    const auto key = MakePhyRxSampleKey(rxNodeId, packet->GetUid());
    if (g_phyRxFinalized.find(key) != g_phyRxFinalized.end())
    {
        return;
    }

    auto it = g_phyRxBeginSamples.find(key);
    WritePhyLog(rxNodeId,
                packet->GetUid(),
                it != g_phyRxBeginSamples.end() ? &it->second : nullptr,
                packet,
                "DROP",
                std::numeric_limits<double>::quiet_NaN(),
                std::to_string(static_cast<int>(reason)));

    g_phyRxFinalized.insert(key);
    if (it != g_phyRxBeginSamples.end())
    {
        g_phyRxBeginSamples.erase(it);
    }
}

int
main(int argc, char* argv[])
{
    bool verbose = true;
    bool tracing = false;
    bool caching = true;
    std::string environment = "apartment/apartment.xml";
    std::string outputFile = "example-sionna-tcp-sensing-low-l1.csv";
    int wifi_channel_num = 42;
    int sim_end_time_sec = 10;
    int channel_width = 80;
    int fixed_coherence_time_ms = 100;
    int data_Rate_mbps = 150;
    uint32_t seed = 1;
    double speed = 1.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("verbose", "Enable logging", verbose);
    cmd.AddValue("tracing", "Enable pcap tracing", tracing);
    cmd.AddValue("caching", "Enable caching of propagation delay and loss", caching);
    cmd.AddValue("environment", "Xml file of environment", environment);
    cmd.AddValue("channel", "The WiFi channel number", wifi_channel_num);
    cmd.AddValue("simEndTimeSec", "The total simulation time", sim_end_time_sec);
    cmd.AddValue("channelWidth", "The WiFi channel width in MHz", channel_width);
    cmd.AddValue("fixedCoherenceTimeMs", "The fixed coherence time in msec", fixed_coherence_time_ms);
    cmd.AddValue("dataRate", "Data Rate in Mb/s", data_Rate_mbps);
    cmd.AddValue("seed", "Set the seed for the run", seed);
    cmd.AddValue("speed", "STA random-walk speed in m/s", speed);
    cmd.AddValue("txPower", "Transmit power in dBm", g_txPowerDbm);
    cmd.AddValue("output", "CSV file for PHY reception logging", outputFile);
    cmd.Parse(argc, argv);

    g_noiseFloorDbm =
        -174.0 + 10.0 * std::log10(channel_width * 1e6) + NOISE_FIGURE_DB;

    if (verbose)
    {
        LogComponentEnable("ExampleSionnaTcpSensingLow", LOG_INFO);
        LogComponentEnable("SionnaPropagationDelayModel", LOG_INFO);
        LogComponentEnable("SionnaPropagationLossModel", LOG_INFO);
        LogComponentEnable("SionnaPropagationCache", LOG_INFO);
        LogComponentEnable("SionnaSpectrumPropagationLossModel", LOG_INFO);
    }

    std::cout << "TCP ns3sionna example with layer-1 reception logging"
              << std::endl << std::endl;

    g_l1Log.open(outputFile);
    g_l1Log << "Time_s,STA_NodeId,pos_x,pos_y,pos_z,PathLoss_dB,RxPower_dBm,"
            << "SNR_dB,RxStatus,PacketUid,PacketSize_Bytes,FailureReason\n";

    RngSeedManager::SetSeed(seed);
    SionnaHelper sionnaHelper(environment, "tcp://localhost:5555");

    NodeContainer wifiStaNode;
    wifiStaNode.Create(1);

    NodeContainer wifiApNode;
    wifiApNode.Create(1);

    Config::Set("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/ChannelWidth",
                UintegerValue(channel_width));

    Ptr<SionnaPropagationCache> propagationCache = CreateObject<SionnaPropagationCache>();
    propagationCache->SetSionnaHelper(sionnaHelper);
    propagationCache->SetCaching(caching);

    Ptr<MultiModelSpectrumChannel> spectrumChannel = CreateObject<MultiModelSpectrumChannel>();

    Ptr<SionnaPropagationLossModel> lossModel = CreateObject<SionnaPropagationLossModel>();
    lossModel->SetPropagationCache(propagationCache);
    spectrumChannel->AddPropagationLossModel(lossModel);

    Ptr<SionnaSpectrumPropagationLossModel> spectrumLossModel =
        CreateObject<SionnaSpectrumPropagationLossModel>();
    spectrumLossModel->SetPropagationCache(propagationCache);
    spectrumChannel->AddSpectrumPropagationLossModel(spectrumLossModel);

    Ptr<SionnaPropagationDelayModel> delayModel = CreateObject<SionnaPropagationDelayModel>();
    delayModel->SetPropagationCache(propagationCache);
    spectrumChannel->SetPropagationDelayModel(delayModel);

    SpectrumWifiPhyHelper spectrumPhy;
    spectrumPhy.SetChannel(spectrumChannel);
    spectrumPhy.SetErrorRateModel("ns3::NistErrorRateModel");
    spectrumPhy.Set("TxPowerStart", DoubleValue(g_txPowerDbm));
    spectrumPhy.Set("TxPowerEnd", DoubleValue(g_txPowerDbm));

    WifiMacHelper mac;
    Ssid ssid = Ssid("ns-3-ssid");

    WifiHelper wifi;
    WifiStandard wifi_standard = WIFI_STANDARD_80211ax;
    wifi.SetStandard(wifi_standard);
    wifi.SetRemoteStationManager("ns3::IdealWifiManager",
                                 "RtsCtsThreshold",
                                 UintegerValue(999999));

    std::string channelStr = "{" + std::to_string(wifi_channel_num) + ", " +
                             std::to_string(channel_width) + ", BAND_5GHZ, 0}";

    NetDeviceContainer staDevices;
    mac.SetType("ns3::StaWifiMac",
                "Ssid",
                SsidValue(ssid),
                "ActiveProbing",
                BooleanValue(false));
    spectrumPhy.Set("ChannelSettings", StringValue(channelStr));
    staDevices = wifi.Install(spectrumPhy, mac, wifiStaNode);

    NetDeviceContainer apDevices;
    mac.SetType("ns3::ApWifiMac",
                "Ssid",
                SsidValue(ssid),
                "BeaconGeneration",
                BooleanValue(true),
                "BeaconInterval",
                TimeValue(Seconds(5.120)),
                "EnableBeaconJitter",
                BooleanValue(false));
    spectrumPhy.Set("ChannelSettings", StringValue(channelStr));
    apDevices = wifi.Install(spectrumPhy, mac, wifiApNode);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::SionnaMobilityModel");
    mobility.Install(wifiApNode);

    mobility.SetMobilityModel("ns3::SionnaMobilityModel",
                              "Model",
                              EnumValue(SionnaMobilityModel::MODEL_RANDOM_WALK),
                              "Speed",
                              StringValue("ns3::ConstantRandomVariable[Constant=" +
                                          std::to_string(speed) + "]"),
                              "Wall",
                              BooleanValue(true));
    mobility.Install(wifiStaNode);

    wifiApNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(-1.0, 0.5, 1.5));
    wifiStaNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(-2.0, 2.0, 1.6));
    g_staNodeIds.insert(wifiStaNode.Get(0)->GetId());

    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       TypeIdValue(TypeId::LookupByName("ns3::TcpCubic")));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1472));

    InternetStackHelper stack;
    stack.Install(wifiApNode);
    stack.Install(wifiStaNode);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer wifiStaInterfaces = address.Assign(staDevices);
    address.Assign(apDevices);

    PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), 9));
    ApplicationContainer sinkApps = sinkHelper.Install(wifiStaNode);
    sinkApps.Start(Seconds(0.9));
    sinkApps.Stop(Seconds(sim_end_time_sec));

    OnOffHelper tcpClient("ns3::TcpSocketFactory",
                          InetSocketAddress(wifiStaInterfaces.GetAddress(0), 9));
    tcpClient.SetAttribute("PacketSize", UintegerValue(1472));
    tcpClient.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    tcpClient.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    tcpClient.SetAttribute("DataRate",
                           DataRateValue(DataRate(std::to_string(data_Rate_mbps) + "Mb/s")));

    ApplicationContainer clientApps = tcpClient.Install(wifiApNode);
    clientApps.Start(Seconds(1.0));
    clientApps.Stop(Seconds(sim_end_time_sec));

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    double fc = get_center_freq(apDevices.Get(0));
    sionnaHelper.Configure(fc,
                           channel_width,
                           getFFTSize(wifi_standard, channel_width),
                           getSubcarrierSpacing(wifi_standard),
                           fixed_coherence_time_ms);
    sionnaHelper.SetMode(SionnaHelper::MODE_P2P);

    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/PhyRxBegin",
                    MakeCallback(&PhyRxBeginTrace));
    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/PhyRxDrop",
                    MakeCallback(&PhyRxDropTrace));
    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/State/RxError",
                    MakeCallback(&PhyRxErrorTrace));
    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/State/RxOk",
                    MakeCallback(&PhyRxOkTrace));

    if (tracing)
    {
        std::cout << "Writing pcap files ..." << std::endl;
        spectrumPhy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        spectrumPhy.EnablePcap("example-sionna-tcp-sensing-low", apDevices.Get(0));
        spectrumPhy.EnablePcap("example-sionna-tcp-sensing-low", staDevices.Get(0));
    }

    Simulator::Stop(Seconds(sim_end_time_sec));

    sionnaHelper.Start();
    Simulator::Run();
    Simulator::Destroy();

    propagationCache->PrintStats();
    sionnaHelper.Destroy();

    g_l1Log.close();
    std::cout << "Layer-1 log can be found in: " << outputFile << std::endl;

    return 0;
}
