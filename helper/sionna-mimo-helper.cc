/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "sionna-mimo-helper.h"

#include "sionna-helper.h"
#include "../model/sionna-spectrum-propagation-loss-model.h"
#include "../model/he-data-tone-mask.h"
#include "ns3/minstrel-ht-wifi-manager.h"
#include "ns3/error-rate-model.h"
#include "ns3/spectrum-value.h"
#include "ns3/wifi-spectrum-value-helper.h"
#include "ns3/wifi-acknowledgment.h"
#include "ns3/wifi-protection.h"
#include "ns3/ap-wifi-mac.h"
#include "ns3/double.h"
#include "ns3/he-configuration.h"
#include "ns3/he-capabilities.h"
#include "ns3/he-frame-exchange-manager.h"
#include "ns3/interference-helper.h"
#include "ns3/pointer.h"
#include "ns3/mpdu-aggregator.h"
#include "ns3/qos-txop.h"
#include "ns3/simulator.h"
#include "ns3/spectrum-wifi-phy.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-mac-queue.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-remote-station-manager.h"
#include "ns3/wifi-utils.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace ns3
{

namespace
{

constexpr uint32_t WIFI_MAX_ANTENNAS = 8;
constexpr uint8_t HE_MAX_MCS = 11;

[[noreturn]] void
Reject(const std::string& reason)
{
    throw std::invalid_argument("SionnaMimoHelper: " + reason);
}

uint32_t
GetPolarizationCount(SionnaMimoPolarization polarization)
{
    switch (polarization)
    {
    case SionnaMimoPolarization::VERTICAL:
    case SionnaMimoPolarization::HORIZONTAL:
        return 1;
    case SionnaMimoPolarization::DUAL:
        return 2;
    }
    Reject("unknown antenna polarization");
}

std::string
ToSionnaPattern(SionnaMimoAntennaPattern pattern)
{
    switch (pattern)
    {
    case SionnaMimoAntennaPattern::ISOTROPIC:
        return "iso";
    case SionnaMimoAntennaPattern::DIPOLE:
        return "dipole";
    case SionnaMimoAntennaPattern::HALF_WAVE_DIPOLE:
        return "hw_dipole";
    case SionnaMimoAntennaPattern::TR38901:
        return "tr38901";
    }
    Reject("unknown antenna element pattern");
}

std::string
ToSionnaPolarization(SionnaMimoPolarization polarization)
{
    switch (polarization)
    {
    case SionnaMimoPolarization::VERTICAL:
        return "V";
    case SionnaMimoPolarization::HORIZONTAL:
        return "H";
    case SionnaMimoPolarization::DUAL:
        return "VH";
    }
    Reject("unknown antenna polarization");
}

void
ValidatePattern(SionnaMimoAntennaPattern pattern)
{
    switch (pattern)
    {
    case SionnaMimoAntennaPattern::ISOTROPIC:
    case SionnaMimoAntennaPattern::DIPOLE:
    case SionnaMimoAntennaPattern::HALF_WAVE_DIPOLE:
    case SionnaMimoAntennaPattern::TR38901:
        return;
    }
    Reject("unknown antenna element pattern");
}

void
ValidateArray(const SionnaMimoArrayConfiguration& array, const std::string& role)
{
    if (array.numRows == 0 || array.numColumns == 0)
    {
        Reject(role + " array rows and columns must be positive");
    }
    if (!std::isfinite(array.verticalSpacing) || array.verticalSpacing <= 0.0 ||
        !std::isfinite(array.horizontalSpacing) || array.horizontalSpacing <= 0.0)
    {
        Reject(role + " array spacings must be finite and positive");
    }
    ValidatePattern(array.pattern);
    const uint64_t numAntennas = array.GetNumAntennas();
    if (numAntennas > WIFI_MAX_ANTENNAS)
    {
        Reject(role + " array exceeds ns-3's limit of eight Wi-Fi antennas");
    }
}

void
ValidateOrientation(const SionnaMimoOrientation& orientation)
{
    if (!std::isfinite(orientation.azimuthDegrees) ||
        orientation.azimuthDegrees < -180.0 || orientation.azimuthDegrees > 180.0)
    {
        Reject("node azimuth must be finite and in [-180, 180] degrees");
    }
    if (!std::isfinite(orientation.elevationDegrees) ||
        orientation.elevationDegrees < -90.0 || orientation.elevationDegrees > 90.0)
    {
        Reject("node elevation must be finite and in [-90, 90] degrees");
    }
    if (!std::isfinite(orientation.rollDegrees) || orientation.rollDegrees < -180.0 ||
        orientation.rollDegrees > 180.0)
    {
        Reject("node roll must be finite and in [-180, 180] degrees");
    }
}

void
ValidateReceiverType(SionnaMimoReceiverType receiverType)
{
    if (receiverType != SionnaMimoReceiverType::LINEAR_MMSE)
    {
        Reject("only the linear MMSE receiver is supported");
    }
}

void
ValidateNormalization(SionnaMimoChannelNormalization normalization)
{
    if (normalization != SionnaMimoChannelNormalization::UNIT_AVERAGE_PAIR_POWER)
    {
        Reject("only joint unit-average CFR normalization is supported");
    }
}

void
ValidateEffectiveSinrModel(SionnaMimoEffectiveSinrModel model)
{
    if (model != SionnaMimoEffectiveSinrModel::EESM &&
        model != SionnaMimoEffectiveSinrModel::MEAN_UNCODED_BER &&
        model != SionnaMimoEffectiveSinrModel::LEGACY_SISO)
    {
        Reject("unsupported HE effective SINR mapping");
    }
}

void
ValidateEesmBeta(uint8_t mcs, double betaLinear)
{
    if (mcs > HE_MAX_MCS)
    {
        Reject("EESM beta MCS must be in [0, 11]");
    }
    if (!std::isfinite(betaLinear) || betaLinear <= 0.0)
    {
        Reject("EESM beta must be finite and positive");
    }
}

void
ValidateBeamformingOptions(const SionnaMimoBeamformingConfiguration& beamforming)
{
    switch (beamforming.method)
    {
    case SionnaMimoBeamformingMethod::MRT:
    case SionnaMimoBeamformingMethod::SVD:
        break;
    default:
        Reject("unknown SU beamforming method");
    }

    switch (beamforming.svd.powerAllocation)
    {
    case MimoPowerAllocationMode::EQUAL_POWER:
    case MimoPowerAllocationMode::WATER_FILLING:
        break;
    default:
        Reject("unknown SVD power-allocation mode");
    }
    if (!std::isfinite(beamforming.svd.referenceSnrLinear) ||
        beamforming.svd.referenceSnrLinear <= 0.0)
    {
        Reject("SVD reference SNR must be finite and positive");
    }
    if (!std::isfinite(beamforming.svd.noisePower) ||
        beamforming.svd.noisePower <= 0.0)
    {
        Reject("SVD noise power must be finite and positive");
    }
    if (!std::isfinite(beamforming.svd.rankTolerance) ||
        beamforming.svd.rankTolerance <= 0.0 || beamforming.svd.rankTolerance >= 1.0)
    {
        Reject("SVD rank tolerance must be finite and in (0, 1)");
    }

    switch (beamforming.feedback.standard)
    {
    case MimoFeedbackStandard::HE:
    case MimoFeedbackStandard::EHT:
        break;
    default:
        Reject("unknown feedback standard");
    }
    switch (beamforming.feedback.codebook)
    {
    case MimoFeedbackCodebook::UNQUANTIZED:
    case MimoFeedbackCodebook::SU_COARSE:
    case MimoFeedbackCodebook::SU_FINE:
        break;
    default:
        Reject("unknown SU feedback codebook");
    }

    if (beamforming.csiTiming.updateInterval < NanoSeconds(0) ||
        beamforming.csiTiming.feedbackDelay < NanoSeconds(0))
    {
        Reject("CSI update interval and feedback delay must be non-negative");
    }
    const bool hasAging = !beamforming.csiTiming.updateInterval.IsZero() ||
                          !beamforming.csiTiming.feedbackDelay.IsZero();

    if (beamforming.method == SionnaMimoBeamformingMethod::MRT)
    {
        if (beamforming.feedback.standard != MimoFeedbackStandard::HE ||
            beamforming.feedback.codebook != MimoFeedbackCodebook::UNQUANTIZED ||
            hasAging)
        {
            Reject("MRT supports only unquantized current perfect CSI");
        }
        if (beamforming.svd.powerAllocation != MimoPowerAllocationMode::EQUAL_POWER ||
            beamforming.svd.referenceSnrLinear != 1.0 ||
            beamforming.svd.noisePower != 1.0 ||
            beamforming.svd.rankTolerance != MimoSvdPrecodingOptions::DEFAULT_RANK_TOLERANCE)
        {
            Reject("MRT does not accept SVD or water-filling options");
        }
    }
    else if (beamforming.svd.powerAllocation == MimoPowerAllocationMode::WATER_FILLING &&
             (beamforming.feedback.codebook != MimoFeedbackCodebook::UNQUANTIZED ||
              hasAging))
    {
        Reject("water-filling cannot be combined with quantized feedback or CSI aging");
    }
}

MimoCsiArrayIdentity
MakeCsiArrayIdentity(const SionnaMimoArrayConfiguration& array,
                     const SionnaMimoOrientation& orientation)
{
    MimoCsiArrayIdentity identity;
    identity.numRows = array.numRows;
    identity.numColumns = array.numColumns;
    identity.verticalSpacing = array.verticalSpacing;
    identity.horizontalSpacing = array.horizontalSpacing;
    identity.pattern = ToSionnaPattern(array.pattern);
    identity.polarization = ToSionnaPolarization(array.polarization);
    identity.azimuthDegrees = orientation.azimuthDegrees;
    identity.elevationDegrees = orientation.elevationDegrees;
    identity.rollDegrees = orientation.rollDegrees;
    return identity;
}

MultiUserChannelSnapshot
SubsetSnapshot(const MultiUserChannelSnapshot& snapshot, const std::set<uint32_t>& userIds)
{
    MultiUserChannelSnapshot subset{snapshot.csiTimestamp, snapshot.direction, {}};
    for (const auto& user : snapshot.users)
    {
        if (userIds.count(user.userId) != 0)
        {
            subset.users.push_back(user);
        }
    }
    if (subset.users.size() != userIds.size())
    {
        Reject("the downlink snapshot does not contain every scheduled stable user ID");
    }
    return subset;
}

class SionnaMimoDownlinkScheduler final : public MultiUserScheduler
{
  public:
    void Configure(Ptr<WifiNetDevice> apDevice,
                   std::vector<SionnaMimoStationBinding> stationBindings,
                   Ptr<SionnaSpectrumPropagationLossModel> spectrumModel,
                   SionnaMimoDownlinkSnapshotProvider snapshotProvider,
                   DlMuMimoRuntimeConfiguration runtimeConfiguration,
                   SionnaMimoDownlinkScheduling scheduling,
                   SionnaMimoDownlinkDiagnostic diagnostic,
                   SionnaMimoDownlinkRateControl rateControl,
                   const SionnaMimoConfiguration& configuration)
    {
        std::sort(stationBindings.begin(),
                  stationBindings.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.userId < rhs.userId; });
        m_apDevice = apDevice;
        m_stationBindings = std::move(stationBindings);
        m_spectrumModel = spectrumModel;
        m_snapshotProvider = std::move(snapshotProvider);
        m_runtimeConfiguration = runtimeConfiguration;
        m_runtime.emplace(std::move(runtimeConfiguration));
        m_scheduling = scheduling;
        m_diagnostic = std::move(diagnostic);
        m_rateControl = rateControl;
        m_rateMapping = CreateObject<HeSuMimoLinkAbstraction>();
        m_rateMapping->SetEffectiveSinrModel(configuration.effectiveSinrModel);
        for (const auto& [mcs, beta] : configuration.eesmBetaLinear) m_rateMapping->SetEesmBeta(mcs, beta);
        for (const auto& [key, offset] : configuration.awgnSnrOffsetsDb)
            m_rateMapping->SetAwgnSnrOffsetDb(key, offset);
        m_phy = apDevice->GetPhy();
        m_phy->TraceConnectWithoutContext(
            "PhyTxPsduBegin",
            MakeCallback(&SionnaMimoDownlinkScheduler::InstallAtTransmissionTime, this));
    }

  private:
    struct Candidate
    {
        uint32_t userId;
        uint16_t staId;
        Mac48Address address;
        Ptr<WifiMpdu> mpdu;
        uint8_t mcs;
    };

    TxFormat SelectTxFormat() override
    {
        m_hasPendingDlMu = false;
        if (m_diagnostic)
        {
            m_decision = {};
            m_decision.id = ++m_decisionId;
            m_decision.time = Simulator::Now();
            m_decision.availableTime = m_availableTime;
            m_decision.initialFrame = m_initialFrame;
            m_decision.allowedWidth = m_allowedWidth;
            for (const auto& binding : m_stationBindings)
            {
                SionnaMimoDownlinkUserStatus status;
                status.userId = binding.userId;
                status.nodeId = binding.stationDevice->GetNode()->GetId();
                m_decision.users.push_back(status);
            }
        }
        if (m_scheduling == SionnaMimoDownlinkScheduling::SU_ROUND_ROBIN)
        {
            if (m_diagnostic)
            {
                m_decision.outcome = "policy_su";
                m_diagnostic(m_decision);
            }
            return SU_TX;
        }
        const bool mu = BuildDlMuInfo();
        if (m_diagnostic)
        {
            if (mu) m_decision.outcome = "mu_ready";
            m_diagnostic(m_decision);
        }
        return mu ? DL_MU_TX : SU_TX;
    }

    DlMuInfo ComputeDlMuInfo() override
    {
        NS_ABORT_MSG_IF(!m_hasPendingDlMu, "Sionna downlink scheduler has no prepared MU PPDU");
        m_hasPendingDlMu = false;
        return std::move(m_pendingDlMu);
    }

    UlMuInfo ComputeUlMuInfo() override
    {
        return UlMuInfo{};
    }

    bool RejectGroup(const std::string& reason, const std::string& detail = {})
    {
        if (m_diagnostic)
        {
            m_decision.outcome = reason;
            m_decision.detail = detail;
        }
        return false;
    }

    using RateTable = std::map<uint32_t, std::array<double, 10>>;

    Ptr<WifiNetDevice> Station(uint32_t userId) const
    {
        return std::find_if(m_stationBindings.begin(), m_stationBindings.end(),
            [userId](const auto& b) { return b.userId == userId; })->stationDevice;
    }

    WifiTxVector MuVector(const std::vector<Candidate>& users) const
    {
        WifiTxVector tx;
        tx.SetPreambleType(WIFI_PREAMBLE_HE_MU);
        tx.SetChannelWidth(m_allowedWidth);
        tx.SetGuardInterval(800);
        tx.SetBssColor(m_apMac->GetHeConfiguration()->GetBssColor());
        for (const auto& u : users)
            tx.SetHeMuUserInfo(u.staId, {{HeRu::GetRuType(m_allowedWidth), 1, true}, u.mcs, 1});
        return tx;
    }

    void UpdateRateSnapshot(const MultiUserChannelSnapshot& snapshot)
    {
        const auto mask = m_phy->GetTxMaskRejectionParams();
        auto psd = WifiSpectrumValueHelper::CreateHeOfdmTxPowerSpectralDensity(
            m_phy->GetFrequency(), m_allowedWidth, DbmToW(m_phy->GetTxPowerStart() + m_phy->GetTxGain()),
            m_phy->GetGuardBandwidth(m_allowedWidth), std::get<0>(mask), std::get<1>(mask), std::get<2>(mask));
        double power = 0;
        const auto center = (psd->GetValuesN() - 1) / 2;
        for (const auto& range : HeRu::GetSubcarrierGroup(m_allowedWidth, HeRu::GetRuType(m_allowedWidth), 1))
            for (int sc = range.first; sc <= range.second; ++sc)
                power += (*psd)[center + sc] * 78125.0;
        std::vector<double> rxGains;
        for (const auto& b : m_stationBindings) rxGains.push_back(b.stationDevice->GetPhy()->GetRxGain());
        bool same = m_rateSnapshot && power == m_ruPower && rxGains == m_rxGains &&
                    snapshot.users.size() == m_rateSnapshot->users.size();
        if (same)
            for (std::size_t i = 0; i < snapshot.users.size(); ++i)
            {
                const auto& a = snapshot.users[i];
                const auto& b = m_rateSnapshot->users[i];
                same &= a.userId == b.userId && a.widebandLossDb == b.widebandLossDb &&
                        a.frequencies == b.frequencies &&
                        a.channel.GetCoefficients() == b.channel.GetCoefficients();
            }
        if (!same)
        {
            m_rateCache.clear();
            m_rateSnapshot = snapshot;
            m_ruPower = power;
            m_rxGains = std::move(rxGains);
        }
    }

    const RateTable& GroupRates(const std::vector<Candidate>& users,
                                const MultiUserChannelSnapshot& snapshot)
    {
        std::vector<uint32_t> ids;
        for (const auto& u : users) ids.push_back(u.userId);
        std::sort(ids.begin(), ids.end());
        auto found = m_rateCache.find(ids);
        if (found != m_rateCache.end()) return found->second;
        const auto group = SubsetSnapshot(snapshot, std::set<uint32_t>(ids.begin(), ids.end()));
        std::vector<DlMuMimoStreamMapping> mappings;
        std::vector<DlMuMimoUserSnr> snrs;
        for (const auto& u : group.users)
        {
            mappings.push_back({u.userId, 0});
            const auto rx = Station(u.userId)->GetPhy();
            const auto budget = WifiLinkBudget::Calculate(
                WToDbm(m_ruPower) + rx->GetRxGain(), u.widebandLossDb, m_allowedWidth * 1e6, 290,
                m_spectrumModel->GetReceiverNoiseFigureDb());
            snrs.push_back({u.userId, budget.GetReferenceSnrLinear(), 0});
        }
        const auto precoding = m_runtimeConfiguration.precoder == DlMuMimoRuntimePrecoder::ZERO_FORCING
            ? DlMuMimoPrecodingMatrix::ZeroForcing(group, mappings, m_runtimeConfiguration.rankTolerance)
            : DlMuMimoPrecodingMatrix::RegularizedZeroForcing(group, mappings,
                m_runtimeConfiguration.rzfAlpha, m_runtimeConfiguration.rankTolerance);
        const auto link = DlMuMimoLinkModel::Calculate(group, precoding, snrs);
        const auto bins = HeDataToneMask::GetDataBins(m_allowedWidth, HeRu::GetRuType(m_allowedWidth), 1,
            group.users.front().channel.GetNumSubcarriers() / 2);
        RateTable rates;
        for (const auto& u : group.users)
        {
            std::vector<double> tones;
            for (auto sc : bins) tones.push_back(link.GetUserById(u.userId).GetSinrLinear(sc, 0));
            for (uint8_t mcs = 0; mcs < 10; ++mcs)
                if (m_rateMapping->GetEffectiveSinrModel() != SionnaMimoEffectiveSinrModel::EESM ||
                    m_rateMapping->HasEesmBeta(mcs))
                    rates[u.userId][mcs] = m_rateMapping->CalculateEffectiveSinr(tones, mcs);
        }
        return m_rateCache.emplace(std::move(ids), std::move(rates)).first->second;
    }

    // Read-only estimate before aggregation; actual PSDU sizes refine rates once afterward.
    uint32_t PlannedSize(const Candidate& u, const WifiTxVector& tx) const
    {
        const auto tid = u.mpdu->GetHeader().GetQosTid();
        const WifiContainerQueueId queue{WIFI_QOSDATA_QUEUE, WIFI_UNICAST, u.address, tid};
        const auto bytes = m_edca->GetWifiMacQueue()->GetNBytes(queue);
        const auto packets = bytes ? m_edca->GetWifiMacQueue()->GetNPackets(queue) : 0;
        const uint32_t limit = m_apMac->GetMaxAmpduSize(m_edca->GetAccessCategory());
        const auto payloadTime = GetPpduMaxTime(WIFI_PREAMBLE_HE_MU) -
            WifiPhy::CalculatePhyPreambleAndHeaderDuration(tx) - NanoSeconds(12800 + tx.GetGuardInterval());
        const uint32_t durationLimit = tx.GetMode(u.staId).GetDataRate(tx, u.staId) *
            payloadTime.GetSeconds() / 8;
        return std::max(u.mpdu->GetSize() + 4,
            std::min({bytes + 4 * packets, limit ? limit : u.mpdu->GetSize() + 4, durationLimit}));
    }

    double RateScore(const std::vector<Candidate>& users, const RateTable& rates,
                     const std::map<uint16_t, uint32_t>& sizes = {})
    {
        WifiTxParameters params;
        params.m_txVector = MuVector(users);
        const auto available = m_initialFrame ? Time::Min() : m_availableTime;
        Time duration;
        double delivered = 0;
        for (const auto& u : users)
        {
            const auto capabilities = GetWifiRemoteStationManager(m_linkId)->GetStationHeCapabilities(u.address);
            if (!capabilities || !capabilities->IsSupportedRxMcs(u.mcs)) return 0;
            const auto sinr = rates.at(u.userId)[u.mcs];
            if (sinr <= 0 || !GetHeFem(m_linkId)->TryAddMpdu(u.mpdu, params, available)) return 0;
            const auto size = sizes.empty() ? PlannedSize(u, params.m_txVector) : sizes.at(u.staId);
            duration = std::max(duration, WifiPhy::CalculateTxDuration(
                size, params.m_txVector, m_phy->GetPhyBand(), u.staId));
            PointerValue pointer;
            Station(u.userId)->GetPhy()->GetAttribute("InterferenceHelper", pointer);
            auto error = pointer.Get<InterferenceHelper>()->GetErrorRateModel();
            const double adjusted = m_rateMapping->GetErrorModelSinrLinear(u.mcs,
                params.m_txVector, size, 1, u.staId, sinr);
            const double success = error->GetChunkSuccessRate(params.m_txVector.GetMode(u.staId),
                params.m_txVector, adjusted, uint64_t(size) * 8, 1, WIFI_PPDU_FIELD_DATA, u.staId);
            delivered += size * success;
        }
        if (duration > GetPpduMaxTime(WIFI_PREAMBLE_HE_MU)) return 0;
        auto fem = GetHeFem(m_linkId);
        fem->CalculateAcknowledgmentTime(params.m_acknowledgment.get());
        fem->CalculateProtectionTime(params.m_protection.get());
        const auto total = duration + params.m_acknowledgment->acknowledgmentTime +
                           params.m_protection->protectionTime;
        if (!m_initialFrame && available != Time::Min() && total > available) return 0;
        return total.IsStrictlyPositive() ? delivered / total.GetSeconds() : 0;
    }

    double OptimizeRates(std::vector<Candidate>& users, const RateTable& rates,
                         const std::map<uint16_t, uint32_t>& sizes = {})
    {
        // Start at each user's highest supported rate. A low-rate start can trap
        // coordinate search: speeding up only one user leaves the common PPDU
        // duration unchanged, so an equally fast group is never considered.
        for (auto& u : users)
        {
            const auto capabilities = GetWifiRemoteStationManager(m_linkId)->GetStationHeCapabilities(u.address);
            for (uint8_t mcs = 0; mcs < 10; ++mcs)
                if (capabilities && capabilities->IsSupportedRxMcs(mcs) && rates.at(u.userId)[mcs] > 0)
                    u.mcs = mcs;
        }
        // Bounded coordinate search accounts for the common, longest-user PPDU.
        for (unsigned pass = 0; pass < 2; ++pass)
            for (auto& u : users)
            {
                uint8_t best = u.mcs;
                double score = -1;
                for (uint8_t mcs = 0; mcs < 10; ++mcs)
                {
                    u.mcs = mcs;
                    const auto value = RateScore(users, rates, sizes);
                    if (value > score) { score = value; best = mcs; }
                }
                u.mcs = best;
            }
        return RateScore(users, rates, sizes);
    }

    bool ChooseIdealGroup(std::vector<Candidate>& candidates)
    {
        const auto now = Simulator::Now();
        const auto snapshot = m_snapshotProvider(m_runtimeConfiguration.timing.ResolveCsiTimestamp(now));
        UpdateRateSnapshot(snapshot);
        const bool adaptive = m_scheduling == SionnaMimoDownlinkScheduling::MU_ADAPTIVE;
        if (!adaptive)
        {
            for (auto id : m_runtimeConfiguration.requestedUserIds)
                if (std::none_of(candidates.begin(), candidates.end(),
                    [id](const auto& u) { return u.userId == id; }))
                    return RejectGroup("incomplete_fixed_group");
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [this](const auto& u) {
                return std::find(m_runtimeConfiguration.requestedUserIds.begin(),
                    m_runtimeConfiguration.requestedUserIds.end(), u.userId) ==
                    m_runtimeConfiguration.requestedUserIds.end();
            }), candidates.end());
            const auto& rates = GroupRates(candidates, snapshot);
            return OptimizeRates(candidates, rates) > 0 || RejectGroup("no_viable_mu_rate");
        }
        // Oldest-served eligible user is mandatory, so pruning cannot starve a weak peer.
        std::stable_sort(candidates.begin(), candidates.end(), [this](const auto& a, const auto& b) {
            return std::make_pair(m_lastService[a.userId], a.userId) <
                   std::make_pair(m_lastService[b.userId], b.userId);
        });
        auto group = candidates;
        std::vector<Candidate> best;
        double bestScore = 0;
        while (group.size() >= 2)
        {
            const RateTable* rates = nullptr;
            try
            {
                rates = &GroupRates(group, snapshot);
                const double score = OptimizeRates(group, *rates);
                if (score > bestScore) { bestScore = score; best = group; }
            }
            catch (const std::invalid_argument&) { /* prune an infeasible group */ }
            auto remove = group.end() - 1;
            if (rates)
                remove = std::min_element(group.begin() + 1, group.end(),
                    [rates](const auto& a, const auto& b) {
                        return rates->at(a.userId)[0] < rates->at(b.userId)[0];
                    });
            group.erase(remove);
        }
        if (best.empty()) return RejectGroup("no_viable_mu_group");
        candidates = std::move(best);
        auto config = m_runtimeConfiguration;
        config.groupingMode = DlMuMimoGroupingMode::FIXED;
        config.requestedGroupSize = 0;
        config.requestedUserIds.clear();
        for (const auto& u : candidates) config.requestedUserIds.push_back(u.userId);
        m_runtime.emplace(config);
        return true;
    }

    bool BuildDlMuInfo()
    {
        // Full-channel MU only; a narrowed channel-access opportunity uses SU.
        if ((m_allowedWidth != 20 && m_allowedWidth != 40 &&
             m_allowedWidth != 80 && m_allowedWidth != 160) ||
            !m_edca || !m_runtime || !m_phy || m_allowedWidth != m_phy->GetChannelWidth())
        {
            ++m_rejectedGroups;
            return RejectGroup("unsupported_scheduler_context");
        }
        Ptr<WifiMpdu> first = m_edca->PeekNextMpdu(m_linkId);
        if (!first || !first->GetHeader().IsQosData())
        {
            return RejectGroup(first ? "non_qos_head" : "no_eligible_head");
        }
        const uint8_t tid = first->GetHeader().GetQosTid();
        if (m_diagnostic) m_decision.tid = tid;
        const HeRu::RuSpec ru{HeRu::GetRuType(m_allowedWidth), 1, true};
        std::vector<Candidate> candidates;
        DlMuInfo candidateInfo;
        candidateInfo.txParams.m_txVector.SetPreambleType(WIFI_PREAMBLE_HE_MU);
        candidateInfo.txParams.m_txVector.SetChannelWidth(m_allowedWidth);
        candidateInfo.txParams.m_txVector.SetGuardInterval(
            m_apMac->GetHeConfiguration()->GetGuardInterval().GetNanoSeconds());
        candidateInfo.txParams.m_txVector.SetBssColor(m_apMac->GetHeConfiguration()->GetBssColor());

        for (std::size_t index = 0; index < m_stationBindings.size(); ++index)
        {
            const auto& binding = m_stationBindings[index];
            const Mac48Address address = binding.stationDevice->GetMac()->GetAddress();
            const uint16_t staId = m_apMac->GetAssociationId(address, m_linkId);
            auto* status = m_diagnostic ? &m_decision.users[index] : nullptr;
            if (status)
            {
                status->staId = staId;
                const WifiContainerQueueId queue{WIFI_QOSDATA_QUEUE, WIFI_UNICAST, address, tid};
                status->queuedBytes = m_edca->GetWifiMacQueue()->GetNBytes(queue);
                status->queuedPackets = status->queuedBytes
                    ? m_edca->GetWifiMacQueue()->GetNPackets(queue) : 0;
            }
            if (staId == SU_STA_ID)
            {
                if (status) status->eligibility = "unassociated";
                continue;
            }
            if (!m_apMac->GetBaAgreementEstablishedAsOriginator(address, tid))
            {
                if (status) status->eligibility = "missing_block_ack";
                continue;
            }
            if (!GetWifiRemoteStationManager(m_linkId)->GetHeSupported(address))
            {
                if (status) status->eligibility = "not_he_capable";
                continue;
            }
            Ptr<WifiMpdu> mpdu = m_edca->PeekNextMpdu(m_linkId, tid, address);
            if (!mpdu)
            {
                if (status) status->eligibility = status->queuedPackets
                    ? "queued_not_transmittable" : "empty_queue";
                continue;
            }
            uint8_t mcs = 0;
            if (m_rateControl == SionnaMimoDownlinkRateControl::STATION_MANAGER)
            {
                const WifiTxVector suTx =
                    GetWifiRemoteStationManager(m_linkId)->GetDataTxVector(mpdu->GetHeader(), m_allowedWidth);
                if (!suTx.GetModeInitialized() || suTx.GetMode().GetModulationClass() != WIFI_MOD_CLASS_HE ||
                    (m_spectrumModel->GetEffectiveSinrModel() == SionnaMimoEffectiveSinrModel::EESM &&
                     !m_spectrumModel->HasEesmBeta(suTx.GetMode().GetMcsValue())))
                {
                    ++m_rejectedGroups;
                    if (status) status->eligibility = "invalid_rate";
                    return RejectGroup("invalid_rate");
                }
                mcs = suTx.GetMode().GetMcsValue();
            }
            if (status) status->eligibility = "eligible";
            candidateInfo.txParams.m_txVector.SetHeMuUserInfo(staId, {ru, mcs, 1});
            candidateInfo.psduMap.emplace(staId, Create<WifiPsdu>(mpdu, true));
            candidates.push_back({binding.userId, staId, address, mpdu, mcs});
        }
        if (candidates.size() < 2)
        {
            return RejectGroup("fewer_than_two_candidates");
        }

        if (m_rateControl == SionnaMimoDownlinkRateControl::IDEAL_CSI)
        {
            try { if (!ChooseIdealGroup(candidates)) return false; }
            catch (const std::invalid_argument& error) { return RejectGroup("ideal_csi_rejected", error.what()); }
            candidateInfo = {};
            candidateInfo.txParams.m_txVector = MuVector(candidates);
            for (const auto& u : candidates)
                candidateInfo.psduMap.emplace(u.staId, Create<WifiPsdu>(u.mpdu, true));
        }

        std::vector<DlMuMimoStaUserBinding> runtimeBindings;
        std::set<uint32_t> candidateUserIds;
        for (const auto& candidate : candidates)
        {
            runtimeBindings.push_back({candidate.staId, candidate.userId});
            candidateUserIds.insert(candidate.userId);
        }

        std::optional<DlMuMimoRuntimeSelection> selection;
        try
        {
            const Time now = Simulator::Now();
            const Time csiTime = m_runtimeConfiguration.timing.ResolveCsiTimestamp(now);
            selection = m_runtime->SelectAndStore(
                m_apDevice->GetNode()->GetId(),
                SubsetSnapshot(m_snapshotProvider(csiTime), candidateUserIds),
                candidateInfo,
                runtimeBindings,
                now);
        }
        catch (const std::invalid_argument& error)
        {
            ++m_rejectedGroups;
            const std::string detail = error.what();
            return RejectGroup(detail.find("numerical rank") != std::string::npos ? "rank_rejected"
                : detail.find("fixed requested user is absent") != std::string::npos
                    ? "incomplete_fixed_group" : "runtime_rejected", detail);
        }

        DlMuInfo finalInfo;
        finalInfo.txParams.m_txVector.SetPreambleType(WIFI_PREAMBLE_HE_MU);
        finalInfo.txParams.m_txVector.SetChannelWidth(m_allowedWidth);
        finalInfo.txParams.m_txVector.SetGuardInterval(
            m_apMac->GetHeConfiguration()->GetGuardInterval().GetNanoSeconds());
        finalInfo.txParams.m_txVector.SetBssColor(m_apMac->GetHeConfiguration()->GetBssColor());
        std::vector<Candidate> selected;
        for (const auto& selectedUser : selection->GetUsers())
        {
            const auto candidate = std::find_if(candidates.begin(),
                                                candidates.end(),
                                                [&](const auto& value) {
                                                    return value.userId == selectedUser.userId &&
                                                           value.staId == selectedUser.staId;
                                                });
            if (candidate == candidates.end())
            {
                ++m_rejectedGroups;
                return RejectGroup("selected_user_missing");
            }
            finalInfo.txParams.m_txVector.SetHeMuUserInfo(
                candidate->staId, {ru, candidate->mcs, 1});
            selected.push_back(*candidate);
        }
        if (selected.size() < 2)
        {
            ++m_rejectedGroups;
            return RejectGroup("fewer_than_two_selected");
        }

        const Time available = m_initialFrame ? Time::Min() : m_availableTime;
        for (const auto& candidate : selected)
        {
            if (!GetHeFem(m_linkId)->TryAddMpdu(candidate.mpdu, finalInfo.txParams, available))
            {
                ++m_rejectedGroups;
                return RejectGroup("mpdu_time_or_size_limit");
            }
        }
        for (const auto& candidate : selected)
        {
            Ptr<WifiMpdu> item = candidate.mpdu;
            if (!item->GetHeader().IsRetry())
            {
                item = GetHeFem(m_linkId)->GetMsduAggregator()->GetNextAmsdu(
                    candidate.mpdu, finalInfo.txParams, m_availableTime);
                if (!item)
                {
                    item = candidate.mpdu;
                }
                m_edca->AssignSequenceNumber(item);
            }
            auto mpduList = GetHeFem(m_linkId)->GetMpduAggregator()->GetNextAmpdu(
                item, finalInfo.txParams, m_availableTime);
            finalInfo.psduMap[candidate.staId] =
                mpduList.size() > 1 ? Create<WifiPsdu>(std::move(mpduList))
                                    : Create<WifiPsdu>(item, true);
        }
        if (m_rateControl == SionnaMimoDownlinkRateControl::IDEAL_CSI)
        {
            std::map<uint16_t, uint32_t> sizes;
            for (const auto& [id, psdu] : finalInfo.psduMap) sizes[id] = psdu->GetSize();
            auto refined = selected;
            const auto& rates = GroupRates(selected, *m_rateSnapshot);
            if (OptimizeRates(refined, rates, sizes) > 0)
            {
                WifiTxParameters params;
                params.m_txVector = MuVector(refined);
                bool fits = true;
                for (const auto& [id, psdu] : finalInfo.psduMap)
                    for (const auto& mpdu : *psdu)
                        fits &= GetHeFem(m_linkId)->TryAddMpdu(mpdu, params, available);
                if (fits) finalInfo.txParams = std::move(params);
            }
            if (m_diagnostic) m_decision.detail = "ideal_csi; bounded rate/size refinement; oldest eligible user retained";
        }
        m_pendingDlMu = std::move(finalInfo);
        m_hasPendingDlMu = true;
        ++m_muTransmissions;
        return true;
    }

    void InstallAtTransmissionTime(WifiConstPsduMap psduMap, WifiTxVector txVector, double)
    {
        if (!m_apMac)
        {
            return;
        }
        if (m_rateControl == SionnaMimoDownlinkRateControl::IDEAL_CSI)
        {
            auto minstrel = DynamicCast<MinstrelHtWifiManager>(GetWifiRemoteStationManager(m_linkId));
            for (const auto& [id, psdu] : psduMap)
                if (psdu->GetHeader(0).IsQosData())
                {
                    if (minstrel) minstrel->SetExternalDataRate(psdu->GetAddr1(), txVector.IsDlMu());
                    for (const auto& b : m_stationBindings)
                        if (b.stationDevice->GetMac()->GetAddress() == psdu->GetAddr1())
                            m_lastService[b.userId] = Simulator::Now();
                }
        }
        try
        {
            if (txVector.GetPreambleType() == WIFI_PREAMBLE_HE_SU)
            {
                if (psduMap.empty())
                {
                    return;
                }
                const Mac48Address receiver = psduMap.begin()->second->GetAddr1();
                const auto binding = std::find_if(m_stationBindings.begin(),
                                                  m_stationBindings.end(),
                                                  [&](const auto& value) {
                                                      return value.stationDevice->GetMac()->GetAddress() ==
                                                             receiver;
                                                  });
                if (binding == m_stationBindings.end())
                {
                    return;
                }
                const auto snapshot = m_snapshotProvider(Simulator::Now());
                const auto user = std::find_if(snapshot.users.begin(),
                                               snapshot.users.end(),
                                               [&](const auto& value) {
                                                   return value.userId == binding->userId &&
                                                          value.txNodeId == m_apDevice->GetNode()->GetId();
                                               });
                if (user == snapshot.users.end())
                {
                    Reject("the current SU transmission snapshot has no destination user");
                }
                m_spectrumModel->SetDownlinkTransmissionSnapshot(snapshot);
                m_spectrumModel->SetPrecodingMatrix(
                    MimoPrecodingMatrix::MaximumRatioTransmission(user->channel));
                return;
            }
            if (txVector.GetPreambleType() != WIFI_PREAMBLE_HE_MU || !m_runtime)
            {
                return;
            }
            DlMuInfo emitted;
            emitted.txParams.m_txVector = txVector;
            std::vector<DlMuMimoStaUserBinding> bindings;
            std::set<uint32_t> userIds;
            for (const auto& binding : m_stationBindings)
            {
                const uint16_t staId =
                    m_apMac->GetAssociationId(binding.stationDevice->GetMac()->GetAddress(), m_linkId);
                const auto psdu = psduMap.find(staId);
                if (staId != SU_STA_ID && psdu != psduMap.end() &&
                    txVector.GetHeMuUserInfoMap().count(staId) != 0)
                {
                    emitted.psduMap.emplace(staId, ConstCast<WifiPsdu>(psdu->second));
                    bindings.push_back({staId, binding.userId});
                    userIds.insert(binding.userId);
                }
            }
            const Time now = Simulator::Now();
            const Time csiTime = m_runtimeConfiguration.timing.ResolveCsiTimestamp(now);
            const auto csiSnapshot = SubsetSnapshot(m_snapshotProvider(csiTime), userIds);
            const auto transmissionSnapshot = SubsetSnapshot(m_snapshotProvider(now), userIds);
            if (m_scheduling == SionnaMimoDownlinkScheduling::MU_ADAPTIVE)
            {
                auto config = m_runtimeConfiguration;
                config.groupingMode = DlMuMimoGroupingMode::FIXED;
                config.requestedGroupSize = 0;
                config.requestedUserIds.assign(userIds.begin(), userIds.end());
                m_runtime.emplace(config);
            }
            m_runtime->SelectAndStore(m_apDevice->GetNode()->GetId(),
                                      csiSnapshot,
                                      emitted,
                                      bindings,
                                      now);
            m_spectrumModel->ClearPrecodingMatrix();
            m_runtime->Install(*m_spectrumModel,
                               m_apDevice->GetNode()->GetId(),
                               emitted,
                               bindings,
                               transmissionSnapshot,
                               now);
            m_spectrumModel->SetDownlinkTransmissionSnapshot(m_snapshotProvider(now));
        }
        catch (const std::invalid_argument& error)
        {
            NS_ABORT_MSG("Sionna downlink scheduler rejected the emitted MU PPDU: " << error.what());
        }
    }

    void DoDispose() override
    {
        if (m_phy)
        {
            m_phy->TraceDisconnectWithoutContext(
                "PhyTxPsduBegin",
                MakeCallback(&SionnaMimoDownlinkScheduler::InstallAtTransmissionTime, this));
        }
        m_phy = nullptr;
        m_apDevice = nullptr;
        m_spectrumModel = nullptr;
        m_stationBindings.clear();
        m_runtime.reset();
        MultiUserScheduler::DoDispose();
    }

    Ptr<WifiNetDevice> m_apDevice;
    Ptr<WifiPhy> m_phy;
    std::vector<SionnaMimoStationBinding> m_stationBindings;
    Ptr<SionnaSpectrumPropagationLossModel> m_spectrumModel;
    SionnaMimoDownlinkSnapshotProvider m_snapshotProvider;
    std::optional<DlMuMimoRuntime> m_runtime;
    DlMuMimoRuntimeConfiguration m_runtimeConfiguration;
    SionnaMimoDownlinkScheduling m_scheduling{SionnaMimoDownlinkScheduling::MU};
    DlMuInfo m_pendingDlMu;
    bool m_hasPendingDlMu{false};
    uint64_t m_muTransmissions{0};
    uint64_t m_rejectedGroups{0};
    SionnaMimoDownlinkDiagnostic m_diagnostic;
    SionnaMimoDownlinkDecision m_decision;
    uint64_t m_decisionId{0};
    SionnaMimoDownlinkRateControl m_rateControl{SionnaMimoDownlinkRateControl::STATION_MANAGER};
    Ptr<HeSuMimoLinkAbstraction> m_rateMapping;
    std::optional<MultiUserChannelSnapshot> m_rateSnapshot;
    std::map<std::vector<uint32_t>, RateTable> m_rateCache;
    std::map<uint32_t, Time> m_lastService;
    double m_ruPower{0};
    std::vector<double> m_rxGains;
};

} // namespace

uint64_t
SionnaMimoArrayConfiguration::GetNumAntennas() const
{
    const uint64_t numElements = static_cast<uint64_t>(numRows) * numColumns;
    const uint32_t numPolarizations = GetPolarizationCount(polarization);
    if (numElements > std::numeric_limits<uint64_t>::max() / numPolarizations)
    {
        Reject("array antenna-port count overflows uint64_t");
    }
    return numElements * numPolarizations;
}

SionnaMimoPrecodingSelection::SionnaMimoPrecodingSelection(
    SionnaMimoBeamformingMethod method,
    MimoPrecodingMatrix precoding,
    Time csiTimestamp,
    Time transmissionTimestamp)
    : m_method(method),
      m_precoding(std::move(precoding)),
      m_csiTimestamp(csiTimestamp),
      m_transmissionTimestamp(transmissionTimestamp)
{
}

SionnaMimoPrecodingSelection::SionnaMimoPrecodingSelection(
    MimoSvdPrecodingResult svd,
    Time csiTimestamp,
    Time transmissionTimestamp)
    : m_method(SionnaMimoBeamformingMethod::SVD),
      m_precoding(svd.GetPrecodingMatrix()),
      m_csiTimestamp(csiTimestamp),
      m_transmissionTimestamp(transmissionTimestamp),
      m_svd(std::move(svd))
{
}

SionnaMimoPrecodingSelection::SionnaMimoPrecodingSelection(
    MimoCsiSelectionResult csiSelection)
    : m_method(SionnaMimoBeamformingMethod::SVD),
      m_precoding(csiSelection.GetReconstruction().GetPrecodingMatrix()),
      m_csiTimestamp(csiSelection.GetCsiTimestamp()),
      m_transmissionTimestamp(csiSelection.GetTransmissionTimestamp()),
      m_csiSelection(std::move(csiSelection))
{
}

SionnaMimoBeamformingMethod
SionnaMimoPrecodingSelection::GetMethod() const
{
    return m_method;
}

const MimoPrecodingMatrix&
SionnaMimoPrecodingSelection::GetPrecodingMatrix() const
{
    return m_precoding;
}

Time
SionnaMimoPrecodingSelection::GetCsiTimestamp() const
{
    return m_csiTimestamp;
}

Time
SionnaMimoPrecodingSelection::GetTransmissionTimestamp() const
{
    return m_transmissionTimestamp;
}

Time
SionnaMimoPrecodingSelection::GetCsiAge() const
{
    return m_transmissionTimestamp - m_csiTimestamp;
}

bool
SionnaMimoPrecodingSelection::HasSvdResult() const
{
    return m_svd.has_value() || m_csiSelection.has_value();
}

const MimoSvdPrecodingResult&
SionnaMimoPrecodingSelection::GetSvdResult() const
{
    if (m_csiSelection)
    {
        return m_csiSelection->GetIdealSvdResult();
    }
    if (!m_svd)
    {
        Reject("selected MRT precoding has no SVD diagnostics");
    }
    return *m_svd;
}

bool
SionnaMimoPrecodingSelection::HasCsiSelectionResult() const
{
    return m_csiSelection.has_value();
}

const MimoCsiSelectionResult&
SionnaMimoPrecodingSelection::GetCsiSelectionResult() const
{
    if (!m_csiSelection)
    {
        Reject("selected precoding has no SVD feedback or CSI-aging diagnostics");
    }
    return *m_csiSelection;
}

double
SionnaMimoPrecodingSelection::GetPowerFraction(uint32_t subcarrier,
                                                uint32_t stream) const
{
    if (subcarrier >= m_precoding.GetNumSubcarriers() ||
        stream >= m_precoding.GetNumStreams())
    {
        Reject("precoding diagnostic index is out of range");
    }

    double streamPower = 0.0;
    for (uint32_t tx = 0; tx < m_precoding.GetNumTxAntennas(); ++tx)
    {
        streamPower += std::norm(m_precoding.Get(subcarrier, tx, stream));
    }
    return streamPower / m_precoding.GetNumStreams();
}

SionnaMimoHelper::SionnaMimoHelper()
{
    Validate(m_configuration);
}

SionnaMimoHelper::SionnaMimoHelper(const SionnaMimoConfiguration& configuration)
{
    SetConfiguration(configuration);
}

const SionnaMimoConfiguration&
SionnaMimoHelper::GetConfiguration() const
{
    return m_configuration;
}

void
SionnaMimoHelper::SetConfiguration(const SionnaMimoConfiguration& configuration)
{
    Validate(configuration);
    m_configuration = configuration;
    m_selectedPrecoding.reset();
}

void
SionnaMimoHelper::Apply(
    SionnaHelper& sionnaHelper,
    SpectrumWifiPhyHelper& txPhyHelper,
    SpectrumWifiPhyHelper& rxPhyHelper,
    Ptr<SionnaSpectrumPropagationLossModel> spectrumModel) const
{
    Validate(m_configuration);
    if (m_configuration.mode == SionnaMimoMode::SU_BEAMFORMING && !m_selectedPrecoding)
    {
        Reject("SU beamforming must select a channel-dependent precoder before Apply");
    }
    if (!spectrumModel)
    {
        Reject("SpectrumWifiPhy propagation model must not be null");
    }

    const auto& config = m_configuration;
    sionnaHelper.ConfigureTransmitArray(
        config.txArray.numRows,
        config.txArray.numColumns,
        config.txArray.verticalSpacing,
        config.txArray.horizontalSpacing,
        ToSionnaPattern(config.txArray.pattern),
        ToSionnaPolarization(config.txArray.polarization));
    sionnaHelper.ConfigureReceiveArray(
        config.rxArray.numRows,
        config.rxArray.numColumns,
        config.rxArray.verticalSpacing,
        config.rxArray.horizontalSpacing,
        ToSionnaPattern(config.rxArray.pattern),
        ToSionnaPolarization(config.rxArray.polarization));

    sionnaHelper.ClearNodeOrientations();
    for (const auto& [nodeId, orientation] : config.nodeOrientations)
    {
        sionnaHelper.ConfigureNodeOrientation(
            nodeId,
            orientation.azimuthDegrees,
            orientation.elevationDegrees,
            orientation.rollDegrees);
    }

    const uint32_t apStreams =
        config.mode == SionnaMimoMode::MU_MIMO
            ? (config.dlMu->groupingMode == DlMuMimoGroupingMode::FIXED
                   ? config.dlMu->requestedUserIds.size()
                   : config.dlMu->requestedGroupSize)
            : config.maxNss;
    auto configurePhy = [&config](SpectrumWifiPhyHelper& phyHelper,
                                  uint32_t numAntennas,
                                  uint32_t streams) {
        phyHelper.Set("Antennas", UintegerValue(numAntennas));
        phyHelper.Set("MaxSupportedTxSpatialStreams", UintegerValue(streams));
        phyHelper.Set("MaxSupportedRxSpatialStreams", UintegerValue(streams));
        phyHelper.Set("RxNoiseFigure", DoubleValue(config.receiverNoiseFigureDb));
    };
    configurePhy(txPhyHelper, config.numTxAntennas, apStreams);
    configurePhy(rxPhyHelper, config.numRxAntennas, config.maxNss);

    spectrumModel->SetReceiverNoiseFigureDb(config.receiverNoiseFigureDb);
    spectrumModel->SetEffectiveSinrModel(config.effectiveSinrModel);
    spectrumModel->ConfigureEesmBetas(config.eesmBetaLinear);
    spectrumModel->ConfigureAwgnSnrOffsetsDb(config.awgnSnrOffsetsDb);
    if (m_selectedPrecoding)
    {
        spectrumModel->SetPrecodingMatrix(m_selectedPrecoding->GetPrecodingMatrix());
    }
    else if (config.precodingMatrix)
    {
        spectrumModel->SetPrecodingMatrix(*config.precodingMatrix);
    }
    else
    {
        spectrumModel->ClearPrecodingMatrix();
    }
}

void
SionnaMimoHelper::ApplyForHeMcs(
    uint8_t heMcs,
    SionnaHelper& sionnaHelper,
    SpectrumWifiPhyHelper& txPhyHelper,
    SpectrumWifiPhyHelper& rxPhyHelper,
    Ptr<SionnaSpectrumPropagationLossModel> spectrumModel) const
{
    ValidateForHeMcs(m_configuration, heMcs);
    Apply(sionnaHelper, txPhyHelper, rxPhyHelper, spectrumModel);
}

Ptr<MultiUserScheduler>
SionnaMimoHelper::InstallDownlink(
    Ptr<WifiNetDevice> apDevice,
    const std::vector<SionnaMimoStationBinding>& stationBindings,
    Ptr<SionnaSpectrumPropagationLossModel> spectrumModel,
    SionnaMimoDownlinkSnapshotProvider snapshotProvider,
    SionnaMimoDownlinkScheduling scheduling,
    SionnaMimoDownlinkDiagnostic diagnostic,
    SionnaMimoDownlinkRateControl rateControl) const
{
    Validate(m_configuration);
    if (m_configuration.mode != SionnaMimoMode::MU_MIMO || !m_configuration.dlMu)
    {
        Reject("downlink installation requires an explicit MU-MIMO runtime configuration");
    }
    if (!apDevice || !spectrumModel || !snapshotProvider)
    {
        Reject("downlink installation requires an AP device, spectrum model, and snapshot provider");
    }
    Ptr<ApWifiMac> apMac = DynamicCast<ApWifiMac>(apDevice->GetMac());
    Ptr<SpectrumWifiPhy> apPhy = DynamicCast<SpectrumWifiPhy>(apDevice->GetPhy());
    if (!apMac || !apPhy || !apMac->GetHeConfiguration() ||
        apPhy->GetNumberOfAntennas() != m_configuration.numTxAntennas)
    {
        Reject("downlink installation requires a configured HE AP using SpectrumWifiPhy");
    }

    std::set<uint32_t> userIds;
    std::set<Ptr<WifiNetDevice>> devices;
    for (const auto& binding : stationBindings)
    {
        if (!binding.stationDevice || binding.stationDevice == apDevice ||
            !userIds.insert(binding.userId).second || !devices.insert(binding.stationDevice).second)
        {
            Reject("downlink station bindings must have unique users and station devices");
        }
        Ptr<SpectrumWifiPhy> staPhy = DynamicCast<SpectrumWifiPhy>(binding.stationDevice->GetPhy());
        if (!staPhy || !binding.stationDevice->GetMac() ||
            staPhy->GetNumberOfAntennas() != 1 ||
            staPhy->GetMaxSupportedRxSpatialStreams() != 1 ||
            staPhy->GetMaxSupportedTxSpatialStreams() != 1)
        {
            Reject("downlink stations require SpectrumWifiPhy with one antenna and one stream");
        }
    }
    if (stationBindings.size() < 2)
    {
        Reject("downlink installation requires at least two station bindings");
    }

    uint32_t groupSize = m_configuration.dlMu->groupingMode == DlMuMimoGroupingMode::FIXED
                             ? m_configuration.dlMu->requestedUserIds.size()
                             : m_configuration.dlMu->requestedGroupSize;
    if (groupSize < 2 || groupSize > m_configuration.numTxAntennas ||
        groupSize > stationBindings.size() ||
        apPhy->GetMaxSupportedTxSpatialStreams() < groupSize)
    {
        Reject("configured MU group size is incompatible with AP or station capabilities");
    }

    if (scheduling == SionnaMimoDownlinkScheduling::MU_ADAPTIVE &&
        (rateControl != SionnaMimoDownlinkRateControl::IDEAL_CSI ||
         m_configuration.dlMu->precoder == DlMuMimoRuntimePrecoder::FIXED))
        Reject("adaptive grouping requires ideal-CSI rates and ZF/RZF precoding");
    if (rateControl == SionnaMimoDownlinkRateControl::IDEAL_CSI &&
        (m_configuration.dlMu->precoder == DlMuMimoRuntimePrecoder::FIXED ||
         m_configuration.dlMu->groupingMode != DlMuMimoGroupingMode::FIXED))
        Reject("ideal-CSI rates require a fixed configured user ceiling with ZF/RZF precoding");

    Ptr<SionnaMimoDownlinkScheduler> scheduler = CreateObject<SionnaMimoDownlinkScheduler>();
    scheduler->Configure(apDevice,
                         stationBindings,
                         spectrumModel,
                         std::move(snapshotProvider),
                         *m_configuration.dlMu,
                         scheduling,
                         std::move(diagnostic), rateControl, m_configuration);
    apMac->AggregateObject(scheduler);
    return scheduler;
}

void
SionnaMimoHelper::SetNodeOrientation(uint32_t nodeId,
                                     const SionnaMimoOrientation& orientation)
{
    ValidateOrientation(orientation);
    m_configuration.nodeOrientations[nodeId] = orientation;
    m_selectedPrecoding.reset();
}

void
SionnaMimoHelper::ClearNodeOrientation(uint32_t nodeId)
{
    m_configuration.nodeOrientations.erase(nodeId);
    m_selectedPrecoding.reset();
}

bool
SionnaMimoHelper::HasNodeOrientation(uint32_t nodeId) const
{
    return m_configuration.nodeOrientations.count(nodeId) != 0;
}

SionnaMimoOrientation
SionnaMimoHelper::GetNodeOrientation(uint32_t nodeId) const
{
    const auto orientation = m_configuration.nodeOrientations.find(nodeId);
    return orientation == m_configuration.nodeOrientations.end() ? SionnaMimoOrientation{}
                                                                  : orientation->second;
}

void
SionnaMimoHelper::SetEesmBeta(uint8_t mcs, double betaLinear)
{
    ValidateEesmBeta(mcs, betaLinear);
    if (m_configuration.heCalibrationProfile &&
        mcs == m_configuration.heCalibrationProfile->key.mcs &&
        betaLinear != m_configuration.heCalibrationProfile->betaLinear)
    {
        Reject("EESM beta conflicts with the loaded HE calibration profile");
    }
    m_configuration.eesmBetaLinear[mcs] = betaLinear;
}

void
SionnaMimoHelper::SetAwgnSnrOffsetDb(const HeAwgnCalibrationKey& key,
                                     double offsetDb)
{
    SionnaMimoConfiguration candidate = m_configuration;
    candidate.awgnSnrOffsetsDb[key] = offsetDb;
    Validate(candidate);
    m_configuration = std::move(candidate);
}

void
SionnaMimoHelper::LoadHeCalibrationProfile(const std::string& path)
{
    const auto profile = HeCalibrationProfile::Load(path);
    auto candidate = m_configuration;
    candidate.effectiveSinrModel = SionnaMimoEffectiveSinrModel::EESM;
    candidate.heCalibrationProfile = profile;
    candidate.eesmBetaLinear[profile.key.mcs] = profile.betaLinear;
    candidate.awgnSnrOffsetsDb[profile.key] = profile.offsetDb;
    Validate(candidate);
    m_configuration = std::move(candidate);
}

Ptr<HeCalibratedErrorRateModel>
SionnaMimoHelper::InstallHeCalibration(Ptr<WifiNetDevice> receiver) const
{
    Validate(m_configuration);
    auto phy = receiver ? DynamicCast<SpectrumWifiPhy>(receiver->GetPhy()) : nullptr;
    if (!phy || !m_configuration.heCalibrationProfile || !receiver->GetHeConfiguration() ||
        phy->GetNumberOfAntennas() != 1 || phy->GetMaxSupportedRxSpatialStreams() != 1)
    {
        Reject("HE calibration installation requires a configured 1x1 HE SpectrumWifiPhy receiver");
    }
    PointerValue pointer;
    phy->GetAttribute("InterferenceHelper", pointer);
    auto interference = pointer.Get<InterferenceHelper>();
    auto model = CreateObject<HeCalibratedErrorRateModel>();
    model->Configure(*m_configuration.heCalibrationProfile, interference->GetErrorRateModel());
    phy->SetErrorRateModel(model);
    return model;
}

bool
SionnaMimoHelper::HasEesmBeta(uint8_t mcs) const
{
    return m_configuration.eesmBetaLinear.count(mcs) != 0;
}

double
SionnaMimoHelper::GetEesmBeta(uint8_t mcs) const
{
    const auto beta = m_configuration.eesmBetaLinear.find(mcs);
    if (beta == m_configuration.eesmBetaLinear.end())
    {
        Reject("HE MCS " + std::to_string(mcs) + " has no explicit EESM beta");
    }
    return beta->second;
}

void
SionnaMimoHelper::SetPrecodingMatrix(const MimoPrecodingMatrix& precoding)
{
    SionnaMimoConfiguration candidate = m_configuration;
    candidate.precodingMatrix = precoding;
    Validate(candidate);
    m_configuration = candidate;
}

void
SionnaMimoHelper::ClearPrecodingMatrix()
{
    SionnaMimoConfiguration candidate = m_configuration;
    candidate.precodingMatrix.reset();
    Validate(candidate);
    m_configuration = candidate;
}

bool
SionnaMimoHelper::HasPrecodingMatrix() const
{
    return m_configuration.precodingMatrix.has_value();
}

const MimoPrecodingMatrix&
SionnaMimoHelper::GetPrecodingMatrix() const
{
    if (!m_configuration.precodingMatrix)
    {
        Reject("no fixed precoding matrix is configured");
    }
    return *m_configuration.precodingMatrix;
}

const SionnaMimoPrecodingSelection&
SionnaMimoHelper::SelectPrecoding(const MimoChannelMatrix& csiTimeChannelEstimate,
                                  uint32_t txNodeId,
                                  uint32_t rxNodeId,
                                  Time csiTimestamp,
                                  Time transmissionTimestamp)
{
    Validate(m_configuration);
    if (m_configuration.mode != SionnaMimoMode::SU_BEAMFORMING ||
        !m_configuration.beamforming)
    {
        Reject("channel-dependent precoding requires SU_BEAMFORMING configuration");
    }
    if (csiTimeChannelEstimate.GetNumTxAntennas() != m_configuration.numTxAntennas ||
        csiTimeChannelEstimate.GetNumRxAntennas() != m_configuration.numRxAntennas)
    {
        Reject("CSI matrix dimensions must match the configured TX and RX antennas");
    }

    const auto& beamforming = *m_configuration.beamforming;
    std::optional<SionnaMimoPrecodingSelection> candidate;
    if (beamforming.method == SionnaMimoBeamformingMethod::MRT)
    {
        if (csiTimestamp != transmissionTimestamp)
        {
            Reject("MRT requires current perfect CSI with zero age");
        }
        candidate = SionnaMimoPrecodingSelection(
            SionnaMimoBeamformingMethod::MRT,
            MimoPrecodingMatrix::MaximumRatioTransmission(csiTimeChannelEstimate),
            csiTimestamp,
            transmissionTimestamp);
    }
    else if (beamforming.svd.powerAllocation == MimoPowerAllocationMode::WATER_FILLING)
    {
        const Time expectedTimestamp =
            beamforming.csiTiming.ResolveCsiTimestamp(transmissionTimestamp);
        if (csiTimestamp != expectedTimestamp)
        {
            Reject("CSI timestamp does not match update interval and feedback delay");
        }
        candidate = SionnaMimoPrecodingSelection(
            MimoPrecodingMatrix::SingularValueDecomposition(csiTimeChannelEstimate,
                                                            m_configuration.maxNss,
                                                            beamforming.svd),
            csiTimestamp,
            transmissionTimestamp);
    }
    else
    {
        MimoCsiSelectionConfiguration selection;
        selection.txNodeId = txNodeId;
        selection.rxNodeId = rxNodeId;
        selection.txArray = MakeCsiArrayIdentity(m_configuration.txArray,
                                                 GetNodeOrientation(txNodeId));
        selection.rxArray = MakeCsiArrayIdentity(m_configuration.rxArray,
                                                 GetNodeOrientation(rxNodeId));
        selection.numStreams = m_configuration.maxNss;
        selection.feedback = beamforming.feedback;
        selection.timing = beamforming.csiTiming;
        selection.svd = beamforming.svd;
        candidate = SionnaMimoPrecodingSelection(
            MimoCsiAgingModel::Select(csiTimeChannelEstimate,
                                      csiTimestamp,
                                      transmissionTimestamp,
                                      selection));
    }

    m_selectedPrecoding = std::move(candidate);
    return *m_selectedPrecoding;
}

void
SionnaMimoHelper::ClearSelectedPrecoding()
{
    m_selectedPrecoding.reset();
}

bool
SionnaMimoHelper::HasSelectedPrecoding() const
{
    return m_selectedPrecoding.has_value();
}

const SionnaMimoPrecodingSelection&
SionnaMimoHelper::GetSelectedPrecoding() const
{
    if (!m_selectedPrecoding)
    {
        Reject("no channel-dependent precoding has been selected");
    }
    return *m_selectedPrecoding;
}

const std::array<SionnaMimoMode, 4>&
SionnaMimoHelper::GetSupportedModes()
{
    static const std::array<SionnaMimoMode, 4> modes{
        SionnaMimoMode::SISO,
        SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING,
        SionnaMimoMode::SU_BEAMFORMING,
        SionnaMimoMode::MU_MIMO};
    return modes;
}

bool
SionnaMimoHelper::IsModeSupported(SionnaMimoMode mode)
{
    const auto& supportedModes = GetSupportedModes();
    return std::find(supportedModes.begin(), supportedModes.end(), mode) != supportedModes.end();
}

void
SionnaMimoHelper::Validate(const SionnaMimoConfiguration& configuration)
{
    if (!IsModeSupported(configuration.mode))
    {
        Reject("only SISO, SU-MIMO spatial multiplexing, SU beamforming, and MU-MIMO are supported");
    }

    ValidateArray(configuration.txArray, "TX");
    ValidateArray(configuration.rxArray, "RX");

    if (configuration.numTxAntennas == 0 ||
        configuration.numTxAntennas > WIFI_MAX_ANTENNAS ||
        configuration.numRxAntennas == 0 ||
        configuration.numRxAntennas > WIFI_MAX_ANTENNAS)
    {
        Reject("TX and RX antenna counts must be in [1, 8]");
    }
    if (configuration.txArray.GetNumAntennas() != configuration.numTxAntennas)
    {
        Reject("TX antenna count must match the TX array geometry and polarization");
    }
    if (configuration.rxArray.GetNumAntennas() != configuration.numRxAntennas)
    {
        Reject("RX antenna count must match the RX array geometry and polarization");
    }
    if (configuration.maxNss == 0 || configuration.maxNss > WIFI_MAX_ANTENNAS)
    {
        Reject("maximum NSS must be in [1, 8]");
    }
    if (configuration.maxNss > configuration.numTxAntennas ||
        configuration.maxNss > configuration.numRxAntennas)
    {
        Reject("maximum NSS cannot exceed the TX or RX antenna count");
    }

    if (configuration.mode == SionnaMimoMode::SISO)
    {
        if (configuration.numTxAntennas != 1 || configuration.numRxAntennas != 1 ||
            configuration.maxNss != 1)
        {
            Reject("SISO requires one TX antenna, one RX antenna, and maximum NSS one");
        }
        if (configuration.beamforming)
        {
            Reject("SISO cannot configure SU beamforming");
        }
    }
    else if (configuration.mode == SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING)
    {
        if (configuration.beamforming)
        {
            Reject("spatial multiplexing cannot configure channel-dependent beamforming");
        }
        if (configuration.numTxAntennas < 2)
        {
            Reject("SU-MIMO spatial multiplexing requires at least two TX antennas");
        }
        if (!configuration.precodingMatrix &&
            configuration.numRxAntennas < configuration.numTxAntennas)
        {
            Reject("open-loop SU-MIMO requires at least one RX antenna per TX stream");
        }
        if (!configuration.precodingMatrix &&
            configuration.maxNss != configuration.numTxAntennas)
        {
            Reject("open-loop SU-MIMO uses one stream per TX antenna");
        }
    }
    else if (configuration.mode == SionnaMimoMode::SU_BEAMFORMING)
    {
        if (configuration.numTxAntennas < 2)
        {
            Reject("SU beamforming requires at least two TX antennas");
        }
        if (configuration.precodingMatrix)
        {
            Reject("SU beamforming selects W[k]; fixed precoding must use spatial multiplexing");
        }
        if (!configuration.beamforming)
        {
            Reject("SU beamforming requires an explicit MRT or SVD configuration");
        }

        ValidateBeamformingOptions(*configuration.beamforming);
        if (configuration.beamforming->method == SionnaMimoBeamformingMethod::MRT &&
            (configuration.numRxAntennas != 1 || configuration.maxNss != 1))
        {
            Reject("MRT requires exactly one RX antenna and one spatial stream");
        }
    }

    else if (configuration.mode == SionnaMimoMode::MU_MIMO)
    {
        if (configuration.numTxAntennas < 2 || configuration.numRxAntennas != 1 ||
            configuration.maxNss != 1)
        {
            Reject("MU-MIMO requires two to eight AP antennas, one station RX antenna, and per-user NSS one");
        }
        if (configuration.precodingMatrix || configuration.beamforming)
        {
            Reject("MU-MIMO cannot combine SU fixed precoding or SU beamforming");
        }
        if (!configuration.dlMu)
        {
            Reject("MU-MIMO requires an explicit downlink MU runtime configuration");
        }
        if (configuration.effectiveSinrModel == SionnaMimoEffectiveSinrModel::EESM &&
            configuration.eesmBetaLinear.empty())
        {
            Reject("MU-MIMO requires at least one explicit EESM beta");
        }
        DlMuMimoRuntime runtime(*configuration.dlMu);
        static_cast<void>(runtime);
        const uint32_t groupSize =
            configuration.dlMu->groupingMode == DlMuMimoGroupingMode::FIXED
                ? configuration.dlMu->requestedUserIds.size()
                : configuration.dlMu->requestedGroupSize;
        if (groupSize < 2 || groupSize > configuration.numTxAntennas)
        {
            Reject("MU-MIMO group size must be between two and the AP antenna count");
        }
    }

    if (configuration.mode != SionnaMimoMode::MU_MIMO && configuration.dlMu)
    {
        Reject("a downlink MU runtime configuration requires MU-MIMO mode");
    }

    if (configuration.precodingMatrix)
    {
        const auto& precoding = *configuration.precodingMatrix;
        if (precoding.GetNumTxAntennas() != configuration.numTxAntennas)
        {
            Reject("precoding TX dimension must match the configured TX antennas");
        }
        if (precoding.GetNumStreams() != configuration.maxNss)
        {
            Reject("maximum NSS must equal the fixed-precoding stream dimension");
        }
        if (precoding.GetNumStreams() > configuration.numRxAntennas)
        {
            Reject("fixed-precoding streams cannot exceed the RX antennas");
        }
    }

    ValidateReceiverType(configuration.receiverType);
    ValidateNormalization(configuration.channelNormalization);
    ValidateEffectiveSinrModel(configuration.effectiveSinrModel);
    if (configuration.effectiveSinrModel == SionnaMimoEffectiveSinrModel::LEGACY_SISO &&
        (configuration.numTxAntennas != 1 || configuration.numRxAntennas != 1))
    {
        Reject("LEGACY_SISO requires 1x1 reception");
    }

    if (!std::isfinite(configuration.receiverNoiseFigureDb) ||
        configuration.receiverNoiseFigureDb < 0.0)
    {
        Reject("receiver noise figure must be finite and non-negative");
    }

    for (const auto& [mcs, betaLinear] : configuration.eesmBetaLinear)
    {
        ValidateEesmBeta(mcs, betaLinear);
    }
    for (const auto& [key, offsetDb] : configuration.awgnSnrOffsetsDb)
    {
        if (!key.IsValid() || !std::isfinite(offsetDb) || std::abs(offsetDb) > 30.0)
        {
            Reject("AWGN calibration requires a valid HE PHY context and a finite +/-30 dB offset");
        }
    }
    if (configuration.heCalibrationProfile)
    {
        const auto& profile = *configuration.heCalibrationProfile;
        profile.Validate();
        const auto beta = configuration.eesmBetaLinear.find(profile.key.mcs);
        const auto offset = configuration.awgnSnrOffsetsDb.find(profile.key);
        if (configuration.effectiveSinrModel != SionnaMimoEffectiveSinrModel::EESM ||
            configuration.mode != SionnaMimoMode::SISO ||
            configuration.numTxAntennas != 1 || configuration.numRxAntennas != 1 ||
            beta == configuration.eesmBetaLinear.end() || beta->second != profile.betaLinear ||
            offset == configuration.awgnSnrOffsetsDb.end() || offset->second != profile.offsetDb)
        {
            Reject("HE pilot requires consistent beta/offset and SISO helper dimensions");
        }
    }
    for (const auto& [nodeId, orientation] : configuration.nodeOrientations)
    {
        static_cast<void>(nodeId);
        ValidateOrientation(orientation);
    }
}

void
SionnaMimoHelper::ValidateForHeMcs(const SionnaMimoConfiguration& configuration, uint8_t mcs)
{
    Validate(configuration);
    if (mcs > HE_MAX_MCS)
    {
        Reject("HE MCS must be in [0, 11]");
    }
    if (configuration.effectiveSinrModel == SionnaMimoEffectiveSinrModel::EESM &&
        configuration.eesmBetaLinear.count(mcs) == 0)
    {
        const std::string context = configuration.mode == SionnaMimoMode::MU_MIMO ? "MU-MIMO" :
            configuration.mode == SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING ? "open-loop SU-MIMO" :
            configuration.mode == SionnaMimoMode::SU_BEAMFORMING ? "SU beamforming" : "EESM";
        Reject(context + " requires an explicit EESM beta for HE MCS " + std::to_string(mcs));
    }
}

} // namespace ns3
