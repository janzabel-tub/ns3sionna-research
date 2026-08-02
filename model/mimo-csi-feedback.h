/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef MIMO_CSI_FEEDBACK_H
#define MIMO_CSI_FEEDBACK_H

#include "mimo-precoding-matrix.h"
#include "su-mimo-link-model.h"

#include "ns3/nstime.h"

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace ns3
{

/** Standard family whose SU compressed-beamforming representation is used. */
enum class MimoFeedbackStandard
{
    HE,
    EHT
};

/**
 * Supported SU angle resolutions. The two finite codebooks use (phi, psi)
 * widths (4, 2) and (6, 4), respectively.
 */
enum class MimoFeedbackCodebook
{
    UNQUANTIZED,
    SU_COARSE,
    SU_FINE
};

/** Configuration of the semantic HE/EHT SU compressed feedback matrix. */
struct MimoFeedbackConfiguration
{
    MimoFeedbackStandard standard{MimoFeedbackStandard::HE};
    MimoFeedbackCodebook codebook{MimoFeedbackCodebook::UNQUANTIZED};
};

class MimoFeedbackReconstruction;

/**
 * \ingroup sionna
 *
 * Quantized HE/EHT SU compressed-beamforming angles for every subcarrier.
 *
 * Angles use the standard Givens order: for i = 0, ..., min(N_s,N_tx-1)-1,
 * phi(i,i), ..., phi(N_tx-2,i), followed by psi(i+1,i), ...,
 * psi(N_tx-1,i). Integer arrays are subcarrier-major within each angle type.
 * This value type models angle feedback, not MAC framing, CQI, segmentation,
 * or subcarrier grouping.
 */
class MimoCompressedBeamformingFeedback
{
  public:
    /** Encode a semi-unitary ideal precoder into the selected SU codebook. */
    static MimoCompressedBeamformingFeedback Encode(
        const MimoPrecodingMatrix& idealPrecoding,
        const MimoFeedbackConfiguration& configuration = {});

    /**
     * Build a bit-exact finite-codebook representation from angle indices.
     *
     * The index vectors must each contain N_sc times the number of angles of
     * their type, in the documented subcarrier-major order.
     */
    static MimoCompressedBeamformingFeedback FromQuantizedAngles(
        uint32_t numSubcarriers,
        uint32_t numTxAntennas,
        uint32_t numStreams,
        const MimoFeedbackConfiguration& configuration,
        const std::vector<uint16_t>& phiIndices,
        const std::vector<uint16_t>& psiIndices);

    /** Deterministically reconstruct and power-renormalize fixed W[k]. */
    MimoFeedbackReconstruction Reconstruct() const;

    MimoFeedbackConfiguration GetConfiguration() const;
    uint32_t GetNumSubcarriers() const;
    uint32_t GetNumTxAntennas() const;
    uint32_t GetNumStreams() const;
    uint32_t GetNumPhiAnglesPerSubcarrier() const;
    uint32_t GetNumPsiAnglesPerSubcarrier() const;
    uint8_t GetPhiBitWidth() const;
    uint8_t GetPsiBitWidth() const;
    uint16_t GetPhiIndex(uint32_t subcarrier, uint32_t angle) const;
    uint16_t GetPsiIndex(uint32_t subcarrier, uint32_t angle) const;

    /** Angle payload only; PHY/MAC headers and CQI fields are excluded. */
    uint64_t GetFeedbackBitCount() const;

  private:
    MimoCompressedBeamformingFeedback(
        uint32_t numSubcarriers,
        uint32_t numTxAntennas,
        uint32_t numStreams,
        MimoFeedbackConfiguration configuration,
        std::vector<uint16_t> phiIndices,
        std::vector<uint16_t> psiIndices,
        std::vector<std::complex<double>> unquantizedCoefficients);

    std::size_t GetFlatAngleIndex(uint32_t subcarrier,
                                  uint32_t angle,
                                  uint32_t anglesPerSubcarrier) const;

    uint32_t m_numSubcarriers;
    uint32_t m_numTxAntennas;
    uint32_t m_numStreams;
    MimoFeedbackConfiguration m_configuration;
    std::vector<uint16_t> m_phiIndices;
    std::vector<uint16_t> m_psiIndices;
    std::vector<std::complex<double>> m_unquantizedCoefficients;
};

/** Fixed W[k] reconstructed from feedback, with power-renormalization data. */
class MimoFeedbackReconstruction
{
  public:
    const MimoPrecodingMatrix& GetPrecodingMatrix() const;
    double GetPowerBeforeNormalization(uint32_t subcarrier) const;
    double GetNormalizationFactor(uint32_t subcarrier) const;
    double GetPowerAfterNormalization(uint32_t subcarrier) const;

  private:
    friend class MimoCompressedBeamformingFeedback;

    MimoFeedbackReconstruction(MimoPrecodingMatrix precoding,
                               std::vector<double> powerBeforeNormalization,
                               std::vector<double> normalizationFactors);

    MimoPrecodingMatrix m_precoding;
    std::vector<double> m_powerBeforeNormalization;
    std::vector<double> m_normalizationFactors;
};

/** Quantization-error and feedback-size diagnostics for one ideal/rebuilt pair. */
class MimoFeedbackDiagnostics
{
  public:
    static MimoFeedbackDiagnostics Calculate(
        const MimoPrecodingMatrix& idealPrecoding,
        const MimoCompressedBeamformingFeedback& feedback,
        const MimoFeedbackReconstruction& reconstruction);

    uint64_t GetFeedbackBitCount() const;
    double GetChordalError(uint32_t subcarrier) const;
    double GetRmsCoefficientError(uint32_t subcarrier) const;
    double GetMeanChordalError() const;
    double GetMeanRmsCoefficientError() const;

  private:
    MimoFeedbackDiagnostics(uint64_t feedbackBitCount,
                            std::vector<double> chordalErrors,
                            std::vector<double> rmsCoefficientErrors);

    uint64_t m_feedbackBitCount;
    std::vector<double> m_chordalErrors;
    std::vector<double> m_rmsCoefficientErrors;
};

/** Array and orientation fields that make one CSI state unambiguous. */
struct MimoCsiArrayIdentity
{
    uint32_t numRows{1};
    uint32_t numColumns{1};
    double verticalSpacing{0.5};
    double horizontalSpacing{0.5};
    std::string pattern;
    std::string polarization{"V"};
    double azimuthDegrees{0.0};
    double elevationDegrees{0.0};
    double rollDegrees{0.0};

    bool operator==(const MimoCsiArrayIdentity& other) const;
    bool operator<(const MimoCsiArrayIdentity& other) const;
};

/** Precoder-selection algorithm represented by a CSI state. */
enum class MimoCsiPrecodingMode
{
    SVD_EQUAL_POWER
};

/**
 * Directed identity for a precoder-selection state. Reversing TX/RX, changing
 * either array, the mode, feedback profile, or CSI timestamp changes identity.
 */
struct MimoCsiStateIdentity
{
    uint32_t txNodeId{0};
    uint32_t rxNodeId{0};
    MimoCsiArrayIdentity txArray;
    MimoCsiArrayIdentity rxArray;
    MimoCsiPrecodingMode precodingMode{MimoCsiPrecodingMode::SVD_EQUAL_POWER};
    MimoFeedbackConfiguration feedback;
    Time csiTimestamp{NanoSeconds(0)};

    bool operator==(const MimoCsiStateIdentity& other) const;
    bool operator<(const MimoCsiStateIdentity& other) const;
};

/** Periodic CSI sampling and feedback-availability timing. */
struct MimoCsiTimingConfiguration
{
    /** Zero means continuous sampling; otherwise samples lie on this time grid. */
    Time updateInterval{NanoSeconds(0)};
    /** Time from channel estimation until that estimate is available for TX. */
    Time feedbackDelay{NanoSeconds(0)};

    /** Newest CSI timestamp whose feedback is available at transmissionTime. */
    Time ResolveCsiTimestamp(Time transmissionTime) const;
};

/** Inputs for deterministic CSI-time SVD selection. */
struct MimoCsiSelectionConfiguration
{
    uint32_t txNodeId{0};
    uint32_t rxNodeId{0};
    MimoCsiArrayIdentity txArray;
    MimoCsiArrayIdentity rxArray;
    MimoCsiPrecodingMode precodingMode{MimoCsiPrecodingMode::SVD_EQUAL_POWER};
    uint32_t numStreams{1};
    MimoFeedbackConfiguration feedback;
    MimoCsiTimingConfiguration timing;
    MimoSvdPrecodingOptions svd;
};

/** Immutable CSI-time selection and feedback result. */
class MimoCsiSelectionResult
{
  public:
    const MimoSvdPrecodingResult& GetIdealSvdResult() const;
    const MimoCompressedBeamformingFeedback& GetFeedback() const;
    const MimoFeedbackReconstruction& GetReconstruction() const;
    const MimoFeedbackDiagnostics& GetDiagnostics() const;
    const MimoCsiStateIdentity& GetStateIdentity() const;
    Time GetCsiTimestamp() const;
    Time GetTransmissionTimestamp() const;
    Time GetCsiAge() const;
    Time GetUpdateInterval() const;
    Time GetFeedbackDelay() const;

    /**
     * Evaluate the selected fixed W[k] against an explicitly supplied
     * transmission-time CFR. The CSI-time CFR is never reused here.
     */
    SuMimoLinkResult CalculateLink(const MimoChannelMatrix& transmissionTimeChannel,
                                   double referenceSnrLinear) const;

  private:
    friend class MimoCsiAgingModel;

    MimoCsiSelectionResult(MimoSvdPrecodingResult idealSvd,
                           MimoCompressedBeamformingFeedback feedback,
                           MimoFeedbackReconstruction reconstruction,
                           MimoFeedbackDiagnostics diagnostics,
                           MimoCsiStateIdentity identity,
                           Time transmissionTimestamp,
                           Time updateInterval,
                           Time feedbackDelay);

    MimoSvdPrecodingResult m_idealSvd;
    MimoCompressedBeamformingFeedback m_feedback;
    MimoFeedbackReconstruction m_reconstruction;
    MimoFeedbackDiagnostics m_diagnostics;
    MimoCsiStateIdentity m_identity;
    Time m_transmissionTimestamp;
    Time m_updateInterval;
    Time m_feedbackDelay;
};

/** Deterministic CSI-time selection, quantization, and aging contract. */
class MimoCsiAgingModel
{
  public:
    static MimoCsiSelectionResult Select(
        const MimoChannelMatrix& csiTimeChannelEstimate,
        Time csiTimestamp,
        Time transmissionTimestamp,
        const MimoCsiSelectionConfiguration& configuration = {});
};

} // namespace ns3

#endif // MIMO_CSI_FEEDBACK_H
