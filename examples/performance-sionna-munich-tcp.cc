/*
* Copyright (c) 2024 Yannik Pilz, Zubow, Jan
*
* SPDX-License-Identifier: GPL-2.0-only
*
* Author: Yannik Pilz <y.pilz@campus.tu-berlin.de>
*/

#include <ostream>

#include "ns3/sionna-helper.h"
#include "ns3/sionna-mobility-model.h"

#include "ns3/sionna-propagation-cache.h"
#include "ns3/sionna-propagation-delay-model.h"
#include "ns3/sionna-propagation-loss-model.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/cfr-tag.h"
#include "ns3/spectrum-module.h"
#include "ns3/spectrum-wifi-helper.h"

#include "ns3/packet-sink-helper.h"
#include "ns3/packet-sink.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/ssid.h"

#include "ns3/wifi-net-device.h"
#include "ns3/tcp-westwood-plus.h"
#include "ns3/minstrel-wifi-manager.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("PerformanceSionnaTCP");

Ptr<PacketSink> sink;
uint64_t lastTotalRx10 = 0;
uint64_t lastTotalRx100 = 0;

// std::ofstream ofs_tp("throughput_results.csv"); // This name is also used for pathloss
std::ofstream ofs_throughput("throughput_results.csv");
std::ofstream ofs_snr("snr_results.csv");

double TX_POWER_DBM = 30.0;
static constexpr double NOISE_FIGURE_DB = 7.0;  // typical WiFi NF
static constexpr double NOISE_FLOOR_DBM = -174.0 + 10.0 * std::log10(80e6) + NOISE_FIGURE_DB;

// mapping of IPv4 addr to nodeIds (needed to look up transmitter position)
std::map<Ipv4Address, uint32_t> g_ipToNodeIdMap;

void
BuildIpToNodeIdMap()
{
    g_ipToNodeIdMap.clear();
    for (uint32_t i = 0; i < NodeList::GetNNodes(); ++i)
    {
        Ptr<Node> node = NodeList::GetNode(i);
        Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
        if (ipv4)
        {
            for (uint32_t j = 0; j < ipv4->GetNInterfaces(); ++j)
            {
                for (uint32_t k = 0; k < ipv4->GetNAddresses(j); ++k)
                {
                    Ipv4Address addr = ipv4->GetAddress(j, k).GetLocal();
                    if (!addr.IsLocalhost())
                        g_ipToNodeIdMap[addr] = node->GetId();
                }
            }
        }
    }
}

uint32_t
GetNodeIdFromIpv4Address(Ipv4Address targetAddr)
{
    auto it = g_ipToNodeIdMap.find(targetAddr);
    if (it != g_ipToNodeIdMap.end())
        return it->second;
    NS_LOG_WARN("No node found for IPv4 address " << targetAddr);
    return 0xFFFFFFFF;
}

/**
 * L3 RX callback — hooks Ipv4L3Protocol/Rx to capture CFRTag before TCP reassembly
 * can strip packet tags. Derives SNR from pathloss and TX power and logs:
 * time, STA node ID, STA position, pathloss (dB), RX power (dBm), SNR (dB).
 */
void
RxTraceWithAddresses(std::string context,
                     Ptr<const Packet> packet,
                     Ptr<Ipv4> ipv4,
                     uint32_t interface)
{
    // Extract source IP by peeking the IP header (packet still has headers at L3)
    Ipv4Header ipHeader;
    packet->PeekHeader(ipHeader);
    Ipv4Address srcAddr = ipHeader.GetSource();

    uint32_t srcNodeId = GetNodeIdFromIpv4Address(srcAddr);
    if (srcNodeId == 0xFFFFFFFF)
        return;

    Ptr<Node> srcNode = NodeList::GetNode(srcNodeId);
    Ptr<MobilityModel> mob = srcNode->GetObject<MobilityModel>();
    Vector pos = mob ? mob->GetPosition() : Vector(0, 0, 0);

    CFRTag tag;
    if (packet->PeekPacketTag(tag))
    {
        double pathLossDb = tag.GetPathloss();
        double rxPowerDbm  = TX_POWER_DBM - pathLossDb;
        double snrDb       = rxPowerDbm - NOISE_FLOOR_DBM;

        ofs_snr << Simulator::Now().GetSeconds()
                << "," << srcNodeId
                << "," << pos.x << "," << pos.y << "," << pos.z
                << "," << pathLossDb
                << "," << rxPowerDbm
                << "," << snrDb
                << "\n";
    }
}

/**
 * Calculate the throughput and shamelessly stolen
 */
void
CalculateThroughput10()
{
    Time now = Simulator::Now(); /* Return the simulator's virtual time. */
    double cur = (sink->GetTotalRx() - lastTotalRx10) * 8.0 /
                 1e4; /* Convert Application RX Packets to MBits. */
    // std::cout << now.GetSeconds() << "s: \t" << cur << " Mbit/s" << std::endl;
    lastTotalRx10 = sink->GetTotalRx();

    ofs_throughput << Simulator::Now().GetSeconds() << "," << cur << ",10\n";

    Simulator::Schedule(MilliSeconds(10), &CalculateThroughput10);
}

void
CalculateThroughput100()
{
    Time now = Simulator::Now(); /* Return the simulator's virtual time. */
    double cur = (sink->GetTotalRx() - lastTotalRx100) * 8.0 /
                 1e5; /* Convert Application RX Packets to MBits. */
    std::cout << now.GetSeconds() << "s: \t" << cur << " Mbit/s" << std::endl;
    lastTotalRx100 = sink->GetTotalRx();

    ofs_throughput << Simulator::Now().GetSeconds() << "," << cur << ",100\n";

    Simulator::Schedule(MilliSeconds(100), &CalculateThroughput100);
}

double
RunSimulation(  const std::string environment,
                const uint32_t numStas,
                const bool mobile_scenario,
                const double mobile_speed,
                const bool caching,
                const int mode,
                const int sub_mode,
                const bool verbose,
                const int min_coherence_time_ms,
                const int data_Rate_mbps,
                const int seed)
{
    // Wifi config
    int wifi_channel_num = 42; // center at 5200, set to 40
    int channel_width = 80; //set to 20
    // Set the seed
    RngSeedManager::SetSeed(seed);
    // RngSeedManager::SetRun(run);
    // TODO DONE: check run number with setrun
    // SetSeed and SetRun do a very similar thing, but setrun is supposed to be cleaner and make
    // documentation easier. SetRun implies multiple repetitions of an experiment.

    SionnaHelper sionnaHelper(environment, "tcp://localhost:5555");

    // Create Station and Access Points, one access point and variable Stations
    NodeContainer wifiStaNodes;
    wifiStaNodes.Create(numStas);

    NodeContainer wifiApNode;
    wifiApNode.Create(1);

    // What does this do?
    Config::Set("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/ChannelWidth",
                UintegerValue(channel_width));

    Ptr<SionnaPropagationCache> propagationCache =
        CreateObject<SionnaPropagationCache>();
    propagationCache->SetSionnaHelper(sionnaHelper);
    propagationCache->SetCaching(caching);

    // new Channel thingy
    Ptr<MultiModelSpectrumChannel> spectrumChannel =
        CreateObject<MultiModelSpectrumChannel>();

    Ptr<SionnaPropagationLossModel> lossModel =
        CreateObject<SionnaPropagationLossModel>();
    lossModel->SetPropagationCache(propagationCache);

    spectrumChannel->AddPropagationLossModel(lossModel);

    // SISO only
    Ptr<SionnaSpectrumPropagationLossModel> spectrumLossModel =
        CreateObject<SionnaSpectrumPropagationLossModel>();
    spectrumLossModel->SetPropagationCache(propagationCache);

    spectrumChannel->AddSpectrumPropagationLossModel(spectrumLossModel);

    Ptr<SionnaPropagationDelayModel> delayModel =
        CreateObject<SionnaPropagationDelayModel>();
    delayModel->SetPropagationCache(propagationCache);

    spectrumChannel->SetPropagationDelayModel(delayModel);

    // Yet more new Stuff with Spectrum Phy
    SpectrumWifiPhyHelper spectrumPhy;
    spectrumPhy.SetChannel(spectrumChannel);
    spectrumPhy.SetErrorRateModel("ns3::NistErrorRateModel");
    spectrumPhy.Set("TxPowerStart", DoubleValue(TX_POWER_DBM));
    spectrumPhy.Set("TxPowerEnd", DoubleValue(TX_POWER_DBM));

    // Wifi and MAC
    WifiMacHelper mac;
    Ssid ssid = Ssid("ns-3-ssid");

    WifiHelper wifi;

    // WifiStandard wifi_standard = WIFI_STANDARD_80211g;
    WifiStandard wifi_standard = WIFI_STANDARD_80211ax;
    wifi.SetStandard(wifi_standard);

    std::string channelStr = "{" + std::to_string(wifi_channel_num) + ", " +
                             std::to_string(channel_width) + ", BAND_5GHZ, 0}";
    spectrumPhy.Set("ChannelSettings", StringValue(channelStr));

    std::string wifiManager("MinstrelHt");
    uint32_t rtsThreshold = 999999; // disabled even for large A-MPDU
    wifi.SetRemoteStationManager("ns3::" + wifiManager + "WifiManager",
                                 "RtsCtsThreshold",
                                 UintegerValue(rtsThreshold));

    // TCP Setup
    DataRate dataRate(std::to_string(data_Rate_mbps) + "Mb/s");
    //std::string tcpVariant{"ns3::TcpCubic"};
    std::string tcpVariant{"ns3::TcpCubic"};

    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
        TypeIdValue(TypeId::LookupByName(tcpVariant)));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1472));

    NetDeviceContainer staDevices;
    mac.SetType("ns3::StaWifiMac",
                "Ssid",
                SsidValue(ssid),
                "ActiveProbing",
                BooleanValue(false));
    spectrumPhy.Set("ChannelSettings", StringValue(channelStr));
    staDevices = wifi.Install(spectrumPhy, mac, wifiStaNodes);

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

    if (mobile_scenario)
    {
        // static AP
        mobility.SetMobilityModel("ns3::SionnaMobilityModel");
        mobility.Install(wifiApNode);
        // mobile STA
        mobility.SetMobilityModel("ns3::SionnaMobilityModel",
                                  "Model",
                                  EnumValue(SionnaMobilityModel::MODEL_RANDOM_WALK),
                                  "Speed",
                                  StringValue("ns3::ConstantRandomVariable[Constant=" +
                                              std::to_string(mobile_speed) + "]"),
                                  "Wall",
                                  BooleanValue(true));
        mobility.Install(wifiStaNodes);
    }
    else
    {
        // all nodes are static
        mobility.SetMobilityModel("ns3::SionnaMobilityModel");
        mobility.Install(wifiStaNodes);
        mobility.Install(wifiApNode);
    }


    // random placement of all nodes
    // wifiApNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(1.0, 2.0, 1.0));

    Ptr<UniformRandomVariable> randX = CreateObject<UniformRandomVariable>();
    Ptr<UniformRandomVariable> randY = CreateObject<UniformRandomVariable>();
    randX->SetAttribute("Min", DoubleValue(0.1));
    randX->SetAttribute("Max", DoubleValue(5.9));
    randY->SetAttribute("Min", DoubleValue(0.1));
    randY->SetAttribute("Max", DoubleValue(3.9));

    //    wifiStaNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(45.0, 90.0, 1.5));
    //    wifiApNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(8.5, 21.0, 27.0));

    // For Munich setting:
    // Yes, the chosen numbers are as of yet arbitrary.
    if (environment == "munich/munich.xml") {
        std::cout << "Munich executed" << std::endl;

        wifiApNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(8.5, 21.0, 27.0));
        // TODO: Somehow, the throughput is not as expected.
        // Speed has a huge influence on the munich scenario. High speed means almost no signal.
        /*
        randX->SetAttribute("Min", DoubleValue(0.0));
        randX->SetAttribute("Max", DoubleValue(80.0));
        randY->SetAttribute("Min", DoubleValue(50.0));
        randY->SetAttribute("Max", DoubleValue(110.0));
        */
    }

    for (uint32_t i = 0; i < numStas; i++)
    {
        //wifiStaNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(
        //    Vector(randX->GetValue(), randY->GetValue(), 1.0));
        // TODO: Maybe make this random. Currently set position.
        // TODO: This distance is too far. Around 82m, no signal is received.
        std::cout << "Set Position CORRECTLY!!!" << std::endl;
        wifiStaNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(
            Vector(45.0, 90.0, 1.5));
    }

    // configure IP
    InternetStackHelper stack;
    stack.Install(wifiApNode);
    stack.Install(wifiStaNodes);

    Ipv4AddressHelper address;

    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer wifiStaInterfaces = address.Assign(staDevices);
    Ipv4InterfaceContainer wifiApInterfaces = address.Assign(apDevices);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    /* Install TCP Receiver on the access point*/
    PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), 9));
    ApplicationContainer clientApps = sinkHelper.Install(wifiApNode);
    sink = StaticCast<PacketSink>(clientApps.Get(0));

    /* Install TCP/UDP Transmitter on the station */
    OnOffHelper server("ns3::TcpSocketFactory", (InetSocketAddress(wifiApInterfaces.GetAddress(0), 9)));
    server.SetAttribute("PacketSize", UintegerValue(1472));
    server.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    server.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    server.SetAttribute("DataRate", DataRateValue(DataRate(dataRate)));

    ApplicationContainer serverApps = server.Install(wifiStaNodes);

    // Start applications.
    clientApps.Start(Seconds(0.9));
    clientApps.Stop(Seconds(10.0));

    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(10.0));
    Simulator::Schedule(Seconds(1.01), &CalculateThroughput10);
    Simulator::Schedule(Seconds(1.1), &CalculateThroughput100);

    // set center frequency for Sionna
    double fc = get_center_freq(apDevices.Get(0));

    // set center frequency & bandwidth for Sionna
    // int min_coherence_time_ms = tc;
    sionnaHelper.Configure(fc,
                           channel_width,
                           getFFTSize(wifi_standard, channel_width),
                           getSubcarrierSpacing(wifi_standard),
                           min_coherence_time_ms);

    sionnaHelper.SetMode(mode);
    sionnaHelper.SetSubMode(sub_mode);

    Simulator::Stop(Seconds(10.0));

    auto startTime = std::chrono::system_clock::now();

    // Log for throughput:
    ofs_throughput << "Time,Throughput_Mbps,measuring interval in ms\n";

    // Build IP→NodeId map (must be done after IP assignment) and wire SNR callback
    BuildIpToNodeIdMap();
    ofs_snr << "Time_s,STA_NodeId,pos_x,pos_y,pos_z,PathLoss_dB,RxPower_dBm,SNR_dB\n";
    Config::Connect("/NodeList/*/$ns3::Ipv4L3Protocol/Rx",
                    MakeCallback(&RxTraceWithAddresses));

    sionnaHelper.Start();

    Simulator::Run();
    Simulator::Destroy();

    // propagationCache->PrintStats();

    sionnaHelper.Destroy();

    ofs_throughput.close();
    ofs_snr.close();

    auto endTime = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsedTime = endTime - startTime;
    double computationTime = elapsedTime.count();

    std::ofstream ofs_compTime("computationtime.csv", std::ios::app);

    ofs_compTime << min_coherence_time_ms << "," << computationTime << "," << mobile_speed << "," << seed << "\n";

    ofs_compTime.close();

    std::cout << "Finished simulation with " << numStas << " stations in " << computationTime
              << " sec";

    std::cout << std::endl << std::endl;
    return computationTime;
}

int
main(int argc, char* argv[])
{
   bool verbose = false;
   bool caching = true;
   // std::string environment = "simple_room/simple_room.xml";
   std::string environment = "munich/munich.xml";
   int wifi_channel_num = 40;
   bool mobile_scenario = false;
   double mobile_speed = 1.0;
   int sim_min_stas = 1;
   int sim_max_stas = 4;
   int mode = 3;
   int sub_mode = 16;
   int min_coherence_time_ms = 0;
   int data_Rate_mbps = 150;
   int seed = 1;

   CommandLine cmd(__FILE__);
   cmd.AddValue("channel", "The WiFi channel number", wifi_channel_num);
   cmd.AddValue("mobile_scenario", "Enable node movement", mobile_scenario);
   cmd.AddValue("mobile_speed", "STA speed when mobile_scenario is true", mobile_speed);
   cmd.AddValue("sim_min_stas", "Min number of STAs to be simulated", sim_min_stas);
   cmd.AddValue("sim_max_stas", "Max number of STAs to be simulated", sim_max_stas);
   cmd.AddValue("environment", "Xml file of Sionna environment", environment);
   cmd.AddValue("caching", "Enable caching of propagation delay and loss", caching);
   cmd.AddValue("mode", "The Sionna mode", mode);
   cmd.AddValue("sub_mode", "The Sionna submode", sub_mode);
   cmd.AddValue("verbose", "Enable logging", verbose);
   cmd.AddValue("minCoherenceTimeMs", "The minimal coherence time in msec", min_coherence_time_ms);
   cmd.AddValue("dataRate", "Data Rate in Mb/s", data_Rate_mbps);
   cmd.AddValue("seed", "Set the seed for the run", seed);
   cmd.AddValue("txPower", "Transmit power in dBm (applied to all nodes)", TX_POWER_DBM);

   cmd.Parse(argc, argv);

   if (verbose)
   {
       LogComponentEnable("SionnaPropagationEnforcedCache", LOG_LEVEL_DEBUG);
       LogComponentEnable("SionnaHelper", LOG_LEVEL_DEBUG);
       LogComponentEnable("SionnaPropagationDelayModel", LOG_INFO);
       LogComponentEnable("SionnaPropagationDelayModel", LOG_PREFIX_TIME);
   }

   std::cout << "Performance test: 1 AP and N STAs with ns3sionna" << std::endl;
   std::cout << "Config: ch " << wifi_channel_num << " mob " << mobile_scenario;
   std::cout << " speed " << mobile_speed;
   std::cout << " caching " << caching << " env " << environment;
   std::cout << " mode " << mode << " submode " << sub_mode << " txPower " << TX_POWER_DBM << " dBm" << std::endl;

   uint32_t numStas = sim_min_stas;
   double computationTime = 0.0;
   while (computationTime < 2 * 60 * 60 && numStas <= (uint32_t)sim_max_stas) // as long as a single run is below 2h
   {
       computationTime = RunSimulation(environment, numStas, mobile_scenario, mobile_speed, caching, mode,
            sub_mode, verbose, min_coherence_time_ms, data_Rate_mbps, seed);
       numStas = numStas * 2;
   }

   return 0;
}
