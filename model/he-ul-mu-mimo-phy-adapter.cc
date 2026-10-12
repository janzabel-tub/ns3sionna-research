/* SPDX-License-Identifier: GPL-2.0-only */

#include "he-ul-mu-mimo-phy-adapter.h"
#include "he-data-tone-mask.h"

#include "ns3/ctrl-headers.h"
#include "ns3/error-rate-model.h"
#include "ns3/he-ppdu.h"
#include "ns3/wifi-utils.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace ns3
{

namespace
{

constexpr double WIFI_NOISE_TEMPERATURE_K = 290.0;

[[noreturn]] void
Reject(const std::string& reason)
{
    throw std::invalid_argument("HeUlMuMimoPhyAdapter: " + reason);
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
    return subset;
}

} // namespace

NS_OBJECT_ENSURE_REGISTERED(HeUlMuMimoPhyAdapter);

TypeId
HeUlMuMimoPhyAdapter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HeUlMuMimoPhyAdapter")
                            .SetParent<Object>()
                            .SetGroupName("Sionna")
                            .AddConstructor<HeUlMuMimoPhyAdapter>();
    return tid;
}

const HeRu::RuSpec&
HeUlMuMimoPhyGroupResult::GetRu() const
{
    return m_ru;
}

const HeUlMuMimoLinkResult&
HeUlMuMimoPhyGroupResult::GetLinkResult() const
{
    return m_linkResult;
}

uint32_t
HeUlMuMimoPhyResult::GetNumGroups() const
{
    return static_cast<uint32_t>(m_groups.size());
}

uint32_t
HeUlMuMimoPhyResult::GetNumDetectedUsers() const
{
    return static_cast<uint32_t>(m_equivalentPowers.size());
}

const HeUlMuMimoPhyGroupResult&
HeUlMuMimoPhyResult::GetGroup(uint32_t index) const
{
    return m_groups.at(index);
}

const HeUlMuMimoUserResult&
HeUlMuMimoPhyResult::GetUserById(uint32_t userId) const
{
    for (const auto& group : m_groups)
    {
        for (uint32_t index = 0; index < group.GetLinkResult().GetNumUsers(); ++index)
        {
            const auto& user = group.GetLinkResult().GetUser(index);
            if (user.GetUserId() == userId)
            {
                return user;
            }
        }
    }
    Reject("result user ID is absent");
}

double
HeUlMuMimoPhyResult::GetEquivalentReceivedPowerW(uint32_t userId) const
{
    const auto found = std::lower_bound(
        m_equivalentPowers.begin(),
        m_equivalentPowers.end(),
        userId,
        [](const UserEquivalentPower& user, uint32_t id) { return user.userId < id; });
    if (found == m_equivalentPowers.end() || found->userId != userId)
    {
        Reject("equivalent-power user ID is absent");
    }
    return found->powerW;
}

const std::vector<uint16_t>&
HeUlMuMimoPhyResult::GetMissingStaIds() const
{
    return m_missingStaIds;
}

const std::vector<uint16_t>&
HeUlMuMimoPhyResult::GetNonSimultaneousStaIds() const
{
    return m_nonSimultaneousStaIds;
}

void
HeUlMuMimoPhyAdapter::SetErrorRateModel(Ptr<ErrorRateModel> errorRateModel)
{
    m_abstraction->SetErrorRateModel(errorRateModel);
}

Ptr<ErrorRateModel>
HeUlMuMimoPhyAdapter::GetErrorRateModel() const
{
    return m_abstraction->GetErrorRateModel();
}

void
HeUlMuMimoPhyAdapter::SetEffectiveSinrModel(SionnaMimoEffectiveSinrModel model)
{
    m_abstraction->SetEffectiveSinrModel(model);
}
SionnaMimoEffectiveSinrModel
HeUlMuMimoPhyAdapter::GetEffectiveSinrModel() const
{
    return m_abstraction->GetEffectiveSinrModel();
}

void
HeUlMuMimoPhyAdapter::SetEesmBeta(uint8_t mcs, double betaLinear)
{
    m_abstraction->SetEesmBeta(mcs, betaLinear);
}

void
HeUlMuMimoPhyAdapter::SetEesmBetaDb(uint8_t mcs, double betaDb)
{
    m_abstraction->SetEesmBetaDb(mcs, betaDb);
}

HeUlMuMimoPhyResult
HeUlMuMimoPhyAdapter::Calculate(
    const MultiUserChannelSnapshot& snapshot,
    const CtrlTriggerHeader& trigger,
    uint64_t triggerUid,
    const std::vector<HeUlMuMimoUserBinding>& bindings,
    const std::vector<HeUlMuMimoPpduReception>& receptions,
    Time coordinatedStart,
    Time coordinatedEnd,
    uint32_t apNodeId,
    uint8_t apRxAntennas,
    double receiverNoiseFigureDb,
    UlMuMimoDetector detector,
    double rankTolerance,
    uint8_t apPrimary20Index) const
{
    if (!trigger.IsBasic() || trigger.GetVariant() != TriggerFrameVariant::HE)
    {
        Reject("only HE Basic Trigger frames can coordinate data HE-TB PPDUs");
    }
    if (apPrimary20Index >= trigger.GetUlBandwidth() / 20)
    {
        Reject("AP primary 20 MHz index is outside the Trigger bandwidth");
    }
    if (triggerUid == 0)
    {
        Reject("the triggering PPDU UID must be non-zero");
    }
    if (coordinatedStart.IsNegative() || coordinatedEnd <= coordinatedStart)
    {
        Reject("the coordinated receive interval must be positive and ordered");
    }
    if (!std::isfinite(receiverNoiseFigureDb) || receiverNoiseFigureDb < 0.0)
    {
        Reject("receiver noise figure must be finite and non-negative");
    }
    if (!GetErrorRateModel())
    {
        Reject("an ErrorRateModel must be configured");
    }
    if (snapshot.direction != MultiUserChannelDirection::UPLINK || snapshot.users.empty())
    {
        Reject("the channel snapshot must contain uplink users");
    }

    std::map<uint32_t, const MultiUserChannel*> snapshotByUser;
    for (const auto& user : snapshot.users)
    {
        if (!snapshotByUser.emplace(user.userId, &user).second || user.rxNodeId != apNodeId ||
            user.txNodeId == apNodeId || user.channel.GetNumRxAntennas() != apRxAntennas ||
            user.channel.GetNumTxAntennas() != 1)
        {
            Reject("snapshot users must be unique one-antenna uplinks to the configured AP");
        }
    }

    if (bindings.size() != snapshot.users.size() ||
        bindings.size() != trigger.GetNUserInfoFields())
    {
        Reject("snapshot, bindings, and Trigger User Info fields must have equal size");
    }

    std::map<uint16_t, const HeUlMuMimoUserBinding*> bindingBySta;
    std::set<uint32_t> boundUsers;
    std::set<uint32_t> boundTransmitters;
    for (const auto& binding : bindings)
    {
        const auto snapshotUser = snapshotByUser.find(binding.userId);
        const auto triggerUser = trigger.FindUserInfoWithAid(binding.staId);
        if (binding.staId == SU_STA_ID || binding.staId == NO_USER_STA_ID ||
            !bindingBySta.emplace(binding.staId, &binding).second ||
            !boundUsers.insert(binding.userId).second ||
            !boundTransmitters.insert(binding.txNodeId).second ||
            snapshotUser == snapshotByUser.end() || triggerUser == trigger.end() ||
            snapshotUser->second->txNodeId != binding.txNodeId)
        {
            Reject("each Trigger STA requires one unique channel user and transmitter binding");
        }
        if (triggerUser->GetNss() != 1)
        {
            Reject("Phase 4.8 supports exactly one local stream per user");
        }
        if (GetEffectiveSinrModel() == SionnaMimoEffectiveSinrModel::EESM &&
            !m_abstraction->HasEesmBeta(triggerUser->GetUlMcs()))
        {
            Reject("each Trigger MCS requires an explicit EESM beta");
        }
    }
    for (const auto& triggerUser : trigger)
    {
        if (bindingBySta.count(triggerUser.GetAid12()) == 0)
        {
            Reject("a Trigger User Info field has no stable user binding");
        }
    }

    std::map<uint16_t, const HeUlMuMimoPpduReception*> receptionBySta;
    for (const auto& reception : receptions)
    {
        const auto binding = bindingBySta.find(reception.staId);
        if (!receptionBySta.emplace(reception.staId, &reception).second ||
            binding == bindingBySta.end())
        {
            Reject("every received HE-TB PPDU requires one known, unique STA-ID");
        }
        if (!reception.ppdu || reception.ppdu->GetPreamble() != WIFI_PREAMBLE_HE_TB ||
            reception.ppdu->GetUid() != triggerUid ||
            reception.ppdu->GetStaId() != reception.staId)
        {
            Reject("a reception is not an HE-TB PPDU solicited by this Trigger frame");
        }
        if (reception.txNodeId != binding->second->txNodeId ||
            reception.packetSizeBytes == 0 ||
            !std::isfinite(reception.receivedPowerW) || reception.receivedPowerW <= 0.0 ||
            reception.rxEnd <= reception.rxStart)
        {
            Reject("HE-TB transmitter, packet, power, or receive interval is invalid");
        }
        const auto triggerVector = trigger.GetHeTbTxVector(reception.staId);
        if (reception.ppdu->GetTxDuration() != coordinatedEnd - coordinatedStart ||
            triggerVector.GetPreambleType() != WIFI_PREAMBLE_HE_TB ||
            triggerVector.GetNss(reception.staId) != 1)
        {
            Reject("HE-TB duration or NSS is inconsistent with the coordinated exchange");
        }
    }

    HeUlMuMimoPhyResult result;
    std::map<HeRu::RuSpec,
             std::vector<const HeUlMuMimoPpduReception*>,
             HeRu::RuSpecCompare>
        simultaneousByRu{HeRu::RuSpecCompare(trigger.GetUlBandwidth(), apPrimary20Index)};
    for (const auto& [staId, binding] : bindingBySta)
    {
        static_cast<void>(binding);
        const auto reception = receptionBySta.find(staId);
        if (reception == receptionBySta.end())
        {
            result.m_missingStaIds.push_back(staId);
            continue;
        }
        if (reception->second->rxStart != coordinatedStart ||
            reception->second->rxEnd != coordinatedEnd)
        {
            result.m_nonSimultaneousStaIds.push_back(staId);
            continue;
        }
        const auto triggerUser = trigger.FindUserInfoWithAid(staId);
        simultaneousByRu[triggerUser->GetRuAllocation()].push_back(reception->second);
    }

    for (const auto& [ru, group] : simultaneousByRu)
    {
        std::set<uint32_t> userIds;
        std::vector<HeUlMuMimoUserTransmission> transmissions;
        transmissions.reserve(group.size());
        for (const auto* reception : group)
        {
            const auto& binding = *bindingBySta.at(reception->staId);
            userIds.insert(binding.userId);
            const double bandwidthHz =
                static_cast<double>(HeRu::GetBandwidth(ru.GetRuType())) * 1e6;
            const auto linkBudget = WifiLinkBudget::Calculate(WToDbm(reception->receivedPowerW),
                                                              0.0,
                                                              bandwidthHz,
                                                              WIFI_NOISE_TEMPERATURE_K,
                                                              receiverNoiseFigureDb);
            auto selected = reception->subcarrierIndices;
            if (GetEffectiveSinrModel() == SionnaMimoEffectiveSinrModel::MEAN_UNCODED_BER)
            {
                selected = HeDataToneMask::Select(trigger.GetUlBandwidth(), ru.GetRuType(),
                    ru.GetPhyIndex(trigger.GetUlBandwidth(), apPrimary20Index),
                    snapshotByUser.at(binding.userId)->channel.GetNumSubcarriers(), selected);
            }
            transmissions.push_back({binding.userId,
                                     linkBudget,
                                     trigger.GetHeTbTxVector(reception->staId),
                                     reception->packetSizeBytes,
                                     0,
                                     reception->staId,
                                     selected});
        }

        HeUlMuMimoPhyGroupResult groupResult;
        groupResult.m_ru = ru;
        groupResult.m_linkResult = m_abstraction->Calculate(SubsetSnapshot(snapshot, userIds),
                                                            detector,
                                                            transmissions,
                                                            rankTolerance);
        for (uint32_t index = 0; index < groupResult.m_linkResult.GetNumUsers(); ++index)
        {
            const auto& user = groupResult.m_linkResult.GetUser(index);
            const double diversityGain = GetErrorRateModel()->IsAwgn() && apRxAntennas > 1
                                             ? static_cast<double>(apRxAntennas)
                                             : 1.0;
            const double equivalentPower = user.GetEffectiveSinrLinear() *
                                           user.GetLinkBudget().GetReceiverNoisePowerW() /
                                           diversityGain;
            if (!std::isfinite(equivalentPower) || equivalentPower < 0.0)
            {
                Reject("uplink abstraction produced invalid equivalent receive power");
            }
            result.m_equivalentPowers.push_back({user.GetUserId(), equivalentPower});
        }
        result.m_groups.push_back(std::move(groupResult));
    }

    std::sort(result.m_equivalentPowers.begin(),
              result.m_equivalentPowers.end(),
              [](const auto& left, const auto& right) { return left.userId < right.userId; });
    return result;
}

} // namespace ns3
