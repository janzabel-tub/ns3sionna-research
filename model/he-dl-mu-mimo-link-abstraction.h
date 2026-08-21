/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef HE_DL_MU_MIMO_LINK_ABSTRACTION_H
#define HE_DL_MU_MIMO_LINK_ABSTRACTION_H

#include "dl-mu-mimo-link-model.h"
#include "he-su-mimo-link-abstraction.h"
#include "wifi-link-budget.h"

#include "ns3/object.h"
#include "ns3/ptr.h"
#include "ns3/wifi-tx-vector.h"

#include <cstdint>
#include <vector>

namespace ns3
{

class ErrorRateModel;

/** Per-user HE inputs for the Phase-4.2 numerical MU abstraction. */
struct HeDlMuMimoUserTransmission
{
    uint32_t userId;
    WifiLinkBudgetResult linkBudget;
    WifiTxVector txVector;
    uint32_t packetSizeBytes;
};

/** Complete HE result for one downlink MU user. */
class HeDlMuMimoUserResult
{
  public:
    uint32_t GetUserId() const;
    const WifiLinkBudgetResult& GetLinkBudget() const;
    const DlMuMimoUserResult& GetMimoResult() const;
    double GetEffectiveSinrLinear() const;
    double GetPacketSuccessRate() const;
    double GetPacketErrorRate() const;

  private:
    friend class HeDlMuMimoLinkAbstraction;

    uint32_t m_userId{0};
    WifiLinkBudgetResult m_linkBudget;
    DlMuMimoUserResult m_mimoResult;
    double m_effectiveSinrLinear{0.0};
    double m_packetSuccessRate{0.0};
};

/** Per-user EESM/PER results, ordered by stable user ID. */
class HeDlMuMimoLinkResult
{
  public:
    uint32_t GetNumUsers() const;
    const HeDlMuMimoUserResult& GetUser(uint32_t index) const;
    const HeDlMuMimoUserResult& GetUserById(uint32_t userId) const;

  private:
    friend class HeDlMuMimoLinkAbstraction;
    std::vector<HeDlMuMimoUserResult> m_users;
};

/**
 * \ingroup sionna
 * Applies the existing HE SU EESM and ErrorRateModel contract independently
 * to every Phase-4.2 downlink MU user.
 */
class HeDlMuMimoLinkAbstraction : public Object
{
  public:
    static TypeId GetTypeId();

    HeDlMuMimoLinkAbstraction();

    void SetErrorRateModel(Ptr<ErrorRateModel> errorRateModel);
    Ptr<ErrorRateModel> GetErrorRateModel() const;
    void SetEesmBeta(uint8_t mcs, double betaLinear);
    void SetEesmBetaDb(uint8_t mcs, double betaDb);
    bool HasEesmBeta(uint8_t mcs) const;
    double GetEesmBeta(uint8_t mcs) const;

    HeDlMuMimoLinkResult Calculate(
        const MultiUserChannelSnapshot& snapshot,
        const DlMuMimoPrecodingMatrix& precoding,
        const std::vector<HeDlMuMimoUserTransmission>& users) const;

  private:
    Ptr<HeSuMimoLinkAbstraction> m_suAbstraction;
};

} // namespace ns3

#endif // HE_DL_MU_MIMO_LINK_ABSTRACTION_H
