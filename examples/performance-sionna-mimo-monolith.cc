/*
* Copyright (c) 2024 Yannik Pilz, Zubow, Jan
*
* SPDX-License-Identifier: GPL-2.0-only
*
* Author: Yannik Pilz <y.pilz@campus.tu-berlin.de>
*/

// One AP, N contending stations, one TCP downlink per station.
// Adapted from performance-sionna-tcp.cc; optional packet traces remain opt-in.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <stdexcept>
#include <limits>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ns3/sionna-helper.h"
#include "ns3/sionna-mimo-helper.h"
#include "ns3/ampdu-subframe-header.h"
#include "ns3/wifi-module.h"
#include "ns3/wifi-spectrum-signal-parameters.h"
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

#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"
#include "ns3/wifi-tx-vector.h"
#include "ns3/tcp-westwood-plus.h"
#include "ns3/minstrel-wifi-manager.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("PerformanceSionnaMimoMonolith");

struct Options
{
    std::string outputDirectory{"results/mimo-monolith"};
    std::string serverUrl{"tcp://localhost:5555"};
    std::string precoding{"open-loop"};
    std::string tcpVariant{"ns3::TcpCubic"};
    std::string apPosition;
    std::string staPositions;
    uint32_t antennaCount{2}, arrayRows{1}, streams{0};
    uint32_t channelWidthMhz{20}, channelNumber{0}, run{1};
    double antennaSpacingLambda{0.5}, noiseFigureDb{7};
    double simulationTimeSeconds{10}, trafficStartSeconds{1}, reportIntervalMs{100};
    bool pcap{false}, mimoTrace{false}, prtTput{false}, throughput10ms{false};
    bool ldpc{true};
} g_options;

// Reuse the performance example's PPDU-consistent precoding approach,
// extended below for Minstrel's adaptive stream count and spectrum grid.
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

// Bridge Minstrel's actual HE TxVector to the existing fixed-W[k] API.
// Antenna geometry stays fixed; the PPDU chooses MCS, NSS, width, and GI.
// Each transmitted W[k] is shared by the intended receiver and all overhearers.
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
    bool beamforming{false};
    uint32_t streams{1}; // PHY capability ceiling; actual NSS comes from each PPDU.
    double centerFrequencyHz{0};
    double subcarrierSpacingHz{78125};
    std::map<Mac48Address, Ptr<MobilityModel>> receivers;
    std::map<uint32_t, Ptr<MobilityModel>> peers;

  private:
    struct Selection
    {
        uint64_t uid;
        MimoChannelMatrix channel;
        MimoPrecodingMatrix matrix;
    };
    // Bounded to one most-recent PPDU per transmitter and a small set of
    // frequency-flat antenna-selection matrices indexed by grid size/NSS.
    mutable std::map<uint32_t, Selection> m_selections;
    mutable std::map<std::pair<uint32_t, uint32_t>, MimoPrecodingMatrix> m_openLoop;

    MimoChannelMatrix OnTransmitGrid(const MimoChannelMatrix& full,
                                     Ptr<const SpectrumSignalParameters> params) const
    {
        Require(params && params->psd && params->psd->GetValuesN() >= 2 &&
                    full.GetNumSubcarriers() > 0 && centerFrequencyHz > 0 && subcarrierSpacingHz > 0,
                "missing configured Sionna frequency grid");
        const uint32_t bins = params->psd->GetValuesN() - 1;
        const auto bands = params->psd->GetSpectrumModel();
        const double firstHz = centerFrequencyHz -
                               (full.GetNumSubcarriers() / 2) * subcarrierSpacingHz;
        const double offset = (bands->Begin()->fc - firstHz) / subcarrierSpacingHz;
        const double spacing = ((bands->Begin() + 1)->fc - bands->Begin()->fc) /
                               subcarrierSpacingHz;
        const auto first = std::llround(offset);
        const auto step = std::llround(spacing);
        Require(std::abs(offset - first) < 1e-6 && std::abs(spacing - step) < 1e-6 &&
                    first >= 0 && step > 0 &&
                    first + (bins - 1) * step < full.GetNumSubcarriers(),
                "transmitted spectrum does not lie on the configured Sionna CFR grid");
        if (first == 0 && step == 1 && bins == full.GetNumSubcarriers())
            return full;
        // Select matching physical frequencies, including the primary-channel
        // offset of narrower PPDUs. Keep the original wideband normalization.
        MimoChannelMatrix selected(bins, full.GetNumRxAntennas(), full.GetNumTxAntennas());
        for (uint32_t sc = 0; sc < bins; ++sc)
            for (uint32_t rx = 0; rx < full.GetNumRxAntennas(); ++rx)
                for (uint32_t tx = 0; tx < full.GetNumTxAntennas(); ++tx)
                    selected.Set(sc, rx, tx, full.Get(first + sc * step, rx, tx));
        return selected;
    }

    Ptr<SpectrumValue> DoCalcRxPowerSpectralDensity(
        Ptr<const SpectrumSignalParameters> params,
        Ptr<const MobilityModel> a,
        Ptr<const MobilityModel> b) const override
    {
        const auto channel = OnTransmitGrid(cache->GetPropagationChannelMatrix(a, b), params);
        // Scalar path loss was already applied by SpectrumChannel. The fixed-CFR
        // setter only supplies this reception's sampled tensor and path-loss tag.
        model->SetFixedChannelMatrix(channel, cache->GetPropagationLoss(a, b));
        const auto wifi = DynamicCast<const WifiSpectrumSignalParameters>(params);
        if (wifi && wifi->ppdu &&
            wifi->ppdu->GetTxVector().GetPreambleType() == WIFI_PREAMBLE_HE_SU)
        {
            const uint32_t nss = wifi->ppdu->GetTxVector().GetNss();
            Require(nss >= 1 && nss <= streams, "HE PPDU exceeds the configured NSS ceiling");
            if (beamforming)
            {
                const auto txId = a->GetObject<Node>()->GetId();
                const auto uid = wifi->ppdu->GetUid();
                auto selected = m_selections.find(txId);
                if (selected == m_selections.end() || selected->second.uid != uid)
                {
                    const auto psdu = wifi->ppdu->GetPsdu();
                    Require(bool(psdu), "HE-SU transmission has no PSDU");
                    const auto destination = receivers.find(psdu->GetAddr1());
                    auto target = destination != receivers.end() ? destination->second : peers.at(txId);
                    const auto csi = target == b ? channel :
                        OnTransmitGrid(cache->GetPropagationChannelMatrix(a, target), params);
                    if (selected != m_selections.end() &&
                        selected->second.matrix.GetNumStreams() == nss &&
                        selected->second.channel.GetCoefficients() == csi.GetCoefficients())
                    {
                        selected->second.uid = uid;
                    }
                    else
                    {
                        auto matrix = MimoPrecodingMatrix::SingularValueDecomposition(
                                          csi, nss, MimoSvdPrecodingOptions{})
                                          .GetPrecodingMatrix();
                        selected = m_selections.insert_or_assign(txId, Selection{uid, csi, matrix}).first;
                    }
                }
                model->SetPrecodingMatrix(selected->second.matrix);
            }
            else if (channel.GetNumTxAntennas() > 1)
            {
                const auto key = std::make_pair(channel.GetNumSubcarriers(), nss);
                auto selected = m_openLoop.find(key);
                if (selected == m_openLoop.end())
                {
                    // Unprecoded stream s uses antenna port s. All RX antennas
                    // remain active; total TX power is divided over actual NSS.
                    std::vector<std::complex<double>> identity(channel.GetNumTxAntennas() * nss);
                    for (uint32_t stream = 0; stream < nss; ++stream)
                        identity[stream * nss + stream] = 1;
                    selected = m_openLoop.emplace(key, MimoPrecodingMatrix::FrequencyFlat(
                        channel.GetNumSubcarriers(), channel.GetNumTxAntennas(), nss, identity)).first;
                }
                model->SetPrecodingMatrix(selected->second);
            }
            else
                model->ClearPrecodingMatrix();
        }
        else
            model->ClearPrecodingMatrix();
        return model->CalcRxPowerSpectralDensity(params, a, b);
    }
};

std::vector<Ptr<PacketSink>> g_sinks;
std::vector<uint64_t> lastRx10, lastRxReport;
Time lastTime10, lastTimeReport;
std::vector<uint32_t> g_stationIds;
std::ofstream ofs_mimo;

std::ofstream ofs_throughput;
std::ofstream ofs_snr;      // --snr
std::ofstream ofs_position; // --positions
std::ofstream ofs_minstrel_rates;        // --minstrelRateTrace
std::ofstream ofs_minstrel_rate_changes; // --minstrelRateTrace

// Path-loss fallback is only an estimate; PHY SNR includes scalar interference.
double TX_POWER_DBM = 20.0;
double NOISE_FLOOR_DBM;

std::set<uint32_t> g_staNodeIds;
std::map<std::string, uint32_t> g_macNodeIds;
static constexpr uint32_t INVALID_NODE_ID = 0xFFFFFFFF;

struct PhyRxBeginSample
{
    double timeSeconds;
    Vector pos;
    bool isDataFrame;
    uint32_t packetSize;
    bool hasCfrTag;
    double pathLossDb;
    double rxPowerDbm;
};

std::map<std::pair<uint32_t, uint64_t>, PhyRxBeginSample> g_phyRxBeginSamples;
std::set<std::pair<uint32_t, uint64_t>> g_phyRxFinalized;

std::pair<uint32_t, uint64_t>
MakePhyRxSampleKey(uint32_t nodeId, uint64_t packetUid)
{
    return {nodeId, packetUid};
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
    Ptr<MobilityModel> mob = node ? node->GetObject<MobilityModel>() : nullptr;
    return mob ? mob->GetPosition() : Vector(0, 0, 0);
}

bool
IsWifiDataFrame(Ptr<const Packet> packet)
{
    if (!packet || packet->GetSize() < 24)
        return false;
    Ptr<const Packet> mpdu = packet;
    AmpduSubframeHeader delimiter;
    packet->PeekHeader(delimiter);
    if (delimiter.IsSignatureValid() && delimiter.GetLength() >= 24 &&
        packet->GetSize() >= 4u + delimiter.GetLength())
        mpdu = packet->CreateFragment(4, delimiter.GetLength());
    WifiMacHeader hdr;
    return mpdu->PeekHeader(hdr) > 0 && hdr.IsData();
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
               ? sample.rxPowerDbm - NOISE_FLOOR_DBM
               : std::numeric_limits<double>::quiet_NaN();
}

void
WriteStaPhyLogs(uint32_t rxNodeId,
                uint64_t packetUid,
                const PhyRxBeginSample* beginSample,
                Ptr<const Packet> packet,
                const std::string& rxStatus,
                double phySnrDb,
                const std::string& failureReason = "")
{
    if (!packet)
    {
        return;
    }

    PhyRxBeginSample fallback{};
    if (beginSample == nullptr)
    {
        fallback.timeSeconds = Simulator::Now().GetSeconds();
        fallback.pos = GetNodePosition(rxNodeId);
        fallback.isDataFrame = IsWifiDataFrame(packet);
        fallback.packetSize = packet->GetSize();

        CFRTag tag;
        fallback.hasCfrTag = packet->PeekPacketTag(tag);
        if (fallback.hasCfrTag)
        {
            fallback.pathLossDb = tag.GetPathloss();
            fallback.rxPowerDbm = TX_POWER_DBM - fallback.pathLossDb;
        }
        else
        {
            fallback.pathLossDb = std::numeric_limits<double>::quiet_NaN();
            fallback.rxPowerDbm = std::numeric_limits<double>::quiet_NaN();
        }

        beginSample = &fallback;
    }

    if (!beginSample->isDataFrame)
    {
        return;
    }

    // RxOk/RxError can provide a PHY SNR. RxDrop cannot, so for dropped frames
    // use the same pathloss-based estimate as the L3 logger whenever CFRTag is
    // still present. This avoids losing the "bad" reception samples as NaN.
    double snrDb = std::isfinite(phySnrDb) ? phySnrDb : FallbackSnrFromPathLoss(*beginSample);

    if (ofs_snr.is_open())
    {
        ofs_snr << beginSample->timeSeconds
                << "," << rxNodeId
                << "," << beginSample->pos.x << "," << beginSample->pos.y << "," << beginSample->pos.z
                << "," << beginSample->pathLossDb
                << "," << beginSample->rxPowerDbm
                << "," << snrDb
                << "," << rxStatus
                << "," << packetUid
                << "," << beginSample->packetSize
                << "," << failureReason
                << "\n";
    }

    if (ofs_position.is_open())
    {
        ofs_position << beginSample->timeSeconds
                     << "," << rxNodeId
                     << "," << beginSample->pos.x << "," << beginSample->pos.y << "," << beginSample->pos.z
                     << "," << rxStatus
                     << "," << packetUid
                     << "," << beginSample->packetSize
                     << "," << failureReason
                     << "\n";
    }
}


void
RegisterStaNodes(const NodeContainer& wifiStaNodes)
{
    g_staNodeIds.clear();

    for (uint32_t i = 0; i < wifiStaNodes.GetN(); ++i)
    {
        g_staNodeIds.insert(wifiStaNodes.Get(i)->GetId());
    }
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

std::string
WifiModeToString(WifiMode mode)
{
    return mode.GetUniqueName();
}

void
MinstrelRateChangeTrace(std::string context, uint64_t oldRate, uint64_t newRate)
{
    if (!ofs_minstrel_rate_changes.is_open())
    {
        return;
    }

    const uint32_t nodeId = ExtractNodeIdFromContext(context);
    const uint32_t deviceId = ExtractDeviceIdFromContext(context);

    ofs_minstrel_rate_changes << Simulator::Now().GetSeconds()
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
    if (!ofs_minstrel_rates.is_open() || !packet)
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

    ofs_minstrel_rates << Simulator::Now().GetSeconds()
                       << "," << txNodeId
                       << "," << txDeviceId
                       << "," << srcNodeId
                       << "," << dstNodeId
                       << "," << MacAddressToString(dstAddress)
                       << "," << MacAddressToString(srcAddress)
                       << "," << packet->GetUid()
                       << "," << WifiModeToString(txVector.GetMode())
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

/**
 * PHY-level reception logging.
 *
 * We latch the receiver position at PhyRxBegin (start of reception) and write
 * the final CSV row only once the reception outcome is known. This keeps the
 * CSV structure close to the original while also capturing failed receptions
 * that never make it to IPv4/TCP.
 */
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

    PhyRxBeginSample sample{};
    sample.timeSeconds = Simulator::Now().GetSeconds();
    sample.pos = GetNodePosition(rxNodeId);
    sample.isDataFrame = IsWifiDataFrame(packet);
    sample.packetSize = packet->GetSize();

    CFRTag tag;
    sample.hasCfrTag = packet->PeekPacketTag(tag);
    if (sample.hasCfrTag)
    {
        sample.pathLossDb = tag.GetPathloss();
        sample.rxPowerDbm = TX_POWER_DBM - sample.pathLossDb;
    }
    else
    {
        sample.pathLossDb = std::numeric_limits<double>::quiet_NaN();
        sample.rxPowerDbm = std::numeric_limits<double>::quiet_NaN();
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

    WriteStaPhyLogs(rxNodeId,
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

    WriteStaPhyLogs(rxNodeId,
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

    WriteStaPhyLogs(rxNodeId,
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

// Each row measures one station's received TCP payload over the actual interval.
// The final interval may be shorter; no aggregate rows are emitted.
void
CalculateThroughput(bool fine)
{
    const Time now = Simulator::Now();
    const Time stop = Seconds(g_options.simulationTimeSeconds);
    auto& previous = fine ? lastRx10 : lastRxReport;
    auto& last = fine ? lastTime10 : lastTimeReport;
    const double seconds = (now - last).GetSeconds();
    for (uint32_t i = 0; i < g_sinks.size(); ++i)
    {
        const uint64_t bytes = g_sinks[i]->GetTotalRx();
        const uint64_t delta = bytes - previous[i];
        const double mbps = delta * 8.0 / seconds / 1e6;
        ofs_throughput << now.GetSeconds() << ',' << g_sinks.size() << ',' << i
                       << ',' << g_stationIds[i] << ',' << seconds * 1000
                       << ',' << (fine ? 10 : g_options.reportIntervalMs)
                       << ',' << delta << ',' << mbps << '\n';
        if (g_options.prtTput && !fine)
            std::cout << now.GetSeconds() << "s: STA " << i << " (node "
                      << g_stationIds[i] << "): " << mbps << " Mbit/s" << std::endl;
        previous[i] = bytes;
    }
    last = now;
    if (now < stop)
        Simulator::Schedule(std::min(Seconds((fine ? 10 : g_options.reportIntervalMs) / 1000.0),
                                     stop - now), &CalculateThroughput, fine);
}

void
BeginThroughput()
{
    lastTime10 = lastTimeReport = Simulator::Now();
    lastRx10.resize(g_sinks.size());
    lastRxReport.resize(g_sinks.size());
    for (uint32_t i = 0; i < g_sinks.size(); ++i)
        lastRx10[i] = lastRxReport[i] = g_sinks[i]->GetTotalRx();
    const Time remaining = Seconds(g_options.simulationTimeSeconds) - Simulator::Now();
    Simulator::Schedule(std::min(Seconds(g_options.reportIntervalMs / 1000.0), remaining),
                         &CalculateThroughput, false);
    if (g_options.throughput10ms)
        Simulator::Schedule(std::min(MilliSeconds(10), remaining), &CalculateThroughput, true);
}

void
MimoRxTrace(uint64_t uid, uint32_t tx, uint32_t rx, double sinr, double per, uint32_t rank)
{
    ofs_mimo << Simulator::Now().GetSeconds() << ',' << uid << ',' << tx << ',' << rx
             << ',' << sinr << ',' << LinearSnrToDb(sinr) << ',' << per << ',' << rank << '\n';
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
                const int fixed_coherence_time_ms,
                const int data_Rate_mbps,
                const std::string& rate_adaptation,
                const uint32_t mcs,
                const int seed,
                const bool enable_snr,
                const bool enable_positions,
                const bool enable_minstrel_rate_trace)
{
    g_sinks.clear();
    g_phyRxBeginSamples.clear();
    g_phyRxFinalized.clear();
    g_macNodeIds.clear();
    g_stationIds.clear();
    const auto out = std::filesystem::path(g_options.outputDirectory) /
                     ("stas-" + std::to_string(numStas));
    // Each station-count experiment owns a separate directory, including traces.
    Require(!std::filesystem::exists(out), "output directory already exists: " + out.string());
    std::filesystem::create_directories(out);
    auto open = [&out](std::ofstream& stream, const char* name) {
        stream.open(out / name);
        Require(bool(stream), "cannot write " + (out / name).string());
        stream << std::setprecision(12);
    };
    open(ofs_throughput, "throughput_results.csv");
    if (enable_snr) open(ofs_snr, "snr_results.csv");
    if (enable_positions) open(ofs_position, "position_results.csv");
    if (enable_minstrel_rate_trace)
    {
        open(ofs_minstrel_rates, "minstrel_rate_results.csv");
        open(ofs_minstrel_rate_changes, "minstrel_rate_changes.csv");
    }
    if (g_options.mimoTrace) open(ofs_mimo, "mimo_rx_results.csv");

    // Wifi config
    const uint32_t wifi_channel_num = g_options.channelNumber;
    const uint32_t channel_width = g_options.channelWidthMhz;
    NOISE_FLOOR_DBM = -174.0 + 10.0 * std::log10(channel_width * 1e6) +
                      g_options.noiseFigureDb;
    // Use a single seed value so ns-3 and the Python server share the same run seed.
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(g_options.run);

    SionnaHelper sionnaHelper(environment, g_options.serverUrl);

    // Create Station and Access Points, one access point and variable Stations
    NodeContainer wifiStaNodes;
    wifiStaNodes.Create(numStas);

    NodeContainer wifiApNode;
    wifiApNode.Create(1);

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

    Ptr<SionnaSpectrumPropagationLossModel> spectrumLossModel =
        CreateObject<SionnaSpectrumPropagationLossModel>();
    spectrumLossModel->SetPropagationCache(propagationCache);
    auto adapter = CreateObject<ExperimentSpectrumLoss>();
    adapter->model = spectrumLossModel;
    adapter->cache = propagationCache;
    adapter->beamforming = g_options.precoding == "svd";
    adapter->streams = g_options.streams;
    spectrumChannel->AddSpectrumPropagationLossModel(adapter);

    Ptr<SionnaPropagationDelayModel> delayModel =
        CreateObject<SionnaPropagationDelayModel>();
    delayModel->SetPropagationCache(propagationCache);
    spectrumChannel->SetPropagationDelayModel(delayModel);

    SpectrumWifiPhyHelper spectrumPhy, apPhy;
    SionnaMimoConfiguration mimo;
    mimo.mode = g_options.antennaCount == 1 ? SionnaMimoMode::SISO
                                           : SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
    mimo.effectiveSinrModel = SionnaMimoEffectiveSinrModel::MEAN_UNCODED_BER;
    mimo.txArray.numRows = mimo.rxArray.numRows = g_options.arrayRows;
    mimo.txArray.numColumns = mimo.rxArray.numColumns =
        g_options.antennaCount / g_options.arrayRows;
    mimo.txArray.horizontalSpacing = mimo.rxArray.horizontalSpacing = g_options.antennaSpacingLambda;
    mimo.txArray.verticalSpacing = mimo.rxArray.verticalSpacing = g_options.antennaSpacingLambda;
    mimo.numTxAntennas = mimo.numRxAntennas = g_options.antennaCount;
    mimo.maxNss = g_options.streams;
    mimo.receiverNoiseFigureDb = g_options.noiseFigureDb;
    if (g_options.antennaCount > 1)
    {
        // Dimension-valid bootstrap; the adapter replaces it before each HE PPDU.
        const auto bins = getFFTSize(WIFI_STANDARD_80211ax, channel_width) *
                          SionnaHelper::GUARD_MULTIPLIER;
        std::vector<std::complex<double>> identity(g_options.antennaCount * g_options.streams);
        for (uint32_t s = 0; s < g_options.streams; ++s)
            identity[s * g_options.streams + s] = 1;
        mimo.precodingMatrix = MimoPrecodingMatrix::FrequencyFlat(
            bins, g_options.antennaCount, g_options.streams, identity);
    }
    if (rate_adaptation == "minstrel")
        SionnaMimoHelper(mimo).Apply(sionnaHelper, apPhy, spectrumPhy, spectrumLossModel);
    else
        SionnaMimoHelper(mimo).ApplyForHeMcs(mcs, sionnaHelper, apPhy, spectrumPhy, spectrumLossModel);
    for (auto* phy : {&spectrumPhy, &apPhy})
    {
        phy->SetChannel(spectrumChannel);
        phy->SetErrorRateModel("ns3::NistErrorRateModel");
        phy->Set("TxPowerStart", DoubleValue(TX_POWER_DBM));
        phy->Set("TxPowerEnd", DoubleValue(TX_POWER_DBM));
    }
    if (g_options.mimoTrace)
    {
        ofs_mimo << "Time_s,PpduUid,TxNodeId,RxNodeId,EffectiveSinrLinear,EffectiveSinr_dB,PER,MinimumRank\n";
        spectrumLossModel->TraceConnectWithoutContext("MimoRx", MakeCallback(&MimoRxTrace));
    }

    // Wifi and MAC
    WifiMacHelper mac;
    Ssid ssid = Ssid("ns-3-ssid");

    WifiHelper wifi;
    WifiStandard wifi_standard = WIFI_STANDARD_80211ax;
    wifi.SetStandard(wifi_standard);
    wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifi.ConfigHtOptions("LdpcSupported", BooleanValue(g_options.ldpc));

    std::string channelStr = "{" + std::to_string(wifi_channel_num) + ", " +
                             std::to_string(channel_width) + ", BAND_5GHZ, 0}";
    spectrumPhy.Set("ChannelSettings", StringValue(channelStr));
    apPhy.Set("ChannelSettings", StringValue(channelStr));

    const std::string wifiManager = rate_adaptation == "fixed" ? "ConstantRate" : "MinstrelHt";
    uint32_t rtsThreshold = 999999; // disabled even for large A-MPDU
    if (rate_adaptation == "fixed")
    {
        wifi.SetRemoteStationManager("ns3::" + wifiManager + "WifiManager",
                                     "DataMode",
                                     StringValue("HeMcs" + std::to_string(mcs)),
                                     "ControlMode",
                                     StringValue("OfdmRate24Mbps"),
                                     "RtsCtsThreshold",
                                     UintegerValue(rtsThreshold));
    }
    else
    {
        wifi.SetRemoteStationManager("ns3::" + wifiManager + "WifiManager",
                                     "UseLatestAmendmentOnly", BooleanValue(true),
                                     "RtsCtsThreshold", UintegerValue(rtsThreshold));
    }

    // TCP Setup
    DataRate dataRate(std::to_string(data_Rate_mbps) + "Mb/s");
    const std::string& tcpVariant = g_options.tcpVariant;

    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
        TypeIdValue(TypeId::LookupByName(tcpVariant)));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1472));

    NetDeviceContainer staDevices;
    mac.SetType("ns3::StaWifiMac",
                "Ssid",        SsidValue(ssid),
                "ActiveProbing", BooleanValue(true));
    staDevices = wifi.Install(spectrumPhy, mac, wifiStaNodes);

    NetDeviceContainer apDevices;
    mac.SetType("ns3::ApWifiMac",
                "Ssid",              SsidValue(ssid),
                "BeaconGeneration",  BooleanValue(true),
                "BeaconInterval",    TimeValue(MicroSeconds(102400)),
                "EnableBeaconJitter", BooleanValue(false));
    apDevices = wifi.Install(apPhy, mac, wifiApNode);

    if (enable_minstrel_rate_trace)
    {
        RegisterWifiDeviceMacs(staDevices);
        RegisterWifiDeviceMacs(apDevices);
    }

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
    // Fixed placement streams preserve STA i's initial position across station-count runs.
    randX->SetStream(1100000);
    randY->SetStream(1100001);
    randX->SetAttribute("Min", DoubleValue(0.1));
    randX->SetAttribute("Max", DoubleValue(5.9));
    randY->SetAttribute("Min", DoubleValue(0.1));
    randY->SetAttribute("Max", DoubleValue(3.9));

    // For Apartment Setting
    if ((environment == "apartment/apartment.xml") || (environment == "apartment_wood/apartment_wood.xml"))
    {
        std::cout << "Apartment executed" << std::endl;

        wifiApNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(-1,0.5,1.5));

        // Create a random variable to choose the zone and generate coordinates
        Ptr<UniformRandomVariable> randVar = CreateObject<UniformRandomVariable>();
        randVar->SetStream(1100002);

        for (uint32_t i = 0; i < numStas; i++)
        {
            // Randomly choose a zone: 0, 1, or 2
            int zone = static_cast<int>(randVar->GetValue(0, 3));
            double posX, posY, posZ;

            // Keep the newer L3 script behavior: restrict apartment STAs to
            // the AP room unless this line is removed for all-zone placement.
            zone = 0;

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
    else if (environment == "munich/munich.xml")
    {
        std::cout << "Munich executed" << std::endl;

        wifiApNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(8.5, 21.0, 27.0));

        for (uint32_t i = 0; i < numStas; i++)
        {
            wifiStaNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(
                Vector(45.0 + 0.2 * i, 90.0, 1.5));
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

    if (!g_options.apPosition.empty())
    {
        const auto positions = Positions(g_options.apPosition);
        Require(positions.size() == 1, "apPosition requires one x,y,z triple");
        wifiApNode.Get(0)->GetObject<MobilityModel>()->SetPosition(positions.front());
    }
    if (!g_options.staPositions.empty())
    {
        const auto positions = Positions(g_options.staPositions);
        Require(positions.size() == numStas, "staPositions requires one x,y,z triple per STA");
        for (uint32_t i = 0; i < numStas; ++i)
            wifiStaNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(positions[i]);
    }
    std::vector<Vector> points{wifiApNode.Get(0)->GetObject<MobilityModel>()->GetPosition()};
    for (uint32_t i = 0; i < numStas; ++i)
    {
        points.push_back(wifiStaNodes.Get(i)->GetObject<MobilityModel>()->GetPosition());
        g_stationIds.push_back(wifiStaNodes.Get(i)->GetId());
        adapter->peers[wifiStaNodes.Get(i)->GetId()] = wifiApNode.Get(0)->GetObject<MobilityModel>();
    }
    for (uint32_t i = 0; i < points.size(); ++i)
        for (uint32_t j = i + 1; j < points.size(); ++j)
            Require(CalculateDistance(points[i], points[j]) > 0, "initial positions must be distinct");
    adapter->peers[wifiApNode.Get(0)->GetId()] = wifiStaNodes.Get(0)->GetObject<MobilityModel>();
    NetDeviceContainer allDevices;
    allDevices.Add(apDevices);
    allDevices.Add(staDevices);
    wifi.AssignStreams(allDevices, 2000000);
    for (uint32_t i = 0; i < allDevices.GetN(); ++i)
    {
        const auto device = DynamicCast<WifiNetDevice>(allDevices.Get(i));
        adapter->receivers[device->GetMac()->GetAddress()] = device->GetNode()->GetObject<MobilityModel>();
    }
    if (g_options.pcap)
    {
        spectrumPhy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        spectrumPhy.EnablePcap((out / "wifi").string(), allDevices);
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
    clientApps.Start(Seconds(0));
    serverApps.Start(Seconds(g_options.trafficStartSeconds));
    serverApps.Stop(Seconds(g_options.simulationTimeSeconds));
    Simulator::Schedule(Seconds(g_options.trafficStartSeconds), &BeginThroughput);

    // set center frequency for Sionna
    double fc = get_center_freq(apDevices.Get(0));
    adapter->centerFrequencyHz = fc * 1e6;
    adapter->subcarrierSpacingHz = getSubcarrierSpacing(wifi_standard);
    sionnaHelper.Configure(fc,
                           channel_width,
                           getFFTSize(wifi_standard, channel_width),
                           getSubcarrierSpacing(wifi_standard),
                           fixed_coherence_time_ms);
    sionnaHelper.SetMode(mode);
    sionnaHelper.SetSubMode(sub_mode);

    // Run the final interval report at the measurement boundary before stopping.
    Simulator::Stop(Seconds(g_options.simulationTimeSeconds) + NanoSeconds(1));

    // PHY-level SNR and position logging.
    // We sample position at PhyRxBegin and emit the CSV row on RxOk/RxError/RxDrop,
    // so failed decodes are logged too and the sampled position no longer depends
    // on whether the packet made it up to IPv4/TCP.
    if (enable_snr || enable_positions)
    {
        RegisterStaNodes(wifiStaNodes);

        if (enable_snr)
            ofs_snr << "Time_s,STA_NodeId,pos_x,pos_y,pos_z,PathLoss_dB,RxPower_dBm,SNR_dB,RxStatus,PacketUid,PacketSize_Bytes,FailureReason\n";

        if (enable_positions)
            ofs_position << "Time_s,STA_NodeId,pos_x,pos_y,pos_z,RxStatus,PacketUid,PacketSize_Bytes,FailureReason\n";

        Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/PhyRxBegin",
                        MakeCallback(&PhyRxBeginTrace));
        Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/PhyRxDrop",
                        MakeCallback(&PhyRxDropTrace));
        Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/State/RxError",
                        MakeCallback(&PhyRxErrorTrace));
        Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/State/RxOk",
                        MakeCallback(&PhyRxOkTrace));
    }

    if (enable_minstrel_rate_trace)
    {
        ofs_minstrel_rates << "Time_s,TxNodeId,TxDeviceId,SrcNodeId,DstNodeId,DstMac,SrcMac,PacketUid,Mode,Rate_bps,Rate_Mbps,ChannelFreq_MHz,ChannelWidth_MHz,GuardInterval_ns,Nss,IsAggregated,MpduType,MpduRefNumber,StaId\n";
        ofs_minstrel_rate_changes << "Time_s,NodeId,DeviceId,OldRate_bps,NewRate_bps,NewRate_Mbps\n";

        Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/$ns3::WifiPhy/MonitorSnifferTx",
                        MakeCallback(&MinstrelTxVectorTrace));
        if (rate_adaptation == "minstrel")
            Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/RemoteStationManager/$ns3::MinstrelHtWifiManager/Rate",
                            MakeCallback(&MinstrelRateChangeTrace));
    }

    // Log for throughput:
    ofs_throughput << "Time_s,NumStas,STA_Index,STA_NodeId,Interval_ms,ReportInterval_ms,ReceivedBytes,Throughput_Mbps\n";
    std::ofstream config(out / "config.txt");
    Require(bool(config), "cannot write config.txt");
    config << std::setprecision(17)
           << "environment=" << environment << "\nserverUrl=" << g_options.serverUrl
           << "\nnumStas=" << numStas << "\nantennaCount=" << g_options.antennaCount
           << "\nstreams=" << g_options.streams << "\nprecoding=" << g_options.precoding
           << "\narrayRows=" << g_options.arrayRows << "\nantennaSpacingLambda=" << g_options.antennaSpacingLambda
           << "\nchannelNumber=" << wifi_channel_num << "\nchannelWidthMhz=" << channel_width
           << "\neffectiveSinrModel=MEAN_UNCODED_BER\nreceiver=LINEAR_MMSE"
           << "\nnormalization=UNIT_AVERAGE_PAIR_POWER\nrateAdaptation=" << rate_adaptation
           << "\nmcs=" << (rate_adaptation == "fixed" ? std::to_string(mcs) : "adaptive")
           << "\nstreamsMeaning=" << (rate_adaptation == "minstrel" ? "maximumNss" : "fixedNss")
           << "\nopenLoopSpatialMapping=stream s uses antenna port s"
           << "\nldpc=" << g_options.ldpc
           << "\ndataRateMbpsPerStation=" << data_Rate_mbps << "\ntcpVariant=" << tcpVariant
           << "\ntxPowerDbm=" << TX_POWER_DBM << "\nnoiseFigureDb=" << g_options.noiseFigureDb
           << "\nmobile_scenario=" << mobile_scenario << "\nmobile_speed=" << mobile_speed
           << "\nseed=" << seed << "\nrun=" << g_options.run << "\ncaching=" << caching
           << "\nmode=" << mode << "\nsub_mode=" << sub_mode
           << "\nfixedCoherenceTimeMs=" << fixed_coherence_time_ms
           << "\ntrafficStartSeconds=" << g_options.trafficStartSeconds
           << "\nsimulationTimeSeconds=" << g_options.simulationTimeSeconds
           << "\nreportIntervalMs=" << g_options.reportIntervalMs
           << "\nthroughput10ms=" << g_options.throughput10ms
           << "\nsnr=" << enable_snr << "\npositions=" << enable_positions
           << "\nminstrelRateTrace=" << enable_minstrel_rate_trace << "\npcap=" << g_options.pcap
           << "\nmimoTrace=" << g_options.mimoTrace << "\n";
    for (uint32_t i = 0; i < points.size(); ++i)
        config << (i == 0 ? "apPosition=" : "staPosition" + std::to_string(i - 1) + "=")
               << points[i].x << ',' << points[i].y << ',' << points[i].z << '\n';

    auto startTime = std::chrono::system_clock::now();

    sionnaHelper.Start();
    Simulator::Run();
    std::ofstream summary(out / "summary.csv");
    Require(bool(summary), "cannot write summary.csv");
    summary << "NumStas,STA_Index,STA_NodeId,ReceivedBytes,MeasurementSeconds,MeanGoodput_Mbps\n"
            << std::setprecision(12);
    const double duration = (Seconds(g_options.simulationTimeSeconds) -
                             Seconds(g_options.trafficStartSeconds)).GetSeconds();
    for (uint32_t i = 0; i < g_sinks.size(); ++i)
    {
        // lastRxReport is the counter at the exact measurement endpoint.
        const auto bytes = lastRxReport[i];
        summary << numStas << ',' << i << ',' << g_stationIds[i] << ',' << bytes
                << ',' << duration << ',' << bytes * 8.0 / duration / 1e6 << '\n';
    }
    Simulator::Destroy();

    // propagationCache->PrintStats();

    sionnaHelper.Destroy();

    ofs_throughput.close();
    if (g_options.mimoTrace) ofs_mimo.close();
    if (enable_snr)
        ofs_snr.close();
    if (enable_positions)
        ofs_position.close();
    if (enable_minstrel_rate_trace)
    {
        ofs_minstrel_rates.close();
        ofs_minstrel_rate_changes.close();
    }

    auto endTime = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsedTime = endTime - startTime;
    double computationTime = elapsedTime.count();

    std::ofstream ofs_compTime(out / "computationtime.csv");
    Require(bool(ofs_compTime), "cannot write computationtime.csv");
    ofs_compTime << "CoherenceTime_ms,ComputationTime_s,MobileSpeed_mps,Seed\n";

    ofs_compTime << fixed_coherence_time_ms << "," << computationTime << "," << mobile_speed << "," << seed << "\n";
    ofs_compTime.close();

    std::cout << "Finished simulation with " << numStas << " stations in "
              << computationTime << " sec" << std::endl << std::endl;

    return computationTime;
}

int
main(int argc, char* argv[])
{
    bool verbose{false}, caching{true}, mobile_scenario{false};
    bool snr{false}, positions{false}, rateTrace{false};
    std::string environment{"apartment/apartment.xml"}, rateAdaptation{"minstrel"};
    double mobile_speed{1};
    uint32_t minStas{1}, maxStas{4}, mcs{11};
    int mode{2}, subMode{16}, coherenceMs{0}, dataRate{600}, seed{1};
    CommandLine cmd(__FILE__);
    auto addString = [&cmd](const std::string& name, const std::string& help, std::string& value) {
        cmd.AddValue(name, help, MakeBoundCallback(&StringOption, &value), value);
    };
    addString("environment", "Sionna scene XML", environment);
    addString("serverUrl", "Running Sionna server endpoint", g_options.serverUrl);
    addString("outputDirectory", "Root for separate stas-N experiment directories", g_options.outputDirectory);
    cmd.AddValue("sim_min_stas", "First station count; double through sim_max_stas", minStas);
    cmd.AddValue("sim_max_stas", "Maximum station count", maxStas);
    cmd.AddValue("channel", "5 GHz channel; 0 selects 36/38/42 for 20/40/80 MHz", g_options.channelNumber);
    cmd.AddValue("channelNumber", "Alias for channel", g_options.channelNumber);
    cmd.AddValue("channelWidthMhz", "20, 40, or 80 MHz", g_options.channelWidthMhz);
    cmd.AddValue("mobile_scenario", "Enable station random walks; AP remains static", mobile_scenario);
    cmd.AddValue("mobile_speed", "Station walking speed in m/s", mobile_speed);
    cmd.AddValue("caching", "Reuse propagation-cache entries", caching);
    cmd.AddValue("mode", "Sionna mode: 1=P2P, 2=P2MP, 3=lookahead", mode);
    cmd.AddValue("sub_mode", "Sionna submode/depth", subMode);
    cmd.AddValue("subMode", "Alias for sub_mode", subMode);
    cmd.AddValue("verbose", "Enable Sionna debug logging", verbose);
    cmd.AddValue("fixedCoherenceTimeMs", "Fixed cache coherence in ms; 0 uses dynamic value", coherenceMs);
    cmd.AddValue("dataRate", "Offered TCP load in Mb/s PER STATION", dataRate);
    addString("rateAdaptation", "minstrel (adaptive HE MCS/NSS/width/GI), or fixed", rateAdaptation);
    cmd.AddValue("mcs", "HE MCS for rateAdaptation=fixed, 0--11; ignored by Minstrel", mcs);
    addString("tcpVariant", "TCP congestion-control TypeId", g_options.tcpVariant);
    cmd.AddValue("seed", "Shared ns-3/Sionna seed", seed);
    cmd.AddValue("run", "ns-3 RNG run number", g_options.run);
    cmd.AddValue("txPower", "Total transmit power per device in dBm", TX_POWER_DBM);
    cmd.AddValue("txPowerDbm", "Alias for txPower", TX_POWER_DBM);
    cmd.AddValue("antennaCount", "Antenna ports at each AP and STA, 1--8", g_options.antennaCount);
    cmd.AddValue("arrayRows", "Array rows; must divide antennaCount", g_options.arrayRows);
    cmd.AddValue("antennaSpacingLambda", "Array spacing in wavelengths", g_options.antennaSpacingLambda);
    cmd.AddValue("streams", "NSS ceiling for Minstrel, fixed NSS otherwise; 0: antennas for open-loop, one for SVD", g_options.streams);
    addString("precoding", "open-loop or svd (ideal current CSI, equal power)", g_options.precoding);
    cmd.AddValue("noiseFigureDb", "Matching PHY/MIMO noise figure", g_options.noiseFigureDb);
    cmd.AddValue("ldpc", "Use LDPC; required for HE MCS 10/11", g_options.ldpc);
    addString("apPosition", "Optional AP x,y,z; otherwise scene default", g_options.apPosition);
    addString("staPositions", "Optional x,y,z;x,y,z; requires one entry per STA and one station count", g_options.staPositions);
    cmd.AddValue("simulationTimeSeconds", "Simulation end time", g_options.simulationTimeSeconds);
    cmd.AddValue("trafficStartSeconds", "TCP start after association warmup", g_options.trafficStartSeconds);
    cmd.AddValue("reportIntervalMs", "Per-station throughput interval; default 100 ms", g_options.reportIntervalMs);
    cmd.AddValue("prtTput", "Print per-station interval throughput", g_options.prtTput);
    cmd.AddValue("throughput10ms", "Also record legacy 10 ms per-station throughput", g_options.throughput10ms);
    cmd.AddValue("snr", "Record per-reception PHY SNR and failures", snr);
    cmd.AddValue("positions", "Record per-reception positions and failures", positions);
    cmd.AddValue("minstrelRateTrace", "Record per-TX modes/NSS; Minstrel rate changes when selected", rateTrace);
    cmd.AddValue("tracing", "Enable PCAP recording on all devices", g_options.pcap);
    cmd.AddValue("pcap", "Alias for tracing", g_options.pcap);
    cmd.AddValue("mimoTrace", "Record adapter effective SINR, PER and rank per reception", g_options.mimoTrace);
    cmd.Parse(argc, argv);
    try
    {
        Require(minStas >= 1 && maxStas >= minStas && maxStas <= 253,
                "require 1 <= sim_min_stas <= sim_max_stas <= 253");
        Require(g_options.staPositions.empty() || minStas == maxStas,
                "explicit staPositions requires sim_min_stas=sim_max_stas");
        Require(rateAdaptation == "fixed" || rateAdaptation == "minstrel", "rateAdaptation must be fixed or minstrel");
        Require(mcs <= 11 && seed > 0 && g_options.run > 0 && dataRate > 0,
                "invalid MCS, seed, RNG run or data rate");
        Require(g_options.ldpc || (rateAdaptation == "fixed" && mcs < 10),
                "HE MCS 10/11 and Minstrel's full HE rate set require ldpc=true");
        Require(g_options.channelWidthMhz == 20 || g_options.channelWidthMhz == 40 ||
                    g_options.channelWidthMhz == 80, "channelWidthMhz must be 20, 40 or 80");
        if (!g_options.channelNumber)
            g_options.channelNumber = g_options.channelWidthMhz == 20 ? 36 :
                                      g_options.channelWidthMhz == 40 ? 38 : 42;
        Require(g_options.antennaCount >= 1 && g_options.antennaCount <= 8 &&
                    g_options.arrayRows > 0 && g_options.antennaCount % g_options.arrayRows == 0,
                "invalid antennaCount/arrayRows");
        Require(g_options.precoding == "open-loop" || g_options.precoding == "svd", "precoding must be open-loop or svd");
        g_options.streams = g_options.streams ? g_options.streams :
                            (g_options.precoding == "svd" ? 1 : g_options.antennaCount);
        Require(g_options.streams >= 1 && g_options.streams <= g_options.antennaCount, "invalid streams");
        Require(g_options.precoding != "svd" || g_options.antennaCount > 1, "SVD requires at least two antennas");
        Require(std::isfinite(g_options.antennaSpacingLambda) && g_options.antennaSpacingLambda > 0 &&
                    std::isfinite(g_options.noiseFigureDb) && g_options.noiseFigureDb >= 0 &&
                    std::isfinite(TX_POWER_DBM) && std::isfinite(mobile_speed) && mobile_speed >= 0,
                "invalid array spacing, noise figure, power or speed");
        Require(mode >= 1 && mode <= 3 && subMode > 0 && coherenceMs >= 0, "invalid Sionna mode/submode/coherence time");
        Require(std::isfinite(g_options.trafficStartSeconds) && g_options.trafficStartSeconds >= 0.5 &&
                    std::isfinite(g_options.simulationTimeSeconds) &&
                    Seconds(g_options.simulationTimeSeconds) > Seconds(g_options.trafficStartSeconds) &&
                    std::isfinite(g_options.reportIntervalMs) && g_options.reportIntervalMs > 0 &&
                    Seconds(g_options.reportIntervalMs / 1000.0).IsStrictlyPositive(), "invalid measurement window or interval");
        TypeId tcpType;
        Require(TypeId::LookupByNameFailSafe(g_options.tcpVariant, &tcpType) && tcpType.HasConstructor() &&
                    DynamicCast<TcpCongestionOps>(ObjectFactory(g_options.tcpVariant).Create<Object>()) != nullptr,
                "invalid TCP congestion-control TypeId");
        // Reject an existing sweep before starting any simulations.
        for (uint32_t n = minStas; n <= maxStas; n *= 2)
            Require(!std::filesystem::exists(std::filesystem::path(g_options.outputDirectory) /
                        ("stas-" + std::to_string(n))), "station-count output directory already exists");
        if (verbose)
        {
            LogComponentEnable("SionnaHelper", LOG_LEVEL_DEBUG);
            LogComponentEnable("SionnaPropagationDelayModel", LOG_INFO);
        }
        for (uint32_t n = minStas; n <= maxStas; n *= 2)
            RunSimulation(environment, n, mobile_scenario, mobile_speed, caching, mode, subMode,
                          verbose, coherenceMs, dataRate, rateAdaptation, mcs, seed, snr, positions, rateTrace);
    }
    catch (const std::exception& error)
    {
        std::cerr << "experiment_error=" << error.what() << std::endl;
        Simulator::Destroy();
        return 2;
    }
    return 0;
}
