/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "he-dl-mu-mimo-link-abstraction.h"

#include "ns3/error-rate-model.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
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
    throw std::invalid_argument("HeDlMuMimoLinkAbstraction: " + reason);
}

} // namespace

NS_OBJECT_ENSURE_REGISTERED(HeDlMuMimoLinkAbstraction);

TypeId
HeDlMuMimoLinkAbstraction::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HeDlMuMimoLinkAbstraction")
                            .SetParent<Object>()
                            .SetGroupName("Sionna")
                            .AddConstructor<HeDlMuMimoLinkAbstraction>();
    return tid;
}

HeDlMuMimoLinkAbstraction::HeDlMuMimoLinkAbstraction()
    : m_suAbstraction(CreateObject<HeSuMimoLinkAbstraction>())
{
}

uint32_t
HeDlMuMimoUserResult::GetUserId() const
{
    return m_userId;
}

const WifiLinkBudgetResult&
HeDlMuMimoUserResult::GetLinkBudget() const
{
    return m_linkBudget;
}

const DlMuMimoUserResult&
HeDlMuMimoUserResult::GetMimoResult() const
{
    return m_mimoResult;
}

double
HeDlMuMimoUserResult::GetEffectiveSinrLinear() const
{
    return m_effectiveSinrLinear;
}

double
HeDlMuMimoUserResult::GetPacketSuccessRate() const
{
    return m_packetSuccessRate;
}

double
HeDlMuMimoUserResult::GetPacketErrorRate() const
{
    return 1.0 - m_packetSuccessRate;
}

uint32_t
HeDlMuMimoLinkResult::GetNumUsers() const
{
    return static_cast<uint32_t>(m_users.size());
}

const HeDlMuMimoUserResult&
HeDlMuMimoLinkResult::GetUser(uint32_t index) const
{
    return m_users.at(index);
}

const HeDlMuMimoUserResult&
HeDlMuMimoLinkResult::GetUserById(uint32_t userId) const
{
    const auto found = std::lower_bound(
        m_users.begin(),
        m_users.end(),
        userId,
        [](const HeDlMuMimoUserResult& user, uint32_t id) { return user.GetUserId() < id; });
    if (found == m_users.end() || found->GetUserId() != userId)
    {
        Reject("result user ID is absent");
    }
    return *found;
}

void
HeDlMuMimoLinkAbstraction::SetErrorRateModel(Ptr<ErrorRateModel> errorRateModel)
{
    m_suAbstraction->SetErrorRateModel(errorRateModel);
}

Ptr<ErrorRateModel>
HeDlMuMimoLinkAbstraction::GetErrorRateModel() const
{
    return m_suAbstraction->GetErrorRateModel();
}

void
HeDlMuMimoLinkAbstraction::SetEesmBeta(uint8_t mcs, double betaLinear)
{
    m_suAbstraction->SetEesmBeta(mcs, betaLinear);
}

void
HeDlMuMimoLinkAbstraction::SetEesmBetaDb(uint8_t mcs, double betaDb)
{
    m_suAbstraction->SetEesmBetaDb(mcs, betaDb);
}

bool
HeDlMuMimoLinkAbstraction::HasEesmBeta(uint8_t mcs) const
{
    return m_suAbstraction->HasEesmBeta(mcs);
}

double
HeDlMuMimoLinkAbstraction::GetEesmBeta(uint8_t mcs) const
{
    return m_suAbstraction->GetEesmBeta(mcs);
}

HeDlMuMimoLinkResult
HeDlMuMimoLinkAbstraction::Calculate(
    const MultiUserChannelSnapshot& snapshot,
    const DlMuMimoPrecodingMatrix& precoding,
    const std::vector<HeDlMuMimoUserTransmission>& users) const
{
    precoding.ValidateForSnapshot(snapshot);

    std::map<uint32_t, const HeDlMuMimoUserTransmission*> inputs;
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
        const WifiMode mode = user.txVector.GetMode();
        if (mode.GetModulationClass() != WIFI_MOD_CLASS_HE)
        {
            Reject("every user requires an 802.11ax HE mode");
        }
        if (user.txVector.GetNss() != precoding.GetNumStreamsForUser(user.userId))
        {
            Reject("WifiTxVector NSS must match the user's mapped stream count");
        }
        if (user.packetSizeBytes == 0)
        {
            Reject("every packet size must be positive");
        }
        if (!std::isfinite(user.linkBudget.GetReferenceSnrLinear()) ||
            user.linkBudget.GetReferenceSnrLinear() <= 0.0)
        {
            Reject("every user link budget must contain a finite positive reference SNR");
        }
        if (!m_suAbstraction->HasEesmBeta(mode.GetMcsValue()))
        {
            Reject("every user's HE MCS requires an explicit EESM beta");
        }
    }
    if (inputs.size() != snapshot.users.size())
    {
        Reject("exactly one HE input is required for every snapshot user");
    }

    std::vector<DlMuMimoUserSnr> snrs;
    snrs.reserve(snapshot.users.size());
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
        snrs.push_back({snapshotUser.userId,
                        input->second->linkBudget.GetReferenceSnrLinear()});
    }

    const auto mimo = DlMuMimoLinkModel::Calculate(snapshot, precoding, snrs);
    HeDlMuMimoLinkResult result;
    result.m_users.reserve(snapshot.users.size());
    for (const auto& snapshotUser : snapshot.users)
    {
        const auto& input = *inputs.at(snapshotUser.userId);
        const auto& mimoUser = mimo.GetUserById(snapshotUser.userId);
        std::vector<double> sinrs;
        sinrs.reserve(static_cast<std::size_t>(mimoUser.GetNumSubcarriers()) *
                      mimoUser.GetNumStreams());
        for (uint32_t sc = 0; sc < mimoUser.GetNumSubcarriers(); ++sc)
        {
            for (uint32_t stream = 0; stream < mimoUser.GetNumStreams(); ++stream)
            {
                sinrs.push_back(mimoUser.GetSinrLinear(sc, stream));
            }
        }

        HeDlMuMimoUserResult userResult;
        userResult.m_userId = snapshotUser.userId;
        userResult.m_linkBudget = input.linkBudget;
        userResult.m_mimoResult = mimoUser;
        userResult.m_effectiveSinrLinear =
            m_suAbstraction->CalculateEffectiveSinr(sinrs,
                                                    input.txVector.GetMode().GetMcsValue());
        userResult.m_packetSuccessRate =
            GetErrorRateModel()->GetChunkSuccessRate(
                input.txVector.GetMode(),
                input.txVector,
                userResult.m_effectiveSinrLinear,
                static_cast<uint64_t>(input.packetSizeBytes) * 8,
                static_cast<uint8_t>(snapshotUser.channel.GetNumRxAntennas()));
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
