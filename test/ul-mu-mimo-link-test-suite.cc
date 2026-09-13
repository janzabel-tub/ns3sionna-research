/* SPDX-License-Identifier: GPL-2.0-only */

#include "ns3/he-phy.h"
#include "ns3/he-ul-mu-mimo-link-abstraction.h"
#include "ns3/mimo-channel-matrix.h"
#include "ns3/sionna-propagation-cache.h"
#include "ns3/su-mimo-link-model.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/test.h"
#include "ns3/ul-mu-mimo-link-model.h"
#include "ns3/wifi-link-budget.h"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ns3
{
namespace
{

MultiUserChannel
MakeUplinkUser(uint32_t userId, const MimoChannelMatrix& channel)
{
    std::vector<int> frequencies;
    for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
    {
        frequencies.push_back(static_cast<int>(sc));
    }
    // All UL users share the same AP (rxNodeId 100), while each has a
    // distinct transmitter.  This is the minimum valid UL snapshot.
    return {userId,
            userId,
            100,
            MultiUserChannelDirection::UPLINK,
            NanoSeconds(1234),
            {channel.GetNumRxAntennas(), 1, 0.5, 0.5, "dipole", "V"},
            std::move(frequencies),
            0.0,
            channel};
}

MultiUserChannelSnapshot
MakeUplinkSnapshot(const std::vector<std::pair<uint32_t, MimoChannelMatrix>>& users)
{
    MultiUserChannelSnapshot snapshot{NanoSeconds(1234),
                                      MultiUserChannelDirection::UPLINK,
                                      {}};
    for (const auto& user : users)
    {
        snapshot.users.push_back(MakeUplinkUser(user.first, user.second));
    }
    return snapshot;
}

MimoChannelMatrix
MakeChannel(uint32_t rx, std::initializer_list<std::complex<double>> coefficients)
{
    return MimoChannelMatrix(1, rx, 1,
                             std::vector<std::complex<double>>(coefficients));
}

std::vector<UlMuMimoStreamMapping>
Mappings(std::initializer_list<uint32_t> users)
{
    std::vector<UlMuMimoStreamMapping> result;
    for (const auto user : users)
    {
        result.push_back({user, 0});
    }
    return result;
}

std::vector<UlMuMimoUserSnr>
Snrs(std::initializer_list<std::pair<uint32_t, double>> users)
{
    std::vector<UlMuMimoUserSnr> result;
    for (const auto& user : users)
    {
        result.push_back({user.first, user.second});
    }
    return result;
}

WifiTxVector
MakeHeTxVector(WifiMode mode)
{
    return WifiTxVector(mode, 0, WIFI_PREAMBLE_HE_SU, 800, 1, 1, 0, 20, false);
}

WifiLinkBudgetResult
MakeReferenceSnrBudget(double referenceSnrLinear)
{
    constexpr double boltzmann = 1.3803e-23;
    const double txPowerDbm =
        WifiLinkBudget::LinearToDb(referenceSnrLinear * boltzmann) + 30.0;
    return WifiLinkBudget::Calculate(txPowerDbm, 0.0, 1.0, 1.0, 0.0);
}

class UlMuMimoAnalyticalTest : public TestCase
{
  public:
    UlMuMimoAnalyticalTest()
        : TestCase("UL MU ZF-MMSE analytical SINR matrix")
    {
    }

  private:
    void DoRun() override
    {
        // SU boundary: one UL stream is the existing SIMO MMSE/MRC result.
        const auto suChannel = MakeChannel(2, {{1.0, 0.0}, {2.0, -1.0}});
        const auto suSnapshot = MakeUplinkSnapshot({{7, suChannel}});
        const auto suReference = SuMimoLinkModel::Calculate(suChannel, 3.0);
        for (const auto detector : {UlMuMimoDetector::ZERO_FORCING,
                                    UlMuMimoDetector::LINEAR_MMSE})
        {
            const auto su = UlMuMimoLinkModel::Calculate(
                suSnapshot, Mappings({7}), Snrs({{7, 3.0}}), detector);
            NS_TEST_ASSERT_MSG_EQ_TOL(su.GetUserById(7).GetSinrLinear(0, 0),
                                      suReference.GetSinrLinear(0, 0),
                                      1e-10,
                                      "UL SU result differs from the SU boundary");
        }

        // Orthogonal users: both detectors see only their own noise.
        const auto e1 = MakeChannel(2, {{1.0, 0.0}, {0.0, 0.0}});
        const auto e2 = MakeChannel(2, {{0.0, 0.0}, {1.0, 0.0}});
        const auto orthogonal = MakeUplinkSnapshot({{1, e1}, {2, e2}});
        for (const auto detector : {UlMuMimoDetector::ZERO_FORCING,
                                     UlMuMimoDetector::LINEAR_MMSE})
        {
            const auto result = UlMuMimoLinkModel::Calculate(
                orthogonal, Mappings({1, 2}), Snrs({{1, 4.0}, {2, 4.0}}), detector);
            for (const auto user : {1u, 2u})
            {
                NS_TEST_ASSERT_MSG_EQ_TOL(result.GetUserById(user).GetSinrLinear(0, 0),
                                          4.0,
                                          1e-10,
                                          "Orthogonal-user SINR mismatch");
                NS_TEST_ASSERT_MSG_EQ_TOL(
                    result.GetUserById(user).GetInterUserInterferencePower(0, 0),
                    0.0,
                    1e-12,
                    "Orthogonal users interfere");
            }
        }

        // Rank loss: ZF must reject; MMSE remains finite (q/(1+2q)).
        const auto duplicate = MakeChannel(2, {{1.0, 0.0}, {0.0, 0.0}});
        const auto rankLoss = MakeUplinkSnapshot({{1, duplicate}, {2, duplicate}});
        bool rejected = false;
        try
        {
            UlMuMimoLinkModel::Calculate(rankLoss,
                                         Mappings({1, 2}),
                                         Snrs({{1, 1.0}, {2, 1.0}}),
                                         UlMuMimoDetector::ZERO_FORCING);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        NS_TEST_ASSERT_MSG_EQ(rejected, true, "Rank-deficient UL ZF was accepted");
        const auto mmse = UlMuMimoLinkModel::Calculate(rankLoss,
                                                       Mappings({1, 2}),
                                                       Snrs({{1, 1.0}, {2, 1.0}}),
                                                       UlMuMimoDetector::LINEAR_MMSE);
        for (const auto user : {1u, 2u})
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(mmse.GetUserById(user).GetSinrLinear(0, 0),
                                      0.5,
                                      1e-10,
                                      "Rank-loss MMSE SINR mismatch");
        }

        // Near-far and unequal powers, with |h1^H h2|=1/2.
        const auto near = MakeChannel(2, {{1.0, 0.0}, {0.0, 0.0}});
        const auto far = MakeChannel(2, {{0.5, 0.0}, {std::sqrt(3.0) / 2.0, 0.0}});
        const auto nearFar = MakeUplinkSnapshot({{1, near}, {2, far}});
        const auto zf = UlMuMimoLinkModel::Calculate(nearFar,
                                                     Mappings({1, 2}),
                                                     Snrs({{1, 100.0}, {2, 1.0}}),
                                                     UlMuMimoDetector::ZERO_FORCING);
        NS_TEST_ASSERT_MSG_EQ_TOL(zf.GetUserById(1).GetSinrLinear(0, 0),
                                  75.0,
                                  1e-10,
                                  "Near-far ZF SINR mismatch");
        NS_TEST_ASSERT_MSG_EQ_TOL(zf.GetUserById(2).GetSinrLinear(0, 0),
                                  0.75,
                                  1e-10,
                                  "Unequal-power ZF SINR mismatch");
        const auto mmseNearFar = UlMuMimoLinkModel::Calculate(
            nearFar, Mappings({1, 2}), Snrs({{1, 100.0}, {2, 1.0}}),
            UlMuMimoDetector::LINEAR_MMSE);
        NS_TEST_ASSERT_MSG_EQ_TOL(mmseNearFar.GetUserById(1).GetSinrLinear(0, 0),
                                  87.5,
                                  1e-10,
                                  "Near-far MMSE SINR mismatch");
        NS_TEST_ASSERT_MSG_EQ_TOL(mmseNearFar.GetUserById(2).GetSinrLinear(0, 0),
                                  0.752475247524752,
                                  1e-10,
                                  "Unequal-power MMSE SINR mismatch");

        // Invalid contracts are rejected before any result is produced.
        bool badMapping = false;
        try
        {
            UlMuMimoLinkModel::Calculate(orthogonal,
                                         Mappings({1}),
                                         Snrs({{1, 4.0}, {2, 4.0}}),
                                         UlMuMimoDetector::LINEAR_MMSE);
        }
        catch (const std::invalid_argument&)
        {
            badMapping = true;
        }
        NS_TEST_ASSERT_MSG_EQ(badMapping, true, "Incomplete UL mapping was accepted");

        // Per-user EESM/PER remains independent after joint detection.
        const auto mcs0Vector = MakeHeTxVector(HePhy::GetHeMcs0());
        const auto mcs3Vector = MakeHeTxVector(HePhy::GetHeMcs3());
        const auto budget1 = MakeReferenceSnrBudget(4.0);
        const auto budget2 = MakeReferenceSnrBudget(9.0);
        auto errorModel = CreateObject<TableBasedErrorRateModel>();
        auto he = CreateObject<HeUlMuMimoLinkAbstraction>();
        he->SetErrorRateModel(errorModel);
        he->SetEesmBeta(0, 2.0);
        he->SetEesmBeta(3, 3.0);
        const std::vector<HeUlMuMimoUserTransmission> transmissions{
            {2, budget2, mcs3Vector, 900},
            {1, budget1, mcs0Vector, 400}};
        const auto heResult =
            he->Calculate(orthogonal, UlMuMimoDetector::LINEAR_MMSE, transmissions);
        for (const auto& input : transmissions)
        {
            const auto& user = heResult.GetUserById(input.userId);
            const double expectedSinr = input.linkBudget.GetReferenceSnrLinear();
            NS_TEST_ASSERT_MSG_EQ_TOL(user.GetEffectiveSinrLinear(),
                                      expectedSinr,
                                      1e-12,
                                      "UL EESM mixed per-user SINRs");
            const double expectedSuccess = errorModel->GetChunkSuccessRate(
                input.txVector.GetMode(),
                input.txVector,
                expectedSinr,
                static_cast<uint64_t>(input.packetSizeBytes) * 8,
                2,
                WIFI_PPDU_FIELD_DATA,
                SU_STA_ID);
            NS_TEST_ASSERT_MSG_EQ_TOL(user.GetPacketSuccessRate(),
                                      expectedSuccess,
                                      1e-12,
                                      "UL PER differs from the direct per-user error-model call");
        }
    }
};

class UlMuMimoLinkTestSuite : public TestSuite
{
  public:
    UlMuMimoLinkTestSuite()
        : TestSuite("ul-mu-mimo", UNIT)
    {
        AddTestCase(new UlMuMimoAnalyticalTest, TestCase::QUICK);
    }
};

static UlMuMimoLinkTestSuite g_ulMuMimoLinkTestSuite;

} // namespace
} // namespace ns3
