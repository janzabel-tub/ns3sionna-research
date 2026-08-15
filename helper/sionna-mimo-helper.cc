/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "sionna-mimo-helper.h"

#include "sionna-helper.h"
#include "../model/sionna-spectrum-propagation-loss-model.h"
#include "ns3/double.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace ns3
{

namespace
{

constexpr uint32_t WIFI_MAX_ANTENNAS = 8;
constexpr uint8_t HE_MAX_MCS = 11;

[[noreturn]] void
Reject(const std::string& reason)
{
    throw std::invalid_argument("SionnaMimoHelper: " + reason);
}

uint32_t
GetPolarizationCount(SionnaMimoPolarization polarization)
{
    switch (polarization)
    {
    case SionnaMimoPolarization::VERTICAL:
    case SionnaMimoPolarization::HORIZONTAL:
        return 1;
    case SionnaMimoPolarization::DUAL:
        return 2;
    }
    Reject("unknown antenna polarization");
}

std::string
ToSionnaPattern(SionnaMimoAntennaPattern pattern)
{
    switch (pattern)
    {
    case SionnaMimoAntennaPattern::ISOTROPIC:
        return "iso";
    case SionnaMimoAntennaPattern::DIPOLE:
        return "dipole";
    case SionnaMimoAntennaPattern::HALF_WAVE_DIPOLE:
        return "hw_dipole";
    case SionnaMimoAntennaPattern::TR38901:
        return "tr38901";
    }
    Reject("unknown antenna element pattern");
}

std::string
ToSionnaPolarization(SionnaMimoPolarization polarization)
{
    switch (polarization)
    {
    case SionnaMimoPolarization::VERTICAL:
        return "V";
    case SionnaMimoPolarization::HORIZONTAL:
        return "H";
    case SionnaMimoPolarization::DUAL:
        return "VH";
    }
    Reject("unknown antenna polarization");
}

void
ValidatePattern(SionnaMimoAntennaPattern pattern)
{
    switch (pattern)
    {
    case SionnaMimoAntennaPattern::ISOTROPIC:
    case SionnaMimoAntennaPattern::DIPOLE:
    case SionnaMimoAntennaPattern::HALF_WAVE_DIPOLE:
    case SionnaMimoAntennaPattern::TR38901:
        return;
    }
    Reject("unknown antenna element pattern");
}

void
ValidateArray(const SionnaMimoArrayConfiguration& array, const std::string& role)
{
    if (array.numRows == 0 || array.numColumns == 0)
    {
        Reject(role + " array rows and columns must be positive");
    }
    if (!std::isfinite(array.verticalSpacing) || array.verticalSpacing <= 0.0 ||
        !std::isfinite(array.horizontalSpacing) || array.horizontalSpacing <= 0.0)
    {
        Reject(role + " array spacings must be finite and positive");
    }
    ValidatePattern(array.pattern);
    const uint64_t numAntennas = array.GetNumAntennas();
    if (numAntennas > WIFI_MAX_ANTENNAS)
    {
        Reject(role + " array exceeds ns-3's limit of eight Wi-Fi antennas");
    }
}

void
ValidateOrientation(const SionnaMimoOrientation& orientation)
{
    if (!std::isfinite(orientation.azimuthDegrees) ||
        orientation.azimuthDegrees < -180.0 || orientation.azimuthDegrees > 180.0)
    {
        Reject("node azimuth must be finite and in [-180, 180] degrees");
    }
    if (!std::isfinite(orientation.elevationDegrees) ||
        orientation.elevationDegrees < -90.0 || orientation.elevationDegrees > 90.0)
    {
        Reject("node elevation must be finite and in [-90, 90] degrees");
    }
    if (!std::isfinite(orientation.rollDegrees) || orientation.rollDegrees < -180.0 ||
        orientation.rollDegrees > 180.0)
    {
        Reject("node roll must be finite and in [-180, 180] degrees");
    }
}

void
ValidateReceiverType(SionnaMimoReceiverType receiverType)
{
    if (receiverType != SionnaMimoReceiverType::LINEAR_MMSE)
    {
        Reject("only the linear MMSE receiver is supported");
    }
}

void
ValidateNormalization(SionnaMimoChannelNormalization normalization)
{
    if (normalization != SionnaMimoChannelNormalization::UNIT_AVERAGE_PAIR_POWER)
    {
        Reject("only joint unit-average CFR normalization is supported");
    }
}

void
ValidateEffectiveSinrModel(SionnaMimoEffectiveSinrModel model)
{
    if (model != SionnaMimoEffectiveSinrModel::EESM)
    {
        Reject("only EESM is supported for the HE PER mapping");
    }
}

void
ValidateEesmBeta(uint8_t mcs, double betaLinear)
{
    if (mcs > HE_MAX_MCS)
    {
        Reject("EESM beta MCS must be in [0, 11]");
    }
    if (!std::isfinite(betaLinear) || betaLinear <= 0.0)
    {
        Reject("EESM beta must be finite and positive");
    }
}

void
ValidateBeamformingOptions(const SionnaMimoBeamformingConfiguration& beamforming)
{
    switch (beamforming.method)
    {
    case SionnaMimoBeamformingMethod::MRT:
    case SionnaMimoBeamformingMethod::SVD:
        break;
    default:
        Reject("unknown SU beamforming method");
    }

    switch (beamforming.svd.powerAllocation)
    {
    case MimoPowerAllocationMode::EQUAL_POWER:
    case MimoPowerAllocationMode::WATER_FILLING:
        break;
    default:
        Reject("unknown SVD power-allocation mode");
    }
    if (!std::isfinite(beamforming.svd.referenceSnrLinear) ||
        beamforming.svd.referenceSnrLinear <= 0.0)
    {
        Reject("SVD reference SNR must be finite and positive");
    }
    if (!std::isfinite(beamforming.svd.noisePower) ||
        beamforming.svd.noisePower <= 0.0)
    {
        Reject("SVD noise power must be finite and positive");
    }
    if (!std::isfinite(beamforming.svd.rankTolerance) ||
        beamforming.svd.rankTolerance <= 0.0 || beamforming.svd.rankTolerance >= 1.0)
    {
        Reject("SVD rank tolerance must be finite and in (0, 1)");
    }

    switch (beamforming.feedback.standard)
    {
    case MimoFeedbackStandard::HE:
    case MimoFeedbackStandard::EHT:
        break;
    default:
        Reject("unknown feedback standard");
    }
    switch (beamforming.feedback.codebook)
    {
    case MimoFeedbackCodebook::UNQUANTIZED:
    case MimoFeedbackCodebook::SU_COARSE:
    case MimoFeedbackCodebook::SU_FINE:
        break;
    default:
        Reject("unknown SU feedback codebook");
    }

    if (beamforming.csiTiming.updateInterval < NanoSeconds(0) ||
        beamforming.csiTiming.feedbackDelay < NanoSeconds(0))
    {
        Reject("CSI update interval and feedback delay must be non-negative");
    }
    const bool hasAging = !beamforming.csiTiming.updateInterval.IsZero() ||
                          !beamforming.csiTiming.feedbackDelay.IsZero();

    if (beamforming.method == SionnaMimoBeamformingMethod::MRT)
    {
        if (beamforming.feedback.standard != MimoFeedbackStandard::HE ||
            beamforming.feedback.codebook != MimoFeedbackCodebook::UNQUANTIZED ||
            hasAging)
        {
            Reject("MRT supports only unquantized current perfect CSI");
        }
        if (beamforming.svd.powerAllocation != MimoPowerAllocationMode::EQUAL_POWER ||
            beamforming.svd.referenceSnrLinear != 1.0 ||
            beamforming.svd.noisePower != 1.0 ||
            beamforming.svd.rankTolerance != MimoSvdPrecodingOptions::DEFAULT_RANK_TOLERANCE)
        {
            Reject("MRT does not accept SVD or water-filling options");
        }
    }
    else if (beamforming.svd.powerAllocation == MimoPowerAllocationMode::WATER_FILLING &&
             (beamforming.feedback.codebook != MimoFeedbackCodebook::UNQUANTIZED ||
              hasAging))
    {
        Reject("water-filling cannot be combined with quantized feedback or CSI aging");
    }
}

MimoCsiArrayIdentity
MakeCsiArrayIdentity(const SionnaMimoArrayConfiguration& array,
                     const SionnaMimoOrientation& orientation)
{
    MimoCsiArrayIdentity identity;
    identity.numRows = array.numRows;
    identity.numColumns = array.numColumns;
    identity.verticalSpacing = array.verticalSpacing;
    identity.horizontalSpacing = array.horizontalSpacing;
    identity.pattern = ToSionnaPattern(array.pattern);
    identity.polarization = ToSionnaPolarization(array.polarization);
    identity.azimuthDegrees = orientation.azimuthDegrees;
    identity.elevationDegrees = orientation.elevationDegrees;
    identity.rollDegrees = orientation.rollDegrees;
    return identity;
}

} // namespace

uint64_t
SionnaMimoArrayConfiguration::GetNumAntennas() const
{
    const uint64_t numElements = static_cast<uint64_t>(numRows) * numColumns;
    const uint32_t numPolarizations = GetPolarizationCount(polarization);
    if (numElements > std::numeric_limits<uint64_t>::max() / numPolarizations)
    {
        Reject("array antenna-port count overflows uint64_t");
    }
    return numElements * numPolarizations;
}

SionnaMimoPrecodingSelection::SionnaMimoPrecodingSelection(
    SionnaMimoBeamformingMethod method,
    MimoPrecodingMatrix precoding,
    Time csiTimestamp,
    Time transmissionTimestamp)
    : m_method(method),
      m_precoding(std::move(precoding)),
      m_csiTimestamp(csiTimestamp),
      m_transmissionTimestamp(transmissionTimestamp)
{
}

SionnaMimoPrecodingSelection::SionnaMimoPrecodingSelection(
    MimoSvdPrecodingResult svd,
    Time csiTimestamp,
    Time transmissionTimestamp)
    : m_method(SionnaMimoBeamformingMethod::SVD),
      m_precoding(svd.GetPrecodingMatrix()),
      m_csiTimestamp(csiTimestamp),
      m_transmissionTimestamp(transmissionTimestamp),
      m_svd(std::move(svd))
{
}

SionnaMimoPrecodingSelection::SionnaMimoPrecodingSelection(
    MimoCsiSelectionResult csiSelection)
    : m_method(SionnaMimoBeamformingMethod::SVD),
      m_precoding(csiSelection.GetReconstruction().GetPrecodingMatrix()),
      m_csiTimestamp(csiSelection.GetCsiTimestamp()),
      m_transmissionTimestamp(csiSelection.GetTransmissionTimestamp()),
      m_csiSelection(std::move(csiSelection))
{
}

SionnaMimoBeamformingMethod
SionnaMimoPrecodingSelection::GetMethod() const
{
    return m_method;
}

const MimoPrecodingMatrix&
SionnaMimoPrecodingSelection::GetPrecodingMatrix() const
{
    return m_precoding;
}

Time
SionnaMimoPrecodingSelection::GetCsiTimestamp() const
{
    return m_csiTimestamp;
}

Time
SionnaMimoPrecodingSelection::GetTransmissionTimestamp() const
{
    return m_transmissionTimestamp;
}

Time
SionnaMimoPrecodingSelection::GetCsiAge() const
{
    return m_transmissionTimestamp - m_csiTimestamp;
}

bool
SionnaMimoPrecodingSelection::HasSvdResult() const
{
    return m_svd.has_value() || m_csiSelection.has_value();
}

const MimoSvdPrecodingResult&
SionnaMimoPrecodingSelection::GetSvdResult() const
{
    if (m_csiSelection)
    {
        return m_csiSelection->GetIdealSvdResult();
    }
    if (!m_svd)
    {
        Reject("selected MRT precoding has no SVD diagnostics");
    }
    return *m_svd;
}

bool
SionnaMimoPrecodingSelection::HasCsiSelectionResult() const
{
    return m_csiSelection.has_value();
}

const MimoCsiSelectionResult&
SionnaMimoPrecodingSelection::GetCsiSelectionResult() const
{
    if (!m_csiSelection)
    {
        Reject("selected precoding has no SVD feedback or CSI-aging diagnostics");
    }
    return *m_csiSelection;
}

double
SionnaMimoPrecodingSelection::GetPowerFraction(uint32_t subcarrier,
                                                uint32_t stream) const
{
    if (subcarrier >= m_precoding.GetNumSubcarriers() ||
        stream >= m_precoding.GetNumStreams())
    {
        Reject("precoding diagnostic index is out of range");
    }

    double streamPower = 0.0;
    for (uint32_t tx = 0; tx < m_precoding.GetNumTxAntennas(); ++tx)
    {
        streamPower += std::norm(m_precoding.Get(subcarrier, tx, stream));
    }
    return streamPower / m_precoding.GetNumStreams();
}

SionnaMimoHelper::SionnaMimoHelper()
{
    Validate(m_configuration);
}

SionnaMimoHelper::SionnaMimoHelper(const SionnaMimoConfiguration& configuration)
{
    SetConfiguration(configuration);
}

const SionnaMimoConfiguration&
SionnaMimoHelper::GetConfiguration() const
{
    return m_configuration;
}

void
SionnaMimoHelper::SetConfiguration(const SionnaMimoConfiguration& configuration)
{
    Validate(configuration);
    m_configuration = configuration;
    m_selectedPrecoding.reset();
}

void
SionnaMimoHelper::Apply(
    SionnaHelper& sionnaHelper,
    SpectrumWifiPhyHelper& txPhyHelper,
    SpectrumWifiPhyHelper& rxPhyHelper,
    Ptr<SionnaSpectrumPropagationLossModel> spectrumModel) const
{
    Validate(m_configuration);
    if (m_configuration.mode == SionnaMimoMode::SU_BEAMFORMING && !m_selectedPrecoding)
    {
        Reject("SU beamforming must select a channel-dependent precoder before Apply");
    }
    if (!spectrumModel)
    {
        Reject("SpectrumWifiPhy propagation model must not be null");
    }

    const auto& config = m_configuration;
    sionnaHelper.ConfigureTransmitArray(
        config.txArray.numRows,
        config.txArray.numColumns,
        config.txArray.verticalSpacing,
        config.txArray.horizontalSpacing,
        ToSionnaPattern(config.txArray.pattern),
        ToSionnaPolarization(config.txArray.polarization));
    sionnaHelper.ConfigureReceiveArray(
        config.rxArray.numRows,
        config.rxArray.numColumns,
        config.rxArray.verticalSpacing,
        config.rxArray.horizontalSpacing,
        ToSionnaPattern(config.rxArray.pattern),
        ToSionnaPolarization(config.rxArray.polarization));

    sionnaHelper.ClearNodeOrientations();
    for (const auto& [nodeId, orientation] : config.nodeOrientations)
    {
        sionnaHelper.ConfigureNodeOrientation(
            nodeId,
            orientation.azimuthDegrees,
            orientation.elevationDegrees,
            orientation.rollDegrees);
    }

    auto configurePhy = [&config](SpectrumWifiPhyHelper& phyHelper, uint32_t numAntennas) {
        phyHelper.Set("Antennas", UintegerValue(numAntennas));
        phyHelper.Set("MaxSupportedTxSpatialStreams", UintegerValue(config.maxNss));
        phyHelper.Set("MaxSupportedRxSpatialStreams", UintegerValue(config.maxNss));
        phyHelper.Set("RxNoiseFigure", DoubleValue(config.receiverNoiseFigureDb));
    };
    configurePhy(txPhyHelper, config.numTxAntennas);
    configurePhy(rxPhyHelper, config.numRxAntennas);

    spectrumModel->SetReceiverNoiseFigureDb(config.receiverNoiseFigureDb);
    spectrumModel->ConfigureEesmBetas(config.eesmBetaLinear);
    if (m_selectedPrecoding)
    {
        spectrumModel->SetPrecodingMatrix(m_selectedPrecoding->GetPrecodingMatrix());
    }
    else if (config.precodingMatrix)
    {
        spectrumModel->SetPrecodingMatrix(*config.precodingMatrix);
    }
    else
    {
        spectrumModel->ClearPrecodingMatrix();
    }
}

void
SionnaMimoHelper::ApplyForHeMcs(
    uint8_t heMcs,
    SionnaHelper& sionnaHelper,
    SpectrumWifiPhyHelper& txPhyHelper,
    SpectrumWifiPhyHelper& rxPhyHelper,
    Ptr<SionnaSpectrumPropagationLossModel> spectrumModel) const
{
    ValidateForHeMcs(m_configuration, heMcs);
    Apply(sionnaHelper, txPhyHelper, rxPhyHelper, spectrumModel);
}

void
SionnaMimoHelper::SetNodeOrientation(uint32_t nodeId,
                                     const SionnaMimoOrientation& orientation)
{
    ValidateOrientation(orientation);
    m_configuration.nodeOrientations[nodeId] = orientation;
    m_selectedPrecoding.reset();
}

void
SionnaMimoHelper::ClearNodeOrientation(uint32_t nodeId)
{
    m_configuration.nodeOrientations.erase(nodeId);
    m_selectedPrecoding.reset();
}

bool
SionnaMimoHelper::HasNodeOrientation(uint32_t nodeId) const
{
    return m_configuration.nodeOrientations.count(nodeId) != 0;
}

SionnaMimoOrientation
SionnaMimoHelper::GetNodeOrientation(uint32_t nodeId) const
{
    const auto orientation = m_configuration.nodeOrientations.find(nodeId);
    return orientation == m_configuration.nodeOrientations.end() ? SionnaMimoOrientation{}
                                                                  : orientation->second;
}

void
SionnaMimoHelper::SetEesmBeta(uint8_t mcs, double betaLinear)
{
    ValidateEesmBeta(mcs, betaLinear);
    m_configuration.eesmBetaLinear[mcs] = betaLinear;
}

bool
SionnaMimoHelper::HasEesmBeta(uint8_t mcs) const
{
    return m_configuration.eesmBetaLinear.count(mcs) != 0;
}

double
SionnaMimoHelper::GetEesmBeta(uint8_t mcs) const
{
    const auto beta = m_configuration.eesmBetaLinear.find(mcs);
    if (beta == m_configuration.eesmBetaLinear.end())
    {
        Reject("HE MCS " + std::to_string(mcs) + " has no explicit EESM beta");
    }
    return beta->second;
}

void
SionnaMimoHelper::SetPrecodingMatrix(const MimoPrecodingMatrix& precoding)
{
    SionnaMimoConfiguration candidate = m_configuration;
    candidate.precodingMatrix = precoding;
    Validate(candidate);
    m_configuration = candidate;
}

void
SionnaMimoHelper::ClearPrecodingMatrix()
{
    SionnaMimoConfiguration candidate = m_configuration;
    candidate.precodingMatrix.reset();
    Validate(candidate);
    m_configuration = candidate;
}

bool
SionnaMimoHelper::HasPrecodingMatrix() const
{
    return m_configuration.precodingMatrix.has_value();
}

const MimoPrecodingMatrix&
SionnaMimoHelper::GetPrecodingMatrix() const
{
    if (!m_configuration.precodingMatrix)
    {
        Reject("no fixed precoding matrix is configured");
    }
    return *m_configuration.precodingMatrix;
}

const SionnaMimoPrecodingSelection&
SionnaMimoHelper::SelectPrecoding(const MimoChannelMatrix& csiTimeChannelEstimate,
                                  uint32_t txNodeId,
                                  uint32_t rxNodeId,
                                  Time csiTimestamp,
                                  Time transmissionTimestamp)
{
    Validate(m_configuration);
    if (m_configuration.mode != SionnaMimoMode::SU_BEAMFORMING ||
        !m_configuration.beamforming)
    {
        Reject("channel-dependent precoding requires SU_BEAMFORMING configuration");
    }
    if (csiTimeChannelEstimate.GetNumTxAntennas() != m_configuration.numTxAntennas ||
        csiTimeChannelEstimate.GetNumRxAntennas() != m_configuration.numRxAntennas)
    {
        Reject("CSI matrix dimensions must match the configured TX and RX antennas");
    }

    const auto& beamforming = *m_configuration.beamforming;
    std::optional<SionnaMimoPrecodingSelection> candidate;
    if (beamforming.method == SionnaMimoBeamformingMethod::MRT)
    {
        if (csiTimestamp != transmissionTimestamp)
        {
            Reject("MRT requires current perfect CSI with zero age");
        }
        candidate = SionnaMimoPrecodingSelection(
            SionnaMimoBeamformingMethod::MRT,
            MimoPrecodingMatrix::MaximumRatioTransmission(csiTimeChannelEstimate),
            csiTimestamp,
            transmissionTimestamp);
    }
    else if (beamforming.svd.powerAllocation == MimoPowerAllocationMode::WATER_FILLING)
    {
        const Time expectedTimestamp =
            beamforming.csiTiming.ResolveCsiTimestamp(transmissionTimestamp);
        if (csiTimestamp != expectedTimestamp)
        {
            Reject("CSI timestamp does not match update interval and feedback delay");
        }
        candidate = SionnaMimoPrecodingSelection(
            MimoPrecodingMatrix::SingularValueDecomposition(csiTimeChannelEstimate,
                                                            m_configuration.maxNss,
                                                            beamforming.svd),
            csiTimestamp,
            transmissionTimestamp);
    }
    else
    {
        MimoCsiSelectionConfiguration selection;
        selection.txNodeId = txNodeId;
        selection.rxNodeId = rxNodeId;
        selection.txArray = MakeCsiArrayIdentity(m_configuration.txArray,
                                                 GetNodeOrientation(txNodeId));
        selection.rxArray = MakeCsiArrayIdentity(m_configuration.rxArray,
                                                 GetNodeOrientation(rxNodeId));
        selection.numStreams = m_configuration.maxNss;
        selection.feedback = beamforming.feedback;
        selection.timing = beamforming.csiTiming;
        selection.svd = beamforming.svd;
        candidate = SionnaMimoPrecodingSelection(
            MimoCsiAgingModel::Select(csiTimeChannelEstimate,
                                      csiTimestamp,
                                      transmissionTimestamp,
                                      selection));
    }

    m_selectedPrecoding = std::move(candidate);
    return *m_selectedPrecoding;
}

void
SionnaMimoHelper::ClearSelectedPrecoding()
{
    m_selectedPrecoding.reset();
}

bool
SionnaMimoHelper::HasSelectedPrecoding() const
{
    return m_selectedPrecoding.has_value();
}

const SionnaMimoPrecodingSelection&
SionnaMimoHelper::GetSelectedPrecoding() const
{
    if (!m_selectedPrecoding)
    {
        Reject("no channel-dependent precoding has been selected");
    }
    return *m_selectedPrecoding;
}

const std::array<SionnaMimoMode, 3>&
SionnaMimoHelper::GetSupportedModes()
{
    static const std::array<SionnaMimoMode, 3> modes{
        SionnaMimoMode::SISO,
        SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING,
        SionnaMimoMode::SU_BEAMFORMING};
    return modes;
}

bool
SionnaMimoHelper::IsModeSupported(SionnaMimoMode mode)
{
    const auto& supportedModes = GetSupportedModes();
    return std::find(supportedModes.begin(), supportedModes.end(), mode) != supportedModes.end();
}

void
SionnaMimoHelper::Validate(const SionnaMimoConfiguration& configuration)
{
    if (!IsModeSupported(configuration.mode))
    {
        Reject("only SISO, SU-MIMO spatial multiplexing, and SU beamforming are supported");
    }

    ValidateArray(configuration.txArray, "TX");
    ValidateArray(configuration.rxArray, "RX");

    if (configuration.numTxAntennas == 0 ||
        configuration.numTxAntennas > WIFI_MAX_ANTENNAS ||
        configuration.numRxAntennas == 0 ||
        configuration.numRxAntennas > WIFI_MAX_ANTENNAS)
    {
        Reject("TX and RX antenna counts must be in [1, 8]");
    }
    if (configuration.txArray.GetNumAntennas() != configuration.numTxAntennas)
    {
        Reject("TX antenna count must match the TX array geometry and polarization");
    }
    if (configuration.rxArray.GetNumAntennas() != configuration.numRxAntennas)
    {
        Reject("RX antenna count must match the RX array geometry and polarization");
    }
    if (configuration.maxNss == 0 || configuration.maxNss > WIFI_MAX_ANTENNAS)
    {
        Reject("maximum NSS must be in [1, 8]");
    }
    if (configuration.maxNss > configuration.numTxAntennas ||
        configuration.maxNss > configuration.numRxAntennas)
    {
        Reject("maximum NSS cannot exceed the TX or RX antenna count");
    }

    if (configuration.mode == SionnaMimoMode::SISO)
    {
        if (configuration.numTxAntennas != 1 || configuration.numRxAntennas != 1 ||
            configuration.maxNss != 1)
        {
            Reject("SISO requires one TX antenna, one RX antenna, and maximum NSS one");
        }
        if (configuration.beamforming)
        {
            Reject("SISO cannot configure SU beamforming");
        }
    }
    else if (configuration.mode == SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING)
    {
        if (configuration.beamforming)
        {
            Reject("spatial multiplexing cannot configure channel-dependent beamforming");
        }
        if (configuration.numTxAntennas < 2)
        {
            Reject("SU-MIMO spatial multiplexing requires at least two TX antennas");
        }
        if (!configuration.precodingMatrix &&
            configuration.numRxAntennas < configuration.numTxAntennas)
        {
            Reject("open-loop SU-MIMO requires at least one RX antenna per TX stream");
        }
        if (!configuration.precodingMatrix &&
            configuration.maxNss != configuration.numTxAntennas)
        {
            Reject("open-loop SU-MIMO uses one stream per TX antenna");
        }
    }
    else
    {
        if (configuration.numTxAntennas < 2)
        {
            Reject("SU beamforming requires at least two TX antennas");
        }
        if (configuration.precodingMatrix)
        {
            Reject("SU beamforming selects W[k]; fixed precoding must use spatial multiplexing");
        }
        if (!configuration.beamforming)
        {
            Reject("SU beamforming requires an explicit MRT or SVD configuration");
        }

        ValidateBeamformingOptions(*configuration.beamforming);
        if (configuration.beamforming->method == SionnaMimoBeamformingMethod::MRT &&
            (configuration.numRxAntennas != 1 || configuration.maxNss != 1))
        {
            Reject("MRT requires exactly one RX antenna and one spatial stream");
        }
    }

    if (configuration.precodingMatrix)
    {
        const auto& precoding = *configuration.precodingMatrix;
        if (precoding.GetNumTxAntennas() != configuration.numTxAntennas)
        {
            Reject("precoding TX dimension must match the configured TX antennas");
        }
        if (precoding.GetNumStreams() != configuration.maxNss)
        {
            Reject("maximum NSS must equal the fixed-precoding stream dimension");
        }
        if (precoding.GetNumStreams() > configuration.numRxAntennas)
        {
            Reject("fixed-precoding streams cannot exceed the RX antennas");
        }
    }

    ValidateReceiverType(configuration.receiverType);
    ValidateNormalization(configuration.channelNormalization);
    ValidateEffectiveSinrModel(configuration.effectiveSinrModel);

    if (!std::isfinite(configuration.receiverNoiseFigureDb) ||
        configuration.receiverNoiseFigureDb < 0.0)
    {
        Reject("receiver noise figure must be finite and non-negative");
    }

    for (const auto& [mcs, betaLinear] : configuration.eesmBetaLinear)
    {
        ValidateEesmBeta(mcs, betaLinear);
    }
    for (const auto& [nodeId, orientation] : configuration.nodeOrientations)
    {
        static_cast<void>(nodeId);
        ValidateOrientation(orientation);
    }
}

void
SionnaMimoHelper::ValidateForHeMcs(const SionnaMimoConfiguration& configuration, uint8_t mcs)
{
    Validate(configuration);
    if (mcs > HE_MAX_MCS)
    {
        Reject("HE MCS must be in [0, 11]");
    }
    if (configuration.mode == SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING &&
        configuration.eesmBetaLinear.count(mcs) == 0)
    {
        Reject("open-loop SU-MIMO requires an explicit EESM beta for HE MCS " +
               std::to_string(mcs));
    }
    else if (configuration.mode == SionnaMimoMode::SU_BEAMFORMING &&
             configuration.eesmBetaLinear.count(mcs) == 0)
    {
        Reject("SU beamforming requires an explicit EESM beta for HE MCS " +
               std::to_string(mcs));
    }
}

} // namespace ns3
