/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "mimo-precoding-matrix.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

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

} // namespace ns3
