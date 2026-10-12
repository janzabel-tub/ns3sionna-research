/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "he-su-mimo-link-abstraction.h"

#include "ns3/abort.h"
#include "ns3/error-rate-model.h"
#include "ns3/pointer.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/wifi-tx-vector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ns3
{

bool
HeAwgnCalibrationKey::IsValid() const
{
    return mcs <= 11 && packetSizeBytes > 0 && nss > 0 && numRxAntennas > 0 &&
           (channelWidthMhz == 20 || channelWidthMhz == 40 ||
            channelWidthMhz == 80 || channelWidthMhz == 160) &&
           (guardIntervalNs == 800 || guardIntervalNs == 1600 ||
            guardIntervalNs == 3200);
}

NS_OBJECT_ENSURE_REGISTERED(HeSuMimoLinkAbstraction);

TypeId
HeSuMimoLinkAbstraction::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::HeSuMimoLinkAbstraction")
            .SetParent<Object>()
            .SetGroupName("Sionna")
            .AddConstructor<HeSuMimoLinkAbstraction>()
            .AddAttribute("ErrorRateModel",
                          "AWGN error model used after effective SINR mapping",
                          PointerValue(CreateObject<TableBasedErrorRateModel>()),
                          MakePointerAccessor(&HeSuMimoLinkAbstraction::m_errorRateModel),
                          MakePointerChecker<ErrorRateModel>());
    return tid;
}

HeSuMimoLinkAbstraction::HeSuMimoLinkAbstraction()
    : m_errorRateModel(CreateObject<TableBasedErrorRateModel>())
{
}

const WifiLinkBudgetResult&
HeSuMimoLinkResult::GetLinkBudget() const
{
    return m_linkBudget;
}

const SuMimoLinkResult&
HeSuMimoLinkResult::GetMimoResult() const
{
    return m_mimoResult;
}

double
HeSuMimoLinkResult::GetEffectiveSinrLinear() const
{
    return m_effectiveSinrLinear;
}

double
HeSuMimoLinkResult::GetPacketSuccessRate() const
{
    return m_packetSuccessRate;
}

double
HeSuMimoLinkResult::GetPacketErrorRate() const
{
    return 1.0 - m_packetSuccessRate;
}

void
HeSuMimoLinkAbstraction::SetErrorRateModel(Ptr<ErrorRateModel> errorRateModel)
{
    NS_ABORT_MSG_IF(!errorRateModel, "ErrorRateModel must not be null");
    m_errorRateModel = errorRateModel;
}

Ptr<ErrorRateModel>
HeSuMimoLinkAbstraction::GetErrorRateModel() const
{
    return m_errorRateModel;
}

void
HeSuMimoLinkAbstraction::SetEesmBeta(uint8_t mcs, double betaLinear)
{
    NS_ABORT_MSG_IF(mcs > 11, "802.11ax defines HE MCS values from 0 through 11");
    NS_ABORT_MSG_IF(!std::isfinite(betaLinear) || betaLinear <= 0.0,
                    "EESM beta must be finite and positive");
    m_eesmBetaLinear[mcs] = betaLinear;
}

void
HeSuMimoLinkAbstraction::SetEffectiveSinrModel(SionnaMimoEffectiveSinrModel model)
{
    NS_ABORT_MSG_IF(model != SionnaMimoEffectiveSinrModel::EESM &&
                    model != SionnaMimoEffectiveSinrModel::MEAN_UNCODED_BER &&
                    model != SionnaMimoEffectiveSinrModel::LEGACY_SISO,
                    "Unsupported HE effective SINR mapping");
    m_effectiveSinrModel = model;
}
SionnaMimoEffectiveSinrModel
HeSuMimoLinkAbstraction::GetEffectiveSinrModel() const { return m_effectiveSinrModel; }

void
HeSuMimoLinkAbstraction::SetEesmBetaDb(uint8_t mcs, double betaDb)
{
    SetEesmBeta(mcs, WifiLinkBudget::DbToLinear(betaDb));
}

bool
HeSuMimoLinkAbstraction::HasEesmBeta(uint8_t mcs) const
{
    return m_eesmBetaLinear.find(mcs) != m_eesmBetaLinear.end();
}

double
HeSuMimoLinkAbstraction::GetEesmBeta(uint8_t mcs) const
{
    const auto beta = m_eesmBetaLinear.find(mcs);
    NS_ABORT_MSG_IF(beta == m_eesmBetaLinear.end(),
                    "No EESM beta configured for HE MCS " << +mcs);
    return beta->second;
}

void
HeSuMimoLinkAbstraction::SetAwgnSnrOffsetDb(const HeAwgnCalibrationKey& key,
                                            double offsetDb)
{
    NS_ABORT_MSG_IF(!key.IsValid(), "Invalid HE AWGN calibration PHY context");
    NS_ABORT_MSG_IF(!std::isfinite(offsetDb) || std::abs(offsetDb) > 30.0,
                    "HE AWGN SNR offset must be finite and within +/-30 dB");
    m_awgnSnrOffsetsDb[key] = offsetDb;
}

double
HeSuMimoLinkAbstraction::GetErrorModelSinrLinear(uint8_t mcs,
                                                 const WifiTxVector& txVector,
                                                 uint32_t packetSizeBytes,
                                                 uint8_t numRxAntennas,
                                                 uint16_t staId,
                                                 double effectiveSinrLinear) const
{
    NS_ABORT_MSG_IF(!std::isfinite(effectiveSinrLinear) || effectiveSinrLinear < 0.0,
                    "Effective SINR must be finite and non-negative");
    if (m_effectiveSinrModel != SionnaMimoEffectiveSinrModel::EESM)
    {
        return effectiveSinrLinear;
    }
    const HeAwgnCalibrationKey key{mcs,
                                   txVector.IsLdpc(),
                                   packetSizeBytes,
                                   txVector.GetNss(staId),
                                   numRxAntennas,
                                   txVector.GetChannelWidth(),
                                   txVector.GetGuardInterval()};
    const auto calibration = m_awgnSnrOffsetsDb.find(key);
    if (calibration == m_awgnSnrOffsetsDb.end())
    {
        return effectiveSinrLinear;
    }
    const double adjusted = effectiveSinrLinear *
                            WifiLinkBudget::DbToLinear(calibration->second);
    NS_ABORT_MSG_IF(!std::isfinite(adjusted),
                    "Calibrated ErrorRateModel SINR is not finite");
    return adjusted;
}

double
HeSuMimoLinkAbstraction::CalculateEffectiveSinr(const SuMimoLinkResult& mimoResult,
                                                uint8_t mcs,
                                                const std::vector<uint32_t>& dataIndices) const
{
    NS_ABORT_MSG_IF(mimoResult.GetNumSubcarriers() == 0 || mimoResult.GetNumStreams() == 0,
                    "EESM requires at least one SINR sample");

    std::vector<double> sinrLinear;
    sinrLinear.reserve(static_cast<std::size_t>(mimoResult.GetNumSubcarriers()) *
                       mimoResult.GetNumStreams());
    std::vector<uint32_t> selected = dataIndices;
    if (selected.empty())
    {
        for (uint32_t sc = 0; sc < mimoResult.GetNumSubcarriers(); ++sc) { selected.push_back(sc); }
    }
    for (uint32_t sc : selected)
    {
        NS_ABORT_MSG_IF(sc >= mimoResult.GetNumSubcarriers(), "Data tone outside SINR grid");
        for (uint32_t stream = 0; stream < mimoResult.GetNumStreams(); ++stream)
        {
            sinrLinear.push_back(mimoResult.GetSinrLinear(sc, stream));
        }
    }

    return CalculateEffectiveSinr(sinrLinear, mcs);
}

double
HeSuMimoLinkAbstraction::CalculateEffectiveSinr(const std::vector<double>& sinrLinear,
                                                uint8_t mcs) const
{
    NS_ABORT_MSG_IF(sinrLinear.empty(), "EESM requires at least one SINR sample");

    NS_ABORT_MSG_IF(m_effectiveSinrModel == SionnaMimoEffectiveSinrModel::LEGACY_SISO,
                    "LEGACY_SISO uses the physical 1x1 PHY path, not numerical compression");
    if (m_effectiveSinrModel == SionnaMimoEffectiveSinrModel::MEAN_UNCODED_BER)
    {
        return HeEffectiveSinrMapping::MeanUncodedBer(sinrLinear, mcs).effectiveSinrLinear;
    }
    const double beta = GetEesmBeta(mcs);
    double minimumSinr = std::numeric_limits<double>::infinity();
    for (const double sinr : sinrLinear)
    {
        NS_ABORT_MSG_IF(!std::isfinite(sinr) || sinr < 0.0,
                        "EESM SINR samples must be finite and non-negative");
        minimumSinr = std::min(minimumSinr, sinr);
    }

    double exponentialSum = 0.0;
    for (const double sinr : sinrLinear)
    {
        exponentialSum += std::exp(-(sinr - minimumSinr) / beta);
    }

    const double numSamples = static_cast<double>(sinrLinear.size());
    const double effectiveSinr =
        minimumSinr - beta * std::log(exponentialSum / numSamples);
    NS_ABORT_MSG_IF(!std::isfinite(effectiveSinr) || effectiveSinr < 0.0,
                    "EESM produced an invalid effective SINR");
    return effectiveSinr;
}

HeSuMimoLinkResult
HeSuMimoLinkAbstraction::Calculate(const MimoChannelMatrix& channel,
                                   const WifiLinkBudgetResult& linkBudget,
                                   const WifiTxVector& txVector,
                                   uint32_t packetSizeBytes,
                                   const std::vector<uint32_t>& dataIndices) const
{
    NS_ABORT_MSG_IF(!m_errorRateModel, "ErrorRateModel must be configured");
    NS_ABORT_MSG_IF(!txVector.GetModeInitialized(), "WifiTxVector mode is not initialized");
    const WifiMode mode = txVector.GetMode();
    NS_ABORT_MSG_IF(mode.GetModulationClass() != WIFI_MOD_CLASS_HE,
                    "The link abstraction requires an 802.11ax HE mode");
    NS_ABORT_MSG_IF(txVector.GetNss() != channel.GetNumTxAntennas(),
                    "WifiTxVector NSS must match the number of transmitted spatial streams");
    NS_ABORT_MSG_IF(channel.GetNumRxAntennas() > std::numeric_limits<uint8_t>::max(),
                    "ErrorRateModel cannot represent more than 255 receive antennas");
    NS_ABORT_MSG_IF(packetSizeBytes == 0, "Packet size must be positive");

    HeSuMimoLinkResult result;
    result.m_linkBudget = linkBudget;
    result.m_mimoResult =
        SuMimoLinkModel::Calculate(channel, linkBudget.GetReferenceSnrLinear());
    result.m_effectiveSinrLinear =
        CalculateEffectiveSinr(result.m_mimoResult, mode.GetMcsValue(), dataIndices);
    result.m_packetSuccessRate =
        m_errorRateModel->GetChunkSuccessRate(mode,
                                              txVector,
                                              GetErrorModelSinrLinear(mode.GetMcsValue(),
                                                                      txVector,
                                                                      packetSizeBytes,
                                                                      static_cast<uint8_t>(channel.GetNumRxAntennas()),
                                                                      SU_STA_ID,
                                                                      result.m_effectiveSinrLinear),
                                              static_cast<uint64_t>(packetSizeBytes) * 8,
                                              static_cast<uint8_t>(channel.GetNumRxAntennas()));
    NS_ABORT_MSG_IF(!std::isfinite(result.m_packetSuccessRate) ||
                        result.m_packetSuccessRate < 0.0 ||
                        result.m_packetSuccessRate > 1.0,
                    "ErrorRateModel returned an invalid packet success probability");
    return result;
}

HeSuMimoLinkResult
HeSuMimoLinkAbstraction::Calculate(const MimoChannelMatrix& channel,
                                   const MimoPrecodingMatrix& precoding,
                                   const WifiLinkBudgetResult& linkBudget,
                                   const WifiTxVector& txVector,
                                   uint32_t packetSizeBytes,
                                   const std::vector<uint32_t>& dataIndices) const
{
    NS_ABORT_MSG_IF(!m_errorRateModel, "ErrorRateModel must be configured");
    NS_ABORT_MSG_IF(!txVector.GetModeInitialized(), "WifiTxVector mode is not initialized");
    const WifiMode mode = txVector.GetMode();
    NS_ABORT_MSG_IF(mode.GetModulationClass() != WIFI_MOD_CLASS_HE,
                    "The link abstraction requires an 802.11ax HE mode");
    NS_ABORT_MSG_IF(txVector.GetNss() != precoding.GetNumStreams(),
                    "WifiTxVector NSS must match the precoding stream dimension");
    NS_ABORT_MSG_IF(channel.GetNumRxAntennas() > std::numeric_limits<uint8_t>::max(),
                    "ErrorRateModel cannot represent more than 255 receive antennas");
    NS_ABORT_MSG_IF(packetSizeBytes == 0, "Packet size must be positive");

    precoding.ValidateForChannel(channel);

    HeSuMimoLinkResult result;
    result.m_linkBudget = linkBudget;
    result.m_mimoResult = SuMimoLinkModel::Calculate(channel,
                                                     precoding,
                                                     linkBudget.GetReferenceSnrLinear());
    result.m_effectiveSinrLinear =
        CalculateEffectiveSinr(result.m_mimoResult, mode.GetMcsValue(), dataIndices);
    result.m_packetSuccessRate =
        m_errorRateModel->GetChunkSuccessRate(mode,
                                              txVector,
                                              GetErrorModelSinrLinear(mode.GetMcsValue(),
                                                                      txVector,
                                                                      packetSizeBytes,
                                                                      static_cast<uint8_t>(channel.GetNumRxAntennas()),
                                                                      SU_STA_ID,
                                                                      result.m_effectiveSinrLinear),
                                              static_cast<uint64_t>(packetSizeBytes) * 8,
                                              static_cast<uint8_t>(channel.GetNumRxAntennas()));
    NS_ABORT_MSG_IF(!std::isfinite(result.m_packetSuccessRate) ||
                        result.m_packetSuccessRate < 0.0 ||
                        result.m_packetSuccessRate > 1.0,
                    "ErrorRateModel returned an invalid packet success probability");
    return result;
}

} // namespace ns3
