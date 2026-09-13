/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef HE_UL_MU_MIMO_LINK_ABSTRACTION_H
#define HE_UL_MU_MIMO_LINK_ABSTRACTION_H

#include "he-su-mimo-link-abstraction.h"
#include "ul-mu-mimo-link-model.h"
#include "wifi-link-budget.h"

#include "ns3/object.h"
#include "ns3/ptr.h"
#include "ns3/wifi-tx-vector.h"

#include <cstdint>
#include <vector>

namespace ns3
{

class ErrorRateModel;

/** Per-user inputs for Phase-4.7 HE EESM/PER evaluation. */
struct HeUlMuMimoUserTransmission
{
    uint32_t userId;
    WifiLinkBudgetResult linkBudget;
    WifiTxVector txVector;
    uint32_t packetSizeBytes;
    uint32_t userStream{0};
    uint16_t staId{SU_STA_ID};
    /** CFR tones allocated to this user; empty selects the full grid. */
    std::vector<uint32_t> subcarrierIndices{};
};

/** Complete HE numerical result for one uplink MU user. */
class HeUlMuMimoUserResult
{
  public:
    uint32_t GetUserId() const;
    const WifiLinkBudgetResult& GetLinkBudget() const;
    const UlMuMimoUserResult& GetMimoResult() const;
    double GetEffectiveSinrLinear() const;
    double GetPacketSuccessRate() const;
    double GetPacketErrorRate() const;

  private:
    friend class HeUlMuMimoLinkAbstraction;

    uint32_t m_userId{0};
    WifiLinkBudgetResult m_linkBudget;
    UlMuMimoUserResult m_mimoResult;
    double m_effectiveSinrLinear{0.0};
    double m_packetSuccessRate{0.0};
};

/** Per-user uplink EESM/PER results, ordered by stable user ID. */
class HeUlMuMimoLinkResult
{
  public:
    uint32_t GetNumUsers() const;
    const HeUlMuMimoUserResult& GetUser(uint32_t index) const;
    const HeUlMuMimoUserResult& GetUserById(uint32_t userId) const;

  private:
    friend class HeUlMuMimoLinkAbstraction;
    std::vector<HeUlMuMimoUserResult> m_users;
};

/**
 * \ingroup sionna
 * Applies the existing HE SU EESM and ErrorRateModel contract independently
 * to each result of the Phase-4.7 joint uplink detector.
 *
 * This class is numerical only. Coordinated HE-TB reception, trigger frames,
 * and simultaneous-arrival state are intentionally deferred to Phase 4.8.
 */
class HeUlMuMimoLinkAbstraction : public Object
{
  public:
    static TypeId GetTypeId();

    HeUlMuMimoLinkAbstraction();

    void SetErrorRateModel(Ptr<ErrorRateModel> errorRateModel);
    Ptr<ErrorRateModel> GetErrorRateModel() const;
    void SetEesmBeta(uint8_t mcs, double betaLinear);
    void SetEesmBetaDb(uint8_t mcs, double betaDb);
    bool HasEesmBeta(uint8_t mcs) const;
    double GetEesmBeta(uint8_t mcs) const;

    HeUlMuMimoLinkResult Calculate(
        const MultiUserChannelSnapshot& snapshot,
        UlMuMimoDetector detector,
        const std::vector<HeUlMuMimoUserTransmission>& users,
        double rankTolerance = UlMuMimoLinkModel::DEFAULT_RANK_TOLERANCE) const;

  private:
    Ptr<HeSuMimoLinkAbstraction> m_suAbstraction;
};

} // namespace ns3

#endif // HE_UL_MU_MIMO_LINK_ABSTRACTION_H
