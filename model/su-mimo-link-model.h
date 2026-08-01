/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef SU_MIMO_LINK_MODEL_H
#define SU_MIMO_LINK_MODEL_H

#include "mimo-channel-matrix.h"
#include "mimo-precoding-matrix.h"

#include <cstdint>
#include <vector>

namespace ns3
{

/**
 * \ingroup sionna
 * Results of the minimal open-loop SU-MIMO abstraction.
 */
class SuMimoLinkResult
{
  public:
    uint32_t GetNumSubcarriers() const;
    uint32_t GetNumStreams() const;
    double GetSinrLinear(uint32_t subcarrier, uint32_t stream) const;
    double GetEffectiveSinrLinear(uint32_t stream) const;

    /** Minimum numerical channel rank over all subcarriers. */
    uint32_t GetMinimumChannelRank() const;

    /**
     * Average Gaussian spectral efficiency in bit/s/Hz, summed over streams.
     */
    double GetSpectralEfficiency() const;

  private:
    friend class SuMimoLinkModel;

    uint32_t m_numSubcarriers{0};
    uint32_t m_numStreams{0};
    std::vector<double> m_sinrLinear;
    std::vector<double> m_effectiveSinrLinear;
    uint32_t m_minimumChannelRank{0};
    double m_spectralEfficiency{0.0};
};

/**
 * \ingroup sionna
 *
 * Minimal SU-MIMO spatial-multiplexing abstraction. Every transmit antenna
 * carries one independent stream with an equal share of the total power. The
 * receiver has perfect CFR knowledge and applies linear MMSE detection.
 */
class SuMimoLinkModel
{
  public:
    /**
     * Calculate post-MMSE SINR for every stream and subcarrier.
     *
     * @param channel normalized frequency-selective MIMO CFR
     * @param referenceSnrLinear received SNR before small-scale CFR weighting;
     *        this is the total SNR across all spatial streams
     */
    static SuMimoLinkResult Calculate(const MimoChannelMatrix& channel,
                                      double referenceSnrLinear);

    /**
     * Calculate post-MMSE SINR after applying user-provided fixed precoding.
     *
     * MMSE operates on H[k]W[k], and the total reference power is divided
     * equally over the N_s columns of W[k].
     *
     * @param channel normalized frequency-selective MIMO CFR
     * @param precoding validated frequency-selective precoding tensor
     * @param referenceSnrLinear total received SNR before CFR weighting
     */
    static SuMimoLinkResult Calculate(const MimoChannelMatrix& channel,
                                      const MimoPrecodingMatrix& precoding,
                                      double referenceSnrLinear);
};

} // namespace ns3

#endif // SU_MIMO_LINK_MODEL_H
