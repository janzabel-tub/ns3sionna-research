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
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-module.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/ssid.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"

#include <cmath>
#include <fstream>

/**
 * Mobile apartment example with fixed ns3sionna coherence time and PHY SNR
 * logging. One AP sends UDP echo packets to one mobile STA. Successful PHY
 * receptions are written together with the receiver position.
 *
 * To run: ./example-sionna-fixedTc-mobile-sensing.sh
 */

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ExampleSionnaFixedTcMobileSensing");

std::string snrFname = "example-sionna-fixedTc-mobile-sensing-snr-position.csv";
std::ofstream ofs_snr(snrFname);

void
RxTraceWithAddresses(std::string context, Ptr<const Packet> packet, const Address& from, const Address& to)
{
    NS_LOG_INFO("*** " << Simulator::Now().GetSeconds() << "s [" << context
                       << "]: Server received packet of " << packet->GetSize()
                       << " bytes from " << InetSocketAddress::ConvertFrom(from).GetIpv4()
                       << " to " << InetSocketAddress::ConvertFrom(to).GetIpv4()
                       << ":" << InetSocketAddress::ConvertFrom(to).GetPort());
}

void
PhyRxOkTrace(std::string context, Ptr<const Packet> p, double snr, WifiMode mode, WifiPreamble preamble)
{
    double snrDb = 10 * std::log10(snr);
    uint32_t node_id = ContextToNodeId(context);

    Ptr<Node> node = NodeList::GetNode(node_id);
    Ptr<MobilityModel> mobility = node->GetObject<MobilityModel>();
    Vector pos = mobility->GetPosition();

    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s: PHY-RX-OK node="
                                              << node_id << " size=" << p->GetSize()
                                              << " snr=" << snrDb << "db, mode=" << mode
                                              << " preamble=" << preamble);

    ofs_snr << Simulator::Now().GetSeconds() << "," << node_id << "," << snrDb << ","
            << pos.x << "," << pos.y << "," << pos.z << std::endl;
    ofs_snr << std::flush;
}

int
main(int argc, char* argv[])
{
    bool verbose = true;
    bool tracing = true;
    bool caching = true;
    std::string environment = "apartment/apartment.xml";
    int wifi_channel_num = 42;
    int sim_end_time_sec = 10;
    int channel_width = 80;
    int min_coherence_time_ms = 100;
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
    cmd.AddValue("minCoherenceTimeMs", "The fixed coherence time in msec", min_coherence_time_ms);
    cmd.AddValue("seed", "Set the seed for the run", seed);
    cmd.AddValue("speed", "STA random-walk speed in m/s", speed);
    cmd.Parse(argc, argv);

    if (verbose)
    {
        LogComponentEnable("ExampleSionnaFixedTcMobileSensing", LOG_INFO);
        LogComponentEnable("UdpEchoClientApplication", LOG_LEVEL_INFO);
        LogComponentEnable("UdpEchoServerApplication", LOG_LEVEL_INFO);
        LogComponentEnable("SionnaPropagationDelayModel", LOG_INFO);
        LogComponentEnable("SionnaPropagationLossModel", LOG_INFO);
        LogComponentEnable("SionnaPropagationCache", LOG_INFO);
        LogComponentEnable("SionnaSpectrumPropagationLossModel", LOG_INFO);
    }

    std::cout << "Mobile ns3sionna example with fixed coherence time and SNR logging"
              << std::endl << std::endl;

    RngSeedManager::SetSeed(seed);
    SionnaHelper sionnaHelper(environment, "tcp://localhost:5555");

    // Create nodes
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
    spectrumPhy.Set("TxPowerStart", DoubleValue(20));
    spectrumPhy.Set("TxPowerEnd", DoubleValue(20));

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

    // Mobility configuration: fixed AP and mobile STA
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

    // Set up Internet stack and assign IP addresses
    InternetStackHelper stack;
    stack.Install(wifiApNode);
    stack.Install(wifiStaNode);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer wifiStaInterfaces = address.Assign(staDevices);
    address.Assign(apDevices);

    UdpEchoServerHelper echoServer(9);
    ApplicationContainer serverApps = echoServer.Install(wifiStaNode);
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(sim_end_time_sec));

    Config::Connect("/NodeList/*/ApplicationList/*/$ns3::UdpEchoServer/RxWithAddresses",
                    MakeCallback(&RxTraceWithAddresses));

    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/State/RxOk",
                    MakeCallback(&PhyRxOkTrace));

    UdpEchoClientHelper echoClient(wifiStaInterfaces.GetAddress(0), 9);
    echoClient.SetAttribute("MaxPackets", UintegerValue(1000));
    echoClient.SetAttribute("Interval", TimeValue(MilliSeconds(100)));
    echoClient.SetAttribute("PacketSize", UintegerValue(1024));

    ApplicationContainer clientApps = echoClient.Install(wifiApNode);
    clientApps.Start(Seconds(1.0));
    clientApps.Stop(Seconds(sim_end_time_sec));

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Set center frequency, bandwidth, FFT and fixed coherence time for Sionna.
    double fc = get_center_freq(apDevices.Get(0));
    sionnaHelper.Configure(fc,
                           channel_width,
                           getFFTSize(wifi_standard, channel_width),
                           getSubcarrierSpacing(wifi_standard),
                           min_coherence_time_ms);
    sionnaHelper.SetMode(SionnaHelper::MODE_P2P);

    if (tracing)
    {
        std::cout << "Writing pcap files ..." << std::endl;
        spectrumPhy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        spectrumPhy.EnablePcap("example-sionna-fixedTc-mobile-sensing", apDevices.Get(0));
        spectrumPhy.EnablePcap("example-sionna-fixedTc-mobile-sensing", staDevices.Get(0));
    }

    Simulator::Stop(Seconds(sim_end_time_sec));

    sionnaHelper.Start();
    Simulator::Run();
    Simulator::Destroy();

    propagationCache->PrintStats();
    sionnaHelper.Destroy();

    ofs_snr.close();
    std::cout << "SNR and position results can be found in: " << snrFname << std::endl;

    return 0;
}
