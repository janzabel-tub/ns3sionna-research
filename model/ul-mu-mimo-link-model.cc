/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ul-mu-mimo-link-model.h"

#include <Eigen/Dense>
#include <Eigen/SVD>

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace ns3
{

namespace
{

[[noreturn]] void
Reject(const std::string& reason)
{
    throw std::invalid_argument("UlMuMimoLinkModel: " + reason);
}

void
ValidateSnapshot(const MultiUserChannelSnapshot& snapshot)
{
    if (snapshot.direction != MultiUserChannelDirection::UPLINK)
    {
        Reject("the snapshot direction must be uplink");
    }
    if (snapshot.users.empty())
    {
        Reject("the snapshot must contain at least one user");
    }

    const auto& first = snapshot.users.front();
    const uint32_t numSubcarriers = first.channel.GetNumSubcarriers();
    const uint32_t numRxAntennas = first.channel.GetNumRxAntennas();
    const uint32_t apNodeId = first.rxNodeId;
    if (first.frequencies.size() != numSubcarriers)
    {
        Reject("the frequency grid must contain one value per subcarrier");
    }

    std::set<uint32_t> userIds;
    std::set<uint32_t> txNodeIds;
    uint32_t previousUserId = 0;
    bool firstUser = true;
    for (const auto& user : snapshot.users)
    {
        if (!userIds.insert(user.userId).second)
        {
            Reject("the snapshot contains a duplicate user ID");
        }
        if (!firstUser && user.userId <= previousUserId)
        {
            Reject("snapshot users must be ordered by stable user ID");
        }
        firstUser = false;
        previousUserId = user.userId;
        if (!txNodeIds.insert(user.txNodeId).second || user.txNodeId == apNodeId)
        {
            Reject("uplink users must have distinct transmitter nodes different from the AP");
        }
        if (user.direction != snapshot.direction || user.csiTimestamp != snapshot.csiTimestamp)
        {
            Reject("user direction or CSI timestamp differs from the snapshot");
        }
        if (user.rxNodeId != apNodeId ||
            user.channel.GetNumSubcarriers() != numSubcarriers ||
            user.channel.GetNumRxAntennas() != numRxAntennas ||
            user.channel.GetNumTxAntennas() != 1 || user.frequencies != first.frequencies)
        {
            Reject("users must share one AP receive array and frequency grid and use one TX antenna");
        }
        if (user.rxArray.numRows == 0 || user.rxArray.numColumns == 0 ||
            static_cast<uint64_t>(user.rxArray.numRows) * user.rxArray.numColumns !=
                numRxAntennas ||
            !std::isfinite(user.rxArray.verticalSpacing) ||
            !std::isfinite(user.rxArray.horizontalSpacing))
        {
            Reject("the AP receive-array geometry must match the CFR dimension");
        }
        if (user.rxArray.numRows != first.rxArray.numRows ||
            user.rxArray.numColumns != first.rxArray.numColumns ||
            user.rxArray.verticalSpacing != first.rxArray.verticalSpacing ||
            user.rxArray.horizontalSpacing != first.rxArray.horizontalSpacing ||
            user.rxArray.pattern != first.rxArray.pattern ||
            user.rxArray.polarization != first.rxArray.polarization)
        {
            Reject("users must identify the same AP receive-array geometry");
        }
        if (!std::isfinite(user.widebandLossDb))
        {
            Reject("every user path loss must be finite");
        }
        for (const auto coefficient : user.channel.GetCoefficients())
        {
            if (!std::isfinite(coefficient.real()) || !std::isfinite(coefficient.imag()))
            {
                Reject("all CFR coefficients must be finite");
            }
        }
    }
}

std::map<uint32_t, double>
ValidateInputs(const MultiUserChannelSnapshot& snapshot,
               const std::vector<UlMuMimoStreamMapping>& streamMappings,
               const std::vector<UlMuMimoUserSnr>& userSnrs,
               double rankTolerance)
{
    ValidateSnapshot(snapshot);
    if (!std::isfinite(rankTolerance) || rankTolerance <= 0.0 || rankTolerance >= 1.0)
    {
        Reject("rank tolerance must be finite and in (0, 1)");
    }

    std::map<uint32_t, uint32_t> streamByUser;
    for (const auto& mapping : streamMappings)
    {
        if (mapping.userStream != 0)
        {
            Reject("Phase 4.7 supports only user-local stream zero");
        }
        if (!streamByUser.emplace(mapping.userId, mapping.userStream).second)
        {
            Reject("exactly one stream mapping is required per user");
        }
    }

    std::map<uint32_t, double> snrByUser;
    for (const auto& userSnr : userSnrs)
    {
        if (!std::isfinite(userSnr.referenceSnrLinear) || userSnr.referenceSnrLinear < 0.0)
        {
            Reject("every reference SNR must be finite and non-negative");
        }
        if (!snrByUser.emplace(userSnr.userId, userSnr.referenceSnrLinear).second)
        {
            Reject("exactly one reference SNR is required per user");
        }
    }
    if (streamByUser.size() != snapshot.users.size() ||
        snrByUser.size() != snapshot.users.size())
    {
        Reject("every snapshot user requires exactly one stream mapping and reference SNR");
    }
    for (const auto& user : snapshot.users)
    {
        if (streamByUser.find(user.userId) == streamByUser.end() ||
            snrByUser.find(user.userId) == snrByUser.end())
        {
            Reject("a snapshot user has no stream mapping or reference SNR");
        }
    }
    for (const auto& mapping : streamByUser)
    {
        if (snrByUser.find(mapping.first) == snrByUser.end())
        {
            Reject("an input refers to a user absent from the snapshot");
        }
    }
    return snrByUser;
}

std::size_t
GetIndex(const UlMuMimoUserResult& result, uint32_t subcarrier, uint32_t stream)
{
    if (subcarrier >= result.GetNumSubcarriers() || stream >= result.GetNumStreams())
    {
        Reject("user-result subcarrier or stream index is out of range");
    }
    return subcarrier;
}

} // namespace

uint32_t
UlMuMimoUserResult::GetUserId() const
{
    return m_userId;
}

uint32_t
UlMuMimoUserResult::GetNumSubcarriers() const
{
    return m_numSubcarriers;
}

uint32_t
UlMuMimoUserResult::GetNumStreams() const
{
    return 1;
}

double
UlMuMimoUserResult::GetDesiredSignalPower(uint32_t subcarrier, uint32_t stream) const
{
    return m_desiredSignalPower.at(GetIndex(*this, subcarrier, stream));
}

double
UlMuMimoUserResult::GetInterUserInterferencePower(uint32_t subcarrier,
                                                  uint32_t stream) const
{
    return m_interUserInterferencePower.at(GetIndex(*this, subcarrier, stream));
}

double
UlMuMimoUserResult::GetNoisePower(uint32_t subcarrier, uint32_t stream) const
{
    return m_noisePower.at(GetIndex(*this, subcarrier, stream));
}

double
UlMuMimoUserResult::GetSinrLinear(uint32_t subcarrier, uint32_t stream) const
{
    return m_sinrLinear.at(GetIndex(*this, subcarrier, stream));
}

uint32_t
UlMuMimoLinkResult::GetNumUsers() const
{
    return static_cast<uint32_t>(m_users.size());
}

const UlMuMimoUserResult&
UlMuMimoLinkResult::GetUser(uint32_t index) const
{
    return m_users.at(index);
}

const UlMuMimoUserResult&
UlMuMimoLinkResult::GetUserById(uint32_t userId) const
{
    const auto found = std::lower_bound(
        m_users.begin(),
        m_users.end(),
        userId,
        [](const UlMuMimoUserResult& user, uint32_t id) { return user.GetUserId() < id; });
    if (found == m_users.end() || found->GetUserId() != userId)
    {
        Reject("result user ID is absent");
    }
    return *found;
}

UlMuMimoLinkResult
UlMuMimoLinkModel::Calculate(const MultiUserChannelSnapshot& snapshot,
                             const std::vector<UlMuMimoStreamMapping>& streamMappings,
                             const std::vector<UlMuMimoUserSnr>& userSnrs,
                             UlMuMimoDetector detector,
                             double rankTolerance)
{
    const auto snrByUser =
        ValidateInputs(snapshot, streamMappings, userSnrs, rankTolerance);
    const uint32_t numUsers = static_cast<uint32_t>(snapshot.users.size());
    const uint32_t numSubcarriers = snapshot.users.front().channel.GetNumSubcarriers();
    const uint32_t numRxAntennas = snapshot.users.front().channel.GetNumRxAntennas();
    if (detector == UlMuMimoDetector::ZERO_FORCING && numUsers > numRxAntennas)
    {
        Reject("zero forcing cannot detect an overloaded user group");
    }

    UlMuMimoLinkResult result;
    result.m_users.reserve(numUsers);
    for (const auto& user : snapshot.users)
    {
        UlMuMimoUserResult userResult;
        userResult.m_userId = user.userId;
        userResult.m_numSubcarriers = numSubcarriers;
        userResult.m_desiredSignalPower.resize(numSubcarriers);
        userResult.m_interUserInterferencePower.resize(numSubcarriers);
        userResult.m_noisePower.resize(numSubcarriers);
        userResult.m_sinrLinear.resize(numSubcarriers);
        result.m_users.push_back(std::move(userResult));
    }

    for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
    {
        Eigen::MatrixXcd channel(numRxAntennas, numUsers);
        for (uint32_t user = 0; user < numUsers; ++user)
        {
            for (uint32_t rx = 0; rx < numRxAntennas; ++rx)
            {
                channel(rx, user) = snapshot.users.at(user).channel.Get(sc, rx, 0);
            }
        }

        Eigen::MatrixXcd combiners(numRxAntennas, numUsers);
        if (detector == UlMuMimoDetector::ZERO_FORCING)
        {
            const Eigen::JacobiSVD<Eigen::MatrixXcd> decomposition(
                channel,
                Eigen::ComputeThinU | Eigen::ComputeThinV);
            if (decomposition.info() != Eigen::Success)
            {
                Reject("zero-forcing SVD decomposition failed");
            }
            const Eigen::VectorXd singularValues = decomposition.singularValues();
            const double threshold = rankTolerance * singularValues(0);
            uint32_t rank = 0;
            for (Eigen::Index mode = 0; mode < singularValues.size(); ++mode)
            {
                if (!std::isfinite(singularValues(mode)) || singularValues(mode) < 0.0)
                {
                    Reject("zero forcing produced an invalid singular value");
                }
                if (singularValues(mode) > threshold)
                {
                    ++rank;
                }
            }
            if (rank < numUsers)
            {
                Reject("zero-forcing user group on subcarrier " + std::to_string(sc) +
                       " is numerically rank deficient");
            }
            combiners = decomposition.matrixU() * singularValues.cwiseInverse().asDiagonal() *
                        decomposition.matrixV().adjoint();
        }
        else if (detector == UlMuMimoDetector::LINEAR_MMSE)
        {
            Eigen::MatrixXcd covariance =
                Eigen::MatrixXcd::Identity(numRxAntennas, numRxAntennas);
            for (uint32_t user = 0; user < numUsers; ++user)
            {
                covariance += snrByUser.at(snapshot.users.at(user).userId) *
                              channel.col(user) * channel.col(user).adjoint();
            }
            const Eigen::LDLT<Eigen::MatrixXcd> decomposition(covariance);
            if (decomposition.info() != Eigen::Success)
            {
                Reject("could not decompose the MMSE covariance matrix");
            }
            combiners = decomposition.solve(channel);
            if (decomposition.info() != Eigen::Success)
            {
                Reject("could not solve the MMSE covariance system");
            }
        }
        else
        {
            Reject("unknown uplink detector");
        }
        if (!combiners.allFinite())
        {
            Reject("the uplink detector produced a non-finite combiner");
        }

        for (uint32_t user = 0; user < numUsers; ++user)
        {
            const Eigen::VectorXcd combiner = combiners.col(user);
            const double desired =
                snrByUser.at(snapshot.users.at(user).userId) *
                std::norm(combiner.dot(channel.col(user)));
            double interference = 0.0;
            for (uint32_t other = 0; other < numUsers; ++other)
            {
                if (other != user)
                {
                    interference += snrByUser.at(snapshot.users.at(other).userId) *
                                    std::norm(combiner.dot(channel.col(other)));
                }
            }
            const double noise = combiner.squaredNorm();
            const double denominator = interference + noise;
            if (!std::isfinite(desired) || !std::isfinite(interference) ||
                !std::isfinite(noise) || desired < 0.0 || interference < 0.0 || noise < 0.0)
            {
                Reject("the uplink detector produced invalid power terms");
            }
            if (denominator == 0.0 && desired != 0.0)
            {
                Reject("the uplink detector produced inconsistent zero denominator power");
            }
            const double sinr = denominator == 0.0 ? 0.0 : desired / denominator;
            if (!std::isfinite(sinr) || sinr < 0.0)
            {
                Reject("the uplink detector produced an invalid SINR");
            }
            result.m_users.at(user).m_desiredSignalPower.at(sc) = desired;
            result.m_users.at(user).m_interUserInterferencePower.at(sc) = interference;
            result.m_users.at(user).m_noisePower.at(sc) = noise;
            result.m_users.at(user).m_sinrLinear.at(sc) = sinr;
        }
    }
    return result;
}

} // namespace ns3
