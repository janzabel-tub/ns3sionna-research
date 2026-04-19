/*
* Copyright (c) 2024 Yannik Pilz, Zubow, Jan
*
* SPDX-License-Identifier: GPL-2.0-only
*
* Author: Yannik Pilz <y.pilz@campus.tu-berlin.de>
*/

#include <ostream>
#include <set>
#include <string>
#include <vector>

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

std::vector<Ptr<PacketSink>> g_sinks;
uint64_t lastTotalRx10 = 0;
uint64_t lastTotalRx100 = 0;

// std::ofstream ofs_tp("throughput_results.csv"); // This name is also used for pathloss
std::ofstream ofs_throughput("throughput_results.csv");
std::ofstream ofs_snr;      // --snr
std::ofstream ofs_position; // --positions

// ── SNR constants (match the Munich script; --txPower overrides TX_POWER_DBM) ──
double TX_POWER_DBM = 20.0;                          // default PHY TX power, dBm
static constexpr double NOISE_FIGURE_DB  = 7.0;      // typical WiFi noise figure
// Thermal noise floor for 80 MHz channel with the given noise figure
static const double NOISE_FLOOR_DBM =
    -174.0 + 10.0 * std::log10(80e6) + NOISE_FIGURE_DB; // Around -95dB

std::set<uint32_t> g_staNodeIds;
static constexpr uint32_t INVALID_NODE_ID = 0xFFFFFFFF;

void
RegisterStaNodes(const NodeContainer& wifiStaNodes)
{
    g_staNodeIds.clear();

    for (uint32_t i = 0; i < wifiStaNodes.GetN(); ++i)
    {
        g_staNodeIds.insert(wifiStaNodes.Get(i)->GetId());
    }
}

uint64_t
GetTotalRxBytes()
{
    uint64_t totalRx = 0;
    for (const auto& sink : g_sinks)
    {
        if (sink)
        {
            totalRx += sink->GetTotalRx();
        }
    }
    return totalRx;
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

/**
 * L3 RX callback — hooks Ipv4L3Protocol/Rx to capture CFRTag before TCP
 * reassembly can strip packet tags.
 *
 * When --snr is set: derives SNR from pathloss and TX power and logs it only
 *   for packets received by STA nodes. This keeps the CSV focused on the
 *   mobile-station side of the link instead of mixing in AP-side samples.
 *
 * When --positions is set: logs time, STA node ID and XYZ coordinates to
 *   position_results.csv for every Sionna-tagged packet received by a STA.
 *
 * Connected only when at least one of --snr or --positions is active.
 */
void
RxTraceWithAddresses(std::string context,
                     Ptr<const Packet> packet,
                     Ptr<Ipv4> ipv4,
                     uint32_t interface)
{
    (void) ipv4;
    (void) interface;

    const uint32_t rxNodeId = ExtractNodeIdFromContext(context);
    if (rxNodeId == INVALID_NODE_ID)
    {
        NS_LOG_WARN("Could not extract receiver node ID from context " << context);
        return;
    }

    if (g_staNodeIds.find(rxNodeId) == g_staNodeIds.end())
    {
        return;
    }

    Ptr<Node> rxNode = NodeList::GetNode(rxNodeId);
    Ptr<MobilityModel> mob = rxNode->GetObject<MobilityModel>();
    Vector pos = mob ? mob->GetPosition() : Vector(0, 0, 0);

    CFRTag tag;
    if (packet->PeekPacketTag(tag))
    {
        double pathLossDb = tag.GetPathloss();
        double rxPowerDbm = TX_POWER_DBM - pathLossDb;
        double snrDb      = rxPowerDbm - NOISE_FLOOR_DBM;

        if (ofs_snr.is_open())
        {
            ofs_snr << Simulator::Now().GetSeconds()
                    << "," << rxNodeId
                    << "," << pos.x << "," << pos.y << "," << pos.z
                    << "," << pathLossDb
                    << "," << rxPowerDbm
                    << "," << snrDb
                    << "\n";
        }

        // Position log: mirrors the ofs_tp output in example-sionna-sensing-mobile-tcp.cc
        // (time, node_id, x, y, z). node_id is added here to support multi-STA runs.
        if (ofs_position.is_open())
        {
            ofs_position << Simulator::Now().GetSeconds()
                         << "," << rxNodeId
                         << "," << pos.x << "," << pos.y << "," << pos.z
                         << "\n";
        }
    }
}

/**
 * Calculate the throughput
 */
void
CalculateThroughput10()
{
    Time now = Simulator::Now(); /* Return the simulator's virtual time. */
    const uint64_t totalRx = GetTotalRxBytes();
    double cur = (totalRx - lastTotalRx10) * 8.0 /
                 1e4; /* Convert Application RX Packets to MBits. */
    // std::cout << now.GetSeconds() << "s: \t" << cur << " Mbit/s" << std::endl;
    lastTotalRx10 = totalRx;

    ofs_throughput << now.GetSeconds() << "," << cur << ",10\n";

    Simulator::Schedule(MilliSeconds(10), &CalculateThroughput10);
}

void
CalculateThroughput100()
{
    Time now = Simulator::Now(); /* Return the simulator's virtual time. */
    const uint64_t totalRx = GetTotalRxBytes();
    double cur = (totalRx - lastTotalRx100) * 8.0 /
                 1e5; /* Convert Application RX Packets to MBits. */
    std::cout << now.GetSeconds() << "s: \t" << cur << " Mbit/s" << std::endl;
    lastTotalRx100 = totalRx;

    ofs_throughput << now.GetSeconds() << "," << cur << ",100\n";

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
                const int seed,
                const bool enable_snr,
                const bool enable_positions)
{
    g_sinks.clear();
    lastTotalRx10 = 0;
    lastTotalRx100 = 0;

    // Wifi config
    int wifi_channel_num = 42; // center at 5200, set to 40
    int channel_width = 80; //set to 20
    // Use a single seed value so ns-3 and the Python server share the same run seed.
    RngSeedManager::SetSeed(seed);

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

    SpectrumWifiPhyHelper spectrumPhy;
    spectrumPhy.SetChannel(spectrumChannel);
    spectrumPhy.SetErrorRateModel("ns3::NistErrorRateModel");    spectrumPhy.Set("TxPowerStart", DoubleValue(TX_POWER_DBM));
    spectrumPhy.Set("TxPowerEnd",   DoubleValue(TX_POWER_DBM));

    // Wifi and MAC
    WifiMacHelper mac;
    Ssid ssid = Ssid("ns-3-ssid");

    WifiHelper wifi;
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
    std::string tcpVariant{"ns3::TcpCubic"};

    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
        TypeIdValue(TypeId::LookupByName(tcpVariant)));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1472));

    NetDeviceContainer staDevices;
    mac.SetType("ns3::StaWifiMac",
                "Ssid",        SsidValue(ssid),
                "ActiveProbing", BooleanValue(true));
    spectrumPhy.Set("ChannelSettings", StringValue(channelStr));
    staDevices = wifi.Install(spectrumPhy, mac, wifiStaNodes);

    NetDeviceContainer apDevices;
    mac.SetType("ns3::ApWifiMac",
                "Ssid",              SsidValue(ssid),
                "BeaconGeneration",  BooleanValue(true),
                "BeaconInterval",    TimeValue(MicroSeconds(102400)),
                "EnableBeaconJitter", BooleanValue(false));
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

    // Independent random stream for mobility
    int64_t stream = 1000000;

    // Assign streams
    int64_t used = 0;
    used = mobility.AssignStreams(wifiApNode, stream);
    stream += used;

    used = mobility.AssignStreams(wifiStaNodes, stream);
    stream += used;

    Ptr<UniformRandomVariable> randX = CreateObject<UniformRandomVariable>();
    Ptr<UniformRandomVariable> randY = CreateObject<UniformRandomVariable>();
    randX->SetStream(stream++);
    randY->SetStream(stream++);
    randX->SetAttribute("Min", DoubleValue(0.1));
    randX->SetAttribute("Max", DoubleValue(5.9));
    randY->SetAttribute("Min", DoubleValue(0.1));
    randY->SetAttribute("Max", DoubleValue(3.9));

    // For Apartment Setting
    if (environment == "apartment/apartment.xml")
    {
        std::cout << "Apartment executed" << std::endl;

        wifiApNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(-1,0.5,1.5));

        // Create a random variable to choose the zone and generate coordinates
        Ptr<UniformRandomVariable> randVar = CreateObject<UniformRandomVariable>();
        randVar->SetStream(stream++);

        for (uint32_t i = 0; i < numStas; i++)
        {
            // Randomly choose a zone: 0, 1, or 2
            int zone = static_cast<int>(randVar->GetValue(0, 3));
            double posX, posY, posZ;

            if (zone == 0)
            {
                // Same room as AP
                posX = randVar->GetValue(-2.9, -0.7);
                posY = randVar->GetValue(0.5, 3.0);
                posZ = 1.6;
            }
            else if (zone == 1)
            {
                // Room with door to AP
                posX = randVar->GetValue(-5.0, -4.0);
                posY = 3.0;
                posZ = 1.6;
            }
            else
            {
                // Room next to AP, no direct door
                posX = -4.5;
                posY = randVar->GetValue(0.6, 2.0);
                posZ = 1.6;
            }

            wifiStaNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(
                Vector(posX, posY, posZ));
        }
    }
    else
    {
        wifiApNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(1.0, 2.0, 1.0));
        for (uint32_t i = 0; i < numStas; i++)
        {
            wifiStaNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(
                Vector(randX->GetValue(), randY->GetValue(), 1.0));
        }
    }

    // configure IP
    InternetStackHelper stack;
    stack.Install(wifiApNode);
    stack.Install(wifiStaNodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer wifiStaInterfaces = address.Assign(staDevices);
    address.Assign(apDevices);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    /* Install TCP receivers on all STAs */
    PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), 9));
    ApplicationContainer clientApps;
    for (uint32_t i = 0; i < numStas; ++i)
    {
        ApplicationContainer staSinkApps = sinkHelper.Install(wifiStaNodes.Get(i));
        clientApps.Add(staSinkApps);
        g_sinks.push_back(StaticCast<PacketSink>(staSinkApps.Get(0)));
    }

    /* Install one downstream TCP transmitter on the AP per STA */
    ApplicationContainer serverApps;
    for (uint32_t i = 0; i < numStas; ++i)
    {
        OnOffHelper server("ns3::TcpSocketFactory",
                           InetSocketAddress(wifiStaInterfaces.GetAddress(i), 9));
        server.SetAttribute("PacketSize", UintegerValue(1472));
        server.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        server.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        server.SetAttribute("DataRate", DataRateValue(DataRate(dataRate)));
        serverApps.Add(server.Install(wifiApNode.Get(0)));
    }

    // Start applications.
    clientApps.Start(Seconds(0.9));
    clientApps.Stop(Seconds(10.0));

    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(10.0));
    Simulator::Schedule(Seconds(1.01), &CalculateThroughput10);
    Simulator::Schedule(Seconds(1.1), &CalculateThroughput100);

    // set center frequency for Sionna
    double fc = get_center_freq(apDevices.Get(0));
    sionnaHelper.Configure(fc,
                           channel_width,
                           getFFTSize(wifi_standard, channel_width),
                           getSubcarrierSpacing(wifi_standard),
                           min_coherence_time_ms);
    sionnaHelper.SetMode(mode);
    sionnaHelper.SetSubMode(sub_mode);

    Simulator::Stop(Seconds(10.0));

    // SNR and position (Position implemented because apartment has some issues)
    // The same L3 Rx callback serves both features; connect it when either is
    // active. Node roles must be registered after the node containers exist.
    if (enable_snr || enable_positions)
    {
        RegisterStaNodes(wifiStaNodes);

        if (enable_snr)
            ofs_snr << "Time_s,STA_NodeId,pos_x,pos_y,pos_z,PathLoss_dB,RxPower_dBm,SNR_dB\n";

        if (enable_positions)
            ofs_position << "Time_s,STA_NodeId,pos_x,pos_y,pos_z\n";

        Config::Connect("/NodeList/*/$ns3::Ipv4L3Protocol/Rx",
                        MakeCallback(&RxTraceWithAddresses));
    }

    // Log for throughput:
    ofs_throughput << "Time,Throughput_Mbps,measuring interval in ms\n";

    auto startTime = std::chrono::system_clock::now();

    sionnaHelper.Start();
    Simulator::Run();
    Simulator::Destroy();

    // propagationCache->PrintStats();

    sionnaHelper.Destroy();

    ofs_throughput.close();
    if (enable_snr)
        ofs_snr.close();
    if (enable_positions)
        ofs_position.close();

    auto endTime = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsedTime = endTime - startTime;
    double computationTime = elapsedTime.count();

    std::ofstream ofs_compTime("computationtime.csv", std::ios::app);

    ofs_compTime << min_coherence_time_ms << "," << computationTime << "," << mobile_speed << "," << seed << "\n";
    ofs_compTime.close();

    std::cout << "Finished simulation with " << numStas << " stations in "
              << computationTime << " sec" << std::endl << std::endl;

    return computationTime;
}

int
main(int argc, char* argv[])
{
   bool verbose = false;
   bool caching = true;
   //std::string environment = "simple_room/simple_room.xml";
   std::string environment = "apartment/apartment.xml";
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
   bool enable_snr = false;
   bool enable_positions = false;

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
   cmd.AddValue("snr", "Enable per-packet SNR logging to snr_results.csv", enable_snr);
   cmd.AddValue("positions", "Enable per-packet position logging to position_results.csv", enable_positions);
   cmd.AddValue("txPower", "Transmit power in dBm (used by PHY and SNR math)", TX_POWER_DBM);

   cmd.Parse(argc, argv);

   if (enable_snr)
       ofs_snr.open("snr_results.csv");

   if (enable_positions)
   {
       ofs_position.open("position_results.csv");
       std::cout << "Position logging enabled → position_results.csv" << std::endl;
       std::cout << "  (compatible with plot3d_mobile_csi.py as the position argument)" << std::endl;
   }

   if (verbose)
   {
       LogComponentEnable("SionnaHelper", LOG_LEVEL_DEBUG);
       LogComponentEnable("SionnaPropagationDelayModel", LOG_INFO);
       LogComponentEnable("SionnaPropagationDelayModel", LOG_PREFIX_TIME);
   }

   std::cout << "Performance test: 1 AP and N STAs with ns3sionna" << std::endl;
   std::cout << "Config: ch " << wifi_channel_num << " mob " << mobile_scenario;
   std::cout << " speed " << mobile_speed;
   std::cout << " caching " << caching << " env " << environment;
   std::cout << " mode " << mode << " submode " << sub_mode << std::endl;
   std::cout << " txPower " << TX_POWER_DBM << " dBm";
   std::cout << " snr " << enable_snr << " positions " << enable_positions << std::endl;

   uint32_t numStas = sim_min_stas;
   double computationTime = 0.0;
   while (computationTime < 2 * 60 * 60 && numStas <= (uint32_t)sim_max_stas)
   {
       computationTime = RunSimulation(environment, numStas, mobile_scenario,
                                       mobile_speed, caching, mode, sub_mode,
                                       verbose, min_coherence_time_ms,
                                       data_Rate_mbps, seed, enable_snr,
                                       enable_positions);
       numStas = numStas * 2;
   }

   return 0;
}
