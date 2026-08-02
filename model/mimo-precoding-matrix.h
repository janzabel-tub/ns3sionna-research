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

/** Spatial power allocation used by ideal per-subcarrier SVD precoding. */
enum class MimoPowerAllocationMode
{
    EQUAL_POWER,
    WATER_FILLING
};

/**
 * \ingroup sionna
 * Options for ideal per-subcarrier SVD precoding.
 *
 * The default is equal power. Water filling is confined to the selected
 * spatial modes of each subcarrier and therefore preserves the per-tone total
 * power. The total power used for diagnostics is referenceSnrLinear times
 * noisePower.
 */
struct MimoSvdPrecodingOptions
{
    /** A singular value is non-zero iff sigma > tolerance * sigma_max. */
    static constexpr double DEFAULT_RANK_TOLERANCE = 1e-12;

    MimoPowerAllocationMode powerAllocation{MimoPowerAllocationMode::EQUAL_POWER};
    double referenceSnrLinear{1.0};
    double noisePower{1.0};
    double rankTolerance{DEFAULT_RANK_TOLERANCE};
};

class MimoSvdPrecodingResult;

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

    /**
     * Derive deterministic per-subcarrier SVD precoding from perfect CFR.
     *
     * The requested strongest right singular vectors are used as W[k]. Equal
     * power is the default. In water-filling mode, their column norms encode
     * the allocated powers while preserving ||W[k]||_F^2 = N_s.
     *
     * \param channel perfect frequency-selective MIMO CFR
     * \param numStreams explicit number of selected spatial modes
     * \param options rank threshold and power-allocation inputs
     * \throws std::invalid_argument for invalid inputs or if any subcarrier's
     *         numerical rank is smaller than numStreams
     */
    static MimoSvdPrecodingResult SingularValueDecomposition(
        const MimoChannelMatrix& channel,
        uint32_t numStreams,
        const MimoSvdPrecodingOptions& options = {});

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

/**
 * \ingroup sionna
 * Immutable diagnostics and fixed W[k] produced by ideal SVD precoding.
 */
class MimoSvdPrecodingResult
{
  public:
    const MimoPrecodingMatrix& GetPrecodingMatrix() const;
    MimoPowerAllocationMode GetPowerAllocationMode() const;
    double GetReferenceSnrLinear() const;
    double GetNoisePower() const;
    double GetTotalPower() const;
    double GetRankTolerance() const;

    uint32_t GetNumericalRank(uint32_t subcarrier) const;
    uint32_t GetNumActiveModes(uint32_t subcarrier) const;
    double GetSingularValue(uint32_t subcarrier, uint32_t stream) const;
    double GetPowerFraction(uint32_t subcarrier, uint32_t stream) const;
    double GetAllocatedPower(uint32_t subcarrier, uint32_t stream) const;
    double GetTotalAllocatedPower(uint32_t subcarrier) const;

    /** Water level in power units; zero for equal-power allocation. */
    double GetWaterLevel(uint32_t subcarrier) const;

  private:
    friend class MimoPrecodingMatrix;

    MimoSvdPrecodingResult(MimoPrecodingMatrix precodingMatrix,
                           const MimoSvdPrecodingOptions& options,
                           std::vector<uint32_t> numericalRanks,
                           std::vector<uint32_t> activeModes,
                           std::vector<double> singularValues,
                           std::vector<double> powerFractions,
                           std::vector<double> waterLevels);

    std::size_t GetFlatIndex(uint32_t subcarrier, uint32_t stream) const;

    MimoPrecodingMatrix m_precodingMatrix;
    MimoSvdPrecodingOptions m_options;
    double m_totalPower;
    std::vector<uint32_t> m_numericalRanks;
    std::vector<uint32_t> m_activeModes;
    std::vector<double> m_singularValues;
    std::vector<double> m_powerFractions;
    std::vector<double> m_waterLevels;
};

} // namespace ns3

#endif // MIMO_PRECODING_MATRIX_H
