/*
 * Copyright (c) 2026 Jan
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Author: jan.zabel@tu-berlin.de
 */

// Sionna models
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
#include "ns3/minstrel-wifi-manager.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-module.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/ssid.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"
#include "ns3/wifi-tx-vector.h"

#include <exception>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

/**
 * Small TCP example showing MinstrelHt rate adaptation with ns3sionna.
 * One AP sends TCP traffic to one mobile STA. The example logs the selected
 * WifiTxVector for transmitted data frames and Minstrel rate-change events.
 *
 * To run: ./example-sionna-tcp-minstrel.sh
 */

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ExampleSionnaTcpMinstrel");

static constexpr uint32_t INVALID_NODE_ID = 0xFFFFFFFF;

std::ofstream g_txVectorLog;
std::ofstream g_rateChangeLog;
std::map<std::string, uint32_t> g_macNodeIds;

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

uint32_t
ExtractDeviceIdFromContext(const std::string& context)
{
    static const std::string needle = "/DeviceList/";
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

std::string
MacAddressToString(Mac48Address address)
{
    std::ostringstream stream;
    stream << address;
    return stream.str();
}

uint32_t
LookupNodeIdForMac(Mac48Address address)
{
    const auto it = g_macNodeIds.find(MacAddressToString(address));
    return it != g_macNodeIds.end() ? it->second : INVALID_NODE_ID;
}

void
RegisterWifiDeviceMacs(const NetDeviceContainer& devices)
{
    for (uint32_t i = 0; i < devices.GetN(); ++i)
    {
        Ptr<WifiNetDevice> wifiDevice = DynamicCast<WifiNetDevice>(devices.Get(i));
        if (!wifiDevice || !wifiDevice->GetMac() || !wifiDevice->GetNode())
        {
            continue;
        }

        g_macNodeIds[MacAddressToString(wifiDevice->GetMac()->GetAddress())] =
            wifiDevice->GetNode()->GetId();
    }
}

void
MinstrelRateChangeTrace(std::string context, uint64_t oldRate, uint64_t newRate)
{
    if (!g_rateChangeLog.is_open())
    {
        return;
    }

    const uint32_t nodeId = ExtractNodeIdFromContext(context);
    const uint32_t deviceId = ExtractDeviceIdFromContext(context);

    g_rateChangeLog << Simulator::Now().GetSeconds()
                    << "," << nodeId
                    << "," << deviceId
                    << "," << oldRate
                    << "," << newRate
                    << "," << (newRate / 1e6)
                    << "\n";
}

void
MinstrelTxVectorTrace(std::string context,
                      Ptr<const Packet> packet,
                      uint16_t channelFreqMhz,
                      WifiTxVector txVector,
                      MpduInfo mpduInfo,
                      uint16_t staId)
{
    if (!g_txVectorLog.is_open() || !packet)
    {
        return;
    }

    WifiMacHeader hdr;
    if (packet->PeekHeader(hdr) == 0 || !hdr.IsData())
    {
        return;
    }

    const uint32_t txNodeId = ExtractNodeIdFromContext(context);
    const uint32_t txDeviceId = ExtractDeviceIdFromContext(context);
    const Mac48Address dstAddress = hdr.GetAddr1();
    const Mac48Address srcAddress = hdr.GetAddr2();
    const uint32_t srcNodeId = LookupNodeIdForMac(srcAddress);
    const uint32_t dstNodeId = LookupNodeIdForMac(dstAddress);
    const uint16_t channelWidthMhz = txVector.GetChannelWidth();
    const uint16_t guardIntervalNs = txVector.GetGuardInterval();
    const uint8_t nss = txVector.GetNss();
    const uint64_t rateBps = txVector.GetMode().GetDataRate(txVector);

    g_txVectorLog << Simulator::Now().GetSeconds()
                  << "," << txNodeId
                  << "," << txDeviceId
                  << "," << srcNodeId
                  << "," << dstNodeId
                  << "," << MacAddressToString(dstAddress)
                  << "," << MacAddressToString(srcAddress)
                  << "," << packet->GetUid()
                  << "," << txVector.GetMode().GetUniqueName()
                  << "," << rateBps
                  << "," << (rateBps / 1e6)
                  << "," << channelFreqMhz
                  << "," << channelWidthMhz
                  << "," << guardIntervalNs
                  << "," << static_cast<uint32_t>(nss)
                  << "," << txVector.IsAggregation()
                  << "," << static_cast<uint32_t>(mpduInfo.type)
                  << "," << mpduInfo.mpduRefNumber
                  << "," << staId
                  << "\n";
}

int
main(int argc, char* argv[])
{
    bool verbose = true;
    bool tracing = false;
    bool caching = true;
    std::string environment = "apartment/apartment.xml";
    std::string txVectorOutput = "example-sionna-tcp-minstrel-tx-vectors.csv";
    std::string rateChangeOutput = "example-sionna-tcp-minstrel-rate-changes.csv";
    int wifi_channel_num = 42;
    int sim_end_time_sec = 10;
    int channel_width = 80;
    int fixed_coherence_time_ms = 100;
    int data_Rate_mbps = 150;
    uint32_t seed = 1;
    double speed = 1.0;
    double txPowerDbm = 20.0;

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
    cmd.AddValue("txPower", "Transmit power in dBm", txPowerDbm);
    cmd.AddValue("txVectorOutput", "CSV file for transmitted WifiTxVector samples", txVectorOutput);
    cmd.AddValue("rateChangeOutput", "CSV file for Minstrel rate-change events", rateChangeOutput);
    cmd.Parse(argc, argv);

    if (verbose)
    {
        LogComponentEnable("ExampleSionnaTcpMinstrel", LOG_INFO);
        LogComponentEnable("SionnaPropagationDelayModel", LOG_INFO);
        LogComponentEnable("SionnaPropagationLossModel", LOG_INFO);
        LogComponentEnable("SionnaPropagationCache", LOG_INFO);
        LogComponentEnable("SionnaSpectrumPropagationLossModel", LOG_INFO);
    }

    std::cout << "TCP ns3sionna example with MinstrelHt rate adaptation"
              << std::endl << std::endl;

    g_txVectorLog.open(txVectorOutput);
    g_rateChangeLog.open(rateChangeOutput);

    g_txVectorLog << "Time_s,TxNodeId,TxDeviceId,SrcNodeId,DstNodeId,DstMac,SrcMac,"
                  << "PacketUid,Mode,Rate_bps,Rate_Mbps,ChannelFreq_MHz,"
                  << "ChannelWidth_MHz,GuardInterval_ns,Nss,IsAggregated,"
                  << "MpduType,MpduRefNumber,StaId\n";
    g_rateChangeLog << "Time_s,NodeId,DeviceId,OldRate_bps,NewRate_bps,NewRate_Mbps\n";

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
    spectrumPhy.Set("TxPowerStart", DoubleValue(txPowerDbm));
    spectrumPhy.Set("TxPowerEnd", DoubleValue(txPowerDbm));

    WifiMacHelper mac;
    Ssid ssid = Ssid("ns-3-ssid");

    WifiHelper wifi;
    WifiStandard wifi_standard = WIFI_STANDARD_80211ax;
    wifi.SetStandard(wifi_standard);
    wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager",
                                 "RtsCtsThreshold",
                                 UintegerValue(999999));

    std::string channelStr = "{" + std::to_string(wifi_channel_num) + ", " +
                             std::to_string(channel_width) + ", BAND_5GHZ, 0}";

    NetDeviceContainer staDevices;
    mac.SetType("ns3::StaWifiMac",
                "Ssid",
                SsidValue(ssid),
                "ActiveProbing",
                BooleanValue(true));
    spectrumPhy.Set("ChannelSettings", StringValue(channelStr));
    staDevices = wifi.Install(spectrumPhy, mac, wifiStaNode);

    NetDeviceContainer apDevices;
    mac.SetType("ns3::ApWifiMac",
                "Ssid",
                SsidValue(ssid),
                "BeaconGeneration",
                BooleanValue(true),
                "BeaconInterval",
                TimeValue(MicroSeconds(102400)),
                "EnableBeaconJitter",
                BooleanValue(false));
    spectrumPhy.Set("ChannelSettings", StringValue(channelStr));
    apDevices = wifi.Install(spectrumPhy, mac, wifiApNode);

    RegisterWifiDeviceMacs(staDevices);
    RegisterWifiDeviceMacs(apDevices);

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

    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/MonitorSnifferTx",
                    MakeCallback(&MinstrelTxVectorTrace));
    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/RemoteStationManager/$ns3::MinstrelHtWifiManager/Rate",
                    MakeCallback(&MinstrelRateChangeTrace));

    if (tracing)
    {
        std::cout << "Writing pcap files ..." << std::endl;
        spectrumPhy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        spectrumPhy.EnablePcap("example-sionna-tcp-minstrel", apDevices.Get(0));
        spectrumPhy.EnablePcap("example-sionna-tcp-minstrel", staDevices.Get(0));
    }

    Simulator::Stop(Seconds(sim_end_time_sec));

    sionnaHelper.Start();
    Simulator::Run();
    Simulator::Destroy();

    propagationCache->PrintStats();
    sionnaHelper.Destroy();

    g_txVectorLog.close();
    g_rateChangeLog.close();

    std::cout << "Minstrel TX-vector log can be found in: " << txVectorOutput << std::endl;
    std::cout << "Minstrel rate-change log can be found in: " << rateChangeOutput << std::endl;

    return 0;
}
