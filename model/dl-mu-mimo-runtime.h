/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef DL_MU_MIMO_RUNTIME_H
#define DL_MU_MIMO_RUNTIME_H

#include "dl-mu-mimo-link-model.h"
#include "mimo-csi-feedback.h"

#include "ns3/he-ru.h"
#include "ns3/multi-user-scheduler.h"
#include "ns3/nstime.h"
#include "ns3/wifi-acknowledgment.h"
#include "ns3/wifi-protection.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace ns3
{

class SionnaSpectrumPropagationLossModel;

/** MU feedback representation supplied to the runtime selector. */
enum class DlMuMimoFeedbackRepresentation
{
    PERFECT_CHANNEL,
    QUANTIZED_CHANNEL,
    SU_PRECODER_RECONSTRUCTION
};

/** User-group selection policy. */
enum class DlMuMimoGroupingMode
{
    FIXED,
    MINIMUM_CORRELATION
};

/** Joint precoder selected from the complete MU channel snapshot. */
enum class DlMuMimoRuntimePrecoder
{
    FIXED,
    ZERO_FORCING,
    REGULARIZED_ZERO_FORCING
};

/** Explicit association between an ns-3 STA-ID and a stable channel user ID. */
struct DlMuMimoStaUserBinding
{
    uint16_t staId;
    uint32_t userId;
};

/** Scheduler identity retained for one selected user. */
struct DlMuMimoRuntimeUser
{
    uint32_t userId;
    uint16_t staId;
    HeRu::RuSpec ru;
    uint8_t nss;
};

/** Configuration of deterministic runtime grouping and CSI timing. */
struct DlMuMimoRuntimeConfiguration
{
    DlMuMimoGroupingMode groupingMode{DlMuMimoGroupingMode::FIXED};
    DlMuMimoRuntimePrecoder precoder{DlMuMimoRuntimePrecoder::ZERO_FORCING};
    DlMuMimoFeedbackRepresentation feedback{
        DlMuMimoFeedbackRepresentation::PERFECT_CHANNEL};
    std::vector<uint32_t> requestedUserIds;
    uint32_t requestedGroupSize{0};
    double rzfAlpha{0.0};
    double rankTolerance{MimoSvdPrecodingOptions::DEFAULT_RANK_TOLERANCE};
    MimoCsiTimingConfiguration timing;
    /** Explicit joint precoder required when precoder is FIXED. */
    std::optional<DlMuMimoPrecodingMatrix> fixedPrecoding;
};

/**
 * Immutable scheduler and PHY state selected for one exact transmission time.
 *
 * GetUsers() is the decision returned to the MultiUserScheduler. The scheduler
 * must emit exactly these STA-ID, RU, and NSS assignments before Lookup() or
 * Install() can expose the corresponding state to the PHY.
 */
class DlMuMimoRuntimeSelection
{
  public:
    uint32_t GetApNodeId() const;
    MultiUserChannelDirection GetDirection() const;
    const std::vector<DlMuMimoRuntimeUser>& GetUsers() const;
    const MultiUserChannelSnapshot& GetSnapshot() const;
    const DlMuMimoPrecodingMatrix& GetPrecoding() const;
    Time GetCsiTimestamp() const;
    Time GetTransmissionTimestamp() const;
    Time GetCsiAge() const;
    Time GetUpdateInterval() const;
    Time GetFeedbackDelay() const;
    double GetGroupingMetric() const;

  private:
    friend class DlMuMimoRuntime;

    DlMuMimoRuntimeSelection(uint32_t apNodeId,
                             std::vector<DlMuMimoRuntimeUser> users,
                             MultiUserChannelSnapshot snapshot,
                             DlMuMimoPrecodingMatrix precoding,
                             Time transmissionTimestamp,
                             Time updateInterval,
                             Time feedbackDelay,
                             double groupingMetric);

    uint32_t m_apNodeId;
    MultiUserChannelDirection m_direction;
    std::vector<DlMuMimoRuntimeUser> m_users;
    MultiUserChannelSnapshot m_snapshot;
    DlMuMimoPrecodingMatrix m_precoding;
    Time m_transmissionTimestamp;
    Time m_updateInterval;
    Time m_feedbackDelay;
    double m_groupingMetric;
};

/**
 * Runtime bridge between an ns-3 MultiUserScheduler decision and Phase-4.4 PHY state.
 *
 * Phase 4.5 supports HE downlink, perfect full-channel MU CSI, and exactly one
 * stream and receive antenna per user. EHT, uplink, compressed MU feedback,
 * and reconstructed SU precoders are rejected explicitly.
 */
class DlMuMimoRuntime
{
  public:
    explicit DlMuMimoRuntime(DlMuMimoRuntimeConfiguration configuration = {});

    /** Select, validate, and atomically retain state for one scheduler decision. */
    DlMuMimoRuntimeSelection SelectAndStore(
        uint32_t apNodeId,
        const MultiUserChannelSnapshot& snapshot,
        const MultiUserScheduler::DlMuInfo& schedulerInfo,
        const std::vector<DlMuMimoStaUserBinding>& bindings,
        Time transmissionTimestamp);

    /** Retrieve state only for the exact selected scheduler assignment and TX time. */
    const DlMuMimoRuntimeSelection& Lookup(
        uint32_t apNodeId,
        const MultiUserScheduler::DlMuInfo& schedulerInfo,
        const std::vector<DlMuMimoStaUserBinding>& bindings,
        Time transmissionTimestamp) const;

    /** Install the exact retained snapshot and precoder in the Phase-4.4 adapter. */
    void Install(SionnaSpectrumPropagationLossModel& model,
                 uint32_t apNodeId,
                 const MultiUserScheduler::DlMuInfo& schedulerInfo,
                 const std::vector<DlMuMimoStaUserBinding>& bindings,
                 Time transmissionTimestamp) const;

    /** Install retained CSI state with a separate physical TX snapshot. */
    void Install(SionnaSpectrumPropagationLossModel& model,
                 uint32_t apNodeId,
                 const MultiUserScheduler::DlMuInfo& schedulerInfo,
                 const std::vector<DlMuMimoStaUserBinding>& bindings,
                 const MultiUserChannelSnapshot& transmissionSnapshot,
                 Time transmissionTimestamp) const;

  private:
    struct Key
    {
        uint32_t apNodeId;
        MultiUserChannelDirection direction;
        std::vector<DlMuMimoRuntimeUser> users;

        bool operator<(const Key& other) const;
    };

    DlMuMimoRuntimeConfiguration m_configuration;
    std::map<Key, DlMuMimoRuntimeSelection> m_states;
};

} // namespace ns3

#endif // DL_MU_MIMO_RUNTIME_H
