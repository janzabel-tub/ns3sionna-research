/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/mimo-channel-matrix.h"
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
    }
};

static SionnaTestSuite g_sionnaTestSuite;

} // namespace
