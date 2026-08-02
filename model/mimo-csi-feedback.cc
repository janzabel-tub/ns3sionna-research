/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "mimo-csi-feedback.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ns3
{

namespace
{

constexpr double PI = 3.141592653589793238462643383279502884;
constexpr double SEMI_UNITARY_TOLERANCE = 1e-10;
constexpr double ANGLE_EXTRACTION_TOLERANCE = 1e-12;

[[noreturn]] void
Reject(const std::string& reason)
{
    throw std::invalid_argument("MimoCsiFeedback: " + reason);
}

uint32_t
GetAnglesPerType(uint32_t numTxAntennas, uint32_t numStreams)
{
    const uint32_t stages = std::min(numStreams, numTxAntennas - 1);
    uint32_t count = 0;
    for (uint32_t stage = 0; stage < stages; ++stage)
    {
        count += numTxAntennas - stage - 1;
    }
    return count;
}

std::pair<uint8_t, uint8_t>
GetBitWidths(MimoFeedbackCodebook codebook)
{
    switch (codebook)
    {
    case MimoFeedbackCodebook::UNQUANTIZED:
        return {0, 0};
    case MimoFeedbackCodebook::SU_COARSE:
        return {4, 2};
    case MimoFeedbackCodebook::SU_FINE:
        return {6, 4};
    }
    Reject("unknown feedback codebook");
}

void
ValidateFeedbackDimensions(uint32_t numSubcarriers,
                           uint32_t numTxAntennas,
                           uint32_t numStreams,
                           const MimoFeedbackConfiguration& configuration)
{
    if (numSubcarriers == 0 || numTxAntennas == 0 || numStreams == 0)
    {
        Reject("subcarrier, TX-antenna, and stream counts must be positive");
    }
    if (numStreams > numTxAntennas)
    {
        Reject("feedback stream count cannot exceed the TX-antenna count");
    }
    if (configuration.standard != MimoFeedbackStandard::HE &&
        configuration.standard != MimoFeedbackStandard::EHT)
    {
        Reject("unknown feedback standard family");
    }
    static_cast<void>(GetBitWidths(configuration.codebook));
    const uint32_t maximumAntennas =
        configuration.standard == MimoFeedbackStandard::HE ? 8 : 16;
    if (numTxAntennas > maximumAntennas)
    {
        Reject(configuration.standard == MimoFeedbackStandard::HE
                   ? "HE compressed feedback supports at most 8 TX antennas"
                   : "EHT compressed feedback supports at most 16 TX antennas");
    }
}

double
NormalizePhi(double angle)
{
    double normalized = std::fmod(angle, 2.0 * PI);
    if (normalized < 0.0)
    {
        normalized += 2.0 * PI;
    }
    if (normalized >= 2.0 * PI)
    {
        normalized = 0.0;
    }
    return normalized;
}

uint16_t
QuantizePhi(double angle, uint8_t bits)
{
    const uint32_t levels = uint32_t{1} << bits;
    const double step = 2.0 * PI / levels;
    const uint32_t index = static_cast<uint32_t>(std::floor(NormalizePhi(angle) / step));
    return static_cast<uint16_t>(std::min(index, levels - 1));
}

uint16_t
QuantizePsi(double angle, uint8_t bits)
{
    const uint32_t levels = uint32_t{1} << bits;
    const double step = (PI / 2.0) / levels;
    const double bounded = std::clamp(angle, 0.0, PI / 2.0);
    const uint32_t index = static_cast<uint32_t>(std::floor(bounded / step));
    return static_cast<uint16_t>(std::min(index, levels - 1));
}

double
ReconstructPhi(uint16_t index, uint8_t bits)
{
    const double levels = static_cast<double>(uint32_t{1} << bits);
    return 2.0 * PI * (static_cast<double>(index) + 0.5) / levels;
}

double
ReconstructPsi(uint16_t index, uint8_t bits)
{
    const double levels = static_cast<double>(uint32_t{1} << bits);
    return (PI / 2.0) * (static_cast<double>(index) + 0.5) / levels;
}

Eigen::MatrixXcd
MakeRotationProduct(uint32_t numTxAntennas,
                    uint32_t stage,
                    const std::vector<double>& psiAngles)
{
    Eigen::MatrixXcd rotations = Eigen::MatrixXcd::Identity(numTxAntennas, numTxAntennas);
    for (uint32_t offset = 0; offset < psiAngles.size(); ++offset)
    {
        const uint32_t row = stage + offset + 1;
        const double cosine = std::cos(psiAngles.at(offset));
        const double sine = std::sin(psiAngles.at(offset));
        Eigen::MatrixXcd transpose =
            Eigen::MatrixXcd::Identity(numTxAntennas, numTxAntennas);
        transpose(stage, stage) = cosine;
        transpose(stage, row) = -sine;
        transpose(row, stage) = sine;
        transpose(row, row) = cosine;
        rotations = transpose * rotations;
    }
    return rotations;
}

void
ValidateSemiUnitary(const MimoPrecodingMatrix& precoding)
{
    const uint32_t numTxAntennas = precoding.GetNumTxAntennas();
    const uint32_t numStreams = precoding.GetNumStreams();
    for (uint32_t sc = 0; sc < precoding.GetNumSubcarriers(); ++sc)
    {
        for (uint32_t left = 0; left < numStreams; ++left)
        {
            for (uint32_t right = 0; right < numStreams; ++right)
            {
                std::complex<double> inner{0.0, 0.0};
                for (uint32_t tx = 0; tx < numTxAntennas; ++tx)
                {
                    inner += std::conj(precoding.Get(sc, tx, left)) *
                             precoding.Get(sc, tx, right);
                }
                const std::complex<double> expected =
                    left == right ? std::complex<double>{1.0, 0.0} :
                                    std::complex<double>{0.0, 0.0};
                if (std::abs(inner - expected) > SEMI_UNITARY_TOLERANCE)
                {
                    Reject("SU compressed feedback requires a semi-unitary equal-power precoder");
                }
            }
        }
    }
}

void
ValidateArrayIdentity(const MimoCsiArrayIdentity& array)
{
    if (array.numRows == 0 || array.numColumns == 0)
    {
        Reject("CSI array rows and columns must be positive");
    }
    const double values[] = {array.verticalSpacing,
                             array.horizontalSpacing,
                             array.azimuthDegrees,
                             array.elevationDegrees,
                             array.rollDegrees};
    for (double value : values)
    {
        if (!std::isfinite(value))
        {
            Reject("CSI array identity values must be finite");
        }
    }
    if (array.verticalSpacing <= 0.0 || array.horizontalSpacing <= 0.0)
    {
        Reject("CSI array spacings must be positive");
    }
}

} // namespace

MimoCompressedBeamformingFeedback::MimoCompressedBeamformingFeedback(
    uint32_t numSubcarriers,
    uint32_t numTxAntennas,
    uint32_t numStreams,
    MimoFeedbackConfiguration configuration,
    std::vector<uint16_t> phiIndices,
    std::vector<uint16_t> psiIndices,
    std::vector<std::complex<double>> unquantizedCoefficients)
    : m_numSubcarriers(numSubcarriers),
      m_numTxAntennas(numTxAntennas),
      m_numStreams(numStreams),
      m_configuration(configuration),
      m_phiIndices(std::move(phiIndices)),
      m_psiIndices(std::move(psiIndices)),
      m_unquantizedCoefficients(std::move(unquantizedCoefficients))
{
    ValidateFeedbackDimensions(numSubcarriers, numTxAntennas, numStreams, configuration);
}

MimoCompressedBeamformingFeedback
MimoCompressedBeamformingFeedback::Encode(
    const MimoPrecodingMatrix& idealPrecoding,
    const MimoFeedbackConfiguration& configuration)
{
    const uint32_t numSubcarriers = idealPrecoding.GetNumSubcarriers();
    const uint32_t numTxAntennas = idealPrecoding.GetNumTxAntennas();
    const uint32_t numStreams = idealPrecoding.GetNumStreams();
    ValidateFeedbackDimensions(numSubcarriers,
                               numTxAntennas,
                               numStreams,
                               configuration);
    ValidateSemiUnitary(idealPrecoding);

    if (configuration.codebook == MimoFeedbackCodebook::UNQUANTIZED)
    {
        return MimoCompressedBeamformingFeedback(numSubcarriers,
                                                  numTxAntennas,
                                                  numStreams,
                                                  configuration,
                                                  {},
                                                  {},
                                                  idealPrecoding.GetCoefficients());
    }

    const auto [phiBits, psiBits] = GetBitWidths(configuration.codebook);
    const uint32_t anglesPerType = GetAnglesPerType(numTxAntennas, numStreams);
    std::vector<uint16_t> phiIndices;
    std::vector<uint16_t> psiIndices;
    phiIndices.reserve(static_cast<std::size_t>(numSubcarriers) * anglesPerType);
    psiIndices.reserve(static_cast<std::size_t>(numSubcarriers) * anglesPerType);

    const uint32_t stages = std::min(numStreams, numTxAntennas - 1);
    for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
    {
        Eigen::MatrixXcd remaining(numTxAntennas, numStreams);
        for (uint32_t tx = 0; tx < numTxAntennas; ++tx)
        {
            for (uint32_t stream = 0; stream < numStreams; ++stream)
            {
                remaining(tx, stream) = idealPrecoding.Get(sc, tx, stream);
            }
        }

        for (uint32_t stage = 0; stage < stages; ++stage)
        {
            const std::complex<double> bottom = remaining(numTxAntennas - 1, stage);
            if (std::abs(bottom) > ANGLE_EXTRACTION_TOLERANCE)
            {
                remaining.col(stage) *= std::conj(bottom) / std::abs(bottom);
            }
            else
            {
                uint32_t pivot = stage;
                double pivotMagnitude = std::abs(remaining(stage, stage));
                for (uint32_t row = stage + 1; row < numTxAntennas; ++row)
                {
                    const double magnitude = std::abs(remaining(row, stage));
                    if (magnitude > pivotMagnitude)
                    {
                        pivot = row;
                        pivotMagnitude = magnitude;
                    }
                }
                if (pivotMagnitude > ANGLE_EXTRACTION_TOLERANCE)
                {
                    remaining.col(stage) *=
                        std::conj(remaining(pivot, stage)) / pivotMagnitude;
                }
            }

            Eigen::MatrixXcd phases =
                Eigen::MatrixXcd::Identity(numTxAntennas, numTxAntennas);
            for (uint32_t row = stage; row + 1 < numTxAntennas; ++row)
            {
                const double phi = NormalizePhi(std::arg(remaining(row, stage)));
                phiIndices.push_back(QuantizePhi(phi, phiBits));
                phases(row, row) = std::polar(1.0, phi);
            }
            remaining = phases.adjoint() * remaining;

            double residual = 1.0;
            std::vector<double> psiAngles;
            psiAngles.reserve(numTxAntennas - stage - 1);
            for (uint32_t row = stage + 1; row < numTxAntennas; ++row)
            {
                const double ratio =
                    residual > ANGLE_EXTRACTION_TOLERANCE
                        ? std::clamp(std::abs(remaining(row, stage)) / residual, 0.0, 1.0)
                        : 0.0;
                const double psi = std::asin(ratio);
                psiAngles.push_back(psi);
                psiIndices.push_back(QuantizePsi(psi, psiBits));
                residual = std::sqrt(std::max(0.0,
                                              residual * residual -
                                                  std::norm(remaining(row, stage))));
            }
            const Eigen::MatrixXcd rotations =
                MakeRotationProduct(numTxAntennas, stage, psiAngles);
            remaining = rotations.adjoint() * remaining;
        }
    }

    return MimoCompressedBeamformingFeedback(numSubcarriers,
                                              numTxAntennas,
                                              numStreams,
                                              configuration,
                                              std::move(phiIndices),
                                              std::move(psiIndices),
                                              {});
}

MimoCompressedBeamformingFeedback
MimoCompressedBeamformingFeedback::FromQuantizedAngles(
    uint32_t numSubcarriers,
    uint32_t numTxAntennas,
    uint32_t numStreams,
    const MimoFeedbackConfiguration& configuration,
    const std::vector<uint16_t>& phiIndices,
    const std::vector<uint16_t>& psiIndices)
{
    ValidateFeedbackDimensions(numSubcarriers,
                               numTxAntennas,
                               numStreams,
                               configuration);
    if (configuration.codebook == MimoFeedbackCodebook::UNQUANTIZED)
    {
        Reject("unquantized feedback cannot be constructed from angle indices");
    }
    const uint32_t anglesPerType = GetAnglesPerType(numTxAntennas, numStreams);
    const std::size_t expected = static_cast<std::size_t>(numSubcarriers) * anglesPerType;
    if (phiIndices.size() != expected || psiIndices.size() != expected)
    {
        Reject("angle-index count does not match the feedback dimensions");
    }
    const auto [phiBits, psiBits] = GetBitWidths(configuration.codebook);
    const uint32_t phiLevels = uint32_t{1} << phiBits;
    const uint32_t psiLevels = uint32_t{1} << psiBits;
    if (std::any_of(phiIndices.begin(), phiIndices.end(), [phiLevels](uint16_t value) {
            return value >= phiLevels;
        }) ||
        std::any_of(psiIndices.begin(), psiIndices.end(), [psiLevels](uint16_t value) {
            return value >= psiLevels;
        }))
    {
        Reject("angle index exceeds the selected codebook width");
    }
    return MimoCompressedBeamformingFeedback(numSubcarriers,
                                              numTxAntennas,
                                              numStreams,
                                              configuration,
                                              phiIndices,
                                              psiIndices,
                                              {});
}

MimoFeedbackReconstruction
MimoCompressedBeamformingFeedback::Reconstruct() const
{
    std::vector<std::complex<double>> coefficients;
    coefficients.reserve(static_cast<std::size_t>(m_numSubcarriers) *
                         m_numTxAntennas * m_numStreams);
    std::vector<double> powersBefore;
    std::vector<double> normalizationFactors;
    powersBefore.reserve(m_numSubcarriers);
    normalizationFactors.reserve(m_numSubcarriers);

    if (m_configuration.codebook == MimoFeedbackCodebook::UNQUANTIZED)
    {
        const MimoPrecodingMatrix matrix(m_numSubcarriers,
                                         m_numTxAntennas,
                                         m_numStreams,
                                         m_unquantizedCoefficients);
        for (uint32_t sc = 0; sc < m_numSubcarriers; ++sc)
        {
            powersBefore.push_back(matrix.GetPower(sc));
            normalizationFactors.push_back(1.0);
        }
        return MimoFeedbackReconstruction(matrix,
                                          std::move(powersBefore),
                                          std::move(normalizationFactors));
    }

    const auto [phiBits, psiBits] = GetBitWidths(m_configuration.codebook);
    const uint32_t stages = std::min(m_numStreams, m_numTxAntennas - 1);
    for (uint32_t sc = 0; sc < m_numSubcarriers; ++sc)
    {
        Eigen::MatrixXcd product =
            Eigen::MatrixXcd::Identity(m_numTxAntennas, m_numTxAntennas);
        uint32_t phiAngle = 0;
        uint32_t psiAngle = 0;
        for (uint32_t stage = 0; stage < stages; ++stage)
        {
            Eigen::MatrixXcd phases =
                Eigen::MatrixXcd::Identity(m_numTxAntennas, m_numTxAntennas);
            for (uint32_t row = stage; row + 1 < m_numTxAntennas; ++row)
            {
                const double phi = ReconstructPhi(GetPhiIndex(sc, phiAngle++), phiBits);
                phases(row, row) = std::polar(1.0, phi);
            }
            std::vector<double> psiAngles;
            psiAngles.reserve(m_numTxAntennas - stage - 1);
            for (uint32_t row = stage + 1; row < m_numTxAntennas; ++row)
            {
                psiAngles.push_back(
                    ReconstructPsi(GetPsiIndex(sc, psiAngle++), psiBits));
            }
            product *= phases * MakeRotationProduct(m_numTxAntennas, stage, psiAngles);
        }

        Eigen::MatrixXcd rebuilt = product.leftCols(m_numStreams);
        const double powerBefore = rebuilt.squaredNorm();
        if (!std::isfinite(powerBefore) || powerBefore <= 0.0)
        {
            Reject("reconstruction produced non-finite or zero precoder power");
        }
        const double normalization = std::sqrt(m_numStreams / powerBefore);
        rebuilt *= normalization;
        powersBefore.push_back(powerBefore);
        normalizationFactors.push_back(normalization);
        for (uint32_t tx = 0; tx < m_numTxAntennas; ++tx)
        {
            for (uint32_t stream = 0; stream < m_numStreams; ++stream)
            {
                coefficients.push_back(rebuilt(tx, stream));
            }
        }
    }

    return MimoFeedbackReconstruction(
        MimoPrecodingMatrix(m_numSubcarriers,
                            m_numTxAntennas,
                            m_numStreams,
                            coefficients),
        std::move(powersBefore),
        std::move(normalizationFactors));
}

MimoFeedbackConfiguration
MimoCompressedBeamformingFeedback::GetConfiguration() const
{
    return m_configuration;
}

uint32_t
MimoCompressedBeamformingFeedback::GetNumSubcarriers() const
{
    return m_numSubcarriers;
}

uint32_t
MimoCompressedBeamformingFeedback::GetNumTxAntennas() const
{
    return m_numTxAntennas;
}

uint32_t
MimoCompressedBeamformingFeedback::GetNumStreams() const
{
    return m_numStreams;
}

uint32_t
MimoCompressedBeamformingFeedback::GetNumPhiAnglesPerSubcarrier() const
{
    return GetAnglesPerType(m_numTxAntennas, m_numStreams);
}

uint32_t
MimoCompressedBeamformingFeedback::GetNumPsiAnglesPerSubcarrier() const
{
    return GetAnglesPerType(m_numTxAntennas, m_numStreams);
}

uint8_t
MimoCompressedBeamformingFeedback::GetPhiBitWidth() const
{
    return GetBitWidths(m_configuration.codebook).first;
}

uint8_t
MimoCompressedBeamformingFeedback::GetPsiBitWidth() const
{
    return GetBitWidths(m_configuration.codebook).second;
}

uint16_t
MimoCompressedBeamformingFeedback::GetPhiIndex(uint32_t subcarrier, uint32_t angle) const
{
    return m_phiIndices.at(
        GetFlatAngleIndex(subcarrier, angle, GetNumPhiAnglesPerSubcarrier()));
}

uint16_t
MimoCompressedBeamformingFeedback::GetPsiIndex(uint32_t subcarrier, uint32_t angle) const
{
    return m_psiIndices.at(
        GetFlatAngleIndex(subcarrier, angle, GetNumPsiAnglesPerSubcarrier()));
}

uint64_t
MimoCompressedBeamformingFeedback::GetFeedbackBitCount() const
{
    return static_cast<uint64_t>(m_phiIndices.size()) * GetPhiBitWidth() +
           static_cast<uint64_t>(m_psiIndices.size()) * GetPsiBitWidth();
}

std::size_t
MimoCompressedBeamformingFeedback::GetFlatAngleIndex(uint32_t subcarrier,
                                                      uint32_t angle,
                                                      uint32_t anglesPerSubcarrier) const
{
    if (subcarrier >= m_numSubcarriers || angle >= anglesPerSubcarrier)
    {
        throw std::out_of_range("MimoCsiFeedback: angle index is out of range");
    }
    return static_cast<std::size_t>(subcarrier) * anglesPerSubcarrier + angle;
}

MimoFeedbackReconstruction::MimoFeedbackReconstruction(
    MimoPrecodingMatrix precoding,
    std::vector<double> powerBeforeNormalization,
    std::vector<double> normalizationFactors)
    : m_precoding(std::move(precoding)),
      m_powerBeforeNormalization(std::move(powerBeforeNormalization)),
      m_normalizationFactors(std::move(normalizationFactors))
{
}

const MimoPrecodingMatrix&
MimoFeedbackReconstruction::GetPrecodingMatrix() const
{
    return m_precoding;
}

double
MimoFeedbackReconstruction::GetPowerBeforeNormalization(uint32_t subcarrier) const
{
    return m_powerBeforeNormalization.at(subcarrier);
}

double
MimoFeedbackReconstruction::GetNormalizationFactor(uint32_t subcarrier) const
{
    return m_normalizationFactors.at(subcarrier);
}

double
MimoFeedbackReconstruction::GetPowerAfterNormalization(uint32_t subcarrier) const
{
    return m_precoding.GetPower(subcarrier);
}

MimoFeedbackDiagnostics
MimoFeedbackDiagnostics::Calculate(
    const MimoPrecodingMatrix& idealPrecoding,
    const MimoCompressedBeamformingFeedback& feedback,
    const MimoFeedbackReconstruction& reconstruction)
{
    const auto& rebuilt = reconstruction.GetPrecodingMatrix();
    if (idealPrecoding.GetNumSubcarriers() != rebuilt.GetNumSubcarriers() ||
        idealPrecoding.GetNumTxAntennas() != rebuilt.GetNumTxAntennas() ||
        idealPrecoding.GetNumStreams() != rebuilt.GetNumStreams())
    {
        Reject("diagnostic precoder dimensions differ");
    }
    if (feedback.GetNumSubcarriers() != rebuilt.GetNumSubcarriers() ||
        feedback.GetNumTxAntennas() != rebuilt.GetNumTxAntennas() ||
        feedback.GetNumStreams() != rebuilt.GetNumStreams())
    {
        Reject("diagnostic feedback dimensions differ");
    }

    std::vector<double> chordalErrors(rebuilt.GetNumSubcarriers(), 0.0);
    std::vector<double> coefficientErrors(rebuilt.GetNumSubcarriers(), 0.0);
    for (uint32_t sc = 0; sc < rebuilt.GetNumSubcarriers(); ++sc)
    {
        double chordalSquared = 0.0;
        double coefficientSquared = 0.0;
        for (uint32_t stream = 0; stream < rebuilt.GetNumStreams(); ++stream)
        {
            std::complex<double> inner{0.0, 0.0};
            double idealNorm = 0.0;
            double rebuiltNorm = 0.0;
            for (uint32_t tx = 0; tx < rebuilt.GetNumTxAntennas(); ++tx)
            {
                const auto ideal = idealPrecoding.Get(sc, tx, stream);
                const auto actual = rebuilt.Get(sc, tx, stream);
                inner += std::conj(ideal) * actual;
                idealNorm += std::norm(ideal);
                rebuiltNorm += std::norm(actual);
            }
            const double normalizedCorrelation =
                std::norm(inner) / (idealNorm * rebuiltNorm);
            chordalSquared += std::max(0.0, 1.0 - normalizedCorrelation);
            const std::complex<double> phaseAlignment =
                std::abs(inner) > 0.0 ? std::conj(inner) / std::abs(inner) :
                                        std::complex<double>{1.0, 0.0};
            for (uint32_t tx = 0; tx < rebuilt.GetNumTxAntennas(); ++tx)
            {
                coefficientSquared +=
                    std::norm(idealPrecoding.Get(sc, tx, stream) -
                              rebuilt.Get(sc, tx, stream) * phaseAlignment);
            }
        }
        chordalErrors.at(sc) =
            std::sqrt(chordalSquared / rebuilt.GetNumStreams());
        coefficientErrors.at(sc) =
            std::sqrt(coefficientSquared / rebuilt.GetNumStreams());
    }
    return MimoFeedbackDiagnostics(feedback.GetFeedbackBitCount(),
                                   std::move(chordalErrors),
                                   std::move(coefficientErrors));
}

MimoFeedbackDiagnostics::MimoFeedbackDiagnostics(
    uint64_t feedbackBitCount,
    std::vector<double> chordalErrors,
    std::vector<double> rmsCoefficientErrors)
    : m_feedbackBitCount(feedbackBitCount),
      m_chordalErrors(std::move(chordalErrors)),
      m_rmsCoefficientErrors(std::move(rmsCoefficientErrors))
{
}

uint64_t
MimoFeedbackDiagnostics::GetFeedbackBitCount() const
{
    return m_feedbackBitCount;
}

double
MimoFeedbackDiagnostics::GetChordalError(uint32_t subcarrier) const
{
    return m_chordalErrors.at(subcarrier);
}

double
MimoFeedbackDiagnostics::GetRmsCoefficientError(uint32_t subcarrier) const
{
    return m_rmsCoefficientErrors.at(subcarrier);
}

double
MimoFeedbackDiagnostics::GetMeanChordalError() const
{
    return std::accumulate(m_chordalErrors.begin(), m_chordalErrors.end(), 0.0) /
           m_chordalErrors.size();
}

double
MimoFeedbackDiagnostics::GetMeanRmsCoefficientError() const
{
    return std::accumulate(m_rmsCoefficientErrors.begin(),
                           m_rmsCoefficientErrors.end(),
                           0.0) /
           m_rmsCoefficientErrors.size();
}

bool
MimoCsiArrayIdentity::operator==(const MimoCsiArrayIdentity& other) const
{
    return std::tie(numRows,
                    numColumns,
                    verticalSpacing,
                    horizontalSpacing,
                    pattern,
                    polarization,
                    azimuthDegrees,
                    elevationDegrees,
                    rollDegrees) ==
           std::tie(other.numRows,
                    other.numColumns,
                    other.verticalSpacing,
                    other.horizontalSpacing,
                    other.pattern,
                    other.polarization,
                    other.azimuthDegrees,
                    other.elevationDegrees,
                    other.rollDegrees);
}

bool
MimoCsiArrayIdentity::operator<(const MimoCsiArrayIdentity& other) const
{
    return std::tie(numRows,
                    numColumns,
                    verticalSpacing,
                    horizontalSpacing,
                    pattern,
                    polarization,
                    azimuthDegrees,
                    elevationDegrees,
                    rollDegrees) <
           std::tie(other.numRows,
                    other.numColumns,
                    other.verticalSpacing,
                    other.horizontalSpacing,
                    other.pattern,
                    other.polarization,
                    other.azimuthDegrees,
                    other.elevationDegrees,
                    other.rollDegrees);
}

bool
MimoCsiStateIdentity::operator==(const MimoCsiStateIdentity& other) const
{
    return txNodeId == other.txNodeId && rxNodeId == other.rxNodeId &&
           txArray == other.txArray && rxArray == other.rxArray &&
           precodingMode == other.precodingMode &&
           feedback.standard == other.feedback.standard &&
           feedback.codebook == other.feedback.codebook &&
           csiTimestamp == other.csiTimestamp;
}

bool
MimoCsiStateIdentity::operator<(const MimoCsiStateIdentity& other) const
{
    if (txNodeId != other.txNodeId)
    {
        return txNodeId < other.txNodeId;
    }
    if (rxNodeId != other.rxNodeId)
    {
        return rxNodeId < other.rxNodeId;
    }
    if (!(txArray == other.txArray))
    {
        return txArray < other.txArray;
    }
    if (!(rxArray == other.rxArray))
    {
        return rxArray < other.rxArray;
    }
    if (precodingMode != other.precodingMode)
    {
        return precodingMode < other.precodingMode;
    }
    if (feedback.standard != other.feedback.standard)
    {
        return feedback.standard < other.feedback.standard;
    }
    if (feedback.codebook != other.feedback.codebook)
    {
        return feedback.codebook < other.feedback.codebook;
    }
    return csiTimestamp < other.csiTimestamp;
}

Time
MimoCsiTimingConfiguration::ResolveCsiTimestamp(Time transmissionTime) const
{
    if (transmissionTime < NanoSeconds(0))
    {
        Reject("transmission timestamp must be non-negative");
    }
    if (updateInterval < NanoSeconds(0))
    {
        Reject("CSI update interval must be non-negative");
    }
    if (feedbackDelay < NanoSeconds(0))
    {
        Reject("feedback delay must be non-negative");
    }
    if (transmissionTime < feedbackDelay)
    {
        Reject("no CSI feedback is available at the transmission timestamp");
    }
    const int64_t deadline = (transmissionTime - feedbackDelay).GetTimeStep();
    if (updateInterval == NanoSeconds(0))
    {
        return TimeStep(deadline);
    }
    const int64_t interval = updateInterval.GetTimeStep();
    return TimeStep((deadline / interval) * interval);
}

MimoCsiSelectionResult::MimoCsiSelectionResult(
    MimoSvdPrecodingResult idealSvd,
    MimoCompressedBeamformingFeedback feedback,
    MimoFeedbackReconstruction reconstruction,
    MimoFeedbackDiagnostics diagnostics,
    MimoCsiStateIdentity identity,
    Time transmissionTimestamp,
    Time updateInterval,
    Time feedbackDelay)
    : m_idealSvd(std::move(idealSvd)),
      m_feedback(std::move(feedback)),
      m_reconstruction(std::move(reconstruction)),
      m_diagnostics(std::move(diagnostics)),
      m_identity(std::move(identity)),
      m_transmissionTimestamp(transmissionTimestamp),
      m_updateInterval(updateInterval),
      m_feedbackDelay(feedbackDelay)
{
}

const MimoSvdPrecodingResult&
MimoCsiSelectionResult::GetIdealSvdResult() const
{
    return m_idealSvd;
}

const MimoCompressedBeamformingFeedback&
MimoCsiSelectionResult::GetFeedback() const
{
    return m_feedback;
}

const MimoFeedbackReconstruction&
MimoCsiSelectionResult::GetReconstruction() const
{
    return m_reconstruction;
}

const MimoFeedbackDiagnostics&
MimoCsiSelectionResult::GetDiagnostics() const
{
    return m_diagnostics;
}

const MimoCsiStateIdentity&
MimoCsiSelectionResult::GetStateIdentity() const
{
    return m_identity;
}

Time
MimoCsiSelectionResult::GetCsiTimestamp() const
{
    return m_identity.csiTimestamp;
}

Time
MimoCsiSelectionResult::GetTransmissionTimestamp() const
{
    return m_transmissionTimestamp;
}

Time
MimoCsiSelectionResult::GetCsiAge() const
{
    return m_transmissionTimestamp - m_identity.csiTimestamp;
}

Time
MimoCsiSelectionResult::GetUpdateInterval() const
{
    return m_updateInterval;
}

Time
MimoCsiSelectionResult::GetFeedbackDelay() const
{
    return m_feedbackDelay;
}

SuMimoLinkResult
MimoCsiSelectionResult::CalculateLink(
    const MimoChannelMatrix& transmissionTimeChannel,
    double referenceSnrLinear) const
{
    return SuMimoLinkModel::Calculate(transmissionTimeChannel,
                                      m_reconstruction.GetPrecodingMatrix(),
                                      referenceSnrLinear);
}

MimoCsiSelectionResult
MimoCsiAgingModel::Select(
    const MimoChannelMatrix& csiTimeChannelEstimate,
    Time csiTimestamp,
    Time transmissionTimestamp,
    const MimoCsiSelectionConfiguration& configuration)
{
    ValidateArrayIdentity(configuration.txArray);
    ValidateArrayIdentity(configuration.rxArray);
    if (configuration.precodingMode != MimoCsiPrecodingMode::SVD_EQUAL_POWER)
    {
        Reject("unsupported CSI precoding mode");
    }
    if (configuration.svd.powerAllocation != MimoPowerAllocationMode::EQUAL_POWER)
    {
        Reject("compressed feedback currently supports equal-power SVD only");
    }
    const Time expectedCsiTimestamp =
        configuration.timing.ResolveCsiTimestamp(transmissionTimestamp);
    if (csiTimestamp != expectedCsiTimestamp)
    {
        Reject("CSI timestamp does not match update interval and feedback delay");
    }

    auto idealSvd = MimoPrecodingMatrix::SingularValueDecomposition(
        csiTimeChannelEstimate,
        configuration.numStreams,
        configuration.svd);
    auto feedback = MimoCompressedBeamformingFeedback::Encode(
        idealSvd.GetPrecodingMatrix(),
        configuration.feedback);
    auto reconstruction = feedback.Reconstruct();
    auto diagnostics = MimoFeedbackDiagnostics::Calculate(
        idealSvd.GetPrecodingMatrix(),
        feedback,
        reconstruction);

    MimoCsiStateIdentity identity;
    identity.txNodeId = configuration.txNodeId;
    identity.rxNodeId = configuration.rxNodeId;
    identity.txArray = configuration.txArray;
    identity.rxArray = configuration.rxArray;
    identity.precodingMode = configuration.precodingMode;
    identity.feedback = configuration.feedback;
    identity.csiTimestamp = csiTimestamp;

    return MimoCsiSelectionResult(std::move(idealSvd),
                                  std::move(feedback),
                                  std::move(reconstruction),
                                  std::move(diagnostics),
                                  std::move(identity),
                                  transmissionTimestamp,
                                  configuration.timing.updateInterval,
                                  configuration.timing.feedbackDelay);
}

} // namespace ns3
