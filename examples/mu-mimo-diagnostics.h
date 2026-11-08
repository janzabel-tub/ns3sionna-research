/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SIONNA_EXAMPLE_MU_MIMO_DIAGNOSTICS_H
#define SIONNA_EXAMPLE_MU_MIMO_DIAGNOSTICS_H

#include "ns3/ipv4-header.h"
#include "ns3/llc-snap-header.h"
#include "ns3/net-device-container.h"
#include "ns3/qos-txop.h"
#include "ns3/simulator.h"
#include "ns3/sionna-mimo-helper.h"
#include "ns3/tcp-header.h"
#include "ns3/wifi-mac-queue.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy-state-helper.h"
#include "ns3/wifi-psdu.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

/** Passive, opt-in records for the single-link MU performance example. */
class MuMimoDiagnostics
{
  public:
    MuMimoDiagnostics(const std::filesystem::path& directory, bool enabled)
        : m_enabled(enabled)
    {
        if (!enabled) return;
        auto open = [&directory](std::ofstream& stream, const char* name) {
            stream.open(directory / name);
            if (!stream) throw std::runtime_error(std::string("Cannot write ") + name);
        };
        open(m_scheduler, "mu_scheduler.csv");
        open(m_tx, "ppdu_tx.csv");
        open(m_phy, "phy_tx.csv");
        open(m_mac, "mac_events.csv");
        m_scheduler << "Time_ns,DecisionId,Outcome,Detail,Tid,Cw,LastBackoffDraw,Available_ns,InitialFrame,Width_MHz,UserId,NodeId,StaId,Eligibility,QueuePackets,QueueBytes\n";
        m_tx << "Time_ns,TxId,NodeId,StaId,Peer,Kind,Preamble,Users,Width_MHz,GI_ns,Nss,Mcs,Ldpc,RuType,RuIndex,PsduBytes,Mpdus,RetryMpdus,DurationId_ns,CalculatedDuration_ns,Cw,QueueBytes,FirstSeq,LastSeq,Power_W,DecisionId,SchedulerOutcome\n";
        m_phy << "Time_ns,TxId,NodeId,Duration_ns\n";
        m_mac << "Time_ns,NodeId,TxId,Event,Peer,Tid,Sequence,Retry,Value,Cw,PacketUid\n";
        m_tx << std::setprecision(17);
    }

    ns3::SionnaMimoDownlinkDiagnostic SchedulerCallback()
    {
        if (!m_enabled) return {};
        return [this](const ns3::SionnaMimoDownlinkDecision& d) {
            m_lastDecision = d;
            for (const auto& u : d.users)
                m_scheduler << d.time.GetNanoSeconds() << ',' << d.id << ',' << d.outcome
                    << ',' << std::quoted(d.detail, '"', '"') << ',' << unsigned(d.tid)
                    << ',' << Cw(m_apNode) << ',' << m_backoff.at(m_apNode) << ',' << d.availableTime.GetNanoSeconds()
                    << ',' << d.initialFrame << ',' << d.allowedWidth << ',' << u.userId
                    << ',' << u.nodeId << ',' << u.staId << ',' << u.eligibility
                    << ',' << u.queuedPackets << ',' << u.queuedBytes << '\n';
        };
    }

    void Connect(ns3::NetDeviceContainer devices)
    {
        if (!m_enabled) return;
        for (uint32_t i = 0; i < devices.GetN(); ++i)
        {
            auto device = ns3::DynamicCast<ns3::WifiNetDevice>(devices.Get(i));
            const auto node = device->GetNode()->GetId();
            m_devices[node] = device;
            m_cw[node] = device->GetMac()->GetQosTxop(ns3::AC_BE)->GetMinCw(0);
            m_backoff[node] = 0;
            if (device->GetMac()->GetTypeOfStation() == ns3::AP) m_apNode = node;
            auto connect = [](ns3::Ptr<ns3::Object> object, const char* name,
                              const ns3::CallbackBase& callback) {
                if (!object->TraceConnectWithoutContext(name, callback))
                    throw std::runtime_error(std::string("Cannot connect diagnostic trace ") + name);
            };
            connect(device->GetPhy(), "PhyTxPsduBegin",
                    ns3::MakeCallback(&MuMimoDiagnostics::Tx, this).Bind(node));
            connect(device->GetPhy()->GetState(), "State",
                    ns3::MakeCallback(&MuMimoDiagnostics::PhyState, this).Bind(node));
            connect(device->GetPhy(), "PhyRxDrop",
                    ns3::MakeCallback(&MuMimoDiagnostics::RxDrop, this).Bind(node));
            auto mac = device->GetMac();
            connect(mac, "AckedMpdu", ns3::MakeCallback(&MuMimoDiagnostics::Outcome, this)
                    .Bind(node, std::string("acked")));
            connect(mac, "NAckedMpdu", ns3::MakeCallback(&MuMimoDiagnostics::Outcome, this)
                    .Bind(node, std::string("nacked")));
            connect(mac, "DroppedMpdu", ns3::MakeCallback(&MuMimoDiagnostics::Dropped, this).Bind(node));
            connect(mac, "MpduResponseTimeout", ns3::MakeCallback(&MuMimoDiagnostics::MpduTimeout, this).Bind(node));
            connect(mac, "PsduResponseTimeout", ns3::MakeCallback(&MuMimoDiagnostics::PsduTimeout, this).Bind(node));
            connect(mac, "PsduMapResponseTimeout", ns3::MakeCallback(&MuMimoDiagnostics::MapTimeout, this).Bind(node));
            connect(mac->GetQosTxop(ns3::AC_BE), "CwTrace",
                    ns3::MakeCallback(&MuMimoDiagnostics::Contention, this).Bind(node, std::string("cw")));
            connect(mac->GetQosTxop(ns3::AC_BE), "BackoffTrace",
                    ns3::MakeCallback(&MuMimoDiagnostics::Contention, this).Bind(node, std::string("backoff")));
        }
    }

  private:
    using Key = std::tuple<uint32_t, ns3::Mac48Address, uint8_t, uint16_t, uint64_t>;
    static Key AttemptKey(uint32_t node, const ns3::WifiMacHeader& header, uint64_t uid)
    {
        return {node, header.GetAddr1(), header.IsQosData() ? header.GetQosTid() : uint8_t(255),
                header.GetSequenceNumber(), uid};
    }

    uint32_t Cw(uint32_t node) const
    {
        return m_cw.at(node);
    }

    static std::string Kind(ns3::Ptr<const ns3::WifiMpdu> mpdu)
    {
        const auto& h = mpdu->GetHeader();
        if (h.IsAck()) return "mac_ack";
        if (h.IsBlockAck()) return "block_ack";
        if (h.IsBlockAckReq()) return "bar";
        if (h.IsMgt()) return "management";
        if (!h.IsData()) return "control";
        auto packet = mpdu->GetPacket()->Copy();
        ns3::LlcSnapHeader llc;
        ns3::Ipv4Header ip;
        ns3::TcpHeader tcp;
        if (packet->GetSize() >= llc.GetSerializedSize() && packet->RemoveHeader(llc) &&
            llc.GetType() == 0x0800 && packet->GetSize() >= 20 && packet->RemoveHeader(ip) &&
            ip.GetProtocol() == 6 && packet->GetSize() >= 20 && packet->RemoveHeader(tcp))
            return packet->GetSize() ? "tcp_data" :
                (tcp.GetFlags() == ns3::TcpHeader::ACK ? "tcp_ack" : "tcp_control");
        return "data";
    }

    void Tx(uint32_t node, ns3::WifiConstPsduMap psdus, ns3::WifiTxVector tx, double power)
    {
        const auto id = ++m_txId;
        m_pending[node] = id;
        const auto duration = ns3::WifiPhy::CalculateTxDuration(
            psdus, tx, m_devices.at(node)->GetPhy()->GetPhyBand());
        for (const auto& [staId, psdu] : psdus)
        {
            uint32_t retries = 0;
            for (const auto& mpdu : *psdu)
            {
                retries += mpdu->GetHeader().IsRetry();
                if (mpdu->GetHeader().IsData())
                    m_attempts[AttemptKey(node, mpdu->GetHeader(), mpdu->GetPacket()->GetUid())] = id;
            }
            const auto& first = psdu->GetHeader(0);
            const uint8_t tid = first.IsQosData() ? first.GetQosTid() : 255;
            uint32_t queuedBytes = 0;
            if (tid != 255)
            {
                const ns3::WifiContainerQueueId queue{
                    ns3::WIFI_QOSDATA_QUEUE, ns3::WIFI_UNICAST, first.GetAddr1(), tid};
                queuedBytes = m_devices.at(node)->GetMac()->GetQosTxop(tid)->GetWifiMacQueue()->GetNBytes(queue);
            }
            m_tx << ns3::Simulator::Now().GetNanoSeconds() << ',' << id << ',' << node << ','
                << staId << ',' << first.GetAddr1() << ',' << Kind(*psdu->begin()) << ','
                << tx.GetPreambleType() << ',' << psdus.size() << ',' << tx.GetChannelWidth()
                << ',' << tx.GetGuardInterval() << ',' << unsigned(tx.GetNss(staId)) << ',';
            if (tx.GetMode(staId).GetModulationClass() >= ns3::WIFI_MOD_CLASS_HT)
                m_tx << unsigned(tx.GetMode(staId).GetMcsValue());
            else m_tx << -1;
            m_tx << ',' << tx.IsLdpc() << ',';
            if (tx.IsMu()) m_tx << unsigned(tx.GetRu(staId).GetRuType()) << ',' << tx.GetRu(staId).GetIndex();
            else m_tx << "-1,0";
            m_tx << ',' << psdu->GetSize() << ',' << psdu->GetNMpdus() << ',' << retries
                << ',' << first.GetDuration().GetNanoSeconds() << ',' << duration.GetNanoSeconds()
                << ',' << Cw(node) << ',' << queuedBytes << ',' << first.GetSequenceNumber()
                << ',' << psdu->GetHeader(psdu->GetNMpdus() - 1).GetSequenceNumber() << ',' << power;
            const bool selectedNow = node == m_apNode && m_lastDecision.id &&
                                     m_lastDecision.time == ns3::Simulator::Now();
            std::string outcome = "not_ap_qos";
            if (node == m_apNode && first.IsQosData())
                outcome = selectedNow ? m_lastDecision.outcome :
                    !m_devices.at(node)->GetMac()->GetBaAgreementEstablishedAsOriginator(first.GetAddr1(), tid)
                        ? "native_missing_block_ack" : "native_bypass_or_deferred";
            m_tx << ',' << (selectedNow && first.IsQosData() ? m_lastDecision.id : 0)
                 << ',' << outcome << '\n';
        }
    }

    void PhyState(uint32_t node, ns3::Time start, ns3::Time duration, WifiPhyState state)
    {
        if (state == WifiPhyState::TX)
            m_phy << start.GetNanoSeconds() << ',' << m_pending.at(node) << ',' << node
                  << ',' << duration.GetNanoSeconds() << '\n';
    }

    void Event(uint32_t node, const std::string& event, const ns3::WifiMacHeader& h,
               uint64_t uid, int value = 0)
    {
        uint32_t transmitter = node;
        if (event == "rx_drop_addressed")
            for (const auto& [id, device] : m_devices)
                if (device->GetMac()->GetAddress() == h.GetAddr2()) transmitter = id;
        auto found = m_attempts.find(AttemptKey(transmitter, h, uid));
        m_mac << ns3::Simulator::Now().GetNanoSeconds() << ',' << node << ','
            << (found == m_attempts.end() ? 0 : found->second) << ',' << event << ','
            << (event == "rx_drop_addressed" ? h.GetAddr2() : h.GetAddr1())
            << ',' << (h.IsQosData() ? int(h.GetQosTid()) : -1) << ',' << h.GetSequenceNumber()
            << ',' << h.IsRetry() << ',' << value << ',' << Cw(node) << ',' << uid << '\n';
    }
    void Outcome(uint32_t node, std::string event, ns3::Ptr<const ns3::WifiMpdu> mpdu)
    {
        Event(node, event, mpdu->GetOriginal()->GetHeader(), mpdu->GetPacket()->GetUid());
    }
    void Dropped(uint32_t node, ns3::WifiMacDropReason reason, ns3::Ptr<const ns3::WifiMpdu> mpdu)
    {
        Event(node, "dropped", mpdu->GetOriginal()->GetHeader(), mpdu->GetPacket()->GetUid(), int(reason));
    }
    void MpduTimeout(uint32_t node, uint8_t reason, ns3::Ptr<const ns3::WifiMpdu> mpdu,
                     const ns3::WifiTxVector&)
    {
        Event(node, "mpdu_timeout", mpdu->GetOriginal()->GetHeader(), mpdu->GetPacket()->GetUid(), reason);
    }
    void PsduTimeout(uint32_t node, uint8_t reason, ns3::Ptr<const ns3::WifiPsdu> psdu,
                     const ns3::WifiTxVector&)
    {
        for (const auto& mpdu : *psdu)
            Event(node, "psdu_timeout", mpdu->GetOriginal()->GetHeader(), mpdu->GetPacket()->GetUid(), reason);
    }
    void MapTimeout(uint32_t node, uint8_t reason, ns3::WifiPsduMap* psdus,
                    const std::set<ns3::Mac48Address>* missing, std::size_t)
    {
        for (const auto& [staId, psdu] : *psdus)
            if (missing->count(psdu->GetAddr1()))
                for (const auto& mpdu : *psdu)
                    Event(node, "mu_response_timeout", mpdu->GetOriginal()->GetHeader(), mpdu->GetPacket()->GetUid(), reason);
    }
    void Contention(uint32_t node, std::string event, uint32_t value, uint8_t)
    {
        if (event == "cw") m_cw[node] = value;
        else m_backoff[node] = value;
        m_mac << ns3::Simulator::Now().GetNanoSeconds() << ',' << node << ",0," << event
              << ",,-1,-1,0," << value << ',' << Cw(node) << ",0\n";
    }
    void RxDrop(uint32_t node, ns3::Ptr<const ns3::Packet> packet, ns3::WifiPhyRxfailureReason reason)
    {
        ns3::WifiMacHeader header;
        if (packet->PeekHeader(header) && header.IsData() &&
            header.GetAddr1() == m_devices.at(node)->GetMac()->GetAddress())
            Event(node, "rx_drop_addressed", header, packet->GetUid(), int(reason));
    }

    bool m_enabled;
    uint64_t m_txId{0};
    uint32_t m_apNode{0};
    ns3::SionnaMimoDownlinkDecision m_lastDecision;
    std::ofstream m_scheduler, m_tx, m_phy, m_mac;
    std::map<uint32_t, ns3::Ptr<ns3::WifiNetDevice>> m_devices;
    std::map<uint32_t, uint64_t> m_pending;
    std::map<uint32_t, uint32_t> m_cw, m_backoff;
    std::map<Key, uint64_t> m_attempts;
};
#endif
