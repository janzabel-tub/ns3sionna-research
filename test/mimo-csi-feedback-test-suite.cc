/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/he-phy.h"
#include "ns3/he-su-mimo-link-abstraction.h"
#include "ns3/mimo-channel-matrix.h"
#include "ns3/mimo-csi-feedback.h"
#include "ns3/mimo-precoding-matrix.h"
#include "ns3/su-mimo-link-model.h"
#include "ns3/test.h"
#include "ns3/wifi-link-budget.h"
#include "ns3/wifi-tx-vector.h"

#include <cmath>
#include <complex>
#include <stdexcept>
#include <string>

using namespace ns3;

namespace
{

constexpr double PI = 3.141592653589793238462643383279502884;

class MimoCompressedFeedbackTest : public TestCase
{
  public:
    MimoCompressedFeedbackTest()
        : TestCase("HE and EHT SU feedback quantizes and reconstructs deterministically")
    {
    }

  private:
    void DoRun() override
    {
        MimoFeedbackConfiguration coarse;
        coarse.standard = MimoFeedbackStandard::HE;
        coarse.codebook = MimoFeedbackCodebook::SU_COARSE;

        const auto minimum = MimoCompressedBeamformingFeedback::FromQuantizedAngles(
            1, 2, 1, coarse, {0}, {0});
        const auto minimumReconstruction = minimum.Reconstruct();
        const auto& minimumW = minimumReconstruction.GetPrecodingMatrix();
        const std::complex<double> expectedFirst =
            std::polar(std::cos(PI / 16.0), PI / 16.0);
        NS_TEST_ASSERT_MSG_EQ(minimum.GetPhiBitWidth(), 4, "Coarse phi width is wrong");
        NS_TEST_ASSERT_MSG_EQ(minimum.GetPsiBitWidth(), 2, "Coarse psi width is wrong");
        NS_TEST_ASSERT_MSG_EQ(minimum.GetFeedbackBitCount(),
                              6,
                              "2x1 coarse angle payload is not six bits");
        NS_TEST_ASSERT_MSG_EQ_TOL(std::abs(minimumW.Get(0, 0, 0) - expectedFirst),
                                  0.0,
                                  1e-12,
                                  "Minimum indices reconstructed the wrong first coefficient");
        NS_TEST_ASSERT_MSG_EQ_TOL(std::abs(minimumW.Get(0, 1, 0) -
                                           std::complex<double>(std::sin(PI / 16.0), 0.0)),
                                  0.0,
                                  1e-12,
                                  "Minimum psi index reconstructed the wrong second coefficient");
        NS_TEST_ASSERT_MSG_EQ_TOL(minimumW.GetPower(0),
                                  1.0,
                                  1e-12,
                                  "Minimum-index reconstruction lost transmit power");

        const auto maximum = MimoCompressedBeamformingFeedback::FromQuantizedAngles(
            1, 2, 1, coarse, {15}, {3});
        const auto maximumReconstruction = maximum.Reconstruct();
        NS_TEST_ASSERT_MSG_EQ_TOL(
            std::abs(maximumReconstruction.GetPrecodingMatrix().Get(0, 0, 0) -
                     std::polar(std::cos(7.0 * PI / 16.0), 31.0 * PI / 16.0)),
            0.0,
            1e-12,
            "Maximum indices reconstructed the wrong boundary vector");
        NS_TEST_ASSERT_MSG_EQ_TOL(maximumReconstruction.GetPowerAfterNormalization(0),
                                  1.0,
                                  1e-12,
                                  "Maximum-index reconstruction lost transmit power");

        const double phi = 0.73;
        const double psi = 0.41;
        const auto ideal = MimoPrecodingMatrix::FrequencyFlat(
            2,
            2,
            1,
            {std::polar(std::cos(psi), phi), {std::sin(psi), 0.0}});
        const auto coarseFeedback =
            MimoCompressedBeamformingFeedback::Encode(ideal, coarse);
        const auto coarseReconstruction = coarseFeedback.Reconstruct();
        const auto coarseDiagnostics = MimoFeedbackDiagnostics::Calculate(
            ideal, coarseFeedback, coarseReconstruction);
        NS_TEST_ASSERT_MSG_EQ(coarseFeedback.GetPhiIndex(0, 0),
                              coarseFeedback.GetPhiIndex(1, 0),
                              "Frequency-flat input did not encode identically per tone");
        NS_TEST_ASSERT_MSG_EQ(coarseFeedback.GetPsiIndex(0, 0),
                              coarseFeedback.GetPsiIndex(1, 0),
                              "Frequency-flat psi input did not encode identically per tone");
        NS_TEST_ASSERT_MSG_EQ(coarseDiagnostics.GetFeedbackBitCount(),
                              12,
                              "Two-tone coarse feedback bit count is wrong");

        MimoFeedbackConfiguration fine = coarse;
        fine.standard = MimoFeedbackStandard::EHT;
        fine.codebook = MimoFeedbackCodebook::SU_FINE;
        const auto fineFeedback = MimoCompressedBeamformingFeedback::Encode(ideal, fine);
        const auto fineReconstruction = fineFeedback.Reconstruct();
        const auto fineDiagnostics = MimoFeedbackDiagnostics::Calculate(
            ideal, fineFeedback, fineReconstruction);
        NS_TEST_ASSERT_MSG_EQ(fineFeedback.GetPhiBitWidth(), 6, "Fine phi width is wrong");
        NS_TEST_ASSERT_MSG_EQ(fineFeedback.GetPsiBitWidth(), 4, "Fine psi width is wrong");
        NS_TEST_ASSERT_MSG_EQ(fineDiagnostics.GetFeedbackBitCount(),
                              20,
                              "Two-tone fine feedback bit count is wrong");
        NS_TEST_ASSERT_MSG_EQ((fineDiagnostics.GetMeanChordalError() <=
                               coarseDiagnostics.GetMeanChordalError() + 1e-15),
                              true,
                              "Finer angle quantization increased chordal error");
        for (uint32_t sc = 0; sc < 2; ++sc)
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(fineReconstruction.GetPowerAfterNormalization(sc),
                                      1.0,
                                      1e-12,
                                      "Fine reconstruction violated the fixed-W[k] power contract");
            NS_TEST_ASSERT_MSG_EQ_TOL(
                fineReconstruction.GetPowerBeforeNormalization(sc) *
                    std::pow(fineReconstruction.GetNormalizationFactor(sc), 2.0),
                1.0,
                1e-12,
                "Power-renormalization diagnostics are inconsistent");
        }

        const auto multistreamReference =
            MimoCompressedBeamformingFeedback::FromQuantizedAngles(
                1, 3, 2, fine, {1, 17, 42}, {2, 7, 11});
        const auto multistreamReconstruction = multistreamReference.Reconstruct();
        const auto multistreamRoundTrip = MimoCompressedBeamformingFeedback::Encode(
            multistreamReconstruction.GetPrecodingMatrix(), fine);
        NS_TEST_ASSERT_MSG_EQ(multistreamRoundTrip.GetFeedbackBitCount(),
                              30,
                              "3x2 fine feedback payload has the wrong size");
        for (uint32_t angle = 0; angle < 3; ++angle)
        {
            NS_TEST_ASSERT_MSG_EQ(multistreamRoundTrip.GetPhiIndex(0, angle),
                                  multistreamReference.GetPhiIndex(0, angle),
                                  "3x2 phi angle order was not bit-exact on round trip");
            NS_TEST_ASSERT_MSG_EQ(multistreamRoundTrip.GetPsiIndex(0, angle),
                                  multistreamReference.GetPsiIndex(0, angle),
                                  "3x2 psi angle order was not bit-exact on round trip");
        }
        NS_TEST_ASSERT_MSG_EQ_TOL(multistreamReconstruction.GetPowerAfterNormalization(0),
                                  2.0,
                                  1e-12,
                                  "3x2 reconstruction violated fixed-W[k] power");
        MimoFeedbackConfiguration unquantized = coarse;
        unquantized.codebook = MimoFeedbackCodebook::UNQUANTIZED;
        const auto exactFeedback =
            MimoCompressedBeamformingFeedback::Encode(ideal, unquantized);
        const auto exactReconstruction = exactFeedback.Reconstruct();
        const auto exactDiagnostics = MimoFeedbackDiagnostics::Calculate(
            ideal, exactFeedback, exactReconstruction);
        NS_TEST_ASSERT_MSG_EQ(exactFeedback.GetFeedbackBitCount(),
                              0,
                              "Unquantized research limit reported a finite payload");
        NS_TEST_ASSERT_MSG_EQ_TOL(exactDiagnostics.GetMeanChordalError(),
                                  0.0,
                                  1e-12,
                                  "Unquantized feedback changed the beam direction");
        for (std::size_t i = 0; i < ideal.GetCoefficients().size(); ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(exactReconstruction.GetPrecodingMatrix()
                                      .GetCoefficients()
                                      .at(i),
                                  ideal.GetCoefficients().at(i),
                                  "Unquantized reconstruction was not coefficient-exact");
        }

        MimoChannelMatrix twoMode(1, 2, 2);
        twoMode.Set(0, 0, 0, {2.0, 0.0});
        twoMode.Set(0, 1, 1, {1.0, 0.0});
        MimoSvdPrecodingOptions waterOptions;
        waterOptions.powerAllocation = MimoPowerAllocationMode::WATER_FILLING;
        waterOptions.referenceSnrLinear = 0.5;
        const auto water = MimoPrecodingMatrix::SingularValueDecomposition(
            twoMode, 2, waterOptions);
        bool waterRejected = false;
        try
        {
            static_cast<void>(MimoCompressedBeamformingFeedback::Encode(
                water.GetPrecodingMatrix(), coarse));
        }
        catch (const std::invalid_argument& exception)
        {
            waterRejected = std::string(exception.what()).find(
                                "semi-unitary equal-power") != std::string::npos;
        }
        NS_TEST_ASSERT_MSG_EQ(waterRejected,
                              true,
                              "Power-loaded SVD was misrepresented as standard SU feedback V");

        bool invalidIndexRejected = false;
        try
        {
            static_cast<void>(MimoCompressedBeamformingFeedback::FromQuantizedAngles(
                1, 2, 1, coarse, {16}, {0}));
        }
        catch (const std::invalid_argument& exception)
        {
            invalidIndexRejected = std::string(exception.what()).find(
                                       "angle index") != std::string::npos;
        }
        NS_TEST_ASSERT_MSG_EQ(invalidIndexRejected,
                              true,
                              "Out-of-range quantized angle was accepted");
    }
};

class MimoCsiAgingTest : public TestCase
{
  public:
    MimoCsiAgingTest()
        : TestCase("CSI-time selection is distinct from transmission-time channel evaluation")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix firstDirection(1, 1, 2);
        firstDirection.Set(0, 0, 0, {1.0, 0.0});
        MimoChannelMatrix rotatedDirection(1, 1, 2);
        rotatedDirection.Set(0, 0, 1, {1.0, 0.0});

        MimoCsiSelectionConfiguration currentConfiguration;
        currentConfiguration.txNodeId = 7;
        currentConfiguration.rxNodeId = 11;
        currentConfiguration.txArray.numColumns = 2;
        currentConfiguration.txArray.pattern = "tr38901";
        currentConfiguration.rxArray.pattern = "dipole";
        currentConfiguration.feedback.codebook = MimoFeedbackCodebook::UNQUANTIZED;

        const auto current = MimoCsiAgingModel::Select(
            firstDirection, MilliSeconds(10), MilliSeconds(10), currentConfiguration);
        NS_TEST_ASSERT_MSG_EQ(current.GetCsiAge().GetNanoSeconds(),
                              0,
                              "Current CSI did not report zero age");
        const auto ideal = MimoPrecodingMatrix::SingularValueDecomposition(
            firstDirection, 1);
        for (std::size_t i = 0;
             i < ideal.GetPrecodingMatrix().GetCoefficients().size();
             ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(
                current.GetReconstruction().GetPrecodingMatrix().GetCoefficients().at(i),
                ideal.GetPrecodingMatrix().GetCoefficients().at(i),
                "Zero-age unquantized CSI did not reproduce ideal SVD exactly");
        }
        NS_TEST_ASSERT_MSG_EQ_TOL(current.CalculateLink(firstDirection, 10.0)
                                      .GetSinrLinear(0, 0),
                                  10.0,
                                  1e-12,
                                  "Zero-age CSI changed the ideal link result");

        MimoCsiSelectionConfiguration agedConfiguration = currentConfiguration;
        agedConfiguration.timing.updateInterval = MilliSeconds(5);
        agedConfiguration.timing.feedbackDelay = MilliSeconds(2);
        const auto aged = MimoCsiAgingModel::Select(
            firstDirection, MilliSeconds(10), MilliSeconds(13), agedConfiguration);
        NS_TEST_ASSERT_MSG_EQ(aged.GetCsiAge().GetNanoSeconds(),
                              MilliSeconds(3).GetNanoSeconds(),
                              "CSI age does not include periodic sampling and feedback delay");
        NS_TEST_ASSERT_MSG_EQ(aged.GetUpdateInterval().GetNanoSeconds(),
                              MilliSeconds(5).GetNanoSeconds(),
                              "CSI update interval diagnostic changed");
        NS_TEST_ASSERT_MSG_EQ(aged.GetFeedbackDelay().GetNanoSeconds(),
                              MilliSeconds(2).GetNanoSeconds(),
                              "Feedback-delay diagnostic changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(aged.CalculateLink(firstDirection, 10.0)
                                      .GetSinrLinear(0, 0),
                                  10.0,
                                  1e-12,
                                  "Static channel degraded under aged CSI");
        NS_TEST_ASSERT_MSG_EQ_TOL(aged.CalculateLink(rotatedDirection, 10.0)
                                      .GetSinrLinear(0, 0),
                                  0.0,
                                  1e-12,
                                  "Transmission SINR reused H_hat instead of current H");

        constexpr double boltzmann = 1.3803e-23;
        const double txPowerDbm =
            WifiLinkBudget::LinearToDb(10.0 * boltzmann) + 30.0;
        const auto budget =
            WifiLinkBudget::Calculate(txPowerDbm, 0.0, 1.0, 1.0, 0.0);
        const WifiTxVector txVector(HePhy::GetHeMcs0(),
                                    0,
                                    WIFI_PREAMBLE_HE_SU,
                                    800,
                                    1,
                                    1,
                                    0,
                                    20,
                                    false);
        auto heAbstraction = CreateObject<HeSuMimoLinkAbstraction>();
        heAbstraction->SetEesmBeta(0, 1.0);
        const auto transmissionPer = heAbstraction->Calculate(
            rotatedDirection,
            aged.GetReconstruction().GetPrecodingMatrix(),
            budget,
            txVector,
            1000);
        const auto estimatePer = heAbstraction->Calculate(
            firstDirection,
            aged.GetReconstruction().GetPrecodingMatrix(),
            budget,
            txVector,
            1000);
        NS_TEST_ASSERT_MSG_EQ_TOL(transmissionPer.GetEffectiveSinrLinear(),
                                  0.0,
                                  1e-12,
                                  "HE PER path reused H_hat instead of current H");
        NS_TEST_ASSERT_MSG_EQ((transmissionPer.GetPacketErrorRate() >
                               estimatePer.GetPacketErrorRate()),
                              true,
                              "Aged-current-channel PER did not reflect the subspace rotation");

        const auto freshRotated = MimoCsiAgingModel::Select(
            rotatedDirection, MilliSeconds(13), MilliSeconds(13), currentConfiguration);
        NS_TEST_ASSERT_MSG_EQ_TOL(freshRotated.CalculateLink(rotatedDirection, 10.0)
                                      .GetSinrLinear(0, 0),
                                  10.0,
                                  1e-12,
                                  "Fresh rotated CSI did not select the current subspace");

        const auto repeated = MimoCsiAgingModel::Select(
            firstDirection, MilliSeconds(10), MilliSeconds(13), agedConfiguration);
        NS_TEST_ASSERT_MSG_EQ((repeated.GetStateIdentity() == aged.GetStateIdentity()),
                              true,
                              "Repeated selection changed CSI state identity");
        NS_TEST_ASSERT_MSG_EQ(
            (repeated.GetReconstruction().GetPrecodingMatrix().GetCoefficients() ==
             aged.GetReconstruction().GetPrecodingMatrix().GetCoefficients()),
            true,
            "Repeated selection was not bit-reproducible");

        MimoCsiStateIdentity reversed = aged.GetStateIdentity();
        std::swap(reversed.txNodeId, reversed.rxNodeId);
        NS_TEST_ASSERT_MSG_EQ((reversed == aged.GetStateIdentity()),
                              false,
                              "CSI identity ignored link direction");
        MimoCsiStateIdentity changedArray = aged.GetStateIdentity();
        changedArray.txArray.horizontalSpacing = 0.75;
        NS_TEST_ASSERT_MSG_EQ((changedArray == aged.GetStateIdentity()),
                              false,
                              "CSI identity ignored array configuration");
        MimoCsiStateIdentity changedTimestamp = aged.GetStateIdentity();
        changedTimestamp.csiTimestamp += NanoSeconds(1);
        NS_TEST_ASSERT_MSG_EQ((changedTimestamp == aged.GetStateIdentity()),
                              false,
                              "CSI identity ignored CSI timestamp");

        bool timestampRejected = false;
        try
        {
            static_cast<void>(MimoCsiAgingModel::Select(
                firstDirection, MilliSeconds(11), MilliSeconds(13), agedConfiguration));
        }
        catch (const std::invalid_argument& exception)
        {
            timestampRejected = std::string(exception.what()).find(
                                    "update interval and feedback delay") != std::string::npos;
        }
        NS_TEST_ASSERT_MSG_EQ(timestampRejected,
                              true,
                              "An off-grid or unavailable CSI timestamp was accepted");

        MimoChannelMatrix siso(1, 1, 1);
        siso.Set(0, 0, 0, {0.0, 2.0});
        MimoCsiSelectionConfiguration sisoConfiguration;
        const auto sisoSelection = MimoCsiAgingModel::Select(
            siso, NanoSeconds(0), NanoSeconds(0), sisoConfiguration);
        NS_TEST_ASSERT_MSG_EQ_TOL(sisoSelection.CalculateLink(siso, 3.0)
                                      .GetSinrLinear(0, 0),
                                  SuMimoLinkModel::Calculate(siso, 3.0)
                                      .GetSinrLinear(0, 0),
                                  1e-12,
                                  "Default 1x1 CSI selection changed the legacy result");
    }
};

class MimoCsiFeedbackTestSuite : public TestSuite
{
  public:
    MimoCsiFeedbackTestSuite()
        : TestSuite("sionna-mimo-csi-feedback", UNIT)
    {
        AddTestCase(new MimoCompressedFeedbackTest, TestCase::QUICK);
        AddTestCase(new MimoCsiAgingTest, TestCase::QUICK);
    }
};

static MimoCsiFeedbackTestSuite g_mimoCsiFeedbackTestSuite;

} // namespace
