/*
* Copyright (c) 2025
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: A. Zubow <zubow@tkn.tu-berlin.de>
 */

#include "sionna-spectrum-propagation-loss-model.h"
#include "he-calibrated-error-rate-model.h"
#include "he-data-tone-mask.h"
#include "he-ber-error-rate-model.h"
//#include "sionna-utils.h"

#include "ns3/socket.h"
#include <ns3/abort.h>
#include <ns3/double.h>
#include <ns3/error-rate-model.h>
#include <ns3/he-phy.h>
#include <ns3/he-ppdu.h>
#include <ns3/interference-helper.h>
#include <ns3/log.h>
#include <ns3/node.h>
#include <ns3/node-list.h>
#include <ns3/object-factory.h>
#include <ns3/pointer.h>
#include <ns3/random-variable-stream.h>
#include <ns3/spectrum-channel.h>
#include <ns3/spectrum-signal-parameters.h>
#include <ns3/string.h>
#include <ns3/wifi-phy.h>
#include <ns3/wifi-ppdu.h>
#include <ns3/wifi-psdu.h>
#include <ns3/wifi-net-device.h>
#include <ns3/wifi-spectrum-phy-interface.h>
#include <ns3/wifi-spectrum-signal-parameters.h>
#include <ns3/wifi-spectrum-value-helper.h>
#include <ns3/wifi-tx-vector.h>
#include <ns3/wifi-mac.h>
#include <ns3/wifi-utils.h>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("SionnaSpectrumPropagationLossModel");

NS_OBJECT_ENSURE_REGISTERED(SionnaSpectrumPropagationLossModel);

namespace
{

constexpr double WIFI_NOISE_TEMPERATURE_K = 290.0;

Ptr<SpectrumWifiPhy>
GetSpectrumWifiPhy(Ptr<const MobilityModel> mobility, Ptr<Channel> peerChannel)
{
    Ptr<Node> node = mobility->GetObject<Node>();
    NS_ABORT_MSG_IF(!node, "SpectrumWifiPhy mobility is not aggregated to a node");

    Ptr<SpectrumWifiPhy> match;
    for (uint32_t i = 0; i < node->GetNDevices(); ++i)
    {
        Ptr<WifiNetDevice> wifiDevice = DynamicCast<WifiNetDevice>(node->GetDevice(i));
        if (!wifiDevice)
        {
            continue;
        }
        Ptr<SpectrumWifiPhy> candidate = DynamicCast<SpectrumWifiPhy>(wifiDevice->GetPhy());
        if (!candidate || candidate->GetMobility() != mobility)
        {
            continue;
        }
        if (peerChannel && candidate->GetChannel() != peerChannel)
        {
            continue;
        }
        NS_ABORT_MSG_IF(match, "More than one matching SpectrumWifiPhy found on receiver node");
        match = candidate;
    }
    NS_ABORT_MSG_IF(!match, "No matching SpectrumWifiPhy found on receiver node");
    return match;
}

Ptr<SpectrumWifiPhy>
GetTransmitterSpectrumWifiPhy(Ptr<const SpectrumSignalParameters> params)
{
    NS_ABORT_MSG_IF(!params->txPhy || !params->txPhy->GetDevice(),
                    "MIMO Wi-Fi reception requires a transmitting SpectrumPhy device");
    Ptr<WifiNetDevice> device = DynamicCast<WifiNetDevice>(params->txPhy->GetDevice());
    NS_ABORT_MSG_IF(!device, "MIMO Wi-Fi transmitter is not a WifiNetDevice");
    Ptr<SpectrumWifiPhy> phy = DynamicCast<SpectrumWifiPhy>(device->GetPhy());
    NS_ABORT_MSG_IF(!phy, "MIMO Wi-Fi transmitter does not use SpectrumWifiPhy");
    return phy;
}

Ptr<ErrorRateModel>
GetErrorRateModel(Ptr<SpectrumWifiPhy> phy)
{
    PointerValue helperValue;
    phy->GetAttribute("InterferenceHelper", helperValue);
    Ptr<InterferenceHelper> helper = helperValue.Get<InterferenceHelper>();
    NS_ABORT_MSG_IF(!helper || !helper->GetErrorRateModel(),
                    "SpectrumWifiPhy ErrorRateModel is not configured");
    return helper->GetErrorRateModel();
}

double
IntegralBand(const SpectrumValue& psd, const WifiSpectrumBandIndices& indices)
{
    if (indices.first > indices.second || indices.second >= psd.GetValuesN())
    {
        throw std::invalid_argument("Sionna HE-DL-MU adapter: RU spectrum indices are invalid");
    }
    auto band = psd.GetSpectrumModel()->Begin();
    std::advance(band, indices.first);
    double powerW = 0.0;
    for (std::size_t index = indices.first; index <= indices.second; ++index, ++band)
    {
        powerW += psd.ValuesAt(index) * (band->fh - band->fl);
    }
    return powerW;
}

void
ValidateDownlinkTransmissionSnapshot(const MultiUserChannelSnapshot& snapshot)
{
    if (snapshot.direction != MultiUserChannelDirection::DOWNLINK || snapshot.users.empty())
    {
        throw std::invalid_argument("Sionna downlink transmission snapshot must contain downlink users");
    }

    const auto& first = snapshot.users.front();
    const uint32_t txNodeId = first.txNodeId;
    const uint32_t numSubcarriers = first.channel.GetNumSubcarriers();
    const uint32_t numTxAntennas = first.channel.GetNumTxAntennas();
    const auto frequencies = first.frequencies;
    if (numSubcarriers == 0 || numTxAntennas == 0 || frequencies.size() != numSubcarriers)
    {
        throw std::invalid_argument("Sionna downlink transmission snapshot has invalid dimensions");
    }

    std::set<uint32_t> receivers;
    uint32_t previousUserId = 0;
    bool isFirst = true;
    for (const auto& user : snapshot.users)
    {
        if ((!isFirst && user.userId <= previousUserId) ||
            user.txNodeId != txNodeId || user.rxNodeId == txNodeId ||
            !receivers.insert(user.rxNodeId).second ||
            user.direction != snapshot.direction || user.csiTimestamp != snapshot.csiTimestamp ||
            user.channel.GetNumSubcarriers() != numSubcarriers ||
            user.channel.GetNumTxAntennas() != numTxAntennas ||
            user.channel.GetNumRxAntennas() == 0 || user.frequencies != frequencies ||
            !std::isfinite(user.widebandLossDb))
        {
            throw std::invalid_argument(
                "Sionna downlink transmission snapshot has inconsistent directed users");
        }
        for (const auto coefficient : user.channel.GetCoefficients())
        {
            if (!std::isfinite(coefficient.real()) || !std::isfinite(coefficient.imag()))
            {
                throw std::invalid_argument(
                    "Sionna downlink transmission snapshot has non-finite CFR coefficients");
            }
        }
        isFirst = false;
        previousUserId = user.userId;
    }
}

} // namespace

const HeSuMimoLinkResult&
SionnaMimoSpectrumRxResult::GetLinkResult() const
{
    return m_linkResult;
}

double
SionnaMimoSpectrumRxResult::GetEquivalentReceivedPowerW() const
{
    return m_equivalentReceivedPowerW;
}

const HeDlMuMimoLinkResult&
SionnaHeDlMuMimoSpectrumRxResult::GetLinkResult() const
{
    return m_linkResult;
}

double
SionnaHeDlMuMimoSpectrumRxResult::GetEquivalentReceivedPowerW(uint32_t userId) const
{
    const auto found = std::lower_bound(
        m_equivalentPowers.begin(),
        m_equivalentPowers.end(),
        userId,
        [](const auto& user, uint32_t id) { return user.userId < id; });
    if (found == m_equivalentPowers.end() || found->userId != userId)
    {
        throw std::invalid_argument("Sionna HE-DL-MU adapter: result user ID is absent");
    }
    return found->powerW;
}

SionnaSpectrumPropagationLossModel::SionnaSpectrumPropagationLossModel()
    : m_linkAbstraction(CreateObject<HeSuMimoLinkAbstraction>())
{
    NS_LOG_FUNCTION(this);
}

SionnaSpectrumPropagationLossModel::~SionnaSpectrumPropagationLossModel()
{
    NS_LOG_FUNCTION(this);
}

TypeId
SionnaSpectrumPropagationLossModel::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::SionnaSpectrumPropagationLossModel")
            .SetParent<SpectrumPropagationLossModel>()
            .SetGroupName("Sionna")
            .AddConstructor<SionnaSpectrumPropagationLossModel>()
            .AddTraceSource("MimoRx",
                            "SU-MIMO PPDU result: UID, TX node, RX node, effective SINR, PER, "
                            "and minimum channel rank",
                            MakeTraceSourceAccessor(
                                &SionnaSpectrumPropagationLossModel::m_mimoRxTrace),
                            "ns3::TracedCallback::Uint64Uint32Uint32DoubleDoubleUint32")
            .AddTraceSource("DlMuMimoRx",
                            "Addressed HE-MU payload numerical result: UID, user ID, TX node, RX "
                            "node, and per-user link result",
                            MakeTraceSourceAccessor(
                                &SionnaSpectrumPropagationLossModel::m_dlMuMimoRxTrace),
                            "ns3::TracedCallback::Uint64Uint32Uint32Uint32HeDlMuMimoUserResult");
    return tid;
}

void
SionnaSpectrumPropagationLossModel::SetPropagationCache(Ptr<SionnaPropagationCache> propagationCache)
{
    m_propagationCache = propagationCache;
}

void
SionnaSpectrumPropagationLossModel::SetFixedChannelMatrix(const MimoChannelMatrix& channel,
                                                          double pathLossDb)
{
    NS_ABORT_MSG_IF(!std::isfinite(pathLossDb), "Fixed path loss must be finite");
    m_fixedChannel = channel;
    m_fixedPathLossDb = pathLossDb;
    m_fixedChannelEnabled = true;
}

void
SionnaSpectrumPropagationLossModel::SetReceiverNoiseFigureDb(double noiseFigureDb)
{
    NS_ABORT_MSG_IF(!std::isfinite(noiseFigureDb) || noiseFigureDb < 0.0,
                    "Receiver noise figure must be finite and non-negative");
    m_receiverNoiseFigureDb = noiseFigureDb;
}

void
SionnaSpectrumPropagationLossModel::SetEffectiveSinrModel(SionnaMimoEffectiveSinrModel model)
{
    m_linkAbstraction->SetEffectiveSinrModel(model);
}
SionnaMimoEffectiveSinrModel
SionnaSpectrumPropagationLossModel::GetEffectiveSinrModel() const
{
    return m_linkAbstraction->GetEffectiveSinrModel();
}

void
SionnaSpectrumPropagationLossModel::SetEesmBeta(uint8_t mcs, double betaLinear)
{
    m_linkAbstraction->SetEesmBeta(mcs, betaLinear);
    m_eesmBetas[mcs] = betaLinear;
}

void
SionnaSpectrumPropagationLossModel::SetEesmBetaDb(uint8_t mcs, double betaDb)
{
    m_linkAbstraction->SetEesmBetaDb(mcs, betaDb);
    m_eesmBetas[mcs] = m_linkAbstraction->GetEesmBeta(mcs);
}

void
SionnaSpectrumPropagationLossModel::ConfigureEesmBetas(
    const std::map<uint8_t, double>& betas)
{
    auto linkAbstraction = CreateObject<HeSuMimoLinkAbstraction>();
    linkAbstraction->SetEffectiveSinrModel(GetEffectiveSinrModel());
    for (const auto& [mcs, betaLinear] : betas)
    {
        linkAbstraction->SetEesmBeta(mcs, betaLinear);
    }
    for (const auto& [key, offsetDb] : m_awgnSnrOffsetsDb)
    {
        linkAbstraction->SetAwgnSnrOffsetDb(key, offsetDb);
    }
    m_linkAbstraction = linkAbstraction;
    m_eesmBetas = betas;
}

void
SionnaSpectrumPropagationLossModel::SetAwgnSnrOffsetDb(
    const HeAwgnCalibrationKey& key,
    double offsetDb)
{
    m_linkAbstraction->SetAwgnSnrOffsetDb(key, offsetDb);
    m_awgnSnrOffsetsDb[key] = offsetDb;
}

void
SionnaSpectrumPropagationLossModel::ConfigureAwgnSnrOffsetsDb(
    const std::map<HeAwgnCalibrationKey, double>& offsetsDb)
{
    auto linkAbstraction = CreateObject<HeSuMimoLinkAbstraction>();
    linkAbstraction->SetEffectiveSinrModel(GetEffectiveSinrModel());
    for (const auto& [mcs, betaLinear] : m_eesmBetas)
    {
        linkAbstraction->SetEesmBeta(mcs, betaLinear);
    }
    for (const auto& [key, offsetDb] : offsetsDb)
    {
        linkAbstraction->SetAwgnSnrOffsetDb(key, offsetDb);
    }
    m_linkAbstraction = linkAbstraction;
    m_awgnSnrOffsetsDb = offsetsDb;
}

void
SionnaSpectrumPropagationLossModel::SetPrecodingMatrix(
    const MimoPrecodingMatrix& precoding)
{
    m_precodingMatrix = precoding;
}

void
SionnaSpectrumPropagationLossModel::ClearPrecodingMatrix()
{
    m_precodingMatrix.reset();
}

bool
SionnaSpectrumPropagationLossModel::HasPrecodingMatrix() const
{
    return m_precodingMatrix.has_value();
}

const MimoPrecodingMatrix&
SionnaSpectrumPropagationLossModel::GetPrecodingMatrix() const
{
    NS_ABORT_MSG_IF(!m_precodingMatrix, "No fixed precoding matrix is configured");
    return *m_precodingMatrix;
}

void
SionnaSpectrumPropagationLossModel::SetDownlinkTransmissionSnapshot(
    const MultiUserChannelSnapshot& snapshot)
{
    ValidateDownlinkTransmissionSnapshot(snapshot);
    m_downlinkTransmissionSnapshot = snapshot;
}

void
SionnaSpectrumPropagationLossModel::ClearDownlinkTransmissionSnapshot()
{
    m_downlinkTransmissionSnapshot.reset();
}

bool
SionnaSpectrumPropagationLossModel::HasDownlinkTransmissionSnapshot() const
{
    return m_downlinkTransmissionSnapshot.has_value();
}

void
SionnaSpectrumPropagationLossModel::SetDlMuMimoState(
    const MultiUserChannelSnapshot& snapshot,
    const DlMuMimoPrecodingMatrix& precoding)
{
    precoding.ValidateForSnapshot(snapshot);
    // Installed runtime state has physical directed endpoints; the standalone
    // numerical API deliberately also accepts synthetic channel identities.
    const uint32_t txNodeId = snapshot.users.front().txNodeId;
    std::set<uint32_t> receivers;
    for (const auto& user : snapshot.users)
    {
        if (user.txNodeId != txNodeId || user.rxNodeId == txNodeId ||
            !receivers.insert(user.rxNodeId).second || !std::isfinite(user.widebandLossDb))
        {
            throw std::invalid_argument(
                "Sionna HE-DL-MU state requires one AP, unique directed receivers, and finite loss");
        }
    }
    m_dlMuSnapshot = snapshot;
    m_dlMuPrecoding = precoding;
}

void
SionnaSpectrumPropagationLossModel::ClearDlMuMimoState()
{
    m_dlMuSnapshot.reset();
    m_dlMuPrecoding.reset();
}

bool
SionnaSpectrumPropagationLossModel::HasDlMuMimoState() const
{
    return m_dlMuSnapshot.has_value() && m_dlMuPrecoding.has_value();
}

double
SionnaSpectrumPropagationLossModel::GetReceiverNoiseFigureDb() const
{
    return m_receiverNoiseFigureDb;
}

bool
SionnaSpectrumPropagationLossModel::HasEesmBeta(uint8_t mcs) const
{
    return m_linkAbstraction->HasEesmBeta(mcs);
}

double
SionnaSpectrumPropagationLossModel::GetEesmBeta(uint8_t mcs) const
{
    return m_linkAbstraction->GetEesmBeta(mcs);
}

SionnaMimoSpectrumRxResult
SionnaSpectrumPropagationLossModel::CalculateMimoRx(
    const MimoChannelMatrix& channel,
    double receivedPowerW,
    const WifiTxVector& txVector,
    uint32_t packetSizeBytes,
    uint8_t txPhyAntennas,
    uint8_t rxPhyAntennas,
    double bandwidthHz,
    double receiverNoiseFigureDb,
    Ptr<ErrorRateModel> errorRateModel,
    const std::vector<uint32_t>& dataIndices,
    bool useConfiguredPrecoder,
    const MimoPrecodingMatrix* receptionPrecoder) const
{
    NS_ABORT_MSG_IF(!std::isfinite(receivedPowerW) || receivedPowerW <= 0.0,
                    "Received MIMO power must be finite and positive");
    NS_ABORT_MSG_IF(txVector.GetPreambleType() != WIFI_PREAMBLE_HE_SU,
                    "The minimal MIMO SpectrumWifiPhy path supports HE SU PPDUs only");

    const auto* precoder = useConfiguredPrecoder
        ? (receptionPrecoder ? receptionPrecoder : (m_precodingMatrix ? &*m_precodingMatrix : nullptr))
        : nullptr;
    const bool precoded = precoder != nullptr;
    const uint8_t nss = txVector.GetNss();
    const uint32_t expectedNss = precoded
                                     ? precoder->GetNumStreams()
                                     : channel.GetNumTxAntennas();
    NS_ABORT_MSG_IF(nss != expectedNss,
                    "WifiTxVector NSS must equal the transmitted stream dimension");
    NS_ABORT_MSG_IF(txPhyAntennas != channel.GetNumTxAntennas(),
                    "Transmitter PHY antennas must equal the Sionna TX dimension");
    NS_ABORT_MSG_IF(rxPhyAntennas != channel.GetNumRxAntennas(),
                    "Receiver PHY antennas must equal the Sionna RX dimension");
    NS_ABORT_MSG_IF(rxPhyAntennas < nss,
                    "Receiver PHY needs at least one antenna per spatial stream");
    NS_ABORT_MSG_IF(!errorRateModel, "SpectrumWifiPhy ErrorRateModel must be configured");
    if (precoded)
    {
        precoder->ValidateForChannel(channel);
    }

    const auto linkBudget =
        WifiLinkBudget::Calculate(WToDbm(receivedPowerW),
                                  0.0,
                                  bandwidthHz,
                                  WIFI_NOISE_TEMPERATURE_K,
                                  receiverNoiseFigureDb);
    m_linkAbstraction->SetErrorRateModel(errorRateModel);

    SionnaMimoSpectrumRxResult result;
    result.m_linkResult = precoded
                              ? m_linkAbstraction->Calculate(channel,
                                                             *precoder,
                                                             linkBudget,
                                                             txVector,
                                                             packetSizeBytes, dataIndices)
                              : m_linkAbstraction->Calculate(channel,
                                                             linkBudget,
                                                             txVector,
                                                             packetSizeBytes, dataIndices);

    // InterferenceHelper applies N_rx/N_ss for AWGN models. MMSE already contains
    // this array gain, so reduce the equivalent scalar signal by that exact factor.
    const double diversityGain =
        errorRateModel->IsAwgn() && rxPhyAntennas > nss
            ? static_cast<double>(rxPhyAntennas) / nss
            : 1.0;
    result.m_equivalentReceivedPowerW =
        result.m_linkResult.GetEffectiveSinrLinear() *
        linkBudget.GetReceiverNoisePowerW() / diversityGain;
    NS_ABORT_MSG_IF(!std::isfinite(result.m_equivalentReceivedPowerW) ||
                        result.m_equivalentReceivedPowerW < 0.0,
                    "MIMO abstraction produced invalid equivalent receive power");
    return result;
}



SionnaHeDlMuMimoSpectrumRxResult
SionnaSpectrumPropagationLossModel::CalculateHeDlMuMimoRx(
    const MultiUserChannelSnapshot& snapshot,
    const DlMuMimoPrecodingMatrix& precoding,
    const WifiTxVector& txVector,
    const std::vector<SionnaHeDlMuMimoSpectrumUserTransmission>& users,
    uint8_t txPhyAntennas,
    double receiverNoiseFigureDb,
    Ptr<ErrorRateModel> errorRateModel) const
{
    const auto reject = [](const std::string& reason) {
        throw std::invalid_argument("Sionna HE-DL-MU adapter: " + reason);
    };

    if (txVector.GetPreambleType() != WIFI_PREAMBLE_HE_MU)
    {
        reject("only WIFI_PREAMBLE_HE_MU downlink PPDUs are supported");
    }
    if (!txVector.GetModeInitialized() ||
        txVector.GetModulationClass() != WIFI_MOD_CLASS_HE)
    {
        reject("EHT and non-HE MU PPDUs are unsupported");
    }
    if (!errorRateModel)
    {
        reject("SpectrumWifiPhy ErrorRateModel must be configured");
    }
    if (!std::isfinite(receiverNoiseFigureDb) || receiverNoiseFigureDb < 0.0)
    {
        reject("receiver noise figure must be finite and non-negative");
    }

    precoding.ValidateForSnapshot(snapshot);
    if (snapshot.direction != MultiUserChannelDirection::DOWNLINK)
    {
        reject("only downlink channel snapshots are supported");
    }
    if (snapshot.users.empty() ||
        txPhyAntennas != snapshot.users.front().channel.GetNumTxAntennas())
    {
        reject("transmitter PHY antennas must equal the snapshot TX dimension");
    }

    const auto& userInfoMap = txVector.GetHeMuUserInfoMap();
    if (users.size() != snapshot.users.size() || users.size() != userInfoMap.size())
    {
        reject("snapshot, receiver inputs, and HeMuUserInfoMap must contain the same users");
    }

    std::map<uint32_t, const SionnaHeDlMuMimoSpectrumUserTransmission*> inputs;
    std::set<uint16_t> staIds;
    std::vector<HeRu::RuSpec> ruGroups;
    std::vector<HeDlMuMimoUserTransmission> heUsers;
    heUsers.reserve(users.size());

    for (const auto& user : users)
    {
        if (!inputs.emplace(user.userId, &user).second)
        {
            reject("a snapshot user occurs more than once");
        }
        if (!staIds.insert(user.staId).second || user.staId == NO_USER_STA_ID ||
            !txVector.IsAllocated(user.staId))
        {
            reject("every receiver requires one unique addressed STA-ID");
        }
        if (!std::isfinite(user.receivedPowerW) || user.receivedPowerW <= 0.0)
        {
            reject("received power must be finite and positive");
        }
        const auto snapshotUser = std::lower_bound(
            snapshot.users.begin(),
            snapshot.users.end(),
            user.userId,
            [](const MultiUserChannel& candidate, uint32_t id) {
                return candidate.userId < id;
            });
        if (snapshotUser == snapshot.users.end() || snapshotUser->userId != user.userId)
        {
            reject("receiver user ID is absent from the channel snapshot");
        }
        if (snapshotUser->direction != MultiUserChannelDirection::DOWNLINK ||
            snapshotUser->channel.GetNumRxAntennas() != user.rxPhyAntennas)
        {
            reject("receiver direction or antenna dimension is inconsistent");
        }

        const auto& userInfo = userInfoMap.at(user.staId);
        if (userInfo.nss != precoding.GetNumStreamsForUser(user.userId) ||
            userInfo.nss == 0 || user.rxPhyAntennas < userInfo.nss)
        {
            reject("per-user NSS is inconsistent with the precoder or receiver PHY");
        }
        const auto ru = std::find(ruGroups.begin(), ruGroups.end(), userInfo.ru);
        uint32_t group = static_cast<uint32_t>(std::distance(ruGroups.begin(), ru));
        if (ru == ruGroups.end())
        {
            ruGroups.push_back(userInfo.ru);
        }
        const double bandwidthHz =
            static_cast<double>(HeRu::GetBandwidth(userInfo.ru.GetRuType())) * 1e6;
        if (!std::isfinite(bandwidthHz) || bandwidthHz <= 0.0)
        {
            reject("HE RU bandwidth is invalid");
        }
        const auto linkBudget =
            WifiLinkBudget::Calculate(WToDbm(user.receivedPowerW),
                                      0.0,
                                      bandwidthHz,
                                      WIFI_NOISE_TEMPERATURE_K,
                                      receiverNoiseFigureDb);
        heUsers.push_back({user.userId,
                           linkBudget,
                           txVector,
                           user.packetSizeBytes,
                           user.staId,
                           group,
                           user.subcarrierIndices});
    }

    for (const auto& snapshotUser : snapshot.users)
    {
        if (inputs.find(snapshotUser.userId) == inputs.end())
        {
            reject("a snapshot user has no receiver input");
        }
    }
    for (const auto& [staId, userInfo] : userInfoMap)
    {
        static_cast<void>(userInfo);
        if (staIds.find(staId) == staIds.end())
        {
            reject("HeMuUserInfoMap contains an unknown or unaddressed receiver");
        }
    }

    auto abstraction = CreateObject<HeDlMuMimoLinkAbstraction>();
    abstraction->SetEffectiveSinrModel(GetEffectiveSinrModel());
    for (const auto& [mcs, beta] : m_eesmBetas)
    {
        abstraction->SetEesmBeta(mcs, beta);
    }
    for (const auto& [key, offsetDb] : m_awgnSnrOffsetsDb)
    {
        abstraction->SetAwgnSnrOffsetDb(key, offsetDb);
    }
    abstraction->SetErrorRateModel(errorRateModel);

    SionnaHeDlMuMimoSpectrumRxResult result;
    result.m_linkResult = abstraction->Calculate(snapshot, precoding, heUsers);
    result.m_equivalentPowers.reserve(snapshot.users.size());
    for (const auto& snapshotUser : snapshot.users)
    {
        const auto& input = *inputs.at(snapshotUser.userId);
        const auto& userInfo = userInfoMap.at(input.staId);
        const auto& link = result.m_linkResult.GetUserById(snapshotUser.userId);
        const double diversityGain =
            errorRateModel->IsAwgn() && input.rxPhyAntennas > userInfo.nss
                ? static_cast<double>(input.rxPhyAntennas) / userInfo.nss
                : 1.0;
        const double equivalentPower =
            link.GetEffectiveSinrLinear() *
            link.GetLinkBudget().GetReceiverNoisePowerW() / diversityGain;
        if (!std::isfinite(equivalentPower) || equivalentPower < 0.0)
        {
            reject("MU abstraction produced invalid equivalent receive power");
        }
        result.m_equivalentPowers.push_back({snapshotUser.userId, equivalentPower});
    }
    return result;
}

Ptr<SpectrumValue>
SionnaSpectrumPropagationLossModel::DoCalcRxPowerSpectralDensity(Ptr<const SpectrumSignalParameters> params,
                                                Ptr<const MobilityModel> a, Ptr<const MobilityModel> b) const
{
    NS_LOG_FUNCTION(this);
    uint32_t aId = a->GetObject<Node>()->GetId(); // Id of the node a
    uint32_t bId = b->GetObject<Node>()->GetId(); // Id of the node b

    NS_LOG_DEBUG(std::fixed << std::setprecision(9) << Simulator::Now().GetSeconds() <<"s: DoCalcRxPowerSpectralDensity for link " << aId << " - " << bId);

    NS_ASSERT_MSG(aId != bId, "The two nodes must be different from one another");
    NS_ASSERT_MSG(a->GetDistanceFrom(b) > 0.0,
                  "The position of a and b devices cannot be the same");

    // get PSD size
    auto spectrum_bin_size
        = (params->psd->GetSpectrumModel()->Begin()+1)->fc - (params->psd->GetSpectrumModel()->Begin()->fc);
    auto spectrum_dim
        = (params->psd->GetSpectrumModel()->End()-1)->fc - (params->psd->GetSpectrumModel()->Begin()->fc);

    NS_LOG_DEBUG("\t PSD size: " << params->psd->GetValuesN() << " df=" << spectrum_bin_size
        << "Hz, " << "Total=" << spectrum_dim << "Hz");

    Ptr<SpectrumValue> rxPsd = Copy<SpectrumValue>(params->psd);

    Ptr<const WifiSpectrumSignalParameters> wifiParams =
        DynamicCast<const WifiSpectrumSignalParameters>(params);
    const bool hasWifiPpdu = wifiParams && wifiParams->ppdu;
    const WifiPreamble preamble =
        hasWifiPpdu ? wifiParams->ppdu->GetTxVector().GetPreambleType()
                    : WIFI_PREAMBLE_LONG;
    NS_ABORT_MSG_IF(preamble == WIFI_PREAMBLE_EHT_MU ||
                        preamble == WIFI_PREAMBLE_EHT_TB,
                    "The Sionna MIMO adapter does not support EHT MU PPDUs");
    const bool isHeMu = preamble == WIFI_PREAMBLE_HE_MU;
    const bool reciprocalUplink = m_propagationCache &&
        m_propagationCache->IsReciprocalUplink(aId, bId);
    // STA-to-STA overhearing retains the documented scalar approximation.
    const bool overheardUplink = m_propagationCache &&
        m_propagationCache->IsReciprocalStation(aId) &&
        m_propagationCache->IsReciprocalStation(bId);

    Ptr<const HePpdu> heMuPpdu =
        isHeMu ? DynamicCast<const HePpdu>(wifiParams->ppdu) : nullptr;
    NS_ABORT_MSG_IF(isHeMu && !heMuPpdu, "WIFI_PREAMBLE_HE_MU requires an HePpdu");
    const bool heMuPayload = heMuPpdu &&
                            heMuPpdu->GetTxPsdFlag() == HePpdu::PSD_HE_PORTION;

    // SpectrumChannel also delivers to bystanders and hears other transmitters.
    // Physical directed channels apply to every AP-to-STA transmission, while
    // the selected MU state applies only to an addressed MU payload.
    const MultiUserChannel* transmissionReceiver = nullptr;
    if (m_downlinkTransmissionSnapshot)
    {
        for (const auto& user : m_downlinkTransmissionSnapshot->users)
        {
            if (user.txNodeId == aId && user.rxNodeId == bId)
            {
                NS_ABORT_MSG_IF(transmissionReceiver,
                                "Directed downlink snapshot contains the receiver twice");
                transmissionReceiver = &user;
            }
        }
    }

    const MultiUserChannel* muReceiver = nullptr;
    if (isHeMu && HasDlMuMimoState())
    {
        for (const auto& user : m_dlMuSnapshot->users)
        {
            if (user.txNodeId == aId && user.rxNodeId == bId)
            {
                NS_ABORT_MSG_IF(muReceiver,
                                "HE DL MU snapshot contains the directed receiver twice");
                muReceiver = &user;
            }
        }
    }
    if (muReceiver)
    {
        Ptr<WifiSpectrumPhyInterface> txInterface =
            DynamicCast<WifiSpectrumPhyInterface>(params->txPhy);
        NS_ABORT_MSG_IF(!txInterface, "Wi-Fi transmitter has no spectrum PHY interface");
        const auto rxPhy = GetSpectrumWifiPhy(b, txInterface->GetChannel());
        const auto rxDevice = DynamicCast<WifiNetDevice>(rxPhy->GetDevice());
        bool addressed = false;
        for (const auto& [staId, userInfo] : heMuPpdu->GetTxVector().GetHeMuUserInfoMap())
        {
            static_cast<void>(userInfo);
            const auto psdu = heMuPpdu->GetPsdu(0, staId);
            addressed |= psdu && psdu->GetAddr1() == rxDevice->GetMac()->GetAddress();
        }
        if (!addressed)
        {
            // A receiver from an older installed group can also overhear a PPDU.
            muReceiver = nullptr;
        }
    }
    NS_ABORT_MSG_IF(!transmissionReceiver && !muReceiver && !m_fixedChannelEnabled && !m_propagationCache,
                    "Physical propagation for this link requires a cache or fixed CFR");

    // SpectrumChannel has already applied the scalar wideband path loss.
    const double wbLoss =
        transmissionReceiver ? transmissionReceiver->widebandLossDb
                             : (muReceiver ? muReceiver->widebandLossDb
                   : (m_fixedChannelEnabled
                          ? m_fixedPathLossDb
                          : m_propagationCache->GetPropagationLoss(a, b)));
    MimoChannelMatrix channel =
        transmissionReceiver ? transmissionReceiver->channel
                             : (muReceiver ? muReceiver->channel
                   : (m_fixedChannelEnabled
                          ? m_fixedChannel
                          : m_propagationCache->GetPropagationChannelMatrix(a, b)));
    const uint32_t psdSubcarriers = params->psd->GetValuesN() - 1;
    std::optional<MimoPrecodingMatrix> sampledPrecoder;
    const MimoPrecodingMatrix* suPrecoder = m_precodingMatrix ? &*m_precodingMatrix : nullptr;
    // Minstrel and legacy responses can occupy a narrower primary channel.
    // Sample actual frequency centres, preserving the original wideband loss.
    if (hasWifiPpdu && m_propagationCache && !m_fixedChannelEnabled &&
        channel.GetNumSubcarriers() != psdSubcarriers &&
        !(!heMuPayload && preamble != WIFI_PREAMBLE_HE_SU &&
          spectrum_bin_size == 312500.0 && channel.GetNumSubcarriers() == 4 * psdSubcarriers))
    {
        const auto transmitter = GetTransmitterSpectrumWifiPhy(params);
        const double firstHz = transmitter->GetFrequency() * 1e6 -
                               (channel.GetNumSubcarriers() / 2) * 78125.0;
        const double offset = (params->psd->GetSpectrumModel()->Begin()->fc - firstHz) / 78125.0;
        const double spacing = spectrum_bin_size / 78125.0;
        const auto first = std::llround(offset), stride = std::llround(spacing);
        NS_ABORT_MSG_IF(first < 0 || stride < 1 || std::abs(offset - first) > 1e-6 ||
                        std::abs(spacing - stride) > 1e-6 ||
                        first + (static_cast<int64_t>(psdSubcarriers) - 1) * stride >=
                            channel.GetNumSubcarriers(),
                        "Transmitted spectrum lies outside the configured HE CFR grid");
        MimoChannelMatrix selected(psdSubcarriers, channel.GetNumRxAntennas(), channel.GetNumTxAntennas());
        const bool selectPrecoder = preamble == WIFI_PREAMBLE_HE_SU &&
                                    !reciprocalUplink && !overheardUplink && suPrecoder;
        std::vector<std::complex<double>> coefficients;
        if (selectPrecoder) suPrecoder->ValidateForChannel(channel);
        for (uint32_t sc = 0; sc < psdSubcarriers; ++sc)
        {
            const auto source = first + sc * stride;
            for (uint32_t rx = 0; rx < channel.GetNumRxAntennas(); ++rx)
                for (uint32_t tx = 0; tx < channel.GetNumTxAntennas(); ++tx)
                    selected.Set(sc, rx, tx, channel.Get(source, rx, tx));
            if (selectPrecoder)
                for (uint32_t tx = 0; tx < suPrecoder->GetNumTxAntennas(); ++tx)
                    for (uint32_t stream = 0; stream < suPrecoder->GetNumStreams(); ++stream)
                        coefficients.push_back(suPrecoder->Get(source, tx, stream));
        }
        if (selectPrecoder)
        {
            sampledPrecoder.emplace(psdSubcarriers, suPrecoder->GetNumTxAntennas(),
                                     suPrecoder->GetNumStreams(), coefficients);
            suPrecoder = &*sampledPrecoder;
        }
        channel = std::move(selected);
    }
    // ns-3 sends the common non-HE part on a 312.5 kHz grid; the installed
    // HE CFR uses 78.125 kHz. Sample the same frequency centres for that part.
    const bool coarseCommonGrid = !heMuPayload && preamble != WIFI_PREAMBLE_HE_SU &&
                                  spectrum_bin_size == 312500.0 &&
                                  channel.GetNumSubcarriers() == 4 * psdSubcarriers;
    NS_ABORT_MSG_IF(channel.GetNumSubcarriers() != psdSubcarriers && !coarseCommonGrid,
                    "PSD/CFR grids must coincide, or use the common-preamble 4:1 grid");
    const uint32_t scalarStride = coarseCommonGrid ? 4 : 1;

    const bool isHeSu =
        hasWifiPpdu &&
        preamble == WIFI_PREAMBLE_HE_SU &&
        wifiParams->ppdu->GetTxVector().GetModeInitialized() &&
        wifiParams->ppdu->GetTxVector().GetMode().GetModulationClass() ==
            WIFI_MOD_CLASS_HE;

    if (isHeSu && !overheardUplink)
    {
        NS_ABORT_MSG_IF(GetEffectiveSinrModel() == SionnaMimoEffectiveSinrModel::EESM &&
                        !HasEesmBeta(wifiParams->ppdu->GetTxVector().GetMode().GetMcsValue()),
                        "Every HE data MCS in EESM mode requires an explicit beta");
        const uint32_t expectedNss = !reciprocalUplink && suPrecoder
                                         ? suPrecoder->GetNumStreams()
                                         : channel.GetNumTxAntennas();
        NS_ABORT_MSG_IF(wifiParams->ppdu->GetTxVector().GetNss() != expectedNss,
                        "WifiTxVector NSS must equal the transmitted stream dimension");
        if (!reciprocalUplink && suPrecoder)
        {
            suPrecoder->ValidateForChannel(channel);
        }
    }

    const bool legacySiso = channel.GetNumRxAntennas() == 1 &&
                            channel.GetNumTxAntennas() == 1;
    NS_ABORT_MSG_IF(isHeSu && !legacySiso &&
                    GetEffectiveSinrModel() == SionnaMimoEffectiveSinrModel::LEGACY_SISO,
                    "LEGACY_SISO requires 1x1 reception");
    const bool scalarCompatibilityPath = legacySiso || !isHeSu || overheardUplink;

    Ptr<const WifiPsdu> addressedMuPsdu;
    if (heMuPayload && muReceiver)
    {
        Ptr<SpectrumWifiPhy> txPhy = GetTransmitterSpectrumWifiPhy(params);
        Ptr<WifiSpectrumPhyInterface> txInterface =
            DynamicCast<WifiSpectrumPhyInterface>(params->txPhy);
        NS_ABORT_MSG_IF(!txInterface, "Wi-Fi transmitter has no spectrum PHY interface");
        Ptr<WifiNetDevice> txDevice = DynamicCast<WifiNetDevice>(txPhy->GetDevice());
        NS_ABORT_MSG_IF(!txDevice || !txDevice->GetMac() ||
                            txDevice->GetMac()->GetTypeOfStation() != AP,
                        "HE DL MU transmitter must be an AP");

        Ptr<const HePpdu> hePpdu = DynamicCast<const HePpdu>(wifiParams->ppdu);
        NS_ABORT_MSG_IF(!hePpdu, "WIFI_PREAMBLE_HE_MU requires an HePpdu");
        const auto& txVector = hePpdu->GetTxVector();
        NS_ABORT_MSG_IF(txPhy->GetNumberOfAntennas() !=
                            m_dlMuSnapshot->users.front().channel.GetNumTxAntennas() ||
                            txPhy->GetMaxSupportedTxSpatialStreams() <
                                m_dlMuPrecoding->GetNumStreams(),
                        "Transmitter PHY dimensions do not support the joint MU precoder");
        NS_ABORT_MSG_IF(!std::isfinite(m_receiverNoiseFigureDb),
                        "Configure the MIMO receiver noise figure explicitly and consistently "
                        "with SpectrumWifiPhy RxNoiseFigure");

        Ptr<SpectrumWifiPhy> currentRxPhy =
            GetSpectrumWifiPhy(b, txInterface->GetChannel());
        Ptr<HePhy> hePhy =
            DynamicCast<HePhy>(txPhy->GetPhyEntity(WIFI_MOD_CLASS_HE));
        NS_ABORT_MSG_IF(!hePhy, "Transmitter PHY has no HE entity");
        Ptr<ErrorRateModel> errorRateModel = GetErrorRateModel(currentRxPhy);

        std::vector<SionnaHeDlMuMimoSpectrumUserTransmission> users;
        users.reserve(m_dlMuSnapshot->users.size());
        std::optional<WifiSpectrumBandIndices> currentRuIndices;
        for (const auto& snapshotUser : m_dlMuSnapshot->users)
        {
            NS_ABORT_MSG_IF(snapshotUser.rxNodeId >= NodeList::GetNNodes(),
                            "HE DL MU snapshot references an unknown receiver node");
            Ptr<Node> userNode = NodeList::GetNode(snapshotUser.rxNodeId);
            Ptr<MobilityModel> userMobility = userNode->GetObject<MobilityModel>();
            NS_ABORT_MSG_IF(!userMobility,
                            "HE DL MU receiver node has no mobility model");
            Ptr<SpectrumWifiPhy> userPhy =
                GetSpectrumWifiPhy(userMobility, txInterface->GetChannel());
            Ptr<WifiNetDevice> userDevice = DynamicCast<WifiNetDevice>(userPhy->GetDevice());
            NS_ABORT_MSG_IF(!userDevice || !userDevice->GetMac() ||
                                userDevice->GetMac()->GetTypeOfStation() == AP,
                            "HE DL MU receiver must be a non-AP Wi-Fi station");

            uint16_t staId = SU_STA_ID;
            Ptr<const WifiPsdu> psdu;
            for (const auto& [candidateStaId, userInfo] : txVector.GetHeMuUserInfoMap())
            {
                static_cast<void>(userInfo);
                Ptr<const WifiPsdu> candidate = hePpdu->GetPsdu(0, candidateStaId);
                if (candidate && candidate->GetAddr1() == userDevice->GetMac()->GetAddress())
                {
                    NS_ABORT_MSG_IF(psdu,
                                    "HE DL MU receiver address maps to multiple STA-IDs");
                    staId = candidateStaId;
                    psdu = candidate;
                }
            }
            NS_ABORT_MSG_IF(!psdu || staId == SU_STA_ID,
                            "Unknown or unaddressed HE DL MU receiver");
            const auto& userInfo = txVector.GetHeMuUserInfo(staId);
            NS_ABORT_MSG_IF(userPhy->GetNumberOfAntennas() !=
                                snapshotUser.channel.GetNumRxAntennas() ||
                                userPhy->GetMaxSupportedRxSpatialStreams() < userInfo.nss,
                            "Receiver PHY dimensions do not match HE MU NSS and snapshot CFR");

            NS_ABORT_MSG_IF(GetErrorRateModel(userPhy)->GetInstanceTypeId() !=
                                errorRateModel->GetInstanceTypeId(),
                            "HE DL MU receivers must use the same ErrorRateModel type");

            // params->psd and the snapshot use the transmitter spectrum grid,
            // which can differ from a receiver's operating-channel grid.
            const auto ruBand = hePhy->GetRuBandForTx(txVector, staId);
            const auto width = txVector.GetChannelWidth();
            const auto ru = userInfo.ru;
            const auto tones = HeRu::GetSubcarrierGroup(
                width,
                ru.GetRuType(),
                ru.GetPhyIndex(width, txPhy->GetOperatingChannel().GetPrimaryChannelIndex(20)));
            NS_ABORT_MSG_IF(spectrum_bin_size != 78125.0,
                            "HE MU payload requires the 78.125 kHz CFR grid");
            std::vector<uint32_t> occupiedBins;
            for (const auto& range : tones)
            {
                const auto bins = HePhy::ConvertHeRuSubcarriers(
                    width, txPhy->GetGuardBandwidth(width), 78125, range, 0);
                NS_ABORT_MSG_IF(bins.second >= snapshotUser.channel.GetNumSubcarriers(),
                                "Occupied RU tone is outside the CFR grid");
                for (uint32_t bin = bins.first; bin <= bins.second; ++bin)
                {
                    occupiedBins.push_back(bin);
                }
            }
            if (GetEffectiveSinrModel() == SionnaMimoEffectiveSinrModel::MEAN_UNCODED_BER)
            {
                occupiedBins = HeDataToneMask::GetDataBins(width, ru.GetRuType(),
                    ru.GetPhyIndex(width, txPhy->GetOperatingChannel().GetPrimaryChannelIndex(20)),
                    psdSubcarriers / 2);
            }
            NS_ABORT_MSG_IF(occupiedBins.empty(), "HE MU RU contains no occupied tones");
            const double currentPathAttenuatedRuPowerW =
                IntegralBand(*params->psd, ruBand.indices);
            NS_ABORT_MSG_IF(!std::isfinite(currentPathAttenuatedRuPowerW) ||
                                currentPathAttenuatedRuPowerW <= 0.0,
                            "HE MU RU has no finite received power");
            const double apRuPowerW =
                currentPathAttenuatedRuPowerW * DbToRatio(wbLoss);
            const double userReceivedPowerW =
                apRuPowerW / DbToRatio(snapshotUser.widebandLossDb) *
                DbToRatio(userPhy->GetRxGain());
            users.push_back({snapshotUser.userId,
                             staId,
                             userReceivedPowerW,
                             userPhy->GetNumberOfAntennas(),
                             psdu->GetSize(),
                             occupiedBins});
            if (snapshotUser.rxNodeId == bId)
            {
                addressedMuPsdu = psdu;
                currentRuIndices = ruBand.indices;
            }
        }
        NS_ABORT_MSG_IF(!addressedMuPsdu || !currentRuIndices,
                        "Current receiver is not addressed by the HE DL MU PPDU");

        const auto muRx = CalculateHeDlMuMimoRx(*m_dlMuSnapshot,
                                                *m_dlMuPrecoding,
                                                txVector,
                                                users,
                                                txPhy->GetNumberOfAntennas(),
                                                m_receiverNoiseFigureDb,
                                                errorRateModel);
        const double rxGainLinear = DbToRatio(currentRxPhy->GetRxGain());
        const double equivalentPowerBeforeRxGain =
            muRx.GetEquivalentReceivedPowerW(muReceiver->userId) / rxGainLinear;
        const double currentPowerBeforeRxGain =
            IntegralBand(*rxPsd, *currentRuIndices);
        NS_ABORT_MSG_IF(currentPowerBeforeRxGain <= 0.0,
                        "Path-loss-attenuated HE MU RU PSD has no power");
        // For physical energy on another allocated RU, retain its actual
        // precoder instead of assigning an isotropic antenna gain.
        std::map<uint32_t, HeRu::RuSpec> ruByUser;
        for (const auto& user : users)
        {
            ruByUser.emplace(user.userId, txVector.GetRu(user.staId));
        }
        const double scale = equivalentPowerBeforeRxGain / currentPowerBeforeRxGain;
        // Only the user's RU carries its equivalent payload power. Outside it,
        // preserve physical energy for the receiving PHY's interference bands.
        auto value = rxPsd->ValuesBegin();
        for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc, ++value)
        {
            if (sc >= currentRuIndices->first && sc <= currentRuIndices->second)
            {
                *value *= scale;
            }
            else
            {
                double gain = 0.0;
                for (uint32_t tx = 0; tx < channel.GetNumTxAntennas(); ++tx)
                {
                    gain += std::norm(channel.Get(sc, 0, tx));
                }
                gain /= channel.GetNumTxAntennas(); // unallocated spectral-mask bins
                for (const auto& user : users)
                {
                    if (!std::binary_search(user.subcarrierIndices.begin(),
                                            user.subcarrierIndices.end(), sc))
                    {
                        continue;
                    }
                    double received = 0.0;
                    double activePower = 0.0;
                    const auto& matrix = m_dlMuPrecoding->GetPrecodingMatrix();
                    for (uint32_t column = 0; column < m_dlMuPrecoding->GetNumStreams(); ++column)
                    {
                        const auto owner = m_dlMuPrecoding->GetStreamMapping(column).userId;
                        if (ruByUser.at(owner) != ruByUser.at(user.userId))
                        {
                            continue;
                        }
                        std::complex<double> effective{0.0, 0.0};
                        for (uint32_t tx = 0; tx < channel.GetNumTxAntennas(); ++tx)
                        {
                            effective += channel.Get(sc, 0, tx) * matrix.Get(sc, tx, column);
                        }
                        received += std::norm(effective);
                        activePower += m_dlMuPrecoding->GetStreamPower(sc, column);
                    }
                    // The numerical calculation already validated positive group power.
                    gain = received / activePower;
                    break;
                }
                *value *= gain;
            }
        }

        const auto& userResult = muRx.GetLinkResult().GetUserById(muReceiver->userId);
        m_mimoRxTrace(wifiParams->ppdu->GetUid(),
                      aId,
                      bId,
                      userResult.GetEffectiveSinrLinear(),
                      userResult.GetPacketErrorRate(),
                      0);
        m_dlMuMimoRxTrace(wifiParams->ppdu->GetUid(),
                          muReceiver->userId,
                          aId,
                          bId,
                          userResult);
    }

    else if (scalarCompatibilityPath)
    {
        // Preserve the established 1x1 path. Non-HE control frames on a MIMO
        // PHY use the first antenna pair because Phase 1 models HE SU payloads only.
        auto value = rxPsd->ValuesBegin();
        for (uint32_t sc = 0; sc < psdSubcarriers; ++sc, ++value)
        {
            double gain = std::norm(channel.Get(sc * scalarStride, 0, 0));
            if (isHeMu)
            {
                // Common fields and overheard HE MU energy use equal power
                // across TX antennas, with no payload precoder or EESM beta.
                gain = 0.0;
                for (uint32_t tx = 0; tx < channel.GetNumTxAntennas(); ++tx)
                {
                    gain += std::norm(channel.Get(sc * scalarStride, 0, tx));
                }
                gain /= channel.GetNumTxAntennas();
            }
            *value *= gain;
        }
    }
    else
    {
        NS_ABORT_MSG_IF(!wifiParams || !wifiParams->ppdu,
                        "Dimensioned MIMO CFR requires Wi-Fi signal parameters");

        Ptr<SpectrumWifiPhy> txPhy = GetTransmitterSpectrumWifiPhy(params);
        Ptr<WifiSpectrumPhyInterface> txInterface =
            DynamicCast<WifiSpectrumPhyInterface>(params->txPhy);
        NS_ABORT_MSG_IF(!txInterface, "Wi-Fi transmitter has no spectrum PHY interface");
        Ptr<SpectrumWifiPhy> rxPhy = GetSpectrumWifiPhy(b, txInterface->GetChannel());
        const auto& txVector = wifiParams->ppdu->GetTxVector();
        Ptr<const WifiPsdu> psdu = wifiParams->ppdu->GetPsdu();
        NS_ABORT_MSG_IF(!psdu, "HE SU PPDU does not contain a PSDU");
        NS_ABORT_MSG_IF(txPhy->GetMaxSupportedTxSpatialStreams() < txVector.GetNss(),
                        "Transmitter PHY does not support WifiTxVector NSS");
        NS_ABORT_MSG_IF(rxPhy->GetMaxSupportedRxSpatialStreams() < txVector.GetNss(),
                        "Receiver PHY does not support WifiTxVector NSS");
        NS_ABORT_MSG_IF(!std::isfinite(m_receiverNoiseFigureDb),
                        "Configure the MIMO receiver noise figure explicitly and consistently "
                        "with SpectrumWifiPhy RxNoiseFigure");

        const double rxGainLinear = DbToRatio(rxPhy->GetRxGain());
        const double receivedPowerW = Integral(*params->psd) * rxGainLinear;
        std::vector<uint32_t> dataIndices;
        if (GetEffectiveSinrModel() == SionnaMimoEffectiveSinrModel::MEAN_UNCODED_BER)
        {
            const auto width = txVector.GetChannelWidth();
            const auto type = width == 20 ? HeRu::RU_242_TONE : width == 40 ? HeRu::RU_484_TONE :
                              width == 80 ? HeRu::RU_996_TONE : HeRu::RU_2x996_TONE;
            dataIndices = HeDataToneMask::GetDataBins(width, type, 1, psdSubcarriers / 2);
        }
        const auto mimoRx = CalculateMimoRx(channel,
                                            receivedPowerW,
                                            txVector,
                                            psdu->GetSize(),
                                            txPhy->GetNumberOfAntennas(),
                                            rxPhy->GetNumberOfAntennas(),
                                            txVector.GetChannelWidth() * 1e6,
                                            m_receiverNoiseFigureDb,
                                            GetErrorRateModel(rxPhy), dataIndices, !reciprocalUplink, suPrecoder);

        const double equivalentPowerBeforeRxGain =
            mimoRx.GetEquivalentReceivedPowerW() / rxGainLinear;
        const double currentPowerBeforeRxGain = Integral(*rxPsd);
        NS_ABORT_MSG_IF(currentPowerBeforeRxGain <= 0.0,
                        "Path-loss-attenuated MIMO PSD has no power");
        *rxPsd *= equivalentPowerBeforeRxGain / currentPowerBeforeRxGain;

        m_mimoRxTrace(wifiParams->ppdu->GetUid(),
                      aId,
                      bId,
                      mimoRx.GetLinkResult().GetEffectiveSinrLinear(),
                      mimoRx.GetLinkResult().GetPacketErrorRate(),
                      mimoRx.GetLinkResult().GetMimoResult().GetMinimumChannelRank());
    }

    if (isHeSu && legacySiso &&
        GetEffectiveSinrModel() == SionnaMimoEffectiveSinrModel::MEAN_UNCODED_BER &&
        DynamicCast<WifiSpectrumPhyInterface>(params->txPhy))
    {
        const auto txInterface = DynamicCast<WifiSpectrumPhyInterface>(params->txPhy);
        const auto rxPhy = GetSpectrumWifiPhy(b, txInterface->GetChannel());
        auto wrapper = DynamicCast<HeBerErrorRateModel>(GetErrorRateModel(rxPhy));
        if (!wrapper)
        {
            wrapper = CreateObject<HeBerErrorRateModel>();
            wrapper->Configure(GetErrorRateModel(rxPhy));
            rxPhy->SetErrorRateModel(wrapper);
        }
        const auto width = wifiParams->ppdu->GetTxVector().GetChannelWidth();
        const auto type = width == 20 ? HeRu::RU_242_TONE : width == 40 ? HeRu::RU_484_TONE :
                          width == 80 ? HeRu::RU_996_TONE : HeRu::RU_2x996_TONE;
        NS_ABORT_MSG_IF(!std::isfinite(m_receiverNoiseFigureDb),
                        "Configure receiver noise figure to match SpectrumWifiPhy");
        const double noise = WifiLinkBudget::Calculate(0.0, 0.0, width * 1e6,
            WIFI_NOISE_TEMPERATURE_K, m_receiverNoiseFigureDb).GetReceiverNoisePowerW();
        const double rxGain = DbToRatio(rxPhy->GetRxGain());
        // A rate controller can transmit a narrower PPDU than the operating
        // channel. Its PSD has its own grid; PHY band indices refer to the
        // operating RX grid and cannot index this transmitted PSD directly.
        const uint32_t occupiedBins = std::llround(width * 1e6 / spectrum_bin_size);
        NS_ABORT_MSG_IF(occupiedBins == 0 || occupiedBins > psdSubcarriers,
                        "HE transmission bandwidth is outside the PSD grid");
        const uint32_t firstBin = (psdSubcarriers - occupiedBins) / 2;
        const double scalar = IntegralBand(*rxPsd, {firstBin, firstBin + occupiedBins - 1}) *
                              rxGain / noise;
        if (scalar > 0)
        {
            std::vector<double> tones;
            for (uint32_t bin : HeDataToneMask::GetDataBins(width, type, 1, psdSubcarriers / 2))
            {
                NS_ABORT_MSG_IF(bin >= channel.GetNumSubcarriers(), "HE data tone outside CFR grid");
                tones.push_back((*rxPsd)[bin] * rxGain / (noise / (width * 1e6)));
            }
            wrapper->SetReception(wifiParams->ppdu->GetUid(), tones, scalar, params->duration);
        }
    }

    // Calibration changes only payload error probabilities. Keep the physical
    // scalar PSD for CCA, headers, and overlapping receivers. Per-PHY snapshots
    // avoid putting receiver-specific calibration state in shared packet tags.
    if (isHeSu && legacySiso && !m_awgnSnrOffsetsDb.empty() &&
        DynamicCast<WifiSpectrumPhyInterface>(params->txPhy))
    {
        const auto txInterface = DynamicCast<WifiSpectrumPhyInterface>(params->txPhy);
        const auto rxPhy = GetSpectrumWifiPhy(b, txInterface->GetChannel());
        const auto calibrated = DynamicCast<HeCalibratedErrorRateModel>(GetErrorRateModel(rxPhy));
        const auto psdu = wifiParams->ppdu->GetPsdu();
        if (calibrated && psdu &&
            calibrated->Matches(wifiParams->ppdu->GetTxVector(), *psdu, rxPhy->GetNumberOfAntennas()))
        {
            const auto txPhy = GetTransmitterSpectrumWifiPhy(params);
            const double centerHz = txPhy->GetOperatingChannel().GetPrimaryChannelCenterFrequency(20) * 1e6;
            const double rxGain = DbToRatio(rxPhy->GetRxGain());
            const double noise = WifiLinkBudget::Calculate(0.0, 0.0, 20e6,
                WIFI_NOISE_TEMPERATURE_K, m_receiverNoiseFigureDb).GetReceiverNoisePowerW();
            const double scalarSnr = IntegralBand(*rxPsd, txPhy->GetBand(20).indices) * rxGain / noise;
            std::vector<double> tones;
            auto band = params->psd->GetSpectrumModel()->Begin();
            for (uint32_t sc = 0; sc < psdSubcarriers; ++sc, ++band)
            {
                // HE 20 MHz data tones: [-122,-2] and [2,122], excluding pilots.
                const auto tone = std::llround((band->fc - centerHz) / 78125.0);
                const auto absolute = std::abs(tone);
                if (absolute < 2 || absolute > 122 || absolute == 22 || absolute == 48 ||
                    absolute == 90 || absolute == 116)
                {
                    continue;
                }
                tones.push_back((*params->psd)[sc] * rxGain * std::norm(channel.Get(sc, 0, 0)) /
                                (noise / 20e6));
            }
            calibrated->SetReception(wifiParams->ppdu->GetUid(), tones, scalarSnr, params->duration);
        }
    }

    // Common fields and bystanders have no addressed MU payload to annotate.
    if (isHeMu && !addressedMuPsdu)
    {
        return rxPsd;
    }

    auto tagCoefficients = channel.GetCoefficients();
    if (legacySiso)
    {
        tagCoefficients.emplace_back(1.0, 0.0);
    }

    // tag the packet payload with CFR for later processing in application layer
    if (auto wifiTxParams = DynamicCast<const WifiSpectrumSignalParameters>(params))
    {   // WiFi packet found
        Ptr<WifiPpdu> ppdu = wifiTxParams->ppdu->Copy();
        Ptr<const WifiPsdu> psdu =
            isHeMu ? addressedMuPsdu : ppdu->GetPsdu();
        NS_ABORT_MSG_IF(!psdu,
                        "Wi-Fi PPDU has no PSDU addressed to this receiver");

        // for each payload
        for (size_t payload_idx = 0; payload_idx < psdu->GetNMpdus(); payload_idx++) {
            Ptr<const Packet> p = psdu->GetPayload(payload_idx);

            CFRTag tag;
            if (p->PeekPacketTag(tag))
            {
                //std::cout << "tag already exists; replace H" << std::endl;
                tag.SetComplexes(tagCoefficients);
                tag.SetPathloss(wbLoss);
            } else
            {
                //std::cout << "add new tag with H" << std::endl;
                tag.SetComplexes(tagCoefficients);
                tag.SetPathloss(wbLoss);
                p->AddPacketTag(tag);
            }
        }
    } else
    {
        // TODO: support of LTE
        NS_LOG_WARN("Unknown SpectrumSignalParameters: " << typeid(params).name() << "; packet not annotated with CSI");
    }

    return rxPsd;
}

} // namespace ns3
