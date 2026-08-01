/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef MIMO_PRECODING_MATRIX_H
#define MIMO_PRECODING_MATRIX_H

#include "mimo-channel-matrix.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ns3
{

/**
 * \ingroup sionna
 *
 * A validated SU-MIMO precoding tensor.
 *
 * Coefficients are stored in subcarrier-major, transmit-antenna-major order:
 * ((subcarrier * N_tx) + tx) * N_s + stream. Every stored matrix obeys
 * ||W[k]||_F^2 = N_s, so the reference transmit power remains the total power
 * over all streams. Frequency-flat input is expanded explicitly to every
 * subcarrier by FrequencyFlat().
 */
class MimoPrecodingMatrix
{
  public:
    /** Numerical tolerance used for per-subcarrier power validation. */
    static constexpr double POWER_NORMALIZATION_TOLERANCE = 1e-10;

    /**
     * Create a frequency-selective precoding tensor from the documented flat layout.
     *
     * \throws std::invalid_argument for invalid dimensions, coefficients, or power
     */
    MimoPrecodingMatrix(uint32_t numSubcarriers,
                        uint32_t numTxAntennas,
                        uint32_t numStreams,
                        const std::vector<std::complex<double>>& coefficients);

    /**
     * Expand one [tx][stream] matrix explicitly over all subcarriers.
     *
     * \throws std::invalid_argument for invalid dimensions, coefficients, or power
     */
    static MimoPrecodingMatrix FrequencyFlat(
        uint32_t numSubcarriers,
        uint32_t numTxAntennas,
        uint32_t numStreams,
        const std::vector<std::complex<double>>& coefficients);

    /**
     * Derive single-stream maximum-ratio transmission from a perfect MISO CFR.
     *
     * For every subcarrier, this returns W[k] = H[k]^H / ||H[k]||_2. The
     * operation is deliberately restricted to one receive antenna; the
     * general MIMO single-stream optimum belongs to SVD precoding.
     *
     * \throws std::invalid_argument unless N_rx is one and every subcarrier
     *         has a finite, non-zero channel norm
     */
    static MimoPrecodingMatrix MaximumRatioTransmission(
        const MimoChannelMatrix& channel);

    uint32_t GetNumSubcarriers() const;
    uint32_t GetNumTxAntennas() const;
    uint32_t GetNumStreams() const;

    std::complex<double> Get(uint32_t subcarrier, uint32_t tx, uint32_t stream) const;
    const std::vector<std::complex<double>>& GetCoefficients() const;

    /** Return ||W[k]||_F^2. */
    double GetPower(uint32_t subcarrier) const;

    /**
     * Validate the subcarrier, antenna, and stream dimensions against a CFR.
     *
     * \throws std::invalid_argument if the contract does not match the CFR
     */
    void ValidateForChannel(const MimoChannelMatrix& channel) const;

    /**
     * Return the effective channel H[k]W[k].
     *
     * \throws std::invalid_argument if the contract does not match the CFR
     */
    MimoChannelMatrix Apply(const MimoChannelMatrix& channel) const;

  private:
    std::size_t GetFlatIndex(uint32_t subcarrier, uint32_t tx, uint32_t stream) const;

    uint32_t m_numSubcarriers;
    uint32_t m_numTxAntennas;
    uint32_t m_numStreams;
    std::vector<std::complex<double>> m_coefficients;
};

} // namespace ns3

#endif // MIMO_PRECODING_MATRIX_H
