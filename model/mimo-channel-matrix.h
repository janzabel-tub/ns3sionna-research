/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef MIMO_CHANNEL_MATRIX_H
#define MIMO_CHANNEL_MATRIX_H

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ns3
{

/**
 * \ingroup sionna
 *
 * A frequency-selective MIMO channel response.
 *
 * Coefficients are stored in subcarrier-major, receive-antenna-major order:
 * ((subcarrier * N_rx) + rx) * N_tx + tx. Stating the layout here keeps the
 * Sionna/Protocol Buffer boundary independent of a particular matrix library.
 */
class MimoChannelMatrix
{
  public:
    /**
     * Create an empty CFR tensor.
     *
     * This state represents a response for which CSI estimation was disabled.
     */
    MimoChannelMatrix();

    /**
     * Create a zero-filled CFR tensor.
     *
     * @param numSubcarriers number of OFDM subcarriers
     * @param numRxAntennas number of receive antennas
     * @param numTxAntennas number of transmit antennas
     */
    MimoChannelMatrix(uint32_t numSubcarriers,
                      uint32_t numRxAntennas,
                      uint32_t numTxAntennas);

    /**
     * Create a CFR tensor from coefficients in the documented flat layout.
     * Lvalues are copied; an rvalue transfers its coefficient storage.
     */
    MimoChannelMatrix(uint32_t numSubcarriers,
                      uint32_t numRxAntennas,
                      uint32_t numTxAntennas,
                      std::vector<std::complex<double>> coefficients);

    uint32_t GetNumSubcarriers() const;
    uint32_t GetNumRxAntennas() const;
    uint32_t GetNumTxAntennas() const;

    std::complex<double> Get(uint32_t subcarrier, uint32_t rx, uint32_t tx) const;
    void Set(uint32_t subcarrier, uint32_t rx, uint32_t tx, std::complex<double> value);

    const std::vector<std::complex<double>>& GetCoefficients() const;

  private:
    std::size_t GetFlatIndex(uint32_t subcarrier, uint32_t rx, uint32_t tx) const;

    uint32_t m_numSubcarriers;
    uint32_t m_numRxAntennas;
    uint32_t m_numTxAntennas;
    std::vector<std::complex<double>> m_coefficients;
};

} // namespace ns3

#endif // MIMO_CHANNEL_MATRIX_H
