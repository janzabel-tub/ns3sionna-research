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

/**
 * Static apartment example showing how to set a fixed ns3sionna coherence time.
 * One AP sends UDP echo packets to one static STA. The fixed coherence time is
 * passed to SionnaHelper::Configure via minCoherenceTimeMs.
 *
 * To run: ./example-sionna-fixedTc.sh
 */

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ExampleSionnaFixedTc");

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
    int data_Rate_mbps = 150;
    uint32_t seed = 1;

    CommandLine cmd(__FILE__);
    cmd.AddValue("verbose", "Enable logging", verbose);
    cmd.AddValue("tracing", "Enable pcap tracing", tracing);
    cmd.AddValue("caching", "Enable caching of propagation delay and loss", caching);
    cmd.AddValue("environment", "Xml file of environment", environment);
    cmd.AddValue("channel", "The WiFi channel number", wifi_channel_num);
    cmd.AddValue("simEndTimeSec", "The total simulation time", sim_end_time_sec);
    cmd.AddValue("channelWidth", "The WiFi channel width in MHz", channel_width);
    cmd.AddValue("minCoherenceTimeMs", "The fixed coherence time in msec", min_coherence_time_ms);
    cmd.AddValue("dataRate", "Data Rate in Mb/s", data_Rate_mbps);
    cmd.AddValue("seed", "Set the seed for the run", seed);
    cmd.Parse(argc, argv);

    if (verbose)
    {
        LogComponentEnable("ExampleSionnaFixedTc", LOG_INFO);
        LogComponentEnable("SionnaPropagationDelayModel", LOG_INFO);
        LogComponentEnable("SionnaPropagationLossModel", LOG_INFO);
        LogComponentEnable("SionnaPropagationCache", LOG_INFO);
        LogComponentEnable("SionnaSpectrumPropagationLossModel", LOG_INFO);
    }

    std::cout << "Static ns3sionna example with fixed coherence time" << std::endl << std::endl;

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

    // Mobility configuration: fixed AP and fixed STA
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::SionnaMobilityModel");
    mobility.Install(wifiApNode);
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
        spectrumPhy.EnablePcap("example-sionna-fixedTc", apDevices.Get(0));
        spectrumPhy.EnablePcap("example-sionna-fixedTc", staDevices.Get(0));
    }

    Simulator::Stop(Seconds(sim_end_time_sec));

    sionnaHelper.Start();
    Simulator::Run();
    Simulator::Destroy();

    propagationCache->PrintStats();
    sionnaHelper.Destroy();

    return 0;
}
