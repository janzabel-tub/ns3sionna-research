/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "wifi-link-budget.h"

#include "ns3/abort.h"
#include "ns3/wifi-utils.h"

#include <cmath>

namespace ns3
{

namespace
{

// Keep the same Boltzmann constant used by ns-3.40 InterferenceHelper.
constexpr double BOLTZMANN_J_PER_K = 1.3803e-23;

} // namespace

double
WifiLinkBudgetResult::GetReceivedPowerW() const
{
    return m_receivedPowerW;
}

double
WifiLinkBudgetResult::GetThermalNoisePowerW() const
{
    return m_thermalNoisePowerW;
}

double
WifiLinkBudgetResult::GetReceiverNoisePowerW() const
{
    return m_receiverNoisePowerW;
}

double
WifiLinkBudgetResult::GetReferenceSnrLinear() const
{
    return m_referenceSnrLinear;
}

double
WifiLinkBudget::DbToLinear(double valueDb)
{
    NS_ABORT_MSG_IF(!std::isfinite(valueDb), "dB value must be finite");
    const double valueLinear = ns3::DbToRatio(valueDb);
    NS_ABORT_MSG_IF(!std::isfinite(valueLinear) || valueLinear <= 0.0,
                    "dB value is outside the supported linear range");
    return valueLinear;
}

double
WifiLinkBudget::LinearToDb(double valueLinear)
{
    NS_ABORT_MSG_IF(!std::isfinite(valueLinear) || valueLinear <= 0.0,
                    "Linear value must be finite and positive");
    return ns3::RatioToDb(valueLinear);
}

WifiLinkBudgetResult
WifiLinkBudget::Calculate(double txPowerDbm,
                          double pathLossDb,
                          double bandwidthHz,
                          double temperatureK,
                          double receiverNoiseFigureDb)
{
    NS_ABORT_MSG_IF(!std::isfinite(txPowerDbm), "Transmit power must be finite");
    NS_ABORT_MSG_IF(!std::isfinite(pathLossDb), "Path loss must be finite");
    NS_ABORT_MSG_IF(!std::isfinite(bandwidthHz) || bandwidthHz <= 0.0,
                    "Bandwidth must be finite and positive");
    NS_ABORT_MSG_IF(!std::isfinite(temperatureK) || temperatureK <= 0.0,
                    "Temperature must be finite and positive");
    NS_ABORT_MSG_IF(!std::isfinite(receiverNoiseFigureDb),
                    "Receiver noise figure must be finite");

    WifiLinkBudgetResult result;
    result.m_receivedPowerW = ns3::DbmToW(txPowerDbm - pathLossDb);
    result.m_thermalNoisePowerW = BOLTZMANN_J_PER_K * temperatureK * bandwidthHz;
    result.m_receiverNoisePowerW =
        result.m_thermalNoisePowerW * DbToLinear(receiverNoiseFigureDb);
    result.m_referenceSnrLinear = result.m_receivedPowerW / result.m_receiverNoisePowerW;

    NS_ABORT_MSG_IF(!std::isfinite(result.m_receivedPowerW) || result.m_receivedPowerW <= 0.0,
                    "Received power is outside the supported range");
    NS_ABORT_MSG_IF(!std::isfinite(result.m_receiverNoisePowerW) ||
                        result.m_receiverNoisePowerW <= 0.0,
                    "Receiver noise power is outside the supported range");
    NS_ABORT_MSG_IF(!std::isfinite(result.m_referenceSnrLinear) ||
                        result.m_referenceSnrLinear <= 0.0,
                    "Reference SNR is outside the supported range");
    return result;
}

} // namespace ns3
