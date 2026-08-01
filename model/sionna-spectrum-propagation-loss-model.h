/*
* Copyright (c) 2025
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: A. Zubow <zubow@tkn.tu-berlin.de>
 */

#ifndef SIONNA_SPECTRUM_PROPAGATION_LOSS_H
#define SIONNA_SPECTRUM_PROPAGATION_LOSS_H

#include "he-su-mimo-link-abstraction.h"
#include "ns3/spectrum-propagation-loss-model.h"
#include "sionna-propagation-cache.h"
#include "ns3/random-variable-stream.h"
#include <ns3/nstime.h>
#include <ns3/object.h>
#include <ns3/traced-callback.h>
#include "cfr-tag.h"

#include <limits>
#include <map>
#include <optional>

namespace ns3
{

class ErrorRateModel;
class WifiTxVector;

/**
 * Result of mapping a dimensioned Sionna CFR to SpectrumWifiPhy's scalar
 * receive signal representation.
 */
class SionnaMimoSpectrumRxResult
{
  public:
    const HeSuMimoLinkResult& GetLinkResult() const;
    double GetEquivalentReceivedPowerW() const;

  private:
    friend class SionnaSpectrumPropagationLossModel;

    HeSuMimoLinkResult m_linkResult;
    double m_equivalentReceivedPowerW{0.0};
};

/**
 * The fast fading loss model computed in Sionna. Note: it is normalized to 1.
 */
class SionnaSpectrumPropagationLossModel : public SpectrumPropagationLossModel
{
  public:
    SionnaSpectrumPropagationLossModel();
    ~SionnaSpectrumPropagationLossModel() override;

    /**
     * \brief Get the type ID.
     * \return the object TypeId
     */
    static TypeId GetTypeId();

    void SetPropagationCache(Ptr<SionnaPropagationCache> propagationCache);

    /**
     * Bypass the Sionna cache with a deterministic normalized CFR. Intended
     * for reproducible examples and end-to-end tests.
     */
    void SetFixedChannelMatrix(const MimoChannelMatrix& channel, double pathLossDb = 0.0);

    /**
     * Configure the noise figure used by the MIMO link budget. It must match
     * the receiving SpectrumWifiPhy's RxNoiseFigure setting.
     */
    void SetReceiverNoiseFigureDb(double noiseFigureDb);

    /** Configure the explicit EESM beta for one HE MCS in linear units. */
    void SetEesmBeta(uint8_t mcs, double betaLinear);

    /** Configure the explicit EESM beta for one HE MCS in dB. */
    void SetEesmBetaDb(uint8_t mcs, double betaDb);

    /** Atomically replace all configured linear EESM betas. */
    void ConfigureEesmBetas(const std::map<uint8_t, double>& betas);

    /** Configure a validated user-provided fixed precoding tensor. */
    void SetPrecodingMatrix(const MimoPrecodingMatrix& precoding);

    /** Restore the Phase-2 path without precoding. */
    void ClearPrecodingMatrix();

    /** Return whether fixed precoding is configured. */
    bool HasPrecodingMatrix() const;

    /** Return the configured fixed precoding tensor. */
    const MimoPrecodingMatrix& GetPrecodingMatrix() const;

    /** Return the receiver noise figure configured for the integrated path. */
    double GetReceiverNoiseFigureDb() const;

    /** Return whether an explicit EESM beta is configured for an HE MCS. */
    bool HasEesmBeta(uint8_t mcs) const;

    /** Return the configured linear EESM beta for an HE MCS. */
    double GetEesmBeta(uint8_t mcs) const;

    /**
     * Evaluate the SU-MIMO abstraction for an already path-loss-attenuated signal.
     */
    SionnaMimoSpectrumRxResult CalculateMimoRx(
        const MimoChannelMatrix& channel,
        double receivedPowerW,
        const WifiTxVector& txVector,
        uint32_t packetSizeBytes,
        uint8_t txPhyAntennas,
        uint8_t rxPhyAntennas,
        double bandwidthHz,
        double receiverNoiseFigureDb,
        Ptr<ErrorRateModel> errorRateModel) const;

  private:
    /**
     * @param params the spectrum signal parameters.
     * \param a sender mobility
     * \param b receiver mobility
     * \return set of values vs frequency representing the received
     *         power in the same units used for the txPsd parameter.
     */
    Ptr<SpectrumValue> DoCalcRxPowerSpectralDensity(Ptr<const SpectrumSignalParameters> params,
                                                    Ptr<const MobilityModel> a,
                                                    Ptr<const MobilityModel> b) const override;

    Ptr<SionnaPropagationCache> m_propagationCache;
    Ptr<HeSuMimoLinkAbstraction> m_linkAbstraction;
    std::optional<MimoPrecodingMatrix> m_precodingMatrix;
    bool m_fixedChannelEnabled{false};
    MimoChannelMatrix m_fixedChannel;
    double m_fixedPathLossDb{0.0};
    double m_receiverNoiseFigureDb{
        std::numeric_limits<double>::quiet_NaN()};

    TracedCallback<uint64_t, uint32_t, uint32_t, double, double, uint32_t> m_mimoRxTrace;
};

} // namespace ns3

#endif /* SIONNA_SPECTRUM_PROPAGATION_LOSS_H */
