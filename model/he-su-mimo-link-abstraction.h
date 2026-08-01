/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef HE_SU_MIMO_LINK_ABSTRACTION_H
#define HE_SU_MIMO_LINK_ABSTRACTION_H

#include "su-mimo-link-model.h"
#include "wifi-link-budget.h"

#include "ns3/object.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <map>

namespace ns3
{

class ErrorRateModel;
class WifiTxVector;

/**
 * \ingroup sionna
 * Complete result of the 802.11ax SU-MIMO link abstraction.
 */
class HeSuMimoLinkResult
{
  public:
    const WifiLinkBudgetResult& GetLinkBudget() const;
    const SuMimoLinkResult& GetMimoResult() const;
    double GetEffectiveSinrLinear() const;
    double GetPacketSuccessRate() const;
    double GetPacketErrorRate() const;

  private:
    friend class HeSuMimoLinkAbstraction;

    WifiLinkBudgetResult m_linkBudget;
    SuMimoLinkResult m_mimoResult;
    double m_effectiveSinrLinear{0.0};
    double m_packetSuccessRate{0.0};
};

/**
 * \ingroup sionna
 *
 * Maps post-MMSE SINRs to an HE packet error probability. EESM calibration
 * parameters are explicit and MCS-dependent: no default beta values are
 * supplied.
 */
class HeSuMimoLinkAbstraction : public Object
{
  public:
    static TypeId GetTypeId();

    HeSuMimoLinkAbstraction();

    void SetErrorRateModel(Ptr<ErrorRateModel> errorRateModel);
    Ptr<ErrorRateModel> GetErrorRateModel() const;

    /**
     * Set the EESM beta for an HE MCS in linear SINR units.
     */
    void SetEesmBeta(uint8_t mcs, double betaLinear);

    /**
     * Set the EESM beta for an HE MCS in dB.
     */
    void SetEesmBetaDb(uint8_t mcs, double betaDb);

    bool HasEesmBeta(uint8_t mcs) const;
    double GetEesmBeta(uint8_t mcs) const;

    /**
     * Compress every tone and spatial-stream SINR into one effective SINR.
     */
    double CalculateEffectiveSinr(const SuMimoLinkResult& mimoResult, uint8_t mcs) const;

    /**
     * Calculate reference SNR, post-MMSE SINRs, effective SINR and packet PER.
     */
    HeSuMimoLinkResult Calculate(const MimoChannelMatrix& channel,
                                 const WifiLinkBudgetResult& linkBudget,
                                 const WifiTxVector& txVector,
                                 uint32_t packetSizeBytes) const;

    /**
     * Calculate the complete HE result after applying fixed precoding.
     */
    HeSuMimoLinkResult Calculate(const MimoChannelMatrix& channel,
                                 const MimoPrecodingMatrix& precoding,
                                 const WifiLinkBudgetResult& linkBudget,
                                 const WifiTxVector& txVector,
                                 uint32_t packetSizeBytes) const;

  private:
    Ptr<ErrorRateModel> m_errorRateModel;
    std::map<uint8_t, double> m_eesmBetaLinear;
};

} // namespace ns3

#endif // HE_SU_MIMO_LINK_ABSTRACTION_H
