/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "sionna-mimo-helper.h"

#include "sionna-helper.h"
#include "../model/sionna-spectrum-propagation-loss-model.h"
#include "ns3/ap-wifi-mac.h"
#include "ns3/double.h"
#include "ns3/he-configuration.h"
#include "ns3/he-frame-exchange-manager.h"
#include "ns3/mpdu-aggregator.h"
#include "ns3/qos-txop.h"
#include "ns3/simulator.h"
#include "ns3/spectrum-wifi-phy.h"
#include "ns3/wifi-mac-header.h"
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
    if (model != SionnaMimoEffectiveSinrModel::EESM)
    {
        Reject("only EESM is supported for the HE PER mapping");
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
                   SionnaMimoDownlinkScheduling scheduling)
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
        if (m_scheduling == SionnaMimoDownlinkScheduling::SU_ROUND_ROBIN)
        {
            return SU_TX;
        }
        return BuildDlMuInfo() ? DL_MU_TX : SU_TX;
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

    bool BuildDlMuInfo()
    {
        if (m_allowedWidth != 20 || !m_edca || !m_runtime)
        {
            ++m_rejectedGroups;
            return false;
        }
        Ptr<WifiMpdu> first = m_edca->PeekNextMpdu(m_linkId);
        if (!first || !first->GetHeader().IsQosData())
        {
            return false;
        }
        const uint8_t tid = first->GetHeader().GetQosTid();
        const HeRu::RuSpec ru{HeRu::RU_242_TONE, 1, true};
        std::vector<Candidate> candidates;
        DlMuInfo candidateInfo;
        candidateInfo.txParams.m_txVector.SetPreambleType(WIFI_PREAMBLE_HE_MU);
        candidateInfo.txParams.m_txVector.SetChannelWidth(m_allowedWidth);
        candidateInfo.txParams.m_txVector.SetGuardInterval(
            m_apMac->GetHeConfiguration()->GetGuardInterval().GetNanoSeconds());
        candidateInfo.txParams.m_txVector.SetBssColor(m_apMac->GetHeConfiguration()->GetBssColor());

        for (const auto& binding : m_stationBindings)
        {
            const Mac48Address address = binding.stationDevice->GetMac()->GetAddress();
            const uint16_t staId = m_apMac->GetAssociationId(address, m_linkId);
            if (staId == SU_STA_ID || !m_apMac->GetBaAgreementEstablishedAsOriginator(address, tid) ||
                !GetWifiRemoteStationManager(m_linkId)->GetHeSupported(address))
            {
                continue;
            }
            Ptr<WifiMpdu> mpdu = m_edca->PeekNextMpdu(m_linkId, tid, address);
            if (!mpdu)
            {
                continue;
            }
            const WifiTxVector suTx =
                GetWifiRemoteStationManager(m_linkId)->GetDataTxVector(mpdu->GetHeader(), m_allowedWidth);
            if (!suTx.GetModeInitialized() || suTx.GetMode().GetModulationClass() != WIFI_MOD_CLASS_HE ||
                !m_spectrumModel->HasEesmBeta(suTx.GetMode().GetMcsValue()))
            {
                ++m_rejectedGroups;
                return false;
            }
            const uint8_t mcs = suTx.GetMode().GetMcsValue();
            candidateInfo.txParams.m_txVector.SetHeMuUserInfo(staId, {ru, mcs, 1});
            candidateInfo.psduMap.emplace(staId, Create<WifiPsdu>(mpdu, true));
            candidates.push_back({binding.userId, staId, address, mpdu, mcs});
        }
        if (candidates.size() < 2)
        {
            return false;
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
        catch (const std::invalid_argument&)
        {
            ++m_rejectedGroups;
            return false;
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
                return false;
            }
            finalInfo.txParams.m_txVector.SetHeMuUserInfo(
                candidate->staId, {ru, candidate->mcs, 1});
            selected.push_back(*candidate);
        }
        if (selected.size() < 2)
        {
            ++m_rejectedGroups;
            return false;
        }

        const Time available = m_initialFrame ? Time::Min() : m_availableTime;
        for (const auto& candidate : selected)
        {
            if (!GetHeFem(m_linkId)->TryAddMpdu(candidate.mpdu, finalInfo.txParams, available))
            {
                ++m_rejectedGroups;
                return false;
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
    SionnaMimoDownlinkScheduling scheduling) const
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

    Ptr<SionnaMimoDownlinkScheduler> scheduler = CreateObject<SionnaMimoDownlinkScheduler>();
    scheduler->Configure(apDevice,
                         stationBindings,
                         spectrumModel,
                         std::move(snapshotProvider),
                         *m_configuration.dlMu,
                         scheduling);
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
        if (configuration.eesmBetaLinear.empty())
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
    if ((configuration.mode == SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING ||
         configuration.mode == SionnaMimoMode::MU_MIMO) &&
        configuration.eesmBetaLinear.count(mcs) == 0)
    {
        Reject((configuration.mode == SionnaMimoMode::MU_MIMO
                    ? std::string("MU-MIMO requires an explicit EESM beta for HE MCS ")
                    : std::string("open-loop SU-MIMO requires an explicit EESM beta for HE MCS ")) +
               std::to_string(mcs));
    }
    else if (configuration.mode == SionnaMimoMode::SU_BEAMFORMING &&
             configuration.eesmBetaLinear.count(mcs) == 0)
    {
        Reject("SU beamforming requires an explicit EESM beta for HE MCS " +
               std::to_string(mcs));
    }
}

} // namespace ns3
