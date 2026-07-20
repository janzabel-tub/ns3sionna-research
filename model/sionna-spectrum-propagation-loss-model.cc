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

#include "sionna-spectrum-propagation-loss-model.h"
//#include "sionna-utils.h"

#include "ns3/socket.h"
#include <ns3/abort.h>
#include <ns3/double.h>
#include <ns3/error-rate-model.h>
#include <ns3/interference-helper.h>
#include <ns3/log.h>
#include <ns3/node.h>
#include <ns3/object-factory.h>
#include <ns3/pointer.h>
#include <ns3/random-variable-stream.h>
#include <ns3/spectrum-channel.h>
#include <ns3/spectrum-signal-parameters.h>
#include <ns3/string.h>
#include <ns3/wifi-phy.h>
#include <ns3/wifi-ppdu.h>
#include <ns3/wifi-psdu.h>
#include <ns3/wifi-net-device.h>
#include <ns3/wifi-spectrum-phy-interface.h>
#include <ns3/wifi-spectrum-signal-parameters.h>
#include <ns3/wifi-spectrum-value-helper.h>
#include <ns3/wifi-tx-vector.h>
#include <ns3/wifi-utils.h>

#include <algorithm>
#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("SionnaSpectrumPropagationLossModel");

NS_OBJECT_ENSURE_REGISTERED(SionnaSpectrumPropagationLossModel);

namespace
{

constexpr double WIFI_NOISE_TEMPERATURE_K = 290.0;

Ptr<SpectrumWifiPhy>
GetSpectrumWifiPhy(Ptr<const MobilityModel> mobility, Ptr<Channel> peerChannel)
{
    Ptr<Node> node = mobility->GetObject<Node>();
    NS_ABORT_MSG_IF(!node, "SpectrumWifiPhy mobility is not aggregated to a node");

    Ptr<SpectrumWifiPhy> match;
    for (uint32_t i = 0; i < node->GetNDevices(); ++i)
    {
        Ptr<WifiNetDevice> wifiDevice = DynamicCast<WifiNetDevice>(node->GetDevice(i));
        if (!wifiDevice)
        {
            continue;
        }
        Ptr<SpectrumWifiPhy> candidate = DynamicCast<SpectrumWifiPhy>(wifiDevice->GetPhy());
        if (!candidate || candidate->GetMobility() != mobility)
        {
            continue;
        }
        if (peerChannel && candidate->GetChannel() != peerChannel)
        {
            continue;
        }
        NS_ABORT_MSG_IF(match, "More than one matching SpectrumWifiPhy found on receiver node");
        match = candidate;
    }
    NS_ABORT_MSG_IF(!match, "No matching SpectrumWifiPhy found on receiver node");
    return match;
}

Ptr<SpectrumWifiPhy>
GetTransmitterSpectrumWifiPhy(Ptr<const SpectrumSignalParameters> params)
{
    NS_ABORT_MSG_IF(!params->txPhy || !params->txPhy->GetDevice(),
                    "MIMO Wi-Fi reception requires a transmitting SpectrumPhy device");
    Ptr<WifiNetDevice> device = DynamicCast<WifiNetDevice>(params->txPhy->GetDevice());
    NS_ABORT_MSG_IF(!device, "MIMO Wi-Fi transmitter is not a WifiNetDevice");
    Ptr<SpectrumWifiPhy> phy = DynamicCast<SpectrumWifiPhy>(device->GetPhy());
    NS_ABORT_MSG_IF(!phy, "MIMO Wi-Fi transmitter does not use SpectrumWifiPhy");
    return phy;
}

Ptr<ErrorRateModel>
GetErrorRateModel(Ptr<SpectrumWifiPhy> phy)
{
    PointerValue helperValue;
    phy->GetAttribute("InterferenceHelper", helperValue);
    Ptr<InterferenceHelper> helper = helperValue.Get<InterferenceHelper>();
    NS_ABORT_MSG_IF(!helper || !helper->GetErrorRateModel(),
                    "SpectrumWifiPhy ErrorRateModel is not configured");
    return helper->GetErrorRateModel();
}

} // namespace

const HeSuMimoLinkResult&
SionnaMimoSpectrumRxResult::GetLinkResult() const
{
    return m_linkResult;
}

double
SionnaMimoSpectrumRxResult::GetEquivalentReceivedPowerW() const
{
    return m_equivalentReceivedPowerW;
}

SionnaSpectrumPropagationLossModel::SionnaSpectrumPropagationLossModel()
    : m_linkAbstraction(CreateObject<HeSuMimoLinkAbstraction>())
{
    NS_LOG_FUNCTION(this);
}

SionnaSpectrumPropagationLossModel::~SionnaSpectrumPropagationLossModel()
{
    NS_LOG_FUNCTION(this);
}

TypeId
SionnaSpectrumPropagationLossModel::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::SionnaSpectrumPropagationLossModel")
            .SetParent<SpectrumPropagationLossModel>()
            .SetGroupName("Sionna")
            .AddConstructor<SionnaSpectrumPropagationLossModel>()
            .AddTraceSource("MimoRx",
                            "SU-MIMO PPDU result: UID, TX node, RX node, effective SINR, PER, "
                            "and minimum channel rank",
                            MakeTraceSourceAccessor(
                                &SionnaSpectrumPropagationLossModel::m_mimoRxTrace),
                            "ns3::TracedCallback::Uint64Uint32Uint32DoubleDoubleUint32");
    return tid;
}

void
SionnaSpectrumPropagationLossModel::SetPropagationCache(Ptr<SionnaPropagationCache> propagationCache)
{
    m_propagationCache = propagationCache;
}

void
SionnaSpectrumPropagationLossModel::SetFixedChannelMatrix(const MimoChannelMatrix& channel,
                                                          double pathLossDb)
{
    NS_ABORT_MSG_IF(!std::isfinite(pathLossDb), "Fixed path loss must be finite");
    m_fixedChannel = channel;
    m_fixedPathLossDb = pathLossDb;
    m_fixedChannelEnabled = true;
}

void
SionnaSpectrumPropagationLossModel::SetReceiverNoiseFigureDb(double noiseFigureDb)
{
    NS_ABORT_MSG_IF(!std::isfinite(noiseFigureDb) || noiseFigureDb < 0.0,
                    "Receiver noise figure must be finite and non-negative");
    m_receiverNoiseFigureDb = noiseFigureDb;
}

void
SionnaSpectrumPropagationLossModel::SetEesmBeta(uint8_t mcs, double betaLinear)
{
    m_linkAbstraction->SetEesmBeta(mcs, betaLinear);
}

void
SionnaSpectrumPropagationLossModel::SetEesmBetaDb(uint8_t mcs, double betaDb)
{
    m_linkAbstraction->SetEesmBetaDb(mcs, betaDb);
}

void
SionnaSpectrumPropagationLossModel::ConfigureEesmBetas(
    const std::map<uint8_t, double>& betas)
{
    auto linkAbstraction = CreateObject<HeSuMimoLinkAbstraction>();
    for (const auto& [mcs, betaLinear] : betas)
    {
        linkAbstraction->SetEesmBeta(mcs, betaLinear);
    }
    m_linkAbstraction = linkAbstraction;
}

double
SionnaSpectrumPropagationLossModel::GetReceiverNoiseFigureDb() const
{
    return m_receiverNoiseFigureDb;
}

bool
SionnaSpectrumPropagationLossModel::HasEesmBeta(uint8_t mcs) const
{
    return m_linkAbstraction->HasEesmBeta(mcs);
}

double
SionnaSpectrumPropagationLossModel::GetEesmBeta(uint8_t mcs) const
{
    return m_linkAbstraction->GetEesmBeta(mcs);
}

SionnaMimoSpectrumRxResult
SionnaSpectrumPropagationLossModel::CalculateMimoRx(
    const MimoChannelMatrix& channel,
    double receivedPowerW,
    const WifiTxVector& txVector,
    uint32_t packetSizeBytes,
    uint8_t txPhyAntennas,
    uint8_t rxPhyAntennas,
    double bandwidthHz,
    double receiverNoiseFigureDb,
    Ptr<ErrorRateModel> errorRateModel) const
{
    NS_ABORT_MSG_IF(!std::isfinite(receivedPowerW) || receivedPowerW <= 0.0,
                    "Received MIMO power must be finite and positive");
    NS_ABORT_MSG_IF(txVector.GetPreambleType() != WIFI_PREAMBLE_HE_SU,
                    "The minimal MIMO SpectrumWifiPhy path supports HE SU PPDUs only");

    const uint8_t nss = txVector.GetNss();
    NS_ABORT_MSG_IF(nss != channel.GetNumTxAntennas(),
                    "WifiTxVector NSS must equal the Sionna TX dimension");
    NS_ABORT_MSG_IF(txPhyAntennas != channel.GetNumTxAntennas(),
                    "Transmitter PHY antennas must equal the Sionna TX dimension");
    NS_ABORT_MSG_IF(rxPhyAntennas != channel.GetNumRxAntennas(),
                    "Receiver PHY antennas must equal the Sionna RX dimension");
    NS_ABORT_MSG_IF(rxPhyAntennas < nss,
                    "Receiver PHY needs at least one antenna per spatial stream");
    NS_ABORT_MSG_IF(!errorRateModel, "SpectrumWifiPhy ErrorRateModel must be configured");

    const auto linkBudget =
        WifiLinkBudget::Calculate(WToDbm(receivedPowerW),
                                  0.0,
                                  bandwidthHz,
                                  WIFI_NOISE_TEMPERATURE_K,
                                  receiverNoiseFigureDb);
    m_linkAbstraction->SetErrorRateModel(errorRateModel);

    SionnaMimoSpectrumRxResult result;
    result.m_linkResult =
        m_linkAbstraction->Calculate(channel, linkBudget, txVector, packetSizeBytes);

    // InterferenceHelper applies N_rx/N_ss for AWGN models. MMSE already contains
    // this array gain, so reduce the equivalent scalar signal by that exact factor.
    const double diversityGain =
        errorRateModel->IsAwgn() && rxPhyAntennas > nss
            ? static_cast<double>(rxPhyAntennas) / nss
            : 1.0;
    result.m_equivalentReceivedPowerW =
        result.m_linkResult.GetEffectiveSinrLinear() *
        linkBudget.GetReceiverNoisePowerW() / diversityGain;
    NS_ABORT_MSG_IF(!std::isfinite(result.m_equivalentReceivedPowerW) ||
                        result.m_equivalentReceivedPowerW < 0.0,
                    "MIMO abstraction produced invalid equivalent receive power");
    return result;
}



Ptr<SpectrumValue>
SionnaSpectrumPropagationLossModel::DoCalcRxPowerSpectralDensity(Ptr<const SpectrumSignalParameters> params,
                                                Ptr<const MobilityModel> a, Ptr<const MobilityModel> b) const
{
    NS_LOG_FUNCTION(this);
    uint32_t aId = a->GetObject<Node>()->GetId(); // Id of the node a
    uint32_t bId = b->GetObject<Node>()->GetId(); // Id of the node b

    NS_LOG_DEBUG(std::fixed << std::setprecision(9) << Simulator::Now().GetSeconds() <<"s: DoCalcRxPowerSpectralDensity for link " << aId << " - " << bId);

    NS_ASSERT_MSG(aId != bId, "The two nodes must be different from one another");
    NS_ASSERT_MSG(a->GetDistanceFrom(b) > 0.0,
                  "The position of a and b devices cannot be the same");

    // get PSD size
    auto spectrum_bin_size
        = (params->psd->GetSpectrumModel()->Begin()+1)->fc - (params->psd->GetSpectrumModel()->Begin()->fc);
    auto spectrum_dim
        = (params->psd->GetSpectrumModel()->End()-1)->fc - (params->psd->GetSpectrumModel()->Begin()->fc);

    NS_LOG_DEBUG("\t PSD size: " << params->psd->GetValuesN() << " df=" << spectrum_bin_size
        << "Hz, " << "Total=" << spectrum_dim << "Hz");

    Ptr<SpectrumValue> rxPsd = Copy<SpectrumValue>(params->psd);

    NS_ABORT_MSG_IF(!m_fixedChannelEnabled && !m_propagationCache,
                    "SionnaSpectrumPropagationLossModel needs a propagation cache or fixed CFR");

    // The scalar SpectrumChannel loss model already applied this wideband path
    // loss to params->psd. Fetch it only for the diagnostic packet tag.
    const double wbLoss = m_fixedChannelEnabled
                              ? m_fixedPathLossDb
                              : m_propagationCache->GetPropagationLoss(a, b);
    const MimoChannelMatrix channel =
        m_fixedChannelEnabled ? m_fixedChannel
                              : m_propagationCache->GetPropagationChannelMatrix(a, b);
    NS_ABORT_MSG_IF(channel.GetNumSubcarriers() + 1 != params->psd->GetValuesN(),
                    "PSD must contain exactly one more guard bin than the Sionna CFR");

    Ptr<const WifiSpectrumSignalParameters> wifiParams =
        DynamicCast<const WifiSpectrumSignalParameters>(params);
    const bool isHeSu =
        wifiParams && wifiParams->ppdu &&
        wifiParams->ppdu->GetTxVector().GetModeInitialized() &&
        wifiParams->ppdu->GetTxVector().GetMode().GetModulationClass() ==
            WIFI_MOD_CLASS_HE &&
        wifiParams->ppdu->GetTxVector().GetPreambleType() == WIFI_PREAMBLE_HE_SU;
    if (isHeSu)
    {
        NS_ABORT_MSG_IF(wifiParams->ppdu->GetTxVector().GetNss() !=
                            channel.GetNumTxAntennas(),
                        "WifiTxVector NSS must equal the Sionna TX dimension");
    }

    const bool legacySiso = channel.GetNumRxAntennas() == 1 &&
                            channel.GetNumTxAntennas() == 1;
    const bool scalarCompatibilityPath = legacySiso || !isHeSu;
    if (scalarCompatibilityPath)
    {
        // Preserve the established 1x1 path. Non-HE control frames on a MIMO
        // PHY use the first antenna pair because Phase 1 models HE SU payloads only.
        auto value = rxPsd->ValuesBegin();
        for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc, ++value)
        {
            *value *= std::norm(channel.Get(sc, 0, 0));
        }
    }
    else
    {
        NS_ABORT_MSG_IF(!wifiParams || !wifiParams->ppdu,
                        "Dimensioned MIMO CFR requires Wi-Fi signal parameters");

        Ptr<SpectrumWifiPhy> txPhy = GetTransmitterSpectrumWifiPhy(params);
        Ptr<WifiSpectrumPhyInterface> txInterface =
            DynamicCast<WifiSpectrumPhyInterface>(params->txPhy);
        NS_ABORT_MSG_IF(!txInterface, "Wi-Fi transmitter has no spectrum PHY interface");
        Ptr<SpectrumWifiPhy> rxPhy = GetSpectrumWifiPhy(b, txInterface->GetChannel());
        const auto& txVector = wifiParams->ppdu->GetTxVector();
        Ptr<const WifiPsdu> psdu = wifiParams->ppdu->GetPsdu();
        NS_ABORT_MSG_IF(!psdu, "HE SU PPDU does not contain a PSDU");
        NS_ABORT_MSG_IF(txPhy->GetMaxSupportedTxSpatialStreams() < txVector.GetNss(),
                        "Transmitter PHY does not support WifiTxVector NSS");
        NS_ABORT_MSG_IF(rxPhy->GetMaxSupportedRxSpatialStreams() < txVector.GetNss(),
                        "Receiver PHY does not support WifiTxVector NSS");
        NS_ABORT_MSG_IF(!std::isfinite(m_receiverNoiseFigureDb),
                        "Configure the MIMO receiver noise figure explicitly and consistently "
                        "with SpectrumWifiPhy RxNoiseFigure");

        const double rxGainLinear = DbToRatio(rxPhy->GetRxGain());
        const double receivedPowerW = Integral(*params->psd) * rxGainLinear;
        const auto mimoRx = CalculateMimoRx(channel,
                                            receivedPowerW,
                                            txVector,
                                            psdu->GetSize(),
                                            txPhy->GetNumberOfAntennas(),
                                            rxPhy->GetNumberOfAntennas(),
                                            txVector.GetChannelWidth() * 1e6,
                                            m_receiverNoiseFigureDb,
                                            GetErrorRateModel(rxPhy));

        const double equivalentPowerBeforeRxGain =
            mimoRx.GetEquivalentReceivedPowerW() / rxGainLinear;
        const double currentPowerBeforeRxGain = Integral(*rxPsd);
        NS_ABORT_MSG_IF(currentPowerBeforeRxGain <= 0.0,
                        "Path-loss-attenuated MIMO PSD has no power");
        *rxPsd *= equivalentPowerBeforeRxGain / currentPowerBeforeRxGain;

        m_mimoRxTrace(wifiParams->ppdu->GetUid(),
                      aId,
                      bId,
                      mimoRx.GetLinkResult().GetEffectiveSinrLinear(),
                      mimoRx.GetLinkResult().GetPacketErrorRate(),
                      mimoRx.GetLinkResult().GetMimoResult().GetMinimumChannelRank());
    }

    auto tagCoefficients = channel.GetCoefficients();
    if (legacySiso)
    {
        tagCoefficients.emplace_back(1.0, 0.0);
    }

    // tag the packet payload with CFR for later processing in application layer
    if (auto wifiTxParams = DynamicCast<const WifiSpectrumSignalParameters>(params))
    {   // WiFi packet found
        Ptr<WifiPpdu> ppdu = wifiTxParams->ppdu->Copy();
        Ptr<const WifiPsdu> psdu = ppdu->GetPsdu();

        // for each payload
        for (size_t payload_idx = 0; payload_idx < psdu->GetNMpdus(); payload_idx++) {
            Ptr<const Packet> p = psdu->GetPayload(payload_idx);

            CFRTag tag;
            if (p->PeekPacketTag(tag))
            {
                //std::cout << "tag already exists; replace H" << std::endl;
                tag.SetComplexes(tagCoefficients);
                tag.SetPathloss(wbLoss);
            } else
            {
                //std::cout << "add new tag with H" << std::endl;
                tag.SetComplexes(tagCoefficients);
                tag.SetPathloss(wbLoss);
                p->AddPacketTag(tag);
            }
        }
    } else
    {
        // TODO: support of LTE
        NS_LOG_WARN("Unknown SpectrumSignalParameters: " << typeid(params).name() << "; packet not annotated with CSI");
    }

    return rxPsd;
}

} // namespace ns3
