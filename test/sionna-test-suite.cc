/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/boolean.h"
#include "ns3/constant-position-mobility-model.h"
#include "ns3/dl-mu-mimo-link-model.h"
#include "ns3/double.h"
#include "ns3/error-rate-model.h"
#include "ns3/he-dl-mu-mimo-link-abstraction.h"
#include "ns3/he-phy.h"
#include "ns3/he-ppdu.h"
#include "ns3/he-su-mimo-link-abstraction.h"
#include "ns3/mimo-channel-matrix.h"
#include "ns3/mimo-precoding-matrix.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/simulator.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/sionna-helper.h"
#include "ns3/sionna-mobility-model.h"
#include "ns3/sionna-mimo-helper.h"
#include "ns3/sionna-propagation-cache.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/spectrum-wifi-phy.h"
#include "ns3/su-mimo-link-model.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"
#include "ns3/wifi-helper.h"
#include "ns3/wifi-link-budget.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-mac-helper.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/yans-error-rate-model.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-spectrum-signal-parameters.h"
#include "ns3/wifi-spectrum-phy-interface.h"
#include "ns3/wifi-spectrum-value-helper.h"
#include "ns3/wifi-tx-vector.h"

#include <array>
#include <cmath>
#include <complex>
#include <iterator>
#include <iostream>
#include <future>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace ns3;

namespace
{

WifiTxVector
MakeHeTxVector(WifiMode mode, uint8_t nss)
{
    return WifiTxVector(mode,
                        0,
                        WIFI_PREAMBLE_HE_SU,
                        800,
                        nss,
                        nss,
                        0,
                        20,
                        false);
}

WifiLinkBudgetResult
MakeReferenceSnrBudget(double referenceSnrLinear)
{
    constexpr double boltzmann = 1.3803e-23;
    const double txPowerDbm =
        WifiLinkBudget::LinearToDb(referenceSnrLinear * boltzmann) + 30.0;
    return WifiLinkBudget::Calculate(txPowerDbm, 0.0, 1.0, 1.0, 0.0);
}

double
IntegrateSpectrumBand(const SpectrumValue& psd, const WifiSpectrumBandIndices& indices)
{
    auto band = psd.GetSpectrumModel()->Begin();
    std::advance(band, indices.first);
    double powerW = 0.0;
    for (std::size_t index = indices.first; index <= indices.second; ++index, ++band)
    {
        powerW += psd.ValuesAt(index) * (band->fh - band->fl);
    }
    return powerW;
}

class HeDlMuTestHePhy : public HePhy
{
  public:
    explicit HeDlMuTestHePhy(uint16_t staId)
        : m_staId(staId)
    {
    }

    uint16_t GetStaId(const Ptr<const WifiPpdu> ppdu) const override
    {
        return ppdu->GetType() == WIFI_PPDU_TYPE_DL_MU ? m_staId : HePhy::GetStaId(ppdu);
    }

  private:
    uint16_t m_staId;
};

class HeDlMuTestSpectrumWifiPhy : public SpectrumWifiPhy
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::HeDlMuTestSpectrumWifiPhy")
                .SetParent<SpectrumWifiPhy>()
                .AddConstructor<HeDlMuTestSpectrumWifiPhy>()
                .AddAttribute("StaId",
                              "STA-ID returned while receiving a DL MU PPDU",
                              UintegerValue(0),
                              MakeUintegerAccessor(&HeDlMuTestSpectrumWifiPhy::m_staId),
                              MakeUintegerChecker<uint16_t>());
        return tid;
    }

  private:
    void DoInitialize() override
    {
        m_testHePhy = Create<HeDlMuTestHePhy>(m_staId);
        m_testHePhy->SetOwner(this);
        m_phyEntities[WIFI_MOD_CLASS_HE] = m_testHePhy;
        SpectrumWifiPhy::DoInitialize();
    }

    void DoDispose() override
    {
        m_testHePhy = nullptr;
        SpectrumWifiPhy::DoDispose();
    }

    uint16_t m_staId{0};
    Ptr<HeDlMuTestHePhy> m_testHePhy;
};

class HeDlMuTestSpectrumWifiPhyHelper : public SpectrumWifiPhyHelper
{
  public:
    explicit HeDlMuTestSpectrumWifiPhyHelper(uint16_t staId)
    {
        m_phys.at(0).SetTypeId(HeDlMuTestSpectrumWifiPhy::GetTypeId());
        Set("StaId", UintegerValue(staId));
    }
};

SionnaMimoConfiguration
MakeOpenLoop2x2Configuration()
{
    SionnaMimoConfiguration configuration;
    configuration.mode = SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
    configuration.txArray.polarization = SionnaMimoPolarization::DUAL;
    configuration.rxArray.numColumns = 2;
    configuration.numTxAntennas = 2;
    configuration.numRxAntennas = 2;
    configuration.maxNss = 2;
    return configuration;
}

class SionnaMimoHelperDefaultsTest : public TestCase
{
  public:
    SionnaMimoHelperDefaultsTest()
        : TestCase("SionnaMimoHelper defaults preserve the legacy SISO configuration")
    {
    }

  private:
    void DoRun() override
    {
        SionnaMimoHelper helper;
        const auto& configuration = helper.GetConfiguration();

        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(configuration.mode),
                              static_cast<int>(SionnaMimoMode::SISO),
                              "The default mode is not SISO");
        NS_TEST_ASSERT_MSG_EQ(configuration.txArray.numRows, 1, "TX rows changed");
        NS_TEST_ASSERT_MSG_EQ(configuration.txArray.numColumns, 1, "TX columns changed");
        NS_TEST_ASSERT_MSG_EQ(configuration.rxArray.numRows, 1, "RX rows changed");
        NS_TEST_ASSERT_MSG_EQ(configuration.rxArray.numColumns, 1, "RX columns changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(configuration.txArray.verticalSpacing,
                                  0.5,
                                  1e-12,
                                  "TX vertical spacing changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(configuration.txArray.horizontalSpacing,
                                  0.5,
                                  1e-12,
                                  "TX horizontal spacing changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(configuration.rxArray.verticalSpacing,
                                  0.5,
                                  1e-12,
                                  "RX vertical spacing changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(configuration.rxArray.horizontalSpacing,
                                  0.5,
                                  1e-12,
                                  "RX horizontal spacing changed");
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<int>(configuration.txArray.pattern),
            static_cast<int>(SionnaMimoAntennaPattern::TR38901),
            "The default TX element pattern is not the legacy tr38901 pattern");
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<int>(configuration.rxArray.pattern),
            static_cast<int>(SionnaMimoAntennaPattern::DIPOLE),
            "The default RX element pattern is not the legacy dipole pattern");
        NS_TEST_ASSERT_MSG_EQ(configuration.txArray.GetNumAntennas(),
                              1,
                              "The default TX array is not 1x1");
        NS_TEST_ASSERT_MSG_EQ(configuration.rxArray.GetNumAntennas(),
                              1,
                              "The default RX array is not 1x1");
        NS_TEST_ASSERT_MSG_EQ(configuration.numTxAntennas, 1, "Default TX antennas changed");
        NS_TEST_ASSERT_MSG_EQ(configuration.numRxAntennas, 1, "Default RX antennas changed");
        NS_TEST_ASSERT_MSG_EQ(configuration.maxNss, 1, "Default maximum NSS changed");
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<int>(configuration.receiverType),
            static_cast<int>(SionnaMimoReceiverType::LINEAR_MMSE),
            "The default receiver is not linear MMSE");
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<int>(configuration.channelNormalization),
            static_cast<int>(SionnaMimoChannelNormalization::UNIT_AVERAGE_PAIR_POWER),
            "The default CFR normalization changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(configuration.receiverNoiseFigureDb,
                                  7.0,
                                  1e-12,
                                  "The default no longer matches WifiPhy's 7 dB noise figure");
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<int>(configuration.effectiveSinrModel),
            static_cast<int>(SionnaMimoEffectiveSinrModel::EESM),
            "The default effective-SINR model is not EESM");
        NS_TEST_ASSERT_MSG_EQ(configuration.eesmBetaLinear.empty(),
                              true,
                              "The API must not invent EESM calibration values");
        NS_TEST_ASSERT_MSG_EQ(configuration.nodeOrientations.empty(),
                              true,
                              "Default nodes must not receive an implicit orientation override");
        NS_TEST_ASSERT_MSG_EQ(configuration.precodingMatrix.has_value(),
                              false,
                              "The default must preserve the Phase-2 path without precoding");
        NS_TEST_ASSERT_MSG_EQ(configuration.beamforming.has_value(),
                              false,
                              "The default must not configure channel-dependent beamforming");
        NS_TEST_ASSERT_MSG_EQ(helper.HasSelectedPrecoding(),
                              false,
                              "The default helper unexpectedly reports selected precoding");
        NS_TEST_ASSERT_MSG_EQ(helper.HasPrecodingMatrix(),
                              false,
                              "The default helper unexpectedly reports fixed precoding");

        const auto defaultOrientation = helper.GetNodeOrientation(17);
        NS_TEST_ASSERT_MSG_EQ_TOL(defaultOrientation.azimuthDegrees,
                                  0.0,
                                  1e-12,
                                  "Default azimuth is not zero");
        NS_TEST_ASSERT_MSG_EQ_TOL(defaultOrientation.elevationDegrees,
                                  0.0,
                                  1e-12,
                                  "Default elevation is not zero");
        NS_TEST_ASSERT_MSG_EQ_TOL(defaultOrientation.rollDegrees,
                                  0.0,
                                  1e-12,
                                  "Default roll is not zero");

        const auto& supportedModes = SionnaMimoHelper::GetSupportedModes();
        NS_TEST_ASSERT_MSG_EQ(supportedModes.size(), 4, "Unexpected number of supported modes");
        NS_TEST_ASSERT_MSG_EQ(SionnaMimoHelper::IsModeSupported(SionnaMimoMode::SISO),
                              true,
                              "SISO is not reported as supported");
        NS_TEST_ASSERT_MSG_EQ(
            SionnaMimoHelper::IsModeSupported(
                SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING),
            true,
            "Open-loop SU-MIMO is not reported as supported");
        NS_TEST_ASSERT_MSG_EQ(
            SionnaMimoHelper::IsModeSupported(SionnaMimoMode::SU_BEAMFORMING),
            true,
            "Completed SU beamforming is not reported as supported");
        NS_TEST_ASSERT_MSG_EQ(SionnaMimoHelper::IsModeSupported(SionnaMimoMode::MU_MIMO),
                              true,
                              "Completed MU-MIMO is not reported as supported");
    }
};

class SionnaMimoHelperValidConfigurationTest : public TestCase
{
  public:
    SionnaMimoHelperValidConfigurationTest()
        : TestCase("SionnaMimoHelper accepts a complete open-loop 2x2 contract")
    {
    }

  private:
    void DoRun() override
    {
        auto configuration = MakeOpenLoop2x2Configuration();
        configuration.nodeOrientations.emplace(7, SionnaMimoOrientation{45.0, -10.0, 5.0});
        configuration.eesmBetaLinear.emplace(5, 2.0);

        SionnaMimoHelper::Validate(configuration);
        SionnaMimoHelper::ValidateForHeMcs(configuration, 5);
        SionnaMimoHelper helper(configuration);

        NS_TEST_ASSERT_MSG_EQ(helper.GetConfiguration().txArray.GetNumAntennas(),
                              2,
                              "Dual-polarized TX element did not create two antenna ports");
        NS_TEST_ASSERT_MSG_EQ(helper.GetConfiguration().rxArray.GetNumAntennas(),
                              2,
                              "Two-column RX array did not create two antenna ports");
        NS_TEST_ASSERT_MSG_EQ(helper.HasNodeOrientation(7),
                              true,
                              "Explicit node orientation was lost");
        NS_TEST_ASSERT_MSG_EQ_TOL(helper.GetNodeOrientation(7).azimuthDegrees,
                                  45.0,
                                  1e-12,
                                  "Node azimuth changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(helper.GetEesmBeta(5),
                                  2.0,
                                  1e-12,
                                  "Configured EESM beta changed");

        helper.SetNodeOrientation(8, {-180.0, 90.0, 180.0});
        NS_TEST_ASSERT_MSG_EQ(helper.HasNodeOrientation(8),
                              true,
                              "Boundary orientation was rejected");
        helper.ClearNodeOrientation(8);
        NS_TEST_ASSERT_MSG_EQ(helper.HasNodeOrientation(8),
                              false,
                              "Cleared orientation is still present");
        helper.SetEesmBeta(11, 3.5);
        NS_TEST_ASSERT_MSG_EQ_TOL(helper.GetEesmBeta(11),
                                  3.5,
                                  1e-12,
                                  "Setter did not retain the EESM beta");

        const auto identity = MimoPrecodingMatrix::FrequencyFlat(
            2,
            2,
            2,
            {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}});
        helper.SetPrecodingMatrix(identity);
        NS_TEST_ASSERT_MSG_EQ(helper.HasPrecodingMatrix(),
                              true,
                              "Helper did not retain valid identity precoding");
        NS_TEST_ASSERT_MSG_EQ(helper.GetPrecodingMatrix().GetNumSubcarriers(),
                              2,
                              "Helper changed the precoding subcarrier dimension");

        auto selectedConfiguration = MakeOpenLoop2x2Configuration();
        selectedConfiguration.maxNss = 1;
        selectedConfiguration.precodingMatrix = MimoPrecodingMatrix(
            2,
            2,
            1,
            {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 1.0}});
        SionnaMimoHelper::Validate(selectedConfiguration);

        selectedConfiguration.rxArray.numColumns = 1;
        selectedConfiguration.numRxAntennas = 1;
        SionnaMimoHelper::Validate(selectedConfiguration);
    }
};

class SionnaMimoHelperBeamformingSelectionTest : public TestCase
{
  public:
    SionnaMimoHelperBeamformingSelectionTest()
        : TestCase("SionnaMimoHelper selects MRT, SVD feedback and aging, and water-filling W[k]")
    {
    }

  private:
    void DoRun() override
    {
        SionnaMimoConfiguration mrtConfiguration;
        mrtConfiguration.mode = SionnaMimoMode::SU_BEAMFORMING;
        mrtConfiguration.txArray.numColumns = 2;
        mrtConfiguration.numTxAntennas = 2;
        mrtConfiguration.beamforming = SionnaMimoBeamformingConfiguration{};
        mrtConfiguration.beamforming->method = SionnaMimoBeamformingMethod::MRT;

        MimoChannelMatrix miso(2, 1, 2);
        miso.Set(0, 0, 0, {1.0, 1.0});
        miso.Set(0, 0, 1, {2.0, -1.0});
        miso.Set(1, 0, 0, {-1.0, 0.5});
        miso.Set(1, 0, 1, {0.5, 2.0});

        SionnaMimoHelper mrtHelper(mrtConfiguration);
        const auto& mrtSelection =
            mrtHelper.SelectPrecoding(miso, 4, 7, MilliSeconds(3), MilliSeconds(3));
        const auto directMrt = MimoPrecodingMatrix::MaximumRatioTransmission(miso);
        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(mrtSelection.GetMethod()),
                              static_cast<int>(SionnaMimoBeamformingMethod::MRT),
                              "The helper did not report MRT");
        NS_TEST_ASSERT_MSG_EQ(mrtSelection.HasSvdResult(),
                              false,
                              "MRT unexpectedly has SVD diagnostics");
        NS_TEST_ASSERT_MSG_EQ(mrtSelection.GetCsiAge().GetNanoSeconds(),
                              0,
                              "Current MRT CSI did not have zero age");
        NS_TEST_ASSERT_MSG_EQ(mrtHelper.HasSelectedPrecoding(),
                              true,
                              "The helper did not retain MRT");
        for (std::size_t i = 0; i < directMrt.GetCoefficients().size(); ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(mrtSelection.GetPrecodingMatrix().GetCoefficients().at(i),
                                  directMrt.GetCoefficients().at(i),
                                  "Helper MRT changed the completed factory result");
        }

        SionnaMimoConfiguration svdConfiguration = MakeOpenLoop2x2Configuration();
        svdConfiguration.mode = SionnaMimoMode::SU_BEAMFORMING;
        svdConfiguration.maxNss = 1;
        svdConfiguration.beamforming = SionnaMimoBeamformingConfiguration{};
        svdConfiguration.beamforming->feedback.codebook = MimoFeedbackCodebook::SU_FINE;
        svdConfiguration.beamforming->csiTiming.updateInterval = MilliSeconds(5);
        svdConfiguration.beamforming->csiTiming.feedbackDelay = MilliSeconds(2);
        svdConfiguration.nodeOrientations.emplace(
            4,
            SionnaMimoOrientation{15.0, -5.0, 2.0});

        MimoChannelMatrix mimo(2, 2, 2);
        for (uint32_t sc = 0; sc < 2; ++sc)
        {
            mimo.Set(sc, 0, 0, {2.0, 0.0});
            mimo.Set(sc, 1, 1, {1.0, 0.0});
        }

        SionnaMimoHelper svdHelper(svdConfiguration);
        const auto& svdSelection =
            svdHelper.SelectPrecoding(mimo, 4, 7, MilliSeconds(10), MilliSeconds(13));
        NS_TEST_ASSERT_MSG_EQ(svdSelection.HasSvdResult(),
                              true,
                              "SVD diagnostics were not retained");
        NS_TEST_ASSERT_MSG_EQ(svdSelection.HasCsiSelectionResult(),
                              true,
                              "Feedback/aging diagnostics were not retained");
        NS_TEST_ASSERT_MSG_EQ(svdSelection.GetCsiAge().GetMilliSeconds(),
                              3,
                              "The configured CSI age is wrong");
        NS_TEST_ASSERT_MSG_EQ(
            svdSelection.GetCsiSelectionResult().GetDiagnostics().GetFeedbackBitCount(),
            20,
            "Two fine-codebook 2x1 subcarriers should use twenty angle bits");
        NS_TEST_ASSERT_MSG_EQ(
            svdSelection.GetCsiSelectionResult().GetStateIdentity().txNodeId,
            4,
            "The directed TX identity was lost");
        NS_TEST_ASSERT_MSG_EQ_TOL(svdSelection.GetPrecodingMatrix().GetPower(0),
                                  1.0,
                                  1e-12,
                                  "Quantized SVD changed total transmit power");
        NS_TEST_ASSERT_MSG_EQ_TOL(svdSelection.GetPowerFraction(0, 0),
                                  1.0,
                                  1e-12,
                                  "Single-stream SVD did not receive all power");

        SionnaHelper sionna("", "inproc://sionna-mimo-phase35-selection");
        SpectrumWifiPhyHelper txPhy;
        SpectrumWifiPhyHelper rxPhy;
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        svdHelper.Apply(sionna, txPhy, rxPhy, adapter);
        const auto& adapterCoefficients =
            adapter->GetPrecodingMatrix().GetCoefficients();
        const auto& selectedCoefficients =
            svdSelection.GetPrecodingMatrix().GetCoefficients();
        NS_TEST_ASSERT_MSG_EQ(adapterCoefficients.size(),
                              selectedCoefficients.size(),
                              "Apply changed the selected W[k] size");
        for (std::size_t i = 0; i < adapterCoefficients.size(); ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(adapterCoefficients.at(i),
                                  selectedCoefficients.at(i),
                                  "Apply changed a selected fixed W[k] coefficient");
        }

        SionnaMimoConfiguration waterConfiguration = svdConfiguration;
        waterConfiguration.maxNss = 2;
        waterConfiguration.nodeOrientations.clear();
        waterConfiguration.beamforming->feedback.codebook =
            MimoFeedbackCodebook::UNQUANTIZED;
        waterConfiguration.beamforming->csiTiming = {};
        waterConfiguration.beamforming->svd.powerAllocation =
            MimoPowerAllocationMode::WATER_FILLING;
        waterConfiguration.beamforming->svd.referenceSnrLinear = 0.5;
        SionnaMimoHelper waterHelper(waterConfiguration);
        const auto& water =
            waterHelper.SelectPrecoding(mimo, 4, 7, MilliSeconds(0), MilliSeconds(0));
        NS_TEST_ASSERT_MSG_EQ(water.HasSvdResult(),
                              true,
                              "Water-filling lost the SVD diagnostics");
        NS_TEST_ASSERT_MSG_EQ(water.HasCsiSelectionResult(),
                              false,
                              "Water-filling was incorrectly encoded as compressed feedback");
        NS_TEST_ASSERT_MSG_EQ(water.GetSvdResult().GetNumActiveModes(0),
                              1,
                              "Low-SNR water-filling should activate only the strong mode");
        NS_TEST_ASSERT_MSG_EQ_TOL(water.GetPowerFraction(0, 0),
                                  1.0,
                                  1e-12,
                                  "Water-filling strong-mode fraction is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(water.GetPowerFraction(0, 1),
                                  0.0,
                                  1e-12,
                                  "Water-filling weak mode should be inactive");
        NS_TEST_ASSERT_MSG_EQ_TOL(water.GetPrecodingMatrix().GetPower(0),
                                  2.0,
                                  1e-12,
                                  "Water-filling created or removed transmit power");

        svdHelper.SetNodeOrientation(4, {16.0, -5.0, 2.0});
        NS_TEST_ASSERT_MSG_EQ(svdHelper.HasSelectedPrecoding(),
                              false,
                              "Changing a CSI identity field retained a stale selection");
        Simulator::Destroy();
    }
};

class SionnaMimoHelperInvalidConfigurationTest : public TestCase
{
  public:
    SionnaMimoHelperInvalidConfigurationTest()
        : TestCase("SionnaMimoHelper centrally rejects invalid and unsupported contracts")
    {
    }

  private:
    void DoRun() override
    {
        auto expectRejected = [this](const SionnaMimoConfiguration& configuration,
                                     const std::string& description) {
            bool rejected = false;
            try
            {
                SionnaMimoHelper::Validate(configuration);
            }
            catch (const std::invalid_argument& exception)
            {
                rejected = std::string(exception.what()).find("SionnaMimoHelper: ") == 0;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, description);
        };

        SionnaMimoConfiguration configuration;
        configuration.txArray.numRows = 0;
        expectRejected(configuration, "A zero-row TX array was accepted");

        configuration = {};
        configuration.rxArray.horizontalSpacing =
            std::numeric_limits<double>::quiet_NaN();
        expectRejected(configuration, "A non-finite array spacing was accepted");

        configuration = {};
        configuration.txArray.pattern = static_cast<SionnaMimoAntennaPattern>(99);
        expectRejected(configuration, "An unknown Sionna element pattern was accepted");

        configuration = {};
        configuration.txArray.polarization = static_cast<SionnaMimoPolarization>(99);
        expectRejected(configuration, "An unknown Sionna polarization was accepted");

        configuration = {};
        configuration.txArray.numRows = 3;
        configuration.txArray.numColumns = 3;
        expectRejected(configuration, "An array exceeding ns-3's eight-antenna limit was accepted");

        configuration = {};
        configuration.numTxAntennas = 2;
        expectRejected(configuration, "An antenna count inconsistent with the array was accepted");

        configuration = {};
        configuration.maxNss = 0;
        expectRejected(configuration, "Maximum NSS zero was accepted");

        configuration = MakeOpenLoop2x2Configuration();
        configuration.maxNss = 1;
        expectRejected(configuration,
                       "Open-loop MIMO with fewer streams than TX antennas was accepted");

        configuration = MakeOpenLoop2x2Configuration();
        configuration.precodingMatrix = MimoPrecodingMatrix::FrequencyFlat(
            1,
            2,
            1,
            {{1.0, 0.0}, {0.0, 0.0}});
        expectRejected(configuration,
                       "A precoding stream count inconsistent with maximum NSS was accepted");

        configuration = MakeOpenLoop2x2Configuration();
        configuration.precodingMatrix = MimoPrecodingMatrix::FrequencyFlat(
            1,
            1,
            1,
            {{1.0, 0.0}});
        expectRejected(configuration,
                       "A precoding TX dimension inconsistent with the array was accepted");

        configuration = MakeOpenLoop2x2Configuration();
        configuration.rxArray.numColumns = 1;
        configuration.numRxAntennas = 1;
        expectRejected(configuration, "Overloaded open-loop MIMO was accepted");

        configuration = {};
        configuration.mode = SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
        expectRejected(configuration, "One-antenna spatial multiplexing was accepted");

        configuration = MakeOpenLoop2x2Configuration();
        configuration.mode = SionnaMimoMode::SISO;
        expectRejected(configuration, "A multi-antenna SISO contract was accepted");

        configuration = {};
        configuration.mode = SionnaMimoMode::SU_BEAMFORMING;
        expectRejected(configuration, "Beamforming without an explicit method was accepted");
        configuration = MakeOpenLoop2x2Configuration();
        configuration.mode = SionnaMimoMode::SU_BEAMFORMING;
        configuration.maxNss = 1;
        configuration.beamforming = SionnaMimoBeamformingConfiguration{};
        configuration.precodingMatrix = MimoPrecodingMatrix::FrequencyFlat(
            1,
            2,
            1,
            {{1.0, 0.0}, {0.0, 0.0}});
        expectRejected(configuration,
                       "Beamforming combined with user-provided fixed W[k] was accepted");

        configuration.precodingMatrix.reset();
        configuration.beamforming->svd.powerAllocation =
            MimoPowerAllocationMode::WATER_FILLING;
        configuration.beamforming->feedback.codebook = MimoFeedbackCodebook::SU_COARSE;
        expectRejected(configuration,
                       "Water-filling combined with quantized feedback was accepted");

        configuration.beamforming->feedback.codebook = MimoFeedbackCodebook::UNQUANTIZED;
        configuration.beamforming->csiTiming.feedbackDelay = MilliSeconds(1);
        expectRejected(configuration,
                       "Water-filling combined with CSI aging was accepted");

        configuration = MakeOpenLoop2x2Configuration();
        configuration.mode = SionnaMimoMode::SU_BEAMFORMING;
        configuration.maxNss = 1;
        configuration.beamforming = SionnaMimoBeamformingConfiguration{};
        configuration.beamforming->method = SionnaMimoBeamformingMethod::MRT;
        expectRejected(configuration, "MRT with more than one RX antenna was accepted");

        configuration.rxArray.numColumns = 1;
        configuration.numRxAntennas = 1;
        configuration.beamforming->feedback.codebook = MimoFeedbackCodebook::SU_FINE;
        expectRejected(configuration, "Quantized MRT feedback was accepted");

        configuration = MakeOpenLoop2x2Configuration();
        configuration.beamforming = SionnaMimoBeamformingConfiguration{};
        expectRejected(configuration,
                       "Spatial multiplexing with channel-dependent options was accepted");

        configuration = {};
        configuration.mode = SionnaMimoMode::MU_MIMO;
        expectRejected(configuration, "an incomplete MU-MIMO configuration was accepted");

        configuration = {};
        configuration.receiverType = SionnaMimoReceiverType::ZERO_FORCING;
        expectRejected(configuration, "An unimplemented receiver was accepted");

        configuration = {};
        configuration.channelNormalization = SionnaMimoChannelNormalization::NONE;
        expectRejected(configuration, "An unimplemented normalization was accepted");

        configuration = {};
        configuration.effectiveSinrModel = SionnaMimoEffectiveSinrModel::MUTUAL_INFORMATION;
        expectRejected(configuration, "MI was accepted as a calibrated HE PER mapping");

        configuration = {};
        configuration.receiverNoiseFigureDb = -1.0;
        expectRejected(configuration, "A negative receiver noise figure was accepted");

        configuration = {};
        configuration.receiverNoiseFigureDb =
            std::numeric_limits<double>::infinity();
        expectRejected(configuration, "A non-finite receiver noise figure was accepted");

        configuration = {};
        configuration.nodeOrientations.emplace(4, SionnaMimoOrientation{0.0, 91.0, 0.0});
        expectRejected(configuration, "An out-of-range node elevation was accepted");

        configuration = {};
        configuration.eesmBetaLinear.emplace(12, 1.0);
        expectRejected(configuration, "An EESM beta for an invalid HE MCS was accepted");

        configuration = {};
        configuration.eesmBetaLinear.emplace(5, 0.0);
        expectRejected(configuration, "A non-positive EESM beta was accepted");

        configuration = MakeOpenLoop2x2Configuration();
        bool missingBetaRejected = false;
        try
        {
            SionnaMimoHelper::ValidateForHeMcs(configuration, 5);
        }
        catch (const std::invalid_argument&)
        {
            missingBetaRejected = true;
        }
        NS_TEST_ASSERT_MSG_EQ(missingBetaRejected,
                              true,
                              "Open-loop MIMO without an MCS-specific EESM beta was accepted");

        configuration = {};
        bool invalidMcsRejected = false;
        try
        {
            SionnaMimoHelper::ValidateForHeMcs(configuration, 12);
        }
        catch (const std::invalid_argument&)
        {
            invalidMcsRejected = true;
        }
        NS_TEST_ASSERT_MSG_EQ(invalidMcsRejected, true, "HE MCS 12 was accepted");

        SionnaMimoHelper helper;
        configuration = MakeOpenLoop2x2Configuration();
        configuration.maxNss = 1;
        try
        {
            helper.SetConfiguration(configuration);
        }
        catch (const std::invalid_argument&)
        {
        }
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<int>(helper.GetConfiguration().mode),
            static_cast<int>(SionnaMimoMode::SISO),
            "A rejected replacement partially changed the helper's stored configuration");
    }
};

void
ConfigureManualPhy(SpectrumWifiPhyHelper& phyHelper,
                   uint32_t numAntennas,
                   uint32_t maxNss,
                   double noiseFigureDb)
{
    phyHelper.Set("Antennas", UintegerValue(numAntennas));
    phyHelper.Set("MaxSupportedTxSpatialStreams", UintegerValue(maxNss));
    phyHelper.Set("MaxSupportedRxSpatialStreams", UintegerValue(maxNss));
    phyHelper.Set("RxNoiseFigure", DoubleValue(noiseFigureDb));
}

Ptr<SpectrumWifiPhy>
InstallConfiguredPhy(SpectrumWifiPhyHelper& phyHelper)
{
    phyHelper.SetChannel(CreateObject<MultiModelSpectrumChannel>());

    NodeContainer node;
    node.Create(1);

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211ax);
    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");

    NetDeviceContainer devices = wifi.Install(phyHelper, mac, node);
    Ptr<WifiNetDevice> device = DynamicCast<WifiNetDevice>(devices.Get(0));
    return DynamicCast<SpectrumWifiPhy>(device->GetPhy());
}

enum class HelperIntegrationCase
{
    SISO,
    OPEN_LOOP_2X2,
    FIXED_PRECODING_2X1
};

class SionnaMimoHelperIntegrationTest : public TestCase
{
  public:
    explicit SionnaMimoHelperIntegrationTest(HelperIntegrationCase integrationCase)
        : TestCase(integrationCase == HelperIntegrationCase::SISO
                       ? "manual and SionnaMimoHelper 1x1 integrations are equivalent"
                   : integrationCase == HelperIntegrationCase::OPEN_LOOP_2X2
                       ? "manual and SionnaMimoHelper open-loop 2x2 integrations are equivalent"
                       : "manual and SionnaMimoHelper frequency-selective 2x1 precoding are "
                         "equivalent"),
          m_integrationCase(integrationCase)
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint8_t mcs = 3;
        constexpr uint8_t staleMcs = 4;
        constexpr double betaLinear = 1.75;
        constexpr double noiseFigureDb = 5.5;
        constexpr double bandwidthHz = 20e6;

        const bool useMimo = m_integrationCase != HelperIntegrationCase::SISO;
        const bool usePrecoding =
            m_integrationCase == HelperIntegrationCase::FIXED_PRECODING_2X1;
        SionnaMimoConfiguration configuration =
            useMimo ? MakeOpenLoop2x2Configuration() : SionnaMimoConfiguration{};
        if (usePrecoding)
        {
            configuration.maxNss = 1;
            configuration.precodingMatrix = MimoPrecodingMatrix(
                2,
                2,
                1,
                {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 1.0}});
        }
        configuration.receiverNoiseFigureDb = noiseFigureDb;
        configuration.eesmBetaLinear.emplace(mcs, betaLinear);
        configuration.nodeOrientations.emplace(
            7,
            SionnaMimoOrientation{30.0, -15.0, 5.0});

        SionnaHelper manualSionna("", "inproc://sionna-mimo-manual");
        manualSionna.ConfigureTransmitArray(
            configuration.txArray.numRows,
            configuration.txArray.numColumns,
            configuration.txArray.verticalSpacing,
            configuration.txArray.horizontalSpacing,
            "tr38901",
            useMimo ? "VH" : "V");
        manualSionna.ConfigureReceiveArray(
            configuration.rxArray.numRows,
            configuration.rxArray.numColumns,
            configuration.rxArray.verticalSpacing,
            configuration.rxArray.horizontalSpacing,
            "dipole",
            "V");
        manualSionna.ConfigureNodeOrientation(7, 30.0, -15.0, 5.0);

        SpectrumWifiPhyHelper manualTxPhyHelper;
        SpectrumWifiPhyHelper manualRxPhyHelper;
        ConfigureManualPhy(manualTxPhyHelper,
                           configuration.numTxAntennas,
                           configuration.maxNss,
                           noiseFigureDb);
        ConfigureManualPhy(manualRxPhyHelper,
                           configuration.numRxAntennas,
                           configuration.maxNss,
                           noiseFigureDb);
        auto manualModel = CreateObject<SionnaSpectrumPropagationLossModel>();
        manualModel->SetReceiverNoiseFigureDb(noiseFigureDb);
        manualModel->SetEesmBeta(mcs, betaLinear);
        if (configuration.precodingMatrix)
        {
            manualModel->SetPrecodingMatrix(*configuration.precodingMatrix);
        }

        SionnaHelper highLevelSionna("", "inproc://sionna-mimo-high-level");
        SpectrumWifiPhyHelper highLevelTxPhyHelper;
        SpectrumWifiPhyHelper highLevelRxPhyHelper;
        auto highLevelModel = CreateObject<SionnaSpectrumPropagationLossModel>();
        highLevelSionna.ConfigureNodeOrientation(8, 1.0, 2.0, 3.0);
        highLevelModel->SetEesmBeta(staleMcs, 2.0);
        SionnaMimoHelper(configuration).ApplyForHeMcs(mcs,
                                                      highLevelSionna,
                                                      highLevelTxPhyHelper,
                                                      highLevelRxPhyHelper,
                                                      highLevelModel);
        ns3sionna::SimInitMessage manualMessage;
        ns3sionna::SimInitMessage highLevelMessage;
        manualMessage.add_nodes()->set_id(7);
        manualMessage.add_nodes()->set_id(8);
        highLevelMessage.add_nodes()->set_id(7);
        highLevelMessage.add_nodes()->set_id(8);
        manualSionna.PopulateMimoConfiguration(manualMessage);
        highLevelSionna.PopulateMimoConfiguration(highLevelMessage);
        NS_TEST_ASSERT_MSG_EQ(
            highLevelMessage.SerializeAsString(),
            manualMessage.SerializeAsString(),
            "Helper/manual Sionna array or orientation IPC configuration differs");

        NS_TEST_ASSERT_MSG_EQ_TOL(
            highLevelModel->GetReceiverNoiseFigureDb(),
            manualModel->GetReceiverNoiseFigureDb(),
            1e-12,
            "Helper and manual adapter noise figures differ");
        NS_TEST_ASSERT_MSG_EQ(highLevelModel->HasEesmBeta(mcs),
                              true,
                              "Helper did not configure the requested EESM beta");
        NS_TEST_ASSERT_MSG_EQ(highLevelModel->HasEesmBeta(staleMcs),
                              false,
                              "Helper retained an EESM beta absent from the replacement "
                              "configuration");
        NS_TEST_ASSERT_MSG_EQ_TOL(highLevelModel->GetEesmBeta(mcs),
                                  manualModel->GetEesmBeta(mcs),
                                  1e-12,
                                  "Helper and manual EESM betas differ");
        NS_TEST_ASSERT_MSG_EQ(highLevelModel->HasPrecodingMatrix(),
                              manualModel->HasPrecodingMatrix(),
                              "Helper/manual fixed-precoding presence differs");
        if (configuration.precodingMatrix)
        {
            const auto& highLevelCoefficients =
                highLevelModel->GetPrecodingMatrix().GetCoefficients();
            const auto& manualCoefficients =
                manualModel->GetPrecodingMatrix().GetCoefficients();
            NS_TEST_ASSERT_MSG_EQ(highLevelCoefficients.size(),
                                  manualCoefficients.size(),
                                  "Helper/manual fixed-precoding sizes differ");
            for (std::size_t i = 0; i < highLevelCoefficients.size(); ++i)
            {
                NS_TEST_ASSERT_MSG_EQ(highLevelCoefficients.at(i),
                                      manualCoefficients.at(i),
                                      "Helper/manual fixed-precoding coefficient differs");
            }
        }

        Ptr<SpectrumWifiPhy> manualTxPhy = InstallConfiguredPhy(manualTxPhyHelper);
        Ptr<SpectrumWifiPhy> manualRxPhy = InstallConfiguredPhy(manualRxPhyHelper);
        Ptr<SpectrumWifiPhy> highLevelTxPhy = InstallConfiguredPhy(highLevelTxPhyHelper);
        Ptr<SpectrumWifiPhy> highLevelRxPhy = InstallConfiguredPhy(highLevelRxPhyHelper);

        auto comparePhy = [this, &configuration](Ptr<SpectrumWifiPhy> manual,
                                                 Ptr<SpectrumWifiPhy> highLevel,
                                                 uint8_t expectedAntennas,
                                                 const std::string& role) {
            NS_TEST_ASSERT_MSG_EQ(highLevel->GetNumberOfAntennas(),
                                  manual->GetNumberOfAntennas(),
                                  role << " helper/manual antenna counts differ");
            NS_TEST_ASSERT_MSG_EQ(highLevel->GetNumberOfAntennas(),
                                  expectedAntennas,
                                  role << " antenna count was not applied");
            NS_TEST_ASSERT_MSG_EQ(highLevel->GetMaxSupportedTxSpatialStreams(),
                                  manual->GetMaxSupportedTxSpatialStreams(),
                                  role << " helper/manual TX NSS limits differ");
            NS_TEST_ASSERT_MSG_EQ(highLevel->GetMaxSupportedRxSpatialStreams(),
                                  manual->GetMaxSupportedRxSpatialStreams(),
                                  role << " helper/manual RX NSS limits differ");
            NS_TEST_ASSERT_MSG_EQ(highLevel->GetMaxSupportedTxSpatialStreams(),
                                  configuration.maxNss,
                                  role << " TX NSS limit was not applied");
            NS_TEST_ASSERT_MSG_EQ(highLevel->GetMaxSupportedRxSpatialStreams(),
                                  configuration.maxNss,
                                  role << " RX NSS limit was not applied");
        };
        comparePhy(manualTxPhy,
                   highLevelTxPhy,
                   static_cast<uint8_t>(configuration.numTxAntennas),
                   "TX");
        comparePhy(manualRxPhy,
                   highLevelRxPhy,
                   static_cast<uint8_t>(configuration.numRxAntennas),
                   "RX");

        MimoChannelMatrix channel(2,
                                  configuration.numRxAntennas,
                                  configuration.numTxAntennas);
        if (useMimo)
        {
            for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
            {
                channel.Set(sc, 0, 0, {std::sqrt(2.0), 0.0});
                channel.Set(sc, 1, 1, {std::sqrt(2.0), 0.0});
            }
        }
        else
        {
            channel.Set(0, 0, 0, {std::sqrt(0.5), 0.0});
            channel.Set(1, 0, 0, {std::sqrt(1.5), 0.0});
        }

        const auto noiseBudget =
            WifiLinkBudget::Calculate(0.0, 0.0, bandwidthHz, 290.0, noiseFigureDb);
        const double receivedPowerW = 12.0 * noiseBudget.GetReceiverNoisePowerW();
        const auto txVector = MakeHeTxVector(HePhy::GetHeMcs(mcs),
                                             static_cast<uint8_t>(configuration.maxNss));
        const auto manualResult = manualModel->CalculateMimoRx(
            channel,
            receivedPowerW,
            txVector,
            1200,
            static_cast<uint8_t>(configuration.numTxAntennas),
            static_cast<uint8_t>(configuration.numRxAntennas),
            bandwidthHz,
            noiseFigureDb,
            CreateObject<TableBasedErrorRateModel>());
        const auto highLevelResult = highLevelModel->CalculateMimoRx(
            channel,
            receivedPowerW,
            txVector,
            1200,
            static_cast<uint8_t>(configuration.numTxAntennas),
            static_cast<uint8_t>(configuration.numRxAntennas),
            bandwidthHz,
            noiseFigureDb,
            CreateObject<TableBasedErrorRateModel>());

        const auto& manualLink = manualResult.GetLinkResult();
        const auto& highLevelLink = highLevelResult.GetLinkResult();
        NS_TEST_ASSERT_MSG_EQ(highLevelLink.GetEffectiveSinrLinear(),
                              manualLink.GetEffectiveSinrLinear(),
                              "Helper/manual effective SINR differs");
        NS_TEST_ASSERT_MSG_EQ(highLevelLink.GetPacketErrorRate(),
                              manualLink.GetPacketErrorRate(),
                              "Helper/manual PER differs");
        NS_TEST_ASSERT_MSG_EQ(highLevelLink.GetMimoResult().GetMinimumChannelRank(),
                              manualLink.GetMimoResult().GetMinimumChannelRank(),
                              "Helper/manual channel rank differs");
        for (uint32_t sc = 0;
             sc < manualLink.GetMimoResult().GetNumSubcarriers();
             ++sc)
        {
            for (uint32_t stream = 0;
                 stream < manualLink.GetMimoResult().GetNumStreams();
                 ++stream)
            {
                NS_TEST_ASSERT_MSG_EQ(
                    highLevelLink.GetMimoResult().GetSinrLinear(sc, stream),
                    manualLink.GetMimoResult().GetSinrLinear(sc, stream),
                    "Helper/manual post-MMSE SINR differs");
            }
        }

        Simulator::Destroy();
    }

    HelperIntegrationCase m_integrationCase;
};

class MimoChannelMatrixLayoutTest : public TestCase
{
  public:
    MimoChannelMatrixLayoutTest()
        : TestCase("MIMO CFR tensor uses subcarrier-RX-TX flat ordering")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix channel(2, 2, 2);
        uint32_t value = 1;
        for (uint32_t sc = 0; sc < 2; ++sc)
        {
            for (uint32_t rx = 0; rx < 2; ++rx)
            {
                for (uint32_t tx = 0; tx < 2; ++tx)
                {
                    channel.Set(sc, rx, tx, {static_cast<double>(value++), 0.0});
                }
            }
        }

        const auto& flat = channel.GetCoefficients();
        NS_TEST_ASSERT_MSG_EQ(flat.size(), 8, "Unexpected flattened CFR size");
        NS_TEST_ASSERT_MSG_EQ(flat.at(0).real(), 1.0, "First coefficient is misplaced");
        NS_TEST_ASSERT_MSG_EQ(flat.at(5).real(), 6.0, "Subcarrier-major layout is wrong");
        NS_TEST_ASSERT_MSG_EQ(channel.Get(1, 1, 1).real(), 8.0, "Tensor lookup is wrong");
    }
};

class MimoPrecodingMatrixContractTest : public TestCase
{
  public:
    MimoPrecodingMatrixContractTest()
        : TestCase("fixed precoding validates subcarrier-TX-stream layout and power")
    {
    }

  private:
    void DoRun() override
    {
        const auto identity = MimoPrecodingMatrix::FrequencyFlat(
            3,
            2,
            2,
            {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}});
        NS_TEST_ASSERT_MSG_EQ(identity.GetNumSubcarriers(),
                              3,
                              "Frequency-flat precoding was not expanded over all tones");
        NS_TEST_ASSERT_MSG_EQ(identity.GetCoefficients().size(),
                              12,
                              "Expanded fixed-precoding coefficient count is wrong");
        NS_TEST_ASSERT_MSG_EQ(identity.Get(2, 0, 0),
                              std::complex<double>(1.0, 0.0),
                              "Subcarrier-TX-stream ordering misplaced W[2](0,0)");
        NS_TEST_ASSERT_MSG_EQ(identity.Get(2, 1, 1),
                              std::complex<double>(1.0, 0.0),
                              "Subcarrier-TX-stream ordering misplaced W[2](1,1)");
        for (uint32_t sc = 0; sc < identity.GetNumSubcarriers(); ++sc)
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(identity.GetPower(sc),
                                      2.0,
                                      1e-12,
                                      "Identity precoding changed the total stream power");
        }

        auto expectRejected = [this](auto makePrecoding, const std::string& description) {
            bool rejected = false;
            try
            {
                makePrecoding();
            }
            catch (const std::invalid_argument& exception)
            {
                rejected =
                    std::string(exception.what()).find("MimoPrecodingMatrix: ") == 0;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, description);
        };

        expectRejected(
            []() {
                static_cast<void>(MimoPrecodingMatrix::FrequencyFlat(
                    0,
                    1,
                    1,
                    {{1.0, 0.0}}));
            },
            "A zero-subcarrier precoding tensor was accepted");
        expectRejected(
            []() {
                static_cast<void>(MimoPrecodingMatrix::FrequencyFlat(
                    1,
                    2,
                    3,
                    std::vector<std::complex<double>>(6, {1.0, 0.0})));
            },
            "More streams than TX antennas were accepted");
        expectRejected(
            []() {
                static_cast<void>(MimoPrecodingMatrix::FrequencyFlat(
                    1,
                    2,
                    1,
                    {{1.0, 0.0}}));
            },
            "A coefficient count inconsistent with N_tx times N_s was accepted");
        expectRejected(
            []() {
                static_cast<void>(MimoPrecodingMatrix::FrequencyFlat(
                    1,
                    2,
                    1,
                    {{1.0, 0.0}, {1.0, 0.0}}));
            },
            "A precoder with excess Frobenius power was accepted");
        expectRejected(
            []() {
                static_cast<void>(MimoPrecodingMatrix::FrequencyFlat(
                    1,
                    1,
                    1,
                    {{std::numeric_limits<double>::quiet_NaN(), 0.0}}));
            },
            "A non-finite complex coefficient was accepted");
        expectRejected(
            [&identity]() {
                MimoChannelMatrix mismatchedChannel(2, 2, 2);
                identity.ValidateForChannel(mismatchedChannel);
            },
            "A precoding/CFR subcarrier mismatch was accepted");

        const auto twoStreams = MimoPrecodingMatrix::FrequencyFlat(
            1,
            2,
            2,
            {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}});
        expectRejected(
            [&twoStreams]() {
                MimoChannelMatrix oneRxChannel(1, 1, 2);
                twoStreams.ValidateForChannel(oneRxChannel);
            },
            "More precoded streams than RX antennas were accepted");
    }
};

class SuMimoFixedPrecodingTest : public TestCase
{
  public:
    SuMimoFixedPrecodingTest()
        : TestCase("fixed precoding applies MMSE to exact complex H times W references")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix sisoChannel(1, 1, 1);
        sisoChannel.Set(0, 0, 0, {2.0, 1.0});
        const auto sisoPrecoding = MimoPrecodingMatrix::FrequencyFlat(
            1,
            1,
            1,
            {{0.0, 1.0}});
        const auto siso = SuMimoLinkModel::Calculate(sisoChannel, sisoPrecoding, 10.0);
        NS_TEST_ASSERT_MSG_EQ_TOL(siso.GetSinrLinear(0, 0),
                                  50.0,
                                  1e-10,
                                  "Unit-power SISO precoding changed scalar SNR");

        MimoChannelMatrix channel(2, 2, 2);
        channel.Set(0, 0, 0, {1.0, 1.0});
        channel.Set(0, 0, 1, {2.0, 0.0});
        channel.Set(0, 1, 0, {0.0, 3.0});
        channel.Set(0, 1, 1, {4.0, -1.0});
        channel.Set(1, 0, 0, {0.5, -0.5});
        channel.Set(1, 0, 1, {-1.0, 2.0});
        channel.Set(1, 1, 0, {3.0, 1.0});
        channel.Set(1, 1, 1, {-2.0, -1.0});

        const auto identity = MimoPrecodingMatrix::FrequencyFlat(
            2,
            2,
            2,
            {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}});
        const auto legacy = SuMimoLinkModel::Calculate(channel, 7.5);
        const auto identityResult = SuMimoLinkModel::Calculate(channel, identity, 7.5);
        for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
        {
            for (uint32_t stream = 0; stream < 2; ++stream)
            {
                NS_TEST_ASSERT_MSG_EQ_TOL(identityResult.GetSinrLinear(sc, stream),
                                          legacy.GetSinrLinear(sc, stream),
                                          1e-10,
                                          "Identity precoding did not reproduce Phase 2");
            }
        }

        const double scale = 1.0 / std::sqrt(2.0);
        const auto complexPrecoding = MimoPrecodingMatrix::FrequencyFlat(
            2,
            2,
            2,
            {{scale, 0.0}, {0.0, scale}, {0.0, scale}, {scale, 0.0}});
        const MimoChannelMatrix effective = complexPrecoding.Apply(channel);
        NS_TEST_ASSERT_MSG_EQ_TOL(effective.Get(0, 0, 0).real(),
                                  scale,
                                  1e-12,
                                  "Complex H[0]W[0] real reference is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(effective.Get(0, 0, 0).imag(),
                                  3.0 * scale,
                                  1e-12,
                                  "Complex H[0]W[0] imaginary reference is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(effective.Get(0, 0, 1).real(),
                                  scale,
                                  1e-12,
                                  "Complex H[0]W[0] second-column real reference is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(effective.Get(0, 0, 1).imag(),
                                  scale,
                                  1e-12,
                                  "Complex H[0]W[0] second-column imaginary reference is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(effective.Get(0, 1, 0).real(),
                                  scale,
                                  1e-12,
                                  "Complex H[0]W[0] second-row real reference is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(effective.Get(0, 1, 0).imag(),
                                  7.0 * scale,
                                  1e-12,
                                  "Complex H[0]W[0] second-row imaginary reference is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(effective.Get(0, 1, 1).real(),
                                  scale,
                                  1e-12,
                                  "Complex H[0]W[0] last-entry real reference is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(effective.Get(0, 1, 1).imag(),
                                  -scale,
                                  1e-12,
                                  "Complex H[0]W[0] last-entry imaginary reference is wrong");

        const auto complexResult =
            SuMimoLinkModel::Calculate(channel, complexPrecoding, 7.5);
        const auto effectiveReference = SuMimoLinkModel::Calculate(effective, 7.5);
        for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
        {
            for (uint32_t stream = 0; stream < 2; ++stream)
            {
                NS_TEST_ASSERT_MSG_EQ_TOL(complexResult.GetSinrLinear(sc, stream),
                                          effectiveReference.GetSinrLinear(sc, stream),
                                          1e-10,
                                          "MMSE did not operate on H[k]W[k]");
            }
        }

        MimoChannelMatrix selectedChannel(1, 2, 2);
        selectedChannel.Set(0, 0, 0, {std::sqrt(2.0), 0.0});
        selectedChannel.Set(0, 1, 1, {std::sqrt(2.0), 0.0});
        const auto selectedPrecoding = MimoPrecodingMatrix::FrequencyFlat(
            1,
            2,
            1,
            {{0.0, 0.0}, {0.0, 1.0}});
        const auto selected =
            SuMimoLinkModel::Calculate(selectedChannel, selectedPrecoding, 10.0);
        NS_TEST_ASSERT_MSG_EQ(selected.GetNumStreams(),
                              1,
                              "2x2 fixed precoding did not select one stream");
        NS_TEST_ASSERT_MSG_EQ_TOL(selected.GetSinrLinear(0, 0),
                                  20.0,
                                  1e-10,
                                  "Selected stream did not receive the full total power");
    }
};

class SuMimoSingleStreamMrtMrcTest : public TestCase
{
  public:
    SuMimoSingleStreamMrtMrcTest()
        : TestCase("single-stream MRT and MRC are normalized and reach the analytical SNR")
    {
    }

  private:
    void DoRun() override
    {
        constexpr double referenceSnr = 3.0;
        MimoChannelMatrix misoChannel(2, 1, 2);
        misoChannel.Set(0, 0, 0, {1.0, 1.0});
        misoChannel.Set(0, 0, 1, {-2.0, 1.0});
        misoChannel.Set(1, 0, 0, {-1.0, 1.0});
        misoChannel.Set(1, 0, 1, {-1.0, -2.0});

        const auto mrt = MimoPrecodingMatrix::MaximumRatioTransmission(misoChannel);
        const auto effective = mrt.Apply(misoChannel);
        const auto mrtResult = SuMimoLinkModel::Calculate(misoChannel, mrt, referenceSnr);
        NS_TEST_ASSERT_MSG_EQ(mrt.GetNumStreams(), 1, "MRT did not create one stream");
        for (uint32_t sc = 0; sc < misoChannel.GetNumSubcarriers(); ++sc)
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(mrt.GetPower(sc),
                                      1.0,
                                      1e-12,
                                      "MRT violated the fixed-precoding power contract");
            NS_TEST_ASSERT_MSG_EQ_TOL(effective.Get(sc, 0, 0).real(),
                                      std::sqrt(7.0),
                                      1e-12,
                                      "MRT did not coherently combine the MISO channel");
            NS_TEST_ASSERT_MSG_EQ_TOL(effective.Get(sc, 0, 0).imag(),
                                      0.0,
                                      1e-12,
                                      "MRT left an unexpected effective-channel phase");
            NS_TEST_ASSERT_MSG_EQ_TOL(mrtResult.GetSinrLinear(sc, 0),
                                      referenceSnr * 7.0,
                                      1e-10,
                                      "MRT did not reach rho times the squared channel norm");
        }

        const std::complex<double> commonRotation{0.0, 1.0};
        MimoChannelMatrix rotatedChannel(1, 1, 2);
        for (uint32_t tx = 0; tx < 2; ++tx)
        {
            rotatedChannel.Set(0, 0, tx, commonRotation * misoChannel.Get(0, 0, tx));
        }
        const auto rotatedMrt =
            MimoPrecodingMatrix::MaximumRatioTransmission(rotatedChannel);
        for (uint32_t tx = 0; tx < 2; ++tx)
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(
                std::abs(rotatedMrt.Get(0, tx, 0) -
                         std::conj(commonRotation) * mrt.Get(0, tx, 0)),
                0.0,
                1e-12,
                "MRT did not compensate a common channel-phase rotation");
        }
        const auto rotatedResult =
            SuMimoLinkModel::Calculate(rotatedChannel, rotatedMrt, referenceSnr);
        NS_TEST_ASSERT_MSG_EQ_TOL(rotatedResult.GetSinrLinear(0, 0),
                                  mrtResult.GetSinrLinear(0, 0),
                                  1e-10,
                                  "A common channel-phase rotation changed MRT SNR");

        MimoChannelMatrix simoChannel(1, 2, 1);
        simoChannel.Set(0, 0, 0, {1.0, 1.0});
        simoChannel.Set(0, 1, 0, {2.0, -1.0});
        const auto mrcResult = SuMimoLinkModel::Calculate(simoChannel, referenceSnr);
        NS_TEST_ASSERT_MSG_EQ_TOL(mrcResult.GetSinrLinear(0, 0),
                                  referenceSnr * 7.0,
                                  1e-10,
                                  "Single-stream MMSE did not reduce to analytical MRC");

        MimoChannelMatrix sisoChannel(1, 1, 1);
        sisoChannel.Set(0, 0, 0, {0.0, 2.0});
        const auto sisoLegacy = SuMimoLinkModel::Calculate(sisoChannel, referenceSnr);
        const auto sisoMrt = MimoPrecodingMatrix::MaximumRatioTransmission(sisoChannel);
        const auto sisoMrtResult =
            SuMimoLinkModel::Calculate(sisoChannel, sisoMrt, referenceSnr);
        NS_TEST_ASSERT_MSG_EQ(sisoMrtResult.GetSinrLinear(0, 0),
                              sisoLegacy.GetSinrLinear(0, 0),
                              "MRT did not exactly preserve the 1x1 result");

        auto expectMrtRejected = [this](const MimoChannelMatrix& channel,
                                        const std::string& description) {
            bool rejected = false;
            try
            {
                static_cast<void>(MimoPrecodingMatrix::MaximumRatioTransmission(channel));
            }
            catch (const std::invalid_argument& exception)
            {
                rejected =
                    std::string(exception.what()).find("MimoPrecodingMatrix: MRT ") == 0;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, description);
        };

        MimoChannelMatrix zeroChannel(1, 1, 2);
        expectMrtRejected(zeroChannel, "A zero MISO channel was accepted for MRT");
        MimoChannelMatrix generalMimoChannel(1, 2, 2);
        generalMimoChannel.Set(0, 0, 0, {1.0, 0.0});
        generalMimoChannel.Set(0, 1, 1, {1.0, 0.0});
        expectMrtRejected(generalMimoChannel,
                          "A general MIMO channel was ambiguously accepted as MRT/MRC");
    }
};

class SuMimoSvdPrecodingAndPowerAllocationTest : public TestCase
{
  public:
    SuMimoSvdPrecodingAndPowerAllocationTest()
        : TestCase("SVD precoding is deterministic and water filling conserves power")
    {
    }

  private:
    void DoRun() override
    {
        constexpr double referenceSnr = 8.0;
        MimoChannelMatrix frequencySelective(2, 2, 2);
        frequencySelective.Set(0, 0, 0, {3.0, 0.0});
        frequencySelective.Set(0, 1, 1, {1.0, 0.0});
        frequencySelective.Set(1, 0, 0, {2.0, 0.0});
        frequencySelective.Set(1, 1, 1, {4.0, 0.0});

        const auto svd =
            MimoPrecodingMatrix::SingularValueDecomposition(frequencySelective, 2);
        const auto& precoding = svd.GetPrecodingMatrix();
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<int>(svd.GetPowerAllocationMode()),
            static_cast<int>(MimoPowerAllocationMode::EQUAL_POWER),
            "Equal power is not the SVD default");
        NS_TEST_ASSERT_MSG_EQ_TOL(svd.GetReferenceSnrLinear(),
                                  1.0,
                                  1e-12,
                                  "Default SVD diagnostic SNR changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(svd.GetNoisePower(),
                                  1.0,
                                  1e-12,
                                  "Default SVD diagnostic noise changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(svd.GetRankTolerance(),
                                  MimoSvdPrecodingOptions::DEFAULT_RANK_TOLERANCE,
                                  0.0,
                                  "Default numerical-rank threshold changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(svd.GetSingularValue(0, 0),
                                  3.0,
                                  1e-12,
                                  "Strongest first-tone singular value is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(svd.GetSingularValue(0, 1),
                                  1.0,
                                  1e-12,
                                  "Second first-tone singular value is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(svd.GetSingularValue(1, 0),
                                  4.0,
                                  1e-12,
                                  "Frequency-selective singular-value sorting is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(svd.GetSingularValue(1, 1),
                                  2.0,
                                  1e-12,
                                  "Frequency-selective second singular value is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(std::abs(precoding.Get(0, 0, 0) -
                                           std::complex<double>(1.0, 0.0)),
                                  0.0,
                                  1e-12,
                                  "First tone did not select the strongest right singular vector");
        NS_TEST_ASSERT_MSG_EQ_TOL(std::abs(precoding.Get(1, 1, 0) -
                                           std::complex<double>(1.0, 0.0)),
                                  0.0,
                                  1e-12,
                                  "Second tone did not reorder the strongest spatial mode");
        for (uint32_t sc = 0; sc < 2; ++sc)
        {
            NS_TEST_ASSERT_MSG_EQ(svd.GetNumericalRank(sc),
                                  2,
                                  "Full-rank diagonal SVD reported the wrong rank");
            NS_TEST_ASSERT_MSG_EQ(svd.GetNumActiveModes(sc),
                                  2,
                                  "Equal power did not keep every selected mode active");
            NS_TEST_ASSERT_MSG_EQ_TOL(svd.GetPowerFraction(sc, 0),
                                      0.5,
                                      1e-12,
                                      "Equal-power first-mode fraction is wrong");
            NS_TEST_ASSERT_MSG_EQ_TOL(svd.GetPowerFraction(sc, 1),
                                      0.5,
                                      1e-12,
                                      "Equal-power second-mode fraction is wrong");
            NS_TEST_ASSERT_MSG_EQ_TOL(precoding.GetPower(sc),
                                      2.0,
                                      1e-12,
                                      "SVD violated the fixed-W[k] power contract");
            NS_TEST_ASSERT_MSG_EQ_TOL(svd.GetTotalAllocatedPower(sc),
                                      svd.GetTotalPower(),
                                      1e-12,
                                      "Equal SVD allocation did not conserve total power");
        }

        const auto eigenmodes =
            SuMimoLinkModel::Calculate(frequencySelective, precoding, referenceSnr);
        const double expectedSinr[2][2] = {{36.0, 4.0}, {64.0, 16.0}};
        for (uint32_t sc = 0; sc < 2; ++sc)
        {
            for (uint32_t stream = 0; stream < 2; ++stream)
            {
                NS_TEST_ASSERT_MSG_EQ_TOL(eigenmodes.GetSinrLinear(sc, stream),
                                          expectedSinr[sc][stream],
                                          1e-10,
                                          "SVD eigenmode SINR differs from rho sigma^2 / N_s");
            }
        }

        const double inverseSqrtTwo = 1.0 / std::sqrt(2.0);
        MimoChannelMatrix rotated(1, 2, 2);
        rotated.Set(0, 0, 0, {3.0 * inverseSqrtTwo, 0.0});
        rotated.Set(0, 0, 1, {3.0 * inverseSqrtTwo, 0.0});
        rotated.Set(0, 1, 0, {inverseSqrtTwo, 0.0});
        rotated.Set(0, 1, 1, {-inverseSqrtTwo, 0.0});
        const auto rotatedSvd =
            MimoPrecodingMatrix::SingularValueDecomposition(rotated, 2);
        const auto rotatedEffective = rotatedSvd.GetPrecodingMatrix().Apply(rotated);
        NS_TEST_ASSERT_MSG_EQ_TOL(std::abs(rotatedEffective.Get(0, 0, 0) -
                                           std::complex<double>(3.0, 0.0)),
                                  0.0,
                                  1e-12,
                                  "Unitary-rotated strongest SVD mode is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(std::abs(rotatedEffective.Get(0, 1, 1) -
                                           std::complex<double>(1.0, 0.0)),
                                  0.0,
                                  1e-12,
                                  "Unitary-rotated second SVD mode is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(std::abs(rotatedEffective.Get(0, 0, 1)),
                                  0.0,
                                  1e-12,
                                  "Unitary rotation left inter-mode coupling");

        MimoChannelMatrix degenerate(1, 2, 2);
        degenerate.Set(0, 0, 1, {0.0, 1.0});
        degenerate.Set(0, 1, 0, {1.0, 0.0});
        const auto degenerateSvd =
            MimoPrecodingMatrix::SingularValueDecomposition(degenerate, 2);
        const auto& deterministic = degenerateSvd.GetPrecodingMatrix();
        for (uint32_t tx = 0; tx < 2; ++tx)
        {
            for (uint32_t stream = 0; stream < 2; ++stream)
            {
                const std::complex<double> expected =
                    (tx == stream) ? std::complex<double>(1.0, 0.0) :
                                     std::complex<double>(0.0, 0.0);
                NS_TEST_ASSERT_MSG_EQ_TOL(std::abs(deterministic.Get(0, tx, stream) -
                                                   expected),
                                          0.0,
                                          1e-12,
                                          "Equal singular values did not use the canonical basis");
            }
        }

        MimoChannelMatrix sisoChannel(1, 1, 1);
        sisoChannel.Set(0, 0, 0, {0.0, 2.0});
        const auto sisoSvd =
            MimoPrecodingMatrix::SingularValueDecomposition(sisoChannel, 1);
        NS_TEST_ASSERT_MSG_EQ(
            SuMimoLinkModel::Calculate(sisoChannel, sisoSvd.GetPrecodingMatrix(), 3.0)
                .GetSinrLinear(0, 0),
            SuMimoLinkModel::Calculate(sisoChannel, 3.0).GetSinrLinear(0, 0),
            "Default SVD precoding did not preserve the 1x1 result");

        auto expectSvdRejected = [this](auto makeSvd, const std::string& description) {
            bool rejected = false;
            try
            {
                makeSvd();
            }
            catch (const std::invalid_argument& exception)
            {
                rejected =
                    std::string(exception.what()).find("MimoPrecodingMatrix: SVD ") == 0;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, description);
        };

        MimoChannelMatrix rankOne(1, 2, 2);
        rankOne.Set(0, 0, 0, {2.0, 0.0});
        const auto rankOneSvd =
            MimoPrecodingMatrix::SingularValueDecomposition(rankOne, 1);
        NS_TEST_ASSERT_MSG_EQ(rankOneSvd.GetNumericalRank(0),
                              1,
                              "Rank-deficient channel did not report rank one");
        expectSvdRejected(
            [&rankOne]() {
                static_cast<void>(
                    MimoPrecodingMatrix::SingularValueDecomposition(rankOne, 2));
            },
            "Rank loss was hidden instead of rejecting two requested streams");

        MimoChannelMatrix thresholdChannel(1, 2, 2);
        thresholdChannel.Set(0, 0, 0, {1.0, 0.0});
        thresholdChannel.Set(0, 1, 1, {5e-13, 0.0});
        expectSvdRejected(
            [&thresholdChannel]() {
                static_cast<void>(
                    MimoPrecodingMatrix::SingularValueDecomposition(thresholdChannel, 2));
            },
            "A singular value below the documented relative threshold was accepted");
        expectSvdRejected(
            [&frequencySelective]() {
                static_cast<void>(MimoPrecodingMatrix::SingularValueDecomposition(
                    frequencySelective, 0));
            },
            "Zero requested SVD streams were accepted");
        expectSvdRejected(
            [&frequencySelective]() {
                static_cast<void>(MimoPrecodingMatrix::SingularValueDecomposition(
                    frequencySelective, 3));
            },
            "More SVD streams than antenna dimensions were accepted");

        MimoSvdPrecodingOptions invalidOptions;
        invalidOptions.rankTolerance = 0.0;
        expectSvdRejected(
            [&frequencySelective, &invalidOptions]() {
                static_cast<void>(MimoPrecodingMatrix::SingularValueDecomposition(
                    frequencySelective, 2, invalidOptions));
            },
            "A zero numerical-rank threshold was accepted");
        invalidOptions = {};
        invalidOptions.referenceSnrLinear = 0.0;
        expectSvdRejected(
            [&frequencySelective, &invalidOptions]() {
                static_cast<void>(MimoPrecodingMatrix::SingularValueDecomposition(
                    frequencySelective, 2, invalidOptions));
            },
            "A zero SVD reference SNR was accepted");
        invalidOptions = {};
        invalidOptions.noisePower = std::numeric_limits<double>::quiet_NaN();
        expectSvdRejected(
            [&frequencySelective, &invalidOptions]() {
                static_cast<void>(MimoPrecodingMatrix::SingularValueDecomposition(
                    frequencySelective, 2, invalidOptions));
            },
            "A non-finite SVD noise power was accepted");
        invalidOptions = {};
        invalidOptions.powerAllocation = static_cast<MimoPowerAllocationMode>(99);
        expectSvdRejected(
            [&frequencySelective, &invalidOptions]() {
                static_cast<void>(MimoPrecodingMatrix::SingularValueDecomposition(
                    frequencySelective, 2, invalidOptions));
            },
            "An unknown SVD power-allocation mode was accepted");
        MimoChannelMatrix nonFiniteChannel(1, 1, 1);
        nonFiniteChannel.Set(
            0, 0, 0, {std::numeric_limits<double>::infinity(), 0.0});
        expectSvdRejected(
            [&nonFiniteChannel]() {
                static_cast<void>(
                    MimoPrecodingMatrix::SingularValueDecomposition(nonFiniteChannel, 1));
            },
            "A non-finite CFR was accepted for SVD");

        MimoChannelMatrix twoModeChannel(1, 2, 2);
        twoModeChannel.Set(0, 0, 0, {2.0, 0.0});
        twoModeChannel.Set(0, 1, 1, {1.0, 0.0});

        MimoSvdPrecodingOptions lowSnrOptions;
        lowSnrOptions.powerAllocation = MimoPowerAllocationMode::WATER_FILLING;
        lowSnrOptions.referenceSnrLinear = 0.5;
        lowSnrOptions.noisePower = 2.0;
        const auto lowWater = MimoPrecodingMatrix::SingularValueDecomposition(
            twoModeChannel, 2, lowSnrOptions);
        NS_TEST_ASSERT_MSG_EQ(lowWater.GetNumActiveModes(0),
                              1,
                              "Weak mode was active below its water-filling threshold");
        NS_TEST_ASSERT_MSG_EQ_TOL(lowWater.GetPowerFraction(0, 0),
                                  1.0,
                                  1e-12,
                                  "Below-threshold power was not assigned to the strong mode");
        NS_TEST_ASSERT_MSG_EQ_TOL(lowWater.GetPowerFraction(0, 1),
                                  0.0,
                                  1e-12,
                                  "Below-threshold weak mode received power");
        NS_TEST_ASSERT_MSG_EQ_TOL(lowWater.GetAllocatedPower(0, 0),
                                  1.0,
                                  1e-12,
                                  "Low-SNR allocated power is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(lowWater.GetTotalAllocatedPower(0),
                                  lowWater.GetTotalPower(),
                                  1e-12,
                                  "Low-SNR water filling did not conserve power");
        NS_TEST_ASSERT_MSG_EQ_TOL(lowWater.GetWaterLevel(0),
                                  1.5,
                                  1e-12,
                                  "Low-SNR water level is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(lowWater.GetPrecodingMatrix().GetPower(0),
                                  2.0,
                                  1e-12,
                                  "Low-SNR water filling violated fixed-W[k] normalization");

        MimoSvdPrecodingOptions lowEqualOptions = lowSnrOptions;
        lowEqualOptions.powerAllocation = MimoPowerAllocationMode::EQUAL_POWER;
        const auto lowEqual = MimoPrecodingMatrix::SingularValueDecomposition(
            twoModeChannel, 2, lowEqualOptions);
        const double lowWaterRate =
            SuMimoLinkModel::Calculate(twoModeChannel,
                                       lowWater.GetPrecodingMatrix(),
                                       lowSnrOptions.referenceSnrLinear)
                .GetSpectralEfficiency();
        const double lowEqualRate =
            SuMimoLinkModel::Calculate(twoModeChannel,
                                       lowEqual.GetPrecodingMatrix(),
                                       lowEqualOptions.referenceSnrLinear)
                .GetSpectralEfficiency();
        NS_TEST_ASSERT_MSG_EQ((lowWaterRate >= lowEqualRate),
                              true,
                              "Low-SNR water filling reduced ideal rate");

        MimoSvdPrecodingOptions highSnrOptions;
        highSnrOptions.powerAllocation = MimoPowerAllocationMode::WATER_FILLING;
        highSnrOptions.referenceSnrLinear = 4.0;
        highSnrOptions.noisePower = 2.0;
        const auto highWater = MimoPrecodingMatrix::SingularValueDecomposition(
            twoModeChannel, 2, highSnrOptions);
        NS_TEST_ASSERT_MSG_EQ(highWater.GetNumActiveModes(0),
                              2,
                              "Both modes were not active above the threshold");
        NS_TEST_ASSERT_MSG_EQ_TOL(highWater.GetPowerFraction(0, 0),
                                  0.59375,
                                  1e-12,
                                  "High-SNR strong-mode water fraction is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(highWater.GetPowerFraction(0, 1),
                                  0.40625,
                                  1e-12,
                                  "High-SNR weak-mode water fraction is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(highWater.GetTotalAllocatedPower(0),
                                  8.0,
                                  1e-12,
                                  "High-SNR water filling did not conserve total power");
        NS_TEST_ASSERT_MSG_EQ_TOL(highWater.GetWaterLevel(0),
                                  5.25,
                                  1e-12,
                                  "High-SNR water level is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(highWater.GetAllocatedPower(0, 0) + 2.0 / 4.0,
                                  highWater.GetWaterLevel(0),
                                  1e-12,
                                  "Strong active mode violates the KKT water level");
        NS_TEST_ASSERT_MSG_EQ_TOL(highWater.GetAllocatedPower(0, 1) + 2.0 / 1.0,
                                  highWater.GetWaterLevel(0),
                                  1e-12,
                                  "Weak active mode violates the KKT water level");
        NS_TEST_ASSERT_MSG_EQ_TOL(highWater.GetPrecodingMatrix().GetPower(0),
                                  2.0,
                                  1e-12,
                                  "High-SNR water filling violated fixed-W[k] normalization");

        MimoSvdPrecodingOptions highEqualOptions = highSnrOptions;
        highEqualOptions.powerAllocation = MimoPowerAllocationMode::EQUAL_POWER;
        const auto highEqual = MimoPrecodingMatrix::SingularValueDecomposition(
            twoModeChannel, 2, highEqualOptions);
        const double highWaterRate =
            SuMimoLinkModel::Calculate(twoModeChannel,
                                       highWater.GetPrecodingMatrix(),
                                       highSnrOptions.referenceSnrLinear)
                .GetSpectralEfficiency();
        const double highEqualRate =
            SuMimoLinkModel::Calculate(twoModeChannel,
                                       highEqual.GetPrecodingMatrix(),
                                       highEqualOptions.referenceSnrLinear)
                .GetSpectralEfficiency();
        NS_TEST_ASSERT_MSG_EQ((highWaterRate >= highEqualRate),
                              true,
                              "High-SNR water filling reduced ideal rate");
    }
};

class WifiLinkBudgetConversionTest : public TestCase
{
  public:
    WifiLinkBudgetConversionTest()
        : TestCase("Wi-Fi link-budget dB and linear conversions are reversible")
    {
    }

  private:
    void DoRun() override
    {
        const double valuesDb[] = {-100.0, -3.0, 0.0, 10.0, 37.0};
        for (const double valueDb : valuesDb)
        {
            const double valueLinear = WifiLinkBudget::DbToLinear(valueDb);
            NS_TEST_ASSERT_MSG_EQ_TOL(WifiLinkBudget::LinearToDb(valueLinear),
                                      valueDb,
                                      1e-12,
                                      "dB/linear round trip changed");
        }
        NS_TEST_ASSERT_MSG_EQ_TOL(WifiLinkBudget::DbToLinear(10.0),
                                  10.0,
                                  1e-12,
                                  "10 dB must be a power ratio of ten");
    }
};

class WifiLinkBudgetCalculationTest : public TestCase
{
  public:
    WifiLinkBudgetCalculationTest()
        : TestCase("reference SNR includes power, path loss, bandwidth, temperature and NF")
    {
    }

  private:
    void DoRun() override
    {
        constexpr double boltzmann = 1.3803e-23;
        constexpr double bandwidthHz = 20e6;
        constexpr double temperatureK = 290.0;
        const auto result =
            WifiLinkBudget::Calculate(20.0, 100.0, bandwidthHz, temperatureK, 7.0);

        const double expectedReceivedPowerW = 1e-11; // -80 dBm
        const double expectedThermalNoiseW = boltzmann * temperatureK * bandwidthHz;
        const double expectedReceiverNoiseW =
            expectedThermalNoiseW * std::pow(10.0, 7.0 / 10.0);
        const double expectedSnr = expectedReceivedPowerW / expectedReceiverNoiseW;

        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetReceivedPowerW(),
                                  expectedReceivedPowerW,
                                  expectedReceivedPowerW * 1e-12,
                                  "Received power does not include path loss correctly");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetThermalNoisePowerW(),
                                  expectedThermalNoiseW,
                                  expectedThermalNoiseW * 1e-12,
                                  "Thermal noise is not k*T*B");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetReceiverNoisePowerW(),
                                  expectedReceiverNoiseW,
                                  expectedReceiverNoiseW * 1e-12,
                                  "Receiver noise does not include noise figure");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetReferenceSnrLinear(),
                                  expectedSnr,
                                  expectedSnr * 1e-12,
                                  "Reference SNR is incorrect");

        const auto doubledBandwidth =
            WifiLinkBudget::Calculate(20.0, 100.0, 2.0 * bandwidthHz, temperatureK, 7.0);
        const auto doubledTemperature =
            WifiLinkBudget::Calculate(20.0, 100.0, bandwidthHz, 2.0 * temperatureK, 7.0);
        NS_TEST_ASSERT_MSG_EQ_TOL(doubledBandwidth.GetReferenceSnrLinear(),
                                  expectedSnr / 2.0,
                                  expectedSnr * 1e-12,
                                  "Doubling bandwidth must halve SNR");
        NS_TEST_ASSERT_MSG_EQ_TOL(doubledTemperature.GetReferenceSnrLinear(),
                                  expectedSnr / 2.0,
                                  expectedSnr * 1e-12,
                                  "Doubling temperature must halve SNR");
    }
};

class HeEesmFlatChannelTest : public TestCase
{
  public:
    HeEesmFlatChannelTest()
        : TestCase("HE EESM preserves a flat post-MMSE SINR across tones and streams")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix channel(4, 2, 2);
        for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
        {
            channel.Set(sc, 0, 0, {std::sqrt(2.0), 0.0});
            channel.Set(sc, 1, 1, {std::sqrt(2.0), 0.0});
        }

        const auto linkBudget = MakeReferenceSnrBudget(1.0);
        const auto txVector = MakeHeTxVector(HePhy::GetHeMcs0(), 2);
        auto abstraction = CreateObject<HeSuMimoLinkAbstraction>();
        // Any positive beta gives the same answer for a flat channel.
        abstraction->SetEesmBeta(0, 3.0);

        const auto result = abstraction->Calculate(channel, linkBudget, txVector, 1000);
        for (uint32_t sc = 0; sc < result.GetMimoResult().GetNumSubcarriers(); ++sc)
        {
            for (uint32_t stream = 0; stream < result.GetMimoResult().GetNumStreams(); ++stream)
            {
                NS_TEST_ASSERT_MSG_EQ_TOL(result.GetMimoResult().GetSinrLinear(sc, stream),
                                          linkBudget.GetReferenceSnrLinear(),
                                          1e-12,
                                          "Flat-channel post-MMSE SINR changed");
            }
        }
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetEffectiveSinrLinear(),
                                  linkBudget.GetReferenceSnrLinear(),
                                  1e-12,
                                  "EESM must preserve equal SINR samples");

        const double directSuccess =
            abstraction->GetErrorRateModel()->GetChunkSuccessRate(HePhy::GetHeMcs0(),
                                                                  txVector,
                                                                  result.GetEffectiveSinrLinear(),
                                                                  8000,
                                                                  2);
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetPacketSuccessRate(),
                                  directSuccess,
                                  1e-12,
                                  "Packet success did not use the configured ErrorRateModel");
    }
};

class HeAwgnCalibrationContextTest : public TestCase
{
  public:
    HeAwgnCalibrationContextTest()
        : TestCase("HE AWGN calibration applies only to its measured PHY context")
    {
    }

  private:
    void DoRun() override
    {
        const HeAwgnCalibrationKey key{0, false, 32, 1, 1, 20, 800};
        auto abstraction = CreateObject<HeSuMimoLinkAbstraction>();
        abstraction->SetEesmBeta(0, 1.0);
        abstraction->SetAwgnSnrOffsetDb(key, -0.5);
        const auto txVector = MakeHeTxVector(HePhy::GetHeMcs0(), 1);
        const double snr = WifiLinkBudget::DbToLinear(-1.0);
        const double adjusted = snr * WifiLinkBudget::DbToLinear(-0.5);
        NS_TEST_ASSERT_MSG_EQ_TOL(
            abstraction->GetErrorModelSinrLinear(0, txVector, 32, 1, SU_STA_ID, snr),
            adjusted,
            1e-12,
            "Matching HE PHY context did not apply its AWGN correction");
        NS_TEST_ASSERT_MSG_EQ_TOL(
            abstraction->GetErrorModelSinrLinear(0, txVector, 33, 1, SU_STA_ID, snr),
            snr,
            1e-12,
            "A different packet length inherited the AWGN correction");
        NS_TEST_ASSERT_MSG_EQ_TOL(
            abstraction->GetErrorModelSinrLinear(0, txVector, 32, 2, SU_STA_ID, snr),
            snr,
            1e-12,
            "A different receiver dimension inherited the AWGN correction");

        MimoChannelMatrix channel(1, 1, 1);
        channel.Set(0, 0, 0, {1.0, 0.0});
        const auto linkBudget = MakeReferenceSnrBudget(snr);
        const auto result = abstraction->Calculate(channel, linkBudget, txVector, 32);
        const double directSuccess =
            abstraction->GetErrorRateModel()->GetChunkSuccessRate(HePhy::GetHeMcs0(),
                                                                  txVector,
                                                                  adjusted,
                                                                  32 * 8,
                                                                  1);
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetPacketSuccessRate(),
                                  directSuccess,
                                  1e-12,
                                  "SU PER did not use the calibrated SNR");

        SionnaMimoHelper helper;
        helper.SetAwgnSnrOffsetDb(key, -0.5);
        NS_TEST_ASSERT_MSG_EQ_TOL(helper.GetConfiguration().awgnSnrOffsetsDb.at(key),
                                  -0.5,
                                  1e-12,
                                  "High-level API lost the exact calibration context");
    }
};

class HeCalibrationProfileTest : public TestCase
{
  public:
    HeCalibrationProfileTest() : TestCase("HE pilot loading is atomic and context and model matching are exact") {}

  private:
    void DoRun() override
    {
        const auto path = std::filesystem::path(NS_TEST_SOURCEDIR).parent_path() /
                          "data/he_mcs0_bcc_32_calibration.json";
        SionnaMimoHelper helper;
        helper.LoadHeCalibrationProfile(path.string());
        const auto profile = *helper.GetConfiguration().heCalibrationProfile;
        NS_TEST_ASSERT_MSG_EQ_TOL(helper.GetEesmBeta(0), profile.betaLinear, 1e-12,
                                  "Profile beta was not installed");
        auto rejects = [this](const auto& action) {
            bool rejected = false;
            try { action(); } catch (const std::invalid_argument&) { rejected = true; }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, "Invalid calibration was accepted");
        };
        rejects([&] { helper.LoadHeCalibrationProfile(path.string() + ".missing"); });
        rejects([&] { helper.SetEesmBeta(0, 2.0); });
        rejects([&] { helper.SetAwgnSnrOffsetDb(profile.key, 0.0); });

        std::ifstream input(path);
        const std::string original((std::istreambuf_iterator<char>(input)), {});
        const auto badPath = CreateTempDirFilename("bad-calibration.json");
        const std::vector<std::pair<std::string, std::string>> changes{
            {"\"validated\"", "\"rejected\""},
            {"\"link_level\"", "\"synthetic\""},
            {"\"BCC\"", "\"invalid\""},
            {"\"mcs\": 0", "\"mcs\": 0.5"},
            {"\"nss\": 1", "\"nss\": 257"},
            {"\"packet_size_bytes\": 32", "\"packet_size_bytes\": 33"},
            {"\"beta_linear\": 1.0383687667246768", "\"beta_linear\": -1"},
            {"\"awgn_snr_offset_db\": -0.49589212058827303", "\"awgn_snr_offset_db\": 31"},
            {profile.awgnModelSha256, std::string(64, '0')},
            {profile.referenceSha256, "not-a-hash"},
            {"\"source_version\": \"2026a\"", "\"source_version\": \"\""},
            {"\"reasons\": []", "\"reasons\": [\"failed\"]"},
            {"\"beta_linear\"", "\"missing_beta\""}};
        for (const auto& [before, after] : changes)
        {
            std::string bad = original;
            const auto pos = bad.find(before);
            NS_TEST_ASSERT_MSG_NE(pos, std::string::npos, "Invalid-profile fixture did not match");
            bad.replace(pos, before.size(), after);
            { std::ofstream output(badPath); output << bad; }
            rejects([&] { helper.LoadHeCalibrationProfile(badPath); });
        }
        { std::ofstream output(badPath); output << "{"; }
        rejects([&] { helper.LoadHeCalibrationProfile(badPath); });
        NS_TEST_ASSERT_MSG_EQ_TOL(helper.GetEesmBeta(0), profile.betaLinear, 1e-12,
                                  "Rejected load changed the helper state");
        NS_TEST_ASSERT_MSG_EQ(helper.GetConfiguration().heCalibrationProfile->referenceSha256,
                              profile.referenceSha256, "Provenance changed after rejection");

        auto base = CreateObject<TableBasedErrorRateModel>();
        auto model = CreateObject<HeCalibratedErrorRateModel>();
        model->Configure(profile, base);
        WifiMacHeader header;
        header.SetType(WIFI_MAC_DATA);
        auto psdu = Create<WifiPsdu>(Create<Packet>(4), header);
        auto tx = MakeHeTxVector(HePhy::GetHeMcs0(), 1);
        NS_TEST_ASSERT_MSG_EQ(psdu->GetSize(), 32, "PSDU fixture has wrong MAC/FCS length");
        NS_TEST_ASSERT_MSG_EQ(model->Matches(tx, *psdu, 1), true, "Pilot did not match");
        for (int mismatch = 0; mismatch < 6; ++mismatch)
        {
            auto other = tx;
            if (mismatch == 0) { other.SetMode(HePhy::GetHeMcs1()); }
            if (mismatch == 1) { other.SetLdpc(true); }
            if (mismatch == 2) { other.SetNss(2); }
            if (mismatch == 3) { other.SetChannelWidth(40); }
            if (mismatch == 4) { other.SetGuardInterval(1600); }
            if (mismatch == 5) { other.SetPreambleType(WIFI_PREAMBLE_HE_ER_SU); }
            NS_TEST_ASSERT_MSG_EQ(model->Matches(other, *psdu, 1), false,
                                  "Incompatible TXVECTOR inherited calibration");
        }
        NS_TEST_ASSERT_MSG_EQ(model->Matches(tx, *psdu, 2), false, "RX mismatch accepted");
        auto longer = Create<WifiPsdu>(Create<Packet>(5), header);
        NS_TEST_ASSERT_MSG_EQ(model->Matches(tx, *longer, 1), false, "Length mismatch accepted");
        auto single = Create<WifiPsdu>(Create<WifiMpdu>(Create<Packet>(0), header), true);
        NS_TEST_ASSERT_MSG_EQ(single->GetSize(), 32, "Aggregate length control differs");
        NS_TEST_ASSERT_MSG_EQ(model->Matches(tx, *single, 1), false, "S-MPDU inherited calibration");
        WifiPhyOperatingChannel operating;
        operating.Set(36, 0, 20, WIFI_STANDARD_80211ax, WIFI_PHY_BAND_5GHZ);
        auto ppdu = Create<HePpdu>(psdu, tx, operating, MicroSeconds(100), 999);
        const double raw = base->GetChunkSuccessRate(tx.GetMode(), tx, 0.8, 117, 1);
        NS_TEST_ASSERT_MSG_EQ_TOL(model->GetPayloadChunkSuccessRate(tx.GetMode(), tx, 0.8,
                                    117, 1, SU_STA_ID, ppdu, 0.5), raw, 1e-12,
                                  "Missing reception context must delegate unchanged");
        model->SetReception(999, std::vector<double>(234, 0.8), 0.8, MicroSeconds(100));
        const double expected = base->GetChunkSuccessRate(tx.GetMode(), tx,
            0.8 * WifiLinkBudget::DbToLinear(profile.offsetDb), 256, 1);
        const double half = model->GetPayloadChunkSuccessRate(tx.GetMode(), tx, 0.8,
            117, 1, SU_STA_ID, ppdu, 0.5);
        NS_TEST_ASSERT_MSG_EQ_TOL(half * half, expected, 1e-12,
                                  "Chunk splitting changed whole-PSDU calibrated probability");
        const double interfered = model->GetPayloadChunkSuccessRate(tx.GetMode(), tx, 0.4,
            117, 1, SU_STA_ID, ppdu, 0.5);
        NS_TEST_ASSERT_MSG_EQ_TOL(interfered * interfered,
            base->GetChunkSuccessRate(tx.GetMode(), tx,
                0.4 * WifiLinkBudget::DbToLinear(profile.offsetDb), 256, 1), 1e-12,
                                  "Chunk interference did not scale data-tone SINRs");
        NS_TEST_ASSERT_MSG_EQ_TOL(model->GetChunkSuccessRate(HePhy::GetOfdmRate6Mbps(), tx,
            0.8, 48, 1, WIFI_PPDU_FIELD_NON_HT_HEADER),
            base->GetChunkSuccessRate(HePhy::GetOfdmRate6Mbps(), tx,
            0.8, 48, 1, WIFI_PPDU_FIELD_NON_HT_HEADER), 1e-12,
                                  "Header inherited payload calibration");
        rejects([&] { model->Configure(profile, CreateObject<YansErrorRateModel>()); });
        base->SetAttribute("SizeThreshold", UintegerValue(1));
        rejects([&] { model->Configure(profile, base); });
        rejects([&] { model->GetPayloadChunkSuccessRate(tx.GetMode(), tx, 0.8,
                        117, 1, SU_STA_ID, ppdu, 0.5); });
        Simulator::Stop(MicroSeconds(101));
        Simulator::Run();
        base->SetAttribute("SizeThreshold", UintegerValue(400));
        NS_TEST_ASSERT_MSG_EQ_TOL(model->GetPayloadChunkSuccessRate(tx.GetMode(), tx, 0.8,
                                    117, 1, SU_STA_ID, ppdu, 0.5), raw, 1e-12,
                                  "Expired reception context survived PPDU end");
        Simulator::Destroy();
    }
};

class HeCalibrationSpectrumReceptionTest : public TestCase
{
  public:
    HeCalibrationSpectrumReceptionTest()
        : TestCase("HE pilot calibrates real scalar SpectrumWifiPhy payloads with exact PSDU lengths") {}

  private:
    void Payload(WifiTxVector, Time) { ++m_payloads; }
    void RxOk(Ptr<const WifiPsdu>, RxSignalInfo, WifiTxVector, std::vector<bool>) {}
    void RxError(Ptr<const WifiPsdu>) {}
    void Applied(uint64_t, double effective, double corrected, double success, double fraction)
    {
        ++m_applied;
        NS_TEST_ASSERT_MSG_EQ_TOL(effective, m_expectedEffective, 1e-10, "Wrong data-tone EESM");
        NS_TEST_ASSERT_MSG_EQ_TOL(corrected, effective * WifiLinkBudget::DbToLinear(m_profile.offsetDb),
                                  1e-12, "Offset did not reach actual payload error model");
        auto base = CreateObject<TableBasedErrorRateModel>();
        const auto tx = MakeHeTxVector(HePhy::GetHeMcs0(), 1);
        const double expected = base->GetChunkSuccessRate(tx.GetMode(), tx, corrected, 256, 1);
        NS_TEST_ASSERT_MSG_EQ_TOL(success, expected, 1e-12, "Symbol padding replaced PSDU bytes");
        NS_TEST_ASSERT_MSG_EQ_TOL(fraction, 1.0, 1e-12, "Full MPDU window was not normalized");
        if (m_applied == 2)
        {
            const double uncorrected = base->GetChunkSuccessRate(tx.GetMode(), tx, effective, 256, 1);
            NS_TEST_ASSERT_MSG_GT(std::abs(success - uncorrected), 0.01,
                                  "Selective control cannot distinguish calibrated PER");
        }
    }
    void DoRun() override
    {
        SionnaMimoHelper helper;
        helper.LoadHeCalibrationProfile((std::filesystem::path(NS_TEST_SOURCEDIR).parent_path() /
            "data/he_mcs0_bcc_32_calibration.json").string());
        m_profile = *helper.GetConfiguration().heCalibrationProfile;
        auto channel = CreateObject<MultiModelSpectrumChannel>();
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        channel->AddSpectrumPropagationLossModel(adapter);
        SpectrumWifiPhyHelper txHelper, rxHelper;
        txHelper.SetChannel(channel);
        rxHelper.SetChannel(channel);
        SionnaHelper ipc("simple_room", "tcp://127.0.0.1:5555");
        helper.Apply(ipc, txHelper, rxHelper, adapter);
        constexpr double powerDbm = -80;
        txHelper.Set("TxPowerStart", DoubleValue(powerDbm));
        txHelper.Set("TxPowerEnd", DoubleValue(powerDbm));
        NodeContainer nodes;
        nodes.Create(2);
        for (uint32_t i = 0; i < 2; ++i)
        {
            auto mobility = CreateObject<ConstantPositionMobilityModel>();
            mobility->SetPosition({double(i), 0, 0});
            nodes.Get(i)->AggregateObject(mobility);
        }
        WifiHelper wifi;
        wifi.SetStandard(WIFI_STANDARD_80211ax);
        WifiMacHelper mac;
        mac.SetType("ns3::AdhocWifiMac");
        auto txDevice = DynamicCast<WifiNetDevice>(wifi.Install(txHelper, mac, nodes.Get(0)).Get(0));
        auto rxDevice = DynamicCast<WifiNetDevice>(wifi.Install(rxHelper, mac, nodes.Get(1)).Get(0));
        auto txPhy = DynamicCast<SpectrumWifiPhy>(txDevice->GetPhy());
        auto rxPhy = DynamicCast<SpectrumWifiPhy>(rxDevice->GetPhy());
        txPhy->SetOperatingChannel({36, 20, WIFI_PHY_BAND_5GHZ, 0});
        rxPhy->SetOperatingChannel({36, 20, WIFI_PHY_BAND_5GHZ, 0});
        auto model = helper.InstallHeCalibration(rxDevice);
        rxPhy->SetReceiveOkCallback(MakeCallback(&HeCalibrationSpectrumReceptionTest::RxOk, this));
        rxPhy->SetReceiveErrorCallback(MakeCallback(&HeCalibrationSpectrumReceptionTest::RxError, this));
        bool duplicateRejected = false;
        try { helper.InstallHeCalibration(rxDevice); }
        catch (const std::invalid_argument&) { duplicateRejected = true; }
        NS_TEST_ASSERT_MSG_EQ(duplicateRejected, true, "Duplicate wrapper installation was accepted");
        model->TraceConnectWithoutContext("CalibrationApplied",
            MakeCallback(&HeCalibrationSpectrumReceptionTest::Applied, this));
        rxPhy->TraceConnectWithoutContext("PhyRxPayloadBegin",
            MakeCallback(&HeCalibrationSpectrumReceptionTest::Payload, this));
        txPhy->AssignStreams(1);
        rxPhy->AssignStreams(1);
        auto txPsd = WifiSpectrumValueHelper::CreateHeOfdmTxPowerSpectralDensity(
            5180, 20, std::pow(10.0, (powerDbm - 30) / 10.0), txPhy->GetGuardBandwidth(20));
        const uint32_t nsc = txPsd->GetValuesN() - 1;
        MimoChannelMatrix flat(nsc, 1, 1), selective(nsc, 1, 1);
        for (uint32_t sc = 0; sc < nsc; ++sc)
        {
            flat.Set(sc, 0, 0, {1, 0});
            selective.Set(sc, 0, 0, sc < nsc / 2 ? std::complex<double>{0, 0}
                                                : std::complex<double>{std::sqrt(2.0), 0});
        }
        const double noise = WifiLinkBudget::Calculate(0, 0, 20e6, 290, 7).GetReceiverNoisePowerW();
        const double rho = (*txPsd)[nsc / 2 + 2] / (noise / 20e6);
        adapter->SetFixedChannelMatrix(flat, 0);
        WifiMacHeader header;
        header.SetType(WIFI_MAC_DATA);
        header.SetAddr1(rxDevice->GetMac()->GetAddress());
        header.SetAddr2(txDevice->GetMac()->GetAddress());
        auto psdu = Create<WifiPsdu>(Create<Packet>(4), header);
        NS_TEST_ASSERT_MSG_EQ(psdu->GetSize(), 32, "Actual PSDU is not the calibration length");
        const auto tx = MakeHeTxVector(HePhy::GetHeMcs0(), 1);
        Simulator::Schedule(MilliSeconds(1), [this, rho, txPhy, psdu, tx]() {
            m_expectedEffective = rho;
            txPhy->Send({{SU_STA_ID, psdu}}, tx);
        });
        Simulator::Schedule(MilliSeconds(2), [this, rho, adapter, selective, txPhy, psdu, tx]() {
            adapter->SetFixedChannelMatrix(selective, 0);
            m_expectedEffective = -m_profile.betaLinear *
                std::log((1 + std::exp(-2 * rho / m_profile.betaLinear)) / 2);
            txPhy->Send({{SU_STA_ID, psdu}}, tx);
        });
        Simulator::Schedule(MilliSeconds(3), [adapter, flat, txPhy, header, tx]() {
            adapter->SetFixedChannelMatrix(flat, 0);
            txPhy->Send({{SU_STA_ID, Create<WifiPsdu>(Create<Packet>(5), header)}}, tx);
        });
        Simulator::Schedule(MilliSeconds(4), [txPhy, psdu, tx]() {
            auto other = tx;
            other.SetGuardInterval(1600);
            txPhy->Send({{SU_STA_ID, psdu}}, other);
        });
        Simulator::Stop(MilliSeconds(5));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(m_payloads, 4, "Real PPDUs did not reach payload decoding");
        NS_TEST_ASSERT_MSG_EQ(m_applied, 2, "Unmatched reception silently used the pilot");
        Simulator::Destroy();
    }
    HeCalibrationProfile m_profile;
    double m_expectedEffective{0};
    uint32_t m_payloads{0}, m_applied{0};
};

class HeEesmFrequencySelectiveTest : public TestCase
{
  public:
    HeEesmFrequencySelectiveTest()
        : TestCase("HE EESM follows the configured beta on a frequency-selective channel")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix channel(2, 1, 1);
        channel.Set(0, 0, 0, {1.0, 0.0});
        channel.Set(1, 0, 0, {3.0, 0.0});

        const auto linkBudget = MakeReferenceSnrBudget(1.0);
        const auto txVector = MakeHeTxVector(HePhy::GetHeMcs3(), 1);
        auto abstraction = CreateObject<HeSuMimoLinkAbstraction>();
        // Formula-only test value; it is not an HE MCS 3 calibration.
        constexpr double beta = 2.0;
        abstraction->SetEesmBeta(3, beta);

        const auto result = abstraction->Calculate(channel, linkBudget, txVector, 1000);
        const double firstSinr = linkBudget.GetReferenceSnrLinear();
        const double secondSinr = 9.0 * linkBudget.GetReferenceSnrLinear();
        const double expected =
            -beta * std::log((std::exp(-firstSinr / beta) +
                              std::exp(-secondSinr / beta)) /
                             2.0);

        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetMimoResult().GetSinrLinear(0, 0),
                                  firstSinr,
                                  1e-12,
                                  "First tone SINR is incorrect");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetMimoResult().GetSinrLinear(1, 0),
                                  secondSinr,
                                  1e-12,
                                  "Second tone SINR is incorrect");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetEffectiveSinrLinear(),
                                  expected,
                                  1e-12,
                                  "Frequency-selective EESM result is incorrect");
    }
};

class HeSisoErrorModelParityTest : public TestCase
{
  public:
    HeSisoErrorModelParityTest()
        : TestCase("flat SISO HE PER is identical to ns-3 across MCS, SNR and packet size")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix channel(8, 1, 1);
        for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
        {
            channel.Set(sc, 0, 0, {1.0, 0.0});
        }

        const WifiMode modes[] = {HePhy::GetHeMcs0(),
                                  HePhy::GetHeMcs3(),
                                  HePhy::GetHeMcs7(),
                                  HePhy::GetHeMcs11()};
        const double snrPointsDb[] = {-5.0, 0.0, 5.0, 10.0, 15.0, 20.0, 25.0, 30.0, 35.0};
        const uint32_t packetSizes[] = {32, 400, 1458};

        auto referenceErrorModel = CreateObject<TableBasedErrorRateModel>();
        auto abstraction = CreateObject<HeSuMimoLinkAbstraction>();
        abstraction->SetErrorRateModel(referenceErrorModel);
        for (const auto mode : modes)
        {
            // Beta cancels for flat SISO; 1.0 is a sentinel, not a calibration.
            abstraction->SetEesmBeta(mode.GetMcsValue(), 1.0);
        }

        for (const auto mode : modes)
        {
            const auto txVector = MakeHeTxVector(mode, 1);
            for (const double snrDb : snrPointsDb)
            {
                const auto linkBudget =
                    MakeReferenceSnrBudget(WifiLinkBudget::DbToLinear(snrDb));
                NS_TEST_ASSERT_MSG_EQ_TOL(
                    WifiLinkBudget::LinearToDb(linkBudget.GetReferenceSnrLinear()),
                    snrDb,
                    1e-12,
                    "Link budget did not reproduce the requested SNR point");

                for (const uint32_t packetSize : packetSizes)
                {
                    const auto result =
                        abstraction->Calculate(channel, linkBudget, txVector, packetSize);
                    const double directSuccess =
                        referenceErrorModel->GetChunkSuccessRate(
                            mode,
                            txVector,
                            linkBudget.GetReferenceSnrLinear(),
                            static_cast<uint64_t>(packetSize) * 8,
                            1);

                    NS_TEST_ASSERT_MSG_EQ_TOL(
                        result.GetEffectiveSinrLinear(),
                        linkBudget.GetReferenceSnrLinear(),
                        1e-12,
                        "Flat SISO EESM changed the reference SNR for MCS "
                            << +mode.GetMcsValue() << " at " << snrDb << " dB");
                    NS_TEST_ASSERT_MSG_EQ_TOL(
                        result.GetPacketSuccessRate(),
                        directSuccess,
                        1e-12,
                        "SISO parity failed for MCS " << +mode.GetMcsValue() << " at " << snrDb
                                                     << " dB and " << packetSize << " bytes");
                }
            }
        }
    }
};

class HePerMonotonicityTest : public TestCase
{
  public:
    HePerMonotonicityTest()
        : TestCase("frequency-selective HE PER is monotonic with reference SNR")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix channel(4, 1, 1);
        channel.Set(0, 0, 0, {std::sqrt(0.25), 0.0});
        channel.Set(1, 0, 0, {std::sqrt(0.75), 0.0});
        channel.Set(2, 0, 0, {std::sqrt(1.25), 0.0});
        channel.Set(3, 0, 0, {std::sqrt(1.75), 0.0});

        const WifiMode modes[] = {HePhy::GetHeMcs0(),
                                  HePhy::GetHeMcs3(),
                                  HePhy::GetHeMcs7(),
                                  HePhy::GetHeMcs11()};
        const uint32_t packetSizes[] = {32, 400, 1458};
        auto abstraction = CreateObject<HeSuMimoLinkAbstraction>();
        for (const auto mode : modes)
        {
            // Positive formula sentinel only; no calibrated EESM table is implied.
            abstraction->SetEesmBeta(mode.GetMcsValue(), 1.0);
        }

        for (const auto mode : modes)
        {
            const auto txVector = MakeHeTxVector(mode, 1);
            for (const uint32_t packetSize : packetSizes)
            {
                double previousPer = 1.0;
                bool sawDecrease = false;
                for (double snrDb = -10.0; snrDb <= 40.0; snrDb += 2.0)
                {
                    const auto linkBudget =
                        MakeReferenceSnrBudget(WifiLinkBudget::DbToLinear(snrDb));
                    const double per =
                        abstraction->Calculate(channel, linkBudget, txVector, packetSize)
                            .GetPacketErrorRate();
                    NS_TEST_ASSERT_MSG_LT_OR_EQ(
                        per,
                        previousPer + 1e-12,
                        "PER increased for MCS " << +mode.GetMcsValue() << " at " << snrDb
                                                 << " dB and " << packetSize << " bytes");
                    sawDecrease = sawDecrease || (per + 1e-12 < previousPer);
                    previousPer = per;
                }
                NS_TEST_ASSERT_MSG_EQ(sawDecrease,
                                      true,
                                      "PER sweep never decreased for MCS "
                                          << +mode.GetMcsValue() << " and " << packetSize
                                          << " bytes");
            }
        }
    }
};

class HeLinkReproducibilityTest : public TestCase
{
  public:
    HeLinkReproducibilityTest()
        : TestCase("HE MIMO effective SINR and PER are exactly reproducible")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix channel(3, 2, 2);
        channel.Set(0, 0, 0, {1.0, 0.2});
        channel.Set(0, 0, 1, {0.1, -0.1});
        channel.Set(0, 1, 0, {-0.2, 0.3});
        channel.Set(0, 1, 1, {1.1, 0.0});
        channel.Set(1, 0, 0, {0.7, -0.4});
        channel.Set(1, 0, 1, {0.2, 0.1});
        channel.Set(1, 1, 0, {0.05, -0.2});
        channel.Set(1, 1, 1, {1.3, 0.2});
        channel.Set(2, 0, 0, {1.4, 0.0});
        channel.Set(2, 0, 1, {-0.1, 0.2});
        channel.Set(2, 1, 0, {0.3, 0.1});
        channel.Set(2, 1, 1, {0.8, -0.3});

        const auto linkBudget =
            MakeReferenceSnrBudget(WifiLinkBudget::DbToLinear(18.0));
        const auto txVector = MakeHeTxVector(HePhy::GetHeMcs7(), 2);
        auto abstraction = CreateObject<HeSuMimoLinkAbstraction>();
        // Formula-only sentinel for this deterministic regression test.
        abstraction->SetEesmBeta(7, 2.0);

        const auto reference = abstraction->Calculate(channel, linkBudget, txVector, 1458);
        for (uint32_t repetition = 0; repetition < 20; ++repetition)
        {
            const auto repeated = abstraction->Calculate(channel, linkBudget, txVector, 1458);
            NS_TEST_ASSERT_MSG_EQ(repeated.GetEffectiveSinrLinear(),
                                  reference.GetEffectiveSinrLinear(),
                                  "Effective SINR changed between identical calls");
            NS_TEST_ASSERT_MSG_EQ(repeated.GetPacketErrorRate(),
                                  reference.GetPacketErrorRate(),
                                  "PER changed between identical calls");
            for (uint32_t sc = 0; sc < reference.GetMimoResult().GetNumSubcarriers(); ++sc)
            {
                for (uint32_t stream = 0; stream < reference.GetMimoResult().GetNumStreams();
                     ++stream)
                {
                    NS_TEST_ASSERT_MSG_EQ(
                        repeated.GetMimoResult().GetSinrLinear(sc, stream),
                        reference.GetMimoResult().GetSinrLinear(sc, stream),
                        "Post-MMSE SINR changed between identical calls");
                }
            }
        }
    }
};

class SuMimoSisoReductionTest : public TestCase
{
  public:
    SuMimoSisoReductionTest()
        : TestCase("SU-MIMO MMSE formula reduces to scalar SISO SNR")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix channel(1, 1, 1);
        channel.Set(0, 0, 0, {2.0, 1.0});

        const auto result = SuMimoLinkModel::Calculate(channel, 10.0);
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetSinrLinear(0, 0),
                                  50.0,
                                  1e-10,
                                  "SISO reduction must equal rho times |h|^2");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetEffectiveSinrLinear(0),
                                  50.0,
                                  1e-10,
                                  "Single-tone effective SINR changed");
    }
};

class SuMimoOrthogonalChannelTest : public TestCase
{
  public:
    SuMimoOrthogonalChannelTest()
        : TestCase("2x2 orthogonal channel has two interference-free equal-power streams")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix channel(1, 2, 2);
        channel.Set(0, 0, 0, {1.0, 0.0});
        channel.Set(0, 1, 1, {1.0, 0.0});

        const auto result = SuMimoLinkModel::Calculate(channel, 20.0);
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetSinrLinear(0, 0),
                                  10.0,
                                  1e-10,
                                  "Stream 0 did not receive half the total power");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetSinrLinear(0, 1),
                                  10.0,
                                  1e-10,
                                  "Stream 1 did not receive half the total power");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetSpectralEfficiency(),
                                  2.0 * std::log2(11.0),
                                  1e-10,
                                  "Orthogonal-channel spectral efficiency is incorrect");
    }
};

class SuMimoFrequencySelectiveTest : public TestCase
{
  public:
    SuMimoFrequencySelectiveTest()
        : TestCase("frequency-selective SINR is compressed by mutual information")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix channel(2, 2, 2);
        channel.Set(0, 0, 0, {1.0, 0.0});
        channel.Set(0, 1, 1, {2.0, 0.0});
        channel.Set(1, 0, 0, {2.0, 0.0});
        channel.Set(1, 1, 1, {1.0, 0.0});

        const auto result = SuMimoLinkModel::Calculate(channel, 8.0);
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetSinrLinear(0, 0), 4.0, 1e-10, "Wrong tone SINR");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetSinrLinear(0, 1), 16.0, 1e-10, "Wrong tone SINR");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetSinrLinear(1, 0), 16.0, 1e-10, "Wrong tone SINR");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetSinrLinear(1, 1), 4.0, 1e-10, "Wrong tone SINR");

        const double expectedEffectiveSinr = std::sqrt(5.0 * 17.0) - 1.0;
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetEffectiveSinrLinear(0),
                                  expectedEffectiveSinr,
                                  1e-10,
                                  "Wrong effective SINR for stream 0");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetEffectiveSinrLinear(1),
                                  expectedEffectiveSinr,
                                  1e-10,
                                  "Wrong effective SINR for stream 1");
    }
};

class LegacySisoDeserializationTest : public TestCase
{
  public:
    LegacySisoDeserializationTest()
        : TestCase("missing protobuf antenna dimensions decode as legacy 1x1")
    {
    }

  private:
    void DoRun() override
    {
        ns3sionna::ChannelStateResponse::ChannelState::RxNodeInfo message;
        message.add_frequencies(-10);
        message.add_frequencies(10);
        message.add_csi_real(1.5);
        message.add_csi_real(2.5);
        message.add_csi_imag(-1.0);
        message.add_csi_imag(-2.0);

        const auto channel = SionnaPropagationCache::DeserializeChannelMatrix(message);
        NS_TEST_ASSERT_MSG_EQ(channel.GetNumSubcarriers(), 2, "Legacy subcarrier count changed");
        NS_TEST_ASSERT_MSG_EQ(channel.GetNumRxAntennas(), 1, "Legacy RX dimension is not 1");
        NS_TEST_ASSERT_MSG_EQ(channel.GetNumTxAntennas(), 1, "Legacy TX dimension is not 1");
        NS_TEST_ASSERT_MSG_EQ(channel.Get(1, 0, 0),
                              std::complex<double>(2.5, -2.0),
                              "Legacy SISO coefficient changed");
    }
};

class MimoProtobufDeserializationTest : public TestCase
{
  public:
    MimoProtobufDeserializationTest()
        : TestCase("synthetic 2x2 protobuf CFR arrives in exact subcarrier-RX-TX order")
    {
    }

  private:
    void DoRun() override
    {
        ns3sionna::Wrapper outgoing;
        auto* message =
            outgoing.mutable_channel_state_response()->add_csi()->add_rx_nodes();
        message->add_frequencies(-10);
        message->add_frequencies(10);
        message->set_num_rx_antennas(2);
        message->set_num_tx_antennas(2);
        for (int value = 1; value <= 8; ++value)
        {
            message->add_csi_real(value);
            message->add_csi_imag(-value);
        }

        std::string wireBytes;
        NS_TEST_ASSERT_MSG_EQ(outgoing.SerializeToString(&wireBytes),
                              true,
                              "Failed to serialize synthetic MIMO response");

        ns3sionna::Wrapper incoming;
        NS_TEST_ASSERT_MSG_EQ(incoming.ParseFromString(wireBytes),
                              true,
                              "Failed to parse synthetic MIMO response");
        const auto channel = SionnaPropagationCache::DeserializeChannelMatrix(
            incoming.channel_state_response().csi(0).rx_nodes(0));

        NS_TEST_ASSERT_MSG_EQ(channel.GetNumSubcarriers(), 2, "Wrong subcarrier count");
        NS_TEST_ASSERT_MSG_EQ(channel.GetNumRxAntennas(), 2, "Wrong RX dimension");
        NS_TEST_ASSERT_MSG_EQ(channel.GetNumTxAntennas(), 2, "Wrong TX dimension");
        NS_TEST_ASSERT_MSG_EQ(channel.Get(0, 1, 1),
                              std::complex<double>(4.0, -4.0),
                              "First subcarrier was reordered");
        NS_TEST_ASSERT_MSG_EQ(channel.Get(1, 0, 1),
                              std::complex<double>(6.0, -6.0),
                              "Second subcarrier was reordered");
        NS_TEST_ASSERT_MSG_EQ(channel.Get(1, 1, 1),
                              std::complex<double>(8.0, -8.0),
                              "Last MIMO coefficient was reordered");
    }
};

using ChannelStateMessage = ns3sionna::ChannelStateResponse::ChannelState;

void
AddMultiUserChannel(ChannelStateMessage& state, uint32_t userId, uint32_t numRx, double phase)
{
    auto* user = state.add_rx_nodes();
    user->set_id(userId);
    user->set_wb_loss(70.0 + userId);
    user->add_frequencies(-10);
    user->add_frequencies(10);
    user->set_num_rx_antennas(numRx);
    user->set_num_tx_antennas(2);
    for (uint32_t i = 0; i < 2 * numRx * 2; ++i)
    {
        user->add_csi_real(std::cos(phase + i));
        user->add_csi_imag(std::sin(phase + i));
    }
    auto* identity = user->mutable_user_identity();
    identity->set_user_id(userId);
    identity->set_tx_node_id(42);
    identity->set_rx_node_id(userId);
    identity->set_direction(ns3sionna::CHANNEL_DIRECTION_DOWNLINK);
    identity->set_csi_timestamp(1234);
    auto* array = identity->mutable_rx_array();
    array->set_num_rows(1);
    array->set_num_cols(numRx);
    array->set_vertical_spacing(0.5);
    array->set_horizontal_spacing(0.5);
    array->set_pattern("dipole");
    array->set_polarization("V");
}

ChannelStateMessage
MakeMultiUserChannelSnapshotMessage()
{
    ChannelStateMessage state;
    state.set_start_time(1234);
    state.set_end_time(2234);
    state.mutable_tx_node()->set_id(42);
    AddMultiUserChannel(state, 9, 2, 10.0);
    AddMultiUserChannel(state, 3, 1, 0.0);
    return state;
}

class MultiUserChannelSnapshotContractTest : public TestCase
{
  public:
    MultiUserChannelSnapshotContractTest()
        : TestCase("multi-user snapshot preserves stable ordering, dimensions, and RX arrays")
    {
    }

  private:
    void DoRun() override
    {
        ns3sionna::Wrapper outgoing;
        outgoing.mutable_channel_state_response()->add_csi()->CopyFrom(
            MakeMultiUserChannelSnapshotMessage());
        std::string wire;
        outgoing.SerializeToString(&wire);
        ns3sionna::Wrapper incoming;
        incoming.ParseFromString(wire);

        const auto snapshot = SionnaPropagationCache::DeserializeMultiUserChannelSnapshot(
            incoming.channel_state_response().csi(0));
        NS_TEST_ASSERT_MSG_EQ(snapshot.users.size(), 2, "Wrong number of users");
        NS_TEST_ASSERT_MSG_EQ(snapshot.users[0].userId, 3, "Users were not stably ordered");
        NS_TEST_ASSERT_MSG_EQ(snapshot.users[1].userId, 9, "Users were not stably ordered");
        NS_TEST_ASSERT_MSG_EQ(snapshot.users[0].txNodeId, 42, "TX identity changed");
        NS_TEST_ASSERT_MSG_EQ(snapshot.users[1].rxNodeId, 9, "RX identity changed");
        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(snapshot.direction),
                              static_cast<int>(MultiUserChannelDirection::DOWNLINK),
                              "Snapshot direction changed");
        NS_TEST_ASSERT_MSG_EQ(snapshot.csiTimestamp.GetNanoSeconds(),
                              1234,
                              "Shared CSI timestamp changed");
        NS_TEST_ASSERT_MSG_EQ(snapshot.users[0].channel.GetNumRxAntennas(),
                              1,
                              "First heterogeneous RX dimension changed");
        NS_TEST_ASSERT_MSG_EQ(snapshot.users[1].channel.GetNumRxAntennas(),
                              2,
                              "Second heterogeneous RX dimension changed");
        NS_TEST_ASSERT_MSG_EQ(snapshot.users[1].rxArray.numColumns,
                              2,
                              "Per-user RX geometry changed");
        NS_TEST_ASSERT_MSG_EQ(snapshot.users[1].frequencies[1],
                              10,
                              "Shared frequency grid changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(
            std::abs(snapshot.users[1].channel.Get(1, 0, 1) -
                     std::polar(1.0, 15.0)),
            0.0,
            1e-12,
            "Subcarrier/RX/TX tensor index changed");
    }
};

class MultiUserChannelSnapshotRejectionTest : public TestCase
{
  public:
    MultiUserChannelSnapshotRejectionTest()
        : TestCase("inconsistent multi-user snapshots are rejected before cache use")
    {
    }

  private:
    void DoRun() override
    {
        auto expectRejected = [this](const ChannelStateMessage& state, const std::string& reason) {
            bool rejected = false;
            try
            {
                SionnaPropagationCache::DeserializeMultiUserChannelSnapshot(state);
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, reason);
        };

        auto duplicate = MakeMultiUserChannelSnapshotMessage();
        duplicate.mutable_rx_nodes(1)->mutable_user_identity()->set_user_id(9);
        expectRejected(duplicate, "Duplicate user was accepted");

        auto timestamp = MakeMultiUserChannelSnapshotMessage();
        timestamp.mutable_rx_nodes(1)->mutable_user_identity()->set_csi_timestamp(1235);
        expectRejected(timestamp, "Mixed CSI timestamps were accepted");

        auto direction = MakeMultiUserChannelSnapshotMessage();
        direction.mutable_rx_nodes(1)->mutable_user_identity()->set_direction(
            ns3sionna::CHANNEL_DIRECTION_UPLINK);
        expectRejected(direction, "Mixed directions were accepted");

        auto frequencies = MakeMultiUserChannelSnapshotMessage();
        frequencies.mutable_rx_nodes(1)->set_frequencies(1, 11);
        expectRejected(frequencies, "Inconsistent frequency grids were accepted");

        auto array = MakeMultiUserChannelSnapshotMessage();
        array.mutable_rx_nodes(0)->mutable_user_identity()->mutable_rx_array()->set_num_cols(3);
        expectRejected(array, "RX geometry inconsistent with its tensor was accepted");
    }
};

MultiUserChannel
MakeDlMuUser(uint32_t userId, const MimoChannelMatrix& channel)
{
    std::vector<int> frequencies;
    for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
    {
        frequencies.push_back(static_cast<int>(sc));
    }
    return {userId,
            42,
            userId,
            MultiUserChannelDirection::DOWNLINK,
            NanoSeconds(1234),
            {1, channel.GetNumRxAntennas(), 0.5, 0.5, "dipole", "V"},
            std::move(frequencies),
            0.0,
            channel};
}

MultiUserChannelSnapshot
MakeDlMuSnapshot(const std::vector<std::pair<uint32_t, MimoChannelMatrix>>& users)
{
    MultiUserChannelSnapshot snapshot{NanoSeconds(1234),
                                      MultiUserChannelDirection::DOWNLINK,
                                      {}};
    for (const auto& user : users)
    {
        snapshot.users.push_back(MakeDlMuUser(user.first, user.second));
    }
    return snapshot;
}

DlMuMimoPrecodingMatrix
MakeIdentityDlMuPrecoding(uint32_t numSubcarriers)
{
    return DlMuMimoPrecodingMatrix(
        MimoPrecodingMatrix::FrequencyFlat(
            numSubcarriers,
            2,
            2,
            {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}}),
        {{3, 0}, {9, 0}});
}

WifiTxVector
MakeHeDlMuTxVector(const HeRu::RuSpec& user3Ru,
                   const HeRu::RuSpec& user9Ru,
                   uint8_t user3Nss = 1,
                   uint8_t user9Nss = 1)
{
    WifiTxVector txVector;
    txVector.SetPreambleType(WIFI_PREAMBLE_HE_MU);
    txVector.SetChannelWidth(20);
    txVector.SetGuardInterval(800);
    txVector.SetHeMuUserInfo(11, {user9Ru, 3, user9Nss});
    txVector.SetHeMuUserInfo(7, {user3Ru, 0, user3Nss});
    return txVector;
}

class DlMuMimoFixedPrecodingAnalyticalTest : public TestCase
{
  public:
    DlMuMimoFixedPrecodingAnalyticalTest()
        : TestCase("fixed downlink MU precoding preserves SU, power, and analytical IUI")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix suChannel(2, 2, 2);
        suChannel.Set(0, 0, 0, {1.0, 1.0});
        suChannel.Set(0, 0, 1, {2.0, 0.0});
        suChannel.Set(0, 1, 0, {0.0, 3.0});
        suChannel.Set(0, 1, 1, {4.0, -1.0});
        suChannel.Set(1, 0, 0, {0.5, -0.5});
        suChannel.Set(1, 0, 1, {-1.0, 2.0});
        suChannel.Set(1, 1, 0, {3.0, 1.0});
        suChannel.Set(1, 1, 1, {-2.0, -1.0});
        const auto suPrecoding = MimoPrecodingMatrix::FrequencyFlat(
            2,
            2,
            2,
            {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}});
        const DlMuMimoPrecodingMatrix singleUserPrecoding(suPrecoding,
                                                          {{3, 0}, {3, 1}});
        const auto singleUserSnapshot = MakeDlMuSnapshot({{3, suChannel}});
        const auto muSingle = DlMuMimoLinkModel::Calculate(singleUserSnapshot,
                                                           singleUserPrecoding,
                                                           {{3, 7.5}});
        const auto suReference = SuMimoLinkModel::Calculate(suChannel, suPrecoding, 7.5);
        NS_TEST_ASSERT_MSG_EQ(muSingle.GetNumUsers(), 1, "Single-user MU result changed U");
        for (uint32_t sc = 0; sc < suChannel.GetNumSubcarriers(); ++sc)
        {
            for (uint32_t stream = 0; stream < 2; ++stream)
            {
                NS_TEST_ASSERT_MSG_EQ_TOL(
                    muSingle.GetUserById(3).GetSinrLinear(sc, stream),
                    suReference.GetSinrLinear(sc, stream),
                    1e-10,
                    "One-user downlink MU did not reproduce the Phase-3 fixed-precoder SINR");
                NS_TEST_ASSERT_MSG_EQ_TOL(
                    muSingle.GetUserById(3).GetInterUserInterferencePower(sc, stream),
                    0.0,
                    1e-12,
                    "One-user downlink MU reported inter-user interference");
            }
        }

        MimoChannelMatrix sisoChannel(1, 1, 1);
        sisoChannel.Set(0, 0, 0, {2.0, 1.0});
        const DlMuMimoPrecodingMatrix sisoPrecoding(
            MimoPrecodingMatrix::FrequencyFlat(1, 1, 1, {{0.0, 1.0}}),
            {{3, 0}});
        const auto sisoSnapshot = MakeDlMuSnapshot({{3, sisoChannel}});
        const auto muSiso =
            DlMuMimoLinkModel::Calculate(sisoSnapshot, sisoPrecoding, {{3, 10.0}});
        NS_TEST_ASSERT_MSG_EQ_TOL(
            muSiso.GetUserById(3).GetSinrLinear(0, 0),
            SuMimoLinkModel::Calculate(sisoChannel,
                                       sisoPrecoding.GetPrecodingMatrix(),
                                       10.0)
                .GetSinrLinear(0, 0),
            1e-10,
            "One-user 1x1 MU did not reproduce the Phase-3 fixed-precoder result");

        MimoChannelMatrix orthogonalUser3(1, 1, 2);
        orthogonalUser3.Set(0, 0, 0, {1.0, 0.0});
        MimoChannelMatrix orthogonalUser9(1, 1, 2);
        orthogonalUser9.Set(0, 0, 1, {1.0, 0.0});
        const auto orthogonalSnapshot =
            MakeDlMuSnapshot({{3, orthogonalUser3}, {9, orthogonalUser9}});
        const auto identity = MakeIdentityDlMuPrecoding(1);
        const auto orthogonal =
            DlMuMimoLinkModel::Calculate(orthogonalSnapshot, identity, {{9, 10.0}, {3, 10.0}});
        NS_TEST_ASSERT_MSG_EQ(orthogonal.GetUser(0).GetUserId(),
                              3,
                              "MU results are not ordered by stable user ID");
        for (const uint32_t userId : {3u, 9u})
        {
            const auto& user = orthogonal.GetUserById(userId);
            NS_TEST_ASSERT_MSG_EQ_TOL(user.GetInterUserInterferencePower(0, 0),
                                      0.0,
                                      1e-12,
                                      "Orthogonal users produced inter-user interference");
            NS_TEST_ASSERT_MSG_EQ_TOL(user.GetSinrLinear(0, 0),
                                      5.0,
                                      1e-10,
                                      "Orthogonal-user SINR did not use total-stream power");
        }

        MimoChannelMatrix correlatedUser3(1, 1, 2);
        correlatedUser3.Set(0, 0, 0, {1.0, 0.0});
        correlatedUser3.Set(0, 0, 1, {1.0, 0.0});
        MimoChannelMatrix correlatedUser9 = correlatedUser3;
        const auto correlatedSnapshot =
            MakeDlMuSnapshot({{3, correlatedUser3}, {9, correlatedUser9}});
        const auto correlated =
            DlMuMimoLinkModel::Calculate(correlatedSnapshot, identity, {{3, 10.0}, {9, 10.0}});
        constexpr double postCombiningDesired = 5.0 / 121.0;
        constexpr double postCombiningInterference = 5.0 / 121.0;
        constexpr double correlatedSinr = 5.0 / 6.0;
        for (const uint32_t userId : {3u, 9u})
        {
            const auto& user = correlated.GetUserById(userId);
            NS_TEST_ASSERT_MSG_EQ_TOL(user.GetDesiredSignalPower(0, 0),
                                      postCombiningDesired,
                                      1e-12,
                                      "Correlated-user desired power differs from the analytical MMSE value");
            NS_TEST_ASSERT_MSG_EQ_TOL(
                user.GetInterUserInterferencePower(0, 0),
                postCombiningInterference,
                1e-12,
                "Correlated-user interference differs from the analytical MMSE value");
            NS_TEST_ASSERT_MSG_EQ_TOL(user.GetSinrLinear(0, 0),
                                      correlatedSinr,
                                      1e-10,
                                      "Correlated-user post-MMSE SINR is not analytical");
        }

        const DlMuMimoPrecodingMatrix weighted(
            MimoPrecodingMatrix::FrequencyFlat(
                1,
                2,
                2,
                {{std::sqrt(1.5), 0.0},
                 {0.0, 0.0},
                 {0.0, 0.0},
                 {std::sqrt(0.5), 0.0}}),
            {{3, 0}, {9, 0}});
        weighted.ValidateForSnapshot(orthogonalSnapshot);
        NS_TEST_ASSERT_MSG_EQ_TOL(weighted.GetStreamPower(0, 0),
                                  1.5,
                                  1e-12,
                                  "Explicit first-stream power weight changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(weighted.GetStreamPower(0, 1),
                                  0.5,
                                  1e-12,
                                  "Explicit second-stream power weight changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(weighted.GetPrecodingMatrix().GetPower(0),
                                  2.0,
                                  1e-12,
                                  "Unequal MU weights changed total AP power");
        NS_TEST_ASSERT_MSG_EQ(weighted.GetStreamMapping(1).userId,
                              9,
                              "Joint column-to-user mapping changed");
    }
};

class HeDlMuMimoPerUserContractTest : public TestCase
{
  public:
    HeDlMuMimoPerUserContractTest()
        : TestCase("downlink MU applies EESM and PER separately per user")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix user3Channel(2, 1, 2);
        user3Channel.Set(0, 0, 0, {1.0, 0.0});
        user3Channel.Set(1, 0, 0, {2.0, 0.0});
        MimoChannelMatrix user9Channel(2, 1, 2);
        user9Channel.Set(0, 0, 1, {0.5, 0.0});
        user9Channel.Set(1, 0, 1, {1.5, 0.0});
        const auto snapshot = MakeDlMuSnapshot({{3, user3Channel}, {9, user9Channel}});
        const auto precoding = MakeIdentityDlMuPrecoding(2);
        const auto budget3 = MakeReferenceSnrBudget(10.0);
        const auto budget9 = MakeReferenceSnrBudget(6.0);
        const auto tx3 = MakeHeTxVector(HePhy::GetHeMcs0(), 1);
        const auto tx9 = MakeHeTxVector(HePhy::GetHeMcs3(), 1);

        auto abstraction = CreateObject<HeDlMuMimoLinkAbstraction>();
        abstraction->SetEesmBeta(0, 2.0);
        abstraction->SetEesmBeta(3, 3.0);
        const auto result = abstraction->Calculate(
            snapshot,
            precoding,
            {{9, budget9, tx9, 1000}, {3, budget3, tx3, 400}});

        auto suEesm = CreateObject<HeSuMimoLinkAbstraction>();
        suEesm->SetEesmBeta(0, 2.0);
        suEesm->SetEesmBeta(3, 3.0);
        for (const auto& input : std::vector<HeDlMuMimoUserTransmission>{
                 {3, budget3, tx3, 400},
                 {9, budget9, tx9, 1000}})
        {
            const auto& user = result.GetUserById(input.userId);
            std::vector<double> samples;
            for (uint32_t sc = 0; sc < user.GetMimoResult().GetNumSubcarriers(); ++sc)
            {
                samples.push_back(user.GetMimoResult().GetSinrLinear(sc, 0));
            }
            const uint8_t mcs = input.txVector.GetMode().GetMcsValue();
            const double expectedEesm = suEesm->CalculateEffectiveSinr(samples, mcs);
            NS_TEST_ASSERT_MSG_EQ_TOL(user.GetEffectiveSinrLinear(),
                                      expectedEesm,
                                      1e-12,
                                      "Per-user EESM did not use that user's MCS");
            const double expectedSuccess =
                abstraction->GetErrorRateModel()->GetChunkSuccessRate(
                    input.txVector.GetMode(),
                    input.txVector,
                    expectedEesm,
                    static_cast<uint64_t>(input.packetSizeBytes) * 8,
                    1);
            NS_TEST_ASSERT_MSG_EQ_TOL(user.GetPacketSuccessRate(),
                                      expectedSuccess,
                                      1e-12,
                                      "Per-user PER did not use that user's packet size");
        }

        abstraction->SetAwgnSnrOffsetDb({0, false, 400, 1, 1, 20, 800}, -10.0);
        const auto calibrated = abstraction->Calculate(
            snapshot,
            precoding,
            {{9, budget9, tx9, 1000}, {3, budget3, tx3, 400}});
        const auto& calibrated3 = calibrated.GetUserById(3);
        const double correctedSuccess =
            abstraction->GetErrorRateModel()->GetChunkSuccessRate(
                HePhy::GetHeMcs0(),
                tx3,
                calibrated3.GetEffectiveSinrLinear() * WifiLinkBudget::DbToLinear(-10.0),
                400 * 8,
                1);
        NS_TEST_ASSERT_MSG_EQ_TOL(calibrated3.GetPacketSuccessRate(),
                                  correctedSuccess,
                                  1e-12,
                                  "DL MU did not apply the addressed user's AWGN calibration");
        NS_TEST_ASSERT_MSG_EQ((calibrated3.GetPacketSuccessRate() !=
                               result.GetUserById(3).GetPacketSuccessRate()),
                              true,
                              "DL MU calibration did not change the addressed user's PER");
        NS_TEST_ASSERT_MSG_EQ_TOL(calibrated.GetUserById(9).GetPacketSuccessRate(),
                                  result.GetUserById(9).GetPacketSuccessRate(),
                                  1e-12,
                                  "DL MU AWGN calibration leaked to a different MCS and length");
    }
};

class HeDlMuMimoSpectrumAdapterTest : public TestCase
{
  public:
    HeDlMuMimoSpectrumAdapterTest()
        : TestCase("HE DL MU spectrum adapter maps STA metadata, RU interference, SINR, PER, and power")
    {
    }

  private:
    void DoRun() override
    {
        constexpr double noiseFigureDb = 7.0;
        constexpr double referenceSnr = 10.0;
        const HeRu::RuSpec ru1(HeRu::RU_106_TONE, 1, true);
        const HeRu::RuSpec ru2(HeRu::RU_106_TONE, 2, true);
        const double bandwidthHz =
            static_cast<double>(HeRu::GetBandwidth(HeRu::RU_106_TONE)) * 1e6;
        const double noisePowerW =
            WifiLinkBudget::Calculate(0.0, 0.0, bandwidthHz, 290.0, noiseFigureDb)
                .GetReceiverNoisePowerW();
        const double receivedPowerW = referenceSnr * noisePowerW;

        MimoChannelMatrix user3Channel(1, 1, 2);
        user3Channel.Set(0, 0, 0, {1.0, 0.0});
        user3Channel.Set(0, 0, 1, {1.0, 0.0});
        MimoChannelMatrix user9Channel = user3Channel;
        const auto snapshot =
            MakeDlMuSnapshot({{3, user3Channel}, {9, user9Channel}});
        const auto precoding = MakeIdentityDlMuPrecoding(1);
        auto errorModel = CreateObject<TableBasedErrorRateModel>();
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        adapter->SetEesmBeta(0, 2.0);
        adapter->SetEesmBeta(3, 3.0);

        const auto sameRuVector = MakeHeDlMuTxVector(ru1, ru1);
        const std::vector<SionnaHeDlMuMimoSpectrumUserTransmission> users{
            {9, 11, receivedPowerW, 1, 1000},
            {3, 7, receivedPowerW, 1, 400}};
        const auto sameRu =
            adapter->CalculateHeDlMuMimoRx(snapshot,
                                           precoding,
                                           sameRuVector,
                                           users,
                                           2,
                                           noiseFigureDb,
                                           errorModel);
        for (const auto& input : users)
        {
            const auto& link = sameRu.GetLinkResult().GetUserById(input.userId);
            NS_TEST_ASSERT_MSG_EQ_TOL(
                link.GetMimoResult().GetInterUserInterferencePower(0, 0),
                5.0 / 121.0,
                1e-12,
                "Same-RU spatially multiplexed user has incorrect analytical IUI");
            NS_TEST_ASSERT_MSG_EQ_TOL(link.GetEffectiveSinrLinear(),
                                      5.0 / 6.0,
                                      1e-12,
                                      "Same-RU spatially multiplexed user has incorrect SINR");
            const double expectedSuccess =
                errorModel->GetChunkSuccessRate(
                    sameRuVector.GetMode(input.staId),
                    sameRuVector,
                    5.0 / 6.0,
                    static_cast<uint64_t>(input.packetSizeBytes) * 8,
                    1,
                    WIFI_PPDU_FIELD_DATA,
                    input.staId);
            NS_TEST_ASSERT_MSG_EQ_TOL(link.GetPacketSuccessRate(),
                                      expectedSuccess,
                                      1e-12,
                                      "PER did not use the addressed STA-ID MCS and packet size");
            NS_TEST_ASSERT_MSG_EQ_TOL(
                sameRu.GetEquivalentReceivedPowerW(input.userId),
                (5.0 / 6.0) * noisePowerW,
                noisePowerW * 1e-12,
                "Equivalent PSD power did not apply noise exactly once");
            NS_TEST_ASSERT_MSG_EQ_TOL(link.GetLinkBudget().GetReceivedPowerW(),
                                      receivedPowerW,
                                      receivedPowerW * 1e-12,
                                      "Per-user received power changed in the MU adapter");
        }
        NS_TEST_ASSERT_MSG_EQ(sameRuVector.GetMode(7).GetMcsValue(),
                              0,
                              "User 3 was not selected through STA-ID 7");
        NS_TEST_ASSERT_MSG_EQ(sameRuVector.GetMode(11).GetMcsValue(),
                              3,
                              "User 9 was not selected through STA-ID 11");

        const auto differentRuVector = MakeHeDlMuTxVector(ru1, ru2);
        const auto differentRu =
            adapter->CalculateHeDlMuMimoRx(snapshot,
                                           precoding,
                                           differentRuVector,
                                           users,
                                           2,
                                           noiseFigureDb,
                                           errorModel);
        for (const uint32_t userId : {3u, 9u})
        {
            const auto& link = differentRu.GetLinkResult().GetUserById(userId);
            NS_TEST_ASSERT_MSG_EQ_TOL(
                link.GetMimoResult().GetInterUserInterferencePower(0, 0),
                0.0,
                1e-12,
                "Different-RU users were not orthogonal");
            NS_TEST_ASSERT_MSG_EQ_TOL(link.GetEffectiveSinrLinear(),
                                      referenceSnr,
                                      1e-12,
                                      "Different-RU user did not retain the full RU power");
            NS_TEST_ASSERT_MSG_EQ_TOL(
                differentRu.GetEquivalentReceivedPowerW(userId),
                receivedPowerW,
                receivedPowerW * 1e-12,
                "Different-RU equivalent power changed the per-RU AP power convention");
        }

        MimoChannelMatrix twoStreamChannel(1, 2, 2);
        twoStreamChannel.Set(0, 0, 0, {1.0, 0.0});
        twoStreamChannel.Set(0, 1, 1, {1.0, 0.0});
        const auto twoStreamSnapshot = MakeDlMuSnapshot({{3, twoStreamChannel}});
        const DlMuMimoPrecodingMatrix twoStreamPrecoding(
            MimoPrecodingMatrix::FrequencyFlat(
                1,
                2,
                2,
                {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}}),
            {{3, 0}, {3, 1}});
        WifiTxVector twoStreamVector;
        twoStreamVector.SetPreambleType(WIFI_PREAMBLE_HE_MU);
        twoStreamVector.SetChannelWidth(20);
        twoStreamVector.SetGuardInterval(800);
        twoStreamVector.SetHeMuUserInfo(7, {ru1, 0, 2});
        const auto twoStream =
            adapter->CalculateHeDlMuMimoRx(twoStreamSnapshot,
                                           twoStreamPrecoding,
                                           twoStreamVector,
                                           {{3, 7, receivedPowerW, 2, 400}},
                                           2,
                                           noiseFigureDb,
                                           errorModel);
        NS_TEST_ASSERT_MSG_EQ(
            twoStream.GetLinkResult().GetUserById(3).GetMimoResult().GetNumStreams(),
            2,
            "Per-user HE-MU NSS did not select both mapped streams");

        MimoChannelMatrix sisoChannel(1, 1, 1);
        sisoChannel.Set(0, 0, 0, {1.0, 0.0});
        const auto sisoSnapshot = MakeDlMuSnapshot({{3, sisoChannel}});
        const DlMuMimoPrecodingMatrix sisoPrecoding(
            MimoPrecodingMatrix::FrequencyFlat(1, 1, 1, {{1.0, 0.0}}),
            {{3, 0}});
        WifiTxVector sisoVector;
        sisoVector.SetPreambleType(WIFI_PREAMBLE_HE_MU);
        sisoVector.SetChannelWidth(20);
        sisoVector.SetGuardInterval(800);
        sisoVector.SetHeMuUserInfo(7, {ru1, 0, 1});
        const auto siso =
            adapter->CalculateHeDlMuMimoRx(sisoSnapshot,
                                           sisoPrecoding,
                                           sisoVector,
                                           {{3, 7, receivedPowerW, 1, 400}},
                                           1,
                                           noiseFigureDb,
                                           errorModel);
        NS_TEST_ASSERT_MSG_EQ_TOL(
            siso.GetLinkResult().GetUserById(3).GetEffectiveSinrLinear(),
            referenceSnr,
            1e-12,
            "The HE-MU adapter changed normalized 1x1 behavior");
    }
};

class HeDlMuMimoSpectrumAdapterRejectionTest : public TestCase
{
  public:
    HeDlMuMimoSpectrumAdapterRejectionTest()
        : TestCase("HE DL MU spectrum adapter rejects invalid receivers atomically and preserves SU")
    {
    }

  private:
    void DoRun() override
    {
        constexpr double noiseFigureDb = 7.0;
        const HeRu::RuSpec ru(HeRu::RU_106_TONE, 1, true);
        MimoChannelMatrix user3Channel(1, 1, 2);
        user3Channel.Set(0, 0, 0, {1.0, 0.0});
        MimoChannelMatrix user9Channel(1, 1, 2);
        user9Channel.Set(0, 0, 1, {1.0, 0.0});
        const auto snapshot =
            MakeDlMuSnapshot({{3, user3Channel}, {9, user9Channel}});
        const auto precoding = MakeIdentityDlMuPrecoding(1);
        const auto txVector = MakeHeDlMuTxVector(ru, ru);
        const auto errorModel = CreateObject<TableBasedErrorRateModel>();
        const double receivedPowerW =
            10.0 * WifiLinkBudget::Calculate(0.0,
                                             0.0,
                                             static_cast<double>(
                                                 HeRu::GetBandwidth(HeRu::RU_106_TONE)) *
                                                 1e6,
                                             290.0,
                                             noiseFigureDb)
                       .GetReceiverNoisePowerW();
        const std::vector<SionnaHeDlMuMimoSpectrumUserTransmission> users{
            {3, 7, receivedPowerW, 1, 400},
            {9, 11, receivedPowerW, 1, 1000}};

        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        adapter->SetEesmBeta(0, 2.0);
        adapter->SetEesmBeta(3, 3.0);
        adapter->SetDlMuMimoState(snapshot, precoding);
        NS_TEST_ASSERT_MSG_EQ(adapter->HasDlMuMimoState(),
                              true,
                              "Valid HE-DL-MU state was not installed");

        auto expectRejected = [this](const auto& operation, const std::string& reason) {
            bool rejected = false;
            try
            {
                operation();
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, reason);
        };

        expectRejected(
            [&]() {
                auto unknown = users;
                unknown[0].userId = 12;
                adapter->CalculateHeDlMuMimoRx(snapshot,
                                               precoding,
                                               txVector,
                                               unknown,
                                               2,
                                               noiseFigureDb,
                                               errorModel);
            },
            "Unknown snapshot receiver was accepted");
        expectRejected(
            [&]() {
                auto unaddressed = txVector;
                unaddressed.GetHeMuUserInfoMap().erase(11);
                adapter->CalculateHeDlMuMimoRx(snapshot,
                                               precoding,
                                               unaddressed,
                                               users,
                                               2,
                                               noiseFigureDb,
                                               errorModel);
            },
            "Unaddressed receiver was accepted");
        expectRejected(
            [&]() {
                auto mismatch = users;
                mismatch[0].rxPhyAntennas = 2;
                adapter->CalculateHeDlMuMimoRx(snapshot,
                                               precoding,
                                               txVector,
                                               mismatch,
                                               2,
                                               noiseFigureDb,
                                               errorModel);
            },
            "Receiver dimension mismatch was accepted");
        expectRejected(
            [&]() {
                auto mismatch = txVector;
                mismatch.SetNss(2, 7);
                adapter->CalculateHeDlMuMimoRx(snapshot,
                                               precoding,
                                               mismatch,
                                               users,
                                               2,
                                               noiseFigureDb,
                                               errorModel);
            },
            "Per-user NSS mismatch was accepted");
        expectRejected(
            [&]() {
                adapter->CalculateHeDlMuMimoRx(snapshot,
                                               precoding,
                                               txVector,
                                               users,
                                               1,
                                               noiseFigureDb,
                                               errorModel);
            },
            "Transmitter dimension mismatch was accepted");
        expectRejected(
            [&]() {
                auto wrongDirection = snapshot;
                wrongDirection.direction = MultiUserChannelDirection::UPLINK;
                for (auto& user : wrongDirection.users)
                {
                    user.direction = MultiUserChannelDirection::UPLINK;
                }
                adapter->CalculateHeDlMuMimoRx(wrongDirection,
                                               precoding,
                                               txVector,
                                               users,
                                               2,
                                               noiseFigureDb,
                                               errorModel);
            },
            "Uplink state was accepted by the downlink adapter");
        expectRejected(
            [&]() {
                adapter->CalculateHeDlMuMimoRx(snapshot,
                                               precoding,
                                               MakeHeTxVector(HePhy::GetHeMcs0(), 1),
                                               users,
                                               2,
                                               noiseFigureDb,
                                               errorModel);
            },
            "HE-SU preamble was accepted by the HE-MU adapter");
        expectRejected(
            [&]() {
                auto eht = txVector;
                eht.SetPreambleType(WIFI_PREAMBLE_EHT_MU);
                adapter->CalculateHeDlMuMimoRx(snapshot,
                                               precoding,
                                               eht,
                                               users,
                                               2,
                                               noiseFigureDb,
                                               errorModel);
            },
            "EHT-MU preamble was accepted");

        auto invalidState = snapshot;
        invalidState.direction = MultiUserChannelDirection::UPLINK;
        for (auto& user : invalidState.users)
        {
            user.direction = MultiUserChannelDirection::UPLINK;
        }
        expectRejected(
            [&]() {
                adapter->SetDlMuMimoState(invalidState, precoding);
            },
            "Invalid replacement state was accepted");
        NS_TEST_ASSERT_MSG_EQ(adapter->HasDlMuMimoState(),
                              true,
                              "Rejected replacement cleared the valid MU state");
        const auto validAfterRejections =
            adapter->CalculateHeDlMuMimoRx(snapshot,
                                           precoding,
                                           txVector,
                                           users,
                                           2,
                                           noiseFigureDb,
                                           errorModel);
        expectRejected(
            [&]() {
                validAfterRejections.GetEquivalentReceivedPowerW(12);
            },
            "Unknown result user was accepted");

        MimoChannelMatrix sisoChannel(1, 1, 1);
        sisoChannel.Set(0, 0, 0, {1.0, 0.0});
        const auto suBefore =
            adapter->CalculateMimoRx(sisoChannel,
                                     receivedPowerW,
                                     MakeHeTxVector(HePhy::GetHeMcs0(), 1),
                                     400,
                                     1,
                                     1,
                                     20e6,
                                     noiseFigureDb,
                                     errorModel);
        const auto suAfter =
            adapter->CalculateMimoRx(sisoChannel,
                                     receivedPowerW,
                                     MakeHeTxVector(HePhy::GetHeMcs0(), 1),
                                     400,
                                     1,
                                     1,
                                     20e6,
                                     noiseFigureDb,
                                     errorModel);
        NS_TEST_ASSERT_MSG_EQ_TOL(suAfter.GetLinkResult().GetEffectiveSinrLinear(),
                                  suBefore.GetLinkResult().GetEffectiveSinrLinear(),
                                  1e-12,
                                  "Installed Phase-4.4 state changed the SU adapter path");
        NS_TEST_ASSERT_MSG_EQ_TOL(suAfter.GetLinkResult().GetPacketErrorRate(),
                                  suBefore.GetLinkResult().GetPacketErrorRate(),
                                  1e-12,
                                  "Installed Phase-4.4 state changed SU PER");

        adapter->ClearDlMuMimoState();
        NS_TEST_ASSERT_MSG_EQ(adapter->HasDlMuMimoState(),
                              false,
                              "HE-DL-MU state was not cleared");
    }
};

class HeDlMuMimoSpectrumPpduIntegrationTest : public TestCase
{
  public:
    HeDlMuMimoSpectrumPpduIntegrationTest()
        : TestCase("true HE DL MU PPDU runs joint reception before scalar PSD handoff")
    {
    }

  private:
    void DoRun() override
    {
        constexpr double noiseFigureDb = 7.0;
        constexpr double rxGainDb = 3.0;
        constexpr double txPowerW = 1e-6;
        constexpr uint16_t width = 20;

        auto spectrumChannel = CreateObject<MultiModelSpectrumChannel>();
        NodeContainer apNode;
        NodeContainer staNodes;
        apNode.Create(1);
        staNodes.Create(2);
        apNode.Get(0)->AggregateObject(CreateObject<ConstantPositionMobilityModel>());
        apNode.Get(0)->GetObject<MobilityModel>()->SetPosition({0.0, 0.0, 0.0});
        for (auto node = staNodes.Begin(); node != staNodes.End(); ++node)
        {
            (*node)->AggregateObject(CreateObject<ConstantPositionMobilityModel>());
            (*node)->GetObject<MobilityModel>()->SetPosition({1.0, 0.0, 0.0});
        }

        SpectrumWifiPhyHelper apPhyHelper;
        SpectrumWifiPhyHelper staPhyHelper;
        apPhyHelper.SetChannel(spectrumChannel);
        staPhyHelper.SetChannel(spectrumChannel);
        ConfigureManualPhy(apPhyHelper, 2, 2, noiseFigureDb);
        ConfigureManualPhy(staPhyHelper, 1, 1, noiseFigureDb);
        staPhyHelper.Set("RxGain", DoubleValue(rxGainDb));

        WifiHelper wifi;
        wifi.SetStandard(WIFI_STANDARD_80211ax);
        WifiMacHelper apMac;
        WifiMacHelper staMac;
        apMac.SetType("ns3::ApWifiMac");
        staMac.SetType("ns3::StaWifiMac");
        auto apDevice =
            DynamicCast<WifiNetDevice>(wifi.Install(apPhyHelper, apMac, apNode).Get(0));
        const auto staDevices = wifi.Install(staPhyHelper, staMac, staNodes);
        apNode.Get(0)->GetObject<MobilityModel>()->SetPosition({0.0, 0.0, 0.0});
        staNodes.Get(0)->GetObject<MobilityModel>()->SetPosition({1.0, 0.0, 0.0});
        staNodes.Get(1)->GetObject<MobilityModel>()->SetPosition({2.0, 0.0, 0.0});
        NS_TEST_ASSERT_MSG_GT(
            apNode.Get(0)->GetObject<MobilityModel>()->GetDistanceFrom(
                staNodes.Get(0)->GetObject<MobilityModel>()),
            0.0,
            "PPDU test nodes did not retain distinct positions");
        auto apPhy = DynamicCast<SpectrumWifiPhy>(apDevice->GetPhy());
        std::vector<Ptr<SpectrumWifiPhy>> staPhys;
        for (uint32_t index = 0; index < staDevices.GetN(); ++index)
        {
            staPhys.push_back(DynamicCast<SpectrumWifiPhy>(
                DynamicCast<WifiNetDevice>(staDevices.Get(index))->GetPhy()));
        }
        const WifiPhy::ChannelTuple channel{36, width, WIFI_PHY_BAND_5GHZ, 0};
        apPhy->SetOperatingChannel(channel);
        for (const auto& phy : staPhys)
        {
            phy->SetOperatingChannel(channel);
        }

        const HeRu::RuSpec fullRu(HeRu::GetRuType(width), 1, true);
        auto txVector = MakeHeDlMuTxVector(fullRu, fullRu);
        txVector.SetSigBMode(HePhy::GetVhtMcs(5));
        const std::vector<uint16_t> staIds{7, 11};
        const std::vector<uint32_t> packetSizes{400, 1000};
        WifiConstPsduMap psdus;
        for (std::size_t index = 0; index < staIds.size(); ++index)
        {
            WifiMacHeader header;
            header.SetType(WIFI_MAC_QOSDATA);
            header.SetQosTid(0);
            header.SetAddr1(
                DynamicCast<WifiNetDevice>(staDevices.Get(index))->GetMac()->GetAddress());
            header.SetAddr2(apDevice->GetMac()->GetAddress());
            header.SetSequenceNumber(index + 1);
            psdus.emplace(staIds[index],
                          Create<WifiPsdu>(Create<Packet>(packetSizes[index]), header));
        }
        auto ppdu = Create<HePpdu>(psdus,
                                   txVector,
                                   apPhy->GetOperatingChannel(),
                                   MicroSeconds(100),
                                   44,
                                   HePpdu::PSD_HE_PORTION);
        const auto ppduTxVector = ppdu->GetTxVector();
        const uint16_t center =
            apPhy->GetOperatingChannel().GetPrimaryChannelCenterFrequency(width);
        auto txPsd = WifiSpectrumValueHelper::CreateHeOfdmTxPowerSpectralDensity(
            center,
            width,
            txPowerW,
            apPhy->GetGuardBandwidth(width));

        const uint32_t numSubcarriers = txPsd->GetValuesN() - 1;
        MimoChannelMatrix user3Channel(numSubcarriers, 1, 2);
        MimoChannelMatrix user9Channel(numSubcarriers, 1, 2);
        for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
        {
            for (uint32_t tx = 0; tx < 2; ++tx)
            {
                user3Channel.Set(sc, 0, tx, {1.0, 0.0});
                user9Channel.Set(sc, 0, tx, {1.0, 0.0});
            }
        }
        auto snapshot =
            MakeDlMuSnapshot({{3, user3Channel}, {9, user9Channel}});
        const std::vector<double> pathLossDb{6.0, 12.0};
        for (std::size_t index = 0; index < snapshot.users.size(); ++index)
        {
            snapshot.users[index].txNodeId = apNode.Get(0)->GetId();
            snapshot.users[index].rxNodeId = staNodes.Get(index)->GetId();
            snapshot.users[index].widebandLossDb = pathLossDb[index];
        }
        const auto precoding = MakeIdentityDlMuPrecoding(numSubcarriers);
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        adapter->SetReceiverNoiseFigureDb(noiseFigureDb);
        adapter->SetEesmBeta(0, 2.0);
        adapter->SetEesmBeta(3, 3.0);
        adapter->SetDlMuMimoState(snapshot, precoding);

        auto hePhy =
            DynamicCast<HePhy>(staPhys[0]->GetPhyEntity(WIFI_MOD_CLASS_HE));
        const auto ruBand = hePhy->GetRuBandForRx(ppduTxVector, 7);
        const double apRuPowerW = IntegrateSpectrumBand(*txPsd, ruBand.indices);
        const double rxGainLinear = std::pow(10.0, rxGainDb / 10.0);
        std::vector<SionnaHeDlMuMimoSpectrumUserTransmission> users;
        for (std::size_t index = 0; index < staIds.size(); ++index)
        {
            users.push_back(
                {index == 0 ? 3u : 9u,
                 staIds[index],
                 apRuPowerW / std::pow(10.0, pathLossDb[index] / 10.0) *
                     rxGainLinear,
                 1,
                 psdus.at(staIds[index])->GetSize()});
        }
        const auto expected = adapter->CalculateHeDlMuMimoRx(
            snapshot,
            precoding,
            ppduTxVector,
            users,
            2,
            noiseFigureDb,
            CreateObject<TableBasedErrorRateModel>());

        for (std::size_t index = 0; index < staPhys.size(); ++index)
        {
            auto params = Create<WifiSpectrumSignalParameters>();
            params->duration = MicroSeconds(100);
            params->ppdu = ppdu;
            params->txPhy = apPhy->GetCurrentInterface();
            params->psd = Copy<SpectrumValue>(txPsd);
            *params->psd *= 1.0 / std::pow(10.0, pathLossDb[index] / 10.0);

            const auto rxPsd = adapter->CalcRxPowerSpectralDensity(
                params,
                apNode.Get(0)->GetObject<MobilityModel>(),
                staNodes.Get(index)->GetObject<MobilityModel>());
            const auto userRuBand =
                DynamicCast<HePhy>(staPhys[index]->GetPhyEntity(WIFI_MOD_CLASS_HE))
                    ->GetRuBandForRx(ppduTxVector, staIds[index]);
            const uint32_t userId = index == 0 ? 3 : 9;
            const double equivalentPower =
                expected.GetEquivalentReceivedPowerW(userId);
            NS_TEST_ASSERT_MSG_EQ_TOL(
                IntegrateSpectrumBand(*rxPsd, userRuBand.indices),
                equivalentPower / rxGainLinear,
                equivalentPower / rxGainLinear * 1e-10,
                "Runtime HE-MU PSD did not preserve the single downstream PHY Rx gain");
        }
        Simulator::Destroy();
    }
};

class HeDlMuMimoSpectrumChannelReceptionTest : public TestCase
{
  public:
    HeDlMuMimoSpectrumChannelReceptionTest()
        : TestCase("HE DL MU traverses SpectrumChannel and reaches addressed PHY payloads")
    {
    }

  private:
    void RxSuccess(std::size_t i, Ptr<const WifiPsdu>, RxSignalInfo info, WifiTxVector, std::vector<bool>)
    {
        ++m_success[i];
        m_successSnr[i].push_back(info.snr);
    }
    void RxFailure(std::size_t i, Ptr<const WifiPsdu>) { ++m_failure[i]; }
    void RxBegin(std::size_t i, Ptr<const Packet>, RxPowerWattPerChannelBand) { ++m_rxBegin[i]; }
    void PayloadBegin(std::size_t i, WifiTxVector, Time) { ++m_payloadBegin[i]; }

    void MimoRx(uint64_t, uint32_t, uint32_t rxNodeId, double sinr, double, uint32_t)
    {
        for (std::size_t i = 0; i < 2; ++i)
        {
            if (rxNodeId == m_scheduledNodeIds[i])
            {
                m_adapterSinr[i].push_back(sinr);
            }
        }
    }

    void SendCorrelated(Ptr<SionnaSpectrumPropagationLossModel> adapter,
                        MultiUserChannelSnapshot snapshot,
                        DlMuMimoPrecodingMatrix precoding,
                        Ptr<SpectrumWifiPhy> apPhy,
                        WifiConstPsduMap psdus,
                        WifiTxVector txVector)
    {
        adapter->SetDlMuMimoState(snapshot, precoding);
        apPhy->Send(psdus, txVector);
    }

    void CheckBystanderBusy(Ptr<SpectrumWifiPhy> phy)
    {
        ++m_bystanderBusyChecks;
        NS_TEST_ASSERT_MSG_EQ(phy->GetState()->GetState(),
                              WifiPhyState::CCA_BUSY,
                              "Unaddressed station did not retain MU payload energy as CCA");
    }

    void DoRun() override
    {
        constexpr double noiseFigureDb = 7.0;
        constexpr double txPowerDbm = -20.0;
        constexpr uint16_t width = 20;
        const std::array<uint16_t, 3> staIds{7, 11, 13};

        auto channel = CreateObject<MultiModelSpectrumChannel>();
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        adapter->SetReceiverNoiseFigureDb(noiseFigureDb);
        adapter->SetEesmBeta(11, 2.0);
        channel->AddSpectrumPropagationLossModel(adapter);

        NodeContainer apNode;
        NodeContainer staNodes;
        apNode.Create(1);
        staNodes.Create(3);
        apNode.Get(0)->AggregateObject(CreateObject<ConstantPositionMobilityModel>());
        for (uint32_t i = 0; i < staNodes.GetN(); ++i)
        {
            staNodes.Get(i)->AggregateObject(CreateObject<ConstantPositionMobilityModel>());
            staNodes.Get(i)->GetObject<MobilityModel>()->SetPosition({double(i + 1), 0.0, 0.0});
        }

        SpectrumWifiPhyHelper apPhyHelper;
        apPhyHelper.SetChannel(channel);
        ConfigureManualPhy(apPhyHelper, 2, 2, noiseFigureDb);
        apPhyHelper.Set("TxPowerStart", DoubleValue(txPowerDbm));
        apPhyHelper.Set("TxPowerEnd", DoubleValue(txPowerDbm));
        WifiHelper wifi;
        wifi.SetStandard(WIFI_STANDARD_80211ax);
        WifiMacHelper mac;
        mac.SetType("ns3::ApWifiMac", "BeaconGeneration", BooleanValue(false));
        auto apDevice = DynamicCast<WifiNetDevice>(wifi.Install(apPhyHelper, mac, apNode).Get(0));
        auto apPhy = DynamicCast<SpectrumWifiPhy>(apDevice->GetPhy());

        std::array<Ptr<WifiNetDevice>, 3> staDevices;
        std::array<Ptr<SpectrumWifiPhy>, 3> staPhys;
        mac.SetType("ns3::StaWifiMac", "ActiveProbing", BooleanValue(false));
        for (std::size_t i = 0; i < staIds.size(); ++i)
        {
            HeDlMuTestSpectrumWifiPhyHelper staPhyHelper(staIds[i]);
            staPhyHelper.SetChannel(channel);
            ConfigureManualPhy(staPhyHelper, 1, 1, noiseFigureDb);
            staPhyHelper.SetPreambleDetectionModel("ns3::ThresholdPreambleDetectionModel");
            staDevices[i] = DynamicCast<WifiNetDevice>(
                wifi.Install(staPhyHelper, mac, staNodes.Get(i)).Get(0));
            staPhys[i] = DynamicCast<SpectrumWifiPhy>(staDevices[i]->GetPhy());
            staPhys[i]->SetReceiveOkCallback(
                MakeCallback(&HeDlMuMimoSpectrumChannelReceptionTest::RxSuccess, this).Bind(i));
            staPhys[i]->SetReceiveErrorCallback(
                MakeCallback(&HeDlMuMimoSpectrumChannelReceptionTest::RxFailure, this).Bind(i));
            staPhys[i]->TraceConnectWithoutContext(
                "PhyRxBegin",
                MakeCallback(&HeDlMuMimoSpectrumChannelReceptionTest::RxBegin, this).Bind(i));
            staPhys[i]->TraceConnectWithoutContext(
                "PhyRxPayloadBegin",
                MakeCallback(&HeDlMuMimoSpectrumChannelReceptionTest::PayloadBegin, this).Bind(i));
        }
        apNode.Get(0)->GetObject<MobilityModel>()->SetPosition({0.0, 0.0, 0.0});
        for (uint32_t i = 0; i < staNodes.GetN(); ++i)
        {
            staNodes.Get(i)->GetObject<MobilityModel>()->SetPosition({double(i + 1), 0.0, 0.0});
        }

        const WifiPhy::ChannelTuple operatingChannel{36, width, WIFI_PHY_BAND_5GHZ, 0};
        apPhy->SetOperatingChannel(operatingChannel);
        for (const auto& phy : staPhys)
        {
            phy->SetOperatingChannel(operatingChannel);
        }

        const HeRu::RuSpec fullRu(HeRu::RU_242_TONE, 1, true);
        WifiTxVector txVector;
        txVector.SetPreambleType(WIFI_PREAMBLE_HE_MU);
        txVector.SetChannelWidth(width);
        txVector.SetGuardInterval(800);
        txVector.SetSigBMode(HePhy::GetVhtMcs(5));
        txVector.SetHeMuUserInfo(staIds[0], {fullRu, 11, 1});
        txVector.SetHeMuUserInfo(staIds[1], {fullRu, 11, 1});
        NS_TEST_ASSERT_MSG_EQ(txVector.IsValid(), true, "Reception test TXVECTOR is invalid");

        WifiConstPsduMap psdus;
        for (std::size_t i = 0; i < 2; ++i)
        {
            WifiMacHeader header;
            header.SetType(WIFI_MAC_QOSDATA);
            header.SetQosTid(0);
            header.SetAddr1(staDevices[i]->GetMac()->GetAddress());
            header.SetAddr2(apDevice->GetMac()->GetAddress());
            header.SetSequenceNumber(i + 1);
            psdus.emplace(staIds[i], Create<WifiPsdu>(Create<Packet>(1200), header));
            m_scheduledNodeIds[i] = staNodes.Get(i)->GetId();
        }

        const uint16_t center =
            apPhy->GetOperatingChannel().GetPrimaryChannelCenterFrequency(width);
        auto txPsd = WifiSpectrumValueHelper::CreateHeOfdmTxPowerSpectralDensity(
            center,
            width,
            std::pow(10.0, (txPowerDbm - 30.0) / 10.0),
            apPhy->GetGuardBandwidth(width));
        const uint32_t numSubcarriers = txPsd->GetValuesN() - 1;
        MimoChannelMatrix orthogonal3(numSubcarriers, 1, 2);
        MimoChannelMatrix orthogonal9(numSubcarriers, 1, 2);
        MimoChannelMatrix correlated3(numSubcarriers, 1, 2);
        MimoChannelMatrix correlated9(numSubcarriers, 1, 2);
        MimoChannelMatrix bystander(numSubcarriers, 1, 1);
        for (uint32_t sc = 0; sc < numSubcarriers; ++sc)
        {
            orthogonal3.Set(sc, 0, 0, {1.0, 0.0});
            orthogonal9.Set(sc, 0, 1, {1.0, 0.0});
            correlated3.Set(sc, 0, 0, {1.0, 0.0});
            correlated3.Set(sc, 0, 1, {1.0, 0.0});
            correlated9.Set(sc, 0, 0, {1.0, 0.0});
            correlated9.Set(sc, 0, 1, {1.0, 0.0});
            bystander.Set(sc, 0, 0, {1.0, 0.0});
        }
        auto orthogonal = MakeDlMuSnapshot({{3, orthogonal3}, {9, orthogonal9}});
        auto correlated = MakeDlMuSnapshot({{3, correlated3}, {9, correlated9}});
        for (std::size_t i = 0; i < 2; ++i)
        {
            orthogonal.users[i].txNodeId = apNode.Get(0)->GetId();
            orthogonal.users[i].rxNodeId = m_scheduledNodeIds[i];
            orthogonal.users[i].widebandLossDb = 0.0;
            correlated.users[i].txNodeId = apNode.Get(0)->GetId();
            correlated.users[i].rxNodeId = m_scheduledNodeIds[i];
            correlated.users[i].widebandLossDb = 0.0;
        }
        const auto precoding = MakeIdentityDlMuPrecoding(numSubcarriers);
        adapter->SetFixedChannelMatrix(bystander, 0.0);
        adapter->SetDlMuMimoState(orthogonal, precoding);
        adapter->TraceConnectWithoutContext(
            "MimoRx",
            MakeCallback(&HeDlMuMimoSpectrumChannelReceptionTest::MimoRx, this));

        const auto ruBand = DynamicCast<HePhy>(apPhy->GetPhyEntity(WIFI_MOD_CLASS_HE))
                                ->GetRuBandForTx(txVector, staIds[0]);
        const double ruPowerW = IntegrateSpectrumBand(*txPsd, ruBand.indices);
        const double noisePowerW =
            WifiLinkBudget::Calculate(0.0, 0.0,
                                      HeRu::GetBandwidth(HeRu::RU_242_TONE) * 1e6,
                                      290.0, noiseFigureDb).GetReceiverNoisePowerW();
        m_expectedOrthogonalSinr = ruPowerW / (2.0 * noisePowerW);
        m_expectedCorrelatedSinr = m_expectedOrthogonalSinr / (m_expectedOrthogonalSinr + 1.0);

        apPhy->AssignStreams(1);
        for (const auto& phy : staPhys) { phy->AssignStreams(1); }
        const Time firstStart = MilliSeconds(1);
        const Time ppduDuration = WifiPhy::CalculateTxDuration(psdus, txVector, WIFI_PHY_BAND_5GHZ);
        const Time secondStart = firstStart + ppduDuration + MilliSeconds(1);
        const Time payloadCheck = WifiPhy::CalculatePhyPreambleAndHeaderDuration(txVector) + NanoSeconds(1);
        Simulator::Schedule(firstStart, [apPhy, psdus, txVector]() { apPhy->Send(psdus, txVector); });
        Simulator::Schedule(firstStart + payloadCheck, &HeDlMuMimoSpectrumChannelReceptionTest::CheckBystanderBusy, this, staPhys[2]);
        Simulator::Schedule(secondStart, &HeDlMuMimoSpectrumChannelReceptionTest::SendCorrelated,
                            this, adapter, correlated, precoding, apPhy, psdus, txVector);
        Simulator::Schedule(secondStart + payloadCheck, &HeDlMuMimoSpectrumChannelReceptionTest::CheckBystanderBusy, this, staPhys[2]);
        Simulator::Stop(secondStart + ppduDuration + MicroSeconds(100));
        Simulator::Run();

        for (std::size_t i = 0; i < 2; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(m_success[i], 1, "Orthogonal MU payload was not decoded");
            NS_TEST_ASSERT_MSG_EQ(m_failure[i], 1, "Correlated MU payload did not fail");
            NS_TEST_ASSERT_MSG_EQ(m_rxBegin[i], 2, "Scheduled STA missed a common preamble");
            NS_TEST_ASSERT_MSG_EQ(m_payloadBegin[i], 4, "Scheduled STA missed an MU spectrum portion");
            NS_TEST_ASSERT_MSG_EQ(m_adapterSinr[i].size(), 2, "Adapter missed an MU payload");
            if (m_adapterSinr[i].size() == 2)
            {
                NS_TEST_ASSERT_MSG_EQ_TOL(m_adapterSinr[i][0], m_expectedOrthogonalSinr,
                                          m_expectedOrthogonalSinr * 1e-10,
                                          "Orthogonal SINR disagrees with the link budget");
                NS_TEST_ASSERT_MSG_EQ_TOL(m_adapterSinr[i][1], m_expectedCorrelatedSinr, 1e-10,
                                          "Correlated SINR disagrees with the IUI expectation");
            }
            NS_TEST_ASSERT_MSG_EQ(m_successSnr[i].size(), 1, "Unexpected PHY SINR reports");
            if (m_successSnr[i].size() == 1)
            {
                NS_TEST_ASSERT_MSG_EQ_TOL(m_successSnr[i][0], m_expectedOrthogonalSinr,
                                          m_expectedOrthogonalSinr * 1e-6,
                                          "Decoded PHY SINR disagrees with the link budget");
            }
        }
        NS_TEST_ASSERT_MSG_EQ(m_success[2], 0, "Bystander decoded an MU PSDU");
        NS_TEST_ASSERT_MSG_EQ(m_failure[2], 0, "Bystander received an addressed failure");
        NS_TEST_ASSERT_MSG_EQ(m_rxBegin[2], 0, "Bystander reported an addressed RX begin");
        NS_TEST_ASSERT_MSG_EQ(m_payloadBegin[2], 2, "Bystander did not track each MU payload as interference");
        NS_TEST_ASSERT_MSG_EQ(m_bystanderBusyChecks, 2, "Bystander CCA was not checked twice");
        Simulator::Destroy();
    }

    std::array<uint32_t, 3> m_success{};
    std::array<uint32_t, 3> m_failure{};
    std::array<uint32_t, 3> m_rxBegin{};
    std::array<uint32_t, 3> m_payloadBegin{};
    std::array<uint32_t, 2> m_scheduledNodeIds{};
    std::array<std::vector<double>, 2> m_adapterSinr;
    std::array<std::vector<double>, 2> m_successSnr;
    uint32_t m_bystanderBusyChecks{0};
    double m_expectedOrthogonalSinr{0.0};
    double m_expectedCorrelatedSinr{0.0};
};

class DlMuMimoZfRzfAnalyticalTest : public TestCase
{
  public:
    DlMuMimoZfRzfAnalyticalTest()
        : TestCase("downlink ZF and explicit RZF are deterministic and power normalized")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix orthogonalUser3(1, 1, 2);
        orthogonalUser3.Set(0, 0, 0, {1.0, 0.0});
        MimoChannelMatrix orthogonalUser9(1, 1, 2);
        orthogonalUser9.Set(0, 0, 1, {1.0, 0.0});
        const auto orthogonalSnapshot =
            MakeDlMuSnapshot({{3, orthogonalUser3}, {9, orthogonalUser9}});

        const auto zf = DlMuMimoPrecodingMatrix::ZeroForcing(
            orthogonalSnapshot,
            {{9, 0}, {3, 0}});
        const auto repeatedZf = DlMuMimoPrecodingMatrix::ZeroForcing(
            orthogonalSnapshot,
            {{3, 0}, {9, 0}});
        NS_TEST_ASSERT_MSG_EQ(zf.GetStreamMapping(0).userId,
                              3,
                              "ZF did not emit columns in stable user-ID order");
        NS_TEST_ASSERT_MSG_EQ(zf.GetStreamMapping(1).userId,
                              9,
                              "ZF stable user-ID ordering changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(zf.GetPrecodingMatrix().GetPower(0),
                                  2.0,
                                  1e-12,
                                  "ZF changed the Phase-4.2 total-power contract");
        for (std::size_t index = 0;
             index < zf.GetPrecodingMatrix().GetCoefficients().size();
             ++index)
        {
            NS_TEST_ASSERT_MSG_EQ(
                zf.GetPrecodingMatrix().GetCoefficients().at(index),
                repeatedZf.GetPrecodingMatrix().GetCoefficients().at(index),
                "ZF changed with request order or repeated equal-singular-value selection");
        }

        const auto orthogonalResult =
            DlMuMimoLinkModel::Calculate(orthogonalSnapshot,
                                         zf,
                                         {{9, 10.0}, {3, 10.0}});
        for (const uint32_t userId : {3u, 9u})
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(
                orthogonalResult.GetUserById(userId).GetInterUserInterferencePower(0, 0),
                0.0,
                1e-12,
                "ZF did not suppress orthogonal-user interference");
        }

        MimoChannelMatrix correlatedUser3(1, 1, 2);
        correlatedUser3.Set(0, 0, 0, {1.0, 0.0});
        correlatedUser3.Set(0, 0, 1, {0.5, 0.0});
        MimoChannelMatrix correlatedUser9(1, 1, 2);
        correlatedUser9.Set(0, 0, 0, {0.5, 0.0});
        correlatedUser9.Set(0, 0, 1, {1.0, 0.0});
        const auto correlatedSnapshot =
            MakeDlMuSnapshot({{3, correlatedUser3}, {9, correlatedUser9}});
        const auto correlatedZf = DlMuMimoPrecodingMatrix::ZeroForcing(
            correlatedSnapshot,
            {{3, 0}, {9, 0}});
        const auto correlatedResult =
            DlMuMimoLinkModel::Calculate(correlatedSnapshot,
                                         correlatedZf,
                                         {{3, 20.0}, {9, 20.0}});
        NS_TEST_ASSERT_MSG_EQ_TOL(correlatedZf.GetPrecodingMatrix().GetPower(0),
                                  2.0,
                                  1e-12,
                                  "Correlated-channel ZF changed total AP power");
        for (const uint32_t userId : {3u, 9u})
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(
                correlatedResult.GetUserById(userId).GetInterUserInterferencePower(0, 0),
                0.0,
                1e-12,
                "ZF did not suppress correlated-user interference");
        }

        MimoChannelMatrix poorUser3(1, 1, 2);
        poorUser3.Set(0, 0, 0, {1.0, 0.0});
        MimoChannelMatrix poorUser9(1, 1, 2);
        poorUser9.Set(0, 0, 0, {1.0, 0.0});
        poorUser9.Set(0, 0, 1, {1e-8, 0.0});
        const auto poorSnapshot = MakeDlMuSnapshot({{3, poorUser3}, {9, poorUser9}});
        const auto rzf = DlMuMimoPrecodingMatrix::RegularizedZeroForcing(
            poorSnapshot,
            {{9, 0}, {3, 0}},
            1e-4);
        const auto repeatedRzf = DlMuMimoPrecodingMatrix::RegularizedZeroForcing(
            poorSnapshot,
            {{3, 0}, {9, 0}},
            1e-4);
        NS_TEST_ASSERT_MSG_EQ_TOL(rzf.GetPrecodingMatrix().GetPower(0),
                                  2.0,
                                  1e-12,
                                  "RZF changed the Phase-4.2 total-power contract");
        for (std::size_t index = 0;
             index < rzf.GetPrecodingMatrix().GetCoefficients().size();
             ++index)
        {
            const auto coefficient = rzf.GetPrecodingMatrix().GetCoefficients().at(index);
            NS_TEST_ASSERT_MSG_EQ(std::isfinite(coefficient.real()) &&
                                      std::isfinite(coefficient.imag()),
                                  true,
                                  "RZF produced a non-finite coefficient");
            NS_TEST_ASSERT_MSG_EQ(
                coefficient,
                repeatedRzf.GetPrecodingMatrix().GetCoefficients().at(index),
                "RZF is not deterministic");
        }

        MimoChannelMatrix singleUserChannel(1, 1, 2);
        singleUserChannel.Set(0, 0, 0, {1.0, 1.0});
        singleUserChannel.Set(0, 0, 1, {2.0, -1.0});
        const auto singleUserSnapshot = MakeDlMuSnapshot({{3, singleUserChannel}});
        const auto singleUserZf =
            DlMuMimoPrecodingMatrix::ZeroForcing(singleUserSnapshot, {{3, 0}});
        const auto mrt = MimoPrecodingMatrix::MaximumRatioTransmission(singleUserChannel);
        for (uint32_t tx = 0; tx < 2; ++tx)
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(
                std::abs(singleUserZf.GetPrecodingMatrix().Get(0, tx, 0) -
                         mrt.Get(0, tx, 0)),
                0.0,
                1e-12,
                "Single-user ZF did not reduce to Phase-3 MRT");
        }
        const auto singleUserMu =
            DlMuMimoLinkModel::Calculate(singleUserSnapshot, singleUserZf, {{3, 6.0}});
        const auto singleUserSu = SuMimoLinkModel::Calculate(singleUserChannel, mrt, 6.0);
        NS_TEST_ASSERT_MSG_EQ_TOL(singleUserMu.GetUserById(3).GetSinrLinear(0, 0),
                                  singleUserSu.GetSinrLinear(0, 0),
                                  1e-10,
                                  "Single-user ZF did not preserve the Phase-4.2/SU boundary");

        MimoChannelMatrix sisoChannel(1, 1, 1);
        sisoChannel.Set(0, 0, 0, {2.0, 1.0});
        const auto sisoSnapshot = MakeDlMuSnapshot({{3, sisoChannel}});
        const auto sisoRzf = DlMuMimoPrecodingMatrix::RegularizedZeroForcing(
            sisoSnapshot,
            {{3, 0}},
            0.25);
        const auto sisoMu =
            DlMuMimoLinkModel::Calculate(sisoSnapshot, sisoRzf, {{3, 4.0}});
        NS_TEST_ASSERT_MSG_EQ_TOL(
            sisoMu.GetUserById(3).GetSinrLinear(0, 0),
            SuMimoLinkModel::Calculate(sisoChannel, 4.0).GetSinrLinear(0, 0),
            1e-10,
            "RZF did not preserve the normalized 1x1 result");
    }
};

class DlMuMimoZfRzfRejectionTest : public TestCase
{
  public:
    DlMuMimoZfRzfRejectionTest()
        : TestCase("downlink ZF and RZF reject unsupported groups atomically")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix user3Channel(1, 1, 3);
        user3Channel.Set(0, 0, 0, {1.0, 0.0});
        MimoChannelMatrix user9Channel(1, 1, 3);
        user9Channel.Set(0, 0, 1, {1.0, 0.0});
        const auto snapshot = MakeDlMuSnapshot({{3, user3Channel}, {9, user9Channel}});
        const auto before =
            DlMuMimoPrecodingMatrix::ZeroForcing(snapshot, {{3, 0}, {9, 0}});

        auto expectRejected = [this](const auto& operation, const std::string& reason) {
            bool rejected = false;
            try
            {
                operation();
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, reason);
        };

        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix::ZeroForcing(
                    snapshot,
                    {{3, 0}, {3, 1}, {9, 0}});
            },
            "Multiple streams per user were accepted");
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix::ZeroForcing(snapshot, {{3, 0}});
            },
            "A missing user stream was accepted");
        for (const double alpha : {0.0,
                                   -1.0,
                                   std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN()})
        {
            expectRejected(
                [&]() {
                    DlMuMimoPrecodingMatrix::RegularizedZeroForcing(
                        snapshot,
                        {{3, 0}, {9, 0}},
                        alpha);
                },
                "An invalid explicit RZF regularization parameter was accepted");
        }
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix::ZeroForcing(snapshot,
                                                     {{3, 0}, {9, 0}},
                                                     0.0);
            },
            "An invalid ZF rank tolerance was accepted");

        MimoChannelMatrix rankDeficient3(1, 1, 2);
        rankDeficient3.Set(0, 0, 0, {1.0, 0.0});
        rankDeficient3.Set(0, 0, 1, {1.0, 0.0});
        MimoChannelMatrix rankDeficient9 = rankDeficient3;
        const auto rankDeficient =
            MakeDlMuSnapshot({{3, rankDeficient3}, {9, rankDeficient9}});
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix::ZeroForcing(
                    rankDeficient,
                    {{3, 0}, {9, 0}});
            },
            "A rank-deficient group was accepted by ZF");
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix::RegularizedZeroForcing(
                    rankDeficient,
                    {{3, 0}, {9, 0}},
                    0.1);
            },
            "A rank-deficient group was accepted by RZF");

        MimoChannelMatrix overloadedChannel(1, 1, 2);
        overloadedChannel.Set(0, 0, 0, {1.0, 0.0});
        const auto overloaded =
            MakeDlMuSnapshot({{3, overloadedChannel},
                              {6, overloadedChannel},
                              {9, overloadedChannel}});
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix::ZeroForcing(
                    overloaded,
                    {{3, 0}, {6, 0}, {9, 0}});
            },
            "An overloaded user group was accepted");

        auto nonFinite = snapshot;
        nonFinite.users[1].channel.Set(
            0,
            0,
            1,
            {std::numeric_limits<double>::quiet_NaN(), 0.0});
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix::ZeroForcing(
                    nonFinite,
                    {{3, 0}, {9, 0}});
            },
            "A non-finite snapshot was accepted");

        auto inconsistent = snapshot;
        inconsistent.users[1].frequencies[0] = 7;
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix::ZeroForcing(
                    inconsistent,
                    {{3, 0}, {9, 0}});
            },
            "An inconsistent Phase-4.1 snapshot was accepted");

        MimoChannelMatrix multipleRx(1, 2, 3);
        multipleRx.Set(0, 0, 0, {1.0, 0.0});
        multipleRx.Set(0, 1, 1, {1.0, 0.0});
        const auto unsupportedReceiver = MakeDlMuSnapshot({{3, multipleRx}});
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix::ZeroForcing(
                    unsupportedReceiver,
                    {{3, 0}});
            },
            "A multi-antenna receiver was accepted without a joint transceiver contract");

        MimoChannelMatrix poorUser3(1, 1, 2);
        poorUser3.Set(0, 0, 0, {1.0, 0.0});
        MimoChannelMatrix poorUser9(1, 1, 2);
        poorUser9.Set(0, 0, 0, {1.0, 0.0});
        poorUser9.Set(0, 0, 1, {1e-8, 0.0});
        const auto poorSnapshot = MakeDlMuSnapshot({{3, poorUser3}, {9, poorUser9}});
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix::ZeroForcing(
                    poorSnapshot,
                    {{3, 0}, {9, 0}},
                    1e-4);
            },
            "The explicit numerical-rank threshold was ignored");

        const auto after =
            DlMuMimoPrecodingMatrix::ZeroForcing(snapshot, {{9, 0}, {3, 0}});
        const bool stateUnchanged = before.GetPrecodingMatrix().GetCoefficients() ==
                                    after.GetPrecodingMatrix().GetCoefficients();
        NS_TEST_ASSERT_MSG_EQ(stateUnchanged,
                              true,
                              "Rejected selections changed subsequent deterministic state");
    }
};

class DlMuMimoRejectionTest : public TestCase
{
  public:
    DlMuMimoRejectionTest()
        : TestCase("inconsistent fixed downlink MU inputs are rejected before calculation")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix user3Channel(1, 1, 2);
        user3Channel.Set(0, 0, 0, {1.0, 0.0});
        MimoChannelMatrix user9Channel(1, 1, 2);
        user9Channel.Set(0, 0, 1, {1.0, 0.0});
        const auto snapshot = MakeDlMuSnapshot({{3, user3Channel}, {9, user9Channel}});
        const auto identityMatrix = MimoPrecodingMatrix::FrequencyFlat(
            1,
            2,
            2,
            {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}});
        const auto validPrecoding = MakeIdentityDlMuPrecoding(1);

        auto expectRejected = [this](const auto& operation, const std::string& reason) {
            bool rejected = false;
            try
            {
                operation();
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, reason);
        };

        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix duplicate(identityMatrix, {{3, 0}, {3, 0}});
            },
            "Duplicate user/stream mapping was accepted");
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix skipped(identityMatrix, {{3, 1}, {9, 0}});
            },
            "Non-contiguous user-local stream mapping was accepted");
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix missing(
                    MimoPrecodingMatrix::FrequencyFlat(1, 2, 1, {{1.0, 0.0}, {0.0, 0.0}}),
                    {{3, 0}});
                missing.ValidateForSnapshot(snapshot);
            },
            "A snapshot user without a mapped stream was accepted");
        expectRejected(
            [&]() {
                DlMuMimoPrecodingMatrix absent(identityMatrix, {{3, 0}, {12, 0}});
                absent.ValidateForSnapshot(snapshot);
            },
            "A stream mapped to an absent user was accepted");
        expectRejected(
            [&]() {
                auto inconsistent = snapshot;
                inconsistent.direction = MultiUserChannelDirection::UPLINK;
                validPrecoding.ValidateForSnapshot(inconsistent);
            },
            "An uplink snapshot was accepted by the downlink model");
        expectRejected(
            [&]() {
                auto inconsistent = snapshot;
                inconsistent.users[1].frequencies[0] = 7;
                validPrecoding.ValidateForSnapshot(inconsistent);
            },
            "An inconsistent frequency grid was accepted");
        expectRejected(
            [&]() {
                DlMuMimoLinkModel::Calculate(snapshot, validPrecoding, {{3, 10.0}});
            },
            "A missing per-user SNR was accepted");
        expectRejected(
            [&]() {
                DlMuMimoLinkModel::Calculate(
                    snapshot,
                    validPrecoding,
                    {{3, 10.0}, {9, std::numeric_limits<double>::quiet_NaN()}});
            },
            "A non-finite per-user SNR was accepted");

        auto he = CreateObject<HeDlMuMimoLinkAbstraction>();
        he->SetEesmBeta(0, 2.0);
        const auto budget = MakeReferenceSnrBudget(10.0);
        const auto tx = MakeHeTxVector(HePhy::GetHeMcs0(), 1);
        expectRejected(
            [&]() {
                he->Calculate(snapshot, validPrecoding, {{3, budget, tx, 400}});
            },
            "Missing per-user HE inputs were accepted");
        expectRejected(
            [&]() {
                he->Calculate(
                    snapshot,
                    validPrecoding,
                    {{3, budget, MakeHeTxVector(HePhy::GetHeMcs0(), 2), 400},
                     {9, budget, tx, 400}});
            },
            "A per-user NSS mismatch was accepted");
        expectRejected(
            [&]() {
                he->Calculate(snapshot,
                              validPrecoding,
                              {{3, budget, tx, 0}, {9, budget, tx, 400}});
            },
            "A zero packet size was accepted");

        const auto afterRejections = DlMuMimoLinkModel::Calculate(
            snapshot,
            validPrecoding,
            {{3, 10.0}, {9, 10.0}});
        NS_TEST_ASSERT_MSG_EQ_TOL(afterRejections.GetUserById(3).GetSinrLinear(0, 0),
                                  5.0,
                                  1e-10,
                                  "Rejected inputs changed the reusable fixed MU contract");
    }
};

class SuMimoChannelRankTest : public TestCase
{
  public:
    SuMimoChannelRankTest()
        : TestCase("SU-MIMO reports full-rank and rank-one channels")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix fullRank(2, 2, 2);
        MimoChannelMatrix rankOne(2, 2, 2);
        for (uint32_t sc = 0; sc < 2; ++sc)
        {
            fullRank.Set(sc, 0, 0, {std::sqrt(2.0), 0.0});
            fullRank.Set(sc, 1, 1, {std::sqrt(2.0), 0.0});
            for (uint32_t rx = 0; rx < 2; ++rx)
            {
                for (uint32_t tx = 0; tx < 2; ++tx)
                {
                    rankOne.Set(sc, rx, tx, {1.0, 0.0});
                }
            }
        }

        NS_TEST_ASSERT_MSG_EQ(SuMimoLinkModel::Calculate(fullRank, 10.0)
                                  .GetMinimumChannelRank(),
                              2,
                              "Well-conditioned 2x2 channel did not report rank two");
        NS_TEST_ASSERT_MSG_EQ(SuMimoLinkModel::Calculate(rankOne, 10.0)
                                  .GetMinimumChannelRank(),
                              1,
                              "Rank-one 2x2 channel did not report rank one");
    }
};

class SpectrumWifiPhyMimoAdapterTest : public TestCase
{
  public:
    SpectrumWifiPhyMimoAdapterTest()
        : TestCase("SpectrumWifiPhy MIMO adapter preserves effective SINR without double gains")
    {
    }

  private:
    void DoRun() override
    {
        constexpr double bandwidthHz = 20e6;
        constexpr double noiseFigureDb = 7.0;
        constexpr double referenceSnr = 10.0;
        const auto noiseReference =
            WifiLinkBudget::Calculate(0.0, 0.0, bandwidthHz, 290.0, noiseFigureDb);
        const double noisePowerW = noiseReference.GetReceiverNoisePowerW();
        const double receivedPowerW = referenceSnr * noisePowerW;
        auto errorModel = CreateObject<TableBasedErrorRateModel>();
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        // Formula sentinel for this flat-channel test, not an HE calibration.
        adapter->SetEesmBeta(0, 2.0);

        MimoChannelMatrix channel(4, 2, 2);
        for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
        {
            channel.Set(sc, 0, 0, {std::sqrt(2.0), 0.0});
            channel.Set(sc, 1, 1, {std::sqrt(2.0), 0.0});
        }
        const auto result = adapter->CalculateMimoRx(channel,
                                                     receivedPowerW,
                                                     MakeHeTxVector(HePhy::GetHeMcs0(), 2),
                                                     1000,
                                                     2,
                                                     2,
                                                     bandwidthHz,
                                                     noiseFigureDb,
                                                     errorModel);
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetLinkResult().GetEffectiveSinrLinear(),
                                  referenceSnr,
                                  1e-12,
                                  "Well-conditioned 2x2 effective SINR is incorrect");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetEquivalentReceivedPowerW(),
                                  referenceSnr * noisePowerW,
                                  referenceSnr * noisePowerW * 1e-12,
                                  "Equivalent SpectrumWifiPhy power applies noise twice");

        const auto identity = MimoPrecodingMatrix::FrequencyFlat(
            channel.GetNumSubcarriers(),
            2,
            2,
            {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}});
        adapter->SetPrecodingMatrix(identity);
        const auto identityResult = adapter->CalculateMimoRx(
            channel,
            receivedPowerW,
            MakeHeTxVector(HePhy::GetHeMcs0(), 2),
            1000,
            2,
            2,
            bandwidthHz,
            noiseFigureDb,
            errorModel);
        NS_TEST_ASSERT_MSG_EQ_TOL(identityResult.GetLinkResult().GetEffectiveSinrLinear(),
                                  result.GetLinkResult().GetEffectiveSinrLinear(),
                                  1e-12,
                                  "Adapter identity precoding did not reproduce Phase 2");
        NS_TEST_ASSERT_MSG_EQ_TOL(identityResult.GetLinkResult().GetPacketErrorRate(),
                                  result.GetLinkResult().GetPacketErrorRate(),
                                  1e-12,
                                  "Adapter identity precoding changed PER");

        const auto selectedPrecoding = MimoPrecodingMatrix::FrequencyFlat(
            channel.GetNumSubcarriers(),
            2,
            1,
            {{0.0, 0.0}, {0.0, 1.0}});
        adapter->SetPrecodingMatrix(selectedPrecoding);
        const auto selectedResult = adapter->CalculateMimoRx(
            channel,
            receivedPowerW,
            MakeHeTxVector(HePhy::GetHeMcs0(), 1),
            1000,
            2,
            2,
            bandwidthHz,
            noiseFigureDb,
            errorModel);
        NS_TEST_ASSERT_MSG_EQ(selectedResult.GetLinkResult().GetMimoResult().GetNumStreams(),
                              1,
                              "Adapter did not couple WifiTxVector NSS to W columns");
        NS_TEST_ASSERT_MSG_EQ_TOL(selectedResult.GetLinkResult().GetEffectiveSinrLinear(),
                                  2.0 * referenceSnr,
                                  1e-12,
                                  "Adapter selected-stream SINR is incorrect");
        NS_TEST_ASSERT_MSG_EQ_TOL(selectedResult.GetEquivalentReceivedPowerW(),
                                  referenceSnr * noisePowerW,
                                  referenceSnr * noisePowerW * 1e-12,
                                  "Selected-stream AWGN diversity compensation is wrong");

        adapter->ClearPrecodingMatrix();
        NS_TEST_ASSERT_MSG_EQ(adapter->HasPrecodingMatrix(),
                              false,
                              "Adapter did not restore the no-precoding path");

        MimoChannelMatrix extraRxChannel(1, 3, 2);
        extraRxChannel.Set(0, 0, 0, {std::sqrt(3.0), 0.0});
        extraRxChannel.Set(0, 1, 1, {std::sqrt(3.0), 0.0});
        const auto extraRx = adapter->CalculateMimoRx(extraRxChannel,
                                                      receivedPowerW,
                                                      MakeHeTxVector(HePhy::GetHeMcs0(), 2),
                                                      1000,
                                                      2,
                                                      3,
                                                      bandwidthHz,
                                                      noiseFigureDb,
                                                      errorModel);
        NS_TEST_ASSERT_MSG_EQ_TOL(extraRx.GetLinkResult().GetEffectiveSinrLinear(),
                                  15.0,
                                  1e-12,
                                  "MMSE did not include the third-antenna array gain");
        NS_TEST_ASSERT_MSG_EQ_TOL(extraRx.GetEquivalentReceivedPowerW(),
                                  10.0 * noisePowerW,
                                  10.0 * noisePowerW * 1e-12,
                                  "AWGN diversity gain was not compensated exactly once");

        MimoChannelMatrix sisoChannel(1, 1, 1);
        sisoChannel.Set(0, 0, 0, {1.0, 0.0});
        adapter->SetPrecodingMatrix(MimoPrecodingMatrix::FrequencyFlat(
            1,
            1,
            1,
            {{0.0, 1.0}}));
        const auto siso = adapter->CalculateMimoRx(sisoChannel,
                                                   receivedPowerW,
                                                   MakeHeTxVector(HePhy::GetHeMcs0(), 1),
                                                   1000,
                                                   1,
                                                   1,
                                                   bandwidthHz,
                                                   noiseFigureDb,
                                                   errorModel);
        NS_TEST_ASSERT_MSG_EQ_TOL(siso.GetLinkResult().GetEffectiveSinrLinear(),
                                  referenceSnr,
                                  1e-12,
                                  "Adapter SISO fixed precoding changed unit-channel SNR");
    }
};

// Exercise the production REQ/REP path with distinct, overlapping valid replies.
// All data is constructed in memory; no recorded fixture or RT process is needed.
class DisabledCacheFreshReplyTest : public TestCase
{
  public:
    DisabledCacheFreshReplyTest()
        : TestCase("disabled cache returns every fresh reply and retains no snapshots")
    {
    }

  private:
    void DoRun() override
    {
        zmq::context_t context(1);
        zmq::socket_t peer(context, ZMQ_REP);
        peer.set(zmq::sockopt::linger, 0);
        peer.set(zmq::sockopt::rcvtimeo, 3000);
        peer.set(zmq::sockopt::sndtimeo, 3000);
        peer.bind("tcp://127.0.0.1:*");
        const auto endpoint = peer.get(zmq::sockopt::last_endpoint);
        auto tx = CreateObject<Node>();
        auto rx = CreateObject<Node>();
        auto a = CreateObject<SionnaMobilityModel>();
        auto b = CreateObject<SionnaMobilityModel>();
        tx->AggregateObject(a);
        rx->AggregateObject(b);
        auto replies = std::async(std::launch::async, [&]() {
            unsigned requests = 0;
            // Prime enabled cache, three disabled getters, then re-enable.
            for (unsigned value = 1; value <= 5; ++value)
            {
                zmq::message_t request;
                if (!peer.recv(request, zmq::recv_flags::none))
                {
                    break; // Bounded even when a regression suppresses an RPC.
                }
                ++requests;
                ns3sionna::Wrapper reply;
                auto state = reply.mutable_channel_state_response()->add_csi();
                state->set_start_time(0);
                state->set_end_time(5000000);
                state->mutable_tx_node()->set_id(tx->GetId());
                auto radio = state->add_rx_nodes();
                radio->set_id(rx->GetId());
                radio->set_delay(value);
                radio->set_wb_loss(40 + value);
                radio->set_num_rx_antennas(1);
                radio->set_num_tx_antennas(1);
                radio->add_frequencies(78125);
                radio->add_csi_real(value);
                radio->add_csi_imag(-static_cast<double>(value));
                const auto wire = reply.SerializeAsString();
                peer.send(zmq::buffer(wire), zmq::send_flags::none);
            }
            return requests;
        });
        SionnaHelper helper("in-memory-cache-regression", endpoint);
        helper.m_zmq_socket.set(zmq::sockopt::linger, 0);
        helper.m_zmq_socket.set(zmq::sockopt::rcvtimeo, 3000);
        auto cache = CreateObject<SionnaPropagationCache>();
        cache->SetSionnaHelper(helper);
        cache->SetOptimize(false);
        const auto primingLoss = cache->GetPropagationLoss(a, b);
        NS_TEST_ASSERT_MSG_EQ(primingLoss, 41, "Priming reply changed");
        cache->SetCaching(false);
        const auto freshLoss = cache->GetPropagationLoss(a, b);
        NS_TEST_ASSERT_MSG_EQ(freshLoss, 42, "Old valid loss reused");
        const auto freshDelay = cache->GetPropagationDelay(a, b);
        NS_TEST_ASSERT_MSG_EQ(freshDelay.GetNanoSeconds(), 3, "Old valid delay reused");
        const auto channel = cache->GetPropagationChannelMatrix(a, b);
        NS_TEST_ASSERT_MSG_EQ(channel.Get(0, 0, 0), std::complex<double>(4, -4),
                              "Old valid CFR reused");
        cache->SetCaching(true);
        const auto enabledLoss = cache->GetPropagationLoss(a, b);
        NS_TEST_ASSERT_MSG_EQ(enabledLoss, 45, "Disabled requests retained a snapshot");
        const auto hitLoss = cache->GetPropagationLoss(a, b);
        NS_TEST_ASSERT_MSG_EQ(hitLoss, 45, "Re-enabled cache no longer hits");
        const auto requests = replies.get();
        NS_TEST_ASSERT_MSG_EQ(requests, 5, "Unexpected RPC count");
        Simulator::Destroy();
    }
};


// A bounded production REQ/REP peer; every response is generated in memory.
class CachePeer
{
  public:
    explicit CachePeer(std::vector<std::string> replies)
        : m_context(1), m_socket(m_context, ZMQ_REP)
    {
        m_socket.set(zmq::sockopt::linger, 0);
        m_socket.set(zmq::sockopt::rcvtimeo, 3000);
        m_socket.bind("tcp://127.0.0.1:*");
        endpoint = m_socket.get(zmq::sockopt::last_endpoint);
        m_requests = std::async(std::launch::async, [this, replies]() {
            std::vector<ns3sionna::ChannelStateRequest> requests;
            for (const auto& reply : replies)
            {
                zmq::message_t wire;
                if (!m_socket.recv(wire, zmq::recv_flags::none))
                {
                    break;
                }
                ns3sionna::Wrapper request;
                if (!request.ParseFromArray(wire.data(), wire.size()) ||
                    !request.has_channel_state_request())
                {
                    throw std::runtime_error("Invalid cache request");
                }
                requests.push_back(request.channel_state_request());
                m_socket.send(zmq::buffer(reply), zmq::send_flags::none);
            }
            return requests;
        });
    }

    std::vector<ns3sionna::ChannelStateRequest> Finish() { return m_requests.get(); }
    std::string endpoint;

  private:
    zmq::context_t m_context;
    zmq::socket_t m_socket;
    std::future<std::vector<ns3sionna::ChannelStateRequest>> m_requests;
};

Ptr<SionnaMobilityModel>
MakeCacheRadio()
{
    auto node = CreateObject<Node>();
    auto radio = CreateObject<SionnaMobilityModel>();
    node->AggregateObject(radio);
    return radio;
}

uint32_t
CacheRadioId(Ptr<SionnaMobilityModel> radio)
{
    return radio->GetObject<Node>()->GetId();
}

void
AddCacheState(ns3sionna::Wrapper& reply, Ptr<SionnaMobilityModel> tx,
              Ptr<SionnaMobilityModel> rx, uint64_t start, uint64_t end,
              double loss, bool directed = false)
{
    auto* state = reply.mutable_channel_state_response()->add_csi();
    state->set_start_time(start);
    state->set_end_time(end);
    state->mutable_tx_node()->set_id(CacheRadioId(tx));
    auto* user = state->add_rx_nodes();
    user->set_id(CacheRadioId(rx));
    user->set_wb_loss(loss);
    user->set_delay(1);
    user->add_frequencies(78125);
    user->add_csi_real(1);
    user->add_csi_imag(0);
    user->set_num_rx_antennas(1);
    user->set_num_tx_antennas(1);
    if (directed)
    {
        auto* identity = user->mutable_user_identity();
        identity->set_user_id(CacheRadioId(rx));
        identity->set_tx_node_id(CacheRadioId(tx));
        identity->set_rx_node_id(CacheRadioId(rx));
        identity->set_csi_timestamp(start);
        identity->set_direction(ns3sionna::CHANNEL_DIRECTION_DOWNLINK);
        auto* array = identity->mutable_rx_array();
        array->set_num_rows(1);
        array->set_num_cols(1);
        array->set_vertical_spacing(0.5);
        array->set_horizontal_spacing(0.5);
        array->set_pattern("dipole");
        array->set_polarization("V");
    }
}

class CacheContractTest : public TestCase
{
  public:
    explicit CacheContractTest(unsigned scenario)
        : TestCase(scenario == 0 ? "cache validity, overlaps, inactive expiry and storage peaks" :
                   scenario == 1 ? "cache directed MU endpoints and legacy reciprocity" :
                                   "cache rejects malformed batches atomically"),
          m_scenario(scenario)
    {
    }

  private:
    void CheckStorage(Ptr<SionnaPropagationCache> cache, uint64_t links, uint64_t snapshots,
                      uint64_t peakSnapshots)
    {
        const auto stats = cache->GetStorageStatistics();
        const uint64_t bytes = sizeof(std::complex<double>) + sizeof(int);
        NS_TEST_ASSERT_MSG_EQ(stats.links, links, "Wrong retained links");
        NS_TEST_ASSERT_MSG_EQ(stats.snapshots, snapshots, "Wrong retained snapshots");
        NS_TEST_ASSERT_MSG_EQ(stats.payloadBytes, snapshots * bytes, "Wrong live payload");
        NS_TEST_ASSERT_MSG_EQ(stats.peakSnapshots, peakSnapshots, "Wrong lifetime peak");
        NS_TEST_ASSERT_MSG_EQ(stats.peakPayloadBytes, peakSnapshots * bytes, "Wrong peak payload");
        NS_TEST_ASSERT_MSG_GT_OR_EQ(stats.capacityBytes, stats.payloadBytes, "Capacity undercounts");
        NS_TEST_ASSERT_MSG_GT_OR_EQ(stats.peakCapacityBytes, stats.capacityBytes,
                                   "Capacity peak undercounts");
        if (snapshots == 0)
        {
            NS_TEST_ASSERT_MSG_EQ(stats.capacityBytes, 0, "Empty buckets retain capacity");
        }
    }

    void DoRun() override
    {
        auto a = MakeCacheRadio();
        auto b = MakeCacheRadio();
        auto c = MakeCacheRadio();
        auto d = MakeCacheRadio();
        auto e = MakeCacheRadio();
        auto f = MakeCacheRadio();
        std::vector<std::string> replies;
        auto single = [&](auto tx, auto rx, uint64_t start, uint64_t end, double loss,
                          bool directed = false) {
            ns3sionna::Wrapper reply;
            AddCacheState(reply, tx, rx, start, end, loss, directed);
            return reply.SerializeAsString();
        };
        if (m_scenario == 0)
        {
            ns3sionna::Wrapper first;
            AddCacheState(first, a, b, 0, 10, 10);
            AddCacheState(first, c, d, 0, 10, 99); // Never queried: global cleanup must remove it.
            AddCacheState(first, a, b, 20, 30, 50);
            AddCacheState(first, a, b, 20, 40, 60); // First matching overlap wins.
            replies = {first.SerializeAsString(), single(e, f, 11, 11, 55),
                       single(a, b, 19, 19, 70), single(a, b, 41, 41, 80)};
        }
        else if (m_scenario == 1)
        {
            replies = {single(a, b, 0, 100, 1, true), single(b, a, 0, 100, 2, true),
                       single(c, d, 0, 100, 3)};
        }
        else
        {
            replies.push_back(single(a, b, 0, 100, 10));
            for (unsigned malformed = 0; malformed < 13; ++malformed)
            {
                ns3sionna::Wrapper reply;
                AddCacheState(reply, c, d, 0, 100, 20); // Valid prefix must never leak.
                AddCacheState(reply, e, f, 0, 100, 30);
                auto* state = reply.mutable_channel_state_response()->mutable_csi(1);
                auto* user = state->mutable_rx_nodes(0);
                switch (malformed)
                {
                case 0: user->clear_csi_imag(); break;
                case 1: state->set_start_time(101); break;
                case 2: state->set_end_time(std::numeric_limits<uint64_t>::max()); break;
                case 3: user->set_wb_loss(std::numeric_limits<double>::quiet_NaN()); break;
                case 4: user->set_csi_real(0, std::numeric_limits<double>::quiet_NaN()); break;
                case 5: *state->add_rx_nodes() = *user; break;
                case 6: state->clear_rx_nodes(); break;
                case 7: reply.mutable_channel_state_response()->mutable_csi(0)->
                            mutable_rx_nodes(0)->set_id(CacheRadioId(f)); break;
                case 8: user->set_num_rx_antennas(2); break;
                case 9: user->set_delay(std::numeric_limits<uint64_t>::max()); break;
                case 10: replies.push_back(std::string(1, '\xff')); continue;
                case 11: replies.push_back(ns3sionna::Wrapper().SerializeAsString()); continue;
                case 12:
                    user->set_num_rx_antennas(std::numeric_limits<uint32_t>::max());
                    user->set_num_tx_antennas(std::numeric_limits<uint32_t>::max());
                    user->add_frequencies(156250);
                    break;
                }
                replies.push_back(reply.SerializeAsString());
            }
            replies.push_back(single(c, d, 0, 100, 42));
        }
        CachePeer peer(replies);
        SionnaHelper helper("in-memory-cache-contract", peer.endpoint);
        helper.m_zmq_socket.set(zmq::sockopt::linger, 0);
        helper.m_zmq_socket.set(zmq::sockopt::rcvtimeo, 3000);
        auto cache = CreateObject<SionnaPropagationCache>();
        cache->SetSionnaHelper(helper);
        cache->SetOptimize(false);
        const auto emptyRatio = cache->GetStats();
        NS_TEST_ASSERT_MSG_EQ(emptyRatio, 0, "Zero lookups must produce a finite zero ratio");
        CheckStorage(cache, 0, 0, 0);
        auto loss = [&](auto tx, auto rx, double expected) {
            const double actual = cache->GetPropagationLoss(tx, rx);
            NS_TEST_ASSERT_MSG_EQ(actual, expected, "Wrong snapshot selected");
        };
        if (m_scenario == 0)
        {
            loss(a, b, 10);
            CheckStorage(cache, 2, 4, 4);
            Simulator::Schedule(NanoSeconds(10), [&]() { loss(a, b, 10); });
            Simulator::Schedule(NanoSeconds(11), [&]() {
                loss(e, f, 55);
                CheckStorage(cache, 2, 3, 4);
            });
            Simulator::Schedule(NanoSeconds(12), [&]() { CheckStorage(cache, 1, 2, 4); });
            Simulator::Schedule(NanoSeconds(19), [&]() { loss(a, b, 70); });
            Simulator::Schedule(NanoSeconds(20), [&]() { loss(a, b, 50); });
            Simulator::Schedule(NanoSeconds(30), [&]() { loss(a, b, 50); });
            Simulator::Schedule(NanoSeconds(31), [&]() { loss(a, b, 60); });
            Simulator::Schedule(NanoSeconds(40), [&]() { loss(a, b, 60); });
            Simulator::Schedule(NanoSeconds(41), [&]() { loss(a, b, 80); });
            Simulator::Schedule(NanoSeconds(42), [&]() { CheckStorage(cache, 0, 0, 4); });
            Simulator::Run();
            const auto ratio = cache->GetStats();
            NS_TEST_ASSERT_MSG_EQ_TOL(ratio, 5.0 / 9, 1e-12, "Stat reads counted as lookups");
        }
        else if (m_scenario == 1)
        {
            loss(a, b, 1);
            loss(b, a, 2);
            loss(a, b, 1);
            loss(c, d, 3);
            loss(d, c, 3);
            CheckStorage(cache, 2, 3, 3);
        }
        else
        {
            loss(a, b, 10);
            const auto original = cache->GetStorageStatistics();
            for (unsigned malformed = 0; malformed < 13; ++malformed)
            {
                bool rejected = false;
                try { cache->GetPropagationLoss(c, d); }
                catch (const std::invalid_argument&) { rejected = true; }
                NS_TEST_ASSERT_MSG_EQ(rejected, true, "Malformed response accepted: " << malformed);
                loss(a, b, 10);
                CheckStorage(cache, 1, 1, 1);
                const auto stats = cache->GetStorageStatistics();
                NS_TEST_ASSERT_MSG_EQ(stats.capacityBytes, original.capacityBytes,
                                      "Rejected batch changed live capacity");
                NS_TEST_ASSERT_MSG_EQ(stats.peakCapacityBytes, original.peakCapacityBytes,
                                      "Rejected batch changed peak capacity");
            }
            loss(c, d, 42);
            CheckStorage(cache, 2, 2, 2);
        }
        const auto requests = peer.Finish();
        NS_TEST_ASSERT_MSG_EQ(requests.size(), replies.size(), "Unexpected RPC count");
        const auto peak = cache->GetStorageStatistics();
        cache->SetCaching(false);
        CheckStorage(cache, 0, 0, peak.peakSnapshots);
        const auto cleared = cache->GetStorageStatistics();
        NS_TEST_ASSERT_MSG_EQ(cleared.peakLinks, peak.peakLinks, "Disable cleared lifetime peaks");
        NS_TEST_ASSERT_MSG_EQ(cleared.peakCapacityBytes, peak.peakCapacityBytes,
                              "Disable cleared capacity peak");
        Simulator::Destroy();
    }

    unsigned m_scenario;
};

// Public getters own their results even when the underlying snapshot is moved,
// expired, or discarded. Distinct MU channels also catch positional remapping.
class CacheGetterOwnershipTest : public TestCase
{
  public:
    CacheGetterOwnershipTest()
        : TestCase("cache getter ownership, MU identity mapping and atomic rejection")
    {
    }

  private:
    void DoRun() override
    {
        using Coefficients = std::vector<std::complex<double>>;
        const Coefficients bCoefficients{{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
        const Coefficients cCoefficients{{0, -1}, {-1, 0}, {0, 1}, {1, 0}};

        auto movable = bCoefficients;
        const auto* allocation = movable.data();
        MimoChannelMatrix moved(2, 1, 2, std::move(movable));
        NS_TEST_ASSERT_MSG_EQ(moved.GetCoefficients().data(), allocation,
                              "Rvalue construction copied the CFR allocation");
        auto source = cCoefficients;
        MimoChannelMatrix copied(2, 1, 2, source);
        source.at(0) = {7, 8};
        NS_TEST_ASSERT_MSG_EQ(copied.Get(0, 0, 0), cCoefficients.at(0),
                              "Lvalue construction shares caller-owned coefficients");

        auto a = MakeCacheRadio(), b = MakeCacheRadio(), c = MakeCacheRadio();
        auto d = MakeCacheRadio(), e = MakeCacheRadio(), f = MakeCacheRadio();
        auto addUser = [&](ChannelStateMessage& state, auto tx, auto rx,
                           uint64_t start, uint64_t end, double loss, uint64_t delay,
                           const Coefficients& coefficients) {
            ns3sionna::Wrapper temporary;
            AddCacheState(temporary, tx, rx, start, end, loss, true);
            auto* user = state.add_rx_nodes();
            *user = temporary.channel_state_response().csi(0).rx_nodes(0);
            state.set_start_time(start);
            state.set_end_time(end);
            state.mutable_tx_node()->set_id(CacheRadioId(tx));
            user->set_delay(delay);
            user->set_num_tx_antennas(2);
            user->add_frequencies(156250);
            user->clear_csi_real();
            user->clear_csi_imag();
            for (const auto& coefficient : coefficients)
            {
                user->add_csi_real(coefficient.real());
                user->add_csi_imag(coefficient.imag());
            }
        };
        ns3sionna::Wrapper initial;
        auto* initialState = initial.mutable_channel_state_response()->add_csi();
        // Wire order differs from the stable order of decoded MU user IDs.
        addUser(*initialState, a, c, 0, 10, 20, 7, cCoefficients);
        addUser(*initialState, a, b, 0, 10, 10, 3, bCoefficients);

        ns3sionna::Wrapper malformed;
        auto* validPrefix = malformed.mutable_channel_state_response()->add_csi();
        addUser(*validPrefix, d, f, 0, 100, 50, 9, cCoefficients);
        addUser(*validPrefix, d, e, 0, 100, 40, 8, bCoefficients);
        auto* invalidSuffix = malformed.mutable_channel_state_response()->add_csi();
        addUser(*invalidSuffix, a, c, 0, 100, 90, 7, bCoefficients);
        addUser(*invalidSuffix, a, b, 0, 100, 80, 3, cCoefficients);
        invalidSuffix->mutable_rx_nodes(1)->clear_csi_imag();

        auto freshReply = [&](double loss, const Coefficients& coefficients) {
            ns3sionna::Wrapper reply;
            addUser(*reply.mutable_channel_state_response()->add_csi(),
                    a, b, 11, 100, loss, 5, coefficients);
            return reply.SerializeAsString();
        };
        const std::vector<std::string> replies{initial.SerializeAsString(),
                                               malformed.SerializeAsString(),
                                               freshReply(31, cCoefficients),
                                               freshReply(32, bCoefficients),
                                               freshReply(33, bCoefficients)};
        CachePeer peer(replies);
        SionnaHelper helper("in-memory-cache-ownership", peer.endpoint);
        helper.m_zmq_socket.set(zmq::sockopt::linger, 0);
        helper.m_zmq_socket.set(zmq::sockopt::rcvtimeo, 3000);
        auto cache = CreateObject<SionnaPropagationCache>();
        cache->SetSionnaHelper(helper);
        cache->SetOptimize(false);

        const auto heldMatrix = cache->GetPropagationChannelMatrix(a, b);
        const auto heldCsi = cache->GetPropagationCSI(a, b);
        const auto heldFrequencies = cache->GetPropagationFreq(a, b);
        const auto heldLoss = cache->GetPropagationLoss(a, b);
        const auto heldDelay = cache->GetPropagationDelay(a, b);
        auto changedMatrix = cache->GetPropagationChannelMatrix(a, b);
        auto changedCsi = cache->GetPropagationCSI(a, b);
        auto changedFrequencies = cache->GetPropagationFreq(a, b);
        changedMatrix.Set(0, 0, 0, {7, 8});
        changedCsi.at(1) = {9, 10};
        changedFrequencies.at(0) = -1;

        auto checkHit = [&](auto rx, double loss, int64_t delay,
                            const Coefficients& coefficients) {
            NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, rx), loss,
                                  "MU scalar associated with the wrong user");
            NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationDelay(a, rx).GetNanoSeconds(), delay,
                                  "Cached delay changed");
            const auto matrix = cache->GetPropagationChannelMatrix(a, rx);
            const auto csi = cache->GetPropagationCSI(a, rx);
            const auto frequencies = cache->GetPropagationFreq(a, rx);
            NS_TEST_ASSERT_MSG_EQ(matrix.GetNumSubcarriers(), 2, "Subcarrier dimension changed");
            NS_TEST_ASSERT_MSG_EQ(matrix.GetNumRxAntennas(), 1, "RX dimension changed");
            NS_TEST_ASSERT_MSG_EQ(matrix.GetNumTxAntennas(), 2, "TX dimension changed");
            NS_TEST_ASSERT_MSG_EQ(csi.size(), coefficients.size(), "CSI dimension changed");
            for (std::size_t i = 0; i < coefficients.size(); ++i)
            {
                NS_TEST_ASSERT_MSG_EQ(matrix.GetCoefficients().at(i), coefficients.at(i),
                                      "MU CFR mapped by sorted position or returned copy mutated cache");
                NS_TEST_ASSERT_MSG_EQ(csi.at(i), coefficients.at(i), "Cached CSI changed");
            }
            NS_TEST_ASSERT_MSG_EQ(frequencies.size(), 2, "Frequency count changed");
            NS_TEST_ASSERT_MSG_EQ(frequencies.at(0), 78125, "Returned frequency mutated cache");
            NS_TEST_ASSERT_MSG_EQ(frequencies.at(1), 156250, "Frequency order changed");
        };
        for (unsigned hit = 0; hit < 3; ++hit)
        {
            checkHit(b, 10, 3, bCoefficients);
            checkHit(c, 20, 7, cCoefficients);
        }
        const auto beforeRejection = cache->GetStorageStatistics();
        bool rejected = false;
        try { cache->GetPropagationLoss(d, e); }
        catch (const std::invalid_argument&) { rejected = true; }
        NS_TEST_ASSERT_MSG_EQ(rejected, true, "Malformed MU batch accepted");
        const auto afterRejection = cache->GetStorageStatistics();
        NS_TEST_ASSERT_MSG_EQ(afterRejection.snapshots, 2, "Valid MU prefix leaked into cache");
        NS_TEST_ASSERT_MSG_EQ(afterRejection.payloadBytes, beforeRejection.payloadBytes,
                              "Rejected MU batch changed payload");
        NS_TEST_ASSERT_MSG_EQ(afterRejection.capacityBytes, beforeRejection.capacityBytes,
                              "Rejected MU batch changed capacity");
        NS_TEST_ASSERT_MSG_EQ(afterRejection.peakSnapshots, beforeRejection.peakSnapshots,
                              "Rejected MU batch changed lifetime peak");
        checkHit(b, 10, 3, bCoefficients);
        checkHit(c, 20, 7, cCoefficients);

        auto checkHeld = [&]() {
            for (std::size_t i = 0; i < bCoefficients.size(); ++i)
            {
                NS_TEST_ASSERT_MSG_EQ(heldMatrix.GetCoefficients().at(i), bCoefficients.at(i),
                                      "Held matrix invalidated by cache lifetime");
                NS_TEST_ASSERT_MSG_EQ(heldCsi.at(i), bCoefficients.at(i),
                                      "Held CSI invalidated by cache lifetime");
            }
            NS_TEST_ASSERT_MSG_EQ(heldFrequencies.at(0), 78125, "Held frequency changed");
            NS_TEST_ASSERT_MSG_EQ(heldFrequencies.at(1), 156250, "Held frequency changed");
            NS_TEST_ASSERT_MSG_EQ(heldLoss, 10, "Held scalar changed");
            NS_TEST_ASSERT_MSG_EQ(heldDelay.GetNanoSeconds(), 3, "Held delay changed");
            NS_TEST_ASSERT_MSG_EQ(changedMatrix.Get(0, 0, 0), std::complex<double>(7, 8),
                                  "Caller matrix mutation was lost");
            NS_TEST_ASSERT_MSG_EQ(changedCsi.at(1), std::complex<double>(9, 10),
                                  "Caller CSI mutation was lost");
            NS_TEST_ASSERT_MSG_EQ(changedFrequencies.at(0), -1, "Caller frequency mutation lost");
        };
        Simulator::Schedule(NanoSeconds(11), [&]() {
            NS_TEST_ASSERT_MSG_EQ(cache->GetStorageStatistics().snapshots, 0,
                                  "Expired MU entries were not purged");
            checkHeld();
            cache->SetCaching(false);
            const auto disabledMatrix = cache->GetPropagationChannelMatrix(a, b);
            const auto disabledCsi = cache->GetPropagationCSI(a, b);
            NS_TEST_ASSERT_MSG_EQ(cache->GetStorageStatistics().snapshots, 0,
                                  "Disabled getters retained snapshots");
            checkHeld();
            cache->SetCaching(true);
            checkHit(b, 33, 5, bCoefficients);
            NS_TEST_ASSERT_MSG_EQ(disabledMatrix.Get(0, 0, 0), cCoefficients.at(0),
                                  "Disabled matrix invalidated by next reply or re-enable");
            NS_TEST_ASSERT_MSG_EQ(disabledCsi.at(0), bCoefficients.at(0),
                                  "Disabled CSI invalidated by re-enable");
            cache->SetCaching(false); // Discard a live snapshot while results remain held.
            checkHeld();
        });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(peer.Finish().size(), replies.size(),
                              "Repeated cached getters issued extra RPCs");
        helper.m_zmq_socket.close();
        Simulator::Destroy();
    }
};

class CacheBudgetTest : public TestCase
{
  public:
    CacheBudgetTest() : TestCase("cache budget boundaries, overflow, atomic rejection and recovery") {}
  private:
    void DoRun() override
    {
        using Limits = SionnaPropagationCache::ResourceLimits;
        Limits limits;
        limits.maxSubcarriers = 2;
        limits.maxRxAntennas = 2;
        limits.maxTxAntennas = 2;
        limits.maxCoefficientsPerChannel = 8;
        NS_TEST_ASSERT_MSG_EQ(SionnaPropagationCache::ValidateChannelDimensions(2, 2, 2, limits),
                              8, "Inclusive channel boundary");
        for (const auto& dimensions : {std::vector<uint64_t>{3, 2, 2}, {2, 3, 2},
                                      {2, 2, 3}, {0, 1, 1},
                                      {UINT64_MAX, UINT64_MAX, UINT64_MAX}})
        {
            bool rejected = false;
            try { SionnaPropagationCache::ValidateChannelDimensions(
                    dimensions[0], dimensions[1], dimensions[2], limits); }
            catch (const std::invalid_argument&) { rejected = true; }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, "Reject dimensions/overflow before allocation");
        }
        limits.maxCoefficientsPerChannel = 7;
        bool rejected = false;
        try { SionnaPropagationCache::ValidateChannelDimensions(2, 2, 2, limits); }
        catch (const std::invalid_argument&) { rejected = true; }
        NS_TEST_ASSERT_MSG_EQ(rejected, true, "Coefficient boundary");

        auto a = MakeCacheRadio(), b = MakeCacheRadio();
        auto c = MakeCacheRadio(), d = MakeCacheRadio();
        ns3sionna::Wrapper first, oversized, recovery;
        AddCacheState(first, a, b, 0, 10, 10);
        AddCacheState(first, a, b, 20, 30, 50);
        AddCacheState(oversized, c, d, 0, 100, 20);
        AddCacheState(oversized, c, d, 20, 100, 30);
        for (auto& state : *oversized.mutable_channel_state_response()->mutable_csi())
        {
            auto* rx = state.mutable_rx_nodes(0);
            rx->add_frequencies(156250); rx->add_csi_real(1); rx->add_csi_imag(0);
        }
        AddCacheState(recovery, c, d, 11, 100, 70);
        std::vector<std::string> replies{first.SerializeAsString()};
        for (int i = 0; i < 6; ++i) { replies.push_back(oversized.SerializeAsString()); }
        replies.push_back(recovery.SerializeAsString());
        CachePeer peer(replies);
        SionnaHelper helper("budget-contract", peer.endpoint);
        auto cache = CreateObject<SionnaPropagationCache>();
        cache->SetSionnaHelper(helper); cache->SetOptimize(false);
        Limits base;
        base.maxSnapshots = 2; base.maxPayloadBytes = 40;
        base.maxReplyBytes = first.ByteSizeLong();
        cache->SetResourceLimits(base);
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 10, "Exact storage/wire boundaries");
        base.maxReplyBytes = 268435456;
        base.maxSnapshots = 65536; base.maxPayloadBytes = 536870912;
        for (int i = 0; i < 6; ++i)
        {
            auto small = base;
            switch (i)
            {
            case 0: small.maxReplyBytes = oversized.ByteSizeLong() - 1; break;
            case 1: small.maxBatchCoefficients = 3; break;
            case 2: small.maxSnapshots = 3; break;
            case 3: small.maxPayloadBytes = 119; break;
            case 4: small.maxSubcarriers = 1; break;
            case 5: small.maxCoefficientsPerChannel = 1; break;
            }
            cache->SetResourceLimits(small);
            rejected = false;
            try { cache->GetPropagationLoss(c, d); }
            catch (const std::invalid_argument&) { rejected = true; }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, "Oversized reply accepted");
            NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 10, "Old valid entry lost");
            const auto stats = cache->GetStorageStatistics();
            NS_TEST_ASSERT_MSG_EQ(stats.snapshots, 2, "Rejected prefix leaked or future evicted");
            NS_TEST_ASSERT_MSG_EQ(stats.payloadBytes, 40, "Rejected payload retained");
            NS_TEST_ASSERT_MSG_EQ(stats.peakPayloadBytes, 40, "Rejection changed peaks");
        }
        base.maxSnapshots = 2; base.maxPayloadBytes = 40;
        cache->SetResourceLimits(base);
        auto below = base; below.maxPayloadBytes = 39;
        rejected = false;
        try { cache->SetResourceLimits(below); }
        catch (const std::invalid_argument&) { rejected = true; }
        NS_TEST_ASSERT_MSG_EQ(rejected, true, "Lowering budget silently evicted snapshots");
        Simulator::Schedule(NanoSeconds(11), [&] {
            NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(c, d), 70, "Retry after expiry failed");
            NS_TEST_ASSERT_MSG_EQ(cache->GetStorageStatistics().payloadBytes, 40, "Retry exceeds budget");
        });
        Simulator::Schedule(NanoSeconds(20), [&] {
            NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 50, "Future snapshot lost");
        });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(peer.Finish().size(), replies.size(), "Unexpected retry/hit requests");
        helper.m_zmq_socket.close(); Simulator::Destroy();
    }
};

// Pool reuse covers real REQ/REP traffic while public results remain independent.
class CacheBatchPoolReuseTest : public TestCase
{
  public:
    CacheBatchPoolReuseTest()
        : TestCase("bounded batch allocation pool reuses empty capacity and independent getters")
    {
    }

  private:
    void DoRun() override
    {
        auto a = MakeCacheRadio(), b = MakeCacheRadio();
        constexpr unsigned repetitions = 512;
        auto reply = [&](unsigned entries, unsigned value, bool evolving = false) {
            ns3sionna::Wrapper wrapper;
            for (unsigned i = 0; i < entries; ++i)
            {
                AddCacheState(wrapper, a, b, evolving ? value - 1 : 0,
                              evolving ? value - 1 : 10000, value + i);
                auto* rx = wrapper.mutable_channel_state_response()->mutable_csi(i)->
                    mutable_rx_nodes(0);
                rx->set_num_rx_antennas(2);
                rx->set_num_tx_antennas(2);
                rx->set_frequencies(0, 78125 + value);
                rx->add_frequencies(156250 + value);
                rx->clear_csi_real();
                rx->clear_csi_imag();
                for (unsigned coefficient = 0; coefficient < 8; ++coefficient)
                {
                    rx->add_csi_real(value + coefficient);
                    rx->add_csi_imag(-static_cast<double>(value));
                }
            }
            return wrapper.SerializeAsString();
        };
        std::vector<std::string> replies;
        for (unsigned value = 1; value <= repetitions; ++value)
        {
            replies.push_back(reply(2, value, true));
        }
        // Grow, shrink, seed a smaller bounded slot, bypass a large batch, reuse,
        // then exercise a byte bound below one entry and explicit pool disable.
        for (const auto& item : std::vector<std::pair<unsigned, unsigned>>{
                 {32, 200}, {2, 201}, {2, 202}, {32, 203}, {2, 204}, {2, 205}, {2, 206}})
        {
            replies.push_back(reply(item.first, item.second));
        }
        CachePeer peer(replies);
        SionnaHelper helper("batch-pool-reuse", peer.endpoint);
        helper.m_zmq_socket.set(zmq::sockopt::linger, 0);
        helper.m_zmq_socket.set(zmq::sockopt::rcvtimeo, 3000);
        auto cache = CreateObject<SionnaPropagationCache>();
        cache->SetSionnaHelper(helper);
        cache->SetOptimize(false);
        cache->SetCaching(false);
        MimoChannelMatrix heldMatrix;
        std::vector<std::complex<double>> heldCsi;
        std::vector<int> heldFrequencies;
        SionnaPropagationCache::PoolStatistics initial;
        // Each frozen peer response expires at its distinct requested nanosecond.
        // Only the production staging pool is bounded; prebuilt peer fixtures are not.
        for (unsigned value = 1; value <= repetitions; ++value)
        {
            Simulator::Schedule(NanoSeconds(value - 1), [&, value]() {
                if (value == 1)
                {
                    heldMatrix = cache->GetPropagationChannelMatrix(a, b);
                }
                else if (value == 2)
                {
                    heldCsi = cache->GetPropagationCSI(a, b);
                }
                else if (value == 3)
                {
                    heldFrequencies = cache->GetPropagationFreq(a, b);
                    initial = cache->GetPoolStatistics();
                    NS_TEST_ASSERT_MSG_EQ(initial.allocations, 1, "Same-sized batches allocated again");
                    NS_TEST_ASSERT_MSG_EQ(initial.misses, 1, "Initial allocation is one miss");
                    NS_TEST_ASSERT_MSG_EQ(initial.hits, 2, "Subsequent getters did not reuse staging storage");
                    NS_TEST_ASSERT_MSG_EQ(initial.buffers, 1, "No empty staging allocation retained");
                    NS_TEST_ASSERT_MSG_GT(initial.capacityBytes, 0, "Pool capacity was not accounted");
                }
                else if (value % 4 == 0)
                {
                    auto matrix = cache->GetPropagationChannelMatrix(a, b);
                    NS_TEST_ASSERT_MSG_EQ(matrix.GetNumSubcarriers(), 2, "Fresh tensor shape changed");
                    NS_TEST_ASSERT_MSG_EQ(matrix.Get(0, 0, 0),
                        std::complex<double>(value, -static_cast<double>(value)), "Stale pooled CFR");
                    matrix.Set(0, 0, 0, {999, 999});
                }
                else if (value % 4 == 1)
                {
                    auto csi = cache->GetPropagationCSI(a, b);
                    NS_TEST_ASSERT_MSG_EQ(csi.at(7),
                        std::complex<double>(value + 7, -static_cast<double>(value)), "Stale CSI vector");
                    csi.at(0) = {999, 999};
                }
                else if (value % 4 == 2)
                {
                    auto frequencies = cache->GetPropagationFreq(a, b);
                    NS_TEST_ASSERT_MSG_EQ(frequencies.at(0), 78125 + value, "Stale frequency vector");
                    frequencies.at(0) = -1;
                }
                else
                {
                    NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), value, "Stale scalar reply");
                }
                const auto storage = cache->GetStorageStatistics();
                NS_TEST_ASSERT_MSG_EQ(storage.snapshots, 0, "Pool retained disabled snapshots");
                NS_TEST_ASSERT_MSG_EQ(storage.payloadBytes, 0, "Pool retained disabled CFR payload");
                NS_TEST_ASSERT_MSG_EQ(storage.capacityBytes, 0, "Pool changed snapshot capacity semantics");
            });
        }
        Simulator::Run();
        const auto stable = cache->GetPoolStatistics();
        NS_TEST_ASSERT_MSG_EQ(stable.allocations, 1, "Long repeated workload grew staging allocations");
        NS_TEST_ASSERT_MSG_EQ(stable.hits, repetitions - 1, "Long workload missed reusable capacity");
        NS_TEST_ASSERT_MSG_EQ(stable.capacityBytes, initial.capacityBytes, "Long workload grew idle bytes");
        NS_TEST_ASSERT_MSG_EQ(stable.peakCapacityBytes, initial.capacityBytes, "Long workload grew peak bytes");
        NS_TEST_ASSERT_MSG_LT_OR_EQ(stable.capacityBytes, 1048576, "Long workload exceeded pool bound");

        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 200, "Larger batch failed");
        const auto grown = cache->GetPoolStatistics();
        NS_TEST_ASSERT_MSG_EQ(grown.allocations, 2, "Larger staging batch did not grow storage");
        NS_TEST_ASSERT_MSG_GT(grown.capacityBytes, stable.capacityBytes, "Growth capacity not counted");
        NS_TEST_ASSERT_MSG_LT_OR_EQ(grown.capacityBytes, 1048576, "Default pool bound exceeded");
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 201, "Shrinking batch failed");
        NS_TEST_ASSERT_MSG_EQ(cache->GetPoolStatistics().allocations, 2, "Shrinking batch allocated");
        cache->SetBatchPoolLimit(initial.capacityBytes);
        NS_TEST_ASSERT_MSG_EQ(cache->GetPoolStatistics().capacityBytes, 0, "Lower bound retained large slot");
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 202, "Small bounded seed failed");
        const auto small = cache->GetPoolStatistics();
        NS_TEST_ASSERT_MSG_EQ(small.capacityBytes, initial.capacityBytes, "Small seed exceeds exact bound");
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 203, "Oversized pool batch should succeed");
        const auto bypassed = cache->GetPoolStatistics();
        NS_TEST_ASSERT_MSG_EQ(bypassed.bypasses, 1, "Oversized batch did not bypass pool");
        NS_TEST_ASSERT_MSG_EQ(bypassed.capacityBytes, small.capacityBytes, "Bypass consumed smaller idle slot");
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 204, "Small slot failed after bypass");
        const auto reused = cache->GetPoolStatistics();
        NS_TEST_ASSERT_MSG_EQ(reused.hits, bypassed.hits + 1, "Smaller slot not reused after bypass");
        NS_TEST_ASSERT_MSG_EQ(reused.allocations, bypassed.allocations, "Reuse after bypass allocated");
        cache->SetBatchPoolLimit(1);
        NS_TEST_ASSERT_MSG_EQ(cache->GetPoolStatistics().buffers, 0, "Tiny limit did not release idle slot");
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 205, "Tiny pool bound rejected valid reply");
        NS_TEST_ASSERT_MSG_EQ(cache->GetPoolStatistics().capacityBytes, 0, "Tiny bound retained oversized capacity");
        cache->SetBatchPoolLimit(0);
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 206, "Disabled pool rejected valid reply");
        const auto disabled = cache->GetPoolStatistics();
        NS_TEST_ASSERT_MSG_EQ(disabled.capacityBytes, 0, "Disabled pool retained native allocation");
        NS_TEST_ASSERT_MSG_EQ(disabled.bypasses, 3, "Tiny and disabled bounds must bypass");
        NS_TEST_ASSERT_MSG_EQ(disabled.peakCapacityBytes, grown.peakCapacityBytes, "Trim reset lifetime peak");
        NS_TEST_ASSERT_MSG_EQ(heldMatrix.Get(0, 0, 0), std::complex<double>(1, -1),
                              "Held matrix changed during pooled reuse");
        NS_TEST_ASSERT_MSG_EQ(heldCsi.at(7), std::complex<double>(9, -2),
                              "Held CSI changed during pooled reuse");
        NS_TEST_ASSERT_MSG_EQ(heldFrequencies.at(0), 78128, "Held frequencies changed during pooled reuse");
        const auto requests = peer.Finish();
        NS_TEST_ASSERT_MSG_EQ(requests.size(), replies.size(), "Pool changed disabled RPC count");
        for (unsigned i = 0; i < repetitions; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(requests.at(i).time(), i, "Long workload did not evolve request time");
            NS_TEST_ASSERT_MSG_EQ(requests.at(i).tx_node(), CacheRadioId(a), "Wrong long workload TX");
            NS_TEST_ASSERT_MSG_EQ(requests.at(i).rx_node(), CacheRadioId(b), "Wrong long workload RX");
        }
        std::cout << "R3_POOL_SUMMARY {\"requests\":" << requests.size() - 7
                  << ",\"allocations\":" << stable.allocations << ",\"hits\":" << stable.hits
                  << ",\"capacity_bytes\":" << stable.capacityBytes
                  << ",\"limit_bytes\":1048576}" << std::endl;
        helper.m_zmq_socket.close();
        Simulator::Destroy();
    }
};

class CacheBatchPoolValidationTest : public TestCase
{
  public:
    CacheBatchPoolValidationTest()
        : TestCase("batch pool preflight, atomic suffix rejection, recovery and independent limits")
    {
    }

  private:
    void DoRun() override
    {
        auto a = MakeCacheRadio(), b = MakeCacheRadio();
        auto c = MakeCacheRadio(), d = MakeCacheRadio();
        auto e = MakeCacheRadio(), f = MakeCacheRadio();
        auto g = MakeCacheRadio(), h = MakeCacheRadio();
        ns3sionna::Wrapper initial, preflight, malformed, recovery, disabled;
        AddCacheState(initial, a, b, 0, 100, 10);
        AddCacheState(initial, c, d, 0, 100, 20);
        AddCacheState(recovery, e, f, 0, 100, 30);
        AddCacheState(recovery, g, h, 0, 100, 40);
        preflight = recovery;
        preflight.mutable_channel_state_response()->mutable_csi(1)->mutable_rx_nodes(0)->
            set_num_tx_antennas(std::numeric_limits<uint32_t>::max());
        malformed = recovery;
        malformed.mutable_channel_state_response()->mutable_csi(1)->mutable_rx_nodes(0)->
            clear_csi_imag();
        AddCacheState(disabled, e, f, 0, 100, 50);
        const std::vector<std::string> replies{initial.SerializeAsString(), preflight.SerializeAsString(),
            malformed.SerializeAsString(), recovery.SerializeAsString(), disabled.SerializeAsString()};
        CachePeer peer(replies);
        SionnaHelper helper("batch-pool-validation", peer.endpoint);
        helper.m_zmq_socket.set(zmq::sockopt::linger, 0);
        helper.m_zmq_socket.set(zmq::sockopt::rcvtimeo, 3000);
        auto cache = CreateObject<SionnaPropagationCache>();
        cache->SetSionnaHelper(helper);
        cache->SetOptimize(false);
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 10, "Initial batch failed");
        const auto before = cache->GetPoolStatistics();
        const auto liveBefore = cache->GetStorageStatistics();
        bool rejected = false;
        try { cache->GetPropagationLoss(e, f); }
        catch (const std::invalid_argument&) { rejected = true; }
        NS_TEST_ASSERT_MSG_EQ(rejected, true, "Oversized dimensions passed resource preflight");
        const auto afterPreflight = cache->GetPoolStatistics();
        NS_TEST_ASSERT_MSG_EQ(afterPreflight.hits, before.hits, "Preflight acquired pooled storage");
        NS_TEST_ASSERT_MSG_EQ(afterPreflight.misses, before.misses, "Preflight counted pool miss");
        NS_TEST_ASSERT_MSG_EQ(afterPreflight.allocations, before.allocations, "Preflight allocated staging");
        NS_TEST_ASSERT_MSG_EQ(afterPreflight.capacityBytes, before.capacityBytes, "Preflight consumed idle slot");
        NS_TEST_ASSERT_MSG_EQ(afterPreflight.peakCapacityBytes, before.peakCapacityBytes, "Preflight grew pool peak");
        rejected = false;
        try { cache->GetPropagationLoss(e, f); }
        catch (const std::invalid_argument&) { rejected = true; }
        NS_TEST_ASSERT_MSG_EQ(rejected, true, "Malformed suffix passed validation");
        const auto afterMalformed = cache->GetPoolStatistics();
        NS_TEST_ASSERT_MSG_EQ(afterMalformed.hits, before.hits + 1, "Malformed decode did not use lease");
        NS_TEST_ASSERT_MSG_EQ(afterMalformed.allocations, before.allocations, "Exception lost reusable allocation");
        NS_TEST_ASSERT_MSG_EQ(afterMalformed.capacityBytes, before.capacityBytes, "Exception failed to return empty slot");
        const auto liveAfter = cache->GetStorageStatistics();
        NS_TEST_ASSERT_MSG_EQ(liveAfter.snapshots, liveBefore.snapshots, "Valid prefix leaked into cache");
        NS_TEST_ASSERT_MSG_EQ(liveAfter.payloadBytes, liveBefore.payloadBytes, "Rejected batch retained CFR");
        NS_TEST_ASSERT_MSG_EQ(liveAfter.capacityBytes, liveBefore.capacityBytes, "Rejected batch grew cache capacity");
        NS_TEST_ASSERT_MSG_EQ(liveAfter.peakCapacityBytes, liveBefore.peakCapacityBytes, "Rejection changed cache peak");
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(a, b), 10, "Old entry changed after pool rejection");
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(e, f), 30, "Pool failed recovery after rejection");
        const auto recovered = cache->GetPoolStatistics();
        NS_TEST_ASSERT_MSG_EQ(recovered.hits, before.hits + 2, "Recovery did not reuse returned slot");
        NS_TEST_ASSERT_MSG_EQ(recovered.allocations, before.allocations, "Recovery allocated staging again");
        cache->SetBatchPoolLimit(recovered.capacityBytes - 1);
        NS_TEST_ASSERT_MSG_EQ(cache->GetPoolStatistics().capacityBytes, 0, "Lowering limit did not trim pool");
        NS_TEST_ASSERT_MSG_EQ(cache->GetStorageStatistics().snapshots, 4, "Pool trim evicted live snapshots");
        cache->SetBatchPoolLimit(0);
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(e, f), 30, "Disabling pool invalidated cache hit");
        NS_TEST_ASSERT_MSG_EQ(cache->GetPoolStatistics().misses, recovered.misses, "Cache hit touched disabled pool");
        cache->SetCaching(false);
        NS_TEST_ASSERT_MSG_EQ(cache->GetPropagationLoss(e, f), 50, "Disabled cache returned retained pool data");
        const auto storage = cache->GetStorageStatistics();
        NS_TEST_ASSERT_MSG_EQ(storage.snapshots, 0, "Disabled pool/cache retained snapshots");
        NS_TEST_ASSERT_MSG_EQ(storage.payloadBytes, 0, "Disabled pool/cache retained payload");
        NS_TEST_ASSERT_MSG_EQ(storage.capacityBytes, 0, "Disabled cache retained capacity");
        const auto pooled = cache->GetPoolStatistics();
        NS_TEST_ASSERT_MSG_EQ(pooled.buffers, 0, "Disabled pool retained idle allocation");
        NS_TEST_ASSERT_MSG_EQ(pooled.bypasses, 1, "Disabled pool did not bypass staging retention");
        NS_TEST_ASSERT_MSG_EQ(pooled.peakCapacityBytes, before.peakCapacityBytes, "Disable reset pool lifetime peak");
        NS_TEST_ASSERT_MSG_EQ(peer.Finish().size(), replies.size(), "Pool changed hit/rejection RPC count");
        helper.m_zmq_socket.close();
        Simulator::Destroy();
    }
};

class SionnaRandomStreamsTest : public TestCase
{
  public:
    SionnaRandomStreamsTest() : TestCase("explicit seed, run and stream export and atomic rejection") {}
    void DoRun() override
    {
        const auto oldSeed = RngSeedManager::GetSeed();
        const auto oldRun = RngSeedManager::GetRun();
        RngSeedManager::SetSeed(43);
        RngSeedManager::SetRun(7);
        SionnaHelper helper("", "inproc://rng-contract");
        NS_TEST_ASSERT_MSG_EQ(helper.AssignStreams(12), 2, "Reserve RT and mobility IDs");
        ns3sionna::SimInitMessage message;
        helper.PopulateRandomness(message);
        const auto& rng = message.randomness();
        NS_TEST_ASSERT_MSG_EQ(rng.mapping_version(), 1, "Mapping version");
        NS_TEST_ASSERT_MSG_EQ(rng.ns3_seed(), 43, "Export actual global seed");
        NS_TEST_ASSERT_MSG_EQ(rng.ns3_run(), 7, "Export actual global run");
        NS_TEST_ASSERT_MSG_EQ(rng.rt_stream(), 12, "RT stream");
        NS_TEST_ASSERT_MSG_EQ(rng.mobility_stream(), 13, "Mobility stream");
        const auto before = message.SerializeAsString();
        for (int64_t invalid : std::array<int64_t, 2>{-1, std::numeric_limits<int64_t>::max()})
        {
            bool rejected = false;
            try { helper.AssignStreams(invalid); }
            catch (const std::invalid_argument&) { rejected = true; }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, "Invalid stream range accepted");
            helper.PopulateRandomness(message);
            NS_TEST_ASSERT_MSG_EQ(message.SerializeAsString(), before, "Rejection changed assignment");
        }
        RngSeedManager::SetSeed(oldSeed);
        RngSeedManager::SetRun(oldRun);
    }
};

class SionnaTestSuite : public TestSuite
{
  public:
    SionnaTestSuite()
        : TestSuite("sionna", UNIT)
    {
        AddTestCase(new SionnaRandomStreamsTest, TestCase::QUICK);
        AddTestCase(new DisabledCacheFreshReplyTest, TestCase::QUICK);
        AddTestCase(new CacheGetterOwnershipTest, TestCase::QUICK);
        AddTestCase(new CacheBudgetTest, TestCase::QUICK);
        AddTestCase(new CacheBatchPoolReuseTest, TestCase::QUICK);
        AddTestCase(new CacheBatchPoolValidationTest, TestCase::QUICK);
        for (unsigned scenario = 0; scenario < 3; ++scenario)
        {
            AddTestCase(new CacheContractTest(scenario), TestCase::QUICK);
        }
        AddTestCase(new SionnaMimoHelperDefaultsTest, TestCase::QUICK);
        AddTestCase(new SionnaMimoHelperValidConfigurationTest, TestCase::QUICK);
        AddTestCase(new SionnaMimoHelperBeamformingSelectionTest, TestCase::QUICK);
        AddTestCase(new SionnaMimoHelperInvalidConfigurationTest, TestCase::QUICK);
        AddTestCase(new SionnaMimoHelperIntegrationTest(HelperIntegrationCase::SISO),
                    TestCase::QUICK);
        AddTestCase(new SionnaMimoHelperIntegrationTest(HelperIntegrationCase::OPEN_LOOP_2X2),
                    TestCase::QUICK);
        AddTestCase(new SionnaMimoHelperIntegrationTest(
                        HelperIntegrationCase::FIXED_PRECODING_2X1),
                    TestCase::QUICK);
        AddTestCase(new MimoChannelMatrixLayoutTest, TestCase::QUICK);
        AddTestCase(new MimoPrecodingMatrixContractTest, TestCase::QUICK);
        AddTestCase(new WifiLinkBudgetConversionTest, TestCase::QUICK);
        AddTestCase(new WifiLinkBudgetCalculationTest, TestCase::QUICK);
        AddTestCase(new HeEesmFlatChannelTest, TestCase::QUICK);
        AddTestCase(new HeAwgnCalibrationContextTest, TestCase::QUICK);
        AddTestCase(new HeCalibrationProfileTest, TestCase::QUICK);
        AddTestCase(new HeCalibrationSpectrumReceptionTest, TestCase::QUICK);
        AddTestCase(new HeEesmFrequencySelectiveTest, TestCase::QUICK);
        AddTestCase(new HeSisoErrorModelParityTest, TestCase::QUICK);
        AddTestCase(new HePerMonotonicityTest, TestCase::QUICK);
        AddTestCase(new HeLinkReproducibilityTest, TestCase::QUICK);
        AddTestCase(new SuMimoSisoReductionTest, TestCase::QUICK);
        AddTestCase(new SuMimoOrthogonalChannelTest, TestCase::QUICK);
        AddTestCase(new SuMimoFrequencySelectiveTest, TestCase::QUICK);
        AddTestCase(new SuMimoFixedPrecodingTest, TestCase::QUICK);
        AddTestCase(new SuMimoSingleStreamMrtMrcTest, TestCase::QUICK);
        AddTestCase(new SuMimoSvdPrecodingAndPowerAllocationTest, TestCase::QUICK);
        AddTestCase(new SuMimoChannelRankTest, TestCase::QUICK);
        AddTestCase(new SpectrumWifiPhyMimoAdapterTest, TestCase::QUICK);
        AddTestCase(new LegacySisoDeserializationTest, TestCase::QUICK);
        AddTestCase(new MimoProtobufDeserializationTest, TestCase::QUICK);
        AddTestCase(new MultiUserChannelSnapshotContractTest, TestCase::QUICK);
        AddTestCase(new MultiUserChannelSnapshotRejectionTest, TestCase::QUICK);
        AddTestCase(new DlMuMimoFixedPrecodingAnalyticalTest, TestCase::QUICK);
        AddTestCase(new HeDlMuMimoPerUserContractTest, TestCase::QUICK);
        AddTestCase(new HeDlMuMimoSpectrumAdapterTest, TestCase::QUICK);
        AddTestCase(new HeDlMuMimoSpectrumAdapterRejectionTest, TestCase::QUICK);
        AddTestCase(new HeDlMuMimoSpectrumPpduIntegrationTest, TestCase::QUICK);
        AddTestCase(new HeDlMuMimoSpectrumChannelReceptionTest, TestCase::QUICK);
        AddTestCase(new DlMuMimoZfRzfAnalyticalTest, TestCase::QUICK);
        AddTestCase(new DlMuMimoZfRzfRejectionTest, TestCase::QUICK);
        AddTestCase(new DlMuMimoRejectionTest, TestCase::QUICK);
    }
};

static SionnaTestSuite g_sionnaTestSuite;

} // namespace
