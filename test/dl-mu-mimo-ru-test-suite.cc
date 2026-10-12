/* SPDX-License-Identifier: GPL-2.0-only */

#include "ns3/dl-mu-mimo-link-model.h"
#include "ns3/he-dl-mu-mimo-link-abstraction.h"
#include "ns3/he-phy.h"
#include "ns3/test.h"
#include "ns3/wifi-link-budget.h"

#include <cmath>
#include <complex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

MultiUserChannel
MakeUser(uint32_t userId, const MimoChannelMatrix& channel)
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
MakeSnapshot(const std::vector<std::pair<uint32_t, MimoChannelMatrix>>& users)
{
    MultiUserChannelSnapshot snapshot{NanoSeconds(1234),
                                      MultiUserChannelDirection::DOWNLINK,
                                      {}};
    for (const auto& user : users)
    {
        snapshot.users.push_back(MakeUser(user.first, user.second));
    }
    return snapshot;
}

WifiLinkBudgetResult
MakeBudget(double referenceSnrLinear)
{
    constexpr double boltzmann = 1.3803e-23;
    const double txPowerDbm =
        WifiLinkBudget::LinearToDb(referenceSnrLinear * boltzmann) + 30.0;
    return WifiLinkBudget::Calculate(txPowerDbm, 0.0, 1.0, 1.0, 0.0);
}

WifiTxVector
MakeTxVector()
{
    return WifiTxVector(HePhy::GetHeMcs0(),
                        0,
                        WIFI_PREAMBLE_HE_SU,
                        800,
                        1,
                        1,
                        0,
                        20,
                        false);
}

class DlMuMimoRuPowerNormalizationTest : public TestCase
{
  public:
    DlMuMimoRuPowerNormalizationTest()
        : TestCase("DL MU normalizes active precoder energy independently per RU")
    {
    }

  private:
    void DoRun() override
    {
        MimoChannelMatrix user3Channel(1, 1, 2);
        user3Channel.Set(0, 0, 0, {1.0, 0.0});
        MimoChannelMatrix user9Channel(1, 1, 2);
        user9Channel.Set(0, 0, 1, {1.0, 0.0});
        const auto snapshot = MakeSnapshot({{3, user3Channel}, {9, user9Channel}});
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

        const auto equalRuPower = DlMuMimoLinkModel::Calculate(
            snapshot,
            weighted,
            {{3, 10.0, 0}, {9, 10.0, 1}});
        NS_TEST_ASSERT_MSG_EQ_TOL(equalRuPower.GetUserById(3).GetSinrLinear(0, 0),
                                  10.0,
                                  1e-12,
                                  "The 1.5-norm RU did not receive its declared power");
        NS_TEST_ASSERT_MSG_EQ_TOL(equalRuPower.GetUserById(9).GetSinrLinear(0, 0),
                                  10.0,
                                  1e-12,
                                  "The 0.5-norm RU did not receive its declared power");

        const auto unequalRuPower = DlMuMimoLinkModel::Calculate(
            snapshot,
            weighted,
            {{3, 8.0, 0}, {9, 2.0, 1}});
        NS_TEST_ASSERT_MSG_EQ_TOL(unequalRuPower.GetUserById(3).GetSinrLinear(0, 0),
                                  8.0,
                                  1e-12,
                                  "The first RU did not preserve its unequal power budget");
        NS_TEST_ASSERT_MSG_EQ_TOL(unequalRuPower.GetUserById(9).GetSinrLinear(0, 0),
                                  2.0,
                                  1e-12,
                                  "The second RU did not preserve its unequal power budget");

        const auto sharedRu = DlMuMimoLinkModel::Calculate(
            snapshot,
            weighted,
            {{3, 10.0, 0}, {9, 10.0, 0}});
        NS_TEST_ASSERT_MSG_EQ_TOL(sharedRu.GetUserById(3).GetSinrLinear(0, 0),
                                  7.5,
                                  1e-12,
                                  "Shared-RU normalization changed the 1.5 stream weight");
        NS_TEST_ASSERT_MSG_EQ_TOL(sharedRu.GetUserById(9).GetSinrLinear(0, 0),
                                  2.5,
                                  1e-12,
                                  "Shared-RU normalization changed the 0.5 stream weight");

        const DlMuMimoPrecodingMatrix zeroSecondGroup(
            MimoPrecodingMatrix::FrequencyFlat(
                1,
                2,
                2,
                {{std::sqrt(2.0), 0.0},
                 {0.0, 0.0},
                 {0.0, 0.0},
                 {0.0, 0.0}}),
            {{3, 0}, {9, 0}});
        bool rejected = false;
        try
        {
            static_cast<void>(DlMuMimoLinkModel::Calculate(
                snapshot,
                zeroSecondGroup,
                {{3, 10.0, 0}, {9, 10.0, 1}}));
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        NS_TEST_ASSERT_MSG_EQ(rejected,
                              true,
                              "A scheduled RU with zero active precoder power was accepted");
    }
};

class HeDlMuMimoSelectedToneTest : public TestCase
{
  public:
    HeDlMuMimoSelectedToneTest()
        : TestCase("HE DL MU EESM uses only selected tones and validates their indices")
    {
    }

  private:
    HeDlMuMimoUserTransmission
    MakeTransmission(const std::vector<uint32_t>& subcarrierIndices) const
    {
        return {3, MakeBudget(1.0), MakeTxVector(), 400, SU_STA_ID, 0, subcarrierIndices};
    }

    void DoRun() override
    {
        MimoChannelMatrix baselineChannel(2, 1, 2);
        baselineChannel.Set(0, 0, 0, {std::sqrt(10.0), 0.0});
        const auto baselineSnapshot = MakeSnapshot({{3, baselineChannel}});
        const DlMuMimoPrecodingMatrix baselinePrecoding(
            MimoPrecodingMatrix(
                2,
                2,
                1,
                {{1.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}}),
            {{3, 0}});

        auto abstraction = CreateObject<HeDlMuMimoLinkAbstraction>();
        abstraction->SetEffectiveSinrModel(SionnaMimoEffectiveSinrModel::EESM);
        abstraction->SetEesmBeta(0, 2.0);
        const auto selected = abstraction->Calculate(
            baselineSnapshot,
            baselinePrecoding,
            {MakeTransmission({0})});
        NS_TEST_ASSERT_MSG_EQ_TOL(selected.GetUserById(3).GetEffectiveSinrLinear(),
                                  10.0,
                                  1e-12,
                                  "One selected bin did not retain its SINR");

        const auto fullGrid = abstraction->Calculate(
            baselineSnapshot,
            baselinePrecoding,
            {MakeTransmission({})});
        const double expectedFullGrid =
            -2.0 * std::log((std::exp(-10.0 / 2.0) + 1.0) / 2.0);
        NS_TEST_ASSERT_MSG_EQ_TOL(fullGrid.GetUserById(3).GetEffectiveSinrLinear(),
                                  expectedFullGrid,
                                  1e-12,
                                  "An empty selection did not preserve full-grid EESM");

        MimoChannelMatrix perturbedChannel = baselineChannel;
        perturbedChannel.Set(1, 0, 0, {100.0, 0.0});
        perturbedChannel.Set(1, 0, 1, {200.0, 0.0});
        const auto perturbedSnapshot = MakeSnapshot({{3, perturbedChannel}});
        const DlMuMimoPrecodingMatrix perturbedPrecoding(
            MimoPrecodingMatrix(
                2,
                2,
                1,
                {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}}),
            {{3, 0}});
        const auto perturbed = abstraction->Calculate(
            perturbedSnapshot,
            perturbedPrecoding,
            {MakeTransmission({0})});
        NS_TEST_ASSERT_MSG_EQ_TOL(perturbed.GetUserById(3).GetEffectiveSinrLinear(),
                                  selected.GetUserById(3).GetEffectiveSinrLinear(),
                                  1e-12,
                                  "CFR/precoder changes outside the selected RU changed EESM");

        const auto expectRejected =
            [this, &abstraction, &baselineSnapshot, &baselinePrecoding](
                const std::vector<uint32_t>& indices,
                const std::string& reason) {
                bool rejected = false;
                try
                {
                    static_cast<void>(abstraction->Calculate(
                        baselineSnapshot,
                        baselinePrecoding,
                        {MakeTransmission(indices)}));
                }
                catch (const std::invalid_argument&)
                {
                    rejected = true;
                }
                NS_TEST_ASSERT_MSG_EQ(rejected, true, reason);
            };
        expectRejected({0, 0}, "Duplicate selected subcarrier indices were accepted");
        expectRejected({2}, "An out-of-range selected subcarrier index was accepted");
    }
};

class DlMuMimoRuTestSuite : public TestSuite
{
  public:
    DlMuMimoRuTestSuite()
        : TestSuite("dl-mu-mimo-ru", UNIT)
    {
        AddTestCase(new DlMuMimoRuPowerNormalizationTest, TestCase::QUICK);
        AddTestCase(new HeDlMuMimoSelectedToneTest, TestCase::QUICK);
    }
};

static DlMuMimoRuTestSuite g_dlMuMimoRuTestSuite;

} // namespace
