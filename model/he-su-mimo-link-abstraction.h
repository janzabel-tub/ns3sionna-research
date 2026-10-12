/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef HE_SU_MIMO_LINK_ABSTRACTION_H
#define HE_SU_MIMO_LINK_ABSTRACTION_H

#include "su-mimo-link-model.h"
#include "he-effective-sinr-mapping.h"
#include "wifi-link-budget.h"

#include "ns3/object.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace ns3
{

class ErrorRateModel;
class WifiTxVector;

/** Exact HE PHY context for one independently measured AWGN correction. */
struct HeAwgnCalibrationKey
{
    uint8_t mcs{0};
    bool ldpc{false};
    uint32_t packetSizeBytes{0};
    uint8_t nss{0};
    uint8_t numRxAntennas{0};
    uint16_t channelWidthMhz{0};
    uint16_t guardIntervalNs{0};

    bool IsValid() const;

    bool operator<(const HeAwgnCalibrationKey& other) const
    {
        return std::tie(mcs, ldpc, packetSizeBytes, nss, numRxAntennas,
                        channelWidthMhz, guardIntervalNs) <
               std::tie(other.mcs, other.ldpc, other.packetSizeBytes, other.nss,
                        other.numRxAntennas, other.channelWidthMhz,
                        other.guardIntervalNs);
    }
};

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
 * Maps post-MMSE data SINRs to coded HE packet error probability.
 * Mean uncoded BER is the default. Explicit EESM uses caller-supplied betas.
 */
class HeSuMimoLinkAbstraction : public Object
{
  public:
    static TypeId GetTypeId();

    HeSuMimoLinkAbstraction();

    void SetErrorRateModel(Ptr<ErrorRateModel> errorRateModel);
    Ptr<ErrorRateModel> GetErrorRateModel() const;

    /** Select the common mapping; configuring beta alone does not select EESM. */
    void SetEffectiveSinrModel(SionnaMimoEffectiveSinrModel model);
    SionnaMimoEffectiveSinrModel GetEffectiveSinrModel() const;
    /** Set an explicit EESM beta in linear SINR units. */
    void SetEesmBeta(uint8_t mcs, double betaLinear);

    /**
     * Set the EESM beta for an HE MCS in dB.
     */
    void SetEesmBetaDb(uint8_t mcs, double betaDb);

    bool HasEesmBeta(uint8_t mcs) const;
    double GetEesmBeta(uint8_t mcs) const;

    /** Set a measured AWGN SNR correction for an exact PHY context. */
    void SetAwgnSnrOffsetDb(const HeAwgnCalibrationKey& key, double offsetDb);

    /** Return the SINR passed to ErrorRateModel, with a matching correction. */
    double GetErrorModelSinrLinear(uint8_t mcs,
                                   const WifiTxVector& txVector,
                                   uint32_t packetSizeBytes,
                                   uint8_t numRxAntennas,
                                   uint16_t staId,
                                   double effectiveSinrLinear) const;

    /**
     * Compress every tone and spatial-stream SINR into one effective SINR.
     */
    double CalculateEffectiveSinr(const SuMimoLinkResult& mimoResult, uint8_t mcs,
                                  const std::vector<uint32_t>& dataIndices = {}) const;

    /** Compress an explicit non-empty collection of SINR samples with the selected mapping. */
    double CalculateEffectiveSinr(const std::vector<double>& sinrLinear, uint8_t mcs) const;

    /**
     * Calculate reference SNR, post-MMSE SINRs, effective SINR and packet PER.
     */
    HeSuMimoLinkResult Calculate(const MimoChannelMatrix& channel,
                                 const WifiLinkBudgetResult& linkBudget,
                                 const WifiTxVector& txVector,
                                 uint32_t packetSizeBytes,
                                 const std::vector<uint32_t>& dataIndices = {}) const;

    /**
     * Calculate the complete HE result after applying fixed precoding.
     */
    HeSuMimoLinkResult Calculate(const MimoChannelMatrix& channel,
                                 const MimoPrecodingMatrix& precoding,
                                 const WifiLinkBudgetResult& linkBudget,
                                 const WifiTxVector& txVector,
                                 uint32_t packetSizeBytes,
                                 const std::vector<uint32_t>& dataIndices = {}) const;

  private:
    SionnaMimoEffectiveSinrModel m_effectiveSinrModel{SionnaMimoEffectiveSinrModel::MEAN_UNCODED_BER};
    Ptr<ErrorRateModel> m_errorRateModel;
    std::map<uint8_t, double> m_eesmBetaLinear;
    std::map<HeAwgnCalibrationKey, double> m_awgnSnrOffsetsDb;
};

} // namespace ns3

#endif // HE_SU_MIMO_LINK_ABSTRACTION_H
