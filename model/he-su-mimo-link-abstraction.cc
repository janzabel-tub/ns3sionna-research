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
                          "AWGN error model used after EESM compression",
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

double
HeSuMimoLinkAbstraction::CalculateEffectiveSinr(const SuMimoLinkResult& mimoResult,
                                                uint8_t mcs) const
{
    NS_ABORT_MSG_IF(mimoResult.GetNumSubcarriers() == 0 || mimoResult.GetNumStreams() == 0,
                    "EESM requires at least one SINR sample");

    const double beta = GetEesmBeta(mcs);
    double minimumSinr = std::numeric_limits<double>::infinity();
    for (uint32_t sc = 0; sc < mimoResult.GetNumSubcarriers(); ++sc)
    {
        for (uint32_t stream = 0; stream < mimoResult.GetNumStreams(); ++stream)
        {
            minimumSinr = std::min(minimumSinr, mimoResult.GetSinrLinear(sc, stream));
        }
    }

    double exponentialSum = 0.0;
    for (uint32_t sc = 0; sc < mimoResult.GetNumSubcarriers(); ++sc)
    {
        for (uint32_t stream = 0; stream < mimoResult.GetNumStreams(); ++stream)
        {
            const double sinr = mimoResult.GetSinrLinear(sc, stream);
            exponentialSum += std::exp(-(sinr - minimumSinr) / beta);
        }
    }

    const double numSamples =
        static_cast<double>(mimoResult.GetNumSubcarriers()) * mimoResult.GetNumStreams();
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
                                   uint32_t packetSizeBytes) const
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
        CalculateEffectiveSinr(result.m_mimoResult, mode.GetMcsValue());
    result.m_packetSuccessRate =
        m_errorRateModel->GetChunkSuccessRate(mode,
                                              txVector,
                                              result.m_effectiveSinrLinear,
                                              static_cast<uint64_t>(packetSizeBytes) * 8,
                                              static_cast<uint8_t>(channel.GetNumRxAntennas()));
    NS_ABORT_MSG_IF(!std::isfinite(result.m_packetSuccessRate) ||
                        result.m_packetSuccessRate < 0.0 ||
                        result.m_packetSuccessRate > 1.0,
                    "ErrorRateModel returned an invalid packet success probability");
    return result;
}

} // namespace ns3
