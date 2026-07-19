/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef WIFI_LINK_BUDGET_H
#define WIFI_LINK_BUDGET_H

namespace ns3
{

/**
 * \ingroup sionna
 * Power and noise terms of a scalar Wi-Fi link budget.
 */
class WifiLinkBudgetResult
{
  public:
    double GetReceivedPowerW() const;
    double GetThermalNoisePowerW() const;
    double GetReceiverNoisePowerW() const;
    double GetReferenceSnrLinear() const;

  private:
    friend class WifiLinkBudget;

    double m_receivedPowerW{0.0};
    double m_thermalNoisePowerW{0.0};
    double m_receiverNoisePowerW{0.0};
    double m_referenceSnrLinear{0.0};
};

/**
 * \ingroup sionna
 *
 * Calculate the total received SNR before small-scale CFR weighting. The
 * receiver noise is k*T*B multiplied by the receiver noise factor.
 */
class WifiLinkBudget
{
  public:
    /** Convert a finite value in dB to a linear power ratio. */
    static double DbToLinear(double valueDb);

    /** Convert a finite, positive linear power ratio to dB. */
    static double LinearToDb(double valueLinear);

    /**
     * Calculate the received power, thermal noise, receiver noise and SNR.
     *
     * @param txPowerDbm total transmit power in dBm
     * @param pathLossDb positive attenuation in dB
     * @param bandwidthHz receiver noise bandwidth in Hz
     * @param temperatureK receiver noise temperature in kelvin
     * @param receiverNoiseFigureDb receiver noise figure in dB
     */
    static WifiLinkBudgetResult Calculate(double txPowerDbm,
                                          double pathLossDb,
                                          double bandwidthHz,
                                          double temperatureK,
                                          double receiverNoiseFigureDb);
};

} // namespace ns3

#endif // WIFI_LINK_BUDGET_H
