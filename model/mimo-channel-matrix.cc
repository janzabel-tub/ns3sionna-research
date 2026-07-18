/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "mimo-channel-matrix.h"

#include "ns3/abort.h"

namespace ns3
{

MimoChannelMatrix::MimoChannelMatrix()
    : m_numSubcarriers(0),
      m_numRxAntennas(0),
      m_numTxAntennas(0)
{
}

MimoChannelMatrix::MimoChannelMatrix(uint32_t numSubcarriers,
                                     uint32_t numRxAntennas,
                                     uint32_t numTxAntennas)
    : m_numSubcarriers(numSubcarriers),
      m_numRxAntennas(numRxAntennas),
      m_numTxAntennas(numTxAntennas),
      m_coefficients(static_cast<std::size_t>(numSubcarriers) * numRxAntennas * numTxAntennas)
{
    NS_ABORT_MSG_IF(numSubcarriers == 0, "A MIMO channel needs at least one subcarrier");
    NS_ABORT_MSG_IF(numRxAntennas == 0, "A MIMO channel needs at least one receive antenna");
    NS_ABORT_MSG_IF(numTxAntennas == 0, "A MIMO channel needs at least one transmit antenna");
}

MimoChannelMatrix::MimoChannelMatrix(
    uint32_t numSubcarriers,
    uint32_t numRxAntennas,
    uint32_t numTxAntennas,
    const std::vector<std::complex<double>>& coefficients)
    : MimoChannelMatrix(numSubcarriers, numRxAntennas, numTxAntennas)
{
    NS_ABORT_MSG_IF(coefficients.size() != m_coefficients.size(),
                    "CFR coefficient count does not match its dimensions");
    m_coefficients = coefficients;
}

uint32_t
MimoChannelMatrix::GetNumSubcarriers() const
{
    return m_numSubcarriers;
}

uint32_t
MimoChannelMatrix::GetNumRxAntennas() const
{
    return m_numRxAntennas;
}

uint32_t
MimoChannelMatrix::GetNumTxAntennas() const
{
    return m_numTxAntennas;
}

std::complex<double>
MimoChannelMatrix::Get(uint32_t subcarrier, uint32_t rx, uint32_t tx) const
{
    return m_coefficients.at(GetFlatIndex(subcarrier, rx, tx));
}

void
MimoChannelMatrix::Set(uint32_t subcarrier,
                       uint32_t rx,
                       uint32_t tx,
                       std::complex<double> value)
{
    m_coefficients.at(GetFlatIndex(subcarrier, rx, tx)) = value;
}

const std::vector<std::complex<double>>&
MimoChannelMatrix::GetCoefficients() const
{
    return m_coefficients;
}

std::size_t
MimoChannelMatrix::GetFlatIndex(uint32_t subcarrier, uint32_t rx, uint32_t tx) const
{
    NS_ABORT_MSG_IF(subcarrier >= m_numSubcarriers, "Subcarrier index is out of range");
    NS_ABORT_MSG_IF(rx >= m_numRxAntennas, "Receive-antenna index is out of range");
    NS_ABORT_MSG_IF(tx >= m_numTxAntennas, "Transmit-antenna index is out of range");
    return (static_cast<std::size_t>(subcarrier) * m_numRxAntennas + rx) * m_numTxAntennas +
           tx;
}

} // namespace ns3
