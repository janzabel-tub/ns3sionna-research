/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef UL_MU_MIMO_LINK_MODEL_H
#define UL_MU_MIMO_LINK_MODEL_H

#include "sionna-propagation-cache.h"

#include <cstdint>
#include <vector>

namespace ns3
{

/** Joint linear detector used by the Phase-4.7 uplink numerical core. */
enum class UlMuMimoDetector
{
    ZERO_FORCING,
    LINEAR_MMSE
};

/** Explicit assignment of one uplink user to its local spatial stream. */
struct UlMuMimoStreamMapping
{
    uint32_t userId;
    uint32_t userStream;
};

/** Per-user received SNR before small-scale CFR weighting. */
struct UlMuMimoUserSnr
{
    uint32_t userId;
    double referenceSnrLinear;
};

/** Per-user post-combining powers and SINRs. */
class UlMuMimoUserResult
{
  public:
    uint32_t GetUserId() const;
    uint32_t GetNumSubcarriers() const;
    uint32_t GetNumStreams() const;
    double GetDesiredSignalPower(uint32_t subcarrier, uint32_t stream) const;
    double GetInterUserInterferencePower(uint32_t subcarrier, uint32_t stream) const;
    double GetNoisePower(uint32_t subcarrier, uint32_t stream) const;
    double GetSinrLinear(uint32_t subcarrier, uint32_t stream) const;

  private:
    friend class UlMuMimoLinkModel;

    uint32_t m_userId{0};
    uint32_t m_numSubcarriers{0};
    std::vector<double> m_desiredSignalPower;
    std::vector<double> m_interUserInterferencePower;
    std::vector<double> m_noisePower;
    std::vector<double> m_sinrLinear;
};

/** Complete uplink MU result, ordered by stable user ID. */
class UlMuMimoLinkResult
{
  public:
    uint32_t GetNumUsers() const;
    const UlMuMimoUserResult& GetUser(uint32_t index) const;
    const UlMuMimoUserResult& GetUserById(uint32_t userId) const;

  private:
    friend class UlMuMimoLinkModel;
    std::vector<UlMuMimoUserResult> m_users;
};

/**
 * \ingroup sionna
 * Joint uplink MU-MIMO abstraction for ideal time/frequency synchronization.
 *
 * Phase 4.7 intentionally supports exactly one transmit antenna and one
 * stream per user. All user CFR columns terminate at the same AP receive
 * array. Per-user reference SNRs retain independent transmit-power and
 * path-loss effects; they are never averaged into a group power.
 */
class UlMuMimoLinkModel
{
  public:
    static constexpr double DEFAULT_RANK_TOLERANCE = 1e-12;

    /**
     * Calculate desired power, inter-user interference, enhanced noise, and
     * post-processing SINR for every user and subcarrier.
     */
    static UlMuMimoLinkResult Calculate(
        const MultiUserChannelSnapshot& snapshot,
        const std::vector<UlMuMimoStreamMapping>& streamMappings,
        const std::vector<UlMuMimoUserSnr>& userSnrs,
        UlMuMimoDetector detector,
        double rankTolerance = DEFAULT_RANK_TOLERANCE);
};

} // namespace ns3

#endif // UL_MU_MIMO_LINK_MODEL_H
