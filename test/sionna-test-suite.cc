/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/mimo-channel-matrix.h"
#include "ns3/sionna-propagation-cache.h"
#include "ns3/su-mimo-link-model.h"
#include "ns3/test.h"

#include <cmath>
#include <complex>

using namespace ns3;

namespace
{

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

class SionnaTestSuite : public TestSuite
{
  public:
    SionnaTestSuite()
        : TestSuite("sionna", UNIT)
    {
        AddTestCase(new MimoChannelMatrixLayoutTest, TestCase::QUICK);
        AddTestCase(new SuMimoSisoReductionTest, TestCase::QUICK);
        AddTestCase(new SuMimoOrthogonalChannelTest, TestCase::QUICK);
        AddTestCase(new SuMimoFrequencySelectiveTest, TestCase::QUICK);
        AddTestCase(new LegacySisoDeserializationTest, TestCase::QUICK);
        AddTestCase(new MimoProtobufDeserializationTest, TestCase::QUICK);
    }
};

static SionnaTestSuite g_sionnaTestSuite;

} // namespace
