/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef DL_MU_MIMO_LINK_MODEL_H
#define DL_MU_MIMO_LINK_MODEL_H

#include "mimo-precoding-matrix.h"
#include "sionna-propagation-cache.h"

#include <cstdint>
#include <vector>

namespace ns3
{

/** Explicit assignment of one joint-precoder column to a user-local stream. */
struct DlMuMimoStreamMapping
{
    uint32_t userId;
    uint32_t userStream;
};

/**
 * \ingroup sionna
 * Fixed joint downlink precoder W[k]=[W_1[k],...,W_U[k]].
 *
 * The position in streamMappings is the corresponding column of precoding.
 * The embedded Phase-3 matrix retains ||W[k]||_F^2=N_s,total on every tone;
 * unequal column norms therefore express explicit stream-power weighting
 * without changing the total AP transmit power.
 */
class DlMuMimoPrecodingMatrix
{
  public:
    DlMuMimoPrecodingMatrix(MimoPrecodingMatrix precoding,
                            const std::vector<DlMuMimoStreamMapping>& streamMappings);

    /**
     * Derive ideal single-stream-per-user zero-forcing from a perfect snapshot.
     *
     * Requests must contain exactly user-local stream zero once for every
     * snapshot user. Their input order is ignored; output columns follow the
     * snapshot's stable ascending user-ID order.
     */
    static DlMuMimoPrecodingMatrix ZeroForcing(
        const MultiUserChannelSnapshot& snapshot,
        const std::vector<DlMuMimoStreamMapping>& streamMappings,
        double rankTolerance = MimoSvdPrecodingOptions::DEFAULT_RANK_TOLERANCE);

    /**
     * Derive explicitly regularized single-stream-per-user zero-forcing.
     *
     * The dimensionless regularization parameter alpha is mandatory, finite,
     * and strictly positive. No implicit regularization is applied by ZF.
     */
    static DlMuMimoPrecodingMatrix RegularizedZeroForcing(
        const MultiUserChannelSnapshot& snapshot,
        const std::vector<DlMuMimoStreamMapping>& streamMappings,
        double alpha,
        double rankTolerance = MimoSvdPrecodingOptions::DEFAULT_RANK_TOLERANCE);

    const MimoPrecodingMatrix& GetPrecodingMatrix() const;
    uint32_t GetNumStreams() const;
    const DlMuMimoStreamMapping& GetStreamMapping(uint32_t column) const;
    uint32_t GetNumStreamsForUser(uint32_t userId) const;

    /** Return the power in one column of W[k]. */
    double GetStreamPower(uint32_t subcarrier, uint32_t column) const;

    /** Validate all mappings and dimensions against a Phase-4.1 snapshot. */
    void ValidateForSnapshot(const MultiUserChannelSnapshot& snapshot) const;

  private:
    MimoPrecodingMatrix m_precoding;
    std::vector<DlMuMimoStreamMapping> m_streamMappings;
};

/** Per-user post-combining powers and SINRs. */
class DlMuMimoUserResult
{
  public:
    uint32_t GetUserId() const;
    uint32_t GetNumSubcarriers() const;
    uint32_t GetNumStreams() const;
    double GetDesiredSignalPower(uint32_t subcarrier, uint32_t stream) const;
    double GetInterUserInterferencePower(uint32_t subcarrier, uint32_t stream) const;
    double GetSinrLinear(uint32_t subcarrier, uint32_t stream) const;

  private:
    friend class DlMuMimoLinkModel;

    uint32_t m_userId{0};
    uint32_t m_numSubcarriers{0};
    uint32_t m_numStreams{0};
    std::vector<double> m_desiredSignalPower;
    std::vector<double> m_interUserInterferencePower;
    std::vector<double> m_sinrLinear;
};

/** Complete fixed-precoder downlink MU result, ordered by stable user ID. */
class DlMuMimoLinkResult
{
  public:
    uint32_t GetNumUsers() const;
    const DlMuMimoUserResult& GetUser(uint32_t index) const;
    const DlMuMimoUserResult& GetUserById(uint32_t userId) const;

  private:
    friend class DlMuMimoLinkModel;
    std::vector<DlMuMimoUserResult> m_users;
};

/** Total received SNR for one user before small-scale CFR weighting. */
struct DlMuMimoUserSnr
{
    uint32_t userId;
    double referenceSnrLinear;
};

/**
 * \ingroup sionna
 * Fixed downlink MU-MIMO abstraction with a linear MMSE receiver per user.
 */
class DlMuMimoLinkModel
{
  public:
    /**
     * Calculate desired power, inter-user interference, and post-MMSE SINR.
     *
     * One equal-power symbol convention is used across all joint streams:
     * p_u=rho_u/N_s,total. Column norms of W[k] encode any explicit unequal
     * allocation while ||W[k]||_F^2=N_s,total preserves total AP power.
     */
    static DlMuMimoLinkResult Calculate(const MultiUserChannelSnapshot& snapshot,
                                        const DlMuMimoPrecodingMatrix& precoding,
                                        const std::vector<DlMuMimoUserSnr>& userSnrs);
};

} // namespace ns3

#endif // DL_MU_MIMO_LINK_MODEL_H
