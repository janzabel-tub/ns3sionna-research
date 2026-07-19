/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "su-mimo-link-model.h"

#include "ns3/abort.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>

namespace ns3
{

uint32_t
SuMimoLinkResult::GetNumSubcarriers() const
{
    return m_numSubcarriers;
}

uint32_t
SuMimoLinkResult::GetNumStreams() const
{
    return m_numStreams;
}

double
SuMimoLinkResult::GetSinrLinear(uint32_t subcarrier, uint32_t stream) const
{
    NS_ABORT_MSG_IF(subcarrier >= m_numSubcarriers, "Subcarrier index is out of range");
    NS_ABORT_MSG_IF(stream >= m_numStreams, "Stream index is out of range");
    return m_sinrLinear.at(static_cast<std::size_t>(subcarrier) * m_numStreams + stream);
}

double
SuMimoLinkResult::GetEffectiveSinrLinear(uint32_t stream) const
{
    NS_ABORT_MSG_IF(stream >= m_numStreams, "Stream index is out of range");
    return m_effectiveSinrLinear.at(stream);
}

uint32_t
SuMimoLinkResult::GetMinimumChannelRank() const
{
    return m_minimumChannelRank;
}

double
SuMimoLinkResult::GetSpectralEfficiency() const
{
    return m_spectralEfficiency;
}

SuMimoLinkResult
SuMimoLinkModel::Calculate(const MimoChannelMatrix& channel, double referenceSnrLinear)
{
    NS_ABORT_MSG_IF(!std::isfinite(referenceSnrLinear) || referenceSnrLinear < 0.0,
                    "Reference SNR must be finite and non-negative");
    NS_ABORT_MSG_IF(channel.GetNumTxAntennas() > channel.GetNumRxAntennas(),
                    "The minimal model requires at least as many RX antennas as spatial streams");

    const uint32_t numSubcarriers = channel.GetNumSubcarriers();
    const uint32_t numStreams = channel.GetNumTxAntennas();
    const uint32_t numRxAntennas = channel.GetNumRxAntennas();
    const double streamSnr = referenceSnrLinear / numStreams;

    SuMimoLinkResult result;
    result.m_numSubcarriers = numSubcarriers;
    result.m_numStreams = numStreams;
    result.m_sinrLinear.resize(static_cast<std::size_t>(numSubcarriers) * numStreams);
    result.m_effectiveSinrLinear.resize(numStreams);
    result.m_minimumChannelRank = std::min(numRxAntennas, numStreams);
    std::vector<double> accumulatedMutualInformation(numStreams, 0.0);

    for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
    {
        Eigen::MatrixXcd h(numRxAntennas, numStreams);
        for (uint32_t rx = 0; rx < numRxAntennas; ++rx)
        {
            for (uint32_t stream = 0; stream < numStreams; ++stream)
            {
                h(rx, stream) = channel.Get(sc, rx, stream);
            }
        }

        Eigen::FullPivLU<Eigen::MatrixXcd> rankDecomposition(h);
        result.m_minimumChannelRank =
            std::min(result.m_minimumChannelRank,
                     static_cast<uint32_t>(rankDecomposition.rank()));

        const Eigen::MatrixXcd identity = Eigen::MatrixXcd::Identity(numStreams, numStreams);
        const Eigen::MatrixXcd mmseMatrix = identity + streamSnr * h.adjoint() * h;
        const Eigen::LDLT<Eigen::MatrixXcd> decomposition(mmseMatrix);
        NS_ABORT_MSG_IF(decomposition.info() != Eigen::Success,
                        "Could not decompose the MMSE covariance matrix");
        const Eigen::MatrixXcd inverse = decomposition.solve(identity);
        NS_ABORT_MSG_IF(decomposition.info() != Eigen::Success,
                        "Could not solve the MMSE covariance system");

        for (uint32_t stream = 0; stream < numStreams; ++stream)
        {
            const double inverseDiagonal = inverse(stream, stream).real();
            NS_ABORT_MSG_IF(!std::isfinite(inverseDiagonal) || inverseDiagonal <= 0.0,
                            "Invalid MMSE covariance inverse");
            // Numerical round-off can produce a tiny negative value at zero SNR.
            const double sinr = std::max(0.0, 1.0 / inverseDiagonal - 1.0);
            result.m_sinrLinear[static_cast<std::size_t>(sc) * numStreams + stream] = sinr;
            accumulatedMutualInformation[stream] += std::log1p(sinr);
            result.m_spectralEfficiency += std::log2(1.0 + sinr);
        }
    }

    result.m_spectralEfficiency /= numSubcarriers;
    for (uint32_t stream = 0; stream < numStreams; ++stream)
    {
        // Mutual-information-preserving compression across OFDM subcarriers.
        result.m_effectiveSinrLinear[stream] =
            std::expm1(accumulatedMutualInformation[stream] / numSubcarriers);
    }
    return result;
}

} // namespace ns3
