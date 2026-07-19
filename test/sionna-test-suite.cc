/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/error-rate-model.h"
#include "ns3/he-phy.h"
#include "ns3/he-su-mimo-link-abstraction.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/mimo-channel-matrix.h"
#include "ns3/sionna-propagation-cache.h"
#include "ns3/su-mimo-link-model.h"
#include "ns3/test.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/wifi-link-budget.h"
#include "ns3/wifi-tx-vector.h"

#include <cmath>
#include <complex>

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
    }
};

class SionnaTestSuite : public TestSuite
{
  public:
    SionnaTestSuite()
        : TestSuite("sionna", UNIT)
    {
        AddTestCase(new MimoChannelMatrixLayoutTest, TestCase::QUICK);
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
        AddTestCase(new SuMimoChannelRankTest, TestCase::QUICK);
        AddTestCase(new SpectrumWifiPhyMimoAdapterTest, TestCase::QUICK);
        AddTestCase(new LegacySisoDeserializationTest, TestCase::QUICK);
        AddTestCase(new MimoProtobufDeserializationTest, TestCase::QUICK);
    }
};

static SionnaTestSuite g_sionnaTestSuite;

} // namespace
