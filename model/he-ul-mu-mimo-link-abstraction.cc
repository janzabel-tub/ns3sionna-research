/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "he-ul-mu-mimo-link-abstraction.h"

#include "ns3/error-rate-model.h"

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

[[noreturn]] void
Reject(const std::string& reason)
{
    throw std::invalid_argument("HeUlMuMimoLinkAbstraction: " + reason);
}

WifiMode
GetUserMode(const HeUlMuMimoUserTransmission& user)
{
    return user.txVector.IsMu() ? user.txVector.GetMode(user.staId) : user.txVector.GetMode();
}

uint8_t
GetUserNss(const HeUlMuMimoUserTransmission& user)
{
    return user.txVector.IsMu() ? user.txVector.GetNss(user.staId) : user.txVector.GetNss();
}

} // namespace

NS_OBJECT_ENSURE_REGISTERED(HeUlMuMimoLinkAbstraction);

TypeId
HeUlMuMimoLinkAbstraction::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HeUlMuMimoLinkAbstraction")
                            .SetParent<Object>()
                            .SetGroupName("Sionna")
                            .AddConstructor<HeUlMuMimoLinkAbstraction>();
    return tid;
}

HeUlMuMimoLinkAbstraction::HeUlMuMimoLinkAbstraction()
    : m_suAbstraction(CreateObject<HeSuMimoLinkAbstraction>())
{
}

uint32_t
HeUlMuMimoUserResult::GetUserId() const
{
    return m_userId;
}

const WifiLinkBudgetResult&
HeUlMuMimoUserResult::GetLinkBudget() const
{
    return m_linkBudget;
}

const UlMuMimoUserResult&
HeUlMuMimoUserResult::GetMimoResult() const
{
    return m_mimoResult;
}

double
HeUlMuMimoUserResult::GetEffectiveSinrLinear() const
{
    return m_effectiveSinrLinear;
}

double
HeUlMuMimoUserResult::GetPacketSuccessRate() const
{
    return m_packetSuccessRate;
}

double
HeUlMuMimoUserResult::GetPacketErrorRate() const
{
    return 1.0 - m_packetSuccessRate;
}

uint32_t
HeUlMuMimoLinkResult::GetNumUsers() const
{
    return static_cast<uint32_t>(m_users.size());
}

const HeUlMuMimoUserResult&
HeUlMuMimoLinkResult::GetUser(uint32_t index) const
{
    return m_users.at(index);
}

const HeUlMuMimoUserResult&
HeUlMuMimoLinkResult::GetUserById(uint32_t userId) const
{
    const auto found = std::lower_bound(
        m_users.begin(),
        m_users.end(),
        userId,
        [](const HeUlMuMimoUserResult& user, uint32_t id) { return user.GetUserId() < id; });
    if (found == m_users.end() || found->GetUserId() != userId)
    {
        Reject("result user ID is absent");
    }
    return *found;
}

void
HeUlMuMimoLinkAbstraction::SetErrorRateModel(Ptr<ErrorRateModel> errorRateModel)
{
    m_suAbstraction->SetErrorRateModel(errorRateModel);
}

Ptr<ErrorRateModel>
HeUlMuMimoLinkAbstraction::GetErrorRateModel() const
{
    return m_suAbstraction->GetErrorRateModel();
}

void
HeUlMuMimoLinkAbstraction::SetEesmBeta(uint8_t mcs, double betaLinear)
{
    m_suAbstraction->SetEesmBeta(mcs, betaLinear);
}

void
HeUlMuMimoLinkAbstraction::SetEesmBetaDb(uint8_t mcs, double betaDb)
{
    m_suAbstraction->SetEesmBetaDb(mcs, betaDb);
}

bool
HeUlMuMimoLinkAbstraction::HasEesmBeta(uint8_t mcs) const
{
    return m_suAbstraction->HasEesmBeta(mcs);
}

double
HeUlMuMimoLinkAbstraction::GetEesmBeta(uint8_t mcs) const
{
    return m_suAbstraction->GetEesmBeta(mcs);
}

HeUlMuMimoLinkResult
HeUlMuMimoLinkAbstraction::Calculate(
    const MultiUserChannelSnapshot& snapshot,
    UlMuMimoDetector detector,
    const std::vector<HeUlMuMimoUserTransmission>& users,
    double rankTolerance) const
{
    std::map<uint32_t, const HeUlMuMimoUserTransmission*> inputs;
    std::vector<UlMuMimoStreamMapping> mappings;
    std::vector<UlMuMimoUserSnr> snrs;
    mappings.reserve(users.size());
    snrs.reserve(users.size());
    const uint32_t numSubcarriers = snapshot.users.empty()
                                        ? 0
                                        : snapshot.users.front().channel.GetNumSubcarriers();
    for (const auto& user : users)
    {
        if (!inputs.emplace(user.userId, &user).second)
        {
            Reject("a user's HE inputs occur more than once");
        }
        if (!user.txVector.GetModeInitialized())
        {
            Reject("every WifiTxVector mode must be initialized");
        }
        if (user.txVector.IsMu() && !user.txVector.IsAllocated(user.staId))
        {
            Reject("every MU STA-ID must occur in WifiTxVector");
        }
        const WifiMode mode = GetUserMode(user);
        if (mode.GetModulationClass() != WIFI_MOD_CLASS_HE)
        {
            Reject("every user requires an 802.11ax HE mode");
        }
        if (GetUserNss(user) != 1 || user.userStream != 0)
        {
            Reject("Phase 4.7 requires NSS one and user-local stream zero");
        }
        if (user.packetSizeBytes == 0)
        {
            Reject("every packet size must be positive");
        }
        if (!std::isfinite(user.linkBudget.GetReferenceSnrLinear()) ||
            user.linkBudget.GetReferenceSnrLinear() <= 0.0)
        {
            Reject("every link budget must contain a finite positive reference SNR");
        }
        if (!m_suAbstraction->HasEesmBeta(mode.GetMcsValue()))
        {
            Reject("every user's HE MCS requires an explicit EESM beta");
        }
        std::set<uint32_t> selectedSubcarriers;
        for (const uint32_t subcarrier : user.subcarrierIndices)
        {
            if (subcarrier >= numSubcarriers)
            {
                Reject("every selected subcarrier index must be in the CFR grid");
            }
            if (!selectedSubcarriers.insert(subcarrier).second)
            {
                Reject("selected subcarrier indices must be unique per user");
            }
        }
        mappings.push_back({user.userId, user.userStream});
        snrs.push_back({user.userId, user.linkBudget.GetReferenceSnrLinear()});
    }

    const auto mimo =
        UlMuMimoLinkModel::Calculate(snapshot, mappings, snrs, detector, rankTolerance);
    if (inputs.size() != snapshot.users.size())
    {
        Reject("exactly one HE input is required for every snapshot user");
    }

    HeUlMuMimoLinkResult result;
    result.m_users.reserve(snapshot.users.size());
    for (const auto& snapshotUser : snapshot.users)
    {
        const auto input = inputs.find(snapshotUser.userId);
        if (input == inputs.end())
        {
            Reject("a snapshot user has no HE inputs");
        }
        if (snapshotUser.channel.GetNumRxAntennas() > std::numeric_limits<uint8_t>::max())
        {
            Reject("ErrorRateModel cannot represent more than 255 receive antennas");
        }
        const auto& user = *input->second;
        const auto& mimoUser = mimo.GetUserById(snapshotUser.userId);
        std::vector<double> sinrs;
        if (user.subcarrierIndices.empty())
        {
            sinrs.reserve(mimoUser.GetNumSubcarriers());
            for (uint32_t sc = 0; sc < mimoUser.GetNumSubcarriers(); ++sc)
            {
                sinrs.push_back(mimoUser.GetSinrLinear(sc, 0));
            }
        }
        else
        {
            sinrs.reserve(user.subcarrierIndices.size());
            for (const uint32_t sc : user.subcarrierIndices)
            {
                sinrs.push_back(mimoUser.GetSinrLinear(sc, 0));
            }
        }

        HeUlMuMimoUserResult userResult;
        userResult.m_userId = snapshotUser.userId;
        userResult.m_linkBudget = user.linkBudget;
        userResult.m_mimoResult = mimoUser;
        const WifiMode mode = GetUserMode(user);
        userResult.m_effectiveSinrLinear =
            m_suAbstraction->CalculateEffectiveSinr(sinrs, mode.GetMcsValue());
        userResult.m_packetSuccessRate = GetErrorRateModel()->GetChunkSuccessRate(
            mode,
            user.txVector,
            userResult.m_effectiveSinrLinear,
            static_cast<uint64_t>(user.packetSizeBytes) * 8,
            static_cast<uint8_t>(snapshotUser.channel.GetNumRxAntennas()),
            WIFI_PPDU_FIELD_DATA,
            user.staId);
        if (!std::isfinite(userResult.m_packetSuccessRate) ||
            userResult.m_packetSuccessRate < 0.0 || userResult.m_packetSuccessRate > 1.0)
        {
            Reject("ErrorRateModel returned an invalid packet success probability");
        }
        result.m_users.push_back(std::move(userResult));
    }
    return result;
}

} // namespace ns3
