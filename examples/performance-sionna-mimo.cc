/* SPDX-License-Identifier: GPL-2.0-only */
// Configurable HE-SU TCP downlinks. Only interval summaries are written.

#include "ns3/ampdu-subframe-header.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/sionna-helper.h"
#include "ns3/sionna-mimo-helper.h"
#include "ns3/sionna-mobility-model.h"
#include "ns3/sionna-propagation-cache.h"
#include "ns3/sionna-propagation-delay-model.h"
#include "ns3/sionna-propagation-loss-model.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/spectrum-module.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/wifi-module.h"
#include "ns3/wifi-phy-state-helper.h"
#include "ns3/wifi-spectrum-signal-parameters.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ns3;

namespace
{
void
Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::invalid_argument(message);
    }
}

bool
StringOption(std::string* destination, const std::string& value)
{
    *destination = value;
    return true;
}

std::vector<Vector>
Positions(const std::string& text)
{
    std::vector<Vector> result;
    std::istringstream list(text);
    std::string entry;
    while (std::getline(list, entry, ';'))
    {
        std::istringstream point(entry);
        Vector v;
        char comma1, comma2;
        Require(bool(point >> v.x >> comma1 >> v.y >> comma2 >> v.z) &&
                    comma1 == ',' && comma2 == ',' &&
                    std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z),
                "positions must be semicolon-separated x,y,z triples");
        point >> std::ws;
        Require(point.eof(), "unexpected text after position triple");
        result.push_back(v);
    }
    return result;
}

// This example-local adapter selects ONE transmitted W[k] for the intended
// receiver, then uses that same W[k] for every bystander hearing the PPDU.
// Recomputing W for each bystander would incorrectly steer the beam at it.
// CSI is ideal/current; propagation-cache reuse still obeys the chosen policy.
class ExperimentSpectrumLoss : public SpectrumPropagationLossModel
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::ExperimentSpectrumLoss")
                                .SetParent<SpectrumPropagationLossModel>()
                                .AddConstructor<ExperimentSpectrumLoss>();
        return tid;
    }

    Ptr<SionnaSpectrumPropagationLossModel> model;
    Ptr<SionnaPropagationCache> cache;
    std::optional<MimoChannelMatrix> fixedChannel;
    bool beamforming{false};
    uint32_t streams{1};
    std::map<Mac48Address, Ptr<MobilityModel>> receivers;
    std::map<uint32_t, Ptr<MobilityModel>> peers;

  private:
    struct Selection
    {
        uint64_t uid;
        MimoChannelMatrix channel;
        MimoPrecodingMatrix matrix;
    };
    // Bounded to one most-recent PPDU per transmitter, not a packet history.
    mutable std::map<uint32_t, Selection> m_selections;

    Ptr<SpectrumValue> DoCalcRxPowerSpectralDensity(
        Ptr<const SpectrumSignalParameters> params,
        Ptr<const MobilityModel> a,
        Ptr<const MobilityModel> b) const override
    {
        const auto wifi = DynamicCast<const WifiSpectrumSignalParameters>(params);
        if (beamforming && wifi && wifi->ppdu &&
            wifi->ppdu->GetTxVector().GetPreambleType() == WIFI_PREAMBLE_HE_SU)
        {
            const auto txId = a->GetObject<Node>()->GetId();
            const auto uid = wifi->ppdu->GetUid();
            auto selected = m_selections.find(txId);
            if (selected == m_selections.end() || selected->second.uid != uid)
            {
                const auto psdu = wifi->ppdu->GetPsdu();
                Require(bool(psdu), "HE-SU transmission has no PSDU");
                const auto destination = receivers.find(psdu->GetAddr1());
                // HE broadcast frames have no single addressed receiver. Use
                // the paired peer consistently, including at overhearers.
                auto target = destination != receivers.end() ? destination->second : peers.at(txId);
                const auto channel = fixedChannel ? *fixedChannel
                                                  : cache->GetPropagationChannelMatrix(a, target);
                if (selected != m_selections.end() &&
                    selected->second.channel.GetCoefficients() == channel.GetCoefficients())
                {
                    // Reused CSI selects the same beam. Avoid repeating the
                    // per-tone SVD while the propagation snapshot is unchanged.
                    selected->second.uid = uid;
                }
                else
                {
                    auto matrix = MimoPrecodingMatrix::SingularValueDecomposition(
                                      channel, streams, MimoSvdPrecodingOptions{})
                                      .GetPrecodingMatrix();
                    selected = m_selections.insert_or_assign(txId, Selection{uid, channel, matrix}).first;
                }
            }
            model->SetPrecodingMatrix(selected->second.matrix);
        }
        return model->CalcRxPowerSpectralDensity(params, a, b);
    }
};

struct Link
{
    Ptr<PacketSink> sink;
    uint32_t apId;
    uint32_t staId;
    Mac48Address apAddress;
    Mac48Address staAddress;
    uint64_t lastBytes{0};
    double snrSum{0};
    uint64_t snrSamples{0};
};

class Reporter
{
  public:
    std::vector<Link> links;
    std::ofstream csv;
    Time start;
    Time stop;
    Time interval;
    Time last;

    void Begin()
    {
        last = start;
        for (auto& link : links)
        {
            link.lastBytes = link.sink->GetTotalRx();
            link.snrSum = 0;
            link.snrSamples = 0;
        }
        Simulator::Schedule(std::min(interval, stop - start), &Reporter::Report, this);
    }

    void AddSnr(uint32_t index, Ptr<const Packet> packet, double snr)
    {
        if (Simulator::Now() < start || Simulator::Now() > stop ||
            !std::isfinite(snr) || snr <= 0 || packet->GetSize() < 24)
        {
            return;
        }
        // RxOk/RxError provide the whole PSDU, possibly with an A-MPDU
        // delimiter. Inspect just its first header; count one sample per PSDU.
        Ptr<const Packet> mpdu = packet;
        AmpduSubframeHeader delimiter;
        packet->PeekHeader(delimiter);
        if (delimiter.IsSignatureValid() && delimiter.GetLength() >= 24 &&
            packet->GetSize() >= 4u + delimiter.GetLength())
        {
            mpdu = packet->CreateFragment(4, delimiter.GetLength());
        }
        WifiMacHeader header;
        mpdu->PeekHeader(header);
        auto& link = links.at(index);
        if (header.IsData() && header.GetAddr2() == link.apAddress &&
            header.GetAddr1() == link.staAddress)
        {
            link.snrSum += snr;
            ++link.snrSamples;
        }
    }

    void RxOk(uint32_t index, Ptr<const Packet> p, double snr, WifiMode, WifiPreamble)
    {
        AddSnr(index, p, snr);
    }
    void RxError(uint32_t index, Ptr<const Packet> p, double snr)
    {
        AddSnr(index, p, snr);
    }

    void Report()
    {
        const auto now = Simulator::Now();
        const double seconds = (now - last).GetSeconds();
        uint64_t totalBytes = 0, totalSamples = 0;
        double totalSnr = 0;
        for (uint32_t i = 0; i < links.size(); ++i)
        {
            auto& link = links[i];
            const auto bytes = link.sink->GetTotalRx();
            const auto delta = bytes - link.lastBytes;
            Row(now, seconds, i, link.apId, link.staId, delta, link.snrSum, link.snrSamples);
            totalBytes += delta;
            totalSamples += link.snrSamples;
            totalSnr += link.snrSum;
            link.lastBytes = bytes;
            link.snrSum = 0;
            link.snrSamples = 0;
        }
        Row(now, seconds, -1, -1, -1, totalBytes, totalSnr, totalSamples);
        csv.flush();
        last = now;
        if (now < stop)
        {
            Simulator::Schedule(std::min(interval, stop - now), &Reporter::Report, this);
        }
    }

  private:
    void Row(Time now, double seconds, int link, int ap, int sta,
             uint64_t bytes, double snrSum, uint64_t samples)
    {
        const double db = samples ? 10 * std::log10(snrSum / samples)
                                  : std::numeric_limits<double>::quiet_NaN();
        csv << now.GetSeconds() << ',' << seconds << ',' << link << ',' << ap << ',' << sta
            << ',' << bytes << ',' << bytes * 8.0 / seconds / 1e6
            << ',' << db << ',' << samples << '\n';
    }
};
} // namespace

int
main(int argc, char* argv[])
{
    std::string environment{"apartment/apartment.xml"};
    std::string serverUrl{"tcp://localhost:5555"};
    std::string channelSource{"sionna"};
    std::string outputDirectory{"results/mimo-manual"};
    std::string precoding{"open-loop"};
    std::string offeredRate{"1Gbps"};
    std::string tcpVariant{"ns3::TcpCubic"};
    std::string apPositions{"-1,0.5,1.5;2,0.5,1.5"};
    std::string staPositions{"-2,1.5,1.6;3,1.5,1.6"};
    double mobileSpeedMps{1}, txPowerDbm{20}, simulationTimeSeconds{10};
    double trafficStartSeconds{1}, reportIntervalMs{100}, noiseFigureDb{7};
    double antennaSpacingLambda{0.5}, fixedPathLossDb{50};
    double eesmBetaLinear{std::numeric_limits<double>::quiet_NaN()};
    uint32_t antennaCount{2}, arrayRows{1}, streams{0}, numPairs{2};
    uint32_t channelWidthMhz{20}, channelNumber{0}, mcs{11}, seed{1}, run{1};
    int fixedCoherenceTimeMs{0}, mode{1}, subMode{1};
    bool caching{true}, snr{true};
    CommandLine cmd(__FILE__);
    auto addString = [&cmd](const std::string& name, const std::string& help, std::string& value) {
        // ns-3.40's templated string parser truncates at whitespace.
        cmd.AddValue(name, help, MakeBoundCallback(&StringOption, &value), value);
    };
    addString("environment", "Sionna scene XML; positions must fit the scene", environment);
    addString("serverUrl", "Sionna server endpoint", serverUrl);
    addString("channelSource", "sionna or fixed (deterministic smoke test, no server)", channelSource);
    addString("outputDirectory", "Output directory; existing result files are rejected", outputDirectory);
    cmd.AddValue("numPairs", "Independent AP/STA TCP downlinks sharing one Wi-Fi channel", numPairs);
    addString("apPositions", "AP coordinates: x,y,z;x,y,z (one per pair)", apPositions);
    addString("staPositions", "STA initial coordinates: x,y,z;x,y,z (one per pair)", staPositions);
    cmd.AddValue("antennaCount", "Antennas at every AP and STA (1--8; e.g. 1, 2, 4)", antennaCount);
    cmd.AddValue("arrayRows", "Planar-array rows; must divide antennaCount", arrayRows);
    cmd.AddValue("antennaSpacingLambda", "Horizontal/vertical array spacing in wavelengths", antennaSpacingLambda);
    addString("precoding", "open-loop or svd (ideal current-CSI, equal-power beamforming)", precoding);
    cmd.AddValue("streams", "Spatial streams; 0 selects all antennas for open-loop, one for SVD", streams);
    cmd.AddValue("channelWidthMhz", "Channel width: 20, 40, or 80 MHz", channelWidthMhz);
    cmd.AddValue("channelNumber", "5 GHz channel number; 0 selects 36/38/42 for 20/40/80 MHz", channelNumber);
    cmd.AddValue("mobileSpeedMps", "STA random-walk speed; 0 keeps all nodes static", mobileSpeedMps);
    cmd.AddValue("txPowerDbm", "Total transmit power per device across all streams", txPowerDbm);
    cmd.AddValue("noiseFigureDb", "PHY/MIMO receiver noise figure in dB", noiseFigureDb);
    cmd.AddValue("simulationTimeSeconds", "Simulation end time", simulationTimeSeconds);
    cmd.AddValue("trafficStartSeconds", "TCP start after association warmup", trafficStartSeconds);
    cmd.AddValue("reportIntervalMs", "Throughput/SNR summary interval in ms", reportIntervalMs);
    cmd.AddValue("snr", "Aggregate downlink PHY SNR including decoded payload failures", snr);
    cmd.AddValue("mcs", "Fixed HE MCS (0--11)", mcs);
    cmd.AddValue("eesmBetaLinear", "Required explicit positive EESM beta for multi-antenna links", eesmBetaLinear);
    addString("offeredRate", "TCP offered load per AP", offeredRate);
    addString("tcpVariant", "TCP congestion-control TypeId", tcpVariant);
    cmd.AddValue("seed", "ns-3/Sionna random seed", seed);
    cmd.AddValue("run", "ns-3 RNG run number", run);
    cmd.AddValue("caching", "Enable propagation-cache reuse", caching);
    cmd.AddValue("fixedCoherenceTimeMs", "Fixed Sionna coherence time; 0 uses dynamic calculation", fixedCoherenceTimeMs);
    cmd.AddValue("mode", "Sionna IPC mode: 1=P2P, 2=P2MP, 3=lookahead", mode);
    cmd.AddValue("subMode", "Sionna lookahead depth", subMode);
    cmd.AddValue("fixedPathLossDb", "Uniform path loss for fixed-channel smoke tests only", fixedPathLossDb);
    cmd.Parse(argc, argv);

    try
    {
        Require(numPairs > 0 && numPairs <= 254, "numPairs must be in [1,254]");
        Require(antennaCount >= 1 && antennaCount <= 8 && arrayRows > 0 &&
                    antennaCount % arrayRows == 0, "invalid antenna count/array rows");
        Require(precoding == "open-loop" || precoding == "svd", "precoding must be open-loop or svd");
        Require(channelSource == "sionna" || channelSource == "fixed", "channelSource must be sionna or fixed");
        Require(channelWidthMhz == 20 || channelWidthMhz == 40 || channelWidthMhz == 80,
                "channelWidthMhz must be 20, 40, or 80");
        Require(mcs <= 11 && seed > 0 && run > 0, "invalid MCS, seed, or RNG run");
        Require(std::isfinite(mobileSpeedMps) && mobileSpeedMps >= 0 &&
                    std::isfinite(txPowerDbm) && std::isfinite(noiseFigureDb) && noiseFigureDb >= 0 &&
                    std::isfinite(fixedPathLossDb) && fixedPathLossDb >= 0,
                "invalid speed, power, noise figure, or fixed path loss");
        Require(std::isfinite(simulationTimeSeconds) && std::isfinite(trafficStartSeconds) &&
                    trafficStartSeconds >= 0.5 && simulationTimeSeconds > trafficStartSeconds &&
                    std::isfinite(reportIntervalMs) && reportIntervalMs > 0,
                "require end > start >= 0.5 s and a positive report interval");
        Require(fixedCoherenceTimeMs >= 0 && mode >= 1 && mode <= 3 && subMode > 0,
                "invalid coherence time or Sionna IPC mode/depth");
        Require(channelSource != "fixed" || mobileSpeedMps == 0,
                "fixed diagnostic channels require mobileSpeedMps=0");
        streams = streams ? streams : (precoding == "svd" ? 1 : antennaCount);
        Require(streams > 0 && streams <= antennaCount, "streams must be in [1,antennaCount]");
        Require(precoding != "open-loop" || streams == antennaCount,
                "open-loop transmits one stream per antenna; use svd for fewer streams");
        Require(precoding != "svd" || antennaCount > 1, "SVD beamforming requires at least two antennas");
        if (channelNumber == 0)
        {
            channelNumber = channelWidthMhz == 20 ? 36 : (channelWidthMhz == 40 ? 38 : 42);
        }
        auto apPoints = Positions(apPositions);
        auto staPoints = Positions(staPositions);
        Require(apPoints.size() == numPairs && staPoints.size() == numPairs,
                "provide exactly numPairs AP positions and STA positions");
        std::vector<Vector> allPoints = apPoints;
        allPoints.insert(allPoints.end(), staPoints.begin(), staPoints.end());
        for (uint32_t i = 0; i < allPoints.size(); ++i)
        {
            for (uint32_t j = i + 1; j < allPoints.size(); ++j)
            {
                Require(CalculateDistance(allPoints[i], allPoints[j]) > 0,
                        "all initial device positions must be distinct");
            }
        }
        const DataRate load(offeredRate);
        TypeId tcpType;
        Require(TypeId::LookupByNameFailSafe(tcpVariant, &tcpType) &&
                    tcpType.HasConstructor() &&
                    DynamicCast<TcpCongestionOps>(ObjectFactory(tcpVariant).Create<Object>()) != nullptr,
                "invalid TCP congestion-control TypeId");
        Require(load.GetBitRate() > 0, "offeredRate must be positive");
        const auto interval = MilliSeconds(reportIntervalMs);
        Require(interval.IsStrictlyPositive(), "report interval rounds to zero");
        const auto out = std::filesystem::path(outputDirectory);
        for (const auto* file : {"throughput_results.csv", "summary.csv", "config.txt"})
        {
            Require(!std::filesystem::exists(out / file), "output files already exist: " + out.string());
        }

        RngSeedManager::SetSeed(seed);
        RngSeedManager::SetRun(run);
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(tcpType));
        Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1472));
        SionnaHelper sionnaHelper(environment, serverUrl);
        sionnaHelper.SetMode(mode);
        sionnaHelper.SetSubMode(subMode);
        NodeContainer aps, stas;
        aps.Create(numPairs);
        stas.Create(numPairs);
        auto cache = CreateObject<SionnaPropagationCache>();
        cache->SetSionnaHelper(sionnaHelper);
        cache->SetCaching(caching);
        auto channel = CreateObject<MultiModelSpectrumChannel>();
        auto spectrumLoss = CreateObject<SionnaSpectrumPropagationLossModel>();
        auto adapter = CreateObject<ExperimentSpectrumLoss>();
        adapter->model = spectrumLoss;
        adapter->cache = cache;
        adapter->beamforming = precoding == "svd";
        adapter->streams = streams;
        const auto fft = getFFTSize(WIFI_STANDARD_80211ax, channelWidthMhz);
        const auto spectrumBins = fft * SionnaHelper::GUARD_MULTIPLIER;
        if (channelSource == "fixed")
        {
            MimoChannelMatrix matrix(spectrumBins, antennaCount, antennaCount);
            for (uint32_t sc = 0; sc < matrix.GetNumSubcarriers(); ++sc)
            {
                for (uint32_t ant = 0; ant < antennaCount; ++ant)
                {
                    matrix.Set(sc, ant, ant, {std::sqrt(double(antennaCount)), 0});
                }
            }
            adapter->fixedChannel = matrix;
            spectrumLoss->SetFixedChannelMatrix(matrix, fixedPathLossDb);
            auto loss = CreateObject<MatrixPropagationLossModel>();
            loss->SetDefaultLoss(fixedPathLossDb);
            channel->AddPropagationLossModel(loss);
            channel->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
        }
        else
        {
            spectrumLoss->SetPropagationCache(cache);
            auto pathLoss = CreateObject<SionnaPropagationLossModel>();
            pathLoss->SetPropagationCache(cache);
            channel->AddPropagationLossModel(pathLoss);
            auto delay = CreateObject<SionnaPropagationDelayModel>();
            delay->SetPropagationCache(cache);
            channel->SetPropagationDelayModel(delay);
        }
        channel->AddSpectrumPropagationLossModel(adapter);
        SpectrumWifiPhyHelper apPhy, staPhy;
        SionnaMimoConfiguration mimo;
        mimo.mode = antennaCount == 1 ? SionnaMimoMode::SISO
                                     : SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
        mimo.txArray.numRows = mimo.rxArray.numRows = arrayRows;
        mimo.txArray.numColumns = mimo.rxArray.numColumns = antennaCount / arrayRows;
        mimo.txArray.horizontalSpacing = mimo.rxArray.horizontalSpacing = antennaSpacingLambda;
        mimo.txArray.verticalSpacing = mimo.rxArray.verticalSpacing = antennaSpacingLambda;
        mimo.numTxAntennas = mimo.numRxAntennas = antennaCount;
        mimo.maxNss = streams;
        mimo.receiverNoiseFigureDb = noiseFigureDb;
        if (antennaCount > 1)
        {
            Require(std::isfinite(eesmBetaLinear) && eesmBetaLinear > 0,
                    "provide --eesmBetaLinear=<positive value calibrated for this PHY context>");
            mimo.eesmBetaLinear.emplace(mcs, eesmBetaLinear);
        }
        if (adapter->beamforming)
        {
            // Bootstrap the PHY dimension contract with a valid fixed matrix.
            // The adapter replaces this before every HE-SU propagation call.
            std::vector<std::complex<double>> identity(antennaCount * streams);
            for (uint32_t s = 0; s < streams; ++s)
            {
                identity[s * streams + s] = 1;
            }
            mimo.precodingMatrix = MimoPrecodingMatrix::FrequencyFlat(spectrumBins, antennaCount, streams, identity);
        }
        SionnaMimoHelper(mimo).ApplyForHeMcs(mcs, sionnaHelper, apPhy, staPhy, spectrumLoss);
        for (auto* phy : {&apPhy, &staPhy})
        {
            phy->SetChannel(channel);
            phy->SetErrorRateModel("ns3::TableBasedErrorRateModel");
            phy->Set("ChannelSettings", StringValue("{" + std::to_string(channelNumber) + ", " +
                                                    std::to_string(channelWidthMhz) + ", BAND_5GHZ, 0}"));
            phy->Set("TxPowerStart", DoubleValue(txPowerDbm));
            phy->Set("TxPowerEnd", DoubleValue(txPowerDbm));
        }
        WifiHelper wifi;
        wifi.SetStandard(WIFI_STANDARD_80211ax);
        wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
        wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                     "DataMode", StringValue("HeMcs" + std::to_string(mcs)),
                                     "ControlMode", StringValue("OfdmRate24Mbps"));
        MobilityHelper mobility;
        mobility.SetMobilityModel(channelSource == "fixed" ? "ns3::ConstantPositionMobilityModel"
                                                            : "ns3::SionnaMobilityModel");
        mobility.Install(aps);
        if (channelSource == "sionna" && mobileSpeedMps > 0)
        {
            mobility.SetMobilityModel("ns3::SionnaMobilityModel",
                                      "Model", EnumValue(SionnaMobilityModel::MODEL_RANDOM_WALK),
                                      "Speed", StringValue("ns3::ConstantRandomVariable[Constant=" +
                                                           std::to_string(mobileSpeedMps) + "]"),
                                      "Wall", BooleanValue(true));
        }
        mobility.Install(stas);
        mobility.AssignStreams(aps, 10);
        mobility.AssignStreams(stas, 1000);
        for (uint32_t i = 0; i < numPairs; ++i)
        {
            aps.Get(i)->GetObject<MobilityModel>()->SetPosition(apPoints[i]);
            stas.Get(i)->GetObject<MobilityModel>()->SetPosition(staPoints[i]);
        }
        InternetStackHelper internet;
        internet.Install(aps);
        internet.Install(stas);
        Reporter reporter;
        reporter.start = Seconds(trafficStartSeconds);
        reporter.stop = Seconds(simulationTimeSeconds);
        reporter.interval = interval;
        NetDeviceContainer allDevices;
        for (uint32_t i = 0; i < numPairs; ++i)
        {
            WifiMacHelper mac;
            const Ssid ssid("sionna-pair-" + std::to_string(i));
            mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(false));
            auto staDevices = wifi.Install(staPhy, mac, stas.Get(i));
            mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
            auto apDevices = wifi.Install(apPhy, mac, aps.Get(i));
            allDevices.Add(apDevices);
            allDevices.Add(staDevices);
            Ipv4AddressHelper addresses;
            addresses.SetBase(Ipv4Address(("10.1." + std::to_string(i) + ".0").c_str()),
                              Ipv4Mask("255.255.255.0"));
            addresses.Assign(apDevices);
            const auto staInterfaces = addresses.Assign(staDevices);
            const auto apDevice = DynamicCast<WifiNetDevice>(apDevices.Get(0));
            const auto staDevice = DynamicCast<WifiNetDevice>(staDevices.Get(0));
            adapter->receivers[apDevice->GetMac()->GetAddress()] = aps.Get(i)->GetObject<MobilityModel>();
            adapter->receivers[staDevice->GetMac()->GetAddress()] = stas.Get(i)->GetObject<MobilityModel>();
            adapter->peers[aps.Get(i)->GetId()] = stas.Get(i)->GetObject<MobilityModel>();
            adapter->peers[stas.Get(i)->GetId()] = aps.Get(i)->GetObject<MobilityModel>();
            PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                        InetSocketAddress(Ipv4Address::GetAny(), 9));
            const auto sinks = sinkHelper.Install(stas.Get(i));
            reporter.links.push_back({DynamicCast<PacketSink>(sinks.Get(0)),
                                      aps.Get(i)->GetId(), stas.Get(i)->GetId(),
                                      apDevice->GetMac()->GetAddress(), staDevice->GetMac()->GetAddress()});
            if (snr)
            {
                auto state = staDevice->GetPhy()->GetState();
                state->TraceConnectWithoutContext("RxOk", MakeCallback(&Reporter::RxOk, &reporter).Bind(i));
                state->TraceConnectWithoutContext("RxError", MakeCallback(&Reporter::RxError, &reporter).Bind(i));
            }
            OnOffHelper source("ns3::TcpSocketFactory",
                               InetSocketAddress(staInterfaces.GetAddress(0), 9));
            source.SetAttribute("PacketSize", UintegerValue(1472));
            source.SetAttribute("DataRate", DataRateValue(load));
            source.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
            source.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
            const auto sources = source.Install(aps.Get(i));
            sinks.Start(Seconds(0));
            sources.Start(reporter.start);
            sources.Stop(reporter.stop);
        }
        wifi.AssignStreams(allDevices, 10000);
        sionnaHelper.Configure(get_center_freq(allDevices.Get(0)), channelWidthMhz, fft,
                               getSubcarrierSpacing(WIFI_STANDARD_80211ax), fixedCoherenceTimeMs);
        std::filesystem::create_directories(out);
        std::ofstream config(out / "config.txt");
        config << std::setprecision(17)
               << "environment=" << environment << "\nserverUrl=" << serverUrl
               << "\nchannelSource=" << channelSource << "\nnumPairs=" << numPairs
               << "\napPositions=" << apPositions << "\nstaPositions=" << staPositions
               << "\nantennaCount=" << antennaCount << "\narrayRows=" << arrayRows
               << "\nantennaSpacingLambda=" << antennaSpacingLambda << "\nstreams=" << streams
               << "\nprecoding=" << precoding << "\nchannelWidthMhz=" << channelWidthMhz
               << "\nchannelNumber=" << channelNumber << "\nmcs=" << mcs
               << "\neesmBetaLinear=" << eesmBetaLinear << "\ntxPowerDbm=" << txPowerDbm
               << "\nnoiseFigureDb=" << noiseFigureDb << "\nmobileSpeedMps=" << mobileSpeedMps
               << "\nsimulationTimeSeconds=" << simulationTimeSeconds
               << "\ntrafficStartSeconds=" << trafficStartSeconds << "\nreportIntervalMs=" << reportIntervalMs
               << "\nofferedRate=" << offeredRate << "\ntcpVariant=" << tcpVariant
               << "\nseed=" << seed << "\nrun=" << run << "\ncaching=" << caching
               << "\nfixedCoherenceTimeMs=" << fixedCoherenceTimeMs << "\nmode=" << mode
               << "\nsubMode=" << subMode << "\nsnr=" << snr << "\nfixedPathLossDb=" << fixedPathLossDb
               << "\nsnrAverage=10*log10(mean(linear PHY SNR)), one sample per downlink data PSDU\n";
        config.close();
        reporter.csv.open(out / "throughput_results.csv");
        std::ofstream summary(out / "summary.csv");
        Require(bool(config) && bool(reporter.csv) && bool(summary), "cannot write output files");
        reporter.csv << std::setprecision(10)
                     << "time_s,interval_s,link_id,ap_node_id,sta_node_id,received_bytes,throughput_mbps,avg_snr_db,snr_samples\n";
        Simulator::Schedule(reporter.start, &Reporter::Begin, &reporter);
        // Allow the final report at the end of the measurement interval to run.
        Simulator::Stop(reporter.stop + NanoSeconds(1));
        if (channelSource == "sionna")
        {
            sionnaHelper.Start();
        }
        Simulator::Run();
        summary << "link_id,received_bytes,measurement_seconds,mean_goodput_mbps\n" << std::setprecision(10);
        const double duration = (reporter.stop - reporter.start).GetSeconds();
        uint64_t totalBytes = 0;
        for (uint32_t i = 0; i < reporter.links.size(); ++i)
        {
            const auto bytes = reporter.links[i].sink->GetTotalRx();
            totalBytes += bytes;
            summary << i << ',' << bytes << ',' << duration << ',' << bytes * 8.0 / duration / 1e6 << '\n';
        }
        summary << -1 << ',' << totalBytes << ',' << duration << ',' << totalBytes * 8.0 / duration / 1e6 << '\n';
        std::cout << "pairs=" << numPairs << " antennas=" << antennaCount << " streams=" << streams
                  << " precoding=" << precoding << " channel_width_mhz=" << channelWidthMhz
                  << " total_mean_goodput_mbps=" << totalBytes * 8.0 / duration / 1e6
                  << " output=" << out << std::endl;
        reporter.csv.close();
        summary.close();
        Require(bool(reporter.csv) && bool(summary), "error writing result files");
        Simulator::Destroy();
        if (channelSource == "sionna")
        {
            sionnaHelper.Destroy();
        }
    }
    catch (const std::exception& exception)
    {
        std::cerr << "experiment_error=" << exception.what() << std::endl;
        Simulator::Destroy();
        return 2;
    }
    return 0;
}
