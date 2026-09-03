/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "dl-mu-mimo-runtime.h"

#include "sionna-spectrum-propagation-loss-model.h"

#include <algorithm>
#include <cmath>
#include <complex>
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
    throw std::invalid_argument("DlMuMimoRuntime: " + reason);
}

template <typename Function>
decltype(auto)
TranslateInvalidArgument(Function&& operation)
{
    try
    {
        return operation();
    }
    catch (const std::invalid_argument& error)
    {
        Reject(error.what());
    }
}

bool
UserLess(const DlMuMimoRuntimeUser& lhs, const DlMuMimoRuntimeUser& rhs)
{
    if (lhs.userId != rhs.userId)
    {
        return lhs.userId < rhs.userId;
    }
    if (lhs.staId != rhs.staId)
    {
        return lhs.staId < rhs.staId;
    }
    if (lhs.ru != rhs.ru)
    {
        return lhs.ru < rhs.ru;
    }
    return lhs.nss < rhs.nss;
}

void
ValidateConfiguration(const DlMuMimoRuntimeConfiguration& configuration)
{
    switch (configuration.groupingMode)
    {
    case DlMuMimoGroupingMode::FIXED:
        if (configuration.requestedUserIds.size() < 2)
        {
            Reject("a fixed MU group must contain at least two users");
        }
        if (configuration.requestedGroupSize != 0)
        {
            Reject("fixed grouping does not accept a requested group size");
        }
        break;
    case DlMuMimoGroupingMode::MINIMUM_CORRELATION:
        if (!configuration.requestedUserIds.empty())
        {
            Reject("correlation grouping does not accept fixed user IDs");
        }
        if (configuration.requestedGroupSize < 2)
        {
            Reject("a correlation MU group must contain at least two users");
        }
        break;
    default:
        Reject("unknown grouping mode");
    }

    std::set<uint32_t> requestedIds(configuration.requestedUserIds.begin(),
                                    configuration.requestedUserIds.end());
    if (requestedIds.size() != configuration.requestedUserIds.size())
    {
        Reject("requested user IDs must be unique");
    }

    switch (configuration.precoder)
    {
    case DlMuMimoRuntimePrecoder::ZERO_FORCING:
        if (configuration.rzfAlpha != 0.0)
        {
            Reject("ZF does not accept an RZF regularization parameter");
        }
        break;
    case DlMuMimoRuntimePrecoder::REGULARIZED_ZERO_FORCING:
        if (!std::isfinite(configuration.rzfAlpha) || configuration.rzfAlpha <= 0.0)
        {
            Reject("RZF alpha must be finite and positive");
        }
        break;
    default:
        Reject("unknown runtime precoder");
    }

    switch (configuration.feedback)
    {
    case DlMuMimoFeedbackRepresentation::PERFECT_CHANNEL:
        break;
    case DlMuMimoFeedbackRepresentation::QUANTIZED_CHANNEL:
        Reject("quantized MU channel feedback is not supported in Phase 4.5");
    case DlMuMimoFeedbackRepresentation::SU_PRECODER_RECONSTRUCTION:
        Reject("an SU precoder reconstruction is not a complete MU channel estimate");
    default:
        Reject("unknown MU feedback representation");
    }

    if (!std::isfinite(configuration.rankTolerance) ||
        configuration.rankTolerance <= 0.0 || configuration.rankTolerance >= 1.0)
    {
        Reject("rank tolerance must be finite and in (0, 1)");
    }
    if (configuration.timing.updateInterval < NanoSeconds(0) ||
        configuration.timing.feedbackDelay < NanoSeconds(0))
    {
        Reject("CSI update interval and feedback delay must be non-negative");
    }
}

std::vector<DlMuMimoRuntimeUser>
GetSchedulerUsers(const MultiUserScheduler::DlMuInfo& schedulerInfo,
                  const std::vector<DlMuMimoStaUserBinding>& bindings)
{
    const auto& txVector = schedulerInfo.txParams.m_txVector;
    if (txVector.GetPreambleType() != WIFI_PREAMBLE_HE_MU || !txVector.IsDlMu())
    {
        Reject("only HE downlink MU scheduler assignments are supported");
    }

    const auto& userInfoMap = txVector.GetHeMuUserInfoMap();
    if (userInfoMap.size() < 2 || userInfoMap.size() != schedulerInfo.psduMap.size() ||
        userInfoMap.size() != bindings.size())
    {
        Reject("PSDU, TxVector, and binding sets must contain the same MU users");
    }

    std::set<uint16_t> staIds;
    std::set<uint32_t> userIds;
    std::vector<DlMuMimoRuntimeUser> users;
    users.reserve(bindings.size());
    for (const auto& binding : bindings)
    {
        if (!staIds.insert(binding.staId).second || !userIds.insert(binding.userId).second)
        {
            Reject("STA-ID and user-ID bindings must be unique");
        }
        const auto txUser = userInfoMap.find(binding.staId);
        const auto psdu = schedulerInfo.psduMap.find(binding.staId);
        if (txUser == userInfoMap.end() || psdu == schedulerInfo.psduMap.end() || !psdu->second)
        {
            Reject("bindings must match non-null scheduler TxVector and PSDU entries");
        }
        if (txUser->second.nss != 1)
        {
            Reject("Phase 4.5 ZF/RZF supports exactly one stream per user");
        }
        users.push_back(
            {binding.userId, binding.staId, txUser->second.ru, txUser->second.nss});
    }
    std::sort(users.begin(), users.end(), UserLess);
    return users;
}

void
ValidateSnapshot(const MultiUserChannelSnapshot& snapshot,
                 uint32_t apNodeId,
                 const std::vector<DlMuMimoRuntimeUser>& schedulerUsers,
                 Time expectedCsiTimestamp)
{
    if (snapshot.direction != MultiUserChannelDirection::DOWNLINK)
    {
        Reject("the runtime snapshot direction must be downlink");
    }
    if (snapshot.csiTimestamp != expectedCsiTimestamp)
    {
        Reject("the snapshot CSI timestamp does not match the Phase-3 timing contract");
    }
    if (snapshot.users.size() != schedulerUsers.size() || snapshot.users.empty())
    {
        Reject("snapshot and scheduler candidate sets must contain the same users");
    }

    const auto& first = snapshot.users.front();
    const uint32_t numSubcarriers = first.channel.GetNumSubcarriers();
    const uint32_t numTxAntennas = first.channel.GetNumTxAntennas();
    const auto frequencies = first.frequencies;
    if (numSubcarriers == 0 || numTxAntennas == 0 ||
        frequencies.size() != numSubcarriers)
    {
        Reject("snapshot channel dimensions or frequency grid are invalid");
    }

    uint32_t previousUserId = 0;
    bool isFirst = true;
    for (std::size_t index = 0; index < snapshot.users.size(); ++index)
    {
        const auto& user = snapshot.users[index];
        if ((!isFirst && user.userId <= previousUserId) ||
            user.userId != schedulerUsers[index].userId)
        {
            Reject("snapshot users must match the stable scheduler user order");
        }
        isFirst = false;
        previousUserId = user.userId;

        if (user.txNodeId != apNodeId || user.direction != snapshot.direction ||
            user.csiTimestamp != snapshot.csiTimestamp)
        {
            Reject("snapshot endpoints, direction, or per-user timestamp are inconsistent");
        }
        if (user.channel.GetNumSubcarriers() != numSubcarriers ||
            user.channel.GetNumTxAntennas() != numTxAntennas ||
            user.channel.GetNumRxAntennas() != 1 || user.frequencies != frequencies)
        {
            Reject("snapshot frequency, TX, or Phase-4.3 RX dimensions are inconsistent");
        }
        if (!std::isfinite(user.widebandLossDb))
        {
            Reject("snapshot wideband loss must be finite");
        }
        for (const auto coefficient : user.channel.GetCoefficients())
        {
            if (!std::isfinite(coefficient.real()) || !std::isfinite(coefficient.imag()))
            {
                Reject("snapshot CFR coefficients must be finite");
            }
        }
    }
}

MultiUserChannelSnapshot
MakeSnapshotSubset(const MultiUserChannelSnapshot& snapshot,
                   const std::vector<DlMuMimoRuntimeUser>& users)
{
    MultiUserChannelSnapshot subset{snapshot.csiTimestamp, snapshot.direction, {}};
    subset.users.reserve(users.size());
    for (const auto& selected : users)
    {
        const auto user =
            std::lower_bound(snapshot.users.begin(),
                             snapshot.users.end(),
                             selected.userId,
                             [](const MultiUserChannel& candidate, uint32_t userId) {
                                 return candidate.userId < userId;
                             });
        if (user == snapshot.users.end() || user->userId != selected.userId)
        {
            Reject("a selected user is absent from the channel snapshot");
        }
        subset.users.push_back(*user);
    }
    return subset;
}

DlMuMimoPrecodingMatrix
SelectPrecoder(const MultiUserChannelSnapshot& snapshot,
               const DlMuMimoRuntimeConfiguration& configuration)
{
    std::vector<DlMuMimoStreamMapping> mappings;
    mappings.reserve(snapshot.users.size());
    for (const auto& user : snapshot.users)
    {
        mappings.push_back({user.userId, 0});
    }
    return TranslateInvalidArgument([&]() {
        if (configuration.precoder == DlMuMimoRuntimePrecoder::ZERO_FORCING)
        {
            return DlMuMimoPrecodingMatrix::ZeroForcing(snapshot,
                                                        mappings,
                                                        configuration.rankTolerance);
        }
        return DlMuMimoPrecodingMatrix::RegularizedZeroForcing(
            snapshot,
            mappings,
            configuration.rzfAlpha,
            configuration.rankTolerance);
    });
}

double
CalculateCorrelationMetric(const MultiUserChannelSnapshot& snapshot)
{
    double maximumCorrelation = 0.0;
    for (std::size_t lhsIndex = 0; lhsIndex < snapshot.users.size(); ++lhsIndex)
    {
        for (std::size_t rhsIndex = lhsIndex + 1; rhsIndex < snapshot.users.size(); ++rhsIndex)
        {
            const auto& lhs = snapshot.users[lhsIndex].channel;
            const auto& rhs = snapshot.users[rhsIndex].channel;
            std::complex<double> innerProduct{0.0, 0.0};
            double lhsEnergy = 0.0;
            double rhsEnergy = 0.0;
            for (uint32_t subcarrier = 0; subcarrier < lhs.GetNumSubcarriers(); ++subcarrier)
            {
                for (uint32_t tx = 0; tx < lhs.GetNumTxAntennas(); ++tx)
                {
                    const auto lhsValue = lhs.Get(subcarrier, 0, tx);
                    const auto rhsValue = rhs.Get(subcarrier, 0, tx);
                    innerProduct += lhsValue * std::conj(rhsValue);
                    lhsEnergy += std::norm(lhsValue);
                    rhsEnergy += std::norm(rhsValue);
                }
            }
            const double correlation =
                std::abs(innerProduct) / std::sqrt(lhsEnergy * rhsEnergy);
            if (!std::isfinite(correlation) || correlation < 0.0 ||
                correlation > 1.0 + 1e-12)
            {
                Reject("channel correlation metric is not finite in [0, 1]");
            }
            maximumCorrelation =
                std::max(maximumCorrelation, std::min(1.0, correlation));
        }
    }
    return maximumCorrelation;
}

bool
AdvanceCombination(std::vector<std::size_t>& indices, std::size_t candidateCount)
{
    for (std::size_t reverse = indices.size(); reverse > 0; --reverse)
    {
        const std::size_t index = reverse - 1;
        const std::size_t limit = candidateCount - indices.size() + index;
        if (indices[index] != limit)
        {
            ++indices[index];
            for (std::size_t following = index + 1; following < indices.size(); ++following)
            {
                indices[following] = indices[following - 1] + 1;
            }
            return true;
        }
    }
    return false;
}

bool
SameReceiveArray(const MultiUserReceiveArrayIdentity& lhs,
                 const MultiUserReceiveArrayIdentity& rhs)
{
    return lhs.numRows == rhs.numRows && lhs.numColumns == rhs.numColumns &&
           lhs.verticalSpacing == rhs.verticalSpacing &&
           lhs.horizontalSpacing == rhs.horizontalSpacing && lhs.pattern == rhs.pattern &&
           lhs.polarization == rhs.polarization;
}

bool
SameSnapshot(const MultiUserChannelSnapshot& lhs, const MultiUserChannelSnapshot& rhs)
{
    if (lhs.csiTimestamp != rhs.csiTimestamp || lhs.direction != rhs.direction ||
        lhs.users.size() != rhs.users.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < lhs.users.size(); ++index)
    {
        const auto& lhsUser = lhs.users[index];
        const auto& rhsUser = rhs.users[index];
        if (lhsUser.userId != rhsUser.userId || lhsUser.txNodeId != rhsUser.txNodeId ||
            lhsUser.rxNodeId != rhsUser.rxNodeId || lhsUser.direction != rhsUser.direction ||
            lhsUser.csiTimestamp != rhsUser.csiTimestamp ||
            !SameReceiveArray(lhsUser.rxArray, rhsUser.rxArray) ||
            lhsUser.frequencies != rhsUser.frequencies ||
            lhsUser.widebandLossDb != rhsUser.widebandLossDb ||
            lhsUser.channel.GetNumSubcarriers() != rhsUser.channel.GetNumSubcarriers() ||
            lhsUser.channel.GetNumRxAntennas() != rhsUser.channel.GetNumRxAntennas() ||
            lhsUser.channel.GetNumTxAntennas() != rhsUser.channel.GetNumTxAntennas() ||
            lhsUser.channel.GetCoefficients() != rhsUser.channel.GetCoefficients())
        {
            return false;
        }
    }
    return true;
}

} // namespace

DlMuMimoRuntimeSelection::DlMuMimoRuntimeSelection(
    uint32_t apNodeId,
    std::vector<DlMuMimoRuntimeUser> users,
    MultiUserChannelSnapshot snapshot,
    DlMuMimoPrecodingMatrix precoding,
    Time transmissionTimestamp,
    Time updateInterval,
    Time feedbackDelay,
    double groupingMetric)
    : m_apNodeId(apNodeId),
      m_direction(snapshot.direction),
      m_users(std::move(users)),
      m_snapshot(std::move(snapshot)),
      m_precoding(std::move(precoding)),
      m_transmissionTimestamp(transmissionTimestamp),
      m_updateInterval(updateInterval),
      m_feedbackDelay(feedbackDelay),
      m_groupingMetric(groupingMetric)
{
}

uint32_t
DlMuMimoRuntimeSelection::GetApNodeId() const
{
    return m_apNodeId;
}

MultiUserChannelDirection
DlMuMimoRuntimeSelection::GetDirection() const
{
    return m_direction;
}

const std::vector<DlMuMimoRuntimeUser>&
DlMuMimoRuntimeSelection::GetUsers() const
{
    return m_users;
}

const MultiUserChannelSnapshot&
DlMuMimoRuntimeSelection::GetSnapshot() const
{
    return m_snapshot;
}

const DlMuMimoPrecodingMatrix&
DlMuMimoRuntimeSelection::GetPrecoding() const
{
    return m_precoding;
}

Time
DlMuMimoRuntimeSelection::GetCsiTimestamp() const
{
    return m_snapshot.csiTimestamp;
}

Time
DlMuMimoRuntimeSelection::GetTransmissionTimestamp() const
{
    return m_transmissionTimestamp;
}

Time
DlMuMimoRuntimeSelection::GetCsiAge() const
{
    return m_transmissionTimestamp - m_snapshot.csiTimestamp;
}

Time
DlMuMimoRuntimeSelection::GetUpdateInterval() const
{
    return m_updateInterval;
}

Time
DlMuMimoRuntimeSelection::GetFeedbackDelay() const
{
    return m_feedbackDelay;
}

double
DlMuMimoRuntimeSelection::GetGroupingMetric() const
{
    return m_groupingMetric;
}

bool
DlMuMimoRuntime::Key::operator<(const Key& other) const
{
    if (apNodeId != other.apNodeId)
    {
        return apNodeId < other.apNodeId;
    }
    if (direction != other.direction)
    {
        return direction < other.direction;
    }
    return std::lexicographical_compare(users.begin(),
                                        users.end(),
                                        other.users.begin(),
                                        other.users.end(),
                                        UserLess);
}

DlMuMimoRuntime::DlMuMimoRuntime(DlMuMimoRuntimeConfiguration configuration)
    : m_configuration(std::move(configuration))
{
    ValidateConfiguration(m_configuration);
}

DlMuMimoRuntimeSelection
DlMuMimoRuntime::SelectAndStore(
    uint32_t apNodeId,
    const MultiUserChannelSnapshot& snapshot,
    const MultiUserScheduler::DlMuInfo& schedulerInfo,
    const std::vector<DlMuMimoStaUserBinding>& bindings,
    Time transmissionTimestamp)
{
    ValidateConfiguration(m_configuration);
    const Time expectedCsiTimestamp = TranslateInvalidArgument(
        [&]() { return m_configuration.timing.ResolveCsiTimestamp(transmissionTimestamp); });
    if (snapshot.csiTimestamp > transmissionTimestamp)
    {
        Reject("the snapshot CSI timestamp must not be in the future");
    }

    const auto schedulerUsers = GetSchedulerUsers(schedulerInfo, bindings);
    ValidateSnapshot(snapshot, apNodeId, schedulerUsers, expectedCsiTimestamp);

    std::vector<DlMuMimoRuntimeUser> selectedUsers;
    double groupingMetric = 0.0;
    if (m_configuration.groupingMode == DlMuMimoGroupingMode::FIXED)
    {
        selectedUsers.reserve(m_configuration.requestedUserIds.size());
        for (const uint32_t requestedId : m_configuration.requestedUserIds)
        {
            const auto user = std::lower_bound(
                schedulerUsers.begin(),
                schedulerUsers.end(),
                requestedId,
                [](const DlMuMimoRuntimeUser& candidate, uint32_t userId) {
                    return candidate.userId < userId;
                });
            if (user == schedulerUsers.end() || user->userId != requestedId)
            {
                Reject("a fixed requested user is absent from the scheduler candidates");
            }
            selectedUsers.push_back(*user);
        }
        std::sort(selectedUsers.begin(), selectedUsers.end(), UserLess);
        if (std::any_of(selectedUsers.begin() + 1,
                        selectedUsers.end(),
                        [&](const auto& user) { return user.ru != selectedUsers.front().ru; }))
        {
            Reject("a spatially multiplexed fixed group must share one RU");
        }
    }
    else
    {
        const std::size_t groupSize = m_configuration.requestedGroupSize;
        if (groupSize > schedulerUsers.size())
        {
            Reject("requested correlation group size exceeds scheduler candidates");
        }

        std::vector<std::size_t> indices(groupSize);
        for (std::size_t index = 0; index < groupSize; ++index)
        {
            indices[index] = index;
        }
        bool found = false;
        do
        {
            std::vector<DlMuMimoRuntimeUser> candidateUsers;
            candidateUsers.reserve(groupSize);
            for (const std::size_t index : indices)
            {
                candidateUsers.push_back(schedulerUsers[index]);
            }
            if (std::any_of(candidateUsers.begin() + 1,
                            candidateUsers.end(),
                            [&](const auto& user) {
                                return user.ru != candidateUsers.front().ru;
                            }))
            {
                continue;
            }

            const auto candidateSnapshot = MakeSnapshotSubset(snapshot, candidateUsers);
            try
            {
                static_cast<void>(SelectPrecoder(candidateSnapshot, m_configuration));
            }
            catch (const std::invalid_argument&)
            {
                continue;
            }
            const double candidateMetric = CalculateCorrelationMetric(candidateSnapshot);
            const bool stableTieBreak =
                found && std::abs(candidateMetric - groupingMetric) <= 1e-15 &&
                std::lexicographical_compare(candidateUsers.begin(),
                                             candidateUsers.end(),
                                             selectedUsers.begin(),
                                             selectedUsers.end(),
                                             UserLess);
            if (!found || candidateMetric < groupingMetric - 1e-15 || stableTieBreak)
            {
                selectedUsers = std::move(candidateUsers);
                groupingMetric = candidateMetric;
                found = true;
            }
        } while (AdvanceCombination(indices, schedulerUsers.size()));

        if (!found)
        {
            Reject("no feasible minimum-correlation ZF/RZF group exists");
        }
    }

    const auto selectedSnapshot = MakeSnapshotSubset(snapshot, selectedUsers);
    auto precoding = SelectPrecoder(selectedSnapshot, m_configuration);
    if (m_configuration.groupingMode == DlMuMimoGroupingMode::FIXED)
    {
        groupingMetric = CalculateCorrelationMetric(selectedSnapshot);
    }

    DlMuMimoRuntimeSelection selection(apNodeId,
                                       selectedUsers,
                                       selectedSnapshot,
                                       std::move(precoding),
                                       transmissionTimestamp,
                                       m_configuration.timing.updateInterval,
                                       m_configuration.timing.feedbackDelay,
                                       groupingMetric);
    Key key{apNodeId, MultiUserChannelDirection::DOWNLINK, selectedUsers};
    const auto previous = m_states.find(key);
    if (previous != m_states.end())
    {
        if (selection.GetCsiTimestamp() < previous->second.GetCsiTimestamp())
        {
            Reject("an older CSI snapshot cannot replace newer runtime state");
        }
        if (selection.GetCsiTimestamp() == previous->second.GetCsiTimestamp() &&
            !SameSnapshot(selection.GetSnapshot(), previous->second.GetSnapshot()))
        {
            Reject("the same CSI identity cannot carry different channel state");
        }
    }
    m_states.insert_or_assign(std::move(key), selection);
    return selection;
}

const DlMuMimoRuntimeSelection&
DlMuMimoRuntime::Lookup(
    uint32_t apNodeId,
    const MultiUserScheduler::DlMuInfo& schedulerInfo,
    const std::vector<DlMuMimoStaUserBinding>& bindings,
    Time transmissionTimestamp) const
{
    const auto users = GetSchedulerUsers(schedulerInfo, bindings);
    const Key key{apNodeId, MultiUserChannelDirection::DOWNLINK, users};
    const auto state = m_states.find(key);
    if (state == m_states.end())
    {
        Reject("no runtime state exists for the exact AP, user, RU, and NSS assignment");
    }
    if (state->second.GetTransmissionTimestamp() != transmissionTimestamp)
    {
        Reject("runtime state was selected for a different transmission timestamp");
    }
    const Time expectedCsiTimestamp = TranslateInvalidArgument(
        [&]() { return m_configuration.timing.ResolveCsiTimestamp(transmissionTimestamp); });
    if (state->second.GetCsiTimestamp() != expectedCsiTimestamp)
    {
        Reject("stored runtime CSI is stale for the requested transmission");
    }
    return state->second;
}

void
DlMuMimoRuntime::Install(
    SionnaSpectrumPropagationLossModel& model,
    uint32_t apNodeId,
    const MultiUserScheduler::DlMuInfo& schedulerInfo,
    const std::vector<DlMuMimoStaUserBinding>& bindings,
    Time transmissionTimestamp) const
{
    const auto& state =
        Lookup(apNodeId, schedulerInfo, bindings, transmissionTimestamp);
    model.SetDlMuMimoState(state.GetSnapshot(), state.GetPrecoding());
}

} // namespace ns3
