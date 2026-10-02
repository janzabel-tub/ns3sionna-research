/*
 * Copyright (c) 2024 Yannik Pilz
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Author: Yannik Pilz <y.pilz@campus.tu-berlin.de>
 */

#include "sionna-propagation-cache.h"

#include "message.pb.h"
#include "sionna-mobility-model.h"

#include "ns3/abort.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ns3
{

namespace
{

[[noreturn]] void
RejectSnapshot(const std::string& reason)
{
    throw std::invalid_argument("Invalid multi-user channel snapshot: " + reason);
}

uint32_t
GetArrayAntennaCount(const ns3sionna::AntennaArrayConfig& array)
{
    if (array.num_rows() == 0 || array.num_cols() == 0 ||
        !std::isfinite(array.vertical_spacing()) || array.vertical_spacing() <= 0.0 ||
        !std::isfinite(array.horizontal_spacing()) || array.horizontal_spacing() <= 0.0 ||
        array.pattern().empty())
    {
        RejectSnapshot("invalid RX-array identity");
    }
    uint32_t polarizations;
    if (array.polarization() == "V" || array.polarization() == "H")
    {
        polarizations = 1;
    }
    else if (array.polarization() == "VH")
    {
        polarizations = 2;
    }
    else
    {
        RejectSnapshot("invalid RX-array polarization");
    }
    const uint64_t elements = static_cast<uint64_t>(array.num_rows()) * array.num_cols();
    if (elements > std::numeric_limits<uint32_t>::max() / polarizations)
    {
        RejectSnapshot("RX-array antenna count overflows uint32_t");
    }
    return static_cast<uint32_t>(elements * polarizations);
}

uint64_t
CheckedProduct(uint64_t a, uint64_t b)
{
    if (b != 0 && a > std::numeric_limits<uint64_t>::max() / b)
    {
        throw std::invalid_argument("Channel budget arithmetic overflow");
    }
    return a * b;
}

uint64_t
CheckedSum(uint64_t a, uint64_t b)
{
    if (a > std::numeric_limits<uint64_t>::max() - b)
    {
        throw std::invalid_argument("Channel budget arithmetic overflow");
    }
    return a + b;
}

void
CheckLimit(uint64_t value, uint64_t limit, const char* name)
{
    if (limit == 0 || value > limit)
    {
        throw std::invalid_argument(std::string("Channel resource limit exceeded: ") + name);
    }
}

} // namespace

NS_LOG_COMPONENT_DEFINE("SionnaPropagationCache");

NS_OBJECT_ENSURE_REGISTERED(SionnaPropagationCache);

TypeId
SionnaPropagationCache::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::SionnaPropagationCache")
            .SetParent<Object>()
            .SetGroupName("Propagation")
            .AddConstructor<SionnaPropagationCache>();
    return tid;
}

SionnaPropagationCache::SionnaPropagationCache()
    : m_sionnaHelper(nullptr), m_caching(true), m_cache_hits(0), m_cache_miss(0), m_optimize(true)
{
    m_friisLossModel = CreateObject<FriisPropagationLossModel>();
    m_constSpeedDelayModel = CreateObject<ConstantSpeedPropagationDelayModel>();
}

SionnaPropagationCache::~SionnaPropagationCache()
{
    m_cache.clear();
}

Time
SionnaPropagationCache::GetPropagationDelay(Ptr<MobilityModel> a, Ptr<MobilityModel> b) const
{
    // Check if distance is too far so that a simpler model can be used
    if (m_optimize)
    {
        const double MAX_TXPOWER_DBM = 20.0; // AZU: todo: hardcoded
        double resultdBm = m_friisLossModel->CalcRxPower(MAX_TXPOWER_DBM, a, b);
        if (resultdBm + m_optimize_margin < m_sionnaHelper->GetNoiseFloor())
        {
            Time const_delay = m_constSpeedDelayModel->GetDelay(a, b);
            NS_LOG_DEBUG("Skipped raytracing for prop delay due to large distance; const delay used: " << const_delay);
            return const_delay;
        }
        // signal is too strong and delay need to be computed with ray tracing
    }

    return GetPropagationData(a, b).m_delay;
}

double
SionnaPropagationCache::GetPropagationLoss(Ptr<MobilityModel> a, Ptr<MobilityModel> b, double txPowerDbm) const
{
    // Check if distance is too far so that a simpler model can be used
    if (m_optimize)
    {
        double resultdBm = m_friisLossModel->CalcRxPower(txPowerDbm, a, b);
        if (resultdBm + m_optimize_margin < m_sionnaHelper->GetNoiseFloor())
        {
            double friis_loss = (-1) * (resultdBm - txPowerDbm);
            NS_LOG_DEBUG("Skipped raytracing for prop loss due to large distance; friis loss used: " << friis_loss);
            return friis_loss;
        }
        // signal is too strong and need to be computed with ray tracing
    }

    Vector pos_a = a->GetPosition();
    Vector pos_b = b->GetPosition();

    // update position on mobility models to reflect node position in Sionna
    CacheEntry ce = GetPropagationData(a, b);

    Ptr<Node> node_a = a->GetObject<Node>();
    Ptr<Node> node_b = b->GetObject<Node>();

    // needed as the cache assumes channel reciprocity
    if (node_a->GetId() == ce.m_a)
    {
        a->SetPosition(ce.m_a_position);
        b->SetPosition(ce.m_b_position);
    } else
    {
        a->SetPosition(ce.m_b_position);
        b->SetPosition(ce.m_a_position);
    }

    Time current_time = Simulator::Now();
    if (pos_a != a->GetPosition())
    {
        NS_LOG_INFO("ns3sionna::update Pos for node: " << node_a->GetId() << " from " <<
            ": (" << pos_a.x << "," << pos_a.y << "," << pos_a.z << ") to: " <<
            ": (" << a->GetPosition().x << "," << a->GetPosition().y << "," << a->GetPosition().z << ")");
    }

    if (pos_b != b->GetPosition())
    {
        NS_LOG_INFO("ns3sionna::update Pos for node: " << node_b->GetId() << " from " <<
            ": (" << pos_b.x << "," << pos_b.y << "," << pos_b.z << ") to: " <<
            ": (" << b->GetPosition().x << "," << b->GetPosition().y << "," << b->GetPosition().z << ")");
    }

    return ce.m_loss;
}

double
SionnaPropagationCache::GetPropagationLoss(Ptr<const MobilityModel> a, Ptr<const MobilityModel> b) const
{
    Ptr<MobilityModel> tmp_a = ConstCast<MobilityModel>(a);
    Ptr<MobilityModel> tmp_b = ConstCast<MobilityModel>(b);

    return GetPropagationData(tmp_a, tmp_b).m_loss;
}

std::vector<int>
SionnaPropagationCache::GetPropagationFreq(Ptr<const MobilityModel> a, Ptr<const MobilityModel> b) const
{
    Ptr<MobilityModel> tmp_a = ConstCast<MobilityModel>(a);
    Ptr<MobilityModel> tmp_b = ConstCast<MobilityModel>(b);
    return GetPropagationData(tmp_a, tmp_b).m_freq;
}


std::vector<std::complex<double>>
SionnaPropagationCache::GetPropagationCSI(Ptr<const MobilityModel> a, Ptr<const MobilityModel> b) const
{
    Ptr<MobilityModel> tmp_a = ConstCast<MobilityModel>(a);
    Ptr<MobilityModel> tmp_b = ConstCast<MobilityModel>(b);
    return GetPropagationData(tmp_a, tmp_b).m_cfr.GetCoefficients();
}

MimoChannelMatrix
SionnaPropagationCache::GetPropagationChannelMatrix(Ptr<const MobilityModel> a,
                                                     Ptr<const MobilityModel> b) const
{
    Ptr<MobilityModel> tmp_a = ConstCast<MobilityModel>(a);
    Ptr<MobilityModel> tmp_b = ConstCast<MobilityModel>(b);
    return GetPropagationData(tmp_a, tmp_b).m_cfr;
}

void
SionnaPropagationCache::SetResourceLimits(const ResourceLimits& limits)
{
    for (auto value : {limits.maxSubcarriers, limits.maxRxAntennas, limits.maxTxAntennas,
                       limits.maxCoefficientsPerChannel, limits.maxBatchCoefficients,
                       limits.maxReplyBytes, limits.maxSnapshots, limits.maxPayloadBytes})
    {
        if (value == 0) { throw std::invalid_argument("Channel resource limits must be positive"); }
    }
    const auto storage = GetStorageStatistics();
    CheckLimit(storage.snapshots, limits.maxSnapshots, "retained snapshots");
    CheckLimit(storage.payloadBytes, limits.maxPayloadBytes, "retained payload bytes");
    m_limits = limits;
}

uint64_t
SionnaPropagationCache::ValidateChannelDimensions(uint64_t sc, uint64_t rx, uint64_t tx,
                                                 const ResourceLimits& limits)
{
    if (sc == 0 || rx == 0 || tx == 0)
    {
        throw std::invalid_argument("Channel dimensions must be positive");
    }
    const auto count = CheckedProduct(CheckedProduct(sc, rx), tx);
    CheckLimit(sc, limits.maxSubcarriers, "subcarriers");
    CheckLimit(rx, limits.maxRxAntennas, "RX antennas");
    CheckLimit(tx, limits.maxTxAntennas, "TX antennas");
    CheckLimit(count, limits.maxCoefficientsPerChannel, "coefficients per channel");
    CheckLimit(count, std::numeric_limits<std::size_t>::max() / sizeof(std::complex<double>),
               "CFR allocation bytes");
    return count;
}

MimoChannelMatrix
SionnaPropagationCache::DeserializeChannelMatrix(
    const ns3sionna::ChannelStateResponse::ChannelState::RxNodeInfo& message)
{
    return DeserializeChannelMatrix(message, ResourceLimits{});
}

MimoChannelMatrix
SionnaPropagationCache::DeserializeChannelMatrix(
    const ns3sionna::ChannelStateResponse::ChannelState::RxNodeInfo& message,
    const ResourceLimits& limits)
{
    const int coefficientCount = message.csi_real_size();
    if (message.csi_imag_size() != coefficientCount)
    {
        throw std::invalid_argument("Real and imaginary CFR arrays have different sizes");
    }

    const int numSubcarriers = message.frequencies_size();
    if (coefficientCount == 0 && numSubcarriers == 0)
    {
        return MimoChannelMatrix();
    }

    const uint32_t numRxAntennas =
        message.num_rx_antennas() == 0 ? 1 : message.num_rx_antennas();
    const uint32_t numTxAntennas =
        message.num_tx_antennas() == 0 ? 1 : message.num_tx_antennas();
    if (numSubcarriers == 0)
    {
        throw std::invalid_argument("A non-empty CFR needs subcarrier frequencies");
    }
    const auto expectedCount = ValidateChannelDimensions(numSubcarriers, numRxAntennas,
                                                         numTxAntennas, limits);
    if (static_cast<std::size_t>(coefficientCount) != expectedCount)
    {
        throw std::invalid_argument("CFR coefficient count does not match sc/rx/tx dimensions");
    }

    std::vector<std::complex<double>> coefficients;
    coefficients.reserve(expectedCount);
    for (int i = 0; i < coefficientCount; ++i)
    {
        if (!std::isfinite(message.csi_real(i)) || !std::isfinite(message.csi_imag(i)))
        {
            throw std::invalid_argument("Non-finite CFR coefficient");
        }
        coefficients.emplace_back(message.csi_real(i), message.csi_imag(i));
    }

    return MimoChannelMatrix(numSubcarriers,
                             numRxAntennas,
                             numTxAntennas,
                             coefficients);
}

MultiUserChannelSnapshot
SionnaPropagationCache::DeserializeMultiUserChannelSnapshot(
    const ns3sionna::ChannelStateResponse::ChannelState& message)
{
    return DeserializeMultiUserChannelSnapshot(message, ResourceLimits{});
}

MultiUserChannelSnapshot
SionnaPropagationCache::DeserializeMultiUserChannelSnapshot(
    const ns3sionna::ChannelStateResponse::ChannelState& message, const ResourceLimits& limits)
{
    CheckLimit(message.rx_nodes_size(), limits.maxSnapshots, "reply snapshots");
    uint64_t batchCount = 0;
    for (const auto& user : message.rx_nodes())
    {
        batchCount = CheckedSum(batchCount, ValidateChannelDimensions(user.frequencies_size(),
            user.num_rx_antennas() == 0 ? 1 : user.num_rx_antennas(),
            user.num_tx_antennas() == 0 ? 1 : user.num_tx_antennas(), limits));
    }
    CheckLimit(batchCount, limits.maxBatchCoefficients, "batch coefficients");
    CheckLimit(batchCount, std::numeric_limits<std::size_t>::max() / sizeof(std::complex<double>),
               "batch CFR allocation bytes");
    if (message.rx_nodes_size() == 0)
    {
        RejectSnapshot("no users");
    }

    std::set<uint32_t> userIds;
    std::vector<int> sharedFrequencies;
    uint32_t sharedNumTxAntennas = 0;
    for (const auto& user : message.rx_nodes())
    {
        if (!user.has_user_identity())
        {
            RejectSnapshot("missing user identity");
        }
        const auto& identity = user.user_identity();
        if (!userIds.insert(identity.user_id()).second)
        {
            RejectSnapshot("duplicate user identity");
        }
        if (identity.user_id() != user.id() || identity.tx_node_id() != message.tx_node().id() ||
            identity.rx_node_id() != user.id())
        {
            RejectSnapshot("inconsistent directed endpoints");
        }
        if (identity.direction() != ns3sionna::CHANNEL_DIRECTION_DOWNLINK)
        {
            RejectSnapshot("Phase 4.1 accepts only downlink channels");
        }
        if (identity.csi_timestamp() != message.start_time())
        {
            RejectSnapshot("mixed CSI timestamps");
        }
        if (!identity.has_rx_array())
        {
            RejectSnapshot("missing RX-array identity");
        }

        const uint32_t numRxAntennas = user.num_rx_antennas() == 0 ? 1 : user.num_rx_antennas();
        const uint32_t numTxAntennas = user.num_tx_antennas() == 0 ? 1 : user.num_tx_antennas();
        if (numRxAntennas != GetArrayAntennaCount(identity.rx_array()))
        {
            RejectSnapshot("RX-array identity does not match the CFR dimension");
        }
        const int numSubcarriers = user.frequencies_size();
        const uint64_t expectedCount = ValidateChannelDimensions(numSubcarriers, numRxAntennas,
                                                                numTxAntennas, limits);
        if (numSubcarriers == 0 || user.csi_real_size() != user.csi_imag_size() ||
            static_cast<uint64_t>(user.csi_real_size()) != expectedCount)
        {
            RejectSnapshot("CFR coefficient count does not match sc/rx/tx dimensions");
        }

        std::vector<int> frequencies(user.frequencies().begin(), user.frequencies().end());
        if (sharedFrequencies.empty())
        {
            sharedFrequencies = frequencies;
            sharedNumTxAntennas = numTxAntennas;
        }
        else if (frequencies != sharedFrequencies || numTxAntennas != sharedNumTxAntennas)
        {
            RejectSnapshot("inconsistent frequency grid or TX dimension");
        }

        double meanPower = 0.0;
        for (int i = 0; i < user.csi_real_size(); ++i)
        {
            if (!std::isfinite(user.csi_real(i)) || !std::isfinite(user.csi_imag(i)))
            {
                RejectSnapshot("non-finite CFR coefficient");
            }
            meanPower += std::norm(std::complex<double>(user.csi_real(i), user.csi_imag(i)));
        }
        meanPower /= user.csi_real_size();
        if (!std::isfinite(user.wb_loss()) || std::abs(meanPower - 1.0) > 1e-3)
        {
            RejectSnapshot("invalid wideband loss or CFR normalization");
        }
    }

    std::vector<const ns3sionna::ChannelStateResponse::ChannelState::RxNodeInfo*> ordered;
    ordered.reserve(message.rx_nodes_size());
    for (const auto& user : message.rx_nodes())
    {
        ordered.push_back(&user);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->user_identity().user_id() < rhs->user_identity().user_id();
    });

    MultiUserChannelSnapshot snapshot{NanoSeconds(message.start_time()),
                                      MultiUserChannelDirection::DOWNLINK,
                                      {}};
    snapshot.users.reserve(ordered.size());
    for (const auto* user : ordered)
    {
        const auto& identity = user->user_identity();
        const auto& array = identity.rx_array();
        MultiUserReceiveArrayIdentity arrayIdentity{array.num_rows(),
                                                    array.num_cols(),
                                                    array.vertical_spacing(),
                                                    array.horizontal_spacing(),
                                                    array.pattern(),
                                                    array.polarization()};
        snapshot.users.push_back({identity.user_id(),
                                  identity.tx_node_id(),
                                  identity.rx_node_id(),
                                  MultiUserChannelDirection::DOWNLINK,
                                  snapshot.csiTimestamp,
                                  std::move(arrayIdentity),
                                  std::vector<int>(user->frequencies().begin(),
                                                   user->frequencies().end()),
                                  user->wb_loss(),
                                  DeserializeChannelMatrix(*user, limits)});
    }
    return snapshot;
}

void
SionnaPropagationCache::SetSionnaHelper(SionnaHelper &sionnaHelper)
{
    m_sionnaHelper = &sionnaHelper;
}

SionnaHelper*
SionnaPropagationCache::GetSionnaHelper()
{
    return m_sionnaHelper;
}

void
SionnaPropagationCache::SetCaching(bool caching)
{
    m_caching = caching;
    if (!caching)
    {
        // Disabling also releases snapshots retained by earlier enabled requests.
        m_cache.clear();
        m_storage.links = m_storage.snapshots = m_storage.payloadBytes = m_storage.capacityBytes = 0;
        m_nextExpiry = Time::Max();
    }
}

void
SionnaPropagationCache::SetOptimize(bool optimize)
{
    m_optimize = optimize;
}

double
SionnaPropagationCache::GetStats()
{
    const double lookups = m_cache_hits + m_cache_miss;
    return lookups == 0 ? 0.0 : m_cache_hits / lookups;
}

void SionnaPropagationCache::PrintStats()
{
    std::cout << "Ns3-sionna: cache #lookups: " <<  (m_cache_hits + m_cache_miss) << ", #misses:"
        << m_cache_miss << ", hit ratio: " <<  this->GetStats() << std::endl;
}


bool
SionnaPropagationCache::Matches(const CacheEntry& entry, uint32_t tx, uint32_t rx, Time now)
{
    return entry.m_start_time <= now && entry.m_end_time >= now &&
           ((entry.m_a == tx && entry.m_b == rx) ||
            (!entry.m_directed && entry.m_a == rx && entry.m_b == tx));
}

uint64_t
SionnaPropagationCache::PayloadBytes(const CacheEntry& entry)
{
    return entry.m_cfr.GetCoefficients().size() * sizeof(std::complex<double>) +
           entry.m_freq.size() * sizeof(int);
}

uint64_t
SionnaPropagationCache::CapacityBytes(const CacheEntry& entry)
{
    return entry.m_cfr.GetCoefficients().capacity() * sizeof(std::complex<double>) +
           entry.m_freq.capacity() * sizeof(int);
}

void
SionnaPropagationCache::UpdateStoragePeaks() const
{
    m_storage.peakLinks = std::max(m_storage.peakLinks, m_storage.links);
    m_storage.peakSnapshots = std::max(m_storage.peakSnapshots, m_storage.snapshots);
    m_storage.peakPayloadBytes = std::max(m_storage.peakPayloadBytes, m_storage.payloadBytes);
    m_storage.peakCapacityBytes = std::max(m_storage.peakCapacityBytes, m_storage.capacityBytes);
}

void
SionnaPropagationCache::PurgeExpired(Time now) const
{
    // Validity is inclusive. Scan only after the earliest retained end time.
    if (now <= m_nextExpiry)
    {
        return;
    }
    m_nextExpiry = Time::Max();
    for (auto link = m_cache.begin(); link != m_cache.end();)
    {
        auto& entries = link->second;
        auto expired = std::remove_if(entries.begin(), entries.end(), [&](const CacheEntry& entry) {
            if (entry.m_end_time < now)
            {
                --m_storage.snapshots;
                m_storage.payloadBytes -= PayloadBytes(entry);
                m_storage.capacityBytes -= CapacityBytes(entry);
                return true;
            }
            m_nextExpiry = std::min(m_nextExpiry, entry.m_end_time);
            return false;
        });
        entries.erase(expired, entries.end());
        if (entries.empty())
        {
            m_storage.capacityBytes -= entries.capacity() * sizeof(CacheEntry);
            link = m_cache.erase(link);
        }
        else
        {
            ++link;
        }
    }
    m_storage.links = m_cache.size();
}

SionnaPropagationCache::StorageStatistics
SionnaPropagationCache::GetStorageStatistics() const
{
    PurgeExpired(Simulator::Now());
    return m_storage;
}


SionnaPropagationCache::CacheEntry
SionnaPropagationCache::GetPropagationData(Ptr<MobilityModel> a, Ptr<MobilityModel> b) const
{
    NS_ASSERT_MSG(m_sionnaHelper, "SionnaPropagationCache must have reference to SionnaHelper.");
    Ptr<SionnaMobilityModel> sionna_a = DynamicCast<SionnaMobilityModel> (a);
    Ptr<SionnaMobilityModel> sionna_b = DynamicCast<SionnaMobilityModel> (b);
    NS_ASSERT_MSG(sionna_a && sionna_b, "Not using SionnaMobilityModel.");

    Time current_time = Simulator::Now();

    Ptr<Node> node_a = a->GetObject<Node>();
    Ptr<Node> node_b = b->GetObject<Node>();
    NS_ASSERT_MSG(node_a && node_b, "Nodes not found.");

    NS_LOG_DEBUG("ns3sionna::GetPropagationData for lnk: " << node_a->GetId() << " to " << node_b->GetId());

    PurgeExpired(current_time);
    if (m_caching)
    {
        //NS_LOG_INFO("Check cache");
        // Look in the cache and check if delay and loss have already been calculated for the two nodes
        CacheKey key = CacheKey(node_a->GetId(), node_b->GetId());
        auto it = m_cache.find(key);

        if (it != m_cache.end())
        {
            // iterate over all stored entries for that link
            for (CacheEntry c_entry : it->second) {
                //NS_LOG_INFO(" " << c_entry.m_start_time << " " << c_entry.m_end_time << " " << current_time);
                // If delay and loss exist in the cache, check if the entry is not outdated
                if (Matches(c_entry, node_a->GetId(), node_b->GetId(), current_time))
                {
                    NS_LOG_DEBUG("\t: Cache hit for lnk: " << node_a->GetId() << " to " << node_b->GetId());
                    m_cache_hits += 1;
                    // Return cache entry as the value is still fresh
                    return c_entry;
                }
            }
        }
    }

    NS_LOG_INFO("\t: Cache miss for lnk: " << node_a->GetId() << " to " << node_b->GetId());
    m_cache_miss += 1;

    // Prepare the request message
    ns3sionna::Wrapper wrapper;

    // Fill the information message
    ns3sionna::ChannelStateRequest* propagation_request = wrapper.mutable_channel_state_request();
    propagation_request->set_tx_node(node_a->GetId());
    propagation_request->set_rx_node(node_b->GetId());
    propagation_request->set_time(current_time.GetNanoSeconds());
    
    // Serialize the request message
    std::string serialized_message;
    wrapper.SerializeToString(&serialized_message);

    // Send the request message
    zmq::message_t zmq_message(serialized_message.data(), serialized_message.size());
    m_sionnaHelper->m_zmq_socket.send(zmq_message, zmq::send_flags::none);

    // Receive the reply message
    zmq::message_t zmq_reply;
    zmq::recv_result_t result = m_sionnaHelper->m_zmq_socket.recv(zmq_reply, zmq::recv_flags::none);
    
    NS_ASSERT_MSG(result, "Failed to receive reply after propagation request message.");

    // The wire buffer is already received, but reject before Protobuf parsing/copies.
    CheckLimit(zmq_reply.size(), m_limits.maxReplyBytes, "reply bytes");
    CheckLimit(zmq_reply.size(), std::numeric_limits<int>::max(), "Protobuf parse bytes");
    // Check if the reply message is a propagation response
    ns3sionna::Wrapper reply_wrapper;
    if (!reply_wrapper.ParseFromArray(zmq_reply.data(), zmq_reply.size()) ||
        !reply_wrapper.has_channel_state_response())
    {
        throw std::invalid_argument("Invalid channel-state response wrapper: " +
                                    reply_wrapper.sim_ack().error_msg());
    }

    // Extract the delay, loss and time to live value
    const ns3sionna::ChannelStateResponse& csi_response = reply_wrapper.channel_state_response();

    NS_LOG_INFO("ns3sionna::Req CSI data from sionna: #samples=" << csi_response.csi_size());

    // Preflight the entire batch before any CFR/cache allocation. Expired samples
    // still count toward decode limits; only retained samples count toward storage.
    uint64_t batchCount = 0, batchEntries = 0, payload = 0, retained = 0;
    for (const auto& state : csi_response.csi())
    {
        for (const auto& rx : state.rx_nodes())
        {
            const auto count = (rx.frequencies_size() == 0 && rx.csi_real_size() == 0 &&
                                rx.csi_imag_size() == 0) ? 0 :
                ValidateChannelDimensions(rx.frequencies_size(),
                    rx.num_rx_antennas() == 0 ? 1 : rx.num_rx_antennas(),
                    rx.num_tx_antennas() == 0 ? 1 : rx.num_tx_antennas(), m_limits);
            batchCount = CheckedSum(batchCount, count);
            batchEntries = CheckedSum(batchEntries, 1);
            if (state.end_time() >= static_cast<uint64_t>(current_time.GetNanoSeconds()))
            {
                retained = CheckedSum(retained, 1);
                payload = CheckedSum(payload, CheckedSum(
                    CheckedProduct(count, sizeof(std::complex<double>)),
                    CheckedProduct(rx.frequencies_size(), sizeof(int))));
            }
        }
    }
    CheckLimit(batchCount, m_limits.maxBatchCoefficients, "batch coefficients");
    CheckLimit(batchCount, std::numeric_limits<std::size_t>::max() / sizeof(std::complex<double>),
               "batch CFR allocation bytes");
    CheckLimit(batchEntries, std::numeric_limits<std::size_t>::max() /
               sizeof(std::pair<CacheKey, CacheEntry>), "batch entry allocation bytes");
    CheckLimit(batchEntries, m_limits.maxSnapshots, "reply snapshots");
    if (m_caching)
    {
        CheckLimit(CheckedSum(m_storage.snapshots, retained), m_limits.maxSnapshots,
                   "retained snapshots");
        CheckLimit(CheckedSum(m_storage.payloadBytes, payload), m_limits.maxPayloadBytes,
                   "retained payload bytes");
    }

    // Validate and decode every sample before committing any cache entry.
    std::vector<std::pair<CacheKey, CacheEntry>> pendingEntries;
    for (int csi_i=0; csi_i < csi_response.csi_size(); csi_i++) {
        const auto& csiMessage = csi_response.csi(csi_i);
        if (csiMessage.start_time() > csiMessage.end_time() ||
            csiMessage.end_time() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            csiMessage.rx_nodes_size() == 0)
        {
            throw std::invalid_argument("Invalid channel snapshot validity or empty receivers");
        }
        std::set<uint32_t> receivers;
        bool hasUserIdentity = false;
        for (const auto& rxMessage : csiMessage.rx_nodes())
        {
            hasUserIdentity = hasUserIdentity || rxMessage.has_user_identity();
        }
        if (hasUserIdentity)
        {
            DeserializeMultiUserChannelSnapshot(csiMessage, m_limits);
        }

        Time start_time = NanoSeconds(csi_response.csi(csi_i).start_time());
        Time end_time = NanoSeconds(csi_response.csi(csi_i).end_time());

        NS_LOG_DEBUG("\t\t: CSI ts= (" << start_time.GetNanoSeconds() << "ns"
            << " - " << end_time.GetNanoSeconds() << "ns), delta="
            << (end_time - start_time).GetNanoSeconds() << "ns");

        google::protobuf::uint32 txId = csi_response.csi(csi_i).tx_node().id();
        auto txPos = csi_response.csi(csi_i).tx_node().position();

        for (int rx_i=0; rx_i < csi_response.csi(csi_i).rx_nodes_size(); rx_i++) {
            const auto& rxMessage = csi_response.csi(csi_i).rx_nodes(rx_i);
            if (rxMessage.id() == txId || !receivers.insert(rxMessage.id()).second ||
                !std::isfinite(rxMessage.wb_loss()) ||
                rxMessage.delay() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            {
                throw std::invalid_argument("Invalid receiver endpoints, loss or delay");
            }
            Time delay = NanoSeconds(rxMessage.delay());
            double wb_loss = rxMessage.wb_loss();

            google::protobuf::uint32 rxId = rxMessage.id();
            auto rxPos = rxMessage.position();
            MimoChannelMatrix channel = DeserializeChannelMatrix(rxMessage, m_limits);
            uint32_t num_ofdm_subcarrier = channel.GetNumSubcarriers();

            NS_LOG_DEBUG("\t\t: Response (delay: " << delay << ", loss: " << wb_loss << ")"
              << " (TxId: " << txId << " [" << csi_response.csi(csi_i).tx_node().position().x()
              << "," << csi_response.csi(csi_i).tx_node().position().y() << "," << csi_response.csi(csi_i).tx_node().position().z() << "] -> "
              << rxId << " [" << rxPos.x() << "," << rxPos.y() << "," << rxPos.z()
              << ",NSC=" << num_ofdm_subcarrier
              << ",NRX=" << channel.GetNumRxAntennas()
              << ",NTX=" << channel.GetNumTxAntennas() << "])");

            // Add the info from all other receivers to the cache
            CacheKey otherkey = CacheKey(txId, rxId);
            CacheEntry entry = CacheEntry(delay, wb_loss, start_time, end_time, channel,
                txId, rxId, Vector(txPos.x(), txPos.y(), txPos.z()),
                Vector(rxPos.x(), rxPos.y(), rxPos.z()), rxMessage.has_user_identity());

            // Frequencies remain one value per subcarrier, independent of antennas.
            for (int i = 0; i < rxMessage.frequencies_size(); i++)
            {
                entry.m_freq.emplace_back(rxMessage.frequencies(i));
            }

            pendingEntries.emplace_back(otherkey, std::move(entry));
        }
    }

    const auto fresh = std::find_if(pendingEntries.begin(), pendingEntries.end(),
        [&](const auto& pending) {
            return Matches(pending.second, node_a->GetId(), node_b->GetId(), current_time);
        });
    if (fresh == pendingEntries.end())
    {
        throw std::invalid_argument("Fresh response has no valid entry for the requested link");
    }
    if (!m_caching)
    {
        return fresh->second; // Fully validated; retain no snapshots.
    }
    for (auto& pending : pendingEntries)
    {
        if (pending.second.m_end_time < current_time)
        {
            continue; // A valid batch may contain already obsolete samples.
        }
        auto& entries = m_cache[pending.first];
        const auto oldCapacity = entries.capacity();
        entries.push_back(std::move(pending.second));
        const auto& entry = entries.back();
        ++m_storage.snapshots;
        m_storage.payloadBytes += PayloadBytes(entry);
        m_storage.capacityBytes += CapacityBytes(entry) +
                                  (entries.capacity() - oldCapacity) * sizeof(CacheEntry);
        m_nextExpiry = std::min(m_nextExpiry, entry.m_end_time);
    }
    m_storage.links = m_cache.size();
    UpdateStoragePeaks();

    // get result from cache
    CacheKey key2 = CacheKey(node_a->GetId(), node_b->GetId());
    auto cache_it = m_cache.find(key2);
    if (cache_it != m_cache.end())
    {
        // iterate over all stored entries for that link
        for (CacheEntry c_entry : cache_it->second) {
            // If delay and loss exist in the cache, check if the entry is not outdated
            if (Matches(c_entry, node_a->GetId(), node_b->GetId(), current_time))
            {
                // Return cache entry as the value is still fresh
                return c_entry;
            }
        }
    }
    throw std::logic_error("Validated requested channel missing after cache commit");
}

} // namespace ns3
