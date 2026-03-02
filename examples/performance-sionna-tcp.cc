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
std::ofstream ofs_snr;      // --snr
std::ofstream ofs_position; // --positions

// ── SNR constants (match the Munich script; --txPower overrides TX_POWER_DBM) ──
double TX_POWER_DBM = 10.0;                          // default PHY TX power, dBm
static constexpr double NOISE_FIGURE_DB  = 7.0;      // typical WiFi noise figure
// Thermal noise floor for 80 MHz channel with the given noise figure
static const double NOISE_FLOOR_DBM =
    -174.0 + 10.0 * std::log10(80e6) + NOISE_FIGURE_DB; // Around -95dB

// ── MAC-address → Node-ID map (replaces IP map for PHY-level tracing) ──────
std::map<Mac48Address, uint32_t> g_macToNodeIdMap;

void
BuildMacToNodeIdMap()
{
    g_macToNodeIdMap.clear();
    for (uint32_t i = 0; i < NodeList::GetNNodes(); ++i)
    {
        Ptr<Node> node = NodeList::GetNode(i);
        for (uint32_t j = 0; j < node->GetNDevices(); ++j)
        {
            Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(node->GetDevice(j));
            if (wifiDev)
            {
                Mac48Address mac = Mac48Address::ConvertFrom(wifiDev->GetAddress());
                g_macToNodeIdMap[mac] = i;
            }
        }
    }
}

/**
 * Shared logging helper called by both RxOk and RxError PHY callbacks.
 *
 * @param packet     The received (or attempted) MPDU/PSDU
 * @param snrLinear  SNR as a linear power ratio (as provided by ns3 PHY state)
 * @param decoded    true = FCS passed, false = FCS failed / undecodable
 */
void
LogPhySnr(Ptr<const Packet> packet, double snrLinear, bool decoded)
{
    // Peek the 802.11 MAC header — Addr2 is always the transmitter address
    WifiMacHeader hdr;
    if (!packet->PeekHeader(hdr))
        return;

    Mac48Address srcMac = hdr.GetAddr2();
    auto it = g_macToNodeIdMap.find(srcMac);
    if (it == g_macToNodeIdMap.end())
        return;

    uint32_t srcNodeId = it->second;
    Ptr<Node> srcNode  = NodeList::GetNode(srcNodeId);
    Ptr<MobilityModel> mob = srcNode->GetObject<MobilityModel>();
    Vector pos = mob ? mob->GetPosition() : Vector(0, 0, 0);

    double snrDb = 10.0 * std::log10(snrLinear);

    if (ofs_snr.is_open())
    {
        ofs_snr << Simulator::Now().GetSeconds()
                << "," << srcNodeId
                << "," << pos.x << "," << pos.y << "," << pos.z
                << "," << snrDb
                << "," << (decoded ? 1 : 0)   // new column: 1=decoded, 0=dropped
                << "\n";
    }

    if (ofs_position.is_open())
    {
        ofs_position << Simulator::Now().GetSeconds()
                     << "," << srcNodeId
                     << "," << pos.x << "," << pos.y << "," << pos.z
                     << "\n";
    }
}

/**
 * PHY callback for successfully decoded frames.
 * Signature mandated by WifiPhyStateHelper::m_rxOkTrace.
 */
void
PhyRxOkTrace(std::string context,
             Ptr<const Packet> packet,
             double snr,
             WifiMode mode,
             WifiPreamble preamble)
{
    LogPhySnr(packet, snr, /*decoded=*/true);
}

/**
 * PHY callback for frames where reception was attempted but FCS failed.
 * Signature mandated by WifiPhyStateHelper::m_rxErrorTrace.
 * This is the NEW path that catches what the old L3 callback missed.
 */
void
PhyRxErrorTrace(std::string context,
                Ptr<const Packet> packet,
                double snr)
{
    LogPhySnr(packet, snr, /*decoded=*/false);
}

/*
 * Log the position of the stations
 *
 */
void
LogAllPositions(std::ofstream &ofs_position, const ns3::NodeContainer &nodes, uint32_t intervalMs)
{
    double now = ns3::Simulator::Now().GetSeconds();
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        ns3::Ptr<ns3::Node> node = nodes.Get(i);
        ns3::Ptr<ns3::MobilityModel> mob = node->GetObject<ns3::MobilityModel>();
        ns3::Vector pos = mob ? mob->GetPosition() : ns3::Vector(0.0, 0.0, 0.0);
        if (ofs_position.is_open())
        {
            // Format: Time_s, NodeId, pos_x, pos_y, pos_z
            ofs_position << now << "," << node->GetId() << "," << pos.x << "," << pos.y << "," << pos.z << "\n";
        }
    }
    // schedule next call
    ns3::Simulator::Schedule(ns3::MilliSeconds(intervalMs),
                             ns3::MakeBoundCallback(&LogAllPositions,
                                                    std::ref(ofs_position),
                                                    nodes,
                                                    intervalMs));
}

/**
 * Calculate the throughput
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
                const int seed,
                const bool enable_snr,
                const bool enable_positions)
{
    // Wifi config
    int wifi_channel_num = 42; // center at 5200, set to 40
    int channel_width = 80; //set to 20
    // Set the seed
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
    // Make the mobility independent of the rng seed manager and set a unique rng generator seed
    // The seed can be chosen arbitratily, but should be high
    mobilityHelper.AssignStreams(wifiStaNodes, 10000000);

    Ptr<UniformRandomVariable> randX = CreateObject<UniformRandomVariable>();
    Ptr<UniformRandomVariable> randY = CreateObject<UniformRandomVariable>();
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
    Ipv4InterfaceContainer wifiApInterfaces  = address.Assign(apDevices);

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
    // active. BuildIpToNodeIdMap() must run after IP assignment.
    if (enable_snr || enable_positions)
    {
        BuildMacToNodeIdMap();   // <-- MAC map, not IP map

        if (enable_snr)
            ofs_snr << "Time_s,STA_NodeId,pos_x,pos_y,pos_z,SNR_dB,Decoded\n";
                                                           // SNR_dB now comes
                                                           // directly from PHY,
                                                           // not from CFRTag math

        if (enable_positions)
        {
            // Open file (example path, change as required)
            std::ofstream ofs_positions;
            std::string positionFile = "position_results.csv";
            ofs_positions.open(positionFile.c_str());
            if (ofs_positions.is_open())
            {
                // write header
                ofs_positions << "Time_s,STA_NodeId,pos_x,pos_y,pos_z\n";
                // schedule initial call at t=0
                uint32_t logIntervalMs = 100; // choose your logging rate (e.g. 10 ms)
                ns3::Simulator::Schedule(ns3::MilliSeconds(0),
                                         ns3::MakeBoundCallback(&LogAllPositions,
                                                                std::ref(ofs_positions),
                                                                wifiStaNodes, // TODO: This can be made to include the AP
                                                                logIntervalMs));
            }
        }
        // ofs_position << "Time_s,STA_NodeId,pos_x,pos_y,pos_z\n";

        // Connect to PHY state machine on every WiFi device
        Config::Connect(
            "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/State/RxOk",
            MakeCallback(&PhyRxOkTrace));

        Config::Connect(
            "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/State/RxError",
            MakeCallback(&PhyRxErrorTrace));
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
    if (enable_positions || ofs_positions.is_open()) // Minor improvement maybe
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
       LogComponentEnable("SionnaPropagationEnforcedCache", LOG_LEVEL_DEBUG);
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
