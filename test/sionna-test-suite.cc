/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/double.h"
#include "ns3/error-rate-model.h"
#include "ns3/he-phy.h"
#include "ns3/he-su-mimo-link-abstraction.h"
#include "ns3/mimo-channel-matrix.h"
#include "ns3/mimo-precoding-matrix.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/simulator.h"
#include "ns3/sionna-helper.h"
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
#include "ns3/wifi-mac-helper.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-tx-vector.h"

#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>
#include <string>

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
        NS_TEST_ASSERT_MSG_EQ(supportedModes.size(), 2, "Unexpected number of supported modes");
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
            false,
            "Beamforming is falsely reported as supported");
        NS_TEST_ASSERT_MSG_EQ(SionnaMimoHelper::IsModeSupported(SionnaMimoMode::MU_MIMO),
                              false,
                              "MU-MIMO is falsely reported as supported");
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
        expectRejected(configuration, "Phase-3 beamforming was accepted");

        configuration = {};
        configuration.mode = SionnaMimoMode::MU_MIMO;
        expectRejected(configuration, "Phase-4 MU-MIMO was accepted");

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

class SionnaTestSuite : public TestSuite
{
  public:
    SionnaTestSuite()
        : TestSuite("sionna", UNIT)
    {
        AddTestCase(new SionnaMimoHelperDefaultsTest, TestCase::QUICK);
        AddTestCase(new SionnaMimoHelperValidConfigurationTest, TestCase::QUICK);
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
        AddTestCase(new HeEesmFrequencySelectiveTest, TestCase::QUICK);
        AddTestCase(new HeSisoErrorModelParityTest, TestCase::QUICK);
        AddTestCase(new HePerMonotonicityTest, TestCase::QUICK);
        AddTestCase(new HeLinkReproducibilityTest, TestCase::QUICK);
        AddTestCase(new SuMimoSisoReductionTest, TestCase::QUICK);
        AddTestCase(new SuMimoOrthogonalChannelTest, TestCase::QUICK);
        AddTestCase(new SuMimoFrequencySelectiveTest, TestCase::QUICK);
        AddTestCase(new SuMimoFixedPrecodingTest, TestCase::QUICK);
        AddTestCase(new SuMimoChannelRankTest, TestCase::QUICK);
        AddTestCase(new SpectrumWifiPhyMimoAdapterTest, TestCase::QUICK);
        AddTestCase(new LegacySisoDeserializationTest, TestCase::QUICK);
        AddTestCase(new MimoProtobufDeserializationTest, TestCase::QUICK);
    }
};

static SionnaTestSuite g_sionnaTestSuite;

} // namespace
