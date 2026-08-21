/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "dl-mu-mimo-link-model.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <complex>
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
    throw std::invalid_argument("DlMuMimoLinkModel: " + reason);
}

std::size_t
GetFlatIndex(uint32_t subcarrier, uint32_t stream, uint32_t numStreams)
{
    return static_cast<std::size_t>(subcarrier) * numStreams + stream;
}

void
ValidateSnapshot(const MultiUserChannelSnapshot& snapshot)
{
    if (snapshot.direction != MultiUserChannelDirection::DOWNLINK)
    {
        Reject("the snapshot direction must be downlink");
    }
    if (snapshot.users.empty())
    {
        Reject("the snapshot must contain at least one user");
    }

    std::set<uint32_t> userIds;
    const auto& first = snapshot.users.front();
    const uint32_t numSubcarriers = first.channel.GetNumSubcarriers();
    const uint32_t numTxAntennas = first.channel.GetNumTxAntennas();
    const auto frequencies = first.frequencies;
    if (frequencies.size() != numSubcarriers)
    {
        Reject("the frequency grid must contain one value per subcarrier");
    }

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
        if (user.direction != snapshot.direction || user.csiTimestamp != snapshot.csiTimestamp)
        {
            Reject("user direction or CSI timestamp differs from the snapshot");
        }
        if (user.channel.GetNumSubcarriers() != numSubcarriers ||
            user.channel.GetNumTxAntennas() != numTxAntennas ||
            user.frequencies != frequencies)
        {
            Reject("users have inconsistent frequency grids or channel dimensions");
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

} // namespace

DlMuMimoPrecodingMatrix::DlMuMimoPrecodingMatrix(
    MimoPrecodingMatrix precoding,
    const std::vector<DlMuMimoStreamMapping>& streamMappings)
    : m_precoding(std::move(precoding)),
      m_streamMappings(streamMappings)
{
    if (m_streamMappings.size() != m_precoding.GetNumStreams())
    {
        Reject("the stream-mapping count must match the joint precoder columns");
    }

    std::map<uint32_t, std::set<uint32_t>> streamsByUser;
    for (const auto& mapping : m_streamMappings)
    {
        if (!streamsByUser[mapping.userId].insert(mapping.userStream).second)
        {
            Reject("a user/stream mapping occurs more than once");
        }
    }
    for (const auto& entry : streamsByUser)
    {
        uint32_t expected = 0;
        for (const uint32_t stream : entry.second)
        {
            if (stream != expected++)
            {
                Reject("user-local stream indices must be contiguous from zero");
            }
        }
    }
}

const MimoPrecodingMatrix&
DlMuMimoPrecodingMatrix::GetPrecodingMatrix() const
{
    return m_precoding;
}

uint32_t
DlMuMimoPrecodingMatrix::GetNumStreams() const
{
    return m_precoding.GetNumStreams();
}

const DlMuMimoStreamMapping&
DlMuMimoPrecodingMatrix::GetStreamMapping(uint32_t column) const
{
    if (column >= m_streamMappings.size())
    {
        Reject("joint stream-column index is out of range");
    }
    return m_streamMappings.at(column);
}

uint32_t
DlMuMimoPrecodingMatrix::GetNumStreamsForUser(uint32_t userId) const
{
    return static_cast<uint32_t>(
        std::count_if(m_streamMappings.begin(),
                      m_streamMappings.end(),
                      [userId](const auto& mapping) { return mapping.userId == userId; }));
}

double
DlMuMimoPrecodingMatrix::GetStreamPower(uint32_t subcarrier, uint32_t column) const
{
    if (column >= GetNumStreams())
    {
        Reject("joint stream-column index is out of range");
    }
    double power = 0.0;
    for (uint32_t tx = 0; tx < m_precoding.GetNumTxAntennas(); ++tx)
    {
        power += std::norm(m_precoding.Get(subcarrier, tx, column));
    }
    return power;
}

void
DlMuMimoPrecodingMatrix::ValidateForSnapshot(
    const MultiUserChannelSnapshot& snapshot) const
{
    ValidateSnapshot(snapshot);
    const auto& first = snapshot.users.front().channel;
    if (m_precoding.GetNumSubcarriers() != first.GetNumSubcarriers() ||
        m_precoding.GetNumTxAntennas() != first.GetNumTxAntennas())
    {
        Reject("joint precoder subcarrier and TX dimensions must match every user CFR");
    }

    std::set<uint32_t> snapshotUsers;
    for (const auto& user : snapshot.users)
    {
        snapshotUsers.insert(user.userId);
        const uint32_t streams = GetNumStreamsForUser(user.userId);
        if (streams == 0)
        {
            Reject("every snapshot user must have at least one mapped stream");
        }
        if (streams > user.channel.GetNumRxAntennas())
        {
            Reject("a user's stream count cannot exceed its RX-antenna count");
        }
    }
    for (const auto& mapping : m_streamMappings)
    {
        if (snapshotUsers.find(mapping.userId) == snapshotUsers.end())
        {
            Reject("a stream is mapped to a user absent from the snapshot");
        }
    }
}

uint32_t
DlMuMimoUserResult::GetUserId() const
{
    return m_userId;
}

uint32_t
DlMuMimoUserResult::GetNumSubcarriers() const
{
    return m_numSubcarriers;
}

uint32_t
DlMuMimoUserResult::GetNumStreams() const
{
    return m_numStreams;
}

double
DlMuMimoUserResult::GetDesiredSignalPower(uint32_t subcarrier, uint32_t stream) const
{
    if (subcarrier >= m_numSubcarriers || stream >= m_numStreams)
    {
        Reject("user-result subcarrier or stream index is out of range");
    }
    return m_desiredSignalPower.at(GetFlatIndex(subcarrier, stream, m_numStreams));
}

double
DlMuMimoUserResult::GetInterUserInterferencePower(uint32_t subcarrier,
                                                  uint32_t stream) const
{
    if (subcarrier >= m_numSubcarriers || stream >= m_numStreams)
    {
        Reject("user-result subcarrier or stream index is out of range");
    }
    return m_interUserInterferencePower.at(GetFlatIndex(subcarrier, stream, m_numStreams));
}

double
DlMuMimoUserResult::GetSinrLinear(uint32_t subcarrier, uint32_t stream) const
{
    if (subcarrier >= m_numSubcarriers || stream >= m_numStreams)
    {
        Reject("user-result subcarrier or stream index is out of range");
    }
    return m_sinrLinear.at(GetFlatIndex(subcarrier, stream, m_numStreams));
}

uint32_t
DlMuMimoLinkResult::GetNumUsers() const
{
    return static_cast<uint32_t>(m_users.size());
}

const DlMuMimoUserResult&
DlMuMimoLinkResult::GetUser(uint32_t index) const
{
    return m_users.at(index);
}

const DlMuMimoUserResult&
DlMuMimoLinkResult::GetUserById(uint32_t userId) const
{
    const auto found = std::lower_bound(
        m_users.begin(),
        m_users.end(),
        userId,
        [](const DlMuMimoUserResult& user, uint32_t id) { return user.GetUserId() < id; });
    if (found == m_users.end() || found->GetUserId() != userId)
    {
        Reject("result user ID is absent");
    }
    return *found;
}

DlMuMimoLinkResult
DlMuMimoLinkModel::Calculate(const MultiUserChannelSnapshot& snapshot,
                             const DlMuMimoPrecodingMatrix& precoding,
                             const std::vector<DlMuMimoUserSnr>& userSnrs)
{
    precoding.ValidateForSnapshot(snapshot);

    std::map<uint32_t, double> snrByUser;
    for (const auto& userSnr : userSnrs)
    {
        if (!std::isfinite(userSnr.referenceSnrLinear) || userSnr.referenceSnrLinear < 0.0)
        {
            Reject("every reference SNR must be finite and non-negative");
        }
        if (!snrByUser.emplace(userSnr.userId, userSnr.referenceSnrLinear).second)
        {
            Reject("a reference SNR was supplied more than once");
        }
    }
    if (snrByUser.size() != snapshot.users.size())
    {
        Reject("exactly one reference SNR is required for every snapshot user");
    }
    for (const auto& user : snapshot.users)
    {
        if (snrByUser.find(user.userId) == snrByUser.end())
        {
            Reject("a snapshot user has no reference SNR");
        }
    }

    const auto& matrix = precoding.GetPrecodingMatrix();
    const uint32_t totalStreams = matrix.GetNumStreams();
    DlMuMimoLinkResult result;
    result.m_users.reserve(snapshot.users.size());

    for (const auto& user : snapshot.users)
    {
        const uint32_t numSubcarriers = user.channel.GetNumSubcarriers();
        const uint32_t numRxAntennas = user.channel.GetNumRxAntennas();
        const uint32_t numUserStreams = precoding.GetNumStreamsForUser(user.userId);
        const double streamSnr = snrByUser.at(user.userId) / totalStreams;

        DlMuMimoUserResult userResult;
        userResult.m_userId = user.userId;
        userResult.m_numSubcarriers = numSubcarriers;
        userResult.m_numStreams = numUserStreams;
        const std::size_t resultSize =
            static_cast<std::size_t>(numSubcarriers) * numUserStreams;
        userResult.m_desiredSignalPower.resize(resultSize);
        userResult.m_interUserInterferencePower.resize(resultSize);
        userResult.m_sinrLinear.resize(resultSize);

        for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
        {
            Eigen::MatrixXcd effective(numRxAntennas, totalStreams);
            for (uint32_t rx = 0; rx < numRxAntennas; ++rx)
            {
                for (uint32_t column = 0; column < totalStreams; ++column)
                {
                    std::complex<double> value{0.0, 0.0};
                    for (uint32_t tx = 0; tx < user.channel.GetNumTxAntennas(); ++tx)
                    {
                        value += user.channel.Get(sc, rx, tx) * matrix.Get(sc, tx, column);
                    }
                    effective(rx, column) = value;
                }
            }

            const Eigen::MatrixXcd covariance =
                Eigen::MatrixXcd::Identity(numRxAntennas, numRxAntennas) +
                streamSnr * effective * effective.adjoint();
            const Eigen::LDLT<Eigen::MatrixXcd> decomposition(covariance);
            if (decomposition.info() != Eigen::Success)
            {
                Reject("could not decompose the full MMSE covariance matrix");
            }

            for (uint32_t column = 0; column < totalStreams; ++column)
            {
                const auto& mapping = precoding.GetStreamMapping(column);
                if (mapping.userId != user.userId)
                {
                    continue;
                }

                const Eigen::VectorXcd combiner = decomposition.solve(effective.col(column));
                if (decomposition.info() != Eigen::Success || !combiner.allFinite())
                {
                    Reject("could not solve the full MMSE covariance system");
                }

                const double desired =
                    streamSnr * std::norm(combiner.dot(effective.col(column)));
                double interUser = 0.0;
                double intraUser = 0.0;
                for (uint32_t other = 0; other < totalStreams; ++other)
                {
                    if (other == column)
                    {
                        continue;
                    }
                    const double contribution =
                        streamSnr * std::norm(combiner.dot(effective.col(other)));
                    if (precoding.GetStreamMapping(other).userId == user.userId)
                    {
                        intraUser += contribution;
                    }
                    else
                    {
                        interUser += contribution;
                    }
                }
                const double denominator = combiner.squaredNorm() + intraUser + interUser;
                if (!std::isfinite(desired) || !std::isfinite(interUser) ||
                    !std::isfinite(denominator) || denominator < 0.0)
                {
                    Reject("the MMSE receiver produced invalid power terms");
                }
                if (denominator == 0.0 && desired != 0.0)
                {
                    Reject("the MMSE receiver produced inconsistent zero-noise power terms");
                }
                const double sinr = denominator == 0.0 ? 0.0 : desired / denominator;
                const std::size_t index =
                    GetFlatIndex(sc, mapping.userStream, numUserStreams);
                userResult.m_desiredSignalPower[index] = desired;
                userResult.m_interUserInterferencePower[index] = interUser;
                userResult.m_sinrLinear[index] = sinr;
            }
        }
        result.m_users.push_back(std::move(userResult));
    }
    return result;
}

} // namespace ns3
