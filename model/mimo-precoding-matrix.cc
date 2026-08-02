/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "mimo-precoding-matrix.h"

#include <Eigen/SVD>

#include <algorithm>
#include <cmath>
#include <limits>
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
    throw std::invalid_argument("MimoPrecodingMatrix: " + reason);
}

std::size_t
GetCoefficientCount(uint32_t numSubcarriers, uint32_t numTxAntennas, uint32_t numStreams)
{
    if (numSubcarriers == 0 || numTxAntennas == 0 || numStreams == 0)
    {
        Reject("subcarrier, TX-antenna, and stream counts must be positive");
    }
    if (numStreams > numTxAntennas)
    {
        Reject("the stream count cannot exceed the TX-antenna count");
    }

    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (numSubcarriers > maximum / numTxAntennas ||
        static_cast<std::size_t>(numSubcarriers) * numTxAntennas > maximum / numStreams)
    {
        Reject("coefficient count overflows size_t");
    }
    return static_cast<std::size_t>(numSubcarriers) * numTxAntennas * numStreams;
}

constexpr double SINGULAR_VALUE_TIE_TOLERANCE = 1e-12;
constexpr double CANONICAL_BASIS_TOLERANCE = 1e-12;

void
ValidateSvdInputs(const MimoChannelMatrix& channel,
                  uint32_t numStreams,
                  const MimoSvdPrecodingOptions& options)
{
    if (numStreams == 0 ||
        numStreams > std::min(channel.GetNumTxAntennas(), channel.GetNumRxAntennas()))
    {
        Reject("SVD stream count must be in [1, min(N_tx, N_rx)]");
    }
    if (!std::isfinite(options.rankTolerance) || options.rankTolerance <= 0.0 ||
        options.rankTolerance >= 1.0)
    {
        Reject("SVD rank tolerance must be finite and in (0, 1)");
    }
    if (!std::isfinite(options.referenceSnrLinear) || options.referenceSnrLinear <= 0.0)
    {
        Reject("SVD reference SNR must be finite and positive");
    }
    if (!std::isfinite(options.noisePower) || options.noisePower <= 0.0)
    {
        Reject("SVD noise power must be finite and positive");
    }
    if (!std::isfinite(options.referenceSnrLinear * options.noisePower))
    {
        Reject("SVD total power must be finite");
    }
    if (options.powerAllocation != MimoPowerAllocationMode::EQUAL_POWER &&
        options.powerAllocation != MimoPowerAllocationMode::WATER_FILLING)
    {
        Reject("SVD power-allocation mode is invalid");
    }
    for (const auto coefficient : channel.GetCoefficients())
    {
        if (!std::isfinite(coefficient.real()) || !std::isfinite(coefficient.imag()))
        {
            Reject("SVD requires finite CFR coefficients");
        }
    }
}

Eigen::MatrixXcd
MakeDeterministicRightSingularVectors(const Eigen::MatrixXcd& rightSingularVectors,
                                      const Eigen::VectorXd& singularValues)
{
    const Eigen::Index numTxAntennas = rightSingularVectors.rows();
    const Eigen::Index numModes = singularValues.size();
    Eigen::MatrixXcd deterministic(numTxAntennas, numModes);
    const double tieScale = singularValues(0);

    Eigen::Index groupStart = 0;
    while (groupStart < numModes)
    {
        Eigen::Index groupEnd = groupStart + 1;
        while (groupEnd < numModes &&
               std::abs(singularValues(groupEnd) - singularValues(groupStart)) <=
                   SINGULAR_VALUE_TIE_TOLERANCE * tieScale)
        {
            ++groupEnd;
        }

        const Eigen::MatrixXcd group =
            rightSingularVectors.middleCols(groupStart, groupEnd - groupStart);
        const Eigen::MatrixXcd projector = group * group.adjoint();
        for (Eigen::Index output = groupStart; output < groupEnd; ++output)
        {
            Eigen::VectorXcd bestCandidate(numTxAntennas);
            double bestNorm = -1.0;
            for (Eigen::Index axis = 0; axis < numTxAntennas; ++axis)
            {
                Eigen::VectorXcd candidate = projector.col(axis);
                for (Eigen::Index previous = groupStart; previous < output; ++previous)
                {
                    candidate -= deterministic.col(previous) *
                                 deterministic.col(previous).dot(candidate);
                }
                const double candidateNorm = candidate.norm();
                if (bestNorm < 0.0 ||
                    candidateNorm > bestNorm + CANONICAL_BASIS_TOLERANCE)
                {
                    bestCandidate = candidate;
                    bestNorm = candidateNorm;
                }
            }
            if (!std::isfinite(bestNorm) || bestNorm <= CANONICAL_BASIS_TOLERANCE)
            {
                Reject("SVD could not construct a deterministic degenerate-mode basis");
            }

            Eigen::VectorXcd vector = bestCandidate / bestNorm;
            Eigen::Index pivot = 0;
            double pivotMagnitude = std::abs(vector(0));
            for (Eigen::Index tx = 1; tx < numTxAntennas; ++tx)
            {
                const double magnitude = std::abs(vector(tx));
                if (magnitude > pivotMagnitude + CANONICAL_BASIS_TOLERANCE)
                {
                    pivot = tx;
                    pivotMagnitude = magnitude;
                }
            }
            if (!std::isfinite(pivotMagnitude) || pivotMagnitude == 0.0)
            {
                Reject("SVD produced an invalid right singular vector");
            }
            vector *= std::conj(vector(pivot)) / pivotMagnitude;
            deterministic.col(output) = vector;
        }
        groupStart = groupEnd;
    }
    return deterministic;
}

} // namespace

MimoPrecodingMatrix::MimoPrecodingMatrix(
    uint32_t numSubcarriers,
    uint32_t numTxAntennas,
    uint32_t numStreams,
    const std::vector<std::complex<double>>& coefficients)
    : m_numSubcarriers(numSubcarriers),
      m_numTxAntennas(numTxAntennas),
      m_numStreams(numStreams),
      m_coefficients(coefficients)
{
    const std::size_t expectedCount =
        GetCoefficientCount(numSubcarriers, numTxAntennas, numStreams);
    if (m_coefficients.size() != expectedCount)
    {
        Reject("coefficient count does not match [subcarrier][tx][stream] dimensions");
    }
    for (const auto coefficient : m_coefficients)
    {
        if (!std::isfinite(coefficient.real()) || !std::isfinite(coefficient.imag()))
        {
            Reject("all coefficients must have finite real and imaginary parts");
        }
    }
    for (uint32_t sc = 0; sc < m_numSubcarriers; ++sc)
    {
        const double power = GetPower(sc);
        if (std::abs(power - m_numStreams) > POWER_NORMALIZATION_TOLERANCE)
        {
            Reject("each subcarrier must satisfy ||W[k]||_F^2 = N_s");
        }
    }
}

MimoPrecodingMatrix
MimoPrecodingMatrix::FrequencyFlat(
    uint32_t numSubcarriers,
    uint32_t numTxAntennas,
    uint32_t numStreams,
    const std::vector<std::complex<double>>& coefficients)
{
    const std::size_t perSubcarrierCount =
        GetCoefficientCount(1, numTxAntennas, numStreams);
    if (coefficients.size() != perSubcarrierCount)
    {
        Reject("frequency-flat coefficient count must equal N_tx times N_s");
    }

    std::vector<std::complex<double>> expanded;
    expanded.reserve(GetCoefficientCount(numSubcarriers, numTxAntennas, numStreams));
    for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
    {
        expanded.insert(expanded.end(), coefficients.begin(), coefficients.end());
    }
    return MimoPrecodingMatrix(numSubcarriers, numTxAntennas, numStreams, expanded);
}

MimoPrecodingMatrix
MimoPrecodingMatrix::MaximumRatioTransmission(const MimoChannelMatrix& channel)
{
    if (channel.GetNumRxAntennas() != 1)
    {
        Reject("MRT requires exactly one RX antenna");
    }

    std::vector<std::complex<double>> coefficients;
    coefficients.reserve(static_cast<std::size_t>(channel.GetNumSubcarriers()) *
                         channel.GetNumTxAntennas());
    for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
    {
        double channelNorm = 0.0;
        for (uint32_t tx = 0; tx < channel.GetNumTxAntennas(); ++tx)
        {
            const auto coefficient = channel.Get(sc, 0, tx);
            if (!std::isfinite(coefficient.real()) || !std::isfinite(coefficient.imag()))
            {
                Reject("MRT requires finite CFR coefficients");
            }
            channelNorm = std::hypot(channelNorm, std::abs(coefficient));
        }
        if (channelNorm == 0.0)
        {
            Reject("MRT requires a non-zero channel on every subcarrier");
        }
        for (uint32_t tx = 0; tx < channel.GetNumTxAntennas(); ++tx)
        {
            coefficients.push_back(std::conj(channel.Get(sc, 0, tx)) / channelNorm);
        }
    }
    return MimoPrecodingMatrix(channel.GetNumSubcarriers(),
                               channel.GetNumTxAntennas(),
                               1,
                               coefficients);
}

MimoSvdPrecodingResult
MimoPrecodingMatrix::SingularValueDecomposition(const MimoChannelMatrix& channel,
                                                 uint32_t numStreams,
                                                 const MimoSvdPrecodingOptions& options)
{
    ValidateSvdInputs(channel, numStreams, options);

    const uint32_t numSubcarriers = channel.GetNumSubcarriers();
    const uint32_t numRxAntennas = channel.GetNumRxAntennas();
    const uint32_t numTxAntennas = channel.GetNumTxAntennas();
    const double totalPower = options.referenceSnrLinear * options.noisePower;
    const std::size_t modeCount =
        static_cast<std::size_t>(numSubcarriers) * numStreams;

    std::vector<std::complex<double>> coefficients(
        GetCoefficientCount(numSubcarriers, numTxAntennas, numStreams));
    std::vector<uint32_t> numericalRanks(numSubcarriers);
    std::vector<uint32_t> activeModes(numSubcarriers);
    std::vector<double> selectedSingularValues(modeCount);
    std::vector<double> powerFractions(modeCount);
    std::vector<double> waterLevels(numSubcarriers, 0.0);

    for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
    {
        Eigen::MatrixXcd h(numRxAntennas, numTxAntennas);
        for (uint32_t rx = 0; rx < numRxAntennas; ++rx)
        {
            for (uint32_t tx = 0; tx < numTxAntennas; ++tx)
            {
                h(rx, tx) = channel.Get(sc, rx, tx);
            }
        }

        const Eigen::JacobiSVD<Eigen::MatrixXcd> decomposition(h, Eigen::ComputeThinV);
        if (decomposition.info() != Eigen::Success)
        {
            Reject("SVD decomposition failed");
        }
        const Eigen::VectorXd singularValues = decomposition.singularValues();
        for (Eigen::Index mode = 0; mode < singularValues.size(); ++mode)
        {
            if (!std::isfinite(singularValues(mode)) || singularValues(mode) < 0.0)
            {
                Reject("SVD produced a non-finite singular value");
            }
        }

        const double rankThreshold = options.rankTolerance * singularValues(0);
        uint32_t numericalRank = 0;
        for (Eigen::Index mode = 0; mode < singularValues.size(); ++mode)
        {
            if (singularValues(mode) > rankThreshold)
            {
                ++numericalRank;
            }
        }
        numericalRanks.at(sc) = numericalRank;
        if (numericalRank < numStreams)
        {
            Reject("SVD requested " + std::to_string(numStreams) +
                   " streams but subcarrier " + std::to_string(sc) +
                   " has numerical rank " + std::to_string(numericalRank));
        }

        const Eigen::MatrixXcd rightSingularVectors =
            MakeDeterministicRightSingularVectors(decomposition.matrixV(), singularValues);
        std::vector<double> subcarrierFractions(numStreams, 1.0 / numStreams);
        uint32_t numActiveModes = numStreams;
        if (options.powerAllocation == MimoPowerAllocationMode::WATER_FILLING)
        {
            std::vector<double> inverseSnrGains(numStreams);
            for (uint32_t stream = 0; stream < numStreams; ++stream)
            {
                const double gain = singularValues(stream) * singularValues(stream);
                const double inverseSnrGain = 1.0 / (options.referenceSnrLinear * gain);
                if (!std::isfinite(gain) || gain <= 0.0 ||
                    !std::isfinite(inverseSnrGain))
                {
                    Reject("SVD water filling requires finite positive mode gains");
                }
                inverseSnrGains.at(stream) = inverseSnrGain;
            }

            numActiveModes = numStreams;
            double waterLevelFraction = 0.0;
            while (true)
            {
                double sumInverseSnrGains = 0.0;
                for (uint32_t stream = 0; stream < numActiveModes; ++stream)
                {
                    sumInverseSnrGains += inverseSnrGains.at(stream);
                }
                waterLevelFraction =
                    (1.0 + sumInverseSnrGains) / numActiveModes;
                if (numActiveModes == 1 ||
                    waterLevelFraction > inverseSnrGains.at(numActiveModes - 1))
                {
                    break;
                }
                --numActiveModes;
            }

            std::fill(subcarrierFractions.begin(), subcarrierFractions.end(), 0.0);
            double allocatedFraction = 0.0;
            for (uint32_t stream = 0; stream < numActiveModes; ++stream)
            {
                const double fraction = waterLevelFraction - inverseSnrGains.at(stream);
                if (!std::isfinite(fraction) || fraction < 0.0)
                {
                    Reject("SVD water filling produced an invalid power fraction");
                }
                subcarrierFractions.at(stream) = fraction;
                allocatedFraction += fraction;
            }
            subcarrierFractions.at(numActiveModes - 1) += 1.0 - allocatedFraction;
            if (subcarrierFractions.at(numActiveModes - 1) < 0.0)
            {
                Reject("SVD water filling could not conserve total power");
            }
            waterLevels.at(sc) = totalPower * waterLevelFraction;
            if (!std::isfinite(waterLevels.at(sc)))
            {
                Reject("SVD water level must be finite");
            }
        }
        activeModes.at(sc) = numActiveModes;

        for (uint32_t stream = 0; stream < numStreams; ++stream)
        {
            const std::size_t modeIndex =
                static_cast<std::size_t>(sc) * numStreams + stream;
            selectedSingularValues.at(modeIndex) = singularValues(stream);
            powerFractions.at(modeIndex) = subcarrierFractions.at(stream);
            const double columnScale =
                std::sqrt(numStreams * subcarrierFractions.at(stream));
            for (uint32_t tx = 0; tx < numTxAntennas; ++tx)
            {
                const std::size_t coefficientIndex =
                    (static_cast<std::size_t>(sc) * numTxAntennas + tx) *
                        numStreams +
                    stream;
                coefficients.at(coefficientIndex) =
                    rightSingularVectors(tx, stream) * columnScale;
            }
        }
    }

    MimoPrecodingMatrix precoding(
        numSubcarriers, numTxAntennas, numStreams, coefficients);
    return MimoSvdPrecodingResult(std::move(precoding),
                                  options,
                                  std::move(numericalRanks),
                                  std::move(activeModes),
                                  std::move(selectedSingularValues),
                                  std::move(powerFractions),
                                  std::move(waterLevels));
}

uint32_t
MimoPrecodingMatrix::GetNumSubcarriers() const
{
    return m_numSubcarriers;
}

uint32_t
MimoPrecodingMatrix::GetNumTxAntennas() const
{
    return m_numTxAntennas;
}

uint32_t
MimoPrecodingMatrix::GetNumStreams() const
{
    return m_numStreams;
}

std::complex<double>
MimoPrecodingMatrix::Get(uint32_t subcarrier, uint32_t tx, uint32_t stream) const
{
    return m_coefficients.at(GetFlatIndex(subcarrier, tx, stream));
}

const std::vector<std::complex<double>>&
MimoPrecodingMatrix::GetCoefficients() const
{
    return m_coefficients;
}

double
MimoPrecodingMatrix::GetPower(uint32_t subcarrier) const
{
    if (subcarrier >= m_numSubcarriers)
    {
        Reject("subcarrier index is out of range");
    }
    double power = 0.0;
    for (uint32_t tx = 0; tx < m_numTxAntennas; ++tx)
    {
        for (uint32_t stream = 0; stream < m_numStreams; ++stream)
        {
            power += std::norm(Get(subcarrier, tx, stream));
        }
    }
    return power;
}

void
MimoPrecodingMatrix::ValidateForChannel(const MimoChannelMatrix& channel) const
{
    if (m_numSubcarriers != channel.GetNumSubcarriers())
    {
        Reject("subcarrier count must match the CFR");
    }
    if (m_numTxAntennas != channel.GetNumTxAntennas())
    {
        Reject("TX-antenna count must match the CFR");
    }
    if (m_numStreams > channel.GetNumRxAntennas())
    {
        Reject("stream count cannot exceed the RX-antenna count");
    }
}

MimoChannelMatrix
MimoPrecodingMatrix::Apply(const MimoChannelMatrix& channel) const
{
    ValidateForChannel(channel);
    MimoChannelMatrix effectiveChannel(channel.GetNumSubcarriers(),
                                       channel.GetNumRxAntennas(),
                                       m_numStreams);
    for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
    {
        for (uint32_t rx = 0; rx < channel.GetNumRxAntennas(); ++rx)
        {
            for (uint32_t stream = 0; stream < m_numStreams; ++stream)
            {
                std::complex<double> coefficient{0.0, 0.0};
                for (uint32_t tx = 0; tx < m_numTxAntennas; ++tx)
                {
                    coefficient += channel.Get(sc, rx, tx) * Get(sc, tx, stream);
                }
                effectiveChannel.Set(sc, rx, stream, coefficient);
            }
        }
    }
    return effectiveChannel;
}

std::size_t
MimoPrecodingMatrix::GetFlatIndex(uint32_t subcarrier, uint32_t tx, uint32_t stream) const
{
    if (subcarrier >= m_numSubcarriers)
    {
        Reject("subcarrier index is out of range");
    }
    if (tx >= m_numTxAntennas)
    {
        Reject("TX-antenna index is out of range");
    }
    if (stream >= m_numStreams)
    {
        Reject("stream index is out of range");
    }
    return (static_cast<std::size_t>(subcarrier) * m_numTxAntennas + tx) * m_numStreams +
           stream;
}

MimoSvdPrecodingResult::MimoSvdPrecodingResult(
    MimoPrecodingMatrix precodingMatrix,
    const MimoSvdPrecodingOptions& options,
    std::vector<uint32_t> numericalRanks,
    std::vector<uint32_t> activeModes,
    std::vector<double> singularValues,
    std::vector<double> powerFractions,
    std::vector<double> waterLevels)
    : m_precodingMatrix(std::move(precodingMatrix)),
      m_options(options),
      m_totalPower(options.referenceSnrLinear * options.noisePower),
      m_numericalRanks(std::move(numericalRanks)),
      m_activeModes(std::move(activeModes)),
      m_singularValues(std::move(singularValues)),
      m_powerFractions(std::move(powerFractions)),
      m_waterLevels(std::move(waterLevels))
{
}

const MimoPrecodingMatrix&
MimoSvdPrecodingResult::GetPrecodingMatrix() const
{
    return m_precodingMatrix;
}

MimoPowerAllocationMode
MimoSvdPrecodingResult::GetPowerAllocationMode() const
{
    return m_options.powerAllocation;
}

double
MimoSvdPrecodingResult::GetReferenceSnrLinear() const
{
    return m_options.referenceSnrLinear;
}

double
MimoSvdPrecodingResult::GetNoisePower() const
{
    return m_options.noisePower;
}

double
MimoSvdPrecodingResult::GetTotalPower() const
{
    return m_totalPower;
}

double
MimoSvdPrecodingResult::GetRankTolerance() const
{
    return m_options.rankTolerance;
}

uint32_t
MimoSvdPrecodingResult::GetNumericalRank(uint32_t subcarrier) const
{
    GetFlatIndex(subcarrier, 0);
    return m_numericalRanks.at(subcarrier);
}

uint32_t
MimoSvdPrecodingResult::GetNumActiveModes(uint32_t subcarrier) const
{
    GetFlatIndex(subcarrier, 0);
    return m_activeModes.at(subcarrier);
}

double
MimoSvdPrecodingResult::GetSingularValue(uint32_t subcarrier, uint32_t stream) const
{
    return m_singularValues.at(GetFlatIndex(subcarrier, stream));
}

double
MimoSvdPrecodingResult::GetPowerFraction(uint32_t subcarrier, uint32_t stream) const
{
    return m_powerFractions.at(GetFlatIndex(subcarrier, stream));
}

double
MimoSvdPrecodingResult::GetAllocatedPower(uint32_t subcarrier, uint32_t stream) const
{
    return m_totalPower * GetPowerFraction(subcarrier, stream);
}

double
MimoSvdPrecodingResult::GetTotalAllocatedPower(uint32_t subcarrier) const
{
    GetFlatIndex(subcarrier, 0);
    double total = 0.0;
    for (uint32_t stream = 0; stream < m_precodingMatrix.GetNumStreams(); ++stream)
    {
        total += GetAllocatedPower(subcarrier, stream);
    }
    return total;
}

double
MimoSvdPrecodingResult::GetWaterLevel(uint32_t subcarrier) const
{
    GetFlatIndex(subcarrier, 0);
    return m_waterLevels.at(subcarrier);
}

std::size_t
MimoSvdPrecodingResult::GetFlatIndex(uint32_t subcarrier, uint32_t stream) const
{
    if (subcarrier >= m_precodingMatrix.GetNumSubcarriers())
    {
        Reject("SVD diagnostic subcarrier index is out of range");
    }
    if (stream >= m_precodingMatrix.GetNumStreams())
    {
        Reject("SVD diagnostic stream index is out of range");
    }
    return static_cast<std::size_t>(subcarrier) * m_precodingMatrix.GetNumStreams() +
           stream;
}

} // namespace ns3
